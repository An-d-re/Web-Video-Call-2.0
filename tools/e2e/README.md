# e2e 测试

三层测试，从「协议」到「接线」到「真实浏览器」，越往后越接近用户看到的东西：

| 脚本 | 验证什么 | 依赖 | 需要浏览器 |
| --- | --- | --- | --- |
| `ws-smoke.js` | C++ 信令服务的协议状态机：鉴权超时、凭证类型隔离、座位分配、offer/answer/bye 转发、空房间回收 | 只需能连到信令端口 | 否 |
| `e2e-fullstack.js` | 整条链路的接线：nginx 反代、Go 认证服务 + PostgreSQL、wss 升级、建房/密码/解散 | 已按 `deploy/install.sh` 部署 | 否 |
| `browser-e2e.js` | 浏览器里真的能打通：getUserMedia、SDP/ICE 协商、远端画面渲染、自动播放兜底、挂断 | 两个 Chromium 内核浏览器 + 完整部署 | 是 |

三者互补，不能互相替代：`ws-smoke` 通过只说明信令状态机对，`browser-e2e` 才说明媒体真的流起来了。
历史上「远端画面全黑但页面显示通话中」这个 bug 就是 `browser-e2e` 抓出来的，前两层全绿。

## 准备

脚本用 `ws` 库，复用 dev-gateway 里已装好的那份，所以不用额外安装：

```bash
pnpm install                      # 或者在 tools/dev-gateway 下 npm install
```

`browser-e2e.js` 需要 Chromium 内核浏览器，按顺序找 `EDGE_PATH` 环境变量、Edge 默认安装路径、
macOS/Linux 的 Chrome/Chromium。找不到会直接报错退出（退出码 2），不会静默跳过。

## 跑

```bash
# 1) 信令协议冒烟（默认 127.0.0.1:8888）
node tools/e2e/ws-smoke.js 192.168.220.128 8888

# 2) 全栈接线（默认 127.0.0.1:443）
node tools/e2e/e2e-fullstack.js 192.168.220.128

# 3) 真实浏览器通话（默认 127.0.0.1，用自签证书所以要放宽 TLS）
node tools/e2e/browser-e2e.js 192.168.220.128
```

## 环境变量

| 变量 | 用在哪 | 说明 |
| --- | --- | --- |
| `JWT_SECRET` | `ws-smoke.js` | 必须和服务端一致。**部署后的服务用的是 `install.sh` 随机生成的密钥**，见 `/opt/webrtc/signaling.env`；不设则用开发默认值，值不对会看到「自签 token 被拒」的连锁失败。 |
| `EDGE_PATH` | `browser-e2e.js` | 指定浏览器可执行文件（Chromium 内核）。 |
| `BROWSER_HEADLESS=0` | `browser-e2e.js` | 打开真实窗口，方便肉眼看。默认 headless。 |
| `NODE_TLS_REJECT_UNAUTHORIZED=0` | `e2e-fullstack.js` | 自签证书时放宽校验（脚本内部请求也传了 `rejectUnauthorized: false`，一般不用设）。 |

## 几个坑，写在这里省得再踩

- **注册限流是真的**：`auth/server.go` 里 `registerIP` 是 5 次/分钟、突发 3。连着跑两遍 e2e 就会 429，
  两个脚本都做了自动等待重试（`e2e-fullstack` 等 16 秒/轮，`browser-e2e` 撞到限流等 62 秒）。
- **假摄像头必须成对配参数**：`--use-file-for-fake-video-capture=<file>` 单独给是没用的，还要有
  `--use-fake-device-for-media-stream`，否则 Chromium 退回去开真摄像头，第二个实例会
  `NotReadableError: Device in use`。
- **每个浏览器实例用自己那份 y4m**：两个 Chromium 同时打开同一个文件时，后启动的拿不到视频。
  脚本里会复制成 `fake-camera-<name>.y4m`。
- **为什么不用内置图案设备**：headless 下 `--use-fake-device-for-media-stream` 的图案会间歇性不产帧，
  症状是"一边有画面一边全黑、每轮换边"，看着像协商 bug 其实是采集源没帧。改用确定性 y4m
  （`tools/e2e/make-y4m.js` 生成，30 帧移动方块）后稳定复现。
- **Edge 必须给足权限跑**：受限沙箱里 Edge 报 `mojo ... Check failed ... 拒绝访问 (0x5)`，
  DevTools 说 listening 但连不上。
- **`--no-proxy-server` 不能少**：宿主系统代理会把内网 IP 的请求也套进去，页面白屏。
- **每次跑前清 profile**：`--user-data-dir` 里残留的 refresh cookie 会让 `/login.html` 直接跳主页，
  于是等不到登录框。脚本已自动清理。
- **跑完要杀浏览器**：headless 浏览器不随 node 退出，残留进程占着调试端口，下次会接到上一轮的旧页面。
