# 运维演练记录

> 为什么要有这份文件：**"能跑"和"坏了能救回来"是两件事。**
> 备份没恢复过就不算备份；优雅下线没演练过就不知道客户端会不会卡死；数据库挂了会不会连累通话，只能停一次才知道。
>
> 每次演练都记录五件事：目的、怎么操作、预期什么、实际看到什么、结论（能不能写进 runbook 当标准流程）。

演练环境：本机 VMware 上的 Ubuntu 24.04 虚机，4 vCPU / 4 GiB，IP `192.168.220.128`，全部组件按 `deploy/install.sh` 部署。

| # | 演练 | 脚本 | 结果 | 日期 |
| --- | --- | --- | --- | --- |
| 1 | 备份 → 恢复到临时库 | `tools/ops/restore-drill.sh` | ✅ 4 张表行数全部一致 | 2026-11-22 |
| 2 | 优雅下线（SIGTERM） | `tools/e2e/graceful-restart.js` | ✅ 6/6 | 2026-11-22 |
| 3 | 进程被强杀（SIGKILL / OOM） | `tools/e2e/graceful-restart.js --hard` | ✅ 5/5 | 2026-11-22 |
| 4 | 数据库停机 | `tools/ops/pg-outage-drill.js` | ✅ 9/9 | 2026-11-22 |
| 5 | 1 小时长稳 | `tools/ops/soak-sample.sh` + `loadtest.js` | ⏳ 待跑（等选时间窗） | — |

---

## 1. 备份 → 恢复

**目的**：证明 `/var/backups/webrtc/*.dump` 是**能装回去的**，而不是一个大小正常的空壳。

**为什么必须演练**：`pg_dump` 成功 ≠ 能恢复。权限、owner、extension、序列、编码，任何一环出问题都只在恢复时暴露——那时候通常已经在救火了。

**操作**

```bash
bash tools/ops/restore-drill.sh 192.168.220.128
```

脚本做的事：

1. 从 `/var/backups/webrtc/` 取最新的 `*.dump`（不存在就先 `systemctl start webrtc-backup.service` 生成一个）；
2. `sudo -u postgres createdb webrtc_restore_<HHMMSS>` 建**临时库**（不动生产库）；
3. `sudo -u postgres pg_restore --no-owner --no-acl < dump`；
4. 逐表比对 `users` / `rooms` / `room_members` / `auth_tokens` 的行数；
5. `trap` 里 drop 临时库（失败也清理）。

**预期**：4 张表行数完全一致，最后一行打印「恢复演练通过：备份是可用的」。

**实际**

```
备份文件：/var/backups/webrtc/webrtc-20261122-110523.dump（32K）
PASS  users        83 行
PASS  rooms        35 行
PASS  room_members 69 行
PASS  auth_tokens  170 行
恢复演练通过：备份是可用的
```

**踩到的坑（已写进 runbook）**：第一版把文件路径作为参数交给 `pg_restore`，报
`pg_restore: error: could not open input file "...dump": 权限不够`
——备份文件是 `0600 root`、目录 `0700`，`postgres` 用户根本读不到。
改成 `sudo -u postgres pg_restore ... < "$latest"`，让 **root 的 shell 负责重定向**、`postgres` 只负责连库。这个坑很典型：**恢复权限问题和备份内容是两回事，别一开始就怀疑备份坏了。**

**结论**：备份可用；恢复流程固化为 runbook §2.4。生产上真正回滚会丢"备份之后注册的账号和新建的房间"，正在通话的人不掉线（信令不查库）。

---

## 2. 优雅下线（SIGTERM）

**目的**：`systemctl restart` / 发布新版时，**正在通话的用户不该被"无声掐断"**，而应收到明确通知并在几秒内自动重连。

**操作**

```bash
node tools/e2e/graceful-restart.js 192.168.220.128 8888
```

脚本做的事：两个客户端本地签 JWT 进同一房间拿到 `start_call` → 通过 SSH 执行 `systemctl restart webrtc-signaling` → 观察客户端收到什么。

**预期**

