# WebRTC 一对一视频通话

基于房间的一对一实时音视频通话。信令走 WebSocket，媒体优先 P2P 直连，P2P 打不通时经 TURN 中继。

领域术语（房间、发起方/响应方、设备状态、重协商等）见 [`CONTEXT.md`](CONTEXT.md)。

## 架构

```
                    ┌──────────────────────────────┐
   浏览器 A ────┐   │  nginx (443, TLS)            │
   (HTTPS/WSS)  ├──▶│   /      → static/index.html │
                │   │   /ws    → 127.0.0.1:8888   │
   浏览器 B ────┘   └──────────────┬───────────────┘
                                   │ WebSocket（信令）
                          ┌────────▼─────────────┐
                          │ server.cpp  C++/muduo│  房间管理 + 消息转发
                          │ 端口 8888            │  下发 ICE 配置
                          └──────────────────────┘
   浏览器 A ◀──────── 媒体：P2P 直连 或 TURN(coturn) 中继 ────────▶ 浏览器 B
```

**服务端不碰媒体**，只做两件事：维护「房间 → 最多 2 条连接」的映射，以及在两端之间转发信令。

## 目录结构

| 路径 | 说明 |
|---|---|
| `server.cpp` | 信令服务器（muduo + jsoncpp + OpenSSL）：房间管理、信令转发、JWT 鉴权 |
| `jwt_verify.h` / `jwt_verify.cpp` | JWT（HS256）验签，只依赖 OpenSSL + jsoncpp，不依赖 muduo |
| `tests/jwt_selftest.cpp` | JWT 验签的跨语言自检程序（对照 Go 生成的向量） |
| `tests/jwt_vectors.json` | 由 `auth/vectors_test.go` 生成，勿手改 |
| `tools/dev-gateway/` | 本地开发网关（静态托管 + `/api` 反代 + 假信令），仅本地用 |
| `auth/` | 认证服务（Go + PostgreSQL）：注册/登录/刷新/登出 |
| `db/` | 数据库迁移脚本 |
| `static/index.html` | 前端单页（内联 CSS/JS），含 WebRTC 协商、房间 UI |
| `CMakeLists.txt` | 构建脚本（`server` 与 `jwt_selftest` 两个目标） |
| `turn_config.json` | ICE 服务器配置，**含明文凭据，不入库**（模板见下） |
| `turn_config.example.json` | 上者的模板 |
| `deploy_turn.sh` | coturn 一键部署脚本（需在 Linux 上以 root 运行） |
| `nginx.conf` | 生产环境 HTTPS + WSS 反代、`/api` 反代与静态托管 |
| `CONTEXT.md` | 领域词汇表 |
| `docs/project-report.md` | **主文档**：项目定位、系统架构、功能清单、技术栈、里程碑 |
| `docs/implementation-plan-m1.md` | **M1 实施计划**：基础设施改善 + 登录与数据库（开工方案） |
| `docs/ha-scaling-design.md` | 详细设计：高可用与高并发（分布式化、容量模型、压测与故障演练） |
| `docs/multiparty-sfu-design.md` | 详细设计：多人通话 / 直播（Mesh→SFU、房间调度、扇出控制、直播分层） |

## 依赖

| 依赖 | 版本 | 获取方式 |
|---|---|---|
| C++ 编译器 | 支持 C++17 | `apt install build-essential` |
| CMake | ≥ 3.13 | `apt install cmake` |
| muduo | 2.x | **不在软件源里，必须自行编译**，见下 |
| jsoncpp | ≥ 1.7 | `apt install libjsoncpp-dev` |
| OpenSSL | 1.1 / 3.x | `apt install libssl-dev` |
| coturn | 任意 | 用 `deploy_turn.sh` 部署，或 `apt install coturn` |

### 编译安装 muduo

muduo 依赖它自己的 `muduo_base`/`muduo_net` 两个库，官方只提供源码：

```bash
sudo apt install -y git cmake build-essential
git clone https://github.com/chenshuo/muduo.git
cd muduo
git checkout v2.0.2            # 或使用 master
./build.sh -j$(nproc)          # 官方脚本：在 build/ 下编译
sudo cmake --install build     # 安装头文件与库（默认前缀 /usr/local）
```

安装完成后确认：

```bash
ls /usr/local/include/muduo/net/TcpServer.h
ls /usr/local/lib/libmuduo_net.*
```

装在其它前缀时，编译时用 `-DMUDUO_ROOT=/opt/muduo` 指定。

## 编译

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

