#pragma once

// JWT（HS256）验签与解析。
//
// 设计约束：
//   * 只依赖 OpenSSL 与 jsoncpp，不依赖 muduo —— 这样可以写独立自检程序，
//     用 Go 侧真实签发的 token 做跨语言交叉验证（见 tests/jwt_selftest.cpp）。
//   * 算法固定 HS256，绝不跟随 token 自称的 alg（防 alg=none 攻击）。
//   * 签名比较用定长时间，防时序侧信道。
//   * 先验签、后解析载荷。

#include <cstdint>
#include <string>

namespace jwt {

// 返回值：kOk 表示通过，其余为错误码（machine-readable，便于日志与测试）
extern const char* kOk;
extern const char* kMalformed;     // 结构不是三段 / base64url 非法 / JSON 非法
extern const char* kBadAlgorithm;  // alg 不是 HS256（含 alg=none）
extern const char* kBadSignature;  // 签名不匹配（含被篡改的载荷）
extern const char* kExpired;       // 已过期（超过 exp + 60s 容差）
extern const char* kBadType;       // typ 与期望不符

// 令牌类型：登录凭证 / 进房通行证
extern const char* kTypAccess;
extern const char* kTypRoom;

struct Claims {
    int64_t sub = 0;                // 用户 id
    std::string username;           // 登录名
    std::string displayName;        // 展示名（缺省时回退为 username）
    std::string room;               // 房间号（仅 room 令牌）
    std::string typ;                // access | room
    int64_t iat = 0;
    int64_t exp = 0;
    std::string jti;
};

// 验签并解析。成功返回 kOk 并填充 claims。
// expectedTyp 必须是 kTypAccess 或 kTypRoom：签名正确但类型不对的令牌一律拒绝。
// 这样登录凭证无法被拿去当进房凭证，进房凭证也无法被当登录凭证。
std::string Verify(const std::string& token,
                   const std::string& secret,
                   const std::string& expectedTyp,
                   int64_t nowUnix,
                   Claims* claims);

// base64url（无填充）解码，供自检程序复用
bool Base64UrlDecode(const std::string& in, std::string& out);

}  // namespace jwt
