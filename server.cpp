#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/net/Channel.h>
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
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>

#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>

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

    // 当前所在房间的缓存索引。g_rooms 才是唯一真相，这里只用来把
    // "遍历所有房间找这条连接" 从 O(房间数) 降到 O(1)。
    // 查表后仍会校验成员是否真的在房间里，对不上就退化为遍历并打 WARN，
    // 所以这里写漏了只会变慢，不会出现"人在房间里却找不到"。
    std::string roomId;

    // 每连接令牌桶状态（限流用，见 AllowMessage）
    double rateTokens = 0;
    int64_t rateRefillMs = 0;
};

// 身份表按连接地址分片：每条消息都要读一次身份（GetAuth）和一次令牌桶
// （AllowMessage），单表单锁在 setThreadNum(4) 下会退化成全局串行点——压测里
// 表现为"转发延迟降了但 CPU 没降"。分片后 16 条连接才共用一把锁。
const int kAuthShards = 16;
struct AuthShard {
    std::mutex mutex;
    std::unordered_map<TcpConnectionPtr, ConnAuth> map;
};
AuthShard g_auth_shards[kAuthShards];

// 加锁顺序约定：允许 g_rooms_mutex -> 某个分片锁，
// 禁止反向（持有分片锁时不得再去取 g_rooms_mutex），否则可能死锁。
AuthShard& ShardOf(const TcpConnectionPtr& conn) {
    // 右移 4 位：malloc 返回的地址低位固定，直接取模会严重倾斜
    const size_t h = std::hash<const void*>()(conn.get()) >> 4;
    return g_auth_shards[h % kAuthShards];
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 读取连接的身份；未认证返回 false。
bool GetAuth(const TcpConnectionPtr& conn, ConnAuth* out) {
    AuthShard& shard = ShardOf(conn);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.map.find(conn);
    if (it == shard.map.end() || !it->second.authenticated) return false;
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

// ========== 每连接限流（令牌桶）==========
// 目的：一条连接狂刷消息既能把服务端 CPU 打满，也能靠高频转发占满对端带宽。
// 阈值依据：正常通话的峰值约 10 条/秒（ICE 4/s + 静音状态 0.4/s），
// 这里给 60 条/秒、突发 120，留了 6 倍余量，不会误伤正常客户端。
const double kRatePerSec = 60.0;
const double kRateBurst = 120.0;

// 返回 false 表示超限。令牌桶状态挂在 ConnAuth 上，跟身份表共用同一个分片锁，
// 所以不新增锁——这也是把身份表分片的原因之一。
bool AllowMessage(const TcpConnectionPtr& conn) {
    const int64_t now = NowMs();
    AuthShard& shard = ShardOf(conn);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.map.find(conn);
    if (it == shard.map.end()) return true;  // 握手刚完成、还没登记，放行
    ConnAuth& a = it->second;
    if (a.rateRefillMs == 0) {  // 第一次：装满令牌
        a.rateRefillMs = now;
        a.rateTokens = kRateBurst;
    }
    const double elapsedSec = static_cast<double>(now - a.rateRefillMs) / 1000.0;
    if (elapsedSec > 0) {
        a.rateTokens += elapsedSec * kRatePerSec;
        if (a.rateTokens > kRateBurst) a.rateTokens = kRateBurst;
        a.rateRefillMs = now;
    }
    if (a.rateTokens < 1.0) return false;
    a.rateTokens -= 1.0;
    return true;
}

// 房间管理：roomId -> 该房间内的连接列表（最多2人）
// 注意：这里只有"运行时状态"（谁现在连着）。房间是否存在、标题、密码
// 全部在 PostgreSQL 里，由 auth 服务负责；信令服务只认 roomToken。
std::unordered_map<std::string, std::vector<TcpConnectionPtr>> g_rooms;
std::mutex g_rooms_mutex;

// 单条连接最多允许积压多少"还没发出去"的字节。超过就判定为读得太慢的客户端，
// 直接断开——否则对方不读、我们还在往里塞，内存会随慢客户端无限增长。
const size_t kSlowClientBytes = 256 * 1024;

// 在持 g_rooms_mutex 的前提下，找到 conn 所在房间的成员列表。
// 走 ConnAuth.roomId 缓存（O(1)）；缓存缺失或与实际不一致时退化为遍历并告警。
// 索引只是加速手段，g_rooms 始终是唯一真相。
std::vector<TcpConnectionPtr>* RoomMembersLocked(const TcpConnectionPtr& conn,
                                                 std::string* outRoomId) {
    std::string cached;
    {
        AuthShard& shard = ShardOf(conn);
        std::lock_guard<std::mutex> lock(shard.mutex);  // 锁序：rooms -> 分片，允许
        auto it = shard.map.find(conn);
        if (it != shard.map.end()) cached = it->second.roomId;
    }
    if (!cached.empty()) {
        auto it = g_rooms.find(cached);
        if (it != g_rooms.end() &&
            std::find(it->second.begin(), it->second.end(), conn) != it->second.end()) {
            if (outRoomId) *outRoomId = cached;
            return &it->second;
        }
    }
    for (auto& kv : g_rooms) {
        if (std::find(kv.second.begin(), kv.second.end(), conn) != kv.second.end()) {
            if (!cached.empty()) {
                LOG_WARN << "房间索引与实际不一致（缓存=" << cached << "，实际=" << kv.first
                         << "），本次退化为遍历";
            }
            if (outRoomId) *outRoomId = kv.first;
            return &kv.second;
        }
    }
    return nullptr;
}

// 更新"连接当前在哪个房间"的缓存。调用方必须已持 g_rooms_mutex（锁序 rooms -> 分片）。
// 只更新已存在的条目，不新建：新建会让已断开的连接在身份表里复活。
void SetConnRoomLocked(const TcpConnectionPtr& conn, const std::string& roomId) {
    AuthShard& shard = ShardOf(conn);
    std::lock_guard<std::mutex> lock(shard.mutex);
    auto it = shard.map.find(conn);
    if (it != shard.map.end()) it->second.roomId = roomId;
}

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

// ========== HMAC-SHA1 + Base64（coturn 的 TURN REST API 凭据格式）==========
// coturn 配 use-auth-secret + static-auth-secret 后，校验规则是：
//   username   = "<过期时间戳(秒)>:<任意标识>"
//   credential = base64( HMAC-SHA1( static-auth-secret, username ) )
// 所以这里只需要共享密钥，不用把长期账号发给浏览器。
std::string hmacSha1Base64(const std::string& key, const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;
    HMAC(EVP_sha1(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &digestLen);
    return base64Encode(digest, digestLen);
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

// 文字聊天一条消息的最大字符数（不是字节）。前端输入框也是 500。
// 服务端必须自己再截一次：前端限制可以被绕过，一条几十 MB 的 chat 会把对端页面卡死。
const size_t kMaxChatChars = 500;

// 按 UTF-8 码点截断。不能按字节切——从半个汉字/emoji 中间切开，对端会拿到乱码。
std::string TruncateUtf8(const std::string& s, size_t maxChars) {
    size_t i = 0;
    size_t chars = 0;
    while (i < s.size() && chars < maxChars) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (i + len > s.size()) break;   // 末尾是残缺的多字节序列，直接丢掉
        i += len;
        ++chars;
    }
    return s.substr(0, i);
}

// ========== 运行指标（/stats）==========
// 设计原则：热路径只做原子自增 + 一次取时间戳，不加锁、不拼字符串。
// 采样接口（GET /stats）才去读 map 的大小、读 /proc，这些都不在消息路径上。
// STATS_TOKEN 未配置时只允许 loopback 访问；配置后必须带 ?token= 才返回数据。
std::string g_stats_token;
// 是否把消息原文（SDP / ICE 候选 / 状态）打进日志。
// 默认关：一是热路径上每条消息都拼一次 payload 日志很贵，二是 SDP 里带着
// 内网 IP 和候选地址，没必要长期留在磁盘上。排查时用 LOG_PAYLOAD=1 重启打开。
bool g_log_payload = false;
const int64_t g_startedAtSec = static_cast<int64_t>(::time(nullptr));

enum MsgKind {
    kMsgAuth = 0, kMsgJoinRoom, kMsgOffer, kMsgAnswer, kMsgIce,
    kMsgBye, kMsgVideoMute, kMsgMicMute, kMsgListRooms, kMsgChat, kMsgOther,
    kMsgKindCount
};

// 静态存储期对象在 main() 之前被零初始化，因此不需要显式构造。
std::atomic<uint64_t> g_msg_count[kMsgKindCount];

enum ErrKind {
    kErrAuthFail = 0, kErrAuthTimeout, kErrRoomFull, kErrInvalidRoomToken,
    kErrBadJson, kErrRateLimited, kErrSlowClient, kErrKindCount
};
std::atomic<uint64_t> g_err_count[kErrKindCount];

// 延迟直方图：固定分桶，分位数按桶上界估算（误差不超过桶宽）。
// 为什么不用记录所有样本：压测时每秒上万条消息，存样本会把内存吃光。
const uint64_t kLatBucketUs[] = {100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000};
const int kLatBucketCount = 10;  // 前 9 个是上界，最后一个是 "> 50ms"

struct LatencyHist {
    std::atomic<uint64_t> buckets[kLatBucketCount];
    std::atomic<uint64_t> count;
    std::atomic<uint64_t> maxUs;

    LatencyHist() : count(0), maxUs(0) {
        for (int i = 0; i < kLatBucketCount; ++i) buckets[i].store(0);
    }

    void add(uint64_t us) {
        int i = 0;
        while (i < kLatBucketCount - 1 && us > kLatBucketUs[i]) ++i;
        buckets[i].fetch_add(1, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        uint64_t cur = maxUs.load(std::memory_order_relaxed);
        while (us > cur && !maxUs.compare_exchange_weak(cur, us, std::memory_order_relaxed)) {
        }
    }

    // p 为 0~100 的百分位；没有样本时返回 0。
    uint64_t percentile(double p) const {
        const uint64_t total = count.load(std::memory_order_relaxed);
        if (total == 0) return 0;
        uint64_t target = static_cast<uint64_t>(total * p / 100.0);
        if (target == 0) target = 1;
        uint64_t cum = 0;
        for (int i = 0; i < kLatBucketCount; ++i) {
            cum += buckets[i].load(std::memory_order_relaxed);
            if (cum >= target) {
                // 最后一桶没有上界，用已观测到的最大值代替
                if (i == kLatBucketCount - 1) return maxUs.load(std::memory_order_relaxed);
                return kLatBucketUs[i];
            }
        }
        return maxUs.load(std::memory_order_relaxed);
    }
};

// 只测两条关键路径：入房处理、消息转发。给每条消息都取时间戳的话，
// 测出来的就是测量本身的开销了。
LatencyHist g_lat_join_room;
LatencyHist g_lat_forward;

uint64_t NowUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// RAII：离开作用域（包括 continue / return）时自动记一条耗时，
// 这样入房分支里多个提前 return 的地方不用逐个补。
class LatencyTimer {
public:
    explicit LatencyTimer(LatencyHist& h) : hist_(h), startUs_(NowUs()) {}
    ~LatencyTimer() { hist_.add(NowUs() - startUs_); }
    LatencyTimer(const LatencyTimer&) = delete;
    LatencyTimer& operator=(const LatencyTimer&) = delete;

private:
    LatencyHist& hist_;
    uint64_t startUs_;
};

MsgKind MsgKindOf(const std::string& type) {
    if (type == "auth") return kMsgAuth;
    if (type == "join_room") return kMsgJoinRoom;
    if (type == "offer") return kMsgOffer;
    if (type == "answer") return kMsgAnswer;
    if (type == "ice") return kMsgIce;
    if (type == "bye") return kMsgBye;
    if (type == "video_mute") return kMsgVideoMute;
    if (type == "mic_mute") return kMsgMicMute;
    if (type == "list_rooms") return kMsgListRooms;
    if (type == "chat") return kMsgChat;
    return kMsgOther;
}

// 从 "/stats?a=1&token=xyz" 里取 query 参数（值不做 URL 解码：token 用 base64 字母表）
std::string queryParam(const std::string& path, const std::string& key) {
    const size_t q = path.find('?');
    if (q == std::string::npos) return "";
    size_t pos = q + 1;
    while (pos < path.size()) {
        size_t amp = path.find('&', pos);
        const std::string pair =
            path.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.compare(0, eq, key) == 0) {
            return pair.substr(eq + 1);
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

double readRssMb() {
    std::ifstream f("/proc/self/statm");
    long totalPages = 0, residentPages = 0;
    if (!(f >> totalPages >> residentPages)) return 0.0;
    const long pageKb = ::sysconf(_SC_PAGESIZE) / 1024;
    return static_cast<double>(residentPages) * static_cast<double>(pageKb) / 1024.0;
}

int countOpenFds() {
    DIR* d = ::opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (struct dirent* e = ::readdir(d)) {
        if (e->d_name[0] != '.') ++n;
    }
    ::closedir(d);
    return n - 1;  // 减去 opendir 自己占的那个 fd
}

Json::Value histJson(const LatencyHist& h) {
    Json::Value v;
    v["p50"] = static_cast<Json::UInt64>(h.percentile(50));
    v["p95"] = static_cast<Json::UInt64>(h.percentile(95));
    v["p99"] = static_cast<Json::UInt64>(h.percentile(99));
    v["max"] = static_cast<Json::UInt64>(h.maxUs.load(std::memory_order_relaxed));
    v["samples"] = static_cast<Json::UInt64>(h.count.load(std::memory_order_relaxed));
    return v;
}

// 把计数与直方图清零，用于压测时"只看这一轮"。
// 只在 /stats?reset=1 且鉴权通过时调用；计数器是原子变量，并发下个别消息可能记到
// 清零前的桶里，这个精度对压测足够，所以不额外加锁。
void resetStats() {
    for (int i = 0; i < kMsgKindCount; ++i) g_msg_count[i].store(0, std::memory_order_relaxed);
    for (int i = 0; i < kErrKindCount; ++i) g_err_count[i].store(0, std::memory_order_relaxed);
    for (int i = 0; i < kLatBucketCount; ++i) {
        g_lat_join_room.buckets[i].store(0, std::memory_order_relaxed);
        g_lat_forward.buckets[i].store(0, std::memory_order_relaxed);
    }
    g_lat_join_room.count.store(0, std::memory_order_relaxed);
    g_lat_join_room.maxUs.store(0, std::memory_order_relaxed);
    g_lat_forward.count.store(0, std::memory_order_relaxed);
    g_lat_forward.maxUs.store(0, std::memory_order_relaxed);
}

// 汇总当前运行状态。会短暂持锁读 map 大小——这条路径每秒最多被调用几次，
// 与消息热路径无关，所以直接加锁是可接受的。
Json::Value statsJson() {
    Json::Value root;
    root["uptimeSeconds"] = static_cast<Json::Int64>(::time(nullptr) - g_startedAtSec);
    root["pid"] = static_cast<Json::Int>(::getpid());

    size_t wsCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        wsCount = g_ws_clients.size();
    }
    size_t authedCount = 0;
    for (int i = 0; i < kAuthShards; ++i) {
        std::lock_guard<std::mutex> lock(g_auth_shards[i].mutex);
        for (const auto& kv : g_auth_shards[i].map) {
            if (kv.second.authenticated) ++authedCount;
        }
    }
    root["connections"]["ws"] = static_cast<Json::UInt>(wsCount);
    root["connections"]["authed"] = static_cast<Json::UInt>(authedCount);
    root["connections"]["unauthenticated"] =
        static_cast<Json::UInt>(wsCount > authedCount ? wsCount - authedCount : 0);

    size_t roomTotal = 0, roomFull = 0, roomMembers = 0;
    {
        std::lock_guard<std::mutex> lock(g_rooms_mutex);
        roomTotal = g_rooms.size();
        for (const auto& kv : g_rooms) {
            roomMembers += kv.second.size();
            if (kv.second.size() >= 2) ++roomFull;
        }
    }
    root["rooms"]["total"] = static_cast<Json::UInt>(roomTotal);
    root["rooms"]["full"] = static_cast<Json::UInt>(roomFull);
    root["rooms"]["members"] = static_cast<Json::UInt>(roomMembers);

    static const char* kMsgNames[kMsgKindCount] = {
        "auth", "join_room", "offer", "answer", "ice",
        "bye", "video_mute", "mic_mute", "list_rooms", "chat", "other"};
    for (int i = 0; i < kMsgKindCount; ++i) {
        root["messages"][kMsgNames[i]] = static_cast<Json::UInt64>(
            g_msg_count[i].load(std::memory_order_relaxed));
    }

    static const char* kErrNames[kErrKindCount] = {
        "auth_fail", "auth_timeout", "room_full", "invalid_room_token", "bad_json",
        "rate_limited", "slow_client"};
    for (int i = 0; i < kErrKindCount; ++i) {
        root["errors"][kErrNames[i]] = static_cast<Json::UInt64>(
            g_err_count[i].load(std::memory_order_relaxed));
    }

    root["latencyUs"]["join_room"] = histJson(g_lat_join_room);
    root["latencyUs"]["forward"] = histJson(g_lat_forward);

    root["resources"]["rssMb"] = readRssMb();
    root["resources"]["fds"] = static_cast<Json::Int>(countOpenFds());
    return root;
}

// /stats 的访问控制：
//   配了 STATS_TOKEN -> 必须带 ?token=（用定长比较，避免逐字节时序泄漏）
//   没配            -> 只允许本机访问
bool statsAllowed(const TcpConnectionPtr& conn, const std::string& path) {
    if (!g_stats_token.empty()) {
        const std::string given = queryParam(path, "token");
        if (given.size() != g_stats_token.size()) return false;
        return CRYPTO_memcmp(given.data(), g_stats_token.data(), given.size()) == 0;
    }
    const std::string ip = conn->peerAddress().toIp();
    return ip == "127.0.0.1" || ip == "::1";
}

void sendHttp(const TcpConnectionPtr& conn, int status, const std::string& statusText,
              const std::string& contentType, const std::string& body) {
    std::string resp =
        "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n"
        "Content-Type: " + contentType + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n" + body;
    conn->send(resp);
    conn->shutdown();
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
    if (!g_turn_config.get("secret", "").asString().empty()) {
        LOG_INFO << "TURN 凭据模式：动态（每次通话签发临时凭据，TTL "
                 << g_turn_config.get("ttlSeconds", 7200).asInt64() << " 秒）";
    } else {
        LOG_WARN << "TURN 凭据模式：静态（turn_config.json 里写死了账号密码）";
    }
    return true;
}

// ========== 生成下发给浏览器的 iceServers ==========
// 两种模式：
//   1) 静态：iceServers 条目里直接写 username/credential（turn_config.json 里带账号密码）
//   2) 动态（推荐）：配置里有非空 secret，且条目带 "dynamic": true 时，
//      按本次通话生成 "<过期时间戳>:<userId>" + HMAC 凭据，随 start_call 一起下发。
// 动态模式的好处：长期密钥只存在服务器上，浏览器拿到的最多存活 ttlSeconds。
// 没有任何可用配置时返回 null（调用方据此不下发 iceServers）。
Json::Value buildIceServers(const ConnAuth& auth) {
    if (g_turn_config.isNull() || !g_turn_config.isMember("iceServers")) {
        return Json::Value();  // null
    }
    const Json::Value& servers = g_turn_config["iceServers"];
    if (!servers.isArray() || servers.empty()) {
        return Json::Value();
    }

    const std::string secret = g_turn_config.get("secret", "").asString();
    const bool dynamic = !secret.empty();
    int64_t ttl = g_turn_config.get("ttlSeconds", 7200).asInt64();
    if (ttl <= 0) ttl = 7200;

    std::string username;
    std::string credential;
    if (dynamic) {
        const int64_t expireAt = static_cast<int64_t>(::time(nullptr)) + ttl;
        username = std::to_string(expireAt) + ":" + std::to_string(auth.userId);
        credential = hmacSha1Base64(secret, username);
    }

    Json::Value out(Json::arrayValue);
    for (const auto& s : servers) {
        Json::Value entry = s;
        const bool wantDynamic = s.get("dynamic", false).asBool();
        // dynamic 是给服务端看的开关，不要漏给浏览器
        entry.removeMember("dynamic");
        if (dynamic && wantDynamic) {
            entry["username"] = username;
            entry["credential"] = credential;
        }
        out.append(entry);
    }
    return out;
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
    std::string roomId;
    std::vector<TcpConnectionPtr>* clients = RoomMembersLocked(conn, &roomId);
    if (!clients) return nullptr;

    clients->erase(std::find(clients->begin(), clients->end(), conn));
    SetConnRoomLocked(conn, "");  // 清掉索引缓存

    TcpConnectionPtr other = clients->empty() ? nullptr : (*clients)[0];
    if (clients->empty()) {
        // 房间"是否存在"由数据库负责，内存里这个空壳可以立刻回收
        g_rooms.erase(roomId);
        LOG_INFO << "房间 " << roomId << " 已空，回收运行时状态";
    } else {
        LOG_INFO << "客户端离开房间 " << roomId
                 << "，房间仍有 " << clients->size() << " 人（等待新客户端）";
    }
    return other;
}

// ========== 连接回调 ==========
void onConnection(const TcpConnectionPtr& conn) {
    if (conn->connected()) {
        // 关掉 Nagle：信令消息都是几十~几百字节的小包，且一问一答。
        // 不关的话，服务端"刚写完上一条、紧接着写转发消息"会被 Nagle 压住，
        // 等对端延迟 ACK（典型 40ms），实测 offer 转发多出 43ms。
        conn->setTcpNoDelay(true);
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
        // 先从房间移除，并通知对方。
        // 必须放在清理身份表之前：房间索引（ConnAuth.roomId）就存在身份表里，
        // 先擦掉它就只能退化回"遍历所有房间"。
        TcpConnectionPtr peer = removeFromRoom(conn);
        // 清理鉴权状态
        {
            AuthShard& shard = ShardOf(conn);
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.map.erase(conn);
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
    std::string roomId;
    std::vector<TcpConnectionPtr>* clients = RoomMembersLocked(conn, &roomId);
    if (!clients || roomId == keepRoomId) return nullptr;

    clients->erase(std::find(clients->begin(), clients->end(), conn));
    SetConnRoomLocked(conn, "");

    TcpConnectionPtr other = clients->empty() ? nullptr : (*clients)[0];
    LOG_INFO << "连接离开房间 " << roomId << "（切换到房间 " << keepRoomId << "）";
    if (clients->empty()) {
        g_rooms.erase(roomId);
    }
    return other;
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
// 注意：调用点在 g_rooms_mutex 之下，这里会再取身份分片锁（符合约定的加锁顺序）。
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
                AuthShard& shard = ShardOf(conn);
                std::lock_guard<std::mutex> lock(shard.mutex);
                ConnAuth& st = shard.map[conn];
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

        // 运行指标接口：不走 nginx（站点配置里没有这条 location），
        // 只给本机或持有 STATS_TOKEN 的采样端用。
        if (file_path == "/stats" || file_path.compare(0, 7, "/stats?") == 0) {
            if (!statsAllowed(conn, file_path)) {
                sendHttp(conn, 403, "Forbidden", "application/json; charset=utf-8",
                         "{\"error\":\"forbidden\"}");
                return;
            }
            // reset=1：压测前清一次窗口，这样读到的就是"这一轮"的数字。
            // 鉴权通过才会走到这里，所以不会变成随便谁都能清的接口。
            if (queryParam(file_path, "reset") == "1") {
                resetStats();
                LOG_INFO << "指标窗口已重置（/stats?reset=1）";
            }
            sendHttp(conn, 200, "OK", "application/json; charset=utf-8",
                     statsJson().toStyledString());
            return;
        }

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

        if (g_log_payload) {
            LOG_INFO << "收到消息：" << decoded;
        } else {
            LOG_DEBUG << "收到 ws 消息，长度=" << decoded.size();
        }

        Json::Value root;
        Json::Reader reader;
        if (!reader.parse(decoded, root)) {
            LOG_ERROR << "JSON 解析失败";
            g_err_count[kErrBadJson].fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::string type = root.get("type", "").asString();
        g_msg_count[MsgKindOf(type)].fetch_add(1, std::memory_order_relaxed);

        // 限流：超过令牌桶就断开。放在解析出 type 之后，这样 /stats 的 messages
        // 计数仍能如实反映"对方在刷什么"，便于事后判断是客户端 bug 还是攻击。
        if (!AllowMessage(conn)) {
            LOG_WARN << "连接 " << conn->peerAddress().toIpPort()
                     << " 消息频率超限（" << type << "），断开";
            g_err_count[kErrRateLimited].fetch_add(1, std::memory_order_relaxed);
            sendError(conn, "rate_limited", "消息发送过于频繁");
            conn->shutdown();
            continue;
        }

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
                g_err_count[kErrAuthFail].fetch_add(1, std::memory_order_relaxed);
                sendError(conn, "invalid_token", "凭证无效或已过期");
                continue;
            }

            {
                AuthShard& shard = ShardOf(conn);
                std::lock_guard<std::mutex> lock(shard.mutex);
                ConnAuth& st = shard.map[conn];
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
            // 记录整个入房处理耗时（验签 + 找房间 + 发 joined/start_call）。
            // 这是压测里最关心的一条：房间数变多时它会因为线性扫描而变慢。
            LatencyTimer joinTimer(g_lat_join_room);
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
                g_err_count[kErrInvalidRoomToken].fetch_add(1, std::memory_order_relaxed);
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
                g_err_count[kErrRoomFull].fetch_add(1, std::memory_order_relaxed);
                sendError(conn, "room_full", "房间已满");
                continue;
            }
            clients.push_back(conn);
            SetConnRoomLocked(conn, roomId);  // 记进索引，转发/离开就不用再遍历所有房间
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
                // 凭据按各自身份生成：动态模式下 username 里带的 userId 就是收件人自己
                const Json::Value iceForInitiator = buildIceServers(aAuth);
                const Json::Value iceForResponder = buildIceServers(rAuth);
                if (!iceForInitiator.isNull()) {
                    startMsg["iceServers"] = iceForInitiator;
                    LOG_INFO << "start_call 已注入 ICE 配置 ("
                             << iceForInitiator.size() << " 个服务器)";
                } else {
                    LOG_WARN << "start_call 未注入 ICE 配置（g_turn_config 为空或无 iceServers）";
                }
                initiator->send(wrapWsFrame(startMsg.toStyledString()));

                startMsg["initiator"] = false;
                startMsg["peer"] = aJson;  // 发给 responder 的，是对端(initiator)的身份
                if (!iceForResponder.isNull()) {
                    startMsg["iceServers"] = iceForResponder;
                } else {
                    startMsg.removeMember("iceServers");
                }
                responder->send(wrapWsFrame(startMsg.toStyledString()));

                LOG_INFO << "房间 " << roomId << " 已满，开始通话："
                         << aJson["displayName"].asString() << " <-> "
                         << rJson["displayName"].asString();
            }
            continue;
        }

        // 文字聊天：服务端只做两件事——限长 + 转发。服务端不存历史，
        // 客户端只保留本次通话的最近 100 条（刷新即清）。
        if (type == "chat") {
            if (!root["text"].isString() || root["text"].asString().empty()) {
                sendError(conn, "bad_request", "chat 消息缺少 text");
                continue;
            }
            const std::string rawText = root["text"].asString();
            const std::string text = TruncateUtf8(rawText, kMaxChatChars);
            if (text.size() != rawText.size()) {
                LOG_INFO << "chat 文本超长被截断：" << rawText.size()
                         << " 字节 -> " << text.size() << " 字节";
            }
            // 重新组包再转发：只透传 type 与 text，避免客户端塞别的字段进来
            Json::Value out;
            out["type"] = "chat";
            out["text"] = text;
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            decoded = Json::writeString(writer, out);
        }

        if (type == "offer" || type == "answer" || type == "ice" ||
            type == "video_mute" || type == "mic_mute" || type == "chat") {
            // 转发耗时：找对端（走房间索引，O(1)）+ 组帧 + 投递。
            // 注意它包含日志开销，属于现状基线；LOG_PAYLOAD 关掉后这个数字应明显下降。
            LatencyTimer forwardTimer(g_lat_forward);
            TcpConnectionPtr target = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_rooms_mutex);
                std::string roomId;
                std::vector<TcpConnectionPtr>* clients = RoomMembersLocked(conn, &roomId);
                if (clients) {
                    for (auto& c : *clients) {
                        if (c != conn) {
                            target = c;
                            break;
                        }
                    }
                }
            }
            if (!target || !target->connected()) {
                LOG_WARN << "未找到配对客户端，丢弃消息";
                continue;
            }
            // 背压：对端积压太多说明它读得太慢（或者连接已经半死不活）。
            // 继续往里塞只会让服务端内存跟着涨，所以直接断开它。
            const size_t backlog = target->outputBuffer()->readableBytes();
            if (backlog > kSlowClientBytes) {
                LOG_WARN << "对端发送缓冲积压 " << backlog << " 字节（阈值 "
                         << kSlowClientBytes << "），判定为慢客户端并断开："
                         << target->peerAddress().toIpPort();
                g_err_count[kErrSlowClient].fetch_add(1, std::memory_order_relaxed);
                target->shutdown();
                continue;
            }
            if (g_log_payload) {
                LOG_INFO << "转发 " << type << " 长度=" << decoded.size()
                         << " 内容预览: "
                         << decoded.substr(0, std::min(decoded.size(), (size_t)100));
            } else {
                LOG_DEBUG << "转发 " << type << " 长度=" << decoded.size();
            }
            std::string frameData = wrapWsFrame(decoded);
            if (frameData.empty()) {
                LOG_ERROR << "wrapWsFrame 生成空帧，丢弃消息，原始数据长度=" << decoded.size();
            } else {
                target->send(frameData);
                LOG_DEBUG << "转发 " << type << " 成功，帧长度=" << frameData.size();
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

// ========== 优雅下线（SIGTERM / SIGINT）==========
// systemctl restart / stop 发的是 SIGTERM。不接管的话进程立刻消失，
// 客户端只看到 TCP 断开、界面上什么都没变，用户会以为是自己网断了。
// 做法：信号处理器里只往自管道写一个字节（handler 里只能调异步信号安全的函数），
// 真正的收尾交给 EventLoop 线程。
int g_signalPipe[2] = {-1, -1};

void OnSignal(int sig) {
    const char c = static_cast<char>(sig);
    const ssize_t n = ::write(g_signalPipe[1], &c, 1);
    (void)n;  // 管道满/被中断都无所谓，收到一次就够
}

void StartGracefulShutdown(EventLoop* loop) {
    static std::atomic<bool> started(false);
    bool expected = false;
    if (!started.compare_exchange_strong(expected, true)) return;

    LOG_INFO << "收到退出信号，开始优雅下线（广播 server_shutdown 后退出）";

    std::vector<TcpConnectionPtr> clients;
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        clients.assign(g_ws_clients.begin(), g_ws_clients.end());
    }

    Json::Value msg;
    msg["type"] = "server_shutdown";
    msg["reason"] = "restart";
    msg["retryAfterMs"] = 3000;
    const std::string frame = wrapWsFrame(msg.toStyledString());
    size_t sent = 0;
    for (auto& conn : clients) {
        if (!conn || !conn->connected()) continue;
        conn->send(frame);
        ++sent;
    }
    LOG_INFO << "已通知 " << sent << " 个连接，200ms 后关闭";

    // 先等 200ms 让 send 出去的字节真正刷到网卡，再断开、再退出。
    loop->runAfter(0.2, [loop, clients]() {
        for (auto& conn : clients) {
            if (conn && conn->connected()) conn->shutdown();
        }
        loop->runAfter(0.2, [loop]() { loop->quit(); });
    });
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
    // 指标接口的访问令牌：配了就只认 token，没配就只允许本机访问。
    if (const char* t = ::getenv("STATS_TOKEN")) {
        g_stats_token = t;
    }
    // 是否把消息原文打进日志（默认关，见 g_log_payload 的注释）
    if (const char* p = ::getenv("LOG_PAYLOAD")) {
        const std::string flag(p);
        g_log_payload = (flag == "1" || flag == "true" || flag == "yes");
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

    // 接管 SIGTERM / SIGINT：systemd 的 stop/restart 走这条路。
    if (::pipe(g_signalPipe) == 0) {
        ::fcntl(g_signalPipe[0], F_SETFL, O_NONBLOCK);
        ::fcntl(g_signalPipe[1], F_SETFL, O_NONBLOCK);
        // 故意用 new 并一直持有：这个 Channel 要活到进程结束，
        // 用栈对象会在 main 返回后先析构、晚于 EventLoop，反而危险。
        auto* sigChannel = new Channel(&loop, g_signalPipe[0]);
        sigChannel->setReadCallback([&loop](Timestamp) {
            char c;
            while (::read(g_signalPipe[0], &c, 1) > 0) {
            }
            StartGracefulShutdown(&loop);
        });
        sigChannel->enableReading();
        ::signal(SIGTERM, OnSignal);
        ::signal(SIGINT, OnSignal);
    } else {
        LOG_WARN << "建立信号自管道失败，SIGTERM 将按默认行为直接退出";
    }

    // 鉴权超时回收：每秒扫一次，把"建连后 5 秒内没发 auth"的连接关掉。
    // 目的是防止有人只挂连接不认证、白占服务端资源。
    loop.runEvery(1.0, []() {
        std::vector<TcpConnectionPtr> expired;
        const int64_t now = NowMs();
        for (int i = 0; i < kAuthShards; ++i) {
            std::lock_guard<std::mutex> lock(g_auth_shards[i].mutex);
            for (auto& kv : g_auth_shards[i].map) {
                if (!kv.second.authenticated && kv.second.deadlineMs > 0 &&
                    now > kv.second.deadlineMs) {
                    expired.push_back(kv.first);
                }
            }
        }
        // 注意：必须在释放分片锁之后再关闭连接，
        // 否则 onConnection 里再去取分片锁会自死锁。
        for (auto& conn : expired) {
            if (!conn || !conn->connected()) continue;
            LOG_INFO << "鉴权超时，关闭连接: " << conn->peerAddress().toIpPort();
            g_err_count[kErrAuthTimeout].fetch_add(1, std::memory_order_relaxed);
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
        LOG_INFO << "ICE 配置：从 turn_config.json 动态下发"
                 << (g_turn_config.get("secret", "").asString().empty() ? "（静态凭据）"
                                                                        : "（临时凭据）");
    }
    LOG_INFO << "WebSocket 心跳：每 30 秒 ping，连续两次无 pong 回收连接";
    LOG_INFO << "鉴权：" << (g_auth_required ? "开启（需在 5 秒内 auth）" : "关闭（匿名模式）");
    LOG_INFO << "指标接口：/stats " << (g_stats_token.empty() ? "（仅本机可访问）"
                                                              : "（需要 STATS_TOKEN）");
    LOG_INFO << "消息日志：" << (g_log_payload ? "含消息原文（LOG_PAYLOAD=1）"
                                               : "只记类型与长度（LOG_PAYLOAD=1 可打开）");
    LOG_INFO << "优雅下线：收到 SIGTERM/SIGINT 会先通知客户端再退出";
    LOG_INFO << "=====================================";

    loop.loop();
    return 0;
}
