# 运维手册（Runbook）

> 这份文档写给"半夜被叫起来的那个人"——包括三个月后的你自己。
> 原则：**先判断影响面（能不能打电话），再动手；动手前先备份，动完必须验证。**

---

## 0. 怎么用这份手册

| 你的处境 | 看哪一节 |
| --- | --- |
| 刚接手，想知道这台机器上都跑着什么 | §1 系统全貌 |
| 想重启 / 看日志 / 看指标 / 做备份 | §2 日常操作 |
| 用户报"打不开""连不上""没画面" | §3 故障处置（按症状查表） |
| 要发新版本 / 出了事要回滚 | §4 变更与回滚 |
| 浏览器报证书错误 | §5 证书 |
| 想知道这台机器还能撑多少人 | §6 容量 |

---

## 1. 系统全貌

### 1.1 拓扑

```
浏览器 A ──┐                       ┌── /            → /opt/webrtc/static（静态页）
           ├─ HTTPS 443 ─ nginx ───┼── /api/         → 127.0.0.1:8080  webrtc-auth（Go）
浏览器 B ──┘                       └── /ws           → 127.0.0.1:8888  webrtc-signaling（C++/muduo）
                                        │
浏览器 A ←──────── WebRTC 媒体（P2P 直连；打不通时经 coturn 中继 3478）────────→ 浏览器 B

webrtc-auth ── PostgreSQL 127.0.0.1:5432（用户、房间、加入记录、refresh token）
prometheus  ── 每 15s 抓 127.0.0.1:8888/metrics（只监听 loopback）
```

**关键设计**：信令服务**不连数据库**——它只验 JWT 签名。所以数据库挂了，正在通话的两个人不会掉线，只有"登录/建房/进房"这些需要查库的动作会失败。（这条有演练记录，见 `docs/ops-drills.md`）

### 1.2 组件清单

| 组件 | 进程 / unit | 监听 | 日志 |
| --- | --- | --- | --- |
| 反向代理 | `nginx.service` | `0.0.0.0:80`（跳 443）、`0.0.0.0:443` | `/var/log/nginx/{access,error}.log` |
| 信令 | `webrtc-signaling.service` | `0.0.0.0:8888` | `journalctl -u webrtc-signaling` + `/opt/webrtc/webrtc_server.*.log` |
| 认证 | `webrtc-auth.service` | `127.0.0.1:8080` | `journalctl -u webrtc-auth` |
| 数据库 | `postgresql.service` | `127.0.0.1:5432` | `/var/log/postgresql/postgresql-16-main.log` |
| TURN | `coturn.service` | `192.168.220.128:3478`（tcp+udp）、`127.0.0.1:3478` | `journalctl -u coturn` |
| 监控 | `prometheus.service` | `127.0.0.1:9090` | `journalctl -u prometheus` |
| 备份 | `webrtc-backup.timer` → `.service` | 无（每日 03:15 ± 10min） | `journalctl -u webrtc-backup` |

### 1.3 文件都在哪

| 路径 | 内容 |
| --- | --- |
| `/opt/webrtc/server` | 信令二进制（C++ 编译产物） |
| `/opt/webrtc/auth` | 认证二进制（Go 静态链接） |
| `/opt/webrtc/static/` | 前端（`index.html`、`login.html`、`js/auth.js`） |
| `/opt/webrtc/signaling.env` | `JWT_SECRET`、`STATS_TOKEN`、`AUTH_REQUIRED`、`LOG_PAYLOAD` |
| `/opt/webrtc/auth.env` | `DATABASE_URL`、`JWT_SECRET`（**必须和 signaling.env 里的一致**）、`ALLOWED_ORIGINS` |
| `/opt/webrtc/turn_config.json` | 下发给浏览器的 ICE 服务器（含 coturn 的 `static-auth-secret`） |
| `/etc/nginx/sites-enabled/webrtc` | 站点配置（仓库里的 `deploy/nginx-webrtc.conf`） |
| `/etc/nginx/ssl/webrtc.crt` `.key` | 自签证书（SAN 含本机 IP） |
| `/etc/turnserver.conf` | coturn 配置（含 `static-auth-secret`） |
| `/var/backups/webrtc/` | 数据库备份（`*.dump`，保留 7 天，权限 600） |
| `/opt/webrtc/webrtc_server.*.log` | muduo 日志，**按小时/大小滚动，会一直累积** |