产物是项目根目录下的 `server`（`CMAKE_RUNTIME_OUTPUT_DIRECTORY` 已设为源码根，因为程序用相对路径读 `./static` 和 `turn_config.json`）。

不想用 CMake 也可以直接一条命令：

```bash
g++ -std=c++17 -O2 -D_GNU_SOURCE -I. server.cpp jwt_verify.cpp -o server \
    -lmuduo_net -lmuduo_base -ljsoncpp -lssl -lcrypto -lpthread -lz
```

### JWT 验签自检（跨语言交叉验证）

`C++` 的 JWT 验签不依赖 muduo，可以单独构建并与 Go 的实现对答案：

```bash
# 1) 用 Go 侧真实的签发函数生成测试向量
cd auth && WRITE_JWT_VECTORS=1 go test -run TestWriteJwtVectors ./... && cd ..

# 2) 编译并运行自检（不是 cmake 也行）
g++ -std=c++17 -O2 -I. jwt_verify.cpp tests/jwt_selftest.cpp -o jwt_selftest -ljsoncpp -lcrypto
./jwt_selftest tests/jwt_vectors.json
```

期望输出 8 条 `PASS`，覆盖：有效 token、过期、错密钥、篡改载荷、`alg=none`、`typ` 不对、结构非法、空串。

## auth 服务（Go + PostgreSQL）

```bash
cd auth
go mod tidy            # 首次：拉取 pgx 与 x/crypto（国内建议先 go env -w GOPROXY=https://goproxy.cn,direct）
go build -o auth .     # 产物：auth/auth，约 16MB 单二进制

export DATABASE_URL='postgres://postgres@127.0.0.1:5432/webrtc?sslmode=disable'
export JWT_SECRET='与信令服务一致的 ≥32 字节随机串'
export AUTH_LISTEN='127.0.0.1:8080'
export COOKIE_SECURE=true     # 本地用 http 调试时设 false，否则浏览器不会回传 Cookie
./auth
```

首次运行前建库并执行迁移：

```bash
psql -U postgres -c 'CREATE DATABASE webrtc'
DATABASE_URL='postgres://postgres@127.0.0.1:5432/webrtc?sslmode=disable' ./db/migrate.sh
```

接口：

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/register` `/api/login` `/api/refresh` `/api/logout` | 账号 |
| GET | `/api/me` `/api/health` | 当前用户 / 健康检查 |
| POST | `/api/rooms` `{title, password?}` | 建房（房间号由服务端生成），返回 `roomToken` |
| POST | `/api/rooms/{id}/join` `{password?}` | 加入（校验密码 + 记录成员），返回 `roomToken` |
| GET | `/api/rooms` | 全部房间 |
| GET | `/api/rooms/mine` | 我加入过的房间 |
| DELETE | `/api/rooms/{id}` | 解散（仅创建者） |

测试：

```bash
go test ./...                                      # 单元测试（不需要数据库）
TEST_DATABASE_URL='postgres://...' go test ./...   # 追加集成测试（走真实 PostgreSQL）
```

### 本地联调（跨域）

本地时前端由 C++ 服务在 `8888` 提供，auth 服务在 `8080`，属于跨域，需要：

```bash
# 启动 auth 服务时放开前端来源，并允许非 HTTPS 下写 Cookie
export ALLOWED_ORIGINS='http://127.0.0.1:8888,http://localhost:8888'
export COOKIE_SECURE=false
./auth
```

前端会自动识别：`location.port === '8888'` 时请求 `http://127.0.0.1:8080`，否则走同源 `/api`。

> ⚠️ `COOKIE_SECURE=false` 只用于本地 HTTP 调试；生产必须为 `true`。

## 登录流程

```
登录页 login.html
   │ POST /api/login  ──▶ 200 { accessToken, user } + Set-Cookie: rt(httponly)
   │   accessToken 存内存；refreshToken 存 httpOnly Cookie（JS 读不到）
   ▼
房间页 index.html
   │ ① 页面加载：POST /api/refresh（带 Cookie）换回 accessToken，失败则回登录页
   │ ② 连上 WS 后立刻发 {"type":"auth","token":...}
   │ ③ 收到 auth_ok 才发 join_room（服务端要求 5 秒内完成鉴权）
   │ ④ joined 回 self、start_call 回 peer，界面显示双方真实姓名
   │ ⑤ 令牌过期：error{code:"invalid_token"} → 自动 refresh 并重新 auth
   ▼
退出登录：POST /api/logout（作废刷新令牌）→ 回登录页
```

## 本地开发（没有服务器时）

