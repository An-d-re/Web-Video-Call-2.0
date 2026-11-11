# M1 实施计划：基础设施改善 + 登录与数据库

> 本文是**开工前的方案**，确认后再动代码。
> 上位文档见 [`project-report.md`](project-report.md)（项目方案）与两份详细设计。

| 项 | 内容 |
|---|---|
| 里程碑 | M1（原计划的"单机健壮性"扩展为"健壮性 + 用户体系"） |
| 预估工作量 | **6~8 个工作日**（分 4 批，每批可独立验证） |
| 本次目标 | 现有代码工程化改善 + 注册/登录/会话 + 数据库落地，并在阿里云上测试通过 |
| 暂不涉及 | SFU、多人、Redis、分片锁、跨节点集群（留给 M2/M3） |

---

## 一、范围

### 1.1 本次做（In Scope）

| 类别 | 内容 |
|---|---|
| **认证** | 注册、登录、刷新令牌、登出、`/me` |
| **数据库** | MySQL 建库建表、迁移脚本、索引、连接池 |
| **信令改造** | WS 首消息鉴权、未认证拒绝、鉴权超时断开、`auth.required` 开关 |
| **前端** | 新增登录页、令牌管理、WS 鉴权握手、显示名、401 处理 |
| **工程化** | 配置外置、线程数按核数、健康检查与指标、优雅下线、限流、背压、日志轮转 |
| **测试** | Go 单元测试 + API 冒烟脚本 + E2E 验收清单 |

### 1.2 本次不做（Out of Scope，明确写清避免范围膨胀）

| 不做 | 原因 |
|---|---|
| Redis | 刷新令牌先存 MySQL 表，少一个进程、少一份 OOM 风险；Redis 留给 M2 的 presence |
| 房间表 / 通话记录 | 房间仍是易失的内存状态；通话记录等有多人再补 |
| 分片锁 / `conn→roomId` 索引 | 属于性能优化，**必须配压测才讲得清收益**，留给 M2 |
| SFU / 多人 / 直播 | 下一个里程碑 |
| 前端模块化（Vite/ESM） | 本次只新增 `login.html` + 一个 `js/auth.js`，不重构现有通话页 |
| 手机号/邮箱验证、找回密码 | 需要短信/邮件服务，与核心目标无关 |
| OAuth 第三方登录 | 加分项，可后续加 |

---

## 二、架构变更

### 2.1 变更前

```
浏览器 ──HTTP/WSS──▶ nginx ──▶ signaling(C++/muduo) ──▶ 房间（内存）
                                     │
                                     └──▶ coturn（媒体中继）
```

### 2.2 变更后

```
                    ┌──────────────── nginx (443) ────────────────┐
                    │  /            → static/                      │
                    │  /api/*       → auth 服务  127.0.0.1:8080    │
                    │  /ws          → signaling 127.0.0.1:8888     │
                    └───────┬──────────────────────┬───────────────┘
                            │                      │
                 ┌──────────▼─────────┐   ┌────────▼──────────────┐
                 │  **auth 服务(新)** │   │  signaling (C++)      │
                 │  Go · REST · 8080  │   │  · 只做 JWT 验签       │
                 │  · 注册/登录/刷新   │   │  · 不碰数据库          │
                 │  · bcrypt 哈希      │   │  · 保持无状态          │
                 │  · JWT 签发         │   │  · 房间仍在内存        │
                 └──────────┬─────────┘   └───────────────────────┘
                            │
                 ┌──────────▼─────────┐
                 │ PostgreSQL 16       │  users / auth_tokens
                 │ (127.0.0.1:5432)    │
                 └────────────────────┘
```

**关键设计：只有 auth 服务连数据库，信令服务只验签。**

理由（也是面试点）：

1. muduo 生态**没有成熟的异步 MySQL 客户端**。在 `onMessage` 的 IO 线程里做同步 SQL 会阻塞整个 Reactor，所有连接一起卡。
2. 信令服务保持**纯无状态**，M3 拆集群时不需要为它解决数据库连接池问题。
3. 登录接口是唯一会被暴力破解的入口，独立进程便于单独限流、单独扩容。

### 2.3 令牌链路

