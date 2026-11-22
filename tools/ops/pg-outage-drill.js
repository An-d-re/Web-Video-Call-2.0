// 数据库停机演练：证明"认证服务挂了，正在通话的人不会掉线"
//
// 为什么这条值得单独测：
//   信令服务（C++）只验 JWT 签名，不碰数据库；账号与房间在 Go 服务 + PostgreSQL 里。
//   所以把 PostgreSQL 停掉，正在通话的两个人应该完全无感 —— 这是"把认证拆成独立服务"
//   这个设计决定的直接收益。如果这条测挂了，说明信令偷偷依赖了数据库。
//
// 用法：
//   JWT_SECRET=... node tools/ops/pg-outage-drill.js 192.168.220.128 8888
//   需要能停/起数据库，二选一：
//     PG_STOP_CMD / PG_START_CMD      直接给 shell 命令（最灵活）
//     SSH_KEY + SSH_PASSWORD (+SSH_USER/SSH_HOST)   走 ssh + sudo
const crypto = require('crypto');
const https = require('https');
const http = require('http');
const path = require('path');
const { execSync } = require('child_process');

const ROOT = path.join(__dirname, '..', '..');
function requireWs() {
  try { return require('ws'); } catch (e) { return require(path.join(ROOT, 'tools', 'dev-gateway', 'node_modules', 'ws')); }
}
const WebSocket = requireWs();

const HOST = process.argv[2] || '127.0.0.1';
const PORT = Number(process.argv[3] || 8888);
const SECRET = process.env.JWT_SECRET || '';
const HTTPS_PORT = Number(process.env.HTTPS_PORT || 443);

let pass = 0;
let fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log(`  PASS  ${name}`); }
  else { fail++; console.log(`  FAIL  ${name}` + (extra !== undefined ? '  ' + JSON.stringify(extra) : '')); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const b64u = (buf) => Buffer.from(buf).toString('base64').replace(/=+$/, '').replace(/\+/g, '-').replace(/\//g, '_');
function sign(claims) {
  const header = b64u(JSON.stringify({ alg: 'HS256', typ: 'JWT' }));
  const payload = b64u(JSON.stringify(claims));
  const data = header + '.' + payload;
  const sig = crypto.createHmac('sha256', SECRET).update(data).digest();
  return data + '.' + b64u(sig);
}
const now = () => Math.floor(Date.now() / 1000);
// 注意 sub 必须是数字：服务端用 jsoncpp 的 asInt64() 解析，字符串会解析失败
const accessToken = (id, name) => sign({ sub: id, username: name, displayName: name, typ: 'access', iat: now(), exp: now() + 3600, jti: 'a' + id });
const roomToken = (id, name, room) => sign({ sub: id, username: name, displayName: name, typ: 'room', room, iat: now(), exp: now() + 600, jti: 'r' + id });

// 极简 WS 客户端：只关心"能不能收到对端的消息"
class Client {
  constructor(name) {
    this.name = name;
    this.msgs = [];
    this.waiters = [];
    this.closed = false;
  }
  connect() {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(`ws://${HOST}:${PORT}/ws`);
      this.ws.on('open', resolve);
      this.ws.on('error', reject);
      this.ws.on('close', () => { this.closed = true; });
      this.ws.on('message', (raw) => {
        let m;
        try { m = JSON.parse(String(raw)); } catch (e) { return; }
        this.msgs.push(m);
        this.waiters = this.waiters.filter((w) => {
          if (w.pred(m)) { w.resolve(m); return false; }
          return true;
        });
      });
    });
  }
  send(obj) { this.ws.send(JSON.stringify(obj)); }
  wait(pred, timeout = 5000, optional = false) {
    const hit = this.msgs.find(pred);
    if (hit) return Promise.resolve(hit);
    return new Promise((resolve) => {
      const timer = setTimeout(() => resolve(optional ? null : undefined), timeout);
      this.waiters.push({ pred, resolve: (m) => { clearTimeout(timer); resolve(m); } });
    });
  }
  close() { try { this.ws.close(); } catch (e) { /* 忽略 */ } }
}
const isType = (t) => (m) => m.type === t;

function request(method, path, body) {
  return new Promise((resolve) => {
    const data = body ? Buffer.from(JSON.stringify(body)) : null;
    const req = https.request({
      host: HOST, port: HTTPS_PORT, path, method,
      rejectUnauthorized: false,
      headers: data ? { 'Content-Type': 'application/json', 'Content-Length': data.length } : {}
    }, (res) => {
      let text = '';
      res.on('data', (c) => (text += c));
      res.on('end', () => resolve({ status: res.statusCode, body: text }));
    });
    req.on('error', (e) => resolve({ status: 0, body: String(e) }));
    req.setTimeout(8000, () => { req.destroy(); resolve({ status: 0, body: 'timeout' }); });
    if (data) req.write(data);
    req.end();
  });
}

function shell(cmd) {
  return execSync(cmd, { encoding: 'utf8', stdio: 'pipe' });
}

