#!/usr/bin/env node
// 信令服务压测客户端 —— 只走真实业务路径。
//
// 为什么不用"挂着几千个 socket"的方式测：
//   那样测出来的是操作系统的 TCP 栈，不是我们的业务容量。
//   这里每个连接都完整走一遍：握手 → auth → join_room → start_call →
//   offer/answer → ICE 互发 → 挂断，和浏览器里的顺序一致。
//
// 两种模式：
//   --mode signaling  本地用 JWT_SECRET 直接签 token，只压 C++ 信令服务。
//                     回答"信令本身能扛多少"，排除 nginx/Go/PG 的干扰。
//   --mode fullstack  走真实 HTTP API 建号/建房/换 roomToken，压整条链路。
//                     回答"用户实际体验"，但受注册限流影响，规模不要开太大。
//
// 用法示例：
//   node tools/load/loadtest.js --host 192.168.220.128 --port 8888 \
//     --mode signaling --secret "$JWT_SECRET" --rooms 200 --duration 60
//
// 输出：终端一张表 + JSON 文件（--out），JSON 里带服务端 /stats 快照。
'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const http = require('http');
const https = require('https');

function requireWs() {
  try {
    return require('ws');
  } catch (e) {
    // 仓库里没有 node_modules 时，复用 dev-gateway 装的那份
    return require(path.join(__dirname, '..', 'dev-gateway', 'node_modules', 'ws'));
  }
}
const WebSocket = requireWs();

// ---------------- 参数 ----------------
function parseArgs(argv) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) {
      out._.push(a);
      continue;
    }
    const key = a.slice(2);
    const next = argv[i + 1];
    if (next && !next.startsWith('--')) {
      out[key] = next;
      i++;
    } else {
      out[key] = true;
    }
  }
  return out;
}

const args = parseArgs(process.argv.slice(2));
const num = (v, d) => (typeof v === 'string' && v !== '' ? Number(v) : d);
// 布尔开关（如 --relay）会被解析成 true，这里统一挡掉，避免把 true 当字符串用
const str = (v, d) => (typeof v === 'string' && v !== '' ? v : d);

const CFG = {
  host: str(args.host, '127.0.0.1'),
  port: num(args.port, 8888),
  mode: str(args.mode, 'signaling'),
  secret: str(args.secret, process.env.JWT_SECRET || ''),
  rooms: num(args.rooms, 50),
  rampUp: num(args['ramp-up'], 5),
  duration: num(args.duration, 30),
  icePerSec: num(args['ice-per-sec'], 4),
  probeTimeout: num(args['probe-timeout'], 5000),
  prefix: str(args.prefix, 'LT'),
  out: str(args.out, ''),
  statsToken: str(args['stats-token'], process.env.STATS_TOKEN || ''),
  reset: args.reset === true,
  // fullstack 模式才用
  apiPort: num(args['api-port'], 443),
  apiHost: str(args['api-host'], str(args.host, '127.0.0.1')),
  accounts: num(args.accounts, 2),
  password: str(args.password, 'LoadTest#2026'),
  merge: str(args.merge, ''),
  trace: args.trace === true || process.env.LOADTEST_TRACE === '1',
};

const now = () => Date.now();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// 这些消息后续会被 waitFor 消费，其余（video_mute / bye 之类）直接丢弃
const KEEP_TYPES = new Set(['auth_ok', 'joined', 'start_call', 'offer', 'answer', 'error']);

// ---------------- 延迟直方图 ----------------
// 与 server.cpp 的 /stats 用同一套桶边界，方便两边数字对照。
const BUCKETS = [100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000];

function newHist() {
  return { buckets: new Array(BUCKETS.length + 1).fill(0), count: 0, maxUs: 0 };
}

function histAdd(h, us) {
  if (!(us >= 0)) return;
  let i = 0;
  while (i < BUCKETS.length && us > BUCKETS[i]) i++;
  h.buckets[i]++;
  h.count++;
  if (us > h.maxUs) h.maxUs = us;
}

function histPct(h, p) {
  if (!h.count) return 0;
  const target = Math.max(1, Math.ceil((h.count * p) / 100));
  let cum = 0;
  for (let i = 0; i < h.buckets.length; i++) {
    cum += h.buckets[i];
    if (cum >= target) return i === BUCKETS.length ? h.maxUs : BUCKETS[i];
  }
  return h.maxUs;
}