| 检查项 | 预期 |
| --- | --- |
| 客户端收到 `{"type":"server_shutdown","reason":"restart","retryAfterMs":3000}` | ✅ |
| 随后连接被服务端关闭 | ✅ |
| 客户端按 `retryAfterMs` 重连成功 | ≤ 5 秒 |
| `/stats` 的 `uptimeSeconds` 归零、`rooms.total` 归零 | ✅（状态不跨进程残留） |

**实际**

```
PASS  重启前双方已在通话中（收到 start_call）
PASS  重启时客户端收到 server_shutdown（而不是被无声掐断）
PASS  server_shutdown 带 reason=restart 与 retryAfterMs
PASS  收到通知后连接被关闭
PASS  按 retryAfterMs=3000 重连成功
PASS  /stats 显示 uptime 归零、房间数归零
合计: 6 通过, 0 失败
```

服务端日志佐证：

```
收到退出信号，开始优雅下线（广播 server_shutdown 后退出）
已通知 2 个连接，200ms 后关闭
```

**实现要点（给以后改代码的人）**：信号处理器里**只做 `write()` 到 self-pipe**，真正的关闭逻辑在 EventLoop 里跑——信号处理器里调用 `conn->send()` 是未定义行为。

**结论**：可以放心用 `systemctl restart` 发布；客户端不需要人工刷新。

---

## 3. 进程被强杀（SIGKILL / OOM）

**目的**：验证"最坏情况"——进程来不及做任何事就没了，客户端能不能自己恢复。

**为什么和上一条分开测**：优雅下线是**我们主动做对**；SIGKILL 是**假设一切保护都没生效**（OOM killer、`kill -9`、断电）。两条路径的客户端行为完全不同，必须分别断言。

**操作**

```bash
node tools/e2e/graceful-restart.js 192.168.220.128 8888 --hard
```

内部执行 `systemctl kill -s SIGKILL webrtc-signaling`，靠 unit 的 `Restart=always` 拉起。

**预期**

| 检查项 | 预期 |
| --- | --- |
| 客户端**收不到** `server_shutdown` | ✅（进程没机会发） |
| 连接被内核断开（TCP reset） | ✅ |
| 客户端靠**指数退避**重连成功 | ≤ 10 秒 |
| 重启后房间状态归零 | ✅ |

**实际**

```
PASS  重启前双方已在通话中
PASS  SIGKILL 后没有收到 server_shutdown（符合预期，进程没机会发）
PASS  SIGKILL 后连接被断开
PASS  靠退避重连成功（2.1 秒）
PASS  新进程已就绪、房间数归零
合计: 5 通过, 0 失败
```

**结论**：即使进程被强杀，用户最多经历约 2~3 秒的"连接中"再自动恢复；不需要人工介入。前提是 unit 里必须有 `Restart=always`（`deploy/webrtc-signaling.service` 里有）。

---

## 4. 数据库停机

**目的**：验证架构上的一个关键取舍——**信令服务不连数据库，只验 JWT 签名**。所以数据库挂掉时，正在通话的人应该毫无感觉，只有需要查库的动作（登录、注册、建房、进房换 token）失败。

**为什么值得单独演练**：这是"认证拆成独立服务"这个设计最直接的收益。没有演练，它只是设计文档里的一句话。

**操作**

```bash
node tools/ops/pg-outage-drill.js 192.168.220.128 8888
```

脚本做的事：两个 WS 客户端进房拿到 `start_call` → `systemctl stop postgresql` → 检查通话与 HTTP 接口 → `systemctl start postgresql` → 再检查一遍。

**预期**

| 阶段 | 检查项 | 预期 |
| --- | --- | --- |
| 停机中 | 信令仍转发 `ice` | ✅ |
| 停机中 | 两个连接都没被断开 | ✅ |
| 停机中 | `/api/health` → **503** 且 `"db":"down"` | ✅ |
| 停机中 | 登录失败返回 5xx（不能假装成功） | ✅ |
| 恢复后 | `/api/health` → 200 `db=ok` | ✅ |
| 恢复后 | 能重新注册（写路径通了） | ✅ |
| 恢复后 | **原来那两个连接仍然可用** | ✅ |

