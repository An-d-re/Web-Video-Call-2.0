// 真实浏览器端到端：两个 Chromium 内核浏览器（Edge / Chrome）各用一个独立 profile，
// 打开 https://<host>/ 走完「注册 → 建房间 → 输密码加入 → 通话 → 挂断」，
// 再用 CDP 读页面里的 peerConn 状态、WebRTC 收发统计和 video 尺寸，确认媒体真的在流。
//
// 这一层补的是 e2e-fullstack.js 证明不了的东西：浏览器里的 getUserMedia、SDP 协商、
// ICE 打通、远端画面真的渲染出来了。之前就靠它抓出过一个"画面全黑但状态显示通话中"
// 的 bug：ontrack 里只设了 srcObject 没调 play()，带声音的自动播放被浏览器拦掉后
// 元素永远停在 paused。
//
// 用法：
//   node tools/e2e/browser-e2e.js [host]        # 默认 127.0.0.1
//   set BROWSER_HEADLESS=0                      # 想看真实窗口时关掉 headless
//   set EDGE_PATH=...                           # 非默认安装路径时指定可执行文件
//
// 依赖 ws（和 dev-gateway 共用）：tools/dev-gateway/node_modules 里已有的话会自动找到，
// 否则在仓库根跑一次 pnpm install 或 npm i ws --prefix tools/dev-gateway。
const { spawn, execSync } = require('child_process');
const http = require('http');
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..');
function requireWs() {
  try { return require('ws'); } catch (_) { /* 走下面的本地路径 */ }
  return require(path.join(ROOT, 'tools', 'dev-gateway', 'node_modules', 'ws'));
}
const WebSocket = requireWs();
const makeY4m = require('./make-y4m');

const HOST = process.argv[2] || '127.0.0.1';
const BASE = `https://${HOST}`;
const TMP = path.join(ROOT, '.tmp', 'e2e');
const PROFILES = path.join(TMP, 'profiles');
const FAKE_VIDEO = path.join(TMP, 'fake-camera.y4m');
const HEADLESS = process.env.BROWSER_HEADLESS !== '0';
// 强制走 TURN 中继：两端都只收集中继候选（iceTransportPolicy: 'relay'）。
// 用来证明"跨网段也能通话"——这是 P2P 直连测不出来的那一半。
const RELAY = process.argv.includes('--relay') || process.env.BROWSER_RELAY === '1';

const EDGE_CANDIDATES = [
  process.env.EDGE_PATH,
  'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
  'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/usr/bin/google-chrome',
  '/usr/bin/chromium',
  '/usr/bin/chromium-browser',
].filter(Boolean);
const EDGE = EDGE_CANDIDATES.find((p) => fs.existsSync(p));
if (!EDGE) {
  console.error('找不到浏览器可执行文件，可用 EDGE_PATH 环境变量指定（Chromium 内核即可）');
  process.exit(2);
}

let pass = 0, fail = 0;
const ok = (name, cond, extra) => {
  if (cond) { pass++; console.log('PASS  ' + name); }
  else { fail++; console.log('FAIL  ' + name + (extra !== undefined ? '  => ' + String(JSON.stringify(extra)).slice(0, 300) : '')); }
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------- 极简 CDP 客户端 ----------
class CDP {
  constructor(wsUrl) {
    this.ws = new WebSocket(wsUrl, { perMessageDeflate: false, maxPayload: 64 * 1024 * 1024 });
    this.seq = 0;
    this.pending = new Map();
    this.ready = new Promise((res, rej) => {
      this.ws.on('open', res);
      this.ws.on('error', rej);
    });
    this.ws.on('message', (raw) => {
      const msg = JSON.parse(raw.toString());
      if (msg.id && this.pending.has(msg.id)) {
        const { resolve, reject } = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        msg.error ? reject(new Error(msg.error.message)) : resolve(msg.result);
      }
    });
  }
  send(method, params = {}) {
    const id = ++this.seq;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ id, method, params }));
      setTimeout(() => {
        if (this.pending.has(id)) { this.pending.delete(id); reject(new Error('CDP 超时: ' + method)); }
      }, 30000);
    });
  }
  async evaluate(expr) {
    const r = await this.send('Runtime.evaluate', {
      expression: `(() => { try { return (${expr}); } catch (e) { return '__ERR__' + e.message; } })()`,
      returnByValue: true, awaitPromise: true,
    });
    if (r.exceptionDetails) return '__ERR__' + (r.exceptionDetails.text || '');
    return r.result && r.result.value;
  }
  close() { try { this.ws.close(); } catch (_) {} }
}

