#!/usr/bin/env node
'use strict';

/**
 * 本地开发网关（仅在拿不到服务器时使用，不参与生产部署）
 *
 * 用一个端口同时模拟 nginx + C++ 信令服务：
 *   GET  /            → 托管 static/ 目录
 *   *    /api/*       → 反代到 Go auth 服务（真数据库、真令牌）
 *   WS   /ws          → 假信令：实现 auth / auth_ok / join_room / joined / start_call / error
 *
 * 目的：在没有阿里云服务器的环境下，把前端"登录 → 鉴权 → 入房"整条链路跑起来。
 * 假信令只实现协议，不实现媒体；等服务器可用时换成真 C++ 服务，前端一行不用改。
 *
 * 用法：
 *   set JWT_SECRET=与 auth 服务相同的密钥
 *   node server.js
 */

const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { WebSocketServer } = require('ws');

const PORT = Number(process.env.GATEWAY_PORT || 8890);
const AUTH_URL = process.env.AUTH_URL || 'http://127.0.0.1:8080';
const JWT_SECRET = process.env.JWT_SECRET || '';
const AUTH_TIMEOUT_MS = Number(process.env.AUTH_TIMEOUT_MS || 5000);
const STATIC_DIR = path.resolve(__dirname, '..', '..', 'static');
const CLOCK_SKEW = 60;

if (JWT_SECRET.length < 32) {
    console.error('[gateway] 需要 JWT_SECRET（≥32 字节），且必须与 auth 服务一致');
    process.exit(1);
}

const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'application/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.jpeg': 'image/jpeg',
    '.gif': 'image/gif',
    '.svg': 'image/svg+xml',
    '.ico': 'image/x-icon',
};

// ---------------------------------------------------------------
// 静态文件
// ---------------------------------------------------------------
function serveStatic(req, res) {
    let urlPath = decodeURIComponent(req.url.split('?')[0]);
    if (urlPath === '/') urlPath = '/index.html';

    const safe = path.normalize(urlPath).replace(/^([/\\])+/, '');
    const filePath = path.resolve(STATIC_DIR, safe);

    if (!filePath.startsWith(STATIC_DIR + path.sep)) {
        res.writeHead(403, { 'Content-Type': 'text/plain; charset=utf-8' });
        return res.end('403 Forbidden');
    }

    fs.readFile(filePath, (err, buf) => {
        if (err) {
            res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
            return res.end('404 Not Found');
        }
        res.writeHead(200, {
            'Content-Type': MIME[path.extname(filePath).toLowerCase()] || 'application/octet-stream',
            'Cache-Control': 'no-store',
        });
        res.end(buf);
    });
}

// ---------------------------------------------------------------
// /api 反代到 auth 服务
// ---------------------------------------------------------------
function proxyApi(req, res) {
    const target = new URL(AUTH_URL);
    const opts = {
        hostname: target.hostname,
        port: target.port || 80,
        method: req.method,
        path: req.url,
        headers: Object.assign({}, req.headers, {
            host: target.host,
            'x-real-ip': req.socket.remoteAddress || '127.0.0.1',
        }),
    };

    const upstream = http.request(opts, (up) => {
        res.writeHead(up.statusCode || 502, up.headers);
        up.pipe(res);
    });

    upstream.on('error', (e) => {
        console.error('[api] 反代失败:', e.message);
        res.writeHead(502, { 'Content-Type': 'application/json; charset=utf-8' });
        res.end(JSON.stringify({ error: { code: 'upstream_down', message: 'auth 服务不可达: ' + e.message } }));
    });

    req.pipe(upstream);
}

// ---------------------------------------------------------------
// JWT 验签（与 C++/Go 同样的规则，顺带做第三方交叉验证）
// ---------------------------------------------------------------
function nowUnix() {
    return Math.floor(Date.now() / 1000);
}