function histJson(h) {
  return {
    p50: histPct(h, 50),
    p95: histPct(h, 95),
    p99: histPct(h, 99),
    max: h.maxUs,
    samples: h.count,
    buckets: h.buckets,
  };
}

function histMerge(dst, src) {
  if (!src || !src.buckets) return dst;
  for (let i = 0; i < dst.buckets.length; i++) dst.buckets[i] += src.buckets[i] || 0;
  dst.count += src.count || 0;
  dst.maxUs = Math.max(dst.maxUs, src.maxUs || 0);
  return dst;
}

// ---------------- JWT（HS256，与 auth 服务的签发格式一致）----------------
function b64u(buf) {
  return Buffer.from(buf).toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}

function signJwt(claims) {
  const header = b64u(JSON.stringify({ alg: 'HS256', typ: 'JWT' }));
  const payload = b64u(JSON.stringify(claims));
  const data = header + '.' + payload;
  const sig = b64u(crypto.createHmac('sha256', CFG.secret).update(data).digest());
  return data + '.' + sig;
}

const nowSec = () => Math.floor(Date.now() / 1000);
const accessToken = (id, name) =>
  signJwt({ sub: id, username: name, displayName: name, typ: 'access', iat: nowSec(), exp: nowSec() + 7200, jti: 'a' + id + '-' + nowSec() });
const roomTokenOf = (id, name, room) =>
  signJwt({ sub: id, username: name, displayName: name, typ: 'room', room, iat: nowSec(), exp: nowSec() + 7200, jti: 'r' + id + '-' + room });

// 压测用户的 id 从 100000 起，避免和真实用户撞
const userIdBase = 100000;

// ---------------- HTTP（/stats 与 fullstack API）----------------
function httpJson(urlStr, opts = {}) {
  return new Promise((resolve, reject) => {
    const isHttps = urlStr.startsWith('https://');
    const mod = isHttps ? https : http;
    const req = mod.request(
      urlStr,
      {
        method: opts.method || 'GET',
        headers: opts.headers || {},
        rejectUnauthorized: false,
        timeout: opts.timeout || 10000,
      },
      (res) => {
        let data = '';
        res.setEncoding('utf8');
        res.on('data', (c) => (data += c));
        res.on('end', () => {
          let json = null;
          try {
            json = data ? JSON.parse(data) : null;
          } catch (e) {
            /* 非 JSON 响应保持 null */
          }
          resolve({ status: res.statusCode, headers: res.headers, json, text: data });
        });
      }
    );
    req.on('timeout', () => req.destroy(new Error('http timeout')));
    req.on('error', reject);
    if (opts.body) req.write(opts.body);
    req.end();
  });
}

async function fetchStats(reset) {
  if (!CFG.statsToken) return null;
  let url = `http://${CFG.host}:${CFG.port}/stats?token=${encodeURIComponent(CFG.statsToken)}`;
  if (reset) url += '&reset=1';
  try {
    const r = await httpJson(url, { timeout: 5000 });
    return r.status === 200 ? r.json : { error: 'status=' + r.status };
  } catch (e) {
    return { error: String(e.message || e) };
  }
}

// ---------------- 统计容器 ----------------
const M = {
  connect: newHist(),
  auth: newHist(),
  join: newHist(),
  offerAnswer: newHist(),
  iceRtt: newHist(),
  errors: {},
  sent: {},
  recv: {},
};

function bump(map, key, n = 1) {
  map[key] = (map[key] || 0) + n;
}

// ---------------- 单个连接 ----------------
class Peer {
  constructor(roomId, index, tokens) {
    this.roomId = roomId;
    this.index = index;          // 房间内序号：0 = 先入房（服务端判为 initiator）
    this.tokens = tokens;        // { access, room }
    this.ws = null;
    this.inbox = [];
    this.waiters = [];
    this.closed = false;
    this.isInitiator = false;
    this.startAt = 0;
    this.probeSeq = 0;
    this.pendingProbes = new Map();  // seq -> 发出时间
    this.done = false;
  }