> ⚠️ `JWT_SECRET` 在 `signaling.env` 和 `auth.env` 里各有一份，**必须相同**。`install.sh` 生成时写的是同一个值，手动改的话两边都要改。

### 1.4 账号与密钥

| 用途 | 位置 |
| --- | --- |
| 虚机登录 | 用户 `andres`，密码 `Andres&123`（演示环境，公网部署务必换） |
| 数据库 | `webrtc` / `webrtc_dev_pw`，库名 `webrtc`（见 `auth.env`） |
| `/stats` `/metrics` | `STATS_TOKEN`（见 `signaling.env`） |
| TURN 共享密钥 | `/etc/turnserver.conf` 的 `static-auth-secret`，与 `/opt/webrtc/turn_config.json` 的 `secret` 必须一致 |

---

## 2. 日常操作

### 2.1 一眼看健康

```bash
# 五个服务是不是都活着
systemctl is-active nginx webrtc-signaling webrtc-auth postgresql coturn prometheus

# 从外面看（证书是自签的，所以 -k）
curl -sk https://127.0.0.1/api/health          # {"db":"ok","status":"ok",...}
curl -sk -o /dev/null -w '%{http_code}\n' https://127.0.0.1/   # 200

# 信令自己的指标（注意只能在虚机上跑，或者带 token）
TOKEN=$(sudo grep -oP '^STATS_TOKEN=\K.*' /opt/webrtc/signaling.env)
curl -s "http://127.0.0.1:8888/stats?token=$TOKEN" | python3 -m json.tool | head -40
```

`/stats` 关键字段怎么读：

| 字段 | 正常值 | 异常意味着 |
| --- | --- | --- |
| `connections.ws` | 等于在线人数 | 掉到 0 但用户说在通话 → 连接被掐了 |
| `connections.unauthenticated` | 0~2（握手瞬间） | 持续 >10 → 有客户端连上但不发 `auth`，可能是扫描或前端坏了 |
| `rooms.total` | 等于活跃房间数 | 长时间不为 0 且没人 → 座位泄漏，看 §3.9 |
| `latencyUs.forward.p95` | < 500µs | > 5ms → CPU 被打满或有人在灌消息 |
| `errors.rate_limited` | 0 | 涨 → 有人刷接口（见 §3.10） |
| `errors.slow_client` | 0 | 涨 → 有客户端不读数据（见 §3.11） |
| `resources.rssMb` | 25~35 | 持续单调上升 → 内存泄漏（见 §3.12） |
| `resources.fds` | 连接数 + ~22 | 远大于连接数 → fd 泄漏 |

### 2.2 看日志

```bash
journalctl -u webrtc-signaling -n 100 --no-pager          # 最近 100 行
journalctl -u webrtc-signaling --since '10 min ago' | grep -E 'WARN|ERROR'
journalctl -u webrtc-auth --since today | tail -50
tail -n 50 /var/log/nginx/error.log
ls -lt /opt/webrtc/webrtc_server.*.log | head -3          # 最新的 muduo 日志
```

日志分级说明：

- 默认**不记录消息内容**（只记类型和长度），需要排查协议问题时才临时开：`sudo sed -i 's/^LOG_PAYLOAD=.*/LOG_PAYLOAD=1/' /opt/webrtc/signaling.env && sudo systemctl restart webrtc-signaling`，**排查完记得改回 0**（payload 里有 SDP 和聊天内容）。
- `房间索引与实际不一致` 是 WARN，出现即说明索引缓存和 `g_rooms` 对不上（会自动退化为遍历，功能不受影响，但要记下来查根因）。

