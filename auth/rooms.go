package main

// 房间相关的 HTTP 接口：建房 / 加入 / 列表 / 我的房间 / 解散。
//
// 关键设计：加入房间时，auth 服务校验"房间存在 + 密码正确"，
// 然后签发一个短时效的 roomToken。信令服务只验签、取出 roomId，
// 不查数据库也不校验密码——这让 C++ 侧保持无状态。

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"errors"
	"log"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

const (
	roomTokenTTL   = 2 * time.Minute // 进房通行证有效期
	roomTitleMax   = 50
	maxPwdFails    = 5
	pwdFailWindow  = time.Minute
	roomIDAttempts = 5
)

var (
	// 去掉了容易混淆的 I / O / 0 / 1
	roomIDChars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
	roomPwdRe   = regexp.MustCompile(`^[A-Za-z0-9]{4,8}$`)
)

// generateRoomID 生成 6 位房间号。256 % 32 == 0，取模无偏。
func generateRoomID() (string, error) {
	b := make([]byte, 6)
	if _, err := rand.Read(b); err != nil {
		return "", err
	}
	out := make([]byte, 6)
	for i := 0; i < 6; i++ {
		out[i] = roomIDChars[int(b[i])%len(roomIDChars)]
	}
	return string(out), nil
}

// hashRoomPassword 生成随机盐并返回 sha256(salt + 明文口令) 的十六进制。
// 说明：房间口令是短口令（4~8 位），sha256 抗离线爆破能力有限；
// 它的定位是"防误入/防随手进"，不是账号级安全。账号密码用的是 bcrypt。
func hashRoomPassword(pw string) (hash string, salt string, err error) {
	sb := make([]byte, 16)
	if _, err = rand.Read(sb); err != nil {
		return "", "", err
	}
	salt = hex.EncodeToString(sb)
	sum := sha256.Sum256([]byte(salt + pw))
	return hex.EncodeToString(sum[:]), salt, nil
}

// verifyRoomPassword 定长时间比较，避免时序侧信道。
func verifyRoomPassword(hash, salt, pw string) bool {
	sum := sha256.Sum256([]byte(salt + pw))
	got := hex.EncodeToString(sum[:])
	return subtle.ConstantTimeCompare([]byte(got), []byte(hash)) == 1
}

// ---------------- 房间口令失败计数（防爆破） ----------------

type pwdFailEntry struct {
	count   int
	resetAt time.Time
}

type pwdFailCounter struct {
	mu sync.Mutex
	m  map[string]*pwdFailEntry
}

func newPwdFailCounter() *pwdFailCounter {
	return &pwdFailCounter{m: make(map[string]*pwdFailEntry)}
}

func (f *pwdFailCounter) blocked(key string) bool {
	f.mu.Lock()
	defer f.mu.Unlock()
	e, ok := f.m[key]
	if !ok {
		return false
	}
	if time.Now().After(e.resetAt) {
		delete(f.m, key)
		return false
	}
	return e.count >= maxPwdFails
}

func (f *pwdFailCounter) fail(key string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	now := time.Now()
	e, ok := f.m[key]
	if !ok || now.After(e.resetAt) {
		f.m[key] = &pwdFailEntry{count: 1, resetAt: now.Add(pwdFailWindow)}
		return
	}
	e.count++
}

func (f *pwdFailCounter) reset(key string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	delete(f.m, key)
}

// ---------------- 接口实现 ----------------

type roomResp struct {
	Room      *Room  `json:"room"`
	RoomToken string `json:"roomToken"`
	ExpiresIn int    `json:"expiresIn"`
}

func (s *Server) issueRoomToken(claims *Claims, room *Room) (string, error) {
	return signRoomToken(s.cfg.JWTSecret, claims.Sub, claims.Username, room.ID, roomTokenTTL, time.Now())
}

