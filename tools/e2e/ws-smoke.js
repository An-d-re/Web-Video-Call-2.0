// 真 C++ 信令服务的协议冒烟测试（不经过浏览器，直接开 WebSocket 说协议）。
//
// 这套用例刻意绕开 HTTP 和浏览器，只验证 server.cpp 自己的行为：鉴权超时、凭证类型隔离、
// 座位分配、offer/answer/bye 转发、空房间回收。跑通它只说明信令状态机是对的，
// 媒体能不能通要看 browser-e2e.js。
//
// 用法：
//   node tools/e2e/ws-smoke.js [host] [port]
//   set JWT_SECRET=...        # 必须和服务端一致，默认 dev-secret-at-least-32-bytes-long-0001
//
// host 默认 127.0.0.1，port 默认 8888（server.cpp 里的 PORT 常量）。
const crypto = require('crypto');
const http = require('http');
const path = require('path');

const ROOT = path.join(__dirname, '..', '..');
function requireWs() {
  try { return require('ws'); } catch (_) { /* 走下面的本地路径 */ }
  return require(path.join(ROOT, 'tools', 'dev-gateway', 'node_modules', 'ws'));
}
const WebSocket = requireWs();

const HOST = process.argv[2] || '127.0.0.1';
const PORT = Number(process.argv[3] || 8888);
const SECRET = process.env.JWT_SECRET || 'dev-secret-at-least-32-bytes-long-0001';

let pass = 0, fail = 0, skipped = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log('PASS  ' + name); }
  else { fail++; console.log('FAIL  ' + name + (extra ? '  => ' + JSON.stringify(extra) : '')); }
}
function skip(name, why) {
  skipped++;
  console.log('SKIP  ' + name + '  (' + why + ')');
}

const b64u = (buf) => Buffer.from(buf).toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');

// 信封格式必须和 auth/jwt.go 里签出来的一致，否则服务端验签直接失败——
// 那样测出来的"拒绝"是假阳性，所以下面每条用例都先确认"该通的确实通了"。
function sign(claims) {
  const header = b64u(JSON.stringify({ alg: 'HS256', typ: 'JWT' }));
  const payload = b64u(JSON.stringify(claims));
  const data = header + '.' + payload;
  const sig = b64u(crypto.createHmac('sha256', SECRET).update(data).digest());
  return data + '.' + sig;
}

const now = () => Math.floor(Date.now() / 1000);
const accessToken = (id, name) => sign({ sub: id, username: name, typ: 'access', iat: now(), exp: now() + 3600, jti: 'j' + id });
const roomToken = (id, name, room) => sign({ sub: id, username: name, typ: 'room', room, iat: now(), exp: now() + 120, jti: 'r' + id });

// ---- 简易 ws 客户端：收消息进队列，可按条件等待 ----
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
  close() { try { this.ws.close(); } catch (e) {} }
}

const isType = (t) => (m) => m.type === t;
const isErr = (code) => (m) => m.type === 'error' && m.code === code;

function get(p) {
  return new Promise((resolve) => {
    const req = http.get({ host: HOST, port: PORT, path: p }, (res) => {
      let body = '';
      res.on('data', (c) => (body += c));
      res.on('end', () => resolve({ status: res.statusCode, body, headers: res.headers }));
    });
    req.on('error', (e) => resolve({ status: 0, body: String(e) }));
    req.setTimeout(5000, () => { req.destroy(); resolve({ status: 0, body: 'timeout' }); });
  });
}

