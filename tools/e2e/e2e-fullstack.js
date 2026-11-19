// 真全栈 e2e：浏览器之外最接近真实链路的一层。
//   https://<host>/            → nginx → /opt/webrtc/static
//   https://<host>/api/...     → nginx → Go auth (127.0.0.1:8080) → PostgreSQL
//   wss://<host>/ws            → nginx upgrade → C++ muduo (127.0.0.1:8888)
//
// 它验证的是"接线对不对"：nginx 的反代规则、Go 服务的真实数据库读写、PostgreSQL 里的
// 房间/成员表、以及 wss 的 Upgrade 头有没有被 nginx 传对。浏览器里的媒体协商由
// browser-e2e.js 负责，信令状态机的细节由 ws-smoke.js 负责，三者互补。
//
// 用法：
//   node tools/e2e/e2e-fullstack.js [host]
//   set NODE_TLS_REJECT_UNAUTHORIZED=0      # 自签证书，Node 侧必须放宽（脚本内部也传了 rejectUnauthorized）
//
// 注意：注册接口有 IP 限流（5 次/分钟、突发 3），连跑两遍会撞 429，脚本里会自动等桶回血。
const https = require('https');
const http = require('http');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..');
function requireWs() {
  try { return require('ws'); } catch (_) { /* 走下面的本地路径 */ }
  return require(path.join(ROOT, 'tools', 'dev-gateway', 'node_modules', 'ws'));
}
const WebSocket = requireWs();

const HOST = process.argv[2] || '127.0.0.1';
const PORT = Number(process.argv[3] || 443);
const BASE = `https://${HOST}${PORT === 443 ? '' : ':' + PORT}`;
const WSBASE = `wss://${HOST}${PORT === 443 ? '' : ':' + PORT}/ws`;

let pass = 0, fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log('PASS  ' + name); }
  else { fail++; console.log('FAIL  ' + name + (extra !== undefined ? '  => ' + JSON.stringify(extra).slice(0, 300) : '')); }
}

const agent = new https.Agent({ rejectUnauthorized: false, keepAlive: true });

// Go 服务的错误体统一是 { error: { code, message } }，别的地方可能是扁平的 { error: "x" }，
// 两种都认，免得断言写成为实现细节。
function errCode(json) {
  if (!json || !json.error) return undefined;
  return typeof json.error === 'string' ? json.error : json.error.code;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// 注册接口有限流：单 IP 每分钟 5 次、突发 3（auth/server.go: registerIP）。
// 这是真功能不是 bug，但连续跑两遍 e2e 就会撞上 429，所以这里等桶回血再重试。
// 一个 token 约 12 秒回满，最多等 4 轮（约 60 秒）。
async function registerWithRetry(body) {
  for (let i = 0; i < 4; i++) {
    const r = await req('POST', '/api/register', body);
    if (r.status !== 429) return r;
    console.log('  … 注册被限流（429），等 16 秒后重试');
    await sleep(16000);
  }
  return req('POST', '/api/register', body);
}

function req(method, p, body, token) {
  return new Promise((resolve) => {
    const data = body === undefined ? null : JSON.stringify(body);
    const headers = {};
    if (data) { headers['Content-Type'] = 'application/json'; headers['Content-Length'] = Buffer.byteLength(data); }
    if (token) headers['Authorization'] = 'Bearer ' + token;
    const r = https.request({ host: HOST, port: PORT, path: p, method, headers, agent, rejectUnauthorized: false }, (res) => {
      let buf = '';
      res.on('data', (c) => (buf += c));
      res.on('end', () => {
        let json = null;
        try { json = JSON.parse(buf); } catch (e) { /* 非 JSON（静态页） */ }
        resolve({ status: res.statusCode, headers: res.headers, body: buf, json });
      });
    });
    r.on('error', (e) => resolve({ status: 0, body: String(e), json: null }));
    r.setTimeout(8000, () => { r.destroy(); resolve({ status: 0, body: 'timeout', json: null }); });
    if (data) r.write(data);
    r.end();
  });
}

function plainHttp(p) {
  return new Promise((resolve) => {
    const r = http.get({ host: HOST, port: 80, path: p }, (res) => {
      res.resume();
      resolve({ status: res.statusCode, location: res.headers.location });
    });
    r.on('error', (e) => resolve({ status: 0, err: String(e) }));
    r.setTimeout(5000, () => { r.destroy(); resolve({ status: 0 }); });
  });
}

class WS {
  constructor() { this.msgs = []; this.waiters = []; this.closed = false; }
  connect() {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(WSBASE, { rejectUnauthorized: false });
      this.ws.on('open', () => resolve(this));
      this.ws.on('error', reject);
      this.ws.on('close', () => { this.closed = true; this._drain(); });
      this.ws.on('message', (raw) => {
        let m; try { m = JSON.parse(raw.toString()); } catch (e) { m = { raw: raw.toString() }; }
        this.msgs.push(m); this._drain();
      });
    });
  }
  _drain() {
    for (let i = 0; i < this.waiters.length; i++) {
      const w = this.waiters[i];
      const idx = this.msgs.findIndex(w.pred);
      if (idx >= 0) { this.waiters.splice(i, 1); i--; w.resolve(this.msgs.splice(idx, 1)[0]); }
      else if (w.onClose && this.closed) { this.waiters.splice(i, 1); i--; w.resolve(null); }
    }
  }
  wait(pred, ms = 5000, onClose = false) {
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
  send(o) { this.ws.send(JSON.stringify(o)); }
  close() { try { this.ws.close(); } catch (e) {} }
}

