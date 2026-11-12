-- 002_rooms.sql —— 房间与成员记录（房间持久化）
--
-- 执行：psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -f db/migrations/002_rooms.sql
-- 幂等：可重复执行。

BEGIN;

-- ========== 房间 ==========
-- 房间的权威数据在这里：是否存在、标题、密码、创建者都不再依赖信令服务的内存。
CREATE TABLE IF NOT EXISTS rooms (
  id             VARCHAR(16)  PRIMARY KEY,
  title          VARCHAR(50)  NOT NULL,
  owner_id       BIGINT       NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  -- 房间口令：NULL 表示不需要密码；salt/hash 必须同时有或同时无
  password_hash  CHAR(64)     NULL,
  salt           CHAR(32)     NULL,
  created_at     TIMESTAMPTZ  NOT NULL DEFAULT now(),
  last_active_at TIMESTAMPTZ  NOT NULL DEFAULT now(),
  CONSTRAINT ck_rooms_title_len
    CHECK (char_length(btrim(title)) BETWEEN 1 AND 50),
  CONSTRAINT ck_rooms_password_pair
    CHECK ((password_hash IS NULL) = (salt IS NULL))
);

CREATE INDEX IF NOT EXISTS idx_rooms_last_active
  ON rooms (last_active_at DESC);

CREATE INDEX IF NOT EXISTS idx_rooms_owner
  ON rooms (owner_id);

COMMENT ON COLUMN rooms.password_hash IS 'sha256(salt + 明文口令) 的十六进制；NULL 表示无密码';
COMMENT ON COLUMN rooms.salt          IS '16 字节随机盐的十六进制';

-- ========== 成员记录 ==========
-- 这张表就是"我加入过的房间"：每次加入都会更新 last_joined_at 并累加 join_count。
CREATE TABLE IF NOT EXISTS room_members (
  room_id         VARCHAR(16) NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  user_id         BIGINT      NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  first_joined_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  last_joined_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  join_count      INT         NOT NULL DEFAULT 1,
  PRIMARY KEY (room_id, user_id)
);

-- 支撑"我的房间"按最近加入排序
CREATE INDEX IF NOT EXISTS idx_room_members_user
  ON room_members (user_id, last_joined_at DESC);

INSERT INTO schema_migrations (version) VALUES ('002_rooms')
  ON CONFLICT (version) DO NOTHING;

COMMIT;
