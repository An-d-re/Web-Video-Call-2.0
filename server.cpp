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
#include <chrono>
#include <cstdlib>
#include <ctime>

#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>

#include <json/json.h>

#include "jwt_verify.h"

using namespace muduo;
using namespace muduo::net;

const int PORT = 8888;

// ========== 鉴权配置（来自环境变量）==========
// JWT_SECRET 必须与 auth 服务一致，长度 ≥32 字节；AUTH_REQUIRED=false 可临时退回匿名模式。
std::string g_jwt_secret;
bool g_auth_required = true;
const int64_t kAuthTimeoutMs = 5000;  // 建连后必须在此时间内发出 auth

std::set<TcpConnectionPtr> g_ws_clients;
std::mutex g_clients_mutex;

// ========== 连接鉴权状态 ==========
struct ConnAuth {
    bool authenticated = false;
    int64_t userId = 0;
    std::string username;
    std::string displayName;
    int64_t deadlineMs = 0;  // 未认证时的截止时刻（0 = 不设限）
};

// 加锁顺序约定：允许 g_rooms_mutex -> g_auth_mutex，
// 禁止反向（持有 g_auth_mutex 时不得再去取 g_rooms_mutex），否则可能死锁。
std::unordered_map<TcpConnectionPtr, ConnAuth> g_auth;
std::mutex g_auth_mutex;

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 读取连接的身份；未认证返回 false。
bool GetAuth(const TcpConnectionPtr& conn, ConnAuth* out) {
    std::lock_guard<std::mutex> lock(g_auth_mutex);
    auto it = g_auth.find(conn);
    if (it == g_auth.end() || !it->second.authenticated) return false;
    if (out) *out = it->second;
    return true;
}

// 用户身份 -> JSON（用于 joined / start_call）
Json::Value AuthToJson(const ConnAuth& a) {
    Json::Value v;
    v["userId"] = static_cast<Json::Int64>(a.userId);
    v["username"] = a.username;
    v["displayName"] = a.displayName;
    return v;
}

// 匿名模式下也要有可显示的名字
Json::Value GuestJson() {
    Json::Value v;
    v["userId"] = static_cast<Json::Int64>(0);
    v["username"] = "guest";
    v["displayName"] = "游客";
    return v;
}

// 房间管理：roomId -> 该房间内的连接列表（最多2人）
// 注意：这里只有"运行时状态"（谁现在连着）。房间是否存在、标题、密码
// 全部在 PostgreSQL 里，由 auth 服务负责；信令服务只认 roomToken。
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