function getJson(url) {
  return new Promise((resolve, reject) => {
    http.get(url, (res) => {
      let b = '';
      res.on('data', (c) => (b += c));
      res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { reject(e); } });
    }).on('error', reject);
  });
}

async function launch(name, port) {
  const profile = path.join(PROFILES, name);
  // 每次跑都清掉旧 profile：残留的 refresh cookie 会让 /login.html 直接跳到主页，
  // 于是"登录页输入框"永远等不到；缓存也可能让旧 JS 继续跑。
  fs.rmSync(profile, { recursive: true, force: true });
  fs.mkdirSync(profile, { recursive: true });
  // 每个实例用自己的一份 y4m：同一份文件被两个 Chromium 同时打开时，后启动的那个
  // 拿不到视频（表现为 localStream 只有音频轨、senders 里 video 是 null）。
  const videoFile = path.join(TMP, `fake-camera-${name}.y4m`);
  fs.copyFileSync(FAKE_VIDEO, videoFile);
  const args = [
    `--remote-debugging-port=${port}`,
    `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check',
    '--ignore-certificate-errors', '--allow-insecure-localhost',
    // 系统代理会把内网 IP 的请求也套进去，导致页面白屏、拿不到 DOM。
    '--no-proxy-server',
    // --use-file-for-fake-video-capture 必须和 --use-fake-device-for-media-stream 配对：
    // 只给文件路径是没用的，Chromium 会退回去开真摄像头，第二个实例就报
    // NotReadableError: Device in use（一台机器只有一个摄像头）。
    `--use-file-for-fake-video-capture=${videoFile}`,
    '--use-fake-device-for-media-stream',
    '--use-fake-ui-for-media-stream',
    '--autoplay-policy=no-user-gesture-required',
    '--disable-features=Translate,MediaRouter',
    '--window-size=1024,768',
    'about:blank',
  ];
  if (HEADLESS) args.unshift('--headless=new');
  const child = spawn(EDGE, args, { detached: true, stdio: 'ignore' });
  child.unref();

  for (let i = 0; i < 60; i++) {
    try {
      const v = await getJson(`http://127.0.0.1:${port}/json/version`);
      if (v.webSocketDebuggerUrl) return { child, port, version: v.Browser };
    } catch (_) { /* 还没起 */ }
    await sleep(500);
  }
  throw new Error(`${name} 的调试端口 ${port} 没起来`);
}

async function attachPage(port) {
  let list = await getJson(`http://127.0.0.1:${port}/json/list`);
  let page = list.find((t) => t.type === 'page');
  if (!page) {
    await getJson(`http://127.0.0.1:${port}/json/new?about:blank`).catch(() => null);
    list = await getJson(`http://127.0.0.1:${port}/json/list`);
    page = list.find((t) => t.type === 'page');
  }
  const cdp = new CDP(page.webSocketDebuggerUrl);
  await cdp.ready;
  await cdp.send('Runtime.enable');
  await cdp.send('Page.enable');
  if (RELAY) {
    // 包一层 RTCPeerConnection，让页面自己建的连接一律只收集中继候选。
    // 必须在导航之前注入，否则页面脚本已经跑了。
    await cdp.send('Page.addScriptToEvaluateOnNewDocument', {
      source: `(() => {
        const Native = window.RTCPeerConnection;
        if (!Native) return;
        function Patched(config, ...rest) {
          const cfg = Object.assign({}, config || {});
          cfg.iceTransportPolicy = 'relay';
          return new Native(cfg, ...rest);
        }
        Patched.prototype = Native.prototype;
        Patched.generateCertificate = Native.generateCertificate;
        window.RTCPeerConnection = Patched;
        window.__relayForced = true;
      })();`,
    });
  }
  // headless 里没有真实的屏幕选择器，getDisplayMedia 会直接 reject，测不到页面自己的逻辑。
  // 这里把它换成 getUserMedia（同样产出一条真实视频轨），用来验证「共享/停止共享」的
  // 状态流转与 replaceTrack 行为；真实屏幕采集属于浏览器能力，交给人工演示验证。
  await cdp.send('Page.addScriptToEvaluateOnNewDocument', {
    source: `(() => {
      if (!navigator.mediaDevices) return;
      navigator.mediaDevices.getDisplayMedia = function () {
        window.__displayCalls = (window.__displayCalls || 0) + 1;
        return navigator.mediaDevices.getUserMedia({ video: true });
      };
    })();`,
  });
  return cdp;
}