const T = (t) => (m) => m.type === t;
const suffix = Date.now().toString(36).slice(-5);

(async () => {
  console.log(`== 全栈目标 ${BASE} ==\n-- HTTP / nginx --`);

  const redirect = await plainHttp('/');
  ok('http:// 跳转到 https://', redirect.status === 301 && String(redirect.location || '').startsWith('https://'), redirect);

  const index = await req('GET', '/');
  ok('https 根路径返回主页 200', index.status === 200, index.status);
  ok('主页里有侧边栏房间列表容器', index.body.includes('roomList'), { len: index.body.length });

  const loginPage = await req('GET', '/login.html');
  ok('登录页 200', loginPage.status === 200, loginPage.status);

  const health = await req('GET', '/api/health');
  ok('/api/health 经 nginx 反代到 Go 且 db=ok',
    health.status === 200 && health.json && health.json.db === 'ok', health.json);
  ok('/api/health 响应头没泄漏 Go 版本', !String(health.headers.server || '').includes('Go'), health.headers.server);

  console.log('\n-- 认证服务 --');
  const uA = 'alice_' + suffix, uB = 'bob_' + suffix;
  const ra = await registerWithRetry({ username: uA, password: 'Passw0rd!23', displayName: '爱丽丝' });
  ok('注册 alice 201', ra.status === 201 && ra.json && ra.json.accessToken, ra.status);
  const rb = await registerWithRetry({ username: uB, password: 'Passw0rd!23', displayName: '鲍勃' });
  ok('注册 bob 201', rb.status === 201 && rb.json && rb.json.accessToken, rb.status);
  ok('注册响应种下了 refresh cookie（HttpOnly）',
    String(ra.headers['set-cookie'] || '').includes('HttpOnly'), ra.headers['set-cookie']);

  const dup = await registerWithRetry({ username: uA, password: 'Passw0rd!23' });
  ok('重复注册被拒（409/400）', dup.status === 409 || dup.status === 400, dup.status);

  if (!(ra.json && ra.json.accessToken) || !(rb.json && rb.json.accessToken)) {
    console.log('\n注册没拿到 accessToken，后面全依赖它，直接中止');
    process.exit(2);
  }

  const badLogin = await req('POST', '/api/login', { username: uA, password: 'wrong-password' });
  ok('错误密码登录被拒（401）', badLogin.status === 401, badLogin.status);

  const la = await req('POST', '/api/login', { username: uA, password: 'Passw0rd!23' });
  ok('登录成功返回 accessToken', la.status === 200 && la.json && la.json.accessToken, la.status);
  const tokenA = la.json.accessToken;
  const lb = await req('POST', '/api/login', { username: uB, password: 'Passw0rd!23' });
  const tokenB = lb.json && lb.json.accessToken;
  if (!tokenA || !tokenB) {
    console.log(`\n拿不到登录凭证（A=${la.status} B=${lb.status}），后面全依赖它，直接中止`);
    process.exit(2);
  }

  const me = await req('GET', '/api/me', undefined, tokenA);
  ok('/api/me 返回当前用户', me.status === 200 && me.json.user.username === uA, me.json && me.json.user);
  const noAuth = await req('GET', '/api/me');
  ok('/api/me 不带 token 401', noAuth.status === 401, noAuth.status);

  console.log('\n-- 房间 --');
  const title = '演示房间' + suffix;
  const cr = await req('POST', '/api/rooms', { title, password: '1234' }, tokenA);
  ok('创建加密房间 201', cr.status === 201 && cr.json.room && cr.json.room.id, cr.json);
  const roomId = cr.json.room.id;
  ok('房间带密码标记 hasPassword', cr.json.room.hasPassword === true, cr.json.room);
  // 前端靠 ownerId === Auth.user.id 决定要不要画「解散」按钮，所以这里必须真的等于
  // 创建者的用户 id（之前这条断言写成了自己等于自己，等于没测）。
  ok('房间返回 ownerId 且等于创建者', cr.json.room.ownerId === me.json.user.id, { ownerId: cr.json.room.ownerId, me: me.json.user.id });

  const list = await req('GET', '/api/rooms', undefined, tokenB);
  ok('房间列表里能看到新房间', list.status === 200 && (list.json.rooms || []).some((r) => r.id === roomId), list.status);

  const joinBad = await req('POST', `/api/rooms/${roomId}/join`, { password: '9999' }, tokenB);
  ok('密码错误进房被拒 403/bad_password', joinBad.status === 403 && errCode(joinBad.json) === 'bad_password', joinBad.json);

  const joinOkB = await req('POST', `/api/rooms/${roomId}/join`, { password: '1234' }, tokenB);
  ok('密码正确换到 roomToken', joinOkB.status === 200 && joinOkB.json.roomToken, joinOkB.json);
  const roomTokenB = joinOkB.json.roomToken;

  const joinOkA = await req('POST', `/api/rooms/${roomId}/join`, { password: '1234' }, tokenA);
  const roomTokenA = joinOkA.json.roomToken;

  const mine = await req('GET', '/api/rooms/mine', undefined, tokenA);
  ok('我的房间列表包含它', mine.status === 200 && (mine.json.rooms || []).some((r) => r.id === roomId), mine.status);

  console.log('\n-- 信令（wss 经 nginx 升级）--');
  const wsA = new WS(); await wsA.connect();
  wsA.send({ type: 'auth', token: tokenA });
  const aok = await wsA.wait(T('auth_ok'));
  ok('wss 建连 + 鉴权成功（nginx 的 Upgrade 头传对了）', !!aok && aok.username === uA, aok);

  wsA.send({ type: 'join_room', roomToken: roomTokenA });
  const jA = await wsA.wait(T('joined'));
  ok('A 入房 peers=1 inCall=false', !!jA && jA.peers === 1 && jA.inCall === false, jA);

  const wsB = new WS(); await wsB.connect();
  wsB.send({ type: 'auth', token: tokenB });
  await wsB.wait(T('auth_ok'));
  wsB.send({ type: 'join_room', roomToken: roomTokenB });
  const jB = await wsB.wait(T('joined'));
  ok('B 入房 peers=2 inCall=true', !!jB && jB.peers === 2 && jB.inCall === true, jB);

  const sA = await wsA.wait(T('start_call'));
  const sB = await wsB.wait(T('start_call'));
  ok('A 收到 start_call（initiator=true, peer=鲍勃）',
    !!sA && sA.initiator === true && sA.peer && sA.peer.displayName === '鲍勃', sA);
  ok('B 收到 start_call（initiator=false, peer=爱丽丝）',
    !!sB && sB.initiator === false && sB.peer && sB.peer.displayName === '爱丽丝', sB);

  wsA.send({ type: 'offer', sdp: { type: 'offer', sdp: 'v=0\r\nfake' } });
  const fwd = await wsB.wait(T('offer'));
  ok('offer 经 nginx 转发给对端', !!fwd && fwd.sdp && fwd.sdp.sdp.includes('v=0'), fwd);

  wsB.send({ type: 'answer', sdp: { type: 'answer', sdp: 'v=0\r\nanswer' } });
  const ans = await wsA.wait(T('answer'));
  ok('answer 转发给对方', !!ans, ans);

  wsA.send({ type: 'ice', candidate: { candidate: 'candidate:1 1 udp 1 192.168.220.1 5000 typ host' } });
  const ice = await wsB.wait(T('ice'));
  ok('ICE candidate 转发', !!ice, ice);

  wsB.send({ type: 'bye' });
  const bye = await wsA.wait(T('bye'));
  ok('bye 转发', !!bye, bye);

  console.log('\n-- 解散房间 --');
  const delBad = await req('DELETE', `/api/rooms/${roomId}`, undefined, tokenB);
  ok('非创建者解散 403/not_owner', delBad.status === 403 && errCode(delBad.json) === 'not_owner', delBad.json);
  const delOk = await req('DELETE', `/api/rooms/${roomId}`, undefined, tokenA);
  ok('创建者解散 204', delOk.status === 204, delOk.status);
  const gone = await req('GET', '/api/rooms', undefined, tokenA);
  ok('解散后不在列表里', gone.status === 200 && !(gone.json.rooms || []).some((r) => r.id === roomId), gone.status);

  console.log('\n-- 登出 --');
  const lo = await req('POST', '/api/logout', undefined, tokenA);
  ok('登出 204', lo.status === 204, lo.status);

  wsA.close(); wsB.close();
  console.log(`\n合计: ${pass} 通过, ${fail} 失败`);
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.error('测试崩溃:', e); process.exit(2); });
