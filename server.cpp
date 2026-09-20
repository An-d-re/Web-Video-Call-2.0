#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/base/Logging.h>
#include <muduo/base/LogFile.h>
#include <muduo/base/AsyncLogging.h>

#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <sys/stat.h>
#include <sstream>
#include <cstring>
#include <mutex>
#include <queue>
#include <algorithm>

#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>

#include <json/json.h>

using namespace muduo;
using namespace muduo::net;

const int PORT = 8888;

std::set<TcpConnectionPtr> g_ws_clients;
std::mutex g_clients_mutex;

// 房间管理：roomId -> 该房间内的连接列表（最多2人）
std::unordered_map<std::string, std::vector<TcpConnectionPtr>> g_rooms;
std::mutex g_rooms_mutex;

// 客户端挂断状态：记录已处理过 bye 的连接，防止重复处理
std::set<TcpConnectionPtr> g_bye_processed;
std::mutex g_bye_mutex;

// WebSocket 帧分片缓冲：连接 -> 已缓冲的消息片段
struct WsFrameState {
    std::string buffer;       // 已累积的 payload 数据
    uint8_t opcode = 0;       // 首帧的 opcode（0x1=text, 0x2=binary）
    bool fragmenting = false; // 是否正在接收分片
};
std::unordered_map<TcpConnectionPtr, WsFrameState> g_frame_buffers;
std::mutex g_frame_buffers_mutex;

// TCP 层面的不完整帧缓冲区（解决跨TCP分片的WebSocket帧问题）
std::unordered_map<TcpConnectionPtr, std::string> g_raw_buffer;
std::mutex g_raw_buffer_mutex;

// ICE 服务器配置（从 turn_config.json 加载）
Json::Value g_turn_config;

// 文件后缀 -> Content-Type
const std::unordered_map<std::string, std::string> CONTENT_TYPE = {
    {".html", "text/html; charset=utf-8"},
    {".js",  "application/javascript; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg","image/jpeg"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"}
};

// ========== Base64 编码 ==========
std::string base64Encode(const unsigned char* buffer, size_t length) {
    BIO *bio = BIO_new(BIO_s_mem());
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(b64, bio);
    BIO_write(b64, buffer, length);
    BIO_flush(b64);
    BUF_MEM *bufferPtr;
    BIO_get_mem_ptr(bio, &bufferPtr);
    BIO_set_close(bio, BIO_NOCLOSE);
    BIO_free_all(b64);
    return std::string(bufferPtr->data, bufferPtr->length);
}

// ========== WebSocket 握手密钥计算 ==========
std::string calcWsAcceptKey(const std::string& key) {
    const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = key + magic;
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)combined.c_str(), combined.size(), hash);
    return base64Encode(hash, SHA_DIGEST_LENGTH);
}

// ========== 文件操作 ==========
bool fileExists(const std::string& path) {
    struct stat buffer;
    return (stat(path.c_str(), &buffer) == 0);
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";
    std::ostringstream oss;
    oss << file.rdbuf();
    return oss.str();
}

std::string parseHttpPath(const std::string& req) {
    size_t path_start = req.find("GET ");
    if (path_start == std::string::npos) return "/";
    path_start += 4;
    size_t path_end = req.find(" HTTP/", path_start);
    if (path_end == std::string::npos) return "/";
    return req.substr(path_start, path_end - path_start);
}

// ========== 加载 TURN 配置文件 ==========
bool loadTurnConfig(const std::string& path) {
    std::string content = readFile(path);
    if (content.empty()) {
        LOG_ERROR << "无法读取 TURN 配置文件: " << path;
        return false;
    }
    Json::Reader reader;
    if (!reader.parse(content, g_turn_config)) {
        LOG_ERROR << "解析 TURN 配置文件失败";
        return false;
    }
    LOG_INFO << "已加载 TURN 配置，包含 " << g_turn_config["iceServers"].size() << " 个 ICE 服务器";
    return true;
}

// ========== WebSocket 帧解析（支持分片） ==========
struct ParsedFrame {
    std::string payload;
    uint8_t opcode = 0;
    bool fin = true;
    bool valid = false;
    size_t consumed = 0;  // 该帧消耗的原始字节数（含头部和mask）
};