```
注册/登录 ──▶ accessToken (JWT, 2h, 前端内存)
          └─▶ refreshToken (随机串, 7d, httpOnly Cookie + 服务端存 sha256)

页面加载 ──POST /api/refresh（带 Cookie）──▶ 新的 accessToken
WS 连接  ──首消息 {"type":"auth","token":accessToken}──▶ 服务端验签
```

- **accessToken 只存内存**，不落 localStorage → XSS 拿不到长效凭证。
- **refreshToken 存 `httpOnly` + `SameSite=Strict` Cookie** → JS 读不到；`SameSite=Strict` 顺带挡掉大部分 CSRF。
- 刷新时**轮换**：旧 refreshToken 立即失效，新的重新下发（可检测重放）。

---

## 三、技术选型

| 项 | 选型 | 理由 | 备选 |
|---|---|---|---|
| auth 服务语言 | **Go** | 单静态二进制、无运行时依赖、部署即 `scp`；`net/http` + `database/sql` 标准库够用；内存约 20 MB（2 GB 机器友好）；国内后端岗位认可度高 | Node.js（熟 JS 就选它，内存 ~60 MB）；Python/FastAPI（需 venv） |
| 数据库 | **PostgreSQL 16**（已确认） | 常驻内存约 250 MB（比 MySQL 8 省出约 150 MB）；事务与索引能力更强、`EXPLAIN` 输出更清晰；SQL 八股普遍通用 | MySQL 8.0（国内考点更多，但约 400 MB）；MariaDB |
| 密码哈希 | **bcrypt，cost=12** | Go 标准生态 `golang.org/x/crypto/bcrypt`；自带 salt、可调 cost；**绝不用 MD5/SHA256** | Argon2id（更现代，但参数要调） |
| 令牌 | **JWT HS256（用标准库自实现，不引第三方库）** | 无状态，信令节点与未来 SFU 都能独立验签；自己实现便于和 C++ 侧做交叉验证 | PASETO / opaque token + 内省接口 |
| 数据库驱动 | **`jackc/pgx/v5` + `pgxpool`** | Go + PostgreSQL 的事实标准，性能优于 `database/sql` 通用层，连接池开箱即用 | `lib/pq`（已进入维护模式，不推荐） |
| HTTP 路由 | 标准库 `net/http`（Go 1.22+ 的 `ServeMux` 支持方法与路径参数） | 不引框架，代码量小、可控 | chi / gin |
| 迁移 | **编号 SQL 文件 + 极简 runner**（`db/migrations/001_init.sql`） | 不引 golang-migrate；但要能按顺序执行、记录已执行版本 | golang-migrate |

> **不引入 Redis**：刷新令牌存 MySQL 表（见 §4.2）。少一个进程，少一个 OOM 风险点。

---

## 四、数据库设计

### 4.1 `users`

```sql
CREATE TABLE IF NOT EXISTS users (
  id            BIGINT       GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  username      VARCHAR(32)  NOT NULL,
  display_name  VARCHAR(32)  NOT NULL,
  password_hash VARCHAR(100) NOT NULL,          -- bcrypt 输出 60 字符，留余量便于换 Argon2id
  created_at    TIMESTAMPTZ  NOT NULL DEFAULT now(),
  last_login_at TIMESTAMPTZ  NULL,
  disabled      BOOLEAN      NOT NULL DEFAULT FALSE,
  CONSTRAINT ck_username_format CHECK (username ~ '^[a-z0-9_]{3,32}$')
);

CREATE UNIQUE INDEX IF NOT EXISTS uk_users_username   ON users (username);
CREATE INDEX        IF NOT EXISTS idx_users_created_at ON users (created_at);
```

设计要点：

- **库编码 UTF-8**：PostgreSQL 默认即可，中文展示名与 emoji 都支持（不需要 MySQL 的 `utf8mb4` 那一套）。
- **`uk_users_username` 唯一索引**：既做唯一约束，又让登录查询走 `O(log n)`。这条可以说"我为什么把索引建在这里"。
- **`ck_username_format` 数据库层校验**：应用层校验之外再兜一层，说明"校验要放在离数据最近的地方"。
- `password_hash VARCHAR(100)`：bcrypt 固定 60 字符，留余量便于将来换 Argon2id。
- `disabled` 便于演示"踢人/封禁"。

### 4.2 `auth_tokens`（刷新令牌）