`tools/dev-gateway/` 用一个端口模拟 nginx + 信令服务，用于在拿不到服务器时验证前端流程：

```
http://127.0.0.1:8890
  ├── /            托管 static/
  ├── /api/*       反代到 Go auth 服务（真数据库、真令牌）
  └── /ws          假信令：实现 auth / auth_ok / join_room / joined / start_call / error
```

**它只实现协议，不实现媒体**；等服务器可用时换成真的 C++ 服务，前端一行都不用改。

```bash
# 前置：PostgreSQL 已启动、已迁移，auth 服务已在 8080 运行
cd tools/dev-gateway
npm install
# 设置 JWT_SECRET（与 auth 服务相同）
node server.js
# 浏览器打开 http://127.0.0.1:8890/login.html
```

自动自检（22 项：注册/登录/刷新轮换/Cookie 属性/WS 鉴权/超时/篡改令牌/登出）：

```bash
node e2e-test.js
```

> 假信令也校验 JWT 签名（这是 Node 实现的第三份验签），能和 Go、C++ 两方三方对照。

## 配置

```bash
cp turn_config.example.json turn_config.json
# 编辑 turn_config.json，填入真实的 TURN 地址与凭据
```

`turn_config.json` 被 `.gitignore` 忽略（历史上曾误提交过明文口令，见「安全注意事项」）。
`deploy_turn.sh` 跑完会直接打印一份可粘贴的 JSON。

## 运行

信令服务通过**环境变量**读取鉴权配置：

```bash
# 必须在项目根目录运行：静态目录和配置文件都用相对路径
export JWT_SECRET='至少32字节的随机串，必须与 auth 服务完全一致'
export AUTH_REQUIRED=true        # 默认 true；false = 退回匿名模式，仅用于调试
./server
```

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `JWT_SECRET` | 无 | **必填**（`AUTH_REQUIRED=true` 时）。HS256 密钥，≥32 字节，与 auth 服务共享 |
| `AUTH_REQUIRED` | `true` | 是否要求 WebSocket 建连后先鉴权 |

- HTTP/静态文件与 WebSocket 都在 **8888** 端口；直接访问 `http://<host>:8888/` 即可（局域网/本机调试用，非安全上下文，部分浏览器会禁用摄像头）。
- 日志同时写 stdout 和 `webrtc_server.*`（100MB 滚动）。
- 想让两台设备互连测试，建议走 nginx + HTTPS（见下），否则 `navigator.mediaDevices` 在非安全上下文下不可用。

> ⚠️ **升级提醒**：开启鉴权后，**旧版前端（不发送 `auth`）将无法入房**，会在 5 秒后被断开并收到 `auth_timeout`。
> 在完成前端登录改造之前，可先用 `AUTH_REQUIRED=false` 运行。

## 生产部署

1. **信令服务**：把 `server`、`static/`、`turn_config.json` 放到服务器（如 `/root/webrtc/`），用 systemd 守护。
   `JWT_SECRET` 走独立的环境文件，避免出现在 unit 文件里：

   ```bash
   # /root/webrtc/signaling.env   （chmod 600）
   JWT_SECRET=把这里换成与 auth 服务一致的随机串
   AUTH_REQUIRED=true
   ```

   ```ini
   # /etc/systemd/system/webrtc-signaling.service
   [Unit]
   Description=WebRTC signaling server
   After=network.target

   [Service]
   Type=simple
   WorkingDirectory=/root/webrtc
   EnvironmentFile=/root/webrtc/signaling.env
   ExecStart=/root/webrtc/server
   Restart=always
   RestartSec=2

   [Install]
   WantedBy=multi-user.target
   ```

   ```bash
   chmod 600 /root/webrtc/signaling.env
   systemctl daemon-reload && systemctl enable --now webrtc-signaling
   ```

   **auth 服务**用同样的 `JWT_SECRET`：

   ```bash
   # /root/webrtc/auth.env   （chmod 600）
   DATABASE_URL=postgres://webrtc:密码@127.0.0.1:5432/webrtc?sslmode=disable
   JWT_SECRET=与 signaling.env 完全相同的串
   AUTH_LISTEN=127.0.0.1:8080
   COOKIE_SECURE=true
   ```

   ```ini
   # /etc/systemd/system/webrtc-auth.service
   [Unit]
   Description=WebRTC auth service
   After=network.target postgresql.service

   [Service]
   Type=simple
   WorkingDirectory=/root/webrtc/auth
   EnvironmentFile=/root/webrtc/auth.env
   ExecStart=/root/webrtc/auth/auth
   Restart=always
   RestartSec=2

   [Install]
   WantedBy=multi-user.target
   ```