ParsedFrame parseWsFrame(const std::string& msg) {
    ParsedFrame result;
    const uint8_t* data = (const uint8_t*)msg.data();
    int len = msg.size();
    if (len < 2) return result;

    uint8_t first_byte = data[0];
    result.fin = (first_byte & 0x80) != 0;
    result.opcode = first_byte & 0x0F;

    uint64_t payload_len = data[1] & 0x7F;
    bool masked = (data[1] & 0x80) != 0;
    int offset = 2;

    if (payload_len == 126) {
        if (len < 4) return result;
        payload_len = (data[2] << 8) | data[3];
        offset = 4;
    } else if (payload_len == 127) {
        if (len < 10) return result;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | data[2 + i];
        }
        offset = 10;
    }

    // 拒绝超大的 payload（防止 uint64_t → int 溢出和内存耗尽）
    const uint64_t MAX_PAYLOAD = 16 * 1024 * 1024;  // 16MB
    if (payload_len > MAX_PAYLOAD) {
        LOG_WARN << "WebSocket payload " << payload_len << " exceeds max " << MAX_PAYLOAD;
        return result;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (len < offset + 4) return result;
        memcpy(mask, data + offset, 4);
        offset += 4;
    }

    if (static_cast<uint64_t>(len) < static_cast<uint64_t>(offset) + payload_len) {
        LOG_WARN << "WebSocket 帧不完整: 需要 " << (offset + payload_len)
                 << " 字节, 实际 " << len;
        return result;
    }

    result.payload.reserve(static_cast<size_t>(payload_len));
    for (uint64_t i = 0; i < payload_len; ++i) {
        result.payload += data[offset + i] ^ mask[i % 4];
    }
    result.consumed = static_cast<size_t>(offset) + static_cast<size_t>(payload_len);
    result.valid = true;
    return result;
}

// ========== 处理 WebSocket 帧（含分片重组） ==========
// 返回完整的消息内容；如果还在等待更多分片，返回空字符串
// 对于控制帧（ping/pong/close），直接在函数内处理并返回空字符串
std::string handleWsFrame(const TcpConnectionPtr& conn, const std::string& rawData) {
    ParsedFrame frame = parseWsFrame(rawData);
    if (!frame.valid) return "";

    // 控制帧处理
    if (frame.opcode == 0x8) {
        // Close frame — 回复 close 并关闭
        LOG_INFO << "收到 close 帧，关闭连接: " << conn->peerAddress().toIpPort();
        std::string closeFrame;
        closeFrame += (char)0x88;  // FIN=1, opcode=8
        closeFrame += (char)0x00;  // no payload
        conn->send(closeFrame);
        conn->shutdown();
        return "";
    }

    if (frame.opcode == 0x9) {
        // Ping — 回复 pong（浏览器 WebSocket API 自动处理 ping，此路径为自定义客户端准备）
        std::string pong;
        pong += (char)0x8A;  // FIN=1, opcode=10 (pong)
        size_t plen = frame.payload.size();
        if (plen <= 125) {
            pong += (char)plen;
        } else {
            pong += (char)126;
            pong += (char)((plen >> 8) & 0xFF);
            pong += (char)(plen & 0xFF);
        }
        pong += frame.payload;
        conn->send(pong);
        return "";
    }

    if (frame.opcode == 0xA) {
        // Pong — 心跳响应，不需要上层处理
        return "";
    }

    // 数据帧处理（text=0x1, binary=0x2, continuation=0x0）
    if (frame.opcode == 0x0) {
        // 延续帧
        std::lock_guard<std::mutex> lock(g_frame_buffers_mutex);
        auto it = g_frame_buffers.find(conn);
        if (it == g_frame_buffers.end() || !it->second.fragmenting) {
            LOG_WARN << "收到意外的延续帧，丢弃";
            return "";
        }
        it->second.buffer += frame.payload;
        if (frame.fin) {
            // 最后一个分片 — 重组完成
            std::string complete = std::move(it->second.buffer);
            g_frame_buffers.erase(it);
            return complete;
        }
        return "";  // 还有更多分片
    }

    if (frame.opcode == 0x1 || frame.opcode == 0x2) {
        if (frame.fin) {
            // 未分片的普通消息
            return frame.payload;
        }
        // 分片消息的首帧 — 开始缓冲
        std::lock_guard<std::mutex> lock(g_frame_buffers_mutex);
        WsFrameState state;
        state.opcode = frame.opcode;
        state.buffer = frame.payload;
        state.fragmenting = true;
        g_frame_buffers[conn] = std::move(state);
        return "";  // 等待后续分片
    }

    LOG_WARN << "未知 opcode: " << (int)frame.opcode;
    return "";
}