function verifyJwt(token, expectedTyp) {
    const parts = String(token || '').split('.');
    if (parts.length !== 3 || !parts[0] || !parts[1]) return { ok: false, code: 'malformed' };

    let header;
    try {
        header = JSON.parse(Buffer.from(parts[0], 'base64url').toString('utf8'));
    } catch (e) {
        return { ok: false, code: 'malformed' };
    }
    if (header.alg !== 'HS256') return { ok: false, code: 'bad_algorithm' };

    const expected = crypto.createHmac('sha256', JWT_SECRET)
        .update(parts[0] + '.' + parts[1]).digest();
    let actual;
    try {
        actual = Buffer.from(parts[2], 'base64url');
    } catch (e) {
        return { ok: false, code: 'bad_signature' };
    }
    if (expected.length !== actual.length || !crypto.timingSafeEqual(expected, actual)) {
        return { ok: false, code: 'bad_signature' };
    }

    let claims;
    try {
        claims = JSON.parse(Buffer.from(parts[1], 'base64url').toString('utf8'));
    } catch (e) {
        return { ok: false, code: 'malformed' };
    }
    // 类型隔离：登录凭证（access）不能当进房凭证（room）用，反之亦然
    if (claims.typ !== expectedTyp) return { ok: false, code: 'bad_type' };
    if (expectedTyp === 'room' && !claims.room) return { ok: false, code: 'malformed' };
    if (!claims.exp || nowUnix() > claims.exp + CLOCK_SKEW) return { ok: false, code: 'token_expired' };
    if (!claims.sub) return { ok: false, code: 'malformed' };

    return { ok: true, claims };
}

// ---------------------------------------------------------------
// 假信令
// ---------------------------------------------------------------
const sessions = new Set();
// 运行时房间：房间的权威数据（是否存在/标题/密码）在 PostgreSQL，由 auth 服务负责。
// 这里只记"谁现在连着"，房间空了就删。
const rooms = new Map();  // roomId -> [session]

function send(ws, obj) {
    if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(obj));
}

function sendError(ws, code, message) {
    send(ws, { type: 'error', code: code, message: message });
}

function selfOf(s) {
    const c = s.user || {};
    return {
        userId: c.sub || 0,
        username: c.username || 'guest',
        displayName: c.name || c.username || '游客',
    };
}

function peerOf(s) {
    const list = rooms.get(s.roomId);
    if (!list) return null;
    return list.find((x) => x !== s) || null;
}

function leaveRoom(s) {
    if (!s.roomId) return;
    const list = rooms.get(s.roomId);
    s.roomId = null;
    if (!list) return;

    const i = list.indexOf(s);
    if (i >= 0) list.splice(i, 1);

    const peer = list[0];
    if (peer) send(peer.ws, { type: 'bye' });

    if (list.length === 0) rooms.delete(s.roomId);
}

