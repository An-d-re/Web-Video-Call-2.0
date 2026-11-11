package main

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/url"
	"os"
	"testing"
	"time"
)

// TestMain 初始化时序均衡哈希（仅一次，约 300ms）
func TestMain(m *testing.M) {
	if err := initDummyHash(); err != nil {
		panic(err)
	}
	os.Exit(m.Run())
}

// newTestServer 连真实数据库。未设置 TEST_DATABASE_URL 时跳过，
// 这样纯单元测试不会因为缺数据库而失败。
func newTestServer(t *testing.T) (*Server, func()) {
	t.Helper()

	dsn := os.Getenv("TEST_DATABASE_URL")
	if dsn == "" {
		t.Skip("未设置 TEST_DATABASE_URL，跳过集成测试")
	}

	ctx := context.Background()
	pool, err := NewPool(ctx, dsn)
	if err != nil {
		t.Fatalf("连接数据库失败: %v", err)
	}
	if _, err := pool.Exec(ctx, "TRUNCATE auth_tokens, users RESTART IDENTITY CASCADE"); err != nil {
		pool.Close()
		t.Fatalf("清空表失败: %v", err)
	}

	cfg := &Config{
		ListenAddr:        "127.0.0.1:0",
		JWTSecret:         testSecret,
		AccessTokenTTL:    time.Hour,
		RefreshTokenTTL:   time.Hour,
		CookieSecure:      false, // httptest 走 http，带 Secure 的 Cookie 不会被回传
		AllowRegistration: true,
		TrustProxy:        false,
	}
	return NewServer(cfg, pool), func() { pool.Close() }
}

func postJSON(t *testing.T, c *http.Client, url string, body any) (int, map[string]any) {
	t.Helper()
	raw, err := json.Marshal(body)
	if err != nil {
		t.Fatalf("序列化请求失败: %v", err)
	}
	resp, err := c.Post(url, "application/json", bytes.NewReader(raw))
	if err != nil {
		t.Fatalf("请求失败: %v", err)
	}
	defer resp.Body.Close()

	var out map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&out)
	return resp.StatusCode, out
}

func errCode(out map[string]any) string {
	e, ok := out["error"].(map[string]any)
	if !ok {
		return ""
	}
	s, _ := e["code"].(string)
	return s
}