### 2.3 看监控

```bash
# Prometheus 只在虚机本机可访问
curl -s 'http://127.0.0.1:9090/api/v1/targets' | python3 -c 'import json,sys; [print(t["labels"]["job"], t["health"], t.get("lastError","")) for t in json.load(sys.stdin)["data"]["activeTargets"]]'

curl -s 'http://127.0.0.1:9090/api/v1/query?query=webrtc_connections' | python3 -m json.tool

# 如果有浏览器能上这台机器，也可以用 ssh 端口转发：
#   ssh -L 9090:127.0.0.1:9090 andres@192.168.220.128
# 然后本机访问 http://127.0.0.1:9090/graph
```

### 2.4 备份与恢复

```bash
# 手动备份一次
sudo systemctl start webrtc-backup.service
sudo journalctl -u webrtc-backup -n 20 --no-pager

# 看现有备份
sudo ls -lht /var/backups/webrtc/ | head

# 定时器状态（下一个触发时间）
systemctl list-timers webrtc-backup.timer

# 恢复演练：恢复到临时库并比对行数，不动生产库
bash tools/ops/restore-drill.sh 192.168.220.128
```

**真的要把生产库回滚到某个备份**：

```bash
sudo systemctl stop webrtc-auth                    # 先停写
sudo -u postgres dropdb webrtc && sudo -u postgres createdb -O webrtc webrtc
sudo -u postgres pg_restore --no-owner --no-acl -d webrtc < /var/backups/webrtc/webrtc-YYYYmmdd-HHMMSS.dump
sudo -u postgres psql -d webrtc -c 'select count(*) from users;'
sudo systemctl start webrtc-auth
curl -sk https://127.0.0.1/api/health
```

代价：备份之后注册的账号、新建的房间会消失；正在通话的人**不会掉线**（信令不查库），但他们下次登录就没了。

---

## 3. 故障处置

> 每条都按「症状 → 怎么判断 → 怎么处置」写。判断步骤是为了避免"看到 502 就重启"，那样只会让下一个问题更难查。

### 3.1 打开页面 502 Bad Gateway

**症状**：浏览器显示 502，nginx 有响应。

**怎么判断**：

```bash
systemctl is-active webrtc-signaling webrtc-auth
curl -s -o /dev/null -w 'signaling:%{http_code}\n' http://127.0.0.1:8888/
curl -s -o /dev/null -w 'auth:%{http_code}\n' http://127.0.0.1:8080/api/health
journalctl -u webrtc-signaling -n 30 --no-pager
```

- 502 出现在 `/` 或 `/ws` → 信令挂了（nginx 把静态页面也代理给它）。
- 502 出现在 `/api/` → 认证服务挂了。

**怎么处置**：

```bash
sudo systemctl restart webrtc-signaling    # 会优雅下线：先给客户端发 server_shutdown 再退
sudo systemctl restart webrtc-auth
```

起来后必须验证：`curl -sk https://127.0.0.1/api/health` 返回 `db=ok`、`/stats` 里 `connections` 开始增长。

### 3.2 页面能打开，但一直"连接中"

**症状**：登录成功、房间列表也有了，但进房后状态一直不变成"等待对方"。

**怎么判断**：

```bash
# 1) wss 升级有没有被 nginx 正确转发
curl -ski -o /dev/null -w '%{http_code}\n' \
  -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
  -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
  https://127.0.0.1/ws
# 期望 101；401/403 → nginx 没转发 Upgrade 头或 JWT 被拒；502 → 信令挂了（见 3.1）

# 2) nginx 有没有把 Upgrade 头传下去
grep -A3 'location /ws' /etc/nginx/sites-enabled/webrtc
```

**常见根因**：

