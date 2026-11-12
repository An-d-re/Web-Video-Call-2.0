package main

// 生成 C++ 侧的 JWT 交叉验证向量。
//
// 只在显式要求时运行，避免普通 `go test` 去写仓库文件：
//   WRITE_JWT_VECTORS=1 go test -run TestWriteJwtVectors ./...
//
// 产物：../tests/jwt_vectors.json，由 tests/jwt_selftest.cpp 读取。
// 这些向量全部由「本包真实的签发函数」生成，所以能真正验证
// C++ 的验签实现与 Go 的签发实现是否一致。

import (
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestWriteJwtVectors(t *testing.T) {
	if os.Getenv("WRITE_JWT_VECTORS") == "" {
		t.Skip("设置 WRITE_JWT_VECTORS=1 才生成向量")
	}

	const secret = "0123456789abcdef0123456789abcdef" // 32 字节
	secretBytes := []byte(secret)
	now := time.Unix(1700000000, 0)
	nowUnix := now.Unix()

	mustToken := func(id int64, user, name string, ttl time.Duration) string {
		t.Helper()
		tok, err := signAccessToken(secretBytes, id, user, name, ttl, now)
		if err != nil {
			t.Fatalf("签发失败: %v", err)
		}
		return tok
	}

	valid := mustToken(42, "alice", "阿伟", time.Hour)
	expired := mustToken(7, "expired_user", "过期用户", -time.Hour)

	// 用另一个密钥签发 → 验签必须失败
	otherSecret := []byte("ffffffffffffffffffffffffffffffff")
	wrongSig, err := signAccessToken(otherSecret, 42, "alice", "阿伟", time.Hour, now)
	if err != nil {
		t.Fatalf("签发失败: %v", err)
	}

	// 篡改载荷：sub 改成 999，签名保持原样
	parts := strings.Split(valid, ".")
	if len(parts) != 3 {
		t.Fatalf("token 结构异常")
	}
	tamperedClaims := Claims{
		Sub: 999, Username: "alice", Name: "阿伟", Typ: TokenTypeAccess,
		Iat: nowUnix, Exp: now.Add(time.Hour).Unix(), Jti: "tampered",
	}
	tamperedBody, _ := json.Marshal(tamperedClaims)
	tampered := parts[0] + "." + base64.RawURLEncoding.EncodeToString(tamperedBody) + "." + parts[2]

	// alg=none：最经典的 JWT 漏洞
	noneHeader := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"none","typ":"JWT"}`))
	algNone := noneHeader + "." + base64.RawURLEncoding.EncodeToString(tamperedBody) + "."

	// typ 不是 access（例如拿 refresh 当 access 用）
	refreshClaims := Claims{
		Sub: 1, Username: "bob", Name: "小明", Typ: "refresh",
		Iat: nowUnix, Exp: now.Add(time.Hour).Unix(), Jti: "wrongtype",
	}
	refreshBody, _ := json.Marshal(refreshClaims)
	refreshInput := jwtHeader + "." + base64.RawURLEncoding.EncodeToString(refreshBody)
	wrongType := refreshInput + "." + base64.RawURLEncoding.EncodeToString(signHS256(secretBytes, refreshInput))

	// room 类型令牌（进房通行证），用于验证类型隔离
	roomToken, err := signRoomToken(secretBytes, 42, "alice", "ABC234", 2*time.Minute, now)
	if err != nil {
		t.Fatalf("签发 room 令牌失败: %v", err)
	}

	cases := []map[string]any{
		{
			"name": "valid", "token": valid, "expect": "ok",
			"sub": 42, "username": "alice", "displayName": "阿伟",
		},
		{"name": "expired", "token": expired, "expect": "token_expired"},
		{"name": "wrong_secret", "token": wrongSig, "expect": "bad_signature"},
		{"name": "tampered_payload", "token": tampered, "expect": "bad_signature"},
		{"name": "alg_none", "token": algNone, "expect": "bad_algorithm"},
		{"name": "wrong_type", "token": wrongType, "expect": "bad_type"},
		{"name": "malformed", "token": "only.two", "expect": "malformed"},
		{"name": "empty", "token": "", "expect": "malformed"},

		// 类型隔离：room 令牌按 room 验签应通过，按 access 验签必须失败
		{
			"name": "room_token_valid", "token": roomToken, "expect": "ok", "expectTyp": "room",
			"sub": 42, "username": "alice", "room": "ABC234",
		},
		{"name": "room_token_as_access", "token": roomToken, "expect": "bad_type", "expectTyp": "access"},
		{"name": "access_token_as_room", "token": valid, "expect": "bad_type", "expectTyp": "room"},
	}

	doc := map[string]any{
		"_comment": "由 auth/vectors_test.go 生成，勿手改。验证实现见 tests/jwt_selftest.cpp",
		"secret":   secret,
		"now":      nowUnix,
		"cases":    cases,
	}

	raw, err := json.MarshalIndent(doc, "", "  ")
	if err != nil {
		t.Fatalf("序列化失败: %v", err)
	}

	dest := filepath.Join("..", "tests", "jwt_vectors.json")
	if err := os.MkdirAll(filepath.Dir(dest), 0o755); err != nil {
		t.Fatalf("创建目录失败: %v", err)
	}
	if err := os.WriteFile(dest, append(raw, '\n'), 0o644); err != nil {
		t.Fatalf("写入失败: %v", err)
	}
	t.Logf("已写入 %s（%d 个用例）", dest, len(cases))
}
