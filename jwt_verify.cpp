#include "jwt_verify.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <json/json.h>

#include <memory>

namespace jwt {

const char* kOk = "ok";
const char* kMalformed = "malformed";
const char* kBadAlgorithm = "bad_algorithm";
const char* kBadSignature = "bad_signature";
const char* kExpired = "token_expired";
const char* kBadType = "bad_type";

namespace {

// 时钟偏移容差，与 Go 侧保持一致（60 秒）
const int64_t kClockSkew = 60;

int B64UrlValue(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// 定长时间比较：无论在第几个字节发现不同，都完整走完整个长度，
// 避免攻击者用响应时间逐字节猜签名。
bool ConstantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

std::string HmacSha256(const std::string& key, const std::string& data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outLen = 0;
    const unsigned char* result = HMAC(
        EVP_sha256(),
        key.data(), static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char*>(data.data()), data.size(),
        out, &outLen);
    if (result == nullptr) return std::string();
    return std::string(reinterpret_cast<const char*>(out), outLen);
}

// 切成 header.payload.signature 三段；签名段允许为空（alg=none 会走到这里，
// 之后会因为 alg 校验失败被拒）。
bool SplitToken(const std::string& token, std::string* header,
                std::string* payload, std::string* signature) {
    size_t d1 = token.find('.');
    if (d1 == std::string::npos) return false;
    size_t d2 = token.find('.', d1 + 1);
    if (d2 == std::string::npos) return false;
    if (token.find('.', d2 + 1) != std::string::npos) return false;

    *header = token.substr(0, d1);
    *payload = token.substr(d1 + 1, d2 - d1 - 1);
    *signature = token.substr(d2 + 1);
    return !header->empty() && !payload->empty();
}

bool ParseJsonObject(const std::string& text, Json::Value* out) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::string errs;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader) return false;
    if (!reader->parse(text.data(), text.data() + text.size(), out, &errs)) return false;
    return out->isObject();
}

}  // namespace

bool Base64UrlDecode(const std::string& in, std::string& out) {
    out.clear();
    if (in.empty()) return true;
    // base64 编码后长度 %4 不可能等于 1
    if (in.size() % 4 == 1) return false;

    out.reserve(in.size() * 3 / 4 + 3);
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        int v = B64UrlValue(static_cast<unsigned char>(in[i]));
        if (v < 0) return false;
        buffer = (buffer << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return true;
}

std::string Verify(const std::string& token, const std::string& secret,
                   int64_t nowUnix, Claims* claims) {
    if (claims == nullptr) return kMalformed;

    std::string headerB64, payloadB64, signatureB64;
    if (!SplitToken(token, &headerB64, &payloadB64, &signatureB64)) return kMalformed;

    // ---- 1) header：算法白名单 ----
    // 不能"信任 token 声称的 alg"：把 alg 改成 none 若跟着走就等于没有签名。
    std::string headerJson;
    if (!Base64UrlDecode(headerB64, headerJson)) return kMalformed;
    Json::Value header;
    if (!ParseJsonObject(headerJson, &header)) return kMalformed;
    if (header["alg"].asString() != "HS256") return kBadAlgorithm;

    // ---- 2) 签名：定长时间比较 ----
    std::string expected = HmacSha256(secret, headerB64 + "." + payloadB64);
    if (expected.empty()) return kBadSignature;
    std::string actual;
    if (!Base64UrlDecode(signatureB64, actual)) return kBadSignature;
    if (!ConstantTimeEqual(expected, actual)) return kBadSignature;

    // ---- 3) 先验签、后解析载荷（顺序不能反）----
    std::string payloadJson;
    if (!Base64UrlDecode(payloadB64, payloadJson)) return kMalformed;
    Json::Value payload;
    if (!ParseJsonObject(payloadJson, &payload)) return kMalformed;

    Claims c;
    c.typ = payload["typ"].asString();
    if (c.typ != "access") return kBadType;

    c.sub = payload["sub"].asInt64();
    c.username = payload["username"].asString();
    c.displayName = payload["name"].asString();
    c.iat = payload["iat"].asInt64();
    c.exp = payload["exp"].asInt64();
    c.jti = payload["jti"].asString();

    if (c.sub <= 0 || c.exp == 0) return kMalformed;
    if (nowUnix > c.exp + kClockSkew) return kExpired;

    // 展示名缺省时回退到登录名，保证 UI 永远有东西可显示
    if (c.displayName.empty()) c.displayName = c.username;

    *claims = c;
    return kOk;
}

}  // namespace jwt