```sql
CREATE TABLE IF NOT EXISTS auth_tokens (
  id         BIGINT      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
  user_id    BIGINT      NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash CHAR(64)    NOT NULL,              -- sha256(refreshToken) 十六进制，不存明文
  expires_at TIMESTAMPTZ NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  revoked    BOOLEAN     NOT NULL DEFAULT FALSE
);

CREATE UNIQUE INDEX IF NOT EXISTS uk_auth_tokens_hash
  ON auth_tokens (token_hash);
CREATE INDEX IF NOT EXISTS idx_auth_tokens_user_expires
  ON auth_tokens (user_id, expires_at);
```

设计要点：

- **只存 `sha256(token)`**：数据库泄露也拿不到可用令牌（和密码不存明文同一个道理）。
- **`uk_token_hash`**：刷新时按 hash 精确查找，`O(log n)`。
- **`idx_user_expires`**：便于"踢掉某用户所有会话"和清理过期记录。
- 外键 + `ON DELETE CASCADE`：删用户自动清会话。

### 4.3 PostgreSQL 调参（2 GB 机器必须做）

```ini
# /etc/postgresql/16/main/conf.d/zz-small.conf
shared_buffers        = 128MB   # 默认 128MB，保持
effective_cache_size  = 768MB   # 告诉优化器可用文件缓存（不实际占用）
work_mem              = 4MB     # 单次排序/哈希上限，本项目查询很简单
maintenance_work_mem  = 64MB
max_connections       = 50      # auth 服务连接池 10 就够
```

**注意**：`shared_buffers` 是**真正占用**的内存，`effective_cache_size` 只是给优化器的估计值、不占内存。
很多人把这两个搞混，以为都要按物理内存分配——这是面试常问的坑。

---

## 五、API 设计

统一前缀 `/api`，全部 JSON。认证用 `Authorization: Bearer <accessToken>`。

| 方法 | 路径 | 请求 | 响应 | 主要错误 |
|---|---|---|---|---|
| POST | `/api/register` | `{username, password, displayName}` | `201 {user, accessToken}` + Set-Cookie | `username_taken` 409 / `weak_password` 422 / `invalid_username` 422 |
| POST | `/api/login` | `{username, password}` | `200 {user, accessToken}` + Set-Cookie | `invalid_credentials` 401 / `rate_limited` 429 |
| POST | `/api/refresh` | Cookie | `200 {accessToken}` + 轮换后的 Cookie | `invalid_refresh` 401 |
| POST | `/api/logout` | Cookie | `204` | — |
| GET | `/api/me` | Bearer | `200 {user}` | `invalid_token` 401 |
| GET | `/api/health` | — | `200 {status:"ok", db:"ok"}` | `503`（DB 不通） |

**安全细节（面试会问）：**

| 细节 | 做法 | 为什么 |
|---|---|---|
| 用户枚举 | 登录失败统一返回 `invalid_credentials`，**不区分"用户不存在"和"密码错误"** | 否则可探测已注册用户 |
| 时序攻击 | 用户不存在时**也执行一次 bcrypt 校验**（对固定假哈希） | 否则响应时间差会泄露用户是否存在 |
| 暴力破解 | 按 `IP` 和按 `username` 双维度令牌桶：`5 次/分钟/用户名`、`20 次/分钟/IP` | 登录是唯一公网可写入口 |
| SQL 注入 | 全部使用**参数化查询**（pgx 的 `$1` 占位符：`pool.QueryRow(ctx, "… WHERE username = $1", u)`） | 拼接 SQL 是本项目最容易被攻击的地方 |
| 密码强度 | 长度 ≥ 8，且不是纯数字 | `weak_password` |
| 用户名规范 | `^[a-z0-9_]{3,32}$`，入库前统一小写 | 避免 `Alice`/`alice` 两个账号 |
| Cookie | `HttpOnly; Secure; SameSite=Strict; Path=/api` | `Secure` 只在 HTTPS 下发送 |

### JWT 载荷

```json
{ "sub": 42, "username": "alice", "typ": "access",
  "iat": 1731300000, "exp": 1731307200, "jti": "随机" }
```

- HS256，密钥 `JWT_SECRET`（≥32 字节随机串），**只存在于服务端环境变量**，auth 与 signaling 共享。
- 有效期 **2 小时**；时钟偏移容忍 60 秒（`exp` 比较时减 60s 余量，防服务器时间漂移误判）。

---

## 六、信令协议变更

### 6.1 新增

