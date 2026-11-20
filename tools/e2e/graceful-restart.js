// 优雅下线测试：验证 systemctl restart 时客户端能"被告知"而不是被无声掐断。
//
// 为什么要单独一个脚本：这件事只有真的重启一次服务才测得出来，而且它会
// 杀掉正在跑的服务，不能塞进 ws-smoke.js（那套用例是只读的、可以随时跑）。
//
// 用例：
//   1. A/B 两个连接入同一房间，拿到 start_call（确保真的在通话状态）
//   2. 重启服务端 -> 双方都应收到 {type:"server_shutdown", retryAfterMs:3000}
//   3. 连接随后被关闭（而不是一直挂着）
//   4. 按服务端给的建议延迟重连，能正常鉴权（说明它确实回来了）
//   5. /stats 的 uptime 归零、房间数为 0（说明进程真的重启了、没留残留房间）
//
// 用法（二选一）：
//   node tools/e2e/graceful-restart.js 192.168.220.128 8888
//   RESTART_CMD="ssh ... systemctl restart webrtc-signaling" node tools/e2e/graceful-restart.js
//
// 需要的环境变量：
//   JWT_SECRET     与服务端一致（部署环境在 /opt/webrtc/signaling.env）
//   STATS_TOKEN    与服务端一致（同上；不留就跳过 /stats 相关断言）
//   RESTART_CMD    或者 SSH_KEY + SSH_PASSWORD（配 SSH_USER / SSH_HOST）
const crypto = require('crypto');
const http = require('http');
const path = require('path');
const { execSync } = require('child_process');

const ROOT = path.join(__dirname, '..', '..');
function requireWs() {
  try { return require('ws'); } catch (_) { /* 走下面的本地路径 */ }
  return require(path.join(ROOT, 'tools', 'dev-gateway', 'node_modules', 'ws'));
}
const WebSocket = requireWs();

const HOST = process.argv[2] || '127.0.0.1';
const PORT = Number(process.argv[3] || 8888);
const SECRET = process.env.JWT_SECRET || 'dev-secret-at-least-32-bytes-long-0001';
const STATS_TOKEN = process.env.STATS_TOKEN || '';

let pass = 0, fail = 0, skipped = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log('PASS  ' + name); }
  else { fail++; console.log('FAIL  ' + name + (extra ? '  => ' + JSON.stringify(extra) : '')); }
}
function skip(name, why) {
  skipped++;
  console.log('SKIP  ' + name + '  (' + why + ')');
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const b64u = (buf) => Buffer.from(buf).toString('base64')
  .replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
function sign(claims) {
  const header = b64u(JSON.stringify({ alg: 'HS256', typ: 'JWT' }));
  const payload = b64u(JSON.stringify(claims));
  const data = header + '.' + payload;
  return data + '.' + b64u(crypto.createHmac('sha256', SECRET).update(data).digest());
}
const now = () => Math.floor(Date.now() / 1000);
const accessToken = (id, name) => sign({
  sub: id, username: name, typ: 'access', iat: now(), exp: now() + 3600, jti: 'j' + id,
});
const roomToken = (id, name, room) => sign({
  sub: id, username: name, typ: 'room', room, iat: now(), exp: now() + 600, jti: 'r' + id,
});

class Client {
  constructor(tag) {
    this.tag = tag;
    this.msgs = [];
    this.waiters = [];
    this.closed = false;
  }
  connect() {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(`ws://${HOST}:${PORT}/ws`);
      this.ws.on('open', () => resolve(this));
      this.ws.on('error', reject);
      this.ws.on('close', () => { this.closed = true; this._drain(); });
      this.ws.on('message', (raw) => {
        let m;
        try { m = JSON.parse(raw.toString()); } catch (e) { m = { raw: raw.toString() }; }
        this.msgs.push(m);
        this._drain();
      });
    });
  }
  _drain() {
    for (let i = 0; i < this.waiters.length; i++) {
      const w = this.waiters[i];
      const idx = this.msgs.findIndex((m) => w.pred(m));
      if (idx >= 0) { this.waiters.splice(i, 1); i--; w.resolve(this.msgs.splice(idx, 1)[0]); }
      else if (w.onClose && this.closed) { this.waiters.splice(i, 1); i--; w.resolve(null); }
    }
  }
  wait(pred, ms = 4000, onClose = false) {
    return new Promise((resolve) => {
      const idx = this.msgs.findIndex(pred);
      if (idx >= 0) return resolve(this.msgs.splice(idx, 1)[0]);
      this.waiters.push({ pred, resolve, onClose });
      setTimeout(() => {
        const i = this.waiters.findIndex((w) => w.resolve === resolve);
        if (i >= 0) { this.waiters.splice(i, 1); resolve(undefined); }
      }, ms);
    });
  }
  send(obj) { this.ws.send(JSON.stringify(obj)); }
  close() { try { this.ws.close(); } catch (e) { /* 忽略 */ } }
}

const isType = (t) => (m) => m.type === t;