2. **TURN**：`sudo ./deploy_turn.sh`（脚本会问内网/公网 IP、账号口令，并可选放行本机防火墙）。

3. **nginx**：改 `nginx.conf` 里的证书路径和 `server_name`，然后 `nginx -t && systemctl reload nginx`。
   注意 `nginx.conf` 里 `root /root/webrtc/static;` 与后端自带的静态服务是**两套**，生产上只走 nginx 这一套。

4. **必须放行**的端口：

   | 端口 | 协议 | 位置 |
   |---|---|---|
   | 443 | TCP | nginx（WSS + 页面） |
   | 80 | TCP | nginx（跳转到 443） |
   | 8888 | TCP | 仅本机，**不要**对公网开放（nginx 反代） |
   | 3478 | UDP/TCP | coturn |
   | 49152-65535 | UDP | coturn 中继端口段 |

   > 云主机的**安全组/网络 ACL** 与主机防火墙是两回事，`deploy_turn.sh` 只能改后者。安全组没放行 3478 和 49152-65535 的表现是「部署全成功，但通话一直连不上」。

## 信令协议

WebSocket 文本帧，JSON，字段 `type` 区分。服务端对 `offer`/`answer`/`ice`/`video_mute`/`mic_mute` 只做原样转发，不解析内容。

### 客户端 → 服务端

| type | 载荷 | 说明 |
|---|---|---|
| `auth` | `{token}` | **建连后必须发的第一条消息**。5 秒内不发会被断开 |
| `join_room` | `{roomToken}` | 用 auth 服务签发的**进房凭证**入房。信令服务只验签取 roomId，不查数据库 |
| `list_rooms` | `{}` | 拉实时房间列表（只含当前有人的房间，用于侧栏在线人数） |
| `offer` | `{data}` | 转发的 SDP Offer |
| `answer` | `{data}` | 转发的 SDP Answer |
| `ice` | `{data}` | 转发的 ICE 候选 |
| `video_mute` | `{muted}` | 本端是否停止发送视频 |
| `mic_mute` | `{muted, started}` | 本端麦克风：`started`=是否已获取设备，`muted`=是否停止发送 |
| `bye` | `{}` | 请求结束通话。幂等 |

### 服务端 → 客户端

| type | 载荷 | 说明 |
|---|---|---|
| **`auth_ok`** | `{userId, username, displayName}` | 鉴权成功，之后才能 `join_room` |
| `joined` | `{roomId, peers, inCall, self}` | 入房成功应答。**客户端只有收到它才认为真的在房间里**。`peers` 当前人数、`inCall=true` 表示房间已满（通话进行中，别把状态打回"等待对方"）、`self` 是本端身份 |
| `live_rooms` | `{rooms: [{roomId, peers}]}` | 对 `list_rooms` 的应答。只有实时人数，房间标题/密码在 auth 服务 |
| `start_call` | `{initiator, peer, iceServers?}` | 房间已满，通话开始。`initiator=true` 的一方在重协商冲突中不让步（impolite）；`peer` 是对端身份 `{userId, username, displayName}` |
| `offer` / `answer` / `ice` | 原样 | 转发自对端 |
| `video_mute` / `mic_mute` | 原样 | 转发自对端 |
| `bye` | `{}` | 对端挂断 / 对端断线 / 对端切换到别的房间 |
| `bye_ack` | `{}` | 你发出的 `bye` 已受理 |
| `error` | `{code, message}` | 见下表 |

### 错误码

| code | 触发场景 |
|---|---|
| `room_full` | 房间已有 2 人 |
| `unauthorized` | 未鉴权就发 `join_room` / `list_rooms` |
| `invalid_token` | `auth` 的 token 签名错误 / 已过期 / 类型不对 |
| `invalid_room_token` | `join_room` 携带的进房凭证无效或已过期（有效期 2 分钟），客户端应重新向 auth 换取 |
| `auth_timeout` | 建连后 5 秒内没有发送 `auth`，连接被关闭 |
| `auth_unavailable` | 服务端未配置 `JWT_SECRET` |

> 房间不存在、密码错误、房间号被占用等**属于 auth 服务的 HTTP 错误码**（`room_not_found` / `bad_password` / `too_many_attempts` / `not_owner`），不出现在 WS 协议里——因为信令服务根本不知道房间是否存在。

### 房间语义（实现为准）

- 一个房间最多 2 条连接；第 3 条会被 `room_full` 拒绝。
- **房间的权威数据在 PostgreSQL**：是否存在、标题、密码、谁加入过。
  信令服务只维护"谁现在连着"，不判断房间是否存在、也不校验密码。