| 方向 | type | 载荷 | 说明 |
|---|---|---|---|
| C→S | **`auth`** | `{token}` | **连接后必须发的第一条消息** |
| S→C | **`auth_ok`** | `{userId, username, displayName}` | 鉴权成功 |
| S→C | `error` | `{code:"unauthorized"}` | 未鉴权就发 `join_room` |

### 6.2 规则

1. 建立 WS 连接后 **5 秒内**必须发 `auth`，否则 `error{code:"auth_timeout"}` + 关闭连接。
   （防止有人只挂连接不认证，白白占资源——这是上一轮讨论的"未认证连接限流"的具体实现。）
2. `auth` 之前或之后收到 `join_room` 且未认证 → `error{code:"unauthorized"}` + 关闭。
3. 认证失败（签名错 / 过期 / 载荷不合法）→ `error{code:"invalid_token"}` + 关闭。
4. `joined` 增加 `self` 字段与对端信息，前端可显示真实名字：

```json
{ "type": "joined", "roomId": "ABC234", "peers": 2,
  "self": { "userId": 42, "displayName": "阿伟" } }
```

5. `start_call` 增加 `peer` 字段：

```json
{ "type": "start_call", "initiator": true,
  "peer": { "userId": 7, "displayName": "小红" },
  "iceServers": [ ... ] }
```

6. **配置开关** `auth.required`（默认 `true`）：
   置 `false` 时退回"匿名可入房"的旧行为。**这是本次改动的回滚阀门**——
   线上出问题只要改配置重启，不需要回滚代码。

### 6.3 C++ 侧 JWT 验签（自己实现，约 80 行）

```
1. 按 '.' 切三段：header.payload.signature（base64url）
2. 解 header，校验 alg == HS256（拒绝 none）
3. 用 OpenSSL HMAC-SHA256 对 "header.payload" 计算签名，与第三段做**定长时间比较**
4. 解 payload，jsoncpp 解析：校验 exp（带 60s 余量）、sub
5. 返回 userId / username / displayName
```

**验证方式（关键）**：用 Go 侧签发的真实 token 做测试向量，交叉验证 C++ 能正确验签、
篡改 payload 后能被拒绝、过期 token 被拒绝。手写 JWT 是本次最容易出错的地方，必须单测。

---

## 七、现有代码改善清单

按"为什么现在做"排序，前 7 条是本次必做。

| # | 改善项 | 现状 | 改成 | 为什么现在做 |
|---|---|---|---|---|
| 1 | 配置外置 | 端口/线程数/静态目录/日志级别全硬编码 | `server.conf` + 环境变量覆盖（`PORT`、`THREADS`、`JWT_SECRET`、`AUTH_REQUIRED`） | 要加 JWT 密钥，不能写死在源码 |
| 2 | 线程数按核数 | `setThreadNum(4)` 写死 | 默认 `hardware_concurrency()`，可配置 | **你的服务器只有 2 核**，4 个 IO 线程纯属增加上下文切换 |
| 3 | WS 首消息鉴权 | 无 | `auth` + 5 秒超时 | 本次核心功能 |
| 4 | 未认证连接限制 | 无 | 5 秒内未认证即断开；可选每 IP 连接数上限 | 认证入口的攻击面 |
| 5 | `/healthz` `/readyz` `/metrics` | 只有日志 | 复用现有 HTTP 分支加三个路由 | 部署后要能验证"服务真的好了"，也是 M2 压测的前提 |
| 6 | SIGTERM 优雅下线 | 无信号处理，`kill` 即断连 | draining → 拒绝新入房 → `Close(1001)` → 退出 | 加了 auth 后必须能不停机更新 |
| 7 | 背压水位线 | `conn->send()` 无上限 | muduo `setHighWaterMarkCallback`：1 MB 降级（丢 ICE 候选），4 MB 断开 | 认证前也可能被灌垃圾数据 |
| 8 | 限流 | 无 | 每连接 200 msg/s；每 IP 建连 20 次/分钟 | 登录 + WS 是两个公网入口 |
| 9 | WS 握手健壮性 | `find("Upgrade: websocket")` 大小写敏感、不校验版本、握手跨分段会失败 | 逐行解析头部、大小写不敏感、校验 `Sec-WebSocket-Version: 13` | 真实缺陷，改动小 |
| 10 | 日志轮转 | `AsyncLogging` 100 MB 滚动但从不删除；coturn 开了 `verbose` | 配 logrotate；部署脚本去掉 `verbose` | 加了 auth 日志后磁盘涨得更快，你已用 18.7/40 GB |