| 现象 | 根因 | 处置 |
| --- | --- | --- |
| 返回 400 而不是 101 | 少了 `proxy_set_header Upgrade $http_upgrade` | 用仓库里的 `deploy/nginx-webrtc.conf` 覆盖，`nginx -t && nginx -s reload` |
| 连接立刻断，服务端日志有 `auth_timeout` | 前端拿到 ws 后 5 秒内没发 `auth`（token 拿不到/JS 报错） | 浏览器控制台看报错；确认 `js/auth.js` 加载成功 |
| `server_shutdown` 后不重连 | 前端 `scheduleReconnect` 没跑 | 刷新页面；检查是否有 `window.onerror` |

### 3.3 浏览器报"证书不受信任"

**这是自签证书的正常现象，不是故障。** `install.sh` 生成的自签证书 SAN 里包含 `localhost`、`127.0.0.1` 和本机局域网 IP。

**处置**：

- 演示时在浏览器点"高级 → 继续访问"；或者把 `/etc/nginx/ssl/webrtc.crt` 导入到客户端的受信任根。
- **必须用 HTTPS 的原因**：`getUserMedia` 只在安全上下文可用，`http://192.168.x.x` 打不开摄像头。

### 3.4 证书过期

**症状**：突然所有浏览器都拒绝访问，报 `NET::ERR_CERT_DATE_INVALID`。

**怎么判断**：

```bash
openssl x509 -in /etc/nginx/ssl/webrtc.crt -noout -dates -subject -ext subjectAltName
# 当前证书 notAfter = Feb 21 10:06:53 2029 GMT
```

**怎么处置**（重签 + 保留私钥不必要，直接整套换）：

```bash
# 方式一：重跑 install.sh（幂等，会自动检测 IP 重新生成）
sudo bash deploy/install.sh --server /opt/webrtc/server --auth /opt/webrtc/auth --static /opt/webrtc/static

# 方式二：手动重签（IP 换成实际的）
sudo openssl req -x509 -newkey rsa:2048 -nodes -days 1095 \
  -keyout /etc/nginx/ssl/webrtc.key -out /etc/nginx/ssl/webrtc.crt \
  -subj '/CN=webrtc.local' -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:192.168.220.128'
sudo nginx -t && sudo systemctl reload nginx
```

### 3.5 进房提示"房间人数已满"

**症状**：加入房间回 `room_full`。

**怎么判断**：房间上限是 **2 人**（一对一产品）。先确认房间里真的有两个人：

```bash
TOKEN=$(sudo grep -oP '^STATS_TOKEN=\K.*' /opt/webrtc/signaling.env)
curl -s "http://127.0.0.1:8888/stats?token=$TOKEN" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["rooms"])'
```

- `rooms.total` 比实际用户数多 → 有**幽灵房间/幽灵成员**（连接已断但没清理干净）。
- 房间数正常、用户确实两个人 → 就是满了，让对方先挂断。

**幽灵房间的处置**：先看日志有没有大量 `房间 X 已空，回收运行时状态`（说明回收在工作），再确认是不是"用户没点退出就关了标签页"——那种情况靠 TCP 断开触发清理，最迟 30 秒（心跳）内完成。**仍然怀疑时，重启信令服务会清空所有房间状态，代价是所有人掉线重连。**

### 3.6 两人都在房间里，但一直黑屏 / 卡在"连接中"

**症状**：状态显示"通话中"或"等待 ICE"，但看不到对方。

**怎么判断**（按顺序）：

1. **是不是同一个局域网内的 P2P 就能通**？在双方浏览器打开 `chrome://webrtc-internals`，看 `candidate-pair` 的 `state` 是否为 `succeeded`，`localCandidate` 的 type 是 `host`/`srflx`（直连）还是 `relay`（中继）。
2. **服务端有没有把 offer/answer/ice 转发出去**？临时开 payload 日志（§2.2），或看 `/stats` 的 `messages.offer/answer/ice` 计数是否增长。
3. **TURN 是否可用**：

```bash
sudo systemctl is-active coturn
sudo turnutils_uclient -v -u "$(( $(date +%s) + 3600 )):1" \
  -W "$(sudo grep -oP 'static-auth-secret=\K.*' /etc/turnserver.conf)" \
  -p 3478 -e 192.168.220.128 -r 3480 192.168.220.128
# 成功标志：6 sent, 6 received, 0% loss
```