function get(p) {
  return new Promise((resolve) => {
    const req = http.get({ host: HOST, port: PORT, path: p }, (res) => {
      let body = '';
      res.on('data', (c) => (body += c));
      res.on('end', () => resolve({ status: res.statusCode, body }));
    });
    req.on('error', (e) => resolve({ status: 0, body: String(e) }));
    req.setTimeout(5000, () => { req.destroy(); resolve({ status: 0, body: 'timeout' }); });
  });
}

// 触发一次真实重启：优先用 RESTART_CMD（最灵活），否则走 ssh + sudo。
function restartServer() {
  if (process.env.RESTART_CMD) {
    execSync(process.env.RESTART_CMD, { stdio: 'inherit' });
    return;
  }
  const key = process.env.SSH_KEY;
  const pw = process.env.SSH_PASSWORD;
  const user = process.env.SSH_USER || 'andres';
  const sshHost = process.env.SSH_HOST || HOST;
  if (!key || !pw) {
    throw new Error('需要 RESTART_CMD，或同时提供 SSH_KEY + SSH_PASSWORD（见文件头用法）');
  }
  const remote = `echo '${pw}' | sudo -S -p '' systemctl restart webrtc-signaling`;
  execSync(
    `ssh -i "${key}" -o StrictHostKeyChecking=accept-new ${user}@${sshHost} "${remote}"`,
    { stdio: 'inherit' }
  );
}

(async () => {
  console.log(`== 目标 ${HOST}:${PORT} ==`);
  const room = 'RST' + Math.random().toString(36).slice(2, 8).toUpperCase();

  // 1. 建两个连接并进同一房间，先确认"真的在通话"
  const alice = new Client('alice');
  const bob = new Client('bob');
  await alice.connect();
  alice.send({ type: 'auth', token: accessToken(201, 'alice') });
  const aok = await alice.wait(isType('auth_ok'));
  if (!aok) {
    console.log('  ！鉴权失败：请确认 JWT_SECRET 与服务端一致（部署环境看 /opt/webrtc/signaling.env）');
    process.exit(2);
  }
  await bob.connect();
  bob.send({ type: 'auth', token: accessToken(202, 'bob') });
  await bob.wait(isType('auth_ok'));

  alice.send({ type: 'join_room', roomToken: roomToken(201, 'alice', room) });
  await alice.wait(isType('joined'));
  bob.send({ type: 'join_room', roomToken: roomToken(202, 'bob', room) });
  await bob.wait(isType('joined'));
  const s1 = await alice.wait(isType('start_call'), 6000);
  ok('重启前双方已在通话中（收到 start_call）', !!s1, s1);

  // 2. 重启服务端，等 server_shutdown
  console.log('==> 重启服务端（systemctl restart webrtc-signaling）');
  try {
    restartServer();
  } catch (e) {
    console.log('重启失败: ' + e.message);
    process.exit(2);
  }

  const sd1 = await alice.wait(isType('server_shutdown'), 12000, true);
  const sd2 = await bob.wait(isType('server_shutdown'), 12000, true);
  ok('重启时客户端收到 server_shutdown（而不是被无声掐断）', !!sd1 && !!sd2, { alice: sd1, bob: sd2 });
  ok('server_shutdown 带 reason=restart 与 retryAfterMs',
    !!sd1 && sd1.reason === 'restart' && Number(sd1.retryAfterMs) >= 1000,
    sd1);

  // 3. 连接应被关闭
  await sleep(1200);
  ok('收到通知后连接被关闭', alice.closed === true && bob.closed === true,
    { alice: alice.closed, bob: bob.closed });

  const waitMs = (sd1 && Number(sd1.retryAfterMs)) || 3000;

  // 4. 按服务端建议的延迟重连
  await sleep(waitMs + 500);
  let reconnected = false;
  for (let i = 0; i < 20 && !reconnected; i++) {
    const probe = new Client('probe');
    try {
      await probe.connect();
      probe.send({ type: 'auth', token: accessToken(203, 'probe') });
      const okMsg = await probe.wait(isType('auth_ok'), 3000, true);
      if (okMsg) {
        reconnected = true;
        probe.close();
      } else {
        probe.close();
        await sleep(500);
      }
    } catch (e) {
      await sleep(500);
    }
  }
  ok(`按 retryAfterMs=${waitMs} 重连成功`, reconnected);

  // 5. 进程确实重启了：uptime 很小，且没有残留房间
  if (!STATS_TOKEN) {
    skip('/stats 显示 uptime 归零、房间数归零', '未设置 STATS_TOKEN，跳过');
  } else {
    const st = await get('/stats?token=' + encodeURIComponent(STATS_TOKEN));
    let j = null;
    try { j = JSON.parse(st.body); } catch (e) { /* 保持 null */ }
    ok('/stats 显示 uptime 归零、房间数归零',
      !!j && j.uptimeSeconds < 120 && j.rooms && j.rooms.total === 0,
      j ? { uptime: j.uptimeSeconds, rooms: j.rooms } : st.body.slice(0, 120));
  }

  alice.close();
  bob.close();

  console.log(`\n合计: ${pass} 通过, ${fail} 失败${skipped ? ', ' + skipped + ' 跳过' : ''}`);
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.error('测试崩溃:', e); process.exit(2); });
