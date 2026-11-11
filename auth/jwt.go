package main

// JWT（HS256）的签发与验签——刻意只用标准库实现，不引第三方库。
// 好处：① 依赖少；② 每个字节都清楚，便于和 C++ 侧做交叉验证；
//       ③ 面试时能讲清楚"为什么 alg 必须白名单校验"、"为什么用定长时间比较"。

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strings"
	"time"
)

const (
	// TokenTypeAccess 标记 access token，防止 refresh token 或其它用途的 token 被冒用
	TokenTypeAccess = "access"

	// clockSkew 允许的时钟偏移。服务器时间漂移时，不愿把刚签发的 token 判成过期，
	// 也不愿让过期 token 多活太久，60 秒是常见取值。
	clockSkew = 60 * time.Second
)

var (
	ErrTokenMalformed = errors.New("token 格式不合法")
	ErrTokenAlgorithm = errors.New("token 算法不被接受")
	ErrTokenSignature = errors.New("token 签名校验失败")
	ErrTokenExpired   = errors.New("token 已过期")
	ErrTokenType      = errors.New("token 类型不匹配")
)

// Claims 是 access token 的载荷。
type Claims struct {
	Sub      int64  `json:"sub"`      // 用户 id
	Username string `json:"username"` // 登录名，便于日志与排查，不作为鉴权依据
	Name     string `json:"name"`     // 展示名；信令服务据此显示真实姓名，无需再查库
	Typ      string `json:"typ"`      // 固定 "access"
	Iat      int64  `json:"iat"`      // 签发时间（unix 秒）
	Exp      int64  `json:"exp"`      // 过期时间（unix 秒）
	Jti      string `json:"jti"`      // 唯一 id，便于将来做黑名单
}

// jwtHeader 固定为 HS256/JWT。验签时仍然会解析真实 header 并校验 alg，
// 不接受 token 自带 header 里的任意算法。
var jwtHeader = base64.RawURLEncoding.EncodeToString([]byte(`{"alg":"HS256","typ":"JWT"}`))

// signAccessToken 签发 access token。
func signAccessToken(secret []byte, userID int64, username, displayName string, ttl time.Duration, now time.Time) (string, error) {
	jti, err := randomToken(16)
	if err != nil {
		return "", err
	}
	claims := Claims{
		Sub:      userID,
		Username: username,
		Name:     displayName,
		Typ:      TokenTypeAccess,
		Iat:      now.Unix(),
		Exp:      now.Add(ttl).Unix(),
		Jti:      jti,
	}
	payload, err := json.Marshal(claims)
	if err != nil {
		return "", err
	}
	signingInput := jwtHeader + "." + base64.RawURLEncoding.EncodeToString(payload)
	sig := signHS256(secret, signingInput)
	return signingInput + "." + base64.RawURLEncoding.EncodeToString(sig), nil
}

// verifyAccessToken 验签并返回载荷。
func verifyAccessToken(secret []byte, token string, now time.Time) (*Claims, error) {
	parts := strings.Split(token, ".")
	if len(parts) != 3 || parts[0] == "" || parts[1] == "" || parts[2] == "" {
		return nil, ErrTokenMalformed
	}

	// 1) 解析 header 并做算法白名单校验。
	//    不能"信任 token 声称的 alg"：攻击者把 alg 改成 "none"，若实现跟着走就等于没有签名。
	headerJSON, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return nil, ErrTokenMalformed
	}
	var header struct {
		Alg string `json:"alg"`
	}
	if err := json.Unmarshal(headerJSON, &header); err != nil {
		return nil, ErrTokenMalformed
	}
	if header.Alg != "HS256" {
		return nil, ErrTokenAlgorithm
	}

	// 2) 定长时间比较签名，防时序侧信道。
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte(parts[0] + "." + parts[1]))
	expected := mac.Sum(nil)
	got, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return nil, ErrTokenMalformed
	}
	if !hmac.Equal(expected, got) {
		return nil, ErrTokenSignature
	}

	// 3) 先验签、后解析载荷——顺序不能反，否则等于信任了未签名的数据。
	payloadJSON, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return nil, ErrTokenMalformed
	}
	var claims Claims
	if err := json.Unmarshal(payloadJSON, &claims); err != nil {
		return nil, ErrTokenMalformed
	}
	if claims.Typ != TokenTypeAccess {
		return nil, ErrTokenType
	}
	if claims.Exp == 0 || now.Unix() > claims.Exp+int64(clockSkew/time.Second) {
		return nil, ErrTokenExpired
	}
	if claims.Sub <= 0 {
		return nil, ErrTokenMalformed
	}
	return &claims, nil
}

func signHS256(secret []byte, input string) []byte {
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte(input))
	return mac.Sum(nil)
}

// randomToken 生成 n 字节随机数并做 base64url（无填充）编码。
// 用于 refresh token 与 jti；使用 crypto/rand，绝不用 math/rand。
func randomToken(n int) (string, error) {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(b), nil
}

// hashRefreshToken 把刷新令牌做 sha256 后存库。
// 数据库里永远不出现可用的明文令牌。
func hashRefreshToken(token string) string {
	sum := sha256.Sum256([]byte(token))
	return hex.EncodeToString(sum[:])
}
