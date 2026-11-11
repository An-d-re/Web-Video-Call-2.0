// JWT 验签的跨语言自检程序。
//
// 向量由 Go 侧真实签发函数生成（auth/vectors_test.go），
// 因此这里验证的是"C++ 的验签实现是否与 Go 的签发实现一致"，
// 而不是自己跟自己对答案。
//
// 编译（在仓库根目录，需要 libjsoncpp 与 libcrypto）：
//   g++ -std=c++17 -O2 -I. jwt_verify.cpp tests/jwt_selftest.cpp -o jwt_selftest -ljsoncpp -lcrypto
//
// 运行：
//   ./jwt_selftest tests/jwt_vectors.json
//
// 退出码：0 全部通过；1 有用例失败；2 环境/文件问题。

#include "jwt_verify.h"

#include <json/json.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace {

bool ReadFile(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

bool ParseJson(const std::string& text, Json::Value* out) {
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    std::string errs;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader) return false;
    return reader->parse(text.data(), text.data() + text.size(), out, &errs);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "tests/jwt_vectors.json";

    std::string text;
    if (!ReadFile(path, &text)) {
        std::cerr << "无法读取向量文件: " << path << "\n";
        return 2;
    }

    Json::Value doc;
    if (!ParseJson(text, &doc)) {
        std::cerr << "向量文件不是合法 JSON: " << path << "\n";
        return 2;
    }

    const std::string secret = doc["secret"].asString();
    const int64_t now = doc["now"].asInt64();
    const Json::Value& cases = doc["cases"];

    if (!cases.isArray() || cases.empty()) {
        std::cerr << "向量文件里没有 cases\n";
        return 2;
    }

    int passed = 0;
    int failed = 0;

    for (Json::Value::const_iterator it = cases.begin(); it != cases.end(); ++it) {
        const Json::Value& c = *it;
        const std::string name = c["name"].asString();
        const std::string token = c["token"].asString();
        const std::string expect = c["expect"].asString();

        jwt::Claims claims;
        const std::string got = jwt::Verify(token, secret, now, &claims);

        bool ok = (got == expect);
        std::string detail;

        // 通过用例还要核对载荷字段，确保不是"返回 ok 但内容错了"
        if (ok && expect == "ok") {
            const int64_t wantSub = c["sub"].asInt64();
            const std::string wantUser = c["username"].asString();
            const std::string wantName = c["displayName"].asString();
            if (claims.sub != wantSub || claims.username != wantUser ||
                claims.displayName != wantName) {
                ok = false;
                std::ostringstream ss;
                ss << "载荷不符: sub=" << claims.sub
                   << " username=" << claims.username
                   << " displayName=" << claims.displayName;
                detail = ss.str();
            }
        }

        if (ok) {
            ++passed;
            std::cout << "PASS  " << name << "  (" << got << ")\n";
        } else {
            ++failed;
            std::cout << "FAIL  " << name << "  期望 " << expect << "，实际 " << got;
            if (!detail.empty()) std::cout << "  " << detail;
            std::cout << "\n";
        }
    }

    std::cout << "\n合计: " << passed << " 通过, " << failed << " 失败\n";
    return failed == 0 ? 0 : 1;
}