  // 等待满足条件的消息（先查收件箱，再挂 waiter）
  waitFor(pred, timeoutMs = CFG.probeTimeout) {
    for (let i = 0; i < this.inbox.length; i++) {
      if (pred(this.inbox[i])) {
        return Promise.resolve(this.inbox.splice(i, 1)[0]);
      }
    }
    if (this.closed) return Promise.reject(new Error('closed'));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const idx = this.waiters.indexOf(entry);
        if (idx >= 0) this.waiters.splice(idx, 1);
        reject(new Error('timeout waiting for message'));
      }, timeoutMs);
      const entry = {
        pred,
        resolve: (m) => {
          clearTimeout(timer);
          resolve(m);
        },
        timer,
      };
      this.waiters.push(entry);
    });
  }

  onMessage(raw) {
    let msg;
    try {
      msg = JSON.parse(raw);
    } catch (e) {
      bump(M.errors, 'bad_json_from_server');
      return;
    }
    bump(M.recv, msg.type || 'unknown');

    // ICE 探针走快速通道：收到就立刻回，不等轮询。
    // 走 inbox 会被 20ms 的轮询周期拖成几十毫秒，测出来的是客户端自己的延迟。
    if (msg.type === 'ice' && typeof msg.probe === 'number') {
      if (this.onEcho(msg)) return;
      this.echoProbe(msg);
      return;
    }

    for (let i = 0; i < this.waiters.length; i++) {
      const w = this.waiters[i];
      if (w.pred(msg)) {
        this.waiters.splice(i, 1);
        w.resolve(msg);
        return;
      }
    }
    // 只保留后续还会被 waitFor 的消息，避免稳态期间 inbox 无限增长
    if (KEEP_TYPES.has(msg.type)) this.inbox.push(msg);
  }

  connect() {
    return new Promise((resolve, reject) => {
      const t0 = now();
      const ws = new WebSocket(`ws://${CFG.host}:${CFG.port}/ws`, { handshakeTimeout: CFG.probeTimeout });
      this.ws = ws;
      let settled = false;
      ws.on('open', () => {
        if (settled) return;
        settled = true;
        histAdd(M.connect, (now() - t0) * 1000);
        resolve();
      });
      ws.on('message', (data) => this.onMessage(String(data)));
      ws.on('error', (err) => {
        bump(M.errors, 'ws_error');
        if (!settled) {
          settled = true;
          reject(err);
        }
      });
      ws.on('close', () => {
        this.closed = true;
        for (const w of this.waiters.splice(0)) {
          clearTimeout(w.timer);
        }
      });
    });
  }

  send(obj) {
    if (!this.ws || this.closed) return false;
    bump(M.sent, obj.type || 'unknown');
    this.ws.send(JSON.stringify(obj));
    return true;
  }

  async auth() {
    const t0 = now();
    this.send({ type: 'auth', token: this.tokens.access });
    const res = await this.waitFor((m) => m.type === 'auth_ok' || m.type === 'error');
    if (res.type === 'error') {
      bump(M.errors, 'auth:' + res.code);
      throw new Error('auth failed: ' + res.code);
    }
    histAdd(M.auth, (now() - t0) * 1000);
  }

  async join() {
    const t0 = now();
    this.send({ type: 'join_room', roomToken: this.tokens.room });
    const res = await this.waitFor((m) => m.type === 'joined' || m.type === 'error');
    if (res.type === 'error') {
      bump(M.errors, 'join:' + res.code);
      throw new Error('join failed: ' + res.code);
    }
    histAdd(M.join, (now() - t0) * 1000);
  }

  async startCall() {
    const msg = await this.waitFor((m) => m.type === 'start_call', CFG.duration * 1000 + 30000);
    this.isInitiator = msg.initiator === true;
    this.startAt = now();
  }

  async negotiate() {
    const trace = (s) => { if (CFG.trace) console.log(`[trace ${this.roomId}#${this.index}] ${s}`); };
    if (this.isInitiator) {
      const t0 = now();
      this.send({ type: 'offer', t: Date.now(), sdp: 'v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n' });
      trace('offer sent');
      const ans = await this.waitFor((m) => m.type === 'answer' || m.type === 'error');
      trace(`answer recv +${now() - t0}ms`);
      if (ans.type === 'error') {
        bump(M.errors, 'offer:' + ans.code);
        return;
      }
      histAdd(M.offerAnswer, (now() - t0) * 1000);
    } else {
      const offer = await this.waitFor((m) => m.type === 'offer' || m.type === 'error');
      trace(`offer recv, 发出函数 ${offer.t ? Date.now() - offer.t : '?'}ms 前`);
      if (offer.type === 'error') {
        bump(M.errors, 'wait_offer:' + offer.code);
        return;
      }
      this.send({ type: 'answer', sdp: 'v=0\r\no=- 2 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n' });
      trace('answer sent');
    }
  }

  // 稳态流量：ICE 探针（带 seq，对端原样回） + 周期性 mute 状态
  async traffic(deadline) {
    const intervalMs = Math.max(10, Math.floor(1000 / Math.max(1, CFG.icePerSec)));
    let lastMuteAt = 0;
    while (now() < deadline && !this.closed) {
      const seq = ++this.probeSeq;
      this.pendingProbes.set(seq, now());
      this.send({ type: 'ice', candidate: 'candidate:1 1 udp 1 127.0.0.1 5000 typ host', probe: seq });
      if (now() - lastMuteAt > 5000) {
        lastMuteAt = now();
        this.send({ type: 'video_mute', muted: false, screen: false });
        this.send({ type: 'mic_mute', muted: false });
      }
      // 回收超时探针（没有回音就记一次失败，避免 pendingProbes 无限增长）
      for (const [s, t] of this.pendingProbes) {
        if (now() - t > CFG.probeTimeout) {
          this.pendingProbes.delete(s);
          bump(M.errors, 'ice_timeout');
        }
      }
      await sleep(intervalMs);
    }
  }

  // 对端发来的 ICE 探针原样回送，用于测一个来回的 RTT
  echoProbe(msg) {
    if (msg && msg.type === 'ice' && typeof msg.probe === 'number') {
      this.send({ type: 'ice', candidate: 'candidate:2 1 udp 1 127.0.0.1 5001 typ host', probe: msg.probe });
      return true;
    }
    return false;
  }

  onEcho(msg) {
    if (msg && msg.type === 'ice' && typeof msg.probe === 'number') {
      const t = this.pendingProbes.get(msg.probe);
      if (t !== undefined) {
        this.pendingProbes.delete(msg.probe);
        histAdd(M.iceRtt, (now() - t) * 1000);
        return true;
      }
    }
    return false;
  }

  async run(deadline) {
    try {
      await this.connect();
      await this.auth();
      await this.join();
      await this.startCall();
      await this.negotiate();

      // 稳态期间 ICE 探针在 onMessage 里就回掉了，这里只管按节奏发
      await this.traffic(deadline);

      this.send({ type: 'bye' });
      await sleep(50);
    } catch (e) {
      bump(M.errors, 'peer:' + (e.message || 'error'));
    } finally {
      this.done = true;
      if (this.ws) {
        try {
          this.ws.close();
        } catch (e) {
          /* ignore */
        }
      }
    }
  }
}

