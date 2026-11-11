package main

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

// ErrNotFound 表示查询对象不存在，调用方据此决定对外返回什么（通常统一成"凭证错误"）。
var ErrNotFound = errors.New("记录不存在")

// User 是对外可见的用户信息。密码哈希与封禁位不参与序列化。
type User struct {
	ID          int64      `json:"id"`
	Username    string     `json:"username"`
	DisplayName string     `json:"displayName"`
	CreatedAt   time.Time  `json:"createdAt"`
	LastLoginAt *time.Time `json:"lastLoginAt"`
	Disabled    bool       `json:"-"`
}

// Store 封装所有 SQL。全部使用参数化查询（$1/$2），不拼字符串——
// 这是本项目抵御 SQL 注入的第一道也是唯一一道防线。
type Store struct {
	pool *pgxpool.Pool
}

func NewStore(pool *pgxpool.Pool) *Store {
	return &Store{pool: pool}
}

// NewPool 创建连接池并做一次连通性检查。
func NewPool(ctx context.Context, dsn string) (*pgxpool.Pool, error) {
	cfg, err := pgxpool.ParseConfig(dsn)
	if err != nil {
		return nil, fmt.Errorf("解析 DATABASE_URL 失败: %w", err)
	}
	// 2 核小机器 + 极低 QPS：10 个连接绰绰有余
	cfg.MaxConns = 10
	cfg.MinConns = 1
	cfg.MaxConnIdleTime = 5 * time.Minute
	cfg.MaxConnLifetime = time.Hour

	pool, err := pgxpool.NewWithConfig(ctx, cfg)
	if err != nil {
		return nil, fmt.Errorf("创建连接池失败: %w", err)
	}

	pingCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	if err := pool.Ping(pingCtx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("连接 PostgreSQL 失败: %w", err)
	}
	return pool, nil
}

// CreateUser 插入用户；用户名冲突时返回 *pgconn.PgError(23505)。
func (s *Store) CreateUser(ctx context.Context, username, displayName, passwordHash string) (*User, error) {
	u := &User{Username: username, DisplayName: displayName}
	err := s.pool.QueryRow(ctx,
		`INSERT INTO users (username, display_name, password_hash)
		 VALUES ($1, $2, $3)
		 RETURNING id, created_at`,
		username, displayName, passwordHash,
	).Scan(&u.ID, &u.CreatedAt)
	if err != nil {
		return nil, err
	}
	return u, nil
}

// GetUserByUsername 返回用户与其密码哈希。
func (s *Store) GetUserByUsername(ctx context.Context, username string) (*User, string, error) {
	u := &User{}
	var hash string
	err := s.pool.QueryRow(ctx,
		`SELECT id, username, display_name, password_hash, created_at, last_login_at, disabled
		   FROM users WHERE username = $1`,
		username,
	).Scan(&u.ID, &u.Username, &u.DisplayName, &hash, &u.CreatedAt, &u.LastLoginAt, &u.Disabled)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, "", ErrNotFound
	}
	if err != nil {
		return nil, "", err
	}
	return u, hash, nil
}

// GetUserByID 返回用户信息（不含哈希）。
func (s *Store) GetUserByID(ctx context.Context, id int64) (*User, error) {
	u := &User{}
	err := s.pool.QueryRow(ctx,
		`SELECT id, username, display_name, created_at, last_login_at, disabled
		   FROM users WHERE id = $1`,
		id,
	).Scan(&u.ID, &u.Username, &u.DisplayName, &u.CreatedAt, &u.LastLoginAt, &u.Disabled)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	return u, nil
}

func (s *Store) TouchLastLogin(ctx context.Context, id int64) error {
	_, err := s.pool.Exec(ctx, `UPDATE users SET last_login_at = now() WHERE id = $1`, id)
	return err
}

// CreateAuthToken 保存刷新令牌的 sha256 摘要。
func (s *Store) CreateAuthToken(ctx context.Context, userID int64, tokenHash string, expiresAt time.Time) error {
	_, err := s.pool.Exec(ctx,
		`INSERT INTO auth_tokens (user_id, token_hash, expires_at) VALUES ($1, $2, $3)`,
		userID, tokenHash, expiresAt)
	return err
}

// ConsumeAuthToken 原子地"消费"一个刷新令牌：校验有效性并立即作废。
// 单条 UPDATE ... RETURNING 即可完成，不需要显式事务，也不会出现并发双重消费。
func (s *Store) ConsumeAuthToken(ctx context.Context, tokenHash string) (int64, error) {
	var userID int64
	err := s.pool.QueryRow(ctx,
		`UPDATE auth_tokens
		    SET revoked = TRUE
		  WHERE token_hash = $1 AND revoked = FALSE AND expires_at > now()
		RETURNING user_id`,
		tokenHash,
	).Scan(&userID)
	if errors.Is(err, pgx.ErrNoRows) {
		return 0, ErrNotFound
	}
	if err != nil {
		return 0, err
	}
	return userID, nil
}

func (s *Store) RevokeAuthToken(ctx context.Context, tokenHash string) error {
	_, err := s.pool.Exec(ctx,
		`UPDATE auth_tokens SET revoked = TRUE WHERE token_hash = $1 AND revoked = FALSE`,
		tokenHash)
	return err
}

// PurgeExpiredTokens 清理早已过期的记录，避免表无限增长。
func (s *Store) PurgeExpiredTokens(ctx context.Context) (int64, error) {
	tag, err := s.pool.Exec(ctx,
		`DELETE FROM auth_tokens WHERE expires_at < now() - interval '7 days'`)
	if err != nil {
		return 0, err
	}
	return tag.RowsAffected(), nil
}

// isUniqueViolation 判断是否违反唯一约束（PostgreSQL SQLSTATE 23505）。
func isUniqueViolation(err error) bool {
	var pgErr *pgconn.PgError
	return errors.As(err, &pgErr) && pgErr.Code == "23505"
}