**明确推到 M2**（理由：没有压测就没有说服力）

- 房间表 64 分片锁
- `conn→roomId` 索引替换线性扫描

---

## 八、分 4 批实施

### 批次 A：基础设施改造（0.5~1 天）

| 步骤 | 产出 |
|---|---|
| A1 | `server.conf` + 环境变量读取（含 `JWT_SECRET`、`AUTH_REQUIRED`） |
| A2 | 线程数按核数；日志轮转配置 `deploy/logrotate.conf` |
| A3 | `/healthz`（存活）、`/readyz`（就绪）、`/metrics`（Prometheus 文本） |
| A4 | SIGTERM 优雅下线 |
| A5 | 背压水位线 + 每连接/每 IP 限流 |
| A6 | WS 握手逐行解析 + 大小写不敏感 + 版本校验 |

**验收**：`curl /healthz` 返回 ok；`curl /metrics` 有连接数/房间数；`kill -TERM` 后日志显示 draining 并正常退出。

### 批次 B：auth 服务 + 数据库（2~3 天）

| 步骤 | 产出 |
|---|---|
| B1 | `db/migrations/001_init.sql` + `scripts/migrate.sh` |
| B2 | Go 模块骨架、配置、pgxpool 连接池（`MaxConns=10`） |
| B3 | `POST /api/register`（bcrypt + 参数化 SQL + 用户名规范） |
| B4 | `POST /api/login`（统一错误 + 假哈希抵消时序 + 限流） |
| B5 | `POST /api/refresh`（轮换 + sha256 存储）/ `POST /api/logout` / `GET /api/me` |
| B6 | 单元测试：密码哈希、JWT 签发/过期/篡改、令牌轮换 |
| B7 | `scripts/api-smoke.sh`：一串 curl 覆盖正常与异常路径 |

**验收**：`go test ./...` 全绿；冒烟脚本全通过；数据库里 `password_hash` 是 bcrypt 串（`$2a$12$...`）。

### 批次 C：信令与前端接入（2 天）

| 步骤 | 产出 |
|---|---|
| C1 | C++ JWT 验签（HMAC-SHA256 + base64url + jsoncpp）与单测向量 |
| C2 | `auth` 消息处理、5 秒超时、`auth_ok`、`unauthorized` 拒绝 |
| C3 | `joined` / `start_call` 补充用户信息 |
| C4 | `static/login.html`：注册/登录切换、表单校验、错误提示、成功后跳转 |
| C5 | `static/js/auth.js`：令牌存储（内存 + refresh 兜底）、`apiFetch` 封装、401 自动刷新重试 |
| C6 | `static/index.html`：启动校验登录、WS 首消息 `auth`、顶栏显示用户名 + 退出、对端显示真实姓名 |
| C7 | `nginx.conf` 增加 `location /api/` 反代；本地开发 CORS 说明 |

**验收**：未登录访问通话页自动跳登录页；登录后两人通话，双方显示对方真实姓名。

### 批次 D：测试与加固（1~1.5 天）

| 步骤 | 产出 |
|---|---|
| D1 | 完整验收清单逐项过（见 §9） |
| D2 | 安全用例：SQL 注入、越权、篡改 token、过期 token、暴力破解限流 |
| D3 | 回归测试：挂断后重新入房、断网重连、半开连接回收、重复入房幂等 |
| D4 | 部署文档更新（`README.md`）+ 阿里云实操部署 + 排障记录 |
| D5 | 提交 + 打 tag（`v1.0` 锚点 → `v1.1-auth`） |

---

## 九、验收清单

### 功能

- [ ] 注册成功；重复用户名返回 `username_taken`
- [ ] 弱密码被拒（`weak_password`）；非法用户名被拒
- [ ] 登录成功返回 accessToken + refreshToken Cookie
- [ ] 密码错误 / 用户不存在，**返回完全相同的错误信息与相近耗时**
- [ ] 刷新令牌可用且**轮换**（旧 token 再用即失败）
- [ ] 登出后刷新令牌失效
- [ ] `/api/me` 无 token / 坏 token / 过期 token 均返回 401
- [ ] 未认证建立 WS，5 秒后被断开
- [ ] 坏 token 连 WS 被拒；好 token 可正常入房
- [ ] 两人通话，双方显示对方真实姓名
- [ ] 退出登录后回到登录页