// POST /api/rooms  {title, password?}
func (s *Server) handleRoomCreate(w http.ResponseWriter, r *http.Request) {
	claims, err := s.bearerClaims(r)
	if err != nil {
		writeErr(w, http.StatusUnauthorized, "invalid_token", "凭证无效或已过期")
		return
	}

	var req struct {
		Title    string `json:"title"`
		Password string `json:"password"`
	}
	if err := decodeJSON(w, r, &req, 4096); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid_request", "请求体不是合法 JSON")
		return
	}

	title := strings.TrimSpace(req.Title)
	if title == "" || utf8.RuneCountInString(title) > roomTitleMax {
		writeErr(w, http.StatusUnprocessableEntity, "invalid_title",
			"房间标题不能为空，最多 50 个字符")
		return
	}

	pw := strings.TrimSpace(req.Password)
	if pw != "" && !roomPwdRe.MatchString(pw) {
		writeErr(w, http.StatusUnprocessableEntity, "invalid_room_password",
			"房间密码需为 4~8 位字母或数字")
		return
	}

	hash, salt := "", ""
	if pw != "" {
		hash, salt, err = hashRoomPassword(pw)
		if err != nil {
			log.Printf("room create: 生成密码哈希失败: %v", err)
			writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
			return
		}
	}

	// 房间号由服务端生成；撞号（极小概率）则换一个重试
	var room *Room
	for i := 0; i < roomIDAttempts; i++ {
		id, gerr := generateRoomID()
		if gerr != nil {
			log.Printf("room create: 生成房间号失败: %v", gerr)
			break
		}
		room, err = s.store.CreateRoom(r.Context(), id, title, claims.Sub, hash, salt)
		if err == nil {
			break
		}
		if !isUniqueViolation(err) {
			log.Printf("room create: 写库失败: %v", err)
			room = nil
			break
		}
		log.Printf("room create: 房间号 %s 撞号，重试", id)
	}
	if room == nil {
		writeErr(w, http.StatusInternalServerError, "internal", "房间创建失败，请重试")
		return
	}

	room.OwnerName = claims.Name
	if room.OwnerName == "" {
		room.OwnerName = claims.Username
	}
	room.HasPassword = (pw != "")

	token, err := s.issueRoomToken(claims, room)
	if err != nil {
		log.Printf("room create: 签发 roomToken 失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}

	log.Printf("房间 %s 已创建（%s），加密=%v", room.ID, title, room.HasPassword)
	writeJSON(w, http.StatusCreated, roomResp{
		Room:      room,
		RoomToken: token,
		ExpiresIn: int(roomTokenTTL.Seconds()),
	})
}

// POST /api/rooms/{id}/join  {password?}
func (s *Server) handleRoomJoin(w http.ResponseWriter, r *http.Request) {
	claims, err := s.bearerClaims(r)
	if err != nil {
		writeErr(w, http.StatusUnauthorized, "invalid_token", "凭证无效或已过期")
		return
	}

	roomID := strings.ToUpper(strings.TrimSpace(r.PathValue("id")))
	if roomID == "" {
		writeErr(w, http.StatusBadRequest, "invalid_room_id", "缺少房间号")
		return
	}

	var req struct {
		Password string `json:"password"`
	}
	if err := decodeJSON(w, r, &req, 4096); err != nil {
		writeErr(w, http.StatusBadRequest, "invalid_request", "请求体不是合法 JSON")
		return
	}

	// 按用户限流：60 秒内错 5 次就锁一段时间
	failKey := strconv.FormatInt(claims.Sub, 10)
	if s.roomPwdFails.blocked(failKey) {
		writeErr(w, http.StatusTooManyRequests, "too_many_attempts",
			"密码尝试次数过多，请稍后再试")
		return
	}

	room, hash, salt, err := s.store.RoomForJoin(r.Context(), roomID)
	if errors.Is(err, ErrNotFound) {
		writeErr(w, http.StatusNotFound, "room_not_found", "房间不存在")
		return
	}
	if err != nil {
		log.Printf("room join: 查询房间失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}

	if room.HasPassword {
		if !verifyRoomPassword(hash, salt, strings.TrimSpace(req.Password)) {
			s.roomPwdFails.fail(failKey)
			log.Printf("用户 %d 加入房间 %s 密码错误", claims.Sub, roomID)
			writeErr(w, http.StatusForbidden, "bad_password", "房间密码错误")
			return
		}
	}
	s.roomPwdFails.reset(failKey)

	if err := s.store.UpsertRoomMember(r.Context(), roomID, claims.Sub); err != nil {
		log.Printf("room join: 记录成员失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}
	if err := s.store.TouchRoom(r.Context(), roomID); err != nil {
		// 非关键路径
		log.Printf("room join: 更新活跃时间失败: %v", err)
	}
	room.MemberCount++

	token, err := s.issueRoomToken(claims, room)
	if err != nil {
		log.Printf("room join: 签发 roomToken 失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}

	writeJSON(w, http.StatusOK, roomResp{
		Room:      room,
		RoomToken: token,
		ExpiresIn: int(roomTokenTTL.Seconds()),
	})
}

// GET /api/rooms
func (s *Server) handleRoomList(w http.ResponseWriter, r *http.Request) {
	if _, err := s.bearerClaims(r); err != nil {
		writeErr(w, http.StatusUnauthorized, "invalid_token", "凭证无效或已过期")
		return
	}
	rooms, err := s.store.ListRooms(r.Context())
	if err != nil {
		log.Printf("room list: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"rooms": rooms})
}

// GET /api/rooms/mine
func (s *Server) handleRoomMine(w http.ResponseWriter, r *http.Request) {
	claims, err := s.bearerClaims(r)
	if err != nil {
		writeErr(w, http.StatusUnauthorized, "invalid_token", "凭证无效或已过期")
		return
	}
	rooms, err := s.store.ListRoomsForUser(r.Context(), claims.Sub)
	if err != nil {
		log.Printf("room mine: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"rooms": rooms})
}

// DELETE /api/rooms/{id}  仅创建者
func (s *Server) handleRoomDelete(w http.ResponseWriter, r *http.Request) {
	claims, err := s.bearerClaims(r)
	if err != nil {
		writeErr(w, http.StatusUnauthorized, "invalid_token", "凭证无效或已过期")
		return
	}
	roomID := strings.ToUpper(strings.TrimSpace(r.PathValue("id")))
	if roomID == "" {
		writeErr(w, http.StatusBadRequest, "invalid_room_id", "缺少房间号")
		return
	}

	ownerID, err := s.store.RoomOwner(r.Context(), roomID)
	if errors.Is(err, ErrNotFound) {
		writeErr(w, http.StatusNotFound, "room_not_found", "房间不存在")
		return
	}
	if err != nil {
		log.Printf("room delete: 查询失败: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}
	if ownerID != claims.Sub {
		writeErr(w, http.StatusForbidden, "not_owner", "只有创建者可以解散房间")
		return
	}

	if err := s.store.DeleteRoom(r.Context(), roomID); err != nil {
		log.Printf("room delete: %v", err)
		writeErr(w, http.StatusInternalServerError, "internal", "服务器内部错误")
		return
	}
	log.Printf("房间 %s 已被创建者 %d 解散", roomID, claims.Sub)
	w.WriteHeader(http.StatusNoContent)
}