async function nav(cdp, url) {
  await cdp.send('Page.navigate', { url });
  for (let i = 0; i < 60; i++) {
    await sleep(250);
    const st = await cdp.evaluate('document.readyState');
    if (st === 'complete') return;
  }
}

async function waitFor(cdp, expr, ms, label) {
  const until = Date.now() + ms;
  let last;
  while (Date.now() < until) {
    last = await cdp.evaluate(expr);
    if (last && String(last).indexOf('__ERR__') !== 0) return last;
    await sleep(300);
  }
  throw new Error(`等不到「${label}」，最后取值: ${JSON.stringify(last)}（表达式: ${expr}）`);
}

// 直接改 value 不会触发框架事件，但本项目是原生 JS + onclick，够用；
// 为了保险再补 input/change 事件。
const setVal = (cdp, sel, val) => cdp.evaluate(`(() => {
  const el = document.querySelector(${JSON.stringify(sel)});
  if (!el) return 'NO_EL';
  el.value = ${JSON.stringify(val)};
  el.dispatchEvent(new Event('input', { bubbles: true }));
  el.dispatchEvent(new Event('change', { bubbles: true }));
  return el.value;
})()`);

const click = (cdp, sel) => cdp.evaluate(`(() => {
  const el = document.querySelector(${JSON.stringify(sel)});
  if (!el) return 'NO_EL';
  if (el.disabled) return 'DISABLED';
  el.click();
  return 'clicked';
})()`);

async function loginFlow(cdp, user, pass, displayName) {
  await nav(cdp, `${BASE}/login.html`);
  await waitFor(cdp, "!!document.querySelector('#username')", 10000, '登录页输入框');
  // 注册有 IP 限流（5 次/分钟、突发 3，见 auth/server.go 的 registerIP）。本脚本每跑一遍
  // 注册两个号，连着跑就会撞上"注册过于频繁"，所以撞到就等窗口过去再重试，而不是直接判失败。
  for (let attempt = 0; attempt < 4; attempt++) {
    if (displayName) {
      await click(cdp, '#tabRegister');
      await sleep(200);
      await setVal(cdp, '#displayName', displayName);
    }
    await setVal(cdp, '#username', user);
    await setVal(cdp, '#password', pass);
    await click(cdp, '#submitBtn');
    try {
      await waitFor(cdp, "/(^\\/$|index\\.html$)/.test(location.pathname) && !!document.querySelector('#roomList')", 20000, '登录后跳主页');
      return;
    } catch (e) {
      const msg = await cdp.evaluate("(document.querySelector('#msg') || {}).textContent || '(无提示)'");
      if (/频繁|稍后|too many/i.test(String(msg))) {
        console.log(`   （注册限流：${msg}，等 62 秒重试）`);
        await sleep(62000);
        await nav(cdp, `${BASE}/login.html`);
        await waitFor(cdp, "!!document.querySelector('#username')", 10000, '登录页输入框');
        continue;
      }
      const href = await cdp.evaluate('location.href');
      throw new Error(`${e.message}｜页面提示: ${msg}｜当前地址: ${href}`);
    }
  }
  throw new Error('注册重试多次仍被限流，请稍后再跑');
}