### 安全

- [ ] 用户名输入 `' OR 1=1 --` 无法登入任何账号
- [ ] 数据库 `password_hash` 是 `$2a$12$...`（非明文）
- [ ] `auth_tokens.token_hash` 是 64 位十六进制（非明文）
- [ ] 篡改 JWT payload（改 `sub`）→ 验签失败
- [ ] 连续错误登录 6 次 → 触发 `rate_limited`
- [ ] Cookie 带 `HttpOnly` / `Secure` / `SameSite=Strict`

### 回归（不能破坏已有行为）

- [ ] 挂断后重新加入同一房间，第二次挂断对端仍能收到 `bye`
- [ ] 断网重连后自动恢复通话
- [ ] 半开连接 60 秒内被回收
- [ ] 双击"加入房间"不产生重复成员
- [ ] 摄像头/麦克风三态语义不变（未开启 / 已关闭 / 开启）

### 运维

- [ ] `/healthz` 200；`/readyz` 在 DB 断开时返回 503
- [ ] `/metrics` 有连接数、房间数、鉴权成功/失败计数
- [ ] `kill -TERM` 优雅退出，不产生错误日志
- [ ] 内存占用：`free -m` 显示可用 ≥ 300 MB
- [ ] 重启后 `systemctl status` 两个服务均 active

---

## 十、测试方案

| 层级 | 工具 | 覆盖 |
|---|---|---|
| 单元测试 | `go test` | 密码哈希、JWT 签发/过期/篡改、令牌轮换、用户名规范校验 |
| 接口冒烟 | `scripts/api-smoke.sh`（curl） | 6 个接口的正常路径 + 主要异常路径 |
| 安全用例 | curl 手工/脚本 | SQL 注入、越权、重放、暴力破解 |
| 端到端 | 两个浏览器 + 双账号 | 注册 → 登录 → 建/入房 → 通话 → 挂断 → 重入 |
| 回归 | 手工 | §9 回归清单 |
| 性能基线 | 暂不做 | 留给 M2 配压测一起做 |

> **交叉验证要点**：C++ 端手写 JWT 验签最容易出错，必须用 Go 签发的真实 token 做向量测试，
> 并在 CI 之外保留一组"固定 token 测试用例"（签名正确 / 篡改 / 过期 / `alg=none`）。

---

## 十一、风险与回滚

| 风险 | 应对 |
|---|---|
| **强制登录破坏现有流程** | 配置开关 `auth.required=false` 可退回匿名模式（改配置重启即可，不必回滚代码） |
| PostgreSQL 在 2 GB 机器上 OOM | 应用 §4.3 调参 + 开 2 GB swap；上线后 `free -m` 复核 |
| C++ 手写 JWT 验签错误 | Go 生成测试向量做交叉验证；单测覆盖篡改/过期/`alg=none` |
| 刷新令牌轮换引发并发刷新失败 | 前端刷新请求串行化（单飞 Promise，复用现有重连思路） |
| 时间漂移导致 token 误判 | exp 比较留 60 秒余量；服务器开 NTP |
| 改坏现有通话功能 | **先打 `v1.0` tag 锚点**；在新分支 `feat/auth` 上开工；每批结束跑回归清单 |

**回滚方式（三档）**

1. 只关功能：`auth.required=false` + 重启信令服务（秒级）。
2. 回滚代码：`git checkout v1.0` → 重新编译 → 重启。
3. 回滚数据库：本次只有新增表，不动旧数据；`DROP TABLE auth_tokens, users` 即可（旧版本不依赖它们）。

---

## 十二、需要你先拍板的 3 件事

| # | 决策 | 我的建议 | 备选 |
|---|---|---|---|
| 1 | **auth 服务用什么语言** | ✅ **已确认：Go** | Node.js / Python |
| 2 | **数据库用哪个** | ✅ **已确认：PostgreSQL 16** | MySQL 8.0 / MariaDB |
| 3 | **前端登录页怎么放** | **新增 `static/login.html`**，通话页保持不动，只加登录校验 | 塞进现有单文件（改动面大、易回归） |

第 3 项我按建议直接做，除非你反对。前两项请确认。