// ---------------- fullstack 模式的前置准备 ----------------
// 注册账号 + 建房，全部走真实 HTTP API。注意注册限流是 5/分钟、突发 3，
// 所以 --accounts 默认只开 2 个，多个房间复用同一批账号。
async function prepareFullstack(roomCount) {
  const base = `https://${CFG.apiHost}:${CFG.apiPort}`;
  const tag = 'lt' + now().toString(36);
  const accounts = [];

  for (let i = 0; i < CFG.accounts; i++) {
    const username = `${tag}_${i}`;
    let created = null;
    for (let attempt = 0; attempt < 3 && !created; attempt++) {
      const r = await httpJson(`${base}/api/register`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username, password: CFG.password, displayName: '压测' + i }),
      });
      if (r.status === 201 || r.status === 200) {
        created = { username, token: r.json.accessToken, userId: r.json.user && r.json.user.id };
      } else if (r.status === 429) {
        console.log(`  注册限流，等 62 秒后重试（${username}）`);
        await sleep(62000);
      } else {
        throw new Error(`注册失败 ${username}: ${r.status} ${r.text.slice(0, 120)}`);
      }
    }
    if (!created) throw new Error('注册重试次数用尽（限流？可调小 --accounts）');
    accounts.push(created);
  }

  // 建房：房间号由服务端生成，只能拿返回值里的 id
  const owner = accounts[0];
  const roomIds = [];
  for (let i = 0; i < roomCount; i++) {
    const r = await httpJson(`${base}/api/rooms`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Authorization: 'Bearer ' + owner.token },
      body: JSON.stringify({ title: '压测房 ' + i }),
    });
    if (r.status !== 201 || !r.json || !r.json.room) {
      throw new Error(`建房失败 #${i}: ${r.status} ${r.text.slice(0, 120)}`);
    }
    roomIds.push(r.json.room.id);
  }

  // 每个连接单独换 roomToken（这才是真实流程）
  async function tokenFor(roomId, idx) {
    const acc = accounts[idx % accounts.length];
    const r = await httpJson(`${base}/api/rooms/${roomId}/join`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', Authorization: 'Bearer ' + acc.token },
      body: JSON.stringify({}),
    });
    if (r.status !== 200) throw new Error(`换 roomToken 失败 ${roomId}: ${r.status} ${r.text.slice(0, 120)}`);
    return { access: acc.token, room: r.json.roomToken };
  }

  return { accounts, roomIds, tokenFor };
}