func TestAuthFlow(t *testing.T) {
	srv, cleanup := newTestServer(t)
	defer cleanup()

	ts := httptest.NewServer(srv.Handler())
	defer ts.Close()

	jar, _ := cookiejar.New(nil)
	client := &http.Client{Jar: jar}
	base := ts.URL

	// ---------- 1. 注册成功 ----------
	code, out := postJSON(t, client, base+"/api/register", map[string]string{
		"username": "Alice_01", "password": "s3cret-pass", "displayName": "阿伟",
	})
	if code != http.StatusCreated {
		t.Fatalf("注册应返回 201，实际 %d，响应: %v", code, out)
	}
	user, _ := out["user"].(map[string]any)
	if user["username"] != "alice_01" {
		t.Fatalf("用户名应被规范化为小写，实际 %v", user["username"])
	}
	if user["displayName"] != "阿伟" {
		t.Fatalf("展示名不符: %v", user["displayName"])
	}
	if _, leaked := user["passwordHash"]; leaked {
		t.Fatal("响应不应包含密码哈希")
	}
	access, _ := out["accessToken"].(string)
	if access == "" {
		t.Fatal("注册应返回 accessToken")
	}

	// ---------- 2. 重复用户名 → 409 ----------
	code, out = postJSON(t, client, base+"/api/register", map[string]string{
		"username": "alice_01", "password": "another-pass",
	})
	if code != http.StatusConflict || errCode(out) != "username_taken" {
		t.Fatalf("重复用户名应 409/username_taken，实际 %d/%s", code, errCode(out))
	}

	// ---------- 3. 弱密码 → 422 ----------
	code, out = postJSON(t, client, base+"/api/register", map[string]string{
		"username": "weakuser", "password": "12345678",
	})
	if code != http.StatusUnprocessableEntity || errCode(out) != "weak_password" {
		t.Fatalf("纯数字密码应 422/weak_password，实际 %d/%s", code, errCode(out))
	}

	// ---------- 4. 密码错误 → 401（不区分用户是否存在）----------
	code, out = postJSON(t, client, base+"/api/login", map[string]string{
		"username": "alice_01", "password": "wrong-pass",
	})
	if code != http.StatusUnauthorized || errCode(out) != "invalid_credentials" {
		t.Fatalf("错误密码应 401/invalid_credentials，实际 %d/%s", code, errCode(out))
	}

	// ---------- 5. 用户不存在 → 同样的 401 与同样的错误码 ----------
	code, out = postJSON(t, client, base+"/api/login", map[string]string{
		"username": "nobody_here", "password": "whatever1",
	})
	if code != http.StatusUnauthorized || errCode(out) != "invalid_credentials" {
		t.Fatalf("不存在用户应 401/invalid_credentials，实际 %d/%s", code, errCode(out))
	}

	// ---------- 6. 正常登录 ----------
	code, out = postJSON(t, client, base+"/api/login", map[string]string{
		"username": "alice_01", "password": "s3cret-pass",
	})
	if code != http.StatusOK {
		t.Fatalf("登录应 200，实际 %d，响应: %v", code, out)
	}
	access, _ = out["accessToken"].(string)
	if access == "" {
		t.Fatal("登录应返回 accessToken")
	}

	// ---------- 7. /api/me ----------
	req, _ := http.NewRequest(http.MethodGet, base+"/api/me", nil)
	req.Header.Set("Authorization", "Bearer "+access)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("请求 /api/me 失败: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("/api/me 应 200，实际 %d", resp.StatusCode)
	}

	// ---------- 8. 无 token → 401 ----------
	req, _ = http.NewRequest(http.MethodGet, base+"/api/me", nil)
	resp, err = client.Do(req)
	if err != nil {
		t.Fatalf("请求 /api/me 失败: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("无 token 应 401，实际 %d", resp.StatusCode)
	}

	// ---------- 9. 刷新令牌轮换 ----------
	// 刷新 Cookie 的 Path 是 /api，所以必须用 /api 下的 URL 去取（否则拿不到，这本身也是验证点）
	apiURL, _ := url.Parse(base + "/api/refresh")
	oldCookies := jar.Cookies(apiURL)
	var oldRefresh *http.Cookie
	for _, c := range oldCookies {
		if c.Name == refreshCookieName {
			oldRefresh = c
		}
	}
	if oldRefresh == nil {
		t.Fatal("登录后应写入刷新令牌 Cookie")
	}

	code, out = postJSON(t, client, base+"/api/refresh", map[string]string{})
	if code != http.StatusOK {
		t.Fatalf("刷新应 200，实际 %d，响应: %v", code, out)
	}
	if newAccess, _ := out["accessToken"].(string); newAccess == "" {
		t.Fatal("刷新应返回新的 accessToken")
	}

	// ---------- 10. 旧刷新令牌重放 → 401（轮换生效）----------
	req, _ = http.NewRequest(http.MethodPost, base+"/api/refresh", bytes.NewReader([]byte("{}")))
	req.Header.Set("Content-Type", "application/json")
	req.AddCookie(&http.Cookie{Name: oldRefresh.Name, Value: oldRefresh.Value, Path: "/api"})
	resp, err = client.Do(req)
	if err != nil {
		t.Fatalf("重放请求失败: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusUnauthorized {
		t.Fatalf("旧刷新令牌重放应 401，实际 %d", resp.StatusCode)
	}

	// ---------- 11. 登出 → 之后不能再刷新 ----------
	req, _ = http.NewRequest(http.MethodPost, base+"/api/logout", nil)
	resp, err = client.Do(req)
	if err != nil {
		t.Fatalf("登出请求失败: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusNoContent {
		t.Fatalf("登出应 204，实际 %d", resp.StatusCode)
	}

	code, _ = postJSON(t, client, base+"/api/refresh", map[string]string{})
	if code != http.StatusUnauthorized {
		t.Fatalf("登出后刷新应 401，实际 %d", code)
	}
}

func TestSQLInjectionInLogin(t *testing.T) {
	srv, cleanup := newTestServer(t)
	defer cleanup()

	ts := httptest.NewServer(srv.Handler())
	defer ts.Close()
	client := &http.Client{}

	// 先建一个正常用户
	if code, out := postJSON(t, client, ts.URL+"/api/register", map[string]string{
		"username": "victim", "password": "s3cret-pass",
	}); code != http.StatusCreated {
		t.Fatalf("准备用户失败: %d %v", code, out)
	}

	// 经典注入尝试：必须失败
	for _, payload := range []string{
		"' OR 1=1 --",
		"victim'--",
		"victim' OR '1'='1",
	} {
		code, out := postJSON(t, client, ts.URL+"/api/login", map[string]string{
			"username": payload, "password": "whatever1",
		})
		if code == http.StatusOK {
			t.Fatalf("注入 payload %q 竟然登录成功", payload)
		}
		if code != http.StatusUnauthorized && code != http.StatusUnprocessableEntity {
			t.Fatalf("注入 payload %q 返回异常状态 %d: %v", payload, code, out)
		}
	}
}

func TestHealthEndpoint(t *testing.T) {
	srv, cleanup := newTestServer(t)
	defer cleanup()

	ts := httptest.NewServer(srv.Handler())
	defer ts.Close()

	resp, err := http.Get(ts.URL + "/api/health")
	if err != nil {
		t.Fatalf("健康检查失败: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("健康检查应 200，实际 %d", resp.StatusCode)
	}
}