(async () => {
  console.log(`== 目标 ${HOST}:${PORT} ==`);

  // 1. 静态文件（server.cpp 自己也能托管静态页，不依赖 nginx）
  const idx = await get('/');
  ok('静态页 / 返回 200', idx.status === 200, { status: idx.status });
  ok('静态页含 <html 且是登录后的主页面', /<html/i.test(idx.body) && idx.body.includes('roomList'), { len: idx.body.length });

  // 2. 错误凭证
  const bad = new Client('bad');
  await bad.connect();
  bad.send({ type: 'auth', token: 'not-a-jwt' });
  const e1 = await bad.wait(isType('error'));
  ok('伪造 token -> invalid_token', !!e1 && e1.code === 'invalid_token', e1);

  // 3. 进房凭证冒充登录凭证
  const confused = new Client('confused');
  await confused.connect();
  confused.send({ type: 'auth', token: roomToken(1, 'alice', 'ROOM1') });
  const e2 = await confused.wait(isType('error'));
  ok('room token 不能当 access token 用 -> invalid_token', !!e2 && e2.code === 'invalid_token', e2);

  // 4. 正常鉴权
  const alice = new Client('alice');
  await alice.connect();
  alice.send({ type: 'auth', token: accessToken(101, 'alice') });
  const aok = await alice.wait(isType('auth_ok'));
  ok('access token 鉴权成功 -> auth_ok', !!aok && aok.username === 'alice' && aok.userId === 101, aok);
  if (!aok) {
    // 这一条挂了基本只有一个原因：JWT_SECRET 和服务端不一致。部署后的服务用的是
    // install.sh 随机生成的密钥（/opt/webrtc/signaling.env），不是这里的开发默认值。
    // 后面的用例会连锁全红，所以直接把排查方向说出来，别让人一个个去猜。
    console.log('  ！自签 token 被拒：请确认 JWT_SECRET 与服务端一致'
      + '（部署环境看 /opt/webrtc/signaling.env，当前用的长度是 ' + SECRET.length + ' 字节）');
  }

  // 5. 未鉴权就 list_rooms
  const sneak = new Client('sneak');
  await sneak.connect();
  sneak.send({ type: 'list_rooms' });
  const e3 = await sneak.wait(isType('error'), 3000);
  ok('未鉴权 list_rooms -> unauthorized', !!e3 && e3.code === 'unauthorized', e3);

  // 6. 用登录凭证冒充进房凭证
  alice.send({ type: 'join_room', roomToken: accessToken(101, 'alice') });
  const e4 = await alice.wait(isType('error'));
  ok('access token 不能当 roomToken 用 -> invalid_room_token', !!e4 && e4.code === 'invalid_room_token', e4);

  // 7. 正常入房（第一人）
  alice.send({ type: 'join_room', roomToken: roomToken(101, 'alice', 'ROOM1') });
  const j1 = await alice.wait(isType('joined'));
  ok('第一人入房 -> joined peers=1 inCall=false',
    !!j1 && j1.roomId === 'ROOM1' && j1.peers === 1 && j1.inCall === false && j1.self && j1.self.username === 'alice', j1);

  // 8. 重复入房幂等（前端重连时可能重发 join_room）
  alice.send({ type: 'join_room', roomToken: roomToken(101, 'alice', 'ROOM1') });
  const j1b = await alice.wait(isType('joined'));
  ok('同一连接重复入房 -> 幂等 joined peers=1 inCall=false',
    !!j1b && j1b.peers === 1 && j1b.inCall === false, j1b);

  // 9. 第二人入房 -> 双方 start_call
  const bob = new Client('bob');
  await bob.connect();
  bob.send({ type: 'auth', token: accessToken(102, 'bob') });
  await bob.wait(isType('auth_ok'));
  bob.send({ type: 'join_room', roomToken: roomToken(102, 'bob', 'ROOM1') });
  const j2 = await bob.wait(isType('joined'));
  ok('第二人入房 -> joined peers=2 inCall=true', !!j2 && j2.peers === 2 && j2.inCall === true, j2);

  const s1 = await alice.wait(isType('start_call'));
  const s2 = await bob.wait(isType('start_call'));
  ok('先入房者收到 start_call initiator=true 且 peer=bob',
    !!s1 && s1.initiator === true && s1.peer && s1.peer.username === 'bob', s1);
  ok('后入房者收到 start_call initiator=false 且 peer=alice',
    !!s2 && s2.initiator === false && s2.peer && s2.peer.username === 'alice', s2);
  // ICE 配置：没配 TURN 时不下发（属预期）；配了 TURN 就必须带一次性凭据，
  // 且两端的用户名不同（凭据按 userId 签发，不是所有人共用一个账号）。
  const turnEntry = (msg) => ((msg && msg.iceServers) || [])
    .find((s) => [].concat(s.urls).some((u) => String(u).startsWith('turn:')));
  const t1 = turnEntry(s1), t2 = turnEntry(s2);
  if (!turnEntry(s1) && !turnEntry(s2)) {
    ok('未配置 TURN -> start_call 不带 iceServers（属预期）', !!s1 && s1.iceServers === undefined, s1 && s1.iceServers);
  } else {
    ok('start_call 下发了 turn: 服务器', !!t1 && !!t2, { s1: s1 && s1.iceServers, s2: s2 && s2.iceServers });
    ok('TURN 用户名是"过期时间戳:userId"格式',
      !!t1 && /^\d{10,}:\d+$/.test(t1.username || '') && /^\d{10,}:\d+$/.test(t2.username || ''),
      { u1: t1 && t1.username, u2: t2 && t2.username });
    ok('TURN 凭据按用户区分（两端 username 不同）',
      !!t1 && !!t2 && t1.username !== t2.username, { u1: t1 && t1.username, u2: t2 && t2.username });
    ok('TURN 凭据带 credential（非静态密码）',
      !!t1 && typeof t1.credential === 'string' && t1.credential.length >= 20,
      t1 && t1.credential);
    ok('TURN 用户名里的时间戳在未来（未过期）',
      !!t1 && Number(String(t1.username).split(':')[0]) > Math.floor(Date.now() / 1000),
      t1 && t1.username);
  }

  // 10. 实时房间列表
  alice.send({ type: 'list_rooms' });
  const live = await alice.wait(isType('live_rooms'));
  const room1 = live && (live.rooms || []).find((r) => r.roomId === 'ROOM1');
  ok('list_rooms -> live_rooms 里 ROOM1 有 2 人', !!room1 && room1.peers === 2, live);

  // 11. 第三人被拒（一对一房间只有两个座位）
  const carol = new Client('carol');
  await carol.connect();
  carol.send({ type: 'auth', token: accessToken(103, 'carol') });
  await carol.wait(isType('auth_ok'));
  carol.send({ type: 'join_room', roomToken: roomToken(103, 'carol', 'ROOM1') });
  const e5 = await carol.wait(isType('error'));
  ok('第三人入同一房间 -> room_full', !!e5 && e5.code === 'room_full', e5);

  // 12. 转发 offer
  alice.send({ type: 'offer', sdp: { type: 'offer', sdp: 'v=0 fake' } });
  const fwd = await bob.wait(isType('offer'));
  ok('offer 被转发给房内对端', !!fwd && fwd.sdp && fwd.sdp.sdp === 'v=0 fake', fwd);

  // 12.1 文字聊天：服务端只做「限长 + 转发」，不存历史
  alice.send({ type: 'chat', text: '你好，能听到吗？' });
  const chat1 = await bob.wait(isType('chat'));
  ok('chat 被转发给房内对端', !!chat1 && chat1.text === '你好，能听到吗？', chat1);

  // 12.2 超长截断：按「字符」截到 500，不能把多字节汉字切成半个
  const longCn = '字'.repeat(600);   // 每个汉字 3 字节，共 1800 字节
  alice.send({ type: 'chat', text: longCn });
  const chat2 = await bob.wait(isType('chat'), 4000);
  const cutBytes = chat2 && typeof chat2.text === 'string' ? Buffer.byteLength(chat2.text, 'utf8') : -1;
  ok('超长 chat 被截到 500 字符（1500 字节，未切坏汉字）',
    !!chat2 && Array.from(chat2.text).length === 500 && cutBytes === 1500,
    { chars: chat2 ? Array.from(chat2.text).length : -1, bytes: cutBytes });

  // 12.3 只有 type 没有 text：不能静默转发空消息，要明确报错
  alice.send({ type: 'chat' });
  const eChat = await alice.wait(isType('error'), 4000);
  ok('缺 text 的 chat -> bad_request', !!eChat && eChat.code === 'bad_request', eChat);

  // 13. bye：对端收到，且房间腾出座位。
  //     这两条是当初两个 P0 bug 的回归用例：byeSent 不复位导致第二个 bye 根本不发；
  //     g_bye_processed 不清导致同一连接二次 bye 被当重复丢弃、连接永久占着座位。
  alice.send({ type: 'bye' });
  const bye2 = await bob.wait(isType('bye'));
  ok('bye 转发给对端', !!bye2, bye2);

  carol.send({ type: 'join_room', roomToken: roomToken(103, 'carol', 'ROOM1') });
  const j3 = await carol.wait(isType('joined'), 4000);
  ok('挂断后房间腾出座位，第三人可以进来', !!j3 && j3.peers === 2 && j3.inCall === true, j3);

  // 14. 走了的人不能再占座：alice 已 bye，房间只剩 bob+carol，她再进来应该被拒
  alice.send({ type: 'join_room', roomToken: roomToken(101, 'alice', 'ROOM1') });
  const j4 = await alice.wait(isType('error'), 4000);
  ok('原连接重新入房时房间已满 -> room_full（说明它确实已离开房间）', !!j4 && j4.code === 'room_full', j4);

  // 15. 鉴权超时（连上后 5 秒不发 auth 必须被踢，否则空连接能白占线程）
  const mute = new Client('mute');
  await mute.connect();
  const t0 = Date.now();
  const e6 = await mute.wait(isErr('auth_timeout'), 8000, true);
  const dt = Date.now() - t0;
  ok('建连后不发 auth，5 秒后被踢 -> auth_timeout',
    (!!e6 && e6.code === 'auth_timeout') || (e6 === null && mute.closed), { waitedMs: dt, got: e6 });

  // 16. 限流：单条连接狂刷消息必须被掐断。
  //     没有它，一条连接就能把服务端 CPU 打满，或者靠高频转发把对端带宽占满。
  //     令牌桶是 60 条/秒、突发 120，所以一次性灌 400 条必然超限。
  const flood = new Client('flood');
  await flood.connect();
  flood.send({ type: 'auth', token: accessToken(104, 'flood') });
  await flood.wait(isType('auth_ok'));
  for (let i = 0; i < 400; i++) {
    if (flood.closed) break;
    try { flood.send({ type: 'list_rooms' }); } catch (e) { break; }
  }
  const e7 = await flood.wait(isErr('rate_limited'), 5000, true);
  ok('单连接高频发消息 -> rate_limited', !!e7 && e7.code === 'rate_limited', e7);
  await flood.wait(isType('__never__'), 3000, true);
  ok('被限流的连接随后被断开', flood.closed === true, { closed: flood.closed });

  // 17. /stats 指标接口：没 token 必须 403（不能让人白看内部状态）
  const st1 = await get('/stats');
  ok('/stats 不带 token -> 403', st1.status === 403, { status: st1.status });

  // 18. /stats 带对 token -> 200 且结构齐全（压测脚本和运维都靠它）
  const statsToken = process.env.STATS_TOKEN || '';
  if (!statsToken) {
    skip('/stats 带 token 返回结构正确的 JSON', '未设置 STATS_TOKEN，跳过');
  } else {
    const st2 = await get('/stats?token=' + encodeURIComponent(statsToken));
    let j = null;
    try { j = JSON.parse(st2.body); } catch (e) { /* 保持 null */ }
    const need = ['uptimeSeconds', 'pid', 'connections', 'rooms', 'messages', 'errors', 'latencyUs', 'resources'];
    const missing = j ? need.filter((k) => !(k in j)) : need;
    ok('/stats 带 token -> 200 且字段齐全', st2.status === 200 && missing.length === 0,
      { status: st2.status, missing, sample: j ? { uptime: j.uptimeSeconds, ws: j.connections && j.connections.ws } : st2.body.slice(0, 120) });
    const shapeOk = !!j && j.latencyUs && j.latencyUs.join_room && j.latencyUs.forward
      && typeof j.messages.auth === 'number' && typeof j.errors.auth_fail === 'number'
      && typeof j.errors.rate_limited === 'number' && typeof j.errors.slow_client === 'number';
    ok('/stats 延迟与计数结构可直接用于压测', shapeOk, j ? { joinSamples: j.latencyUs.join_room.samples } : null);
    // 刚才那条 flood 连接必须被记到 rate_limited 上，否则等于"限流生效了但没人知道"
    ok('/stats 里记到了 rate_limited', !!j && j.errors.rate_limited >= 1, j && j.errors);
  }

  [bad, confused, sneak, alice, bob, carol, mute, flood].forEach((c) => c.close());

  console.log(`\n合计: ${pass} 通过, ${fail} 失败${skipped ? ', ' + skipped + ' 跳过' : ''}`);
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.error('测试崩溃:', e); process.exit(2); });