// 心跳存活：连接 -> 已发出 ping 但尚未收到 pong。
// 条目存在 = 该连接欠我们一个 pong；连续两轮都没有就判定为半开连接并回收。
// 没有这个机制时，客户端断网但 TCP 未正常关闭，连接会永远留在房间里，
// 表现为"对方还能看到我在房间里，我重连却提示房间已满"。
std::unordered_map<TcpConnectionPtr, bool> g_pong_pending;
std::mutex g_heartbeat_mutex;

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
        // Pong — 心跳响应：销掉该连接欠下的 pong
        {
            std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
            g_pong_pending.erase(conn);
        }
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
                // 房间"是否存在"由数据库负责，内存里这个空壳可以立刻回收
                std::string deletedRoom = it->first;  // 先保存，erase后it失效
                g_rooms.erase(it);
                LOG_INFO << "房间 " << deletedRoom << " 已空，回收运行时状态";
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
        // 清理心跳状态
        {
            std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
            g_pong_pending.erase(conn);
        }
        // 清理鉴权状态
        {
            std::lock_guard<std::mutex> lock(g_auth_mutex);
            g_auth.erase(conn);
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

// ========== 辅助函数：把连接从"除 keepRoomId 之外"的房间里摘掉 ==========
// 一条连接同一时刻只能属于一个房间。keepRoomId 必须排除在外，
// 否则同房间的重复 join_room 会把正在进行的通话打断。
// 返回被离开房间里的另一位对等端（若有），调用方负责通知它。
TcpConnectionPtr removeFromOtherRooms(const TcpConnectionPtr& conn,
                                      const std::string& keepRoomId) {
    std::lock_guard<std::mutex> lock(g_rooms_mutex);
    for (auto it = g_rooms.begin(); it != g_rooms.end();) {
        if (it->first == keepRoomId) {
            ++it;
            continue;
        }
        auto& clients = it->second;
        auto pos = std::find(clients.begin(), clients.end(), conn);
        if (pos == clients.end()) {
            ++it;
            continue;
        }
        std::string leftRoom = it->first;
        clients.erase(pos);
        TcpConnectionPtr other = clients.empty() ? nullptr : clients[0];
        LOG_INFO << "连接离开房间 " << leftRoom << "（切换到房间 " << keepRoomId << "）";
        if (clients.empty()) {
            g_rooms.erase(it);
        }
        return other;
    }
    return nullptr;
}

// ========== 下行消息辅助 ==========
void sendJson(const TcpConnectionPtr& conn, const Json::Value& msg) {
    if (conn && conn->connected()) {
        conn->send(wrapWsFrame(msg.toStyledString()));
    }
}

// 入房应答。客户端只有收到 joined 才会把房间标记为"已确认"，
// 避免乐观更新导致的"界面上已进房、服务端其实没进"。
// inCall=true 表示房间里已经凑满 2 人（通话正在进行），
// 客户端据此保持"通话中"，而不是把状态打回"等待对方"。
// 注意：调用点在 g_rooms_mutex 之下，这里会再取 g_auth_mutex（符合约定的加锁顺序）。
void sendJoined(const TcpConnectionPtr& conn, const std::string& roomId,
                size_t peerCount, bool inCall) {
    Json::Value ack;
    ack["type"] = "joined";
    ack["roomId"] = roomId;
    ack["peers"] = static_cast<Json::UInt>(peerCount);
    ack["inCall"] = inCall;

    ConnAuth me;
    ack["self"] = GetAuth(conn, &me) ? AuthToJson(me) : GuestJson();

    sendJson(conn, ack);
}

void sendError(const TcpConnectionPtr& conn, const std::string& code,
               const std::string& message) {
    Json::Value err;
    err["type"] = "error";
    err["code"] = code;
    err["message"] = message;
    sendJson(conn, err);
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
            // 开启鉴权倒计时：5 秒内必须发出 auth，否则被定时器回收
            if (g_auth_required) {
                std::lock_guard<std::mutex> lock(g_auth_mutex);
                ConnAuth& st = g_auth[conn];
                st.deadlineMs = NowMs() + kAuthTimeoutMs;
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

        // ---------- 鉴权 ----------
        if (type == "auth") {
            if (g_jwt_secret.empty()) {
                LOG_ERROR << "收到 auth 但服务端未配置 JWT_SECRET";
                sendError(conn, "auth_unavailable", "服务端未启用鉴权");
                continue;
            }

            const std::string token = root.get("token", "").asString();
            jwt::Claims claims;
            const std::string reason = jwt::Verify(
                token, g_jwt_secret, jwt::kTypAccess,
                static_cast<int64_t>(::time(nullptr)), &claims);

            if (reason != jwt::kOk) {
                LOG_WARN << "鉴权失败 [" << reason << "] 来源 "
                         << conn->peerAddress().toIpPort();
                sendError(conn, "invalid_token", "凭证无效或已过期");
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(g_auth_mutex);
                ConnAuth& st = g_auth[conn];
                st.authenticated = true;
                st.userId = claims.sub;
                st.username = claims.username;
                st.displayName = claims.displayName;
                st.deadlineMs = 0;  // 已认证，取消倒计时
            }

            Json::Value ok;
            ok["type"] = "auth_ok";
            ok["userId"] = static_cast<Json::Int64>(claims.sub);
            ok["username"] = claims.username;
            ok["displayName"] = claims.displayName;
            sendJson(conn, ok);

            LOG_INFO << "鉴权成功: " << claims.username
                     << " (id=" << claims.sub << ") 来源 "
                     << conn->peerAddress().toIpPort();
            continue;
        }

        // ---------- 实时房间列表 ----------
        // 只回答"现在哪个房间有几个人"。房间标题、是否加密、历史人数在 auth 服务，
        // 客户端按 roomId 合并两份数据即可。
        if (type == "list_rooms") {
            if (g_auth_required && !GetAuth(conn, nullptr)) {
                sendError(conn, "unauthorized", "请先登录");
                continue;
            }
            Json::Value arr(Json::arrayValue);
            {
                std::lock_guard<std::mutex> lock(g_rooms_mutex);
                for (auto& kv : g_rooms) {
                    if (kv.second.empty()) continue;
                    Json::Value r;
                    r["roomId"] = kv.first;
                    r["peers"] = static_cast<Json::UInt>(kv.second.size());
                    arr.append(r);
                }
            }
            Json::Value resp;
            resp["type"] = "live_rooms";
            resp["rooms"] = arr;
            sendJson(conn, resp);
            continue;
        }

        if (type == "join_room") {
            if (g_auth_required && !GetAuth(conn, nullptr)) {
                sendError(conn, "unauthorized", "请先登录");
                continue;
            }

            // 房间的权威数据在 PostgreSQL：房间是否存在、密码对不对，
            // 都已由 auth 服务在签发 roomToken 之前校验完了。
            // 信令服务这里只做一件事：验签，取出 roomId。
            // 这样 C++ 侧不需要连数据库，也不需要知道密码。
            const std::string roomToken = root.get("roomToken", "").asString();
            jwt::Claims roomClaims;
            const std::string roomReason = jwt::Verify(
                roomToken, g_jwt_secret, jwt::kTypRoom,
                static_cast<int64_t>(::time(nullptr)), &roomClaims);
            if (roomReason != jwt::kOk) {
                LOG_WARN << "进房凭证校验失败 [" << roomReason << "] 来源 "
                         << conn->peerAddress().toIpPort();
                sendError(conn, "invalid_room_token", "进房凭证无效或已过期，请重新加入房间");
                continue;
            }
            const std::string roomId = roomClaims.room;

            // 1) 重新加入房间 = 新一次通话的开始，必须清除这条连接的历史挂断标记。
            // 否则连接复用（挂断后再次加入同一房间）时，第二次 bye 会被当成
            // "重复请求"丢弃，连接永久留在房间里，之后该房间一直报"房间已满"。
            // 刻意放在取 g_rooms_mutex 之前，避免引入 g_rooms -> g_bye 的嵌套加锁。
            {
                std::lock_guard<std::mutex> lock(g_bye_mutex);
                g_bye_processed.erase(conn);
            }

            // 2) 一条连接只能属于一个房间：先从其它房间摘掉，并通知旧对端
            TcpConnectionPtr oldPeer = removeFromOtherRooms(conn, roomId);
            if (oldPeer && oldPeer->connected()) {
                bool peerHasBye = false;
                {
                    std::lock_guard<std::mutex> lock(g_bye_mutex);
                    peerHasBye = (g_bye_processed.find(oldPeer) != g_bye_processed.end());
                }
                if (!peerHasBye) {
                    Json::Value byeMsg;
                    byeMsg["type"] = "bye";
                    LOG_INFO << "通知旧房间的对端：对方已转到其它房间";
                    sendJson(oldPeer, byeMsg);
                }
            }

            std::lock_guard<std::mutex> lock(g_rooms_mutex);

            // 运行时房间：auth 服务已经授权过，第一次有人进来时创建即可。
            // 空房间在最后一个人离开时立即回收——房间"是否存在"由数据库负责，
            // 内存里不需要保留空壳（也就不再需要宽限期）。
            auto& clients = g_rooms[roomId];

            // 同一连接重复 join 同一房间时保持幂等：否则 clients 里会出现两个相同连接，
            // size()==2 会让这条连接在"只有自己一人"的情况下收到发给自己的一对 start_call。
            // 幂等路径同样要回 ack，而且必须如实告诉客户端"这里已经在通话中"。
            if (std::find(clients.begin(), clients.end(), conn) != clients.end()) {
                LOG_INFO << "连接已在房间 " << roomId << " 中，忽略重复 join_room";
                sendJoined(conn, roomId, clients.size(), clients.size() >= 2);
                continue;
            }

            if (clients.size() >= 2) {
                LOG_INFO << "房间 " << roomId << " 已满，拒绝加入";
                sendError(conn, "room_full", "房间已满");
                continue;
            }
            clients.push_back(conn);
            LOG_INFO << "客户端 " << conn->peerAddress().toIpPort()
                     << " 加入房间 " << roomId << "，当前人数 " << clients.size();
            sendJoined(conn, roomId, clients.size(), clients.size() >= 2);
            if (clients.size() == 2) {
                TcpConnectionPtr initiator = clients[0];
                TcpConnectionPtr responder = clients[1];

                // 双方各自的身份，用来给对方显示真实姓名
                ConnAuth aAuth, rAuth;
                const bool aOk = GetAuth(initiator, &aAuth);
                const bool rOk = GetAuth(responder, &rAuth);
                const Json::Value aJson = aOk ? AuthToJson(aAuth) : GuestJson();
                const Json::Value rJson = rOk ? AuthToJson(rAuth) : GuestJson();

                Json::Value startMsg;
                startMsg["type"] = "start_call";
                startMsg["initiator"] = true;
                startMsg["peer"] = rJson;  // 发给 initiator 的，是对端(responder)的身份
                if (!g_turn_config.isNull() && g_turn_config.isMember("iceServers")) {
                    startMsg["iceServers"] = g_turn_config["iceServers"];
                    LOG_INFO << "start_call 已注入 ICE 配置 ("
                             << g_turn_config["iceServers"].size() << " 个服务器)";
                } else {
                    LOG_WARN << "start_call 未注入 ICE 配置（g_turn_config 为空或无 iceServers）";
                }
                initiator->send(wrapWsFrame(startMsg.toStyledString()));

                startMsg["initiator"] = false;
                startMsg["peer"] = aJson;  // 发给 responder 的，是对端(initiator)的身份
                responder->send(wrapWsFrame(startMsg.toStyledString()));

                LOG_INFO << "房间 " << roomId << " 已满，开始通话："
                         << aJson["displayName"].asString() << " <-> "
                         << rJson["displayName"].asString();
            }
            continue;
        }

        if (type == "offer" || type == "answer" || type == "ice" ||
            type == "video_mute" || type == "mic_mute") {
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
        LOG_WARN << "未找到 turn_config.json（请从 turn_config.example.json 复制）";
        LOG_WARN << "将不下发 ICE 配置，前端会退回内置的 STUN，无 TURN 中继";
    }

    // 加载鉴权配置（环境变量）
    //   JWT_SECRET     与 auth 服务共享的 HS256 密钥，长度 ≥32 字节
    //   AUTH_REQUIRED  默认 true；设为 false 可临时退回"匿名入房"（回滚阀门）
    if (const char* secret = ::getenv("JWT_SECRET")) {
        g_jwt_secret = secret;
    }
    if (const char* v = ::getenv("AUTH_REQUIRED")) {
        const std::string flag(v);
        g_auth_required = !(flag == "false" || flag == "0" || flag == "no");
    }
    if (g_auth_required) {
        if (g_jwt_secret.size() < 32) {
            LOG_ERROR << "JWT_SECRET 未设置或过短（需 ≥32 字节），且 AUTH_REQUIRED 未关闭";
            LOG_ERROR << "请设置环境变量后重启；调试时可临时设 AUTH_REQUIRED=false";
            return 1;
        }
        LOG_INFO << "鉴权已开启：WebSocket 建连后 5 秒内必须发送 auth";
    } else {
        LOG_WARN << "鉴权已关闭（AUTH_REQUIRED=false），任何人可直接 join_room，仅用于调试";
    }

    EventLoop loop;
    InetAddress addr(PORT);
    TcpServer server(&loop, addr, "VideoCallServer");
    server.setConnectionCallback(onConnection);
    server.setMessageCallback(onMessage);
    server.setThreadNum(4);
    server.start();

    // 鉴权超时回收：每秒扫一次，把"建连后 5 秒内没发 auth"的连接关掉。
    // 目的是防止有人只挂连接不认证、白占服务端资源。
    loop.runEvery(1.0, []() {
        std::vector<TcpConnectionPtr> expired;
        const int64_t now = NowMs();
        {
            std::lock_guard<std::mutex> lock(g_auth_mutex);
            for (auto& kv : g_auth) {
                if (!kv.second.authenticated && kv.second.deadlineMs > 0 &&
                    now > kv.second.deadlineMs) {
                    expired.push_back(kv.first);
                }
            }
        }
        // 注意：必须在释放 g_auth_mutex 之后再关闭连接，
        // 否则 onConnection 里再去取 g_auth_mutex 会自死锁。
        for (auto& conn : expired) {
            if (!conn || !conn->connected()) continue;
            LOG_INFO << "鉴权超时，关闭连接: " << conn->peerAddress().toIpPort();
            sendError(conn, "auth_timeout", "建连后 5 秒内未发送 auth");
            conn->shutdown();
        }
    });

    // 定时心跳：每 30 秒向所有 WebSocket 客户端发送 ping，并回收半开连接。
    // 策略（两轮判定，最坏 60 秒回收）：
    //   第 N 轮  ping，并记账"欠一个 pong"
    //   第 N+1 轮  若仍欠着 pong => 客户端已失联，forceClose 回收
    //   收到 pong 则销账
    // 先拷贝连接列表再释放锁，避免在持有锁时调用 conn->send() 导致竞态。
    loop.runEvery(30.0, []() {
        std::vector<TcpConnectionPtr> clients;
        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            clients.assign(g_ws_clients.begin(), g_ws_clients.end());
        }

        std::vector<TcpConnectionPtr> alive;
        std::vector<TcpConnectionPtr> timeout;
        {
            std::lock_guard<std::mutex> lock(g_heartbeat_mutex);
            for (auto& conn : clients) {
                if (!conn) continue;
                if (g_pong_pending.count(conn) > 0) {
                    timeout.push_back(conn);       // 上一轮的 ping 没等到 pong
                } else {
                    g_pong_pending[conn] = true;
                    alive.push_back(conn);
                }
            }
        }

        for (auto& conn : alive) {
            sendPing(conn);
        }
        for (auto& conn : timeout) {
            LOG_WARN << "心跳超时（30 秒未回 pong），回收半开连接："
                     << conn->peerAddress().toIpPort();
            conn->forceClose();
        }
    });

    LOG_INFO << "=====================================";
    LOG_INFO << "Muduo 视频通话服务器已启动！";
    LOG_INFO << "监听端口：" << PORT;
    LOG_INFO << "静态文件目录：./static/";
    if (!g_turn_config.isNull()) {
        LOG_INFO << "ICE 配置：从 turn_config.json 动态下发";
    }
    LOG_INFO << "WebSocket 心跳：每 30 秒 ping，连续两次无 pong 回收连接";
    LOG_INFO << "鉴权：" << (g_auth_required ? "开启（需在 5 秒内 auth）" : "关闭（匿名模式）");
    LOG_INFO << "=====================================";

    loop.loop();
    return 0;
}
