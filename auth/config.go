package main

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

// Config 全部来自环境变量，不读取任何配置文件——
// 让 auth 服务成为"纯二进制 + 环境变量"的可搬移单元。
type Config struct {
	ListenAddr        string        // AUTH_LISTEN，默认 127.0.0.1:8080（只监听回环）
	DatabaseURL       string        // DATABASE_URL，必填
	JWTSecret         []byte        // JWT_SECRET，必填且 ≥32 字节
	AccessTokenTTL    time.Duration // ACCESS_TOKEN_TTL，默认 2h
	RefreshTokenTTL   time.Duration // REFRESH_TOKEN_TTL，默认 168h(7d)
	AllowedOrigins    []string      // ALLOWED_ORIGINS，逗号分隔；为空=只接受同源
	CookieSecure      bool          // COOKIE_SECURE，默认 true；本地 HTTP 调试设 false
	AllowRegistration bool          // ALLOW_REGISTRATION，默认 true
	TrustProxy        bool          // TRUST_PROXY，默认 true（服务只监听回环，由 nginx 反代）
}

const minSecretLen = 32

func LoadConfig() (*Config, error) {
	cfg := &Config{
		ListenAddr:        envOr("AUTH_LISTEN", "127.0.0.1:8080"),
		DatabaseURL:       os.Getenv("DATABASE_URL"),
		JWTSecret:         []byte(os.Getenv("JWT_SECRET")),
		AccessTokenTTL:    2 * time.Hour,
		RefreshTokenTTL:   7 * 24 * time.Hour,
		CookieSecure:      true,
		AllowRegistration: true,
		TrustProxy:        true,
	}

	if cfg.DatabaseURL == "" {
		return nil, errors.New("环境变量 DATABASE_URL 未设置")
	}
	if len(cfg.JWTSecret) < minSecretLen {
		return nil, fmt.Errorf("环境变量 JWT_SECRET 未设置或过短（需 ≥%d 字节，当前 %d 字节）",
			minSecretLen, len(cfg.JWTSecret))
	}

	if v := os.Getenv("ACCESS_TOKEN_TTL"); v != "" {
		d, err := time.ParseDuration(v)
		if err != nil || d <= 0 {
			return nil, fmt.Errorf("ACCESS_TOKEN_TTL 非法：%q（示例：2h）", v)
		}
		cfg.AccessTokenTTL = d
	}
	if v := os.Getenv("REFRESH_TOKEN_TTL"); v != "" {
		d, err := time.ParseDuration(v)
		if err != nil || d <= 0 {
			return nil, fmt.Errorf("REFRESH_TOKEN_TTL 非法：%q（示例：168h）", v)
		}
		cfg.RefreshTokenTTL = d
	}
	if v := os.Getenv("COOKIE_SECURE"); v != "" {
		b, err := strconv.ParseBool(v)
		if err != nil {
			return nil, fmt.Errorf("COOKIE_SECURE 非法：%q（true/false）", v)
		}
		cfg.CookieSecure = b
	}
	if v := os.Getenv("ALLOW_REGISTRATION"); v != "" {
		b, err := strconv.ParseBool(v)
		if err != nil {
			return nil, fmt.Errorf("ALLOW_REGISTRATION 非法：%q（true/false）", v)
		}
		cfg.AllowRegistration = b
	}
	if v := os.Getenv("TRUST_PROXY"); v != "" {
		b, err := strconv.ParseBool(v)
		if err != nil {
			return nil, fmt.Errorf("TRUST_PROXY 非法：%q（true/false）", v)
		}
		cfg.TrustProxy = b
	}
	if v := os.Getenv("ALLOWED_ORIGINS"); v != "" {
		for _, o := range strings.Split(v, ",") {
			if o = strings.TrimSpace(o); o != "" {
				cfg.AllowedOrigins = append(cfg.AllowedOrigins, o)
			}
		}
	}

	return cfg, nil
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
