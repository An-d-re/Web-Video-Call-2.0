# 信令压测（tools/load）

只回答一个问题：**这台机器能同时扛多少路一对一通话的信令？**

不是"挂着几千个 socket 看连不连得上"——那样测出来的是操作系统的 TCP 栈。
这里每个连接都完整走一遍真实业务顺序：

```
握手 → auth → join_room → 收 start_call → offer/answer 转发 → ICE 互发 → bye
```

所以测出来的数字，和用户实际感受到的延迟是同一件事。

> 第一次做压测？先读 [`docs/loadtest-report.md`](../../docs/loadtest-report.md) 的第 1~3 节
> （看哪些参数、为什么看、用了什么工具），第 11 节是可以照着敲的手把手流程。

---

## 怎么跑

```bash
# 1) 拿到服务端密钥（signaling 模式要自己签 token）
sudo grep -E '^(JWT_SECRET|STATS_TOKEN)=' /opt/webrtc/signaling.env

# 2) 小规模确认能跑通（2 房间 = 4 连接）
node tools/load/loadtest.js --host 192.168.220.128 --mode signaling \
  --secret "<JWT_SECRET>" --stats-token "<STATS_TOKEN>" --rooms 2 --duration 5

# 3) 正式场景
node tools/load/loadtest.js --host 192.168.220.128 --mode signaling \
  --secret "<JWT_SECRET>" --stats-token "<STATS_TOKEN>" \
  --rooms 200 --ramp-up 20 --duration 60 --out .tmp/load/s2.json
```

`ws` 依赖复用 `tools/dev-gateway/node_modules/ws`，仓库根不需要 `npm install`。

### 两种模式

| 模式 | 做什么 | 用来回答 |
|---|---|---|
| `--mode signaling`（默认） | 本地用 `JWT_SECRET` 签 access/room token，直连 8888 | 信令服务本身能扛多少（排除 nginx / Go / PG） |
| `--mode fullstack` | 走真实 API 注册、建房、`POST /api/rooms/{id}/join` 换 roomToken | 用户实际体验，但受注册限流影响，规模别开大 |

### 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--host` / `--port` | `127.0.0.1:8888` | 直连 C++ 信令服务（不是 443，绕过 nginx） |
| `--mode` | `signaling` | 见上表 |
| `--secret` | `$JWT_SECRET` | signaling 模式必需，必须与服务端一致，否则一片 `invalid_token` |
| `--stats-token` | `$STATS_TOKEN` | 拿服务端 `/stats` 快照；不给就只输出客户端侧数字 |
| `--reset` | 关 | 开跑前调 `/stats?reset=1` 清一次窗口，只统计本轮（**对比两轮稳定性时必须加**） |
| `--rooms` | `50` | 房间数，每房 2 个连接，所以连接数 = 2 × rooms |
| `--ramp-up` | `5` | 爬坡秒数，200 房间建议 ≥20 |
| `--duration` | `30` | 稳态秒数（爬坡结束后才开始计） |
| `--ice-per-sec` | `4` | 每个连接每秒发多少 ICE 探针（真实 WebRTC 在连通后就基本不发，这里是故意加压） |
| `--out` | — | 结果 JSON 落盘路径 |
| `--prefix` | `LT` | 房间号前缀，多进程压测时用来分段 |
| `--merge` | — | 合并多个结果 JSON 再出一张总表 |
| `--trace` | — | 打印 offer/answer 逐条时间线（排查延迟用） |

---

## 四个场景

| 场景 | 命令要点 | 看什么 |
|---|---|---|
| **S1 连接建立** | `--rooms 50/250/500/1000 --ramp-up 10/20/40/60 --duration 5` | 连接建立、auth、join 的 P95 随规模怎么涨 |
| **S2 消息延迟** | `--rooms 200 --ramp-up 20 --duration 60 --reset`，连跑两次 | ICE 往返 P95，两次差异应 <10% |
| **S3 房间规模** | `--rooms 1000 --ramp-up 60 --duration 30` | 房间数涨 10 倍后 join 是否明显变慢（线性扫描房间、全局锁的代价） |
| **S4 长稳** | `--rooms 200 --ramp-up 20 --duration 3600` | `rssMb` / `fds` 是否单调上涨、P95 是否漂移 |

单进程打不满就多进程分段（每段用自己的前缀，最后合并）：

```bash
node tools/load/loadtest.js ... --prefix A --rooms 500 --out a.json &
node tools/load/loadtest.js ... --prefix B --rooms 500 --out b.json &
node tools/load/loadtest.js --merge a.json,b.json
```

注意：客户端也是单线程 Node，连接数过千时**先看客户端 CPU**，别把客户端瓶颈当成服务端瓶颈。

---

## 看什么指标

客户端侧（自己测的往返，最接近用户体验）：

- **连接建立**：TCP+WS 握手时间
- **auth 往返**：鉴权消息一来一回
- **join_room**：入房到收到 `joined`
- **offer→answer**：转发的端到端延迟（这条最容易暴露服务端缓冲/锁问题）
- **ICE 往返**：稳态下的小包转发延迟

服务端侧（`/stats`，需要 `STATS_TOKEN`）：

- `connections{ws,authed,unauthenticated}`：在线连接、已鉴权、5 秒内没鉴权的
- `rooms{total,full,members}`：房间总数、满员房间、座位总数
- `messages{...}` / `errors{...}`：分类计数，看错误是不是集中爆发
- `latencyUs{join_room,forward}`：**服务端内部**处理耗时（不含网络），p50/p95/p99/max
- `resources{rssMb,fds}`：内存与文件描述符，长稳场景主要看这两个

---

## 已知坑

1. **`offer→answer` 曾经稳定多出 43ms**。原因不是代码逻辑，是服务端 socket 没关 Nagle：
   服务端刚写完 `start_call`，紧接着转发 `offer`，第二个小包被 Nagle 压住等对端延迟 ACK（40ms）。
   已在 `server.cpp` 的 `onConnection` 里 `conn->setTcpNoDelay(true)` 修掉，修完 P95 从 50ms 降到 1~2ms。
   **如果以后又看到 40ms 整数倍，先查 Nagle。**
2. 密钥不一致时是一片 `invalid_token`，别当代码 bug 查。压测脚本会提示。`/stats` 的直方图**默认是进程启动以来的累计值**，不加 `--reset` 时两轮的数字会互相污染，"看起来稳定"其实是历史平均。
3. 跑完一定要 `sudo systemctl restart webrtc-signaling`：压测期间 `/stats` 计数和房间残留会带到下一次。
4. 虚机只有 4 GiB 内存，500 连接以上注意 `rssMb`；从 500 起步，逼近阈值就停。
5. `--mode fullstack` 会撞注册限流（`registerIP` 5/分钟、突发 3），账号数别超过 3 个，或者提前手工注册。
