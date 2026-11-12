package main

// 房间接口的集成测试（走真实 PostgreSQL）。

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// apiReq 发一个带 Bearer 的 JSON 请求，返回状态码与解析后的响应。
func apiReq(t *testing.T, method, url, token string, body any) (int, map[string]any) {
	t.Helper()

	var reader *bytes.Reader
	if body != nil {
		raw, err := json.Marshal(body)
		if err != nil {
			t.Fatalf("序列化请求失败: %v", err)
		}
		reader = bytes.NewReader(raw)
	} else {
		reader = bytes.NewReader(nil)
	}

	req, err := http.NewRequest(method, url, reader)
	if err != nil {
		t.Fatalf("构造请求失败: %v", err)
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatalf("请求失败: %v", err)
	}
	defer resp.Body.Close()

	var out map[string]any
	_ = json.NewDecoder(resp.Body).Decode(&out)
	return resp.StatusCode, out
}

// registerAndLogin 注册一个用户并返回 accessToken。
func registerAndLogin(t *testing.T, base, username string) string {
	t.Helper()
	code, out := postJSON(t, http.DefaultClient, base+"/api/register", map[string]string{
		"username": username, "password": "s3cret-pass", "displayName": username,
	})
	if code != http.StatusCreated {
		t.Fatalf("注册 %s 失败: %d %v", username, code, out)
	}
	tok, _ := out["accessToken"].(string)
	if tok == "" {
		t.Fatalf("注册 %s 未返回 accessToken", username)
	}
	return tok
}

func roomOf(t *testing.T, out map[string]any) map[string]any {
	t.Helper()
	r, ok := out["room"].(map[string]any)
	if !ok {
		t.Fatalf("响应里没有 room: %v", out)
	}
	return r
}

func TestRoomLifecycle(t *testing.T) {
	srv, cleanup := newTestServer(t)
	defer cleanup()

	ts := httptest.NewServer(srv.Handler())
	defer ts.Close()
	base := ts.URL

	tokA := registerAndLogin(t, base, "alice_room")
	tokB := registerAndLogin(t, base, "bob_room")

	// ---------- 1. 创建无密码房间 ----------
	code, out := apiReq(t, http.MethodPost, base+"/api/rooms", tokA,
		map[string]string{"title": "阿伟的会议室"})
	if code != http.StatusCreated {
		t.Fatalf("建房应 201，实际 %d %v", code, out)
	}
	roomA := roomOf(t, out)
	roomAID, _ := roomA["id"].(string)
	if len(roomAID) != 6 {
		t.Fatalf("房间号应为 6 位，实际 %q", roomAID)
	}
	if roomA["hasPassword"] != false {
		t.Fatalf("无密码房间 hasPassword 应为 false: %v", roomA["hasPassword"])
	}
	if roomA["title"] != "阿伟的会议室" {
		t.Fatalf("标题不符: %v", roomA["title"])
	}
	tokenA, _ := out["roomToken"].(string)
	if tokenA == "" {
		t.Fatal("建房应返回 roomToken")
	}

	// roomToken 必须能被"按 room 类型验签"通过，但按 access 类型校验要失败
	claims, err := verifyRoomToken(testSecret, tokenA, time.Now())
	if err != nil {
		t.Fatalf("roomToken 验签失败: %v", err)
	}
	if claims.Room != roomAID {
		t.Fatalf("roomToken 的 room 不符: %q", claims.Room)
	}
	if _, err := verifyAccessToken(testSecret, tokenA, time.Now()); err == nil {
		t.Fatal("roomToken 不应能当 access token 用")
	}

	// ---------- 2. 创建带密码房间 ----------
	code, out = apiReq(t, http.MethodPost, base+"/api/rooms", tokA,
		map[string]string{"title": "周三组会", "password": "ab12"})
	if code != http.StatusCreated {
		t.Fatalf("建加密房应 201，实际 %d %v", code, out)
	}
	roomBID, _ := roomOf(t, out)["id"].(string)
	if roomOf(t, out)["hasPassword"] != true {
		t.Fatal("加密房 hasPassword 应为 true")
	}

	// ---------- 3. 密码格式非法 ----------
	code, out = apiReq(t, http.MethodPost, base+"/api/rooms", tokA,
		map[string]string{"title": "短密码", "password": "ab"})
	if code != http.StatusUnprocessableEntity {
		t.Fatalf("密码过短应 422，实际 %d %v", code, out)
	}

	// ---------- 4. 标题超长 ----------
	long := ""
	for i := 0; i < 51; i++ {
		long += "字"
	}
	code, _ = apiReq(t, http.MethodPost, base+"/api/rooms", tokA, map[string]string{"title": long})
	if code != http.StatusUnprocessableEntity {
		t.Fatalf("标题超长应 422，实际 %d", code)
	}

	// ---------- 5. 加入无密码房间 ----------
	code, out = apiReq(t, http.MethodPost, base+"/api/rooms/"+roomAID+"/join", tokB,
		map[string]string{})
	if code != http.StatusOK {
		t.Fatalf("加入无密码房间应 200，实际 %d %v", code, out)
	}
	if tok, _ := out["roomToken"].(string); tok == "" {
		t.Fatal("加入应返回 roomToken")
	}

	// ---------- 6. 加密房：密码错 / 密码对 ----------
	code, out = apiReq(t, http.MethodPost, base+"/api/rooms/"+roomBID+"/join", tokB,
		map[string]string{"password": "wrong1"})
	if code != http.StatusForbidden || errCode(out) != "bad_password" {
		t.Fatalf("密码错误应 403/bad_password，实际 %d/%s", code, errCode(out))
	}

	code, out = apiReq(t, http.MethodPost, base+"/api/rooms/"+roomBID+"/join", tokB,
		map[string]string{"password": "ab12"})
	if code != http.StatusOK {
		t.Fatalf("密码正确应 200，实际 %d %v", code, out)
	}

	// ---------- 7. 加入不存在的房间 ----------
	code, out = apiReq(t, http.MethodPost, base+"/api/rooms/ZZZZZZ/join", tokB, map[string]string{})
	if code != http.StatusNotFound || errCode(out) != "room_not_found" {
		t.Fatalf("房间不存在应 404/room_not_found，实际 %d/%s", code, errCode(out))
	}

	// ---------- 8. 列表 ----------
	code, out = apiReq(t, http.MethodGet, base+"/api/rooms", tokB, nil)
	if code != http.StatusOK {
		t.Fatalf("列表应 200，实际 %d", code)
	}
	rooms, _ := out["rooms"].([]any)
	if len(rooms) != 2 {
		t.Fatalf("列表应有 2 个房间，实际 %d", len(rooms))
	}

	// ---------- 9. 我的房间（B 加入过两个）----------
	code, out = apiReq(t, http.MethodGet, base+"/api/rooms/mine", tokB, nil)
	if code != http.StatusOK {
		t.Fatalf("我的房间应 200，实际 %d", code)
	}
	mine, _ := out["rooms"].([]any)
	if len(mine) != 2 {
		t.Fatalf("B 应加入过 2 个房间，实际 %d", len(mine))
	}

	// 新建一个用户，他的"我的房间"应该是空的
	tokC := registerAndLogin(t, base, "carol_room")
	code, out = apiReq(t, http.MethodGet, base+"/api/rooms/mine", tokC, nil)
	if code != http.StatusOK {
		t.Fatalf("我的房间应 200，实际 %d", code)
	}
	if mine, _ := out["rooms"].([]any); len(mine) != 0 {
		t.Fatalf("新用户不应有历史房间，实际 %d", len(mine))
	}

	// ---------- 10. 非创建者不能解散 ----------
	code, out = apiReq(t, http.MethodDelete, base+"/api/rooms/"+roomAID, tokB, nil)
	if code != http.StatusForbidden || errCode(out) != "not_owner" {
		t.Fatalf("非创建者解散应 403/not_owner，实际 %d/%s", code, errCode(out))
	}

	// ---------- 11. 创建者解散 ----------
	code, _ = apiReq(t, http.MethodDelete, base+"/api/rooms/"+roomAID, tokA, nil)
	if code != http.StatusNoContent {
		t.Fatalf("创建者解散应 204，实际 %d", code)
	}

	code, out = apiReq(t, http.MethodPost, base+"/api/rooms/"+roomAID+"/join", tokB, map[string]string{})
	if code != http.StatusNotFound {
		t.Fatalf("解散后加入应 404，实际 %d %v", code, out)
	}

	// ---------- 12. 未登录访问 ----------
	code, _ = apiReq(t, http.MethodGet, base+"/api/rooms", "", nil)
	if code != http.StatusUnauthorized {
		t.Fatalf("未登录列表应 401，实际 %d", code)
	}
}

