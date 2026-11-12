package main

import (
	"encoding/json"
	"errors"
	"log"
	"net"
	"net/http"
	"runtime/debug"
	"strings"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

const refreshCookieName = "rt"

var errNoToken = errors.New("缺少或非法的 Authorization 头")

// Server 持有所有依赖，方便测试时按需替换。
type Server struct {
	cfg          *Config
	pool         *pgxpool.Pool
	store        *Store
	loginIP      *Limiter // 按来源 IP 限制登录尝试
	loginByUser  *Limiter // 按用户名限制登录尝试（防单账号被爆破）
	registerIP   *Limiter
	roomPwdFails *pwdFailCounter // 房间口令失败计数（防爆破）
	startedAt    time.Time
}

func NewServer(cfg *Config, pool *pgxpool.Pool) *Server {
	return &Server{
		cfg:          cfg,
		pool:         pool,
		store:        NewStore(pool),
		loginIP:      NewLimiter(20, 20), // 每分钟 20 次，允许 20 的突发
		loginByUser:  NewLimiter(5, 5),   // 单账号每分钟 5 次
		registerIP:   NewLimiter(5, 3),   // 单 IP 每分钟 5 次注册，突发 3
		roomPwdFails: newPwdFailCounter(),
		startedAt:    time.Now(),
	}
}

// routes 注册全部路由。Go 1.22+ 的 ServeMux 支持「方法 + 路径」模式。
func (s *Server) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("POST /api/register", s.handleRegister)
	mux.HandleFunc("POST /api/login", s.handleLogin)
	mux.HandleFunc("POST /api/refresh", s.handleRefresh)
	mux.HandleFunc("POST /api/logout", s.handleLogout)
	mux.HandleFunc("GET /api/me", s.handleMe)
	mux.HandleFunc("GET /api/health", s.handleHealth)

	// 房间（权威数据在 PostgreSQL，信令服务只认 roomToken）
	mux.HandleFunc("POST /api/rooms", s.handleRoomCreate)
	mux.HandleFunc("GET /api/rooms", s.handleRoomList)
	mux.HandleFunc("GET /api/rooms/mine", s.handleRoomMine)
	mux.HandleFunc("POST /api/rooms/{id}/join", s.handleRoomJoin)
	mux.HandleFunc("DELETE /api/rooms/{id}", s.handleRoomDelete)

	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		writeErr(w, http.StatusNotFound, "not_found", "接口不存在")
	})
	return mux
}

// Handler 串起中间件：CORS → 崩溃保护 → 访问日志 → 路由。
func (s *Server) Handler() http.Handler {
	return s.withCORS(s.withRecover(s.withAccessLog(s.routes())))
}

// ---------- 响应辅助 ----------

type apiError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	// 响应含令牌，禁止任何缓存
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	if v == nil {
		return
	}
	if err := json.NewEncoder(w).Encode(v); err != nil {
		log.Printf("写响应失败: %v", err)
	}
}

func writeErr(w http.ResponseWriter, status int, code, message string) {
	writeJSON(w, status, map[string]any{
		"error": apiError{Code: code, Message: message},
	})
}

func decodeJSON(w http.ResponseWriter, r *http.Request, dst any, maxBytes int64) error {
	r.Body = http.MaxBytesReader(w, r.Body, maxBytes)
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return err
	}
	if dec.More() {
		return errors.New("请求体包含多余内容")
	}
	return nil
}

// ---------- 请求辅助 ----------

// clientIP 取真实客户端 IP。
// 服务只监听 127.0.0.1，请求必然来自 nginx，所以信任 X-Real-IP 是安全的；
// 若将来直接暴露公网，必须把 TRUST_PROXY 设为 false，否则限流会被伪造头绕过。
func (s *Server) clientIP(r *http.Request) string {
	if s.cfg.TrustProxy {
		if v := strings.TrimSpace(r.Header.Get("X-Real-IP")); v != "" {
			return v
		}
		if v := r.Header.Get("X-Forwarded-For"); v != "" {
			if i := strings.IndexByte(v, ','); i > 0 {
				return strings.TrimSpace(v[:i])
			}
			return strings.TrimSpace(v)
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func (s *Server) bearerClaims(r *http.Request) (*Claims, error) {
	h := r.Header.Get("Authorization")
	const prefix = "Bearer "
	if len(h) <= len(prefix) || !strings.EqualFold(h[:len(prefix)], prefix) {
		return nil, errNoToken
	}
	return verifyAccessToken(s.cfg.JWTSecret, strings.TrimSpace(h[len(prefix):]), time.Now())
}

func (s *Server) setRefreshCookie(w http.ResponseWriter, token string) {
	http.SetCookie(w, &http.Cookie{
		Name:     refreshCookieName,
		Value:    token,
		Path:     "/api", // 只在 /api/* 下发送，静态资源与 WS 拿不到
		HttpOnly: true,   // JS 读不到，XSS 也偷不走
		Secure:   s.cfg.CookieSecure,
		SameSite: http.SameSiteStrictMode, // 顺带挡掉大部分 CSRF
		MaxAge:   int(s.cfg.RefreshTokenTTL.Seconds()),
	})
}

func (s *Server) clearRefreshCookie(w http.ResponseWriter) {
	http.SetCookie(w, &http.Cookie{
		Name:     refreshCookieName,
		Value:    "",
		Path:     "/api",
		HttpOnly: true,
		Secure:   s.cfg.CookieSecure,
		SameSite: http.SameSiteStrictMode,
		MaxAge:   -1,
	})
}

// ---------- 中间件 ----------

func (s *Server) originAllowed(origin string) bool {
	for _, o := range s.cfg.AllowedOrigins {
		if o == "*" || strings.EqualFold(o, origin) {
			return true
		}
	}
	return false
}

// withCORS 只在配置了 ALLOWED_ORIGINS 时启用跨域（本地前端跑在别的端口时用）。
// 生产环境前后端同源（都经 nginx），无需 CORS。
func (s *Server) withCORS(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		origin := r.Header.Get("Origin")
		if origin != "" && s.originAllowed(origin) {
			w.Header().Set("Access-Control-Allow-Origin", origin)
			w.Header().Set("Access-Control-Allow-Credentials", "true")
			w.Header().Set("Vary", "Origin")
			if r.Method == http.MethodOptions {
				w.Header().Set("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
				w.Header().Set("Access-Control-Allow-Headers", "Authorization, Content-Type")
				w.Header().Set("Access-Control-Max-Age", "600")
				w.WriteHeader(http.StatusNoContent)
				return
			}
		}
		next.ServeHTTP(w, r)
	})
}

func (s *Server) withRecover(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if v := recover(); v != nil {
				log.Printf("panic: %v\n%s", v, debug.Stack())
				writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
			}
		}()
		next.ServeHTTP(w, r)
	})
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (sr *statusRecorder) WriteHeader(code int) {
	sr.status = code
	sr.ResponseWriter.WriteHeader(code)
}

func (s *Server) withAccessLog(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rec := &statusRecorder{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(rec, r)
		// 不记录请求体、不记录令牌；只记录方法、路径、状态、耗时、来源 IP
		log.Printf("%s %s %d %s ip=%s ua=%q",
			r.Method, r.URL.Path, rec.status,
			time.Since(start).Round(time.Millisecond),
			s.clientIP(r), r.UserAgent())
	})
}