// ---------------- 主流程 ----------------
function buildRooms() {
  const rooms = [];
  for (let i = 0; i < CFG.rooms; i++) {
    rooms.push(`${CFG.prefix}${String(i).padStart(5, '0')}`);
  }
  return rooms;
}

async function runScenario() {
  let fullstack = null;
  let rooms;
  if (CFG.mode === 'fullstack') {
    console.log(`==> 模式=fullstack 目标=https://${CFG.apiHost}:${CFG.apiPort} 房间=${CFG.rooms}`);
    fullstack = await prepareFullstack(CFG.rooms);
    rooms = fullstack.roomIds;
    console.log(`    账号 ${fullstack.accounts.length} 个，房间 ${rooms.length} 个（房间号由服务端生成）`);
  } else {
    rooms = buildRooms();
    if (!CFG.secret || CFG.secret.length < 32) {
      throw new Error('signaling 模式需要 --secret（JWT_SECRET，≥32 字节）');
    }
  }

  console.log(`==> 目标=ws://${CFG.host}:${CFG.port}/ws 房间=${rooms.length} 连接=${rooms.length * 2}`);
  console.log(`    爬坡=${CFG.rampUp}s 稳态=${CFG.duration}s ICE=${CFG.icePerSec}/秒/连接`);

  // 清一次服务端计数窗口：否则 /stats 是累计值，上一轮的延迟会混进来，
  // 两次同参数压测的 P95 永远"看起来很稳定"，其实是历史平均。
  if (CFG.reset && CFG.statsToken) {
    const r = await fetchStats(true);
    if (r && !r.error) console.log('    已重置服务端计数窗口（只统计本轮）');
  }

  const statsBefore = await fetchStats();
  if (CFG.statsToken && statsBefore && statsBefore.error) {
    console.log(`    /stats 取不到（${statsBefore.error}）：确认 --stats-token 与 STATS_TOKEN 一致`);
  }

  const peers = [];
  const total = CFG.rooms * 2;
  const rampPerPeer = CFG.rampUp > 0 ? (CFG.rampUp * 1000) / total : 0;
  const startedAt = now();
  const deadline = startedAt + CFG.rampUp * 1000 + CFG.duration * 1000;

  // 每 5 秒采一次服务端指标
  const snapshots = [];
  const sampler = setInterval(async () => {
    const s = await fetchStats();
    if (s) snapshots.push({ at: now() - startedAt, stats: s });
  }, 5000);

  const tasks = [];
  let idx = 0;
  for (const roomId of rooms) {
    for (let slot = 0; slot < 2; slot++) {
      const uid = userIdBase + idx;
      const name = `lt${uid}`;
      let tokens;
      if (fullstack) {
        tokens = await fullstack.tokenFor(roomId, idx);
      } else {
        tokens = { access: accessToken(uid, name), room: roomTokenOf(uid, name, roomId) };
      }
      const peer = new Peer(roomId, slot, tokens);
      peers.push(peer);
      tasks.push(peer.run(deadline));
      idx++;
      if (rampPerPeer > 0) await sleep(rampPerPeer);
    }
  }

  await Promise.all(tasks);
  clearInterval(sampler);

  const statsAfter = await fetchStats();
  return {
    config: {
      host: CFG.host,
      port: CFG.port,
      mode: CFG.mode,
      rooms: CFG.rooms,
      connections: total,
      rampUpSeconds: CFG.rampUp,
      durationSeconds: CFG.duration,
      icePerSec: CFG.icePerSec,
      startedAt: new Date(startedAt).toISOString(),
      elapsedSeconds: Math.round((now() - startedAt) / 1000),
    },
    latencyUs: {
      connect: histJson(M.connect),
      auth: histJson(M.auth),
      join: histJson(M.join),
      offerAnswer: histJson(M.offerAnswer),
      iceRtt: histJson(M.iceRtt),
    },
    sent: M.sent,
    recv: M.recv,
    errors: M.errors,
    server: { before: statsBefore, snapshots, after: statsAfter },
  };
}