- **进房流程**：客户端先 `POST /api/rooms/{id}/join`（带密码）→ auth 校验并签发 **2 分钟有效**的 `roomToken`
  → 客户端用 `join_room {roomToken}` 入房。凭证过期会收到 `invalid_room_token`，前端会自动重新换取。
- **挂断 = 结束通话 + 离开房间**；房间本身留在数据库里，随时可以再进去。
- 内存里的房间（运行时状态）在最后一个人离开时立即回收；数据库里的房间永久保留，创建者可以解散。
- 一条连接同一时刻只属于一个房间；换房时会自动从旧房间摘除并通知旧对端。

## 前端行为要点

- **设备状态三态**（对应 `CONTEXT.md`）：未开启（无 track）、已关闭（`track.enabled=false`，设备仍占用）、开启。关闭摄像头/麦克风不释放设备，刷新页面才释放。
- **Perfect negotiation**：`initiator` 为 impolite 方，冲突时响应方 rollback 让步。
- **通话中开启新设备**触发一次重协商；关闭不需要。
- **ICE 恢复**：`disconnected` 持续 5 秒、或 `failed` 时自动 `iceRestart`。
- **信令掉线**：指数退避重连（最多 10 次），重连成功后自动补发 `join_room`；页面 `pagehide` 时主动断开，让对端立刻收到 `bye` 而不是等 TCP 超时。

## 已知限制

- **信令层已要求登录**：`join_room` 之前必须先发 `auth`（HS256 JWT），未认证连接 5 秒后被断开。
  进房还要再换一张 `roomToken`（由 auth 服务在校验房间与密码后签发），信令服务只验签。
  仍是待办：**WS 侧限流**（登录接口已有）。
- **房间元数据已持久化**（PostgreSQL），但**实时连接仍在单进程内存里**：信令服务本身还不能水平扩容（见 `docs/ha-scaling-design.md`）。
- **房间口令是短口令**（4~8 位）+ sha256 加盐，抗离线爆破能力有限；它的定位是"防误入"，账号密码才用 bcrypt。
- **TURN 凭据是静态的**，且会下发给每个客户端，等同于公开信息。想避免被白用中继，改用 coturn 的 `use-auth-secret` + REST 临时凭据即可（`deploy_turn.sh` 末尾有说明）：凭据几十秒后过期，公开也无所谓，且不需要引入任何用户系统。
- 渲染层只做了默认摄像头/麦克风，没有设备选择、屏幕共享、文字聊天、通话质量统计（`getStats`）。
- 静态文件由 C++ 服务和 nginx 各实现了一套，生产上只用 nginx，C++ 那套属于冗余。
- 服务端房间查找是线性扫描全部房间，房间多了会退化；应改为 `conn → roomId` 索引。

## 安全注意事项

- `turn_config.json` 中的 TURN 口令会下发给每个客户端，等同于公开信息。**历史上它曾以明文提交进 git**，如果你是从旧提交继承的仓库，请务必在服务器上更换口令，并考虑用 `git filter-repo` / BFG 清理历史。
- `server.cpp` 会把 `turn_config.json` 里的 `iceServers` 原样塞进 `start_call`，所以该文件不要放任何除 ICE 配置之外的敏感内容。
- `nginx.conf` 用的是自签证书（带 IP SAN）。公网使用请换成正式证书，否则浏览器会拦截。

## 排障

| 现象 | 排查方向 |
|---|---|
| 页面能打开，状态栏「WebSocket 连接失败」 | 8888 是否在跑；nginx `/ws` 反代是否配好；有没有 HTTPS 混合内容拦截 |
| 点击开启摄像头无反应/提示未授权 | 必须是 HTTPS 或 `localhost`（安全上下文），否则 `navigator.mediaDevices` 不存在 |
| 「房间已满」但房间里没人 | 对端断网留下的半开连接。服务端每 30 秒 ping，连续两次无 pong（最多 60 秒）会自动回收；稍等重试 |
| 通话建立后很快卡住 | ICE 没打通。查 TURN 是否可达、3478 与 49152-65535 是否放行、`start_call` 日志里有没有注入 ICE 配置 |
| 看不到对方画面、状态一直是「通话中」 | 看服务端日志有没有 `转发 offer/answer/ice`；对端是否只开了麦克风没开摄像头 |
| 服务起不来 | `journalctl -u webrtc-signaling`；常见是 `turn_config.json` 缺失（只是告警，不致命）或 8888 被占用 |