**怎么处置**：

| 判断结果 | 处置 |
| --- | --- |
| 服务端报文计数不涨 | 信令问题，回到 §3.2 |
| coturn 不 active | `sudo systemctl restart coturn`，仍失败看 `journalctl -u coturn` |
| 跨网段永远连不上（没有任何 succeeded 的 candidate-pair） | 云服务器上必须设 `external-ip`：给 `install.sh` 传 `TURN_PUBLIC_IP=<公网IP>`；内网演示则检查防火墙 3478 与 49160-49200/udp |
| 只有 relay 能通、延迟高 | 正常（TURN 中继多一跳），不是故障 |

### 3.7 登录 500 / "/api/health 返回 503 db=down"

**症状**：登录、注册、建房全部失败；正在通话的人**没事**。

**怎么判断**：

```bash
systemctl is-active postgresql
curl -sk https://127.0.0.1/api/health          # {"db":"down"}
journalctl -u postgresql -n 50 --no-pager
df -h /                                        # 磁盘满了 PG 也会挂
```

**怎么处置**：

```bash
sudo systemctl start postgresql
sleep 2
curl -sk https://127.0.0.1/api/health          # 回到 db=ok
```

PG 起不来看日志：`could not write to file` → 磁盘满（§3.8）；`database system was interrupted` → 需要恢复（§2.4）。

> 这条有演练记录：数据库停机期间信令照常转发、两人通话不掉线，只有登录/建房失败。

### 3.8 磁盘满

**症状**：写入失败、PG 报错、nginx 无法写临时文件、备份失败。

**怎么判断**：

```bash
df -h /
sudo du -xh --max-depth=1 / 2>/dev/null | sort -h | tail -8
sudo du -sh /var/log /opt/webrtc /var/backups/webrtc /var/lib/prometheus
sudo ls -lht /opt/webrtc/webrtc_server.*.log | head -5
```

**这台机器上最容易长大的三样**：journald 日志、`/opt/webrtc/webrtc_server.*.log`、`/var/lib/prometheus`。

**怎么处置**：

```bash
sudo journalctl --vacuum-size=200M
sudo find /opt/webrtc -maxdepth 1 -name 'webrtc_server.*.log' -mtime +7 -delete
sudo find /var/backups/webrtc -name '*.dump' -mtime +7 -delete
sudo systemctl restart prometheus              # 清临时 WAL（谨慎，会丢未落盘的样本）
```

### 3.9 `/stats` 里 `rooms.total` 长时间不为 0，但没人在用

**症状**：座位/房间泄漏，新用户进不去（一直 `room_full`）。

**怎么判断**：

```bash
journalctl -u webrtc-signaling --since '30 min ago' | grep -c '房间 .* 已空，回收运行时状态'
journalctl -u webrtc-signaling --since '30 min ago' | grep -E '房间索引与实际不一致' | head
```

- 有大量"已空，回收"→ 回收逻辑在工作，泄漏的是**连接没断**（客户端异常掉线但 TCP 还挂着），等心跳超时会清。心跳每 30 秒一轮、**两轮判定，最坏 60 秒**回收（日志关键字：`心跳超时（30 秒未回 pong），回收半开连接`）。
- 有"索引与实际不一致"→ 索引缓存和真实房间状态不同步，属于 bug，需要记录原始日志并复现。

**兜底处置**：`sudo systemctl restart webrtc-signaling`（会优雅通知所有客户端重连）。

### 3.10 `/stats` 里 `errors.rate_limited` 持续增长

**症状**：用户抱怨"发消息突然断开"。

**怎么判断**：单连接限流是 **60 条/秒、突发 120**（正常通话峰值约 10 条/秒）。持续增长说明有客户端在刷。

```bash
journalctl -u webrtc-signaling --since '10 min ago' | grep -c rate_limited
```

