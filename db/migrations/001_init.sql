-- 001_init.sql —— 用户与会话表
--
-- 执行：
--   psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f db/migrations/001_init.sql
-- 幂等：可重复执行，已存在的对象不会重建。

BEGIN;

-- ========== 用户 ==========
CREATE TABLE IF NOT EXISTS users (
  id            BIGINT       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  username      VARCHAR(32)  NOT NULL,
  display_name  VARCHAR(32)  NOT NULL,
  password_hash VARCHAR(100) NOT NULL,
  created_at    TIMESTAMPTZ  NOT NULL DEFAULT now(),
  last_login_at TIMESTAMPTZ  NULL,
  disabled      BOOLEAN      NOT NULL DEFAULT FALSE,
  CONSTRAINT ck_users_username_format
    CHECK (username ~ '^[a-z0-9_]{3,32}$'),
  CONSTRAINT ck_users_display_name_len
    CHECK (char_length(btrim(display_name)) BETWEEN 1 AND 32)
);

-- 唯一索引：既做唯一约束，又让登录查询走 O(log n)
CREATE UNIQUE INDEX IF NOT EXISTS uk_users_username
  ON users (username);

CREATE INDEX IF NOT EXISTS idx_users_created_at
  ON users (created_at);

COMMENT ON COLUMN users.password_hash IS 'bcrypt 输出（60 字符），绝不存明文';
COMMENT ON COLUMN users.display_name  IS '展示名，支持中文 / emoji';

-- ========== 刷新令牌 ==========
-- 只存 sha256 十六进制，不存明文：数据库泄露也拿不到可用令牌
CREATE TABLE IF NOT EXISTS auth_tokens (
  id         BIGINT      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  user_id    BIGINT      NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash CHAR(64)    NOT NULL,
  expires_at TIMESTAMPTZ NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  revoked    BOOLEAN     NOT NULL DEFAULT FALSE
);

CREATE UNIQUE INDEX IF NOT EXISTS uk_auth_tokens_hash
  ON auth_tokens (token_hash);

-- 便于"踢掉某用户全部会话"与清理过期记录
CREATE INDEX IF NOT EXISTS idx_auth_tokens_user_expires
  ON auth_tokens (user_id, expires_at);

CREATE INDEX IF NOT EXISTS idx_auth_tokens_expires_at
  ON auth_tokens (expires_at);

COMMENT ON COLUMN auth_tokens.token_hash IS 'sha256(refreshToken) 的十六进制，不存明文';

-- ========== 迁移版本 ==========
CREATE TABLE IF NOT EXISTS schema_migrations (
  version    VARCHAR(64) PRIMARY KEY,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

INSERT INTO schema_migrations (version) VALUES ('001_init')
  ON CONFLICT (version) DO NOTHING;

COMMIT;