std::string wrapWsFrame(const std::string& data) {
    std::string frame;
    frame += (char)0x81;          // FIN=1, opcode=text frame
    size_t len = data.size();

    if (len <= 125) {
        frame += (char)len;
    } else if (len <= 65535) {
        frame += (char)126;
        frame += (char)((len >> 8) & 0xFF);
        frame += (char)(len & 0xFF);
    } else {
        // 支持超大 payload（>65535 字节）
        frame += (char)127;
        for (int i = 7; i >= 0; --i) {
            frame += (char)((len >> (i * 8)) & 0xFF);
        }
    }

    frame += data;
    return frame;
}

// ========== 发送 ping 帧 ==========
void sendPing(const TcpConnectionPtr& conn) {
    // Muduo 的 send() 内部有状态检查，此处不必重复检查 connected()
    if (!conn) return;
    std::string ping;
    ping += (char)0x89;  // FIN=1, opcode=9 (ping)
    ping += (char)0x00;  // no payload
    conn->send(ping);
}

// ========== 辅助函数：从房间中移除连接 ==========
TcpConnectionPtr removeFromRoom(const TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(g_rooms_mutex);
    for (auto it = g_rooms.begin(); it != g_rooms.end(); ++it) {
        auto& clients = it->second;
        auto pos = std::find(clients.begin(), clients.end(), conn);
        if (pos != clients.end()) {
            clients.erase(pos);
            TcpConnectionPtr other = nullptr;
            if (!clients.empty()) {
                other = clients[0];
            }
            if (clients.empty()) {
                std::string deletedRoom = it->first;  // 先保存，erase后it失效
                g_rooms.erase(it);
                LOG_INFO << "房间 " << deletedRoom << " 已删除（双方都离开）";
            } else {
                LOG_INFO << "客户端离开房间 " << it->first
                         << "，房间仍有 " << clients.size() << " 人（等待新客户端）";
            }
            return other;
        }
    }
    return nullptr;
}

// ========== 连接回调 ==========
void onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        LOG_INFO << "新客户端连接：" << conn->peerAddress().toIpPort();
    } else {
        LOG_INFO << "客户端断开：" << conn->peerAddress().toIpPort();
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            g_ws_clients.erase(conn);
        }
        // 清理挂断状态记录
        {
            std::lock_guard<std::mutex> lock(g_bye_mutex);
            g_bye_processed.erase(conn);
        }
        // 清理帧分片缓冲
        {
            std::lock_guard<std::mutex> lock(g_frame_buffers_mutex);
            g_frame_buffers.erase(conn);
        }
        // 清理TCP层面不完整帧缓冲
        {
            std::lock_guard<std::mutex> lock(g_raw_buffer_mutex);
            g_raw_buffer.erase(conn);
        }
        // 从房间移除，并通知对方
        TcpConnectionPtr peer = removeFromRoom(conn);
        if (peer && peer->connected()) {
            bool peerHasBye = false;
            {
                std::lock_guard<std::mutex> lock(g_bye_mutex);
                peerHasBye = (g_bye_processed.find(peer) != g_bye_processed.end());
            }

            if (!peerHasBye) {
                Json::Value byeMsg;
                byeMsg["type"] = "bye";
                std::string msg = byeMsg.toStyledString();
                peer->send(wrapWsFrame(msg));
                LOG_INFO << "通知对方挂断";
            }
        }
    }
}