**处置**：正常前端不可能触发；确认是不是有人在写脚本压测，或是前端某处循环发消息（回归测试里加一条"空闲时不应发消息"的断言）。阈值在 `server.cpp` 的 `kRatePerSec`/`kRateBurst`。

### 3.11 `/stats` 里 `errors.slow_client` 增长

**症状**：少量用户被服务端主动断开。

**含义**：转发前发现这个连接**积压了 >256 KiB 未发出去的数据**——典型原因是客户端网络太差或页面卡死（不再读 socket）。服务端主动断开是保护其他人不被拖慢。

**处置**：确认数量很少（偶发属正常）；持续增长说明有客户端在"挂机不读"，查 nginx access log 里对应的 IP。

### 3.12 RSS 持续上涨

**症状**：`resources.rssMb` 从 25 涨到几百，最后被 OOM killer 杀掉。

**怎么判断**：

```bash
# 采样 2 分钟，看是否单调
for i in $(seq 1 8); do
  curl -s "http://127.0.0.1:8888/stats?token=$TOKEN" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["resources"], d["connections"])'
  sleep 15
done
```

- RSS 随连接数上升、连接归零后不回落 → **内存泄漏**（房间/连接对象没释放）。
- RSS 平稳但 `fds` 涨 → **fd 泄漏**。

**兜底**：`sudo systemctl restart webrtc-signaling`（清空状态）；同时把 `webrtc_server.*.log` 和这段时间的 `/stats` 输出保存下来排查。

### 3.13 服务起不来

**症状**：`systemctl restart` 后立刻 `failed`。

**怎么判断**：

```bash
systemctl status webrtc-signaling --no-pager -l
journalctl -u webrtc-signaling -n 50 --no-pager
```

**常见原因**：

| 日志关键字 | 根因 | 处置 |
| --- | --- | --- |
| `JWT_SECRET 长度不足` | `signaling.env` 里密钥 < 32 字节 | 换成长随机串（`openssl rand -base64 36`），**两个 env 都要改** |
| `bind: Address already in use` | 8888 被别的进程占了 | `sudo ss -lntp \| grep 8888`，杀掉或改端口（改端口要同步 nginx） |
| `无法加载 turn_config.json` | 文件权限/JSON 语法错 | `sudo -u webrtc cat /opt/webrtc/turn_config.json \| python3 -m json.tool` |
| 立刻退出、无日志 | 二进制不匹配架构或动态库缺失 | `ldd /opt/webrtc/server \| grep 'not found'` |

### 3.14 改了代码但行为没变

**症状**：部署了新版本，日志/功能还是旧的。

**根因（踩过）**：`systemctl enable --now` 对**已在运行**的服务是 no-op，只换了二进制、进程还是老的。

**怎么判断**：

```bash
systemctl show webrtc-signaling -p MainPID -p ActiveEnterTimestamp
ls -l --time-style=full-iso /opt/webrtc/server
```

**处置**：

```bash
sudo systemctl restart webrtc-signaling        # 必须用 restart，不是 enable --now
journalctl -u webrtc-signaling -n 20 --no-pager   # 确认启动横幅里的版本/配置是新的
```

### 3.15 需要回滚

见 §4.3。

---

## 4. 变更与回滚

### 4.1 发布前

```bash
# 1) 先备份数据库（改 schema 的发布必做）
sudo systemctl start webrtc-backup.service

# 2) 在本地跑一遍三层测试中的前两层（不需要浏览器）
node tools/e2e/ws-smoke.js 192.168.220.128 8888
node tools/e2e/e2e-fullstack.js 192.168.220.128
```

### 4.2 发布

