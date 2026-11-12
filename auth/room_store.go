package main

// 房间与成员记录的数据库访问。
// 房间的权威数据（是否存在、标题、密码、创建者）全部在 PostgreSQL 里，
// 信令服务不再保存这些信息。

import (
	"context"
	"errors"
	"time"

	"github.com/jackc/pgx/v5"
)

// Room 是对外可见的房间信息。
type Room struct {
	ID             string     `json:"id"`
	Title          string     `json:"title"`
	OwnerID        int64      `json:"ownerId"`
	OwnerName      string     `json:"ownerName"`
	HasPassword    bool       `json:"hasPassword"`
	MemberCount    int        `json:"memberCount"` // 历史加入过的人数（不是在线人数）
	CreatedAt      time.Time  `json:"createdAt"`
	LastActiveAt   time.Time  `json:"lastActiveAt"`
	MyLastJoinedAt *time.Time `json:"myLastJoinedAt,omitempty"`
}

// CreateRoom 插入房间，并把创建者记为第一个成员。返回数据库回填的时间字段。
func (s *Store) CreateRoom(ctx context.Context, id, title string, ownerID int64,
	passwordHash, salt string) (*Room, error) {

	r := &Room{ID: id, Title: title, OwnerID: ownerID}
	var hashPtr, saltPtr *string
	if passwordHash != "" {
		hashPtr = &passwordHash
		saltPtr = &salt
	}

	err := s.pool.QueryRow(ctx,
		`INSERT INTO rooms (id, title, owner_id, password_hash, salt)
		 VALUES ($1, $2, $3, $4, $5)
		 RETURNING created_at, last_active_at`,
		id, title, ownerID, hashPtr, saltPtr,
	).Scan(&r.CreatedAt, &r.LastActiveAt)
	if err != nil {
		return nil, err
	}

	if err := s.UpsertRoomMember(ctx, id, ownerID); err != nil {
		return nil, err
	}
	r.MemberCount = 1
	return r, nil
}

// RoomForJoin 取出加入房间所需的信息。房间不存在返回 ErrNotFound。
func (s *Store) RoomForJoin(ctx context.Context, id string) (*Room, string, string, error) {
	r := &Room{}
	var hash, salt *string
	err := s.pool.QueryRow(ctx,
		`SELECT r.id, r.title, r.owner_id, u.display_name,
		        r.password_hash, r.salt, r.created_at, r.last_active_at
		   FROM rooms r JOIN users u ON u.id = r.owner_id
		  WHERE r.id = $1`,
		id,
	).Scan(&r.ID, &r.Title, &r.OwnerID, &r.OwnerName, &hash, &salt, &r.CreatedAt, &r.LastActiveAt)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, "", "", ErrNotFound
	}
	if err != nil {
		return nil, "", "", err
	}
	r.HasPassword = (hash != nil)
	h, sl := "", ""
	if hash != nil {
		h = *hash
	}
	if salt != nil {
		sl = *salt
	}
	return r, h, sl, nil
}

// UpsertRoomMember 记录"某人加入过某房间"，这是"我的房间"列表的数据来源。
func (s *Store) UpsertRoomMember(ctx context.Context, roomID string, userID int64) error {
	_, err := s.pool.Exec(ctx,
		`INSERT INTO room_members (room_id, user_id) VALUES ($1, $2)
		 ON CONFLICT (room_id, user_id) DO UPDATE
		   SET last_joined_at = now(),
		       join_count     = room_members.join_count + 1`,
		roomID, userID)
	return err
}

// TouchRoom 更新房间的最近活跃时间（用于列表排序）。
func (s *Store) TouchRoom(ctx context.Context, roomID string) error {
	_, err := s.pool.Exec(ctx, `UPDATE rooms SET last_active_at = now() WHERE id = $1`, roomID)
	return err
}

const roomListLimit = 200

// ListRooms 返回全部房间，按最近活跃排序。
func (s *Store) ListRooms(ctx context.Context) ([]Room, error) {
	rows, err := s.pool.Query(ctx,
		`SELECT r.id, r.title, r.owner_id, u.display_name,
		        (r.password_hash IS NOT NULL) AS has_password,
		        r.created_at, r.last_active_at,
		        (SELECT count(*) FROM room_members m WHERE m.room_id = r.id) AS members
		   FROM rooms r JOIN users u ON u.id = r.owner_id
		  ORDER BY r.last_active_at DESC
		  LIMIT $1`, roomListLimit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := make([]Room, 0, 32)
	for rows.Next() {
		var r Room
		if err := rows.Scan(&r.ID, &r.Title, &r.OwnerID, &r.OwnerName, &r.HasPassword,
			&r.CreatedAt, &r.LastActiveAt, &r.MemberCount); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

// ListRoomsForUser 返回"我加入过的房间"，按我最近一次加入的时间排序。
func (s *Store) ListRoomsForUser(ctx context.Context, userID int64) ([]Room, error) {
	rows, err := s.pool.Query(ctx,
		`SELECT r.id, r.title, r.owner_id, u.display_name,
		        (r.password_hash IS NOT NULL) AS has_password,
		        r.created_at, r.last_active_at, m.last_joined_at,
		        (SELECT count(*) FROM room_members m2 WHERE m2.room_id = r.id) AS members
		   FROM room_members m
		   JOIN rooms r ON r.id = m.room_id
		   JOIN users u ON u.id = r.owner_id
		  WHERE m.user_id = $1
		  ORDER BY m.last_joined_at DESC
		  LIMIT $2`, userID, roomListLimit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	out := make([]Room, 0, 32)
	for rows.Next() {
		var r Room
		if err := rows.Scan(&r.ID, &r.Title, &r.OwnerID, &r.OwnerName, &r.HasPassword,
			&r.CreatedAt, &r.LastActiveAt, &r.MyLastJoinedAt, &r.MemberCount); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

// RoomOwner 返回房间的创建者 id，用于解散时的权限判断。
func (s *Store) RoomOwner(ctx context.Context, roomID string) (int64, error) {
	var ownerID int64
	err := s.pool.QueryRow(ctx, `SELECT owner_id FROM rooms WHERE id = $1`, roomID).Scan(&ownerID)
	if errors.Is(err, pgx.ErrNoRows) {
		return 0, ErrNotFound
	}
	return ownerID, err
}

// DeleteRoom 删除房间（成员记录靠外键级联删除）。
func (s *Store) DeleteRoom(ctx context.Context, roomID string) error {
	_, err := s.pool.Exec(ctx, `DELETE FROM rooms WHERE id = $1`, roomID)
	return err
}