function printReport(r) {
  const ok = Object.values(r.errors).reduce((a, b) => a + b, 0) === 0;
  console.log('\n===== 客户端视角 =====');
  const rows = [
    ['连接建立', r.latencyUs.connect],
    ['auth 往返', r.latencyUs.auth],
    ['join_room', r.latencyUs.join],
    ['offer→answer', r.latencyUs.offerAnswer],
    ['ICE 往返', r.latencyUs.iceRtt],
  ];
  console.log('指标           样本数     P50      P95      P99      MAX   (单位 µs)');
  for (const [name, h] of rows) {
    console.log(
      `${name.padEnd(14)}${String(h.samples).padStart(6)}  ${String(h.p50).padStart(7)}  ${String(h.p95).padStart(7)}  ${String(h.p99).padStart(7)}  ${String(h.max).padStart(7)}`
    );
  }
  console.log('\n发出：' + JSON.stringify(r.sent));
  console.log('收到：' + JSON.stringify(r.recv));
  console.log('错误：' + JSON.stringify(r.errors));
  console.log(`结论：${ok ? '无错误' : '有错误，见上'}`);

  const after = r.server && r.server.after;
  if (after && !after.error) {
    console.log('\n===== 服务端 /stats（结束时快照）=====');
    console.log('连接数：' + JSON.stringify(after.connections));
    console.log('房间数：' + JSON.stringify(after.rooms));
    console.log('消息数：' + JSON.stringify(after.messages));
    console.log('错误数：' + JSON.stringify(after.errors));
    console.log('自测延迟：' + JSON.stringify(after.latencyUs));
    console.log('资源：' + JSON.stringify(after.resources));
  }
}

// ---------------- --merge：合并多个分片结果 ----------------
function mergeFiles(files) {
  const merged = {
    config: { merged: files },
    latencyUs: {},
    sent: {},
    recv: {},
    errors: {},
    server: { snapshots: [] },
  };
  const hists = {};
  for (const f of files) {
    const r = JSON.parse(fs.readFileSync(f, 'utf8'));
    for (const [k, v] of Object.entries(r.latencyUs || {})) {
      hists[k] = hists[k] || newHist();
      histMerge(hists[k], v);
    }
    for (const key of ['sent', 'recv', 'errors']) {
      for (const [k, v] of Object.entries(r[key] || {})) {
        merged[key][k] = (merged[key][k] || 0) + v;
      }
    }
  }
  for (const [k, h] of Object.entries(hists)) merged.latencyUs[k] = histJson(h);
  return merged;
}

(async () => {
  try {
    if (CFG.merge) {
      const files = String(CFG.merge).split(',').map((s) => s.trim()).filter(Boolean);
      const merged = mergeFiles(files);
      printReport(merged);
      if (CFG.out) fs.writeFileSync(CFG.out, JSON.stringify(merged, null, 2));
      return;
    }
    const result = await runScenario();
    printReport(result);
    if (CFG.out) {
      fs.mkdirSync(path.dirname(CFG.out), { recursive: true });
      fs.writeFileSync(CFG.out, JSON.stringify(result, null, 2));
      console.log(`\n结果已写入 ${CFG.out}`);
    }
    const failed = Object.values(result.errors).reduce((a, b) => a + b, 0);
    process.exit(failed === 0 ? 0 : 1);
  } catch (e) {
    console.error('压测失败：' + (e && e.stack ? e.stack : e));
    process.exit(2);
  }
})();