// ========== 消息回调 ==========
void onMessage(const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
    std::string msg = buf->retrieveAllAsString();

    // 1. WebSocket 握手
    if (msg.find("GET ") != std::string::npos &&
        msg.find("Upgrade: websocket") != std::string::npos) {
        size_t key_pos = msg.find("Sec-WebSocket-Key: ");
        std::string key;
        if (key_pos != std::string::npos) {
            key_pos += 19;
            size_t key_end = msg.find("\r\n", key_pos);
            if (key_end != std::string::npos) {
                key = msg.substr(key_pos, key_end - key_pos);
                size_t start = key.find_first_not_of(" \t");
                size_t end = key.find_last_not_of(" \t");
                if (start != std::string::npos) {
                    key = key.substr(start, end - start + 1);
                }
            }
        }
        if (!key.empty()) {
            std::string accept = calcWsAcceptKey(key);
            std::string resp =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
            conn->send(resp);
            {
                std::lock_guard<std::mutex> lock(g_clients_mutex);
                g_ws_clients.insert(conn);
                LOG_INFO << "WebSocket 握手成功！当前在线：" << g_ws_clients.size();
            }
        } else {
            conn->shutdown();
        }
        return;
    }

    // 2. 判断是否是 WebSocket 客户端
    bool is_ws_client = false;
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        is_ws_client = (g_ws_clients.find(conn) != g_ws_clients.end());
    }
    if (!is_ws_client) {
        // 普通 HTTP 请求
        std::string file_path = parseHttpPath(msg);
        if (file_path.find("..") != std::string::npos) {
            conn->send("HTTP/1.1 403 Forbidden\r\nContent-Length: 9\r\nConnection: close\r\n\r\nForbidden");
            conn->shutdown();
            return;
        }
        if (file_path == "/") file_path = "/index.html";
        file_path = "./static" + file_path;
        if (!fileExists(file_path)) {
            std::string content = "404 Not Found";
            std::string resp =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " + std::to_string(content.size()) + "\r\n"
                "Connection: close\r\n\r\n" + content;
            conn->send(resp);
            conn->shutdown();
            return;
        }
        std::string content_type = "text/plain; charset=utf-8";
        size_t dot_pos = file_path.find_last_of(".");
        if (dot_pos != std::string::npos) {
            std::string ext = file_path.substr(dot_pos);
            auto it = CONTENT_TYPE.find(ext);
            if (it != CONTENT_TYPE.end()) content_type = it->second;
        }
        std::string content = readFile(file_path);
        std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: " + content_type + "\r\n"
            "Content-Length: " + std::to_string(content.size()) + "\r\n"
            "Connection: close\r\n\r\n" + content;
        conn->send(resp);
        conn->shutdown();
        return;
    }

    // 3. 处理 WebSocket 消息（使用支持分片重组的处理器）
    // 先合并之前缓冲的不完整数据（TCP 分片场景）
    {
        std::lock_guard<std::mutex> lock(g_raw_buffer_mutex);
        auto it = g_raw_buffer.find(conn);
        if (it != g_raw_buffer.end()) {
            msg = std::move(it->second) + msg;
            g_raw_buffer.erase(it);
        }
    }

    // 循环解析所有完整帧（处理 TCP 合并帧的场景）
    while (!msg.empty()) {
        ParsedFrame frame = parseWsFrame(msg);
        if (!frame.valid) {
            // 不完整帧 → 缓冲起来等下次数据
            if (!msg.empty()) {
                std::lock_guard<std::mutex> lock(g_raw_buffer_mutex);
                g_raw_buffer[conn] = std::move(msg);
            }
            return;
        }

        std::string decoded = handleWsFrame(conn, msg.substr(0, frame.consumed));
        msg.erase(0, frame.consumed);

        if (decoded.empty()) continue;  // 控制帧或分片等待中

        LOG_INFO << "收到消息：" << decoded;

        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(decoded, root)) {
            LOG_ERROR << "JSON 解析失败";
            continue;
        }
        std::string type = root.get("type", "").asString();

        if (type == "join_room") {
            std::string roomId = root.get("roomId", "").asString();
            if (roomId.empty()) continue;
            std::lock_guard<std::mutex> lock(g_rooms_mutex);
            auto& clients = g_rooms[roomId];
            if (clients.size() >= 2) {
                Json::Value err;
                err["type"] = "error";
                err["message"] = "房间已满";
                conn->send(wrapWsFrame(err.toStyledString()));
                continue;
            }
            clients.push_back(conn);
            LOG_INFO << "客户端 " << conn->peerAddress().toIpPort()
                     << " 加入房间 " << roomId << "，当前人数 " << clients.size();
            if (clients.size() == 2) {
                TcpConnectionPtr initiator = clients[0];
                TcpConnectionPtr responder = clients[1];
                Json::Value startMsg;
                startMsg["type"] = "start_call";
                startMsg["initiator"] = true;
                if (!g_turn_config.isNull() && g_turn_config.isMember("iceServers")) {
                    startMsg["iceServers"] = g_turn_config["iceServers"];
                    LOG_INFO << "start_call 已注入 ICE 配置 ("
                             << g_turn_config["iceServers"].size() << " 个服务器)";
                } else {
                    LOG_WARN << "start_call 未注入 ICE 配置（g_turn_config 为空或无 iceServers）";
                }
                initiator->send(wrapWsFrame(startMsg.toStyledString()));
                startMsg["initiator"] = false;
                responder->send(wrapWsFrame(startMsg.toStyledString()));
                LOG_INFO << "房间 " << roomId << " 已满，开始通话";
            }
            continue;
        }

        if (type == "offer" || type == "answer" || type == "ice" || type == "video_mute") {
            std::lock_guard<std::mutex> lock(g_rooms_mutex);
            TcpConnectionPtr target = nullptr;
            for (auto& kv : g_rooms) {
                auto& clients = kv.second;
                if (std::find(clients.begin(), clients.end(), conn) != clients.end()) {
                    for (auto& c : clients) {
                        if (c != conn) {
                            target = c;
                            break;
                        }
                    }
                    break;
                }
            }
            if (target && target->connected()) {
                LOG_INFO << "转发 " << type << " 长度=" << decoded.size()
                        << " 内容预览: " << decoded.substr(0, std::min(decoded.size(), (size_t)100));
                std::string frameData = wrapWsFrame(decoded);
                if (frameData.empty()) {
                    LOG_ERROR << "wrapWsFrame 生成空帧，丢弃消息，原始数据长度=" << decoded.size();
                } else {
                    target->send(frameData);
                    LOG_INFO << "转发 " << type << " 成功，帧长度=" << frameData.size();
                }
            } else {
                LOG_WARN << "未找到配对客户端，丢弃消息";
            }
            continue;
        }

        if (type == "bye") {
            LOG_INFO << "客户端 " << conn->peerAddress().toIpPort() << " 请求挂断";
            {
                std::lock_guard<std::mutex> lock(g_bye_mutex);
                if (g_bye_processed.find(conn) != g_bye_processed.end()) {
                    LOG_WARN << "连接 " << conn->peerAddress().toIpPort()
                             << " 的 bye 已处理过，忽略重复请求";
                    continue;
                }
                g_bye_processed.insert(conn);
            }
            TcpConnectionPtr peer = removeFromRoom(conn);
            if (conn && conn->connected()) {
                Json::Value ackMsg;
                ackMsg["type"] = "bye_ack";
                conn->send(wrapWsFrame(ackMsg.toStyledString()));
                LOG_INFO << "发送 bye_ack 给 " << conn->peerAddress().toIpPort();
            }
            if (peer && peer->connected()) {
                bool peerHasBye = false;
                {
                    std::lock_guard<std::mutex> lock(g_bye_mutex);
                    peerHasBye = (g_bye_processed.find(peer) != g_bye_processed.end());
                }
                if (!peerHasBye) {
                    Json::Value byeMsg;
                    byeMsg["type"] = "bye";
                    peer->send(wrapWsFrame(byeMsg.toStyledString()));
                    LOG_INFO << "转发 bye 给对方 " << peer->peerAddress().toIpPort();
                } else {
                    LOG_INFO << "对方 " << peer->peerAddress().toIpPort()
                             << " 已处理过 bye，不再转发";
                }
            }
            continue;
        }
    }
}