```bash
# 信令（C++）：把新二进制放上去再重启
sudo install -m 0755 ./server /opt/webrtc/server
sudo systemctl restart webrtc-signaling       # 会先广播 server_shutdown，客户端 3 秒后自动重连

# 前端（纯静态）：不需要重启任何进程
sudo rsync -a --delete ./static/ /opt/webrtc/static/
sudo chown -R webrtc:webrtc /opt/webrtc/static
# 前端有缓存，验证时用 Ctrl+Shift+R 或带 ?v= 参数

# 认证（Go）：改了 schema 先跑迁移
sudo -u webrtc bash db/migrate.sh
sudo install -m 0755 ./auth /opt/webrtc/auth
sudo systemctl restart webrtc-auth
```

发布后**必须**验证：

```bash
curl -sk https://127.0.0.1/api/health                     # db=ok
node tools/e2e/ws-smoke.js 192.168.220.128 8888           # 36/36
node tools/e2e/e2e-fullstack.js 192.168.220.128           # 34/34
```

### 4.3 回滚

二进制回滚（最快的路）：发布前把旧文件留一份。

```bash
sudo cp -a /opt/webrtc/server /opt/webrtc/server.prev     # 发布前
# 出问题：
sudo install -m 0755 /opt/webrtc/server.prev /opt/webrtc/server
sudo systemctl restart webrtc-signaling
```

代码级回滚：

```bash
cd /path/to/WebRTC && git log --oneline -10
git checkout <上一个好版本> -- server.cpp static/
# 重新构建 → 部署 → 验证
```

数据库回滚：

- **只回滚代码、schema 兼容**：不用动库。
- **必须回滚 schema**：按 §2.4 从备份恢复（会丢备份之后的注册/房间数据）。

### 4.4 变更记录

每次发布在 `docs/ops-drills.md` 末尾的「变更记录」表里加一行：时间、改了什么、谁改的、验证结果、有没有回滚。

---

## 5. 证书

- 类型：**自签**（内网演示用）。SAN 由 `install.sh` 用 `ip -4 -o addr show scope global` 自动探测，包含 `DNS:localhost`、`IP:127.0.0.1`、本机 IP。
- 有效期 3 年，当前 `notAfter=Feb 21 10:06:53 2029 GMT`。
- 检查：`openssl x509 -in /etc/nginx/ssl/webrtc.crt -noout -enddate -ext subjectAltName`
- 换 IP 后**必须重签**（否则浏览器报证书不匹配），重跑 `install.sh` 即可。
- 正式上公网时换 Let's Encrypt（`certbot --nginx`），并把 `deploy/nginx-webrtc.conf` 里的证书路径改掉。

---

## 6. 容量

来自压测（详见 `docs/loadtest-report.md`）：

| 指标 | 实测值（4 vCPU / 4 GiB 虚机） |
| --- | --- |
| 稳定承载连接 | 2000（1000 房间）无错误 |
| 转发延迟 P95 / P99 | 0.1 ms / 0.25 ms |
| 入房延迟 P95 | 1 ms |
| CPU（2000 连接稳态） | 7% |
| 内存 | 约 4 KiB / 连接（2000 连接 ≈ 32 MB） |
| fd | 连接数 + ~22（`LimitNOFILE=65536`） |

**判断"该扩容了"的信号**：`latencyUs.forward.p95` 持续 > 2 ms、CPU 持续 > 60%、`errors.slow_client` 开始出现。

**先扩什么**：这台是单机，横向扩展要先做信令无状态化（房间状态迁到 Redis 或按房间分片到不同实例），设计见 `docs/ha-scaling-design.md`——**当前版本不支持多实例部署**，不要简单地在两台机器上各起一个然后加负载均衡，那样同房间的两个人可能被分到不同实例、互相看不见。

---

## 7. 演练与记录

所有演练（备份恢复、SIGKILL、数据库停机、长稳）的操作步骤与实测结果都在 **`docs/ops-drills.md`**。

建议每季度重跑一次：

```bash
bash tools/ops/restore-drill.sh 192.168.220.128
node tools/ops/pg-outage-drill.js 192.168.220.128 8888
node tools/e2e/graceful-restart.js 192.168.220.128 8888          # SIGTERM
node tools/e2e/graceful-restart.js 192.168.220.128 8888 --hard   # SIGKILL
```