function handleMessage(s, raw) {
    let msg;
    try {
        msg = JSON.parse(raw);
    } catch (e) {
        return;
    }
    if (!msg || typeof msg.type !== 'string') return;

    const ws = s.ws;

    if (msg.type === 'auth') {
        const r = verifyJwt(msg.token, 'access');
        if (!r.ok) {
            console.log('[ws] 鉴权失败 [' + r.code + ']');
            sendError(ws, 'invalid_token', '凭证无效或已过期');
            return;
        }
        s.authed = true;
        s.user = r.claims;
        s.deadline = 0;
        console.log('[ws] 鉴权成功: ' + r.claims.username);
        send(ws, {
            type: 'auth_ok',
            userId: r.claims.sub,
            username: r.claims.username,
            displayName: r.claims.name || r.claims.username,
        });
        return;
    }

    // 实时房间列表：只回答"哪个房间现在有几个人"
    if (msg.type === 'list_rooms') {
        if (!s.authed) {
            sendError(ws, 'unauthorized', '请先登录');
            return;
        }
        const out = [];
        for (const [roomId, list] of rooms) {
            if (list.length === 0) continue;
            out.push({ roomId: roomId, peers: list.length });
        }
        send(ws, { type: 'live_rooms', rooms: out });
        return;
    }

    if (msg.type === 'join_room') {
        if (!s.authed) {
            sendError(ws, 'unauthorized', '请先登录');
            return;
        }

        // 房间是否存在、密码对不对，auth 服务已经把过关了；
        // 这里只验签取 roomId（与 C++ 侧一致）
        const r = verifyJwt(msg.roomToken, 'room');
        if (!r.ok) {
            console.log('[ws] 进房凭证校验失败 [' + r.code + ']');
            sendError(ws, 'invalid_room_token', '进房凭证无效或已过期，请重新加入房间');
            return;
        }
        const roomId = r.claims.room;

        let list = rooms.get(roomId);

        // 幂等：已经在这个房间里
        if (list && list.indexOf(s) >= 0) {
            send(ws, {
                type: 'joined', roomId: roomId, peers: list.length,
                inCall: list.length >= 2, self: selfOf(s),
            });
            return;
        }

        if (list && list.length >= 2) {
            sendError(ws, 'room_full', '房间已满');
            return;
        }

        leaveRoom(s);           // 先从旧房间摘掉（会通知旧对端）
        if (!list) {
            list = [];
            rooms.set(roomId, list);
        }
        list.push(s);
        s.roomId = roomId;

        send(ws, {
            type: 'joined', roomId: roomId, peers: list.length,
            inCall: list.length >= 2, self: selfOf(s),
        });
        console.log('[ws] ' + selfOf(s).displayName + ' 加入房间 ' + roomId + '（' + list.length + '/2）');

        if (list.length === 2) {
            const a = list[0];
            const b = list[1];
            send(a.ws, { type: 'start_call', initiator: true, peer: selfOf(b) });
            send(b.ws, { type: 'start_call', initiator: false, peer: selfOf(a) });
            console.log('[ws] 房间 ' + roomId + ' 已满，开始通话（假媒体）');
        }
        return;
    }

    if (msg.type === 'offer' || msg.type === 'answer' || msg.type === 'ice' ||
        msg.type === 'video_mute' || msg.type === 'mic_mute') {
        const peer = peerOf(s);
        if (peer) send(peer.ws, msg);
        return;
    }

    if (msg.type === 'bye') {
        const peer = peerOf(s);
        leaveRoom(s);
        send(ws, { type: 'bye_ack' });
        if (peer) send(peer.ws, { type: 'bye' });
        return;
    }
}

// ---------------------------------------------------------------
// 启动
// ---------------------------------------------------------------
const server = http.createServer((req, res) => {
    if (req.url.startsWith('/api/')) return proxyApi(req, res);
    return serveStatic(req, res);
});

const wss = new WebSocketServer({ noServer: true });

server.on('upgrade', (req, socket, head) => {
    if (!req.url.startsWith('/ws')) {
        socket.destroy();
        return;
    }
    wss.handleUpgrade(req, socket, head, (ws) => wss.emit('connection', ws, req));
});

wss.on('connection', (ws) => {
    const s = { ws: ws, authed: false, user: null, roomId: null, deadline: Date.now() + AUTH_TIMEOUT_MS };
    sessions.add(s);

    ws.on('message', (data) => handleMessage(s, data.toString()));
    ws.on('close', () => {
        leaveRoom(s);
        sessions.delete(s);
    });
    ws.on('error', () => {
        leaveRoom(s);
        sessions.delete(s);
    });
});

// 鉴权超时回收（与 C++ 侧行为一致）
setInterval(() => {
    const now = Date.now();
    for (const s of sessions) {
        if (!s.authed && s.deadline > 0 && now > s.deadline) {
            console.log('[ws] 鉴权超时，断开连接');
            sendError(s.ws, 'auth_timeout', '建连后 ' + Math.round(AUTH_TIMEOUT_MS / 1000) + ' 秒内未发送 auth');
            s.deadline = 0;
            try { s.ws.close(1000, 'auth_timeout'); } catch (e) { /* 忽略 */ }
        }
    }
}, 500);


server.listen(PORT, '127.0.0.1', () => {
    console.log('=========================================');
    console.log('本地开发网关已启动');
    console.log('  页面地址   http://127.0.0.1:' + PORT + '/login.html');
    console.log('  静态目录   ' + STATIC_DIR);
    console.log('  /api 反代  ' + AUTH_URL);
    console.log('  假信令     ws://127.0.0.1:' + PORT + '/ws');
    console.log('  鉴权超时   ' + AUTH_TIMEOUT_MS + ' ms');
    console.log('=========================================');
});