(async () => {
  const suffix = Date.now().toString(36).slice(-5);
  const uA = 'alice_' + suffix, uB = 'bob_' + suffix;
  const PASS = 'Passw0rd!23';

  console.log(`== 真实浏览器 e2e：${BASE}（${HEADLESS ? 'headless' : '有界面'}${RELAY ? '，强制 TURN 中继' : '，P2P 直连'}）==`);
  if (!fs.existsSync(FAKE_VIDEO)) makeY4m(FAKE_VIDEO);

  const a = await launch('peerA', 9222);
  const b = await launch('peerB', 9223);
  console.log(`   两个浏览器已启动：${a.version} / ${b.version}`);
  const pa = await attachPage(9222);
  const pb = await attachPage(9223);

  let crashed = null;
  try {
    console.log('\n-- 登录/注册（真实表单提交）--');
    await loginFlow(pa, uA, PASS, '爱丽丝');
    ok('A 注册并登录后进入主页', /(^\/$|index\.html$)/.test(await pa.evaluate('location.pathname')));
    await loginFlow(pb, uB, PASS, '鲍勃');
    ok('B 注册并登录后进入主页', /(^\/$|index\.html$)/.test(await pb.evaluate('location.pathname')));

    const userA = await pa.evaluate('Auth && Auth.user && Auth.user.username');
    const userB = await pb.evaluate('Auth && Auth.user && Auth.user.username');
    ok('页面里 Auth.user 是当前账号', userA === uA && userB === uB, { userA, userB });

    console.log('\n-- 建房间 / 加入房间 --');
    await click(pa, '#newRoomBtn');
    await waitFor(pa, "document.querySelector('#createMask').classList.contains('show')", 5000, '新建房间弹窗');
    const title = '浏览器实测' + suffix;
    await setVal(pa, '#newTitle', title);
    await setVal(pa, '#newPassword', '1234');
    await click(pa, '#createConfirm');
    const roomId = await waitFor(pa, "typeof currentRoomId === 'string' && currentRoomId || null", 15000, '房间创建成功');
    ok('A 创建房间并进入（房间号 ' + roomId + '）', !!roomId);
    ok('A 侧边栏出现自己的房间', await waitFor(pa,
      `[...document.querySelectorAll('#roomList .room-item')].some(el => el.textContent.includes(${JSON.stringify(title)}))`,
      8000, '侧边栏列表'));

    // 本项目是「按需开启设备」：进房不会自动开摄像头/麦克风，必须点控制按钮。
    // 这本身是个好设计（不会一进页面就抢摄像头），测试里照样点一遍。
    await click(pa, '#videoBtn');
    await click(pa, '#micBtn');
    ok('A 点「开启摄像头」后 getUserMedia 成功',
      await waitFor(pa, "localStream && localStream.getVideoTracks().length > 0", 15000, 'localStream.video'));
    ok('A 点「开启麦克风」后拿到音频轨',
      await waitFor(pa, "localStream && localStream.getAudioTracks().length > 0", 15000, 'localStream.audio'));
    ok('A 的 <video> 有画面',
      await waitFor(pa, "document.querySelector('#localVideo').videoWidth > 0", 15000, 'localVideo.videoWidth'));

    await setVal(pb, '#roomIdInput', roomId);
    await click(pb, '#joinBtn');
    await waitFor(pb, "document.querySelector('#pwdMask').classList.contains('show')", 8000, '密码弹窗（说明房间真的加密了）');
    ok('加密房间在 B 侧弹出了密码框', true);
    await setVal(pb, '#pwdInput', '1234');
    await click(pb, '#pwdConfirm');
    await waitFor(pb, "typeof currentRoomId === 'string' && currentRoomId === " + JSON.stringify(roomId), 15000, 'B 进入同一房间');
    ok('B 输密码后进入同一房间', true);

    await click(pb, '#videoBtn');
    await click(pb, '#micBtn');
    ok('B 也拿到了本地音视频',
      await waitFor(pb, "localStream && localStream.getVideoTracks().length > 0 && localStream.getAudioTracks().length > 0", 15000, 'B localStream'));

    console.log('\n-- 通话建立（真实 SDP/ICE）--');
    const connA = await waitFor(pa, "peerConn && peerConn.connectionState", 10000, 'A 的 peerConn');
    ok('A 创建了 RTCPeerConnection', !!connA, connA);
    if (RELAY) {
      ok('强制中继已生效（页面里的 RTCPeerConnection 被包了一层）',
        (await pa.evaluate('window.__relayForced')) === true &&
        (await pb.evaluate('window.__relayForced')) === true);
    }
    ok(RELAY ? 'A 的 ICE 进入 connected（强制中继）' : 'A 的 ICE 进入 connected（P2P 打通，没走 TURN）',
      await waitFor(pa, "peerConn.iceConnectionState === 'connected' || peerConn.iceConnectionState === 'completed'", 30000, 'A ice'));
    ok('B 的 ICE 进入 connected',
      await waitFor(pb, "peerConn.iceConnectionState === 'connected' || peerConn.iceConnectionState === 'completed'", 30000, 'B ice'));
    ok('A 的 connectionState 是 connected', (await pa.evaluate('peerConn.connectionState')) === 'connected');
    ok('B 的 connectionState 是 connected', (await pb.evaluate('peerConn.connectionState')) === 'connected');

    if (RELAY) {
      // 只看 iceConnectionState 证明不了中继：直连也会 connected。
      // 必须看被选中（selected/nominated）的候选对里，本地候选是不是 relay 类型。
      const pairTypes = `(async () => {
        const st = await peerConn.getStats();
        let pair = null;
        st.forEach((s) => {
          if (s.type === 'candidate-pair' && (s.selected || (s.nominated && s.state === 'succeeded'))) pair = s;
        });
        if (!pair) return 'no-pair';
        const l = st.get(pair.localCandidateId), r = st.get(pair.remoteCandidateId);
        return JSON.stringify({
          local: l && l.candidateType, remote: r && r.candidateType,
          proto: l && l.protocol, relayProto: l && l.relayProtocol,
          bytesSent: pair.bytesSent, bytesReceived: pair.bytesReceived,
        });
      })()`;
      const ptA = await waitFor(pa, pairTypes, 20000, 'A 的候选对');
      const ptB = await waitFor(pb, pairTypes, 20000, 'B 的候选对');
      ok('A 的候选对本地是中继（媒体经 TURN 转发）', /"local":"relay"/.test(String(ptA)), ptA);
      ok('B 的候选对本地是中继（媒体经 TURN 转发）', /"local":"relay"/.test(String(ptB)), ptB);
    }

    // 远端画面的两条路径都要认：
    //   a) 浏览器允许自动播放  → remoteVideo 直接有画面
    //   b) 浏览器拦了带声音的自动播放 → 页面静音兜底播起来 + 显示「点一下开启声音」
    // 之前页面只写 srcObject 不 play()，b) 情况下永远黑屏，所以这里专门覆盖。
    const aPath = await waitFor(pa,
      "document.querySelector('#unmuteBtn').classList.contains('show') ? 'needs-tap' : (document.querySelector('#remoteVideo').videoWidth > 0 ? 'playing' : null)",
      25000, 'A 的远端画面（或静音兜底提示）');
    ok('A 端要么直接看到画面，要么给出「点一下开启声音」兜底', aPath === 'playing' || aPath === 'needs-tap', aPath);
    if (aPath === 'needs-tap') {
      ok('静音兜底确实让 A 的画面动起来了',
        await waitFor(pa, "document.querySelector('#remoteVideo').videoWidth > 0", 15000, 'A 静音播放'));
      ok('兜底状态下 remoteVideo 是静音的', (await pa.evaluate("document.querySelector('#remoteVideo').muted")) === true);
      await click(pa, '#unmuteBtn');
      ok('点一下后恢复声音且按钮消失',
        await waitFor(pa, "document.querySelector('#remoteVideo').muted === false && !document.querySelector('#unmuteBtn').classList.contains('show')", 8000, 'A 恢复声音'));
    } else {
      ok('A 直接自动播放，且不是静音状态', (await pa.evaluate("document.querySelector('#remoteVideo').muted")) === false);
    }
    ok('B 看到对端画面（remoteVideo.videoWidth > 0）',
      await waitFor(pb, "document.querySelector('#remoteVideo').videoWidth > 0", 20000, 'B remoteVideo'));

    const rvA = await pa.evaluate("({w: document.querySelector('#remoteVideo').videoWidth, h: document.querySelector('#remoteVideo').videoHeight, t: document.querySelector('#remoteVideo').currentTime})");
    ok('A 收到的远端画面是有效分辨率', rvA && rvA.w >= 320 && rvA.h >= 240, rvA);

    console.log('\n-- 屏幕共享（替换视频源，不重新协商）--');
    // 关键论点：共享走 videoSender.replaceTrack，不新增轨道，所以 SDP 不用重协商，
    // 对方画面不会中断。这里用 signalingState 和 track id 两个侧面来验证。
    const sigBefore = await pa.evaluate('peerConn.signalingState');
    const trackBefore = await pa.evaluate("peerConn.getSenders().find(s => s.track && s.track.kind === 'video').track.id");
    await click(pa, '#screenBtn');
    ok('A 点「共享屏幕」后进入共享态（本地角标亮起）',
      await waitFor(pa, "screenSharing === true && document.querySelector('#localScreenBadge').classList.contains('active')", 15000, 'A 共享态'));
    ok('共享时摄像头按钮被禁用、按钮文案变成「停止共享」',
      (await pa.evaluate("document.querySelector('#videoBtn').disabled === true && document.querySelector('#screenLabel').textContent === '停止共享'")) === true);
    ok('共享时本地预览换成了屏幕轨（不再是摄像头那条）',
      await waitFor(pa, `(() => { const s = document.querySelector('#localVideo').srcObject; return !!(s && s.getVideoTracks()[0] && s.getVideoTracks()[0].id !== ${JSON.stringify(trackBefore)}); })()`,
        10000, '本地预览切换'));
    ok('共享没有触发重新协商（signalingState 保持 stable）',
      (await pa.evaluate('peerConn.signalingState')) === sigBefore, sigBefore);
    ok('对端收到 screen 标记并显示「对方正在共享屏幕」',
      await waitFor(pb, "remoteScreenSharing === true && document.querySelector('#remoteScreenBadge').classList.contains('active')", 15000, 'B 共享角标'));
    ok('共享期间对端画面仍在（没有被切断）',
      await waitFor(pb, "document.querySelector('#remoteVideo').videoWidth > 0", 15000, 'B 共享中画面'));

    await click(pa, '#screenBtn');
    ok('A 点「停止共享」后回到摄像头态',
      await waitFor(pa, "screenSharing === false && !document.querySelector('#localScreenBadge').classList.contains('active')", 10000, 'A 停止共享'));
    ok('停止共享后摄像头按钮恢复可用、本地预览回到摄像头轨',
      await waitFor(pa, `(() => { const s = document.querySelector('#localVideo').srcObject; return document.querySelector('#videoBtn').disabled === false && !!(s && s.getVideoTracks()[0] && s.getVideoTracks()[0].id === ${JSON.stringify(trackBefore)}); })()`,
        12000, 'A 恢复摄像头'));
    ok('B 侧「对方正在共享屏幕」角标消失',
      await waitFor(pb, "remoteScreenSharing === false && !document.querySelector('#remoteScreenBadge').classList.contains('active')", 10000, 'B 角标消失'));
    ok('停止共享同样没有重新协商',
      (await pa.evaluate('peerConn.signalingState')) === sigBefore);
    ok('整个过程只调用了一次 getDisplayMedia（没有重复采集）',
      (await pa.evaluate('window.__displayCalls')) === 1);

    console.log('\n-- 挂断 --');
    await click(pa, '#hangupBtn');
    const bSawBye = await waitFor(pb, "/已挂断|对方已离开|通话已结束|等待加入/.test(document.querySelector('#status').textContent)", 10000, 'B 感知到挂断');
    ok('A 挂断后 B 的状态发生变化（bye 走通）', !!bSawBye);
    ok('B 的 peerConn 已关闭/断开', ['closed', 'disconnected', 'failed'].includes(await pb.evaluate("peerConn ? peerConn.connectionState : 'closed'")));

    const errs = await pa.evaluate("JSON.stringify(window.__pageErrors || [])");
    console.log('   页面 JS 错误（A）: ' + errs);
  } catch (e) {
    crashed = e;
    console.error('\n运行中断: ' + e.message);
    // 媒体不通时最需要知道的是「轨道到底有没有、收没收到字节」：
    // 只看 videoWidth 分不清"没协商上"、"协商上了但没发"、"发了但没解码"。
    for (const [tag, p] of [['A', pa], ['B', pb]]) {
      try {
        const d = await p.evaluate(`(async () => {
          const v = document.querySelector('#remoteVideo');
          const l = document.querySelector('#localVideo');
          const st = peerConn ? await peerConn.getStats() : null;
          const flows = [];
          if (st) st.forEach((s) => {
            if (s.type === 'inbound-rtp' || s.type === 'outbound-rtp') {
              flows.push(s.type.slice(0, 3) + '-' + s.kind + ':' + (s.type === 'inbound-rtp'
                ? 'rx=' + (s.bytesReceived || 0) + ',dec=' + (s.framesDecoded || 0)
                : 'tx=' + (s.bytesSent || 0) + ',enc=' + (s.framesEncoded || 0)));
            }
          });
          return JSON.stringify({
            paused: v.paused, remoteW: v.videoWidth, localW: l.videoWidth,
            srcTracks: v.srcObject ? v.srcObject.getTracks().map(t => t.kind + ':' + t.readyState + (t.muted ? ':muted' : '')) : null,
            senders: peerConn ? peerConn.getSenders().map(s => s.track ? s.track.kind + ':' + s.track.enabled : 'null') : null,
            receivers: peerConn ? peerConn.getReceivers().map(r => r.track.kind + ':' + r.track.readyState + (r.track.muted ? ':muted' : '')) : null,
            flows,
            signaling: peerConn ? peerConn.signalingState : null,
            ice: peerConn ? peerConn.iceConnectionState : null,
            transceivers: peerConn ? peerConn.getTransceivers().map(t => (t.mid || '?') + ':' + t.direction + ':' + (t.currentDirection || '-')) : null,
            status: (document.querySelector('#status') || {}).textContent,
            trackFlags: { videoStarted: typeof videoStarted !== 'undefined' ? videoStarted : null,
                          micStarted: typeof micStarted !== 'undefined' ? micStarted : null },
            // 直接再问一次设备：区分「页面逻辑没加轨」和「这个浏览器实例根本拿不到摄像头」
            camera: await navigator.mediaDevices.getUserMedia({ video: true })
              .then(s => { const n = s.getVideoTracks().length; s.getTracks().forEach(t => t.stop()); return 'ok:' + n; })
              .catch(e => 'err:' + e.name + ':' + e.message),
          });
        })()`);
        console.log(`   [${tag}] ${d}`);
      } catch (e2) { console.log(`   [${tag}] 诊断失败: ${e2.message}`); }
    }
  } finally {
    pa.close(); pb.close();
    // 必须真的把浏览器杀掉：headless 的浏览器不会随 node 退出，残留进程会一直占着
    // 9222/9223，下次启动的新实例拿不到端口，测试就会莫名接到「上一轮的旧页面」上。
    for (const pid of [a.child.pid, b.child.pid]) {
      try { execSync(`taskkill /PID ${pid} /T /F`, { stdio: 'ignore' }); } catch (_) {
        try { execSync(`kill -9 ${pid}`, { stdio: 'ignore' }); } catch (_) {}
      }
    }
    console.log(`\n合计: ${pass} 通过, ${fail} 失败`);
    if (crashed) { console.log('（用例没跑完，上面的「运行中断」是真正的失败原因）'); process.exit(2); }
    process.exit(fail ? 1 : 0);
  }
})().catch((e) => {
  console.error('\n测试崩溃: ' + e.message);
  console.log(`合计: ${pass} 通过, ${fail} 失败`);
  process.exit(2);
});
