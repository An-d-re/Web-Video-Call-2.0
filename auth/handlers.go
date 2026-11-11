package main

import (
	"context"
	"errors"
	"log"
	"net/http"
	"regexp"
	"strings"
	"time"
	"unicode/utf8"
)

var usernameRe = regexp.MustCompile(`^[a-z0-9_]{3,32}$`)

// normalizeUsername 统一小写并校验格式。
// 不统一大小写的话，"Alice" 和 "alice" 会变成两个账号，用户自己都会懵。
func normalizeUsername(raw string) (string, bool) {
	u := strings.ToLower(strings.TrimSpace(raw))
	if !usernameRe.MatchString(u) {
		return "", false
	}
	return u, true
}

// ---------- 注册 ----------

func (s *Server) handleRegister(w http.ResponseWriter, r *http.Request) {
	if !s.cfg.AllowRegistration {
		writeErr(w, http.StatusForbidden, "registration_disabled", "当前未开放注册")
		return
	}
	if !s.registerIP.Allow(s.clientIP(r)) {
		writeErr(w, http.StatusTooManyRequests, "rate_limited", "注册过于频繁，请稍后再试")
		return
	}

	var req struct {
		Username    string `json:"username"`
		Password    string `json:"password"`
		DisplayName string `json:"displayName"`
	}
	if err := decodeJSON(w, r, &req, 4096); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid_request", "请求体不是合法 JSON")
		return
	}

	username, ok := normalizeUsername(req.Username)
	if !ok {
		writeErr(w, http.StatusUnprocessableEntity, "invalid_username",
			"用户名需为 3~32 位小写字母、数字或下划线")
		return
	}
	if err := validatePassword(req.Password); err != nil {
		writeErr(w, http.StatusUnprocessableEntity, "weak_password", err.Error())
		return
	}

	displayName := strings.TrimSpace(req.DisplayName)
	if displayName == "" {
		displayName = username
	}
	if utf8.RuneCountInString(displayName) > 32 {
		writeErr(w, http.StatusUnprocessableEntity, "invalid_display_name", "展示名最多 32 个字符")
		return
	}

	hash, err := hashPassword(req.Password)
	if err != nil {
		log.Printf("register: bcrypt 失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}

	user, err := s.store.CreateUser(r.Context(), username, displayName, hash)
	if err != nil {
		if isUniqueViolation(err) {
			writeErr(w, http.StatusConflict, "username_taken", "用户名已被占用")
			return
		}
		log.Printf("register: 写库失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}

	s.issueSession(w, r, user, http.StatusCreated)
}

// ---------- 登录 ----------

func (s *Server) handleLogin(w http.ResponseWriter, r *http.Request) {
	ip := s.clientIP(r)
	if !s.loginIP.Allow(ip) {
		writeErr(w, http.StatusTooManyRequests, "rate_limited", "登录尝试过于频繁，请稍后再试")
		return
	}

	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if err := decodeJSON(w, r, &req, 4096); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid_request", "请求体不是合法 JSON")
		return
	}

	// 用户名格式非法时也走"等量耗时 + 统一错误"，避免泄露格式信息
	username, ok := normalizeUsername(req.Username)
	if !ok {
		verifyAgainstDummy(req.Password)
		writeErr(w, http.StatusUnauthorized, "invalid_credentials", "用户名或密码错误")
		return
	}

	if !s.loginByUser.Allow("u:" + username) {
		writeErr(w, http.StatusTooManyRequests, "rate_limited", "该账号登录尝试过于频繁，请稍后再试")
		return
	}

	user, hash, err := s.store.GetUserByUsername(r.Context(), username)
	if err != nil {
		if errors.Is(err, ErrNotFound) {
			// 关键：用户不存在时也要算一次 bcrypt，让耗时与"密码错误"接近，
			// 否则攻击者用响应时间就能枚举出哪些用户名已注册。
			verifyAgainstDummy(req.Password)
			writeErr(w, http.StatusUnauthorized, "invalid_credentials", "用户名或密码错误")
			return
		}
		log.Printf("login: 查询失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}

	if !verifyPassword(hash, req.Password) {
		writeErr(w, http.StatusUnauthorized, "invalid_credentials", "用户名或密码错误")
		return
	}
	if user.Disabled {
		writeErr(w, http.StatusForbidden, "account_disabled", "账号已被禁用")
		return
	}

	if err := s.store.TouchLastLogin(r.Context(), user.ID); err != nil {
		// 非关键路径，失败只记日志，不影响登录
		log.Printf("login: 更新 last_login_at 失败: %v", err)
	} else {
		now := time.Now().UTC()
		user.LastLoginAt = &now
	}

	s.issueSession(w, r, user, http.StatusOK)
}

// ---------- 刷新 ----------

func (s *Server) handleRefresh(w http.ResponseWriter, r *http.Request) {
	c, err := r.Cookie(refreshCookieName)
	if err != nil || c.Value == "" {
		writeErr(w, http.StatusUnauthorized, "invalid_refresh", "缺少刷新令牌")
		return
	}

	// 原子消费：校验有效性的同时立即作废（单条 UPDATE ... RETURNING）
	userID, err := s.store.ConsumeAuthToken(r.Context(), hashRefreshToken(c.Value))
	if err != nil {
		if errors.Is(err, ErrNotFound) {
			s.clearRefreshCookie(w)
			writeErr(w, http.StatusUnauthorized, "invalid_refresh", "刷新令牌无效或已过期")
			return
		}
		log.Printf("refresh: 消费令牌失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}

	user, err := s.store.GetUserByID(r.Context(), userID)
	if err != nil || user.Disabled {
		s.clearRefreshCookie(w)
		writeErr(w, http.StatusUnauthorized, "invalid_refresh", "刷新令牌无效或已过期")
		return
	}

	// 轮换：上面已作废旧令牌，这里下发新的（旧令牌再次使用会失败，可检测重放）
	s.issueSession(w, r, user, http.StatusOK)
}

// ---------- 登出 ----------

func (s *Server) handleLogout(w http.ResponseWriter, r *http.Request) {
	if c, err := r.Cookie(refreshCookieName); err == nil && c.Value != "" {
		if err := s.store.RevokeAuthToken(r.Context(), hashRefreshToken(c.Value)); err != nil {
			log.Printf("logout: 作废令牌失败: %v", err)
		}
	}
	s.clearRefreshCookie(w)
	w.WriteHeader(http.StatusNoContent)
}

// ---------- 当前用户 ----------

func (s *Server) handleMe(w http.ResponseWriter, r *http.Request) {
	claims, err := s.bearerClaims(r)
	if err != nil {
		writeErr(w, http.StatusUnauthorized, "invalid_token", "凭证无效或已过期")
		return
	}
	user, err := s.store.GetUserByID(r.Context(), claims.Sub)
	if err != nil {
		writeErr(w, http.StatusUnauthorized, "invalid_token", "凭证无效或已过期")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"user": user})
}

// ---------- 健康检查 ----------

func (s *Server) handleHealth(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()

	dbOK := true
	if err := s.pool.Ping(ctx); err != nil {
		dbOK = false
		log.Printf("health: 数据库不可达: %v", err)
	}

	status := http.StatusOK
	if !dbOK {
		status = http.StatusServiceUnavailable
	}
	writeJSON(w, status, map[string]any{
		"status":        map[bool]string{true: "ok", false: "degraded"}[dbOK],
		"db":            map[bool]string{true: "ok", false: "down"}[dbOK],
		"uptimeSeconds": int(time.Since(s.startedAt).Seconds()),
	})
}

// ---------- 会话签发 ----------

// issueSession 签发 access token 与刷新令牌，并写入 Cookie。
func (s *Server) issueSession(w http.ResponseWriter, r *http.Request, user *User, status int) {
	now := time.Now()

	access, err := signAccessToken(s.cfg.JWTSecret, user.ID, user.Username, user.DisplayName, s.cfg.AccessTokenTTL, now)
	if err != nil {
		log.Printf("issueSession: 签发 access token 失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}

	refresh, err := randomToken(32)
	if err != nil {
		log.Printf("issueSession: 生成 refresh token 失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}
	if err := s.store.CreateAuthToken(r.Context(), user.ID, hashRefreshToken(refresh),
		now.Add(s.cfg.RefreshTokenTTL)); err != nil {
		log.Printf("issueSession: 保存 refresh token 失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}

	s.setRefreshCookie(w, refresh)
	writeJSON(w, status, map[string]any{
		"user":        user,
		"accessToken": access,
		// 前端据此决定何时主动刷新，避免靠猜
		"expiresIn": int(s.cfg.AccessTokenTTL.Seconds()),
	})
}
