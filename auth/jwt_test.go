package main

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"
)

var testSecret = []byte("0123456789abcdef0123456789abcdef") // 32 字节

func TestSignAndVerify(t *testing.T) {
	now := time.Unix(1700000000, 0)
	tok, err := signAccessToken(testSecret, 42, "alice", "阿伟", time.Hour, now)
	if err != nil {
		t.Fatalf("签发失败: %v", err)
	}
	if strings.Count(tok, ".") != 2 {
		t.Fatalf("token 不是三段式: %s", tok)
	}

	c, err := verifyAccessToken(testSecret, tok, now.Add(time.Minute))
	if err != nil {
		t.Fatalf("验签失败: %v", err)
	}
	if c.Sub != 42 || c.Username != "alice" || c.Name != "阿伟" || c.Typ != TokenTypeAccess {
		t.Fatalf("载荷不符: %+v", c)
	}
	if c.Exp != now.Add(time.Hour).Unix() {
		t.Fatalf("exp 不符: %d", c.Exp)
	}
	if c.Jti == "" {
		t.Fatal("jti 不应为空")
	}
}

func TestVerifyExpired(t *testing.T) {
	now := time.Unix(1700000000, 0)
	tok, _ := signAccessToken(testSecret, 1, "bob", "小明", time.Hour, now)

	// 超过 exp + 60s 容差 → 过期
	_, err := verifyAccessToken(testSecret, tok, now.Add(time.Hour+61*time.Second))
	if !errors.Is(err, ErrTokenExpired) {
		t.Fatalf("期望 ErrTokenExpired，实际: %v", err)
	}
}

func TestVerifyWithinClockSkew(t *testing.T) {
	now := time.Unix(1700000000, 0)
	tok, _ := signAccessToken(testSecret, 1, "bob", "小明", time.Hour, now)

	// 刚过期 30 秒，仍在容差内 → 应通过
	if _, err := verifyAccessToken(testSecret, tok, now.Add(time.Hour+30*time.Second)); err != nil {
		t.Fatalf("容差内不应被拒: %v", err)
	}
}

func TestVerifyWrongSecret(t *testing.T) {
	now := time.Unix(1700000000, 0)
	tok, _ := signAccessToken(testSecret, 1, "bob", "小明", time.Hour, now)

	other := []byte("ffffffffffffffffffffffffffffffff")
	if _, err := verifyAccessToken(other, tok, now); !errors.Is(err, ErrTokenSignature) {
		t.Fatalf("期望 ErrTokenSignature，实际: %v", err)
	}
}

func TestVerifyTamperedPayload(t *testing.T) {
	now := time.Unix(1700000000, 0)
	tok, _ := signAccessToken(testSecret, 1, "bob", "小明", time.Hour, now)
	parts := strings.Split(tok, ".")

	// 把 sub 从 1 改成 999，签名保持原样
	claims := Claims{Sub: 999, Username: "bob", Typ: TokenTypeAccess,
		Iat: now.Unix(), Exp: now.Add(time.Hour).Unix(), Jti: "x"}
	body, _ := json.Marshal(claims)
	parts[1] = base64.RawURLEncoding.EncodeToString(body)
	forged := strings.Join(parts, ".")

	if _, err := verifyAccessToken(testSecret, forged, now); !errors.Is(err, ErrTokenSignature) {
		t.Fatalf("篡改载荷必须被拒，实际: %v", err)
	}
}

// 最经典的 JWT 漏洞：把 header 的 alg 改成 none，让实现跳过验签。
func TestVerifyRejectsAlgNone(t *testing.T) {
	now := time.Unix(1700000000, 0)
	header := base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"none","typ":"JWT"}`))
	claims := Claims{Sub: 1, Username: "bob", Typ: TokenTypeAccess,
		Iat: now.Unix(), Exp: now.Add(time.Hour).Unix(), Jti: "x"}
	body, _ := json.Marshal(claims)
	payload := base64.RawURLEncoding.EncodeToString(body)
	none := header + "." + payload + "."

	_, err := verifyAccessToken(testSecret, none, now)
	if !errors.Is(err, ErrTokenAlgorithm) && !errors.Is(err, ErrTokenMalformed) {
		t.Fatalf("alg=none 必须被拒，实际: %v", err)
	}
}

func TestVerifyRejectsWrongType(t *testing.T) {
	now := time.Unix(1700000000, 0)
	claims := Claims{Sub: 1, Username: "bob", Typ: "refresh",
		Iat: now.Unix(), Exp: now.Add(time.Hour).Unix(), Jti: "x"}
	body, _ := json.Marshal(claims)
	signingInput := jwtHeader + "." + base64.RawURLEncoding.EncodeToString(body)
	sig := base64.RawURLEncoding.EncodeToString(signHS256(testSecret, signingInput))

	if _, err := verifyAccessToken(testSecret, signingInput+"."+sig, now); !errors.Is(err, ErrTokenType) {
		t.Fatalf("typ 不为 access 必须被拒，实际: %v", err)
	}
}

func TestVerifyMalformed(t *testing.T) {
	now := time.Unix(1700000000, 0)
	for _, bad := range []string{"", "abc", "a.b", "a.b.c.d", "..", "a.b.c"} {
		if _, err := verifyAccessToken(testSecret, bad, now); err == nil {
			t.Fatalf("非法 token %q 不应通过", bad)
		}
	}
}

func TestHashRefreshTokenStable(t *testing.T) {
	a := hashRefreshToken("abc")
	b := hashRefreshToken("abc")
	c := hashRefreshToken("abd")
	if a != b {
		t.Fatal("同一输入应得到同一摘要")
	}
	if a == c {
		t.Fatal("不同输入摘要不应相同")
	}
	if len(a) != 64 {
		t.Fatalf("sha256 十六进制应为 64 字符，实际 %d", len(a))
	}
}