// 房间口令连续错误的限流
func TestRoomPasswordRateLimit(t *testing.T) {
	srv, cleanup := newTestServer(t)
	defer cleanup()

	ts := httptest.NewServer(srv.Handler())
	defer ts.Close()
	base := ts.URL

	tokA := registerAndLogin(t, base, "alice_pwd")
	tokB := registerAndLogin(t, base, "bob_pwd")

	_, out := apiReq(t, http.MethodPost, base+"/api/rooms", tokA,
		map[string]string{"title": "限流测试", "password": "ab12"})
	roomID, _ := roomOf(t, out)["id"].(string)

	// 前 5 次是 403，第 6 次开始 429
	for i := 1; i <= 5; i++ {
		code, _ := apiReq(t, http.MethodPost, base+"/api/rooms/"+roomID+"/join", tokB,
			map[string]string{"password": "wrong1"})
		if code != http.StatusForbidden {
			t.Fatalf("第 %d 次错误密码应 403，实际 %d", i, code)
		}
	}
	code, out := apiReq(t, http.MethodPost, base+"/api/rooms/"+roomID+"/join", tokB,
		map[string]string{"password": "wrong1"})
	if code != http.StatusTooManyRequests || errCode(out) != "too_many_attempts" {
		t.Fatalf("第 6 次应 429/too_many_attempts，实际 %d/%s", code, errCode(out))
	}
}

// 建房时"房间号由服务端生成"必须稳定：连续建 10 个房间号都不重复
func TestGenerateRoomIDUnique(t *testing.T) {
	seen := make(map[string]bool, 10)
	for i := 0; i < 10; i++ {
		id, err := generateRoomID()
		if err != nil {
			t.Fatalf("生成房间号失败: %v", err)
		}
		if len(id) != 6 {
			t.Fatalf("房间号长度应为 6，实际 %q", id)
		}
		for _, c := range id {
			if !strings.ContainsRune(roomIDChars, c) {
				t.Fatalf("房间号含非法字符 %q", c)
			}
		}
		if seen[id] {
			t.Fatalf("房间号重复: %s", id)
		}
		seen[id] = true
	}
}