// ========== 日志输出 ==========
AsyncLogging* g_asyncLog = nullptr;

void logOutput(const char* msg, int len) {
    // 同时输出到 stdout 和日志文件
    fwrite(msg, 1, len, stdout);
    fflush(stdout);
    if (g_asyncLog) {
        g_asyncLog->append(msg, len);
    }
}

// ========== 主函数 ==========
int main() {
    // 文件日志
    AsyncLogging asyncLog("webrtc_server", 100 * 1024 * 1024);  // 100MB 滚动
    asyncLog.start();
    g_asyncLog = &asyncLog;
    Logger::setOutput(logOutput);
    Logger::setLogLevel(Logger::INFO);

    // 加载 TURN 配置
    if (!loadTurnConfig("turn_config.json")) {
        LOG_WARN << "未找到 turn_config.json，将不注入 ICE 服务器配置";
        LOG_WARN << "前端将使用 index.html 中硬编码的 ICE 配置";
    }

    EventLoop loop;
    InetAddress addr(PORT);
    TcpServer server(&loop, addr, "VideoCallServer");
    server.setConnectionCallback(onConnection);
    server.setMessageCallback(onMessage);
    server.setThreadNum(4);
    server.start();

    // 定时心跳：每 30 秒向所有 WebSocket 客户端发送 ping
    // 先拷贝连接列表再释放锁，避免在持有锁时调用 conn->send() 导致竞态
    loop.runEvery(30.0, []() {
        std::vector<TcpConnectionPtr> clients;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            clients.assign(g_ws_clients.begin(), g_ws_clients.end());
        }
        for (auto& conn : clients) {
            if (conn) sendPing(conn);
        }
    });

    LOG_INFO << "=====================================";
    LOG_INFO << "Muduo 视频通话服务器已启动！";
    LOG_INFO << "监听端口：" << PORT;
    LOG_INFO << "静态文件目录：./static/";
    if (!g_turn_config.isNull()) {
        LOG_INFO << "ICE 配置：从 turn_config.json 动态下发";
    }
    LOG_INFO << "WebSocket 心跳：每 30 秒 ping/pong";
    LOG_INFO << "=====================================";

    loop.loop();
    return 0;
}