**实际**

```
PASS  停机前双方已在通话中
PASS  停机前 /api/health 是正常的
==> 停掉 PostgreSQL
PASS  数据库停机时，信令仍然转发消息（通话不受影响）
PASS  数据库停机时，两个连接都没被断开
PASS  数据库停机时 /api/health 返回 503 且 db=down
PASS  数据库停机时登录失败（5xx，而不是假装成功）
==> 恢复 PostgreSQL
PASS  数据库恢复后 /api/health 回到 200/db=ok
PASS  数据库恢复后能重新注册账号（写路径通了）
PASS  数据库恢复后，原来的通话连接仍然可用（全程没掉线）
合计: 9 通过, 0 失败
```

**踩到的坑（这个是真的 bug 级发现）**：第一版脚本签 JWT 时用 `sub: String(id)`（字符串），服务端一律回 `invalid_token`。
根因是服务端用 jsoncpp 的 `asInt64()` 读 `sub`，**它不接受字符串型数字**。改成 `sub: id`（JSON number）后通过。
教训：**签 token 的字段类型必须和服务端解析方式一致**，跨语言（Node 签 / C++ 验）尤其容易踩；`tools/e2e/ws-smoke.js` 里一直用的是数字，所以之前没暴露。

**结论**：数据库故障的爆炸半径 = "不能登录/建房"，而不是"通话中断"。这也是 runbook §3.7 敢写"先修库、通话不用管"的依据。

---

## 5. 1 小时长稳（待跑）

**目的**：抓**慢泄漏**和**性能衰减**——5 分钟的压测看不出每小时涨 2 MB 的内存、也看不出 fd 有没有慢慢攒。

**计划**

```bash
# 0) 清场：停掉会抢 CPU 的后台任务（apt 自动更新是惯犯）
sudo systemctl stop unattended-upgrades prometheus
ps -eo pid,pcpu,comm --sort=-pcpu | head -5

# 1) 采样器（每 30 秒采一次 /metrics 写 CSV）
bash tools/ops/soak-sample.sh 192.168.220.128 8888 3600

# 2) 负载：100 房间 / 200 连接 / 1 小时
node tools/load/loadtest.js 192.168.220.128 8888 --secret "$JWT_SECRET" \
  --rooms 100 --ramp-up 30 --duration 3600 --reset --stats-token "$STATS_TOKEN"
```

**判据**

| 指标 | 通过标准 |
| --- | --- |
| RSS | 1 小时增长 < 2 MB，且不单调 |
| fd | 不随时间为增长（应等于连接数 + ~22） |
| 转发 P95 | 后 30 分钟相对前 30 分钟漂移 < 20% |
| 错误 | `errors.*` 全为 0 |

**当前状态**：等使用者挑一个"电脑不做重活"的完整时段再跑（负载客户端跑在同一台电脑上，会互相干扰）。跑完把数据补进本节，并写 `docs/soak-report.md`。

---

## 变更记录

| 日期 | 变更 | 验证 | 回滚 |
| --- | --- | --- | --- |
| 2026-11-19 | 首次全栈部署（PG 16 + Go auth + C++ 信令 + nginx + coturn） | e2e 34/34、浏览器 22/22 | — |
| 2026-11-19 | TURN 动态凭据（coturn REST API） | `--relay` 27/27 | 改 `turn_config.json` 为静态凭据 |
| 2026-11-20 | 阶段二：房间索引、日志降档、限流/背压、优雅下线 | 30/30、34/34、33/33、6/6 | 回滚二进制 |
| 2026-11-21 | 阶段四：设备切换、质量面板、聊天、移动端 | 33/33、57/57、62/62 | 回滚 `static/` |
| 2026-11-22 | 阶段五：`/metrics`、Prometheus、每日备份 | 36/36、34/34、57/57、6/6、5/5、9/9、恢复演练通过 | 卸载 prometheus / `--no-backup` 重跑 |