function sshRun(remote) {
  const key = process.env.SSH_KEY;
  const pw = process.env.SSH_PASSWORD;
  const user = process.env.SSH_USER || 'andres';
  const sshHost = process.env.SSH_HOST || HOST;
  if (!key || !pw) throw new Error('需要 PG_STOP_CMD/PG_START_CMD，或 SSH_KEY + SSH_PASSWORD');
  shell(`ssh -i "${key}" -o StrictHostKeyChecking=accept-new ${user}@${sshHost} "echo '${pw}' | sudo -S -p '' ${remote}"`);
}

function stopDb() {
  const cmd = process.env.PG_STOP_CMD;
  if (cmd) return shell(cmd);
  sshRun('systemctl stop postgresql');
}
function startDb() {
  const cmd = process.env.PG_START_CMD;
  if (cmd) return shell(cmd);
  sshRun('systemctl start postgresql');
}

(async () => {
  if (!SECRET) {
    console.log('需要 JWT_SECRET（部署环境：sudo grep JWT_SECRET /opt/webrtc/signaling.env）');
    process.exit(2);
  }
  console.log(`== 目标 ${HOST}（信令 ${PORT}，HTTPS ${HTTPS_PORT}）==`);

  const room = 'PGX' + Math.random().toString(36).slice(2, 8).toUpperCase();
  const alice = new Client('alice');
  const bob = new Client('bob');
  await alice.connect();
  alice.send({ type: 'auth', token: accessToken(301, 'alice') });
  if (!(await alice.wait(isType('auth_ok')))) {
    console.log('  ！鉴权失败：确认 JWT_SECRET 与服务端一致');
    process.exit(2);
  }
  await bob.connect();
  bob.send({ type: 'auth', token: accessToken(302, 'bob') });
  await bob.wait(isType('auth_ok'));
  alice.send({ type: 'join_room', roomToken: roomToken(301, 'alice', room) });
  await alice.wait(isType('joined'));
  bob.send({ type: 'join_room', roomToken: roomToken(302, 'bob', room) });
  await bob.wait(isType('joined'));
  const started = await alice.wait(isType('start_call'), 6000);
  ok('停机前双方已在通话中', !!started, started);

  const healthBefore = await request('GET', '/api/health');
  ok('停机前 /api/health 是正常的', healthBefore.status === 200 && /"db":"ok"/.test(healthBefore.body),
    { status: healthBefore.status, body: healthBefore.body.slice(0, 80) });

  // ---- 停数据库 ----
  console.log('==> 停掉 PostgreSQL');
  try { stopDb(); } catch (e) { console.log('停库失败: ' + e.message); process.exit(2); }
  await sleep(2500);

  // 1. 通话应该完全不受影响：信令转发的消息照样通
  alice.msgs.length = 0;
  bob.msgs.length = 0;
  alice.send({ type: 'ice', candidate: { probe: 1 } });
  const iceThrough = await bob.wait((m) => m.type === 'ice' && m.candidate && m.candidate.probe === 1, 4000, true);
  ok('数据库停机时，信令仍然转发消息（通话不受影响）', !!iceThrough, iceThrough);
  ok('数据库停机时，两个连接都没被断开', alice.closed === false && bob.closed === false,
    { alice: alice.closed, bob: bob.closed });

  // 2. 依赖数据库的部分应该报错：健康检查降级、登录失败
  const healthDown = await request('GET', '/api/health');
  ok('数据库停机时 /api/health 返回 503 且 db=down',
    healthDown.status === 503 && /"db":"down"/.test(healthDown.body),
    { status: healthDown.status, body: healthDown.body.slice(0, 80) });

  const loginDown = await request('POST', '/api/login', { username: 'nobody', password: 'whatever' });
  ok('数据库停机时登录失败（5xx，而不是假装成功）',
    loginDown.status >= 500 || loginDown.status === 0,
    { status: loginDown.status, body: String(loginDown.body).slice(0, 80) });

  // ---- 起数据库 ----
  console.log('==> 恢复 PostgreSQL');
  try { startDb(); } catch (e) { console.log('起库失败: ' + e.message); process.exit(2); }
  await sleep(4000);

  const healthBack = await request('GET', '/api/health');
  ok('数据库恢复后 /api/health 回到 200/db=ok',
    healthBack.status === 200 && /"db":"ok"/.test(healthBack.body),
    { status: healthBack.status, body: healthBack.body.slice(0, 80) });

  // 用注册来证明"写"也恢复了（read-only 的健康检查可能骗人）
  const uname = 'pgdrill' + Math.random().toString(36).slice(2, 8);
  const reg = await request('POST', '/api/register', { username: uname, password: 'Drill#Passw0rd', displayName: 'drill' });
  ok('数据库恢复后能重新注册账号（写路径通了）',
    reg.status === 201 || reg.status === 409 || reg.status === 429,
    { status: reg.status, body: String(reg.body).slice(0, 80) });

  // 3. 原来那两条连接还活着 —— 这是整条演练的核心
  alice.msgs.length = 0;
  bob.msgs.length = 0;
  alice.send({ type: 'ice', candidate: { probe: 2 } });
  const iceAfter = await bob.wait((m) => m.type === 'ice' && m.candidate && m.candidate.probe === 2, 4000, true);
  ok('数据库恢复后，原来的通话连接仍然可用（全程没掉线）', !!iceAfter, iceAfter);

  alice.close();
  bob.close();
  console.log(`\n合计: ${pass} 通过, ${fail} 失败`);
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.error('演练崩溃:', e); process.exit(2); });
