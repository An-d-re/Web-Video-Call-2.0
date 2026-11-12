#!/usr/bin/env node
'use strict';

/**
 * 端到端自检：通过本地开发网关验证
 *   账号（注册/登录/刷新轮换）
 *   房间（创建带标题密码 / 列表 / 我的房间 / 加入校验 / 解散）
 *   信令（auth → roomToken 入房 → 配对 → 幂等 → 超时）
 *
 * 前提：PostgreSQL、auth 服务（8080）、开发网关（8890）都在运行。
 * 用法：node e2e-test.js
 */

const WebSocket = require('ws');

const PORT = Number(process.env.GATEWAY_PORT || 8890);
const BASE = 'http://127.0.0.1:' + PORT;
const WS_URL = 'ws://127.0.0.1:' + PORT + '/ws';
const PASSWORD = 's3cret-pass';

let passed = 0;
let failed = 0;
const failures = [];

function ok(name, detail) {
    passed++;
    console.log('PASS  ' + name + (detail ? '  (' + detail + ')' : ''));
}
function fail(name, detail) {
    failed++;
    failures.push(name + ': ' + detail);
    console.log('FAIL  ' + name + '  → ' + detail);
}
function check(name, cond, detail) {
    if (cond) ok(name, detail); else fail(name, detail || '断言不成立');
}
function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

async function httpReq(method, path, opts) {
    opts = opts || {};
    const headers = Object.assign({}, opts.headers);
    let body;
    if (opts.body !== undefined) {
        headers['Content-Type'] = 'application/json';
        body = JSON.stringify(opts.body);
    }
    const res = await fetch(BASE + path, { method, headers, body });
    let json = null;
    const text = await res.text();
    if (text) { try { json = JSON.parse(text); } catch (e) { json = null; } }
    return { status: res.status, json, setCookies: res.headers.getSetCookie ? res.headers.getSetCookie() : [] };
}

function rtCookie(setCookies) {
    for (const c of setCookies) {
        const first = c.split(';')[0].trim();
        if (first.startsWith('rt=')) return first;
    }
    return null;
}

function errCode(res) {
    const e = res && res.json && res.json.error;
    return (e && e.code) || '';
}

class WsClient {
    constructor(ws) {
        this.ws = ws;
        this.messages = [];
        this.waiters = [];
        ws.on('message', (d) => {
            let m;
            try { m = JSON.parse(d.toString()); } catch (e) { return; }
            this.messages.push(m);
            const pending = this.waiters.splice(0);
            for (const w of pending) w();
        });
    }

    static connect() {
        return new Promise((resolve, reject) => {
            const ws = new WebSocket(WS_URL);
            const timer = setTimeout(() => { ws.terminate(); reject(new Error('WS 连接超时')); }, 5000);
            ws.on('open', () => { clearTimeout(timer); resolve(new WsClient(ws)); });
            ws.on('error', (e) => { clearTimeout(timer); reject(e); });
        });
    }

    send(obj) { this.ws.send(JSON.stringify(obj)); }

    waitFor(pred, ms) {
        const found = this.messages.find(pred);
        if (found) return Promise.resolve(found);
        return this._wait(pred, ms, 0);
    }

    waitForNew(pred, ms) {
        return this._wait(pred, ms, this.messages.length);
    }

    _wait(pred, ms, start) {
        const timeout = ms || 3000;
        const found = this.messages.slice(start).find(pred);
        if (found) return Promise.resolve(found);

        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error('等待消息超时（' + timeout + 'ms）')), timeout);
            const tick = () => {
                const m = this.messages.slice(start).find(pred);
                if (m) { clearTimeout(timer); resolve(m); }
                else this.waiters.push(tick);
            };
            this.waiters.push(tick);
        });
    }

    close() { try { this.ws.close(); } catch (e) { /* 忽略 */ } }
}

async function registerUser(name, displayName) {
    const r = await httpReq('POST', '/api/register', {
        body: { username: name, password: PASSWORD, displayName: displayName },
    });
    if (r.status !== 201) throw new Error('注册 ' + name + ' 失败: ' + r.status);
    return r.json.accessToken;
}

async function main() {
    const stamp = Date.now().toString(36);
    const userA = 'e2e_a_' + stamp;
    const userB = 'e2e_b_' + stamp;

    console.log('=== 网关: ' + BASE + ' ===\n');

    // ============ 账号 ============
    console.log('--- 账号 ---');

    let r = await httpReq('GET', '/api/health');
    check('1. 健康检查', r.status === 200 && r.json.db === 'ok', 'HTTP ' + r.status);

    r = await httpReq('POST', '/api/register', {
        body: { username: userA, password: PASSWORD, displayName: '房主阿伟' },
    });
    check('2. 注册', r.status === 201 && !!r.json.accessToken, 'HTTP ' + r.status);
    let tokenA = r.json.accessToken;

    r = await httpReq('POST', '/api/login', { body: { username: userA, password: 'wrong-pass' } });
    check('3. 密码错误被拒', r.status === 401 && errCode(r) === 'invalid_credentials', 'HTTP ' + r.status);

    r = await httpReq('POST', '/api/login', { body: { username: userA, password: PASSWORD } });
    const cookieA = rtCookie(r.setCookies);
    check('4. 登录 + 下发 httpOnly Cookie', r.status === 200 && !!cookieA &&
        r.setCookies.join(';').includes('Path=/api'), cookieA ? 'rt=…' : '未下发');

    r = await httpReq('GET', '/api/me', { headers: { Authorization: 'Bearer ' + tokenA } });
    check('5. /api/me', r.status === 200 && r.json.user.username === userA, 'HTTP ' + r.status);

    // ============ 房间 ============
    console.log('\n--- 房间 ---');

    r = await httpReq('POST', '/api/rooms', {
        headers: { Authorization: 'Bearer ' + tokenA },
        body: { title: '阿伟的会议室' },
    });
    const roomOpen = r.json && r.json.room;
    let openToken = r.json && r.json.roomToken;
    check('6. 创建无密码房间', r.status === 201 && !!roomOpen && roomOpen.id.length === 6,
        roomOpen ? roomOpen.id + ' / ' + roomOpen.title : ('HTTP ' + r.status));
    check('6b. 无密码房间 hasPassword=false', !!roomOpen && roomOpen.hasPassword === false,
        roomOpen ? String(roomOpen.hasPassword) : '-');

    r = await httpReq('POST', '/api/rooms', {
        headers: { Authorization: 'Bearer ' + tokenA },
        body: { title: '周三组会', password: 'ab12' },
    });
    const roomLocked = r.json && r.json.room;
    check('7. 创建加密房间', r.status === 201 && roomLocked && roomLocked.hasPassword === true,
        roomLocked ? roomLocked.id + ' 加密=' + roomLocked.hasPassword : ('HTTP ' + r.status));

    r = await httpReq('POST', '/api/rooms', {
        headers: { Authorization: 'Bearer ' + tokenA },
        body: { title: '短密码', password: 'ab' },
    });
    check('8. 房间密码过短被拒', r.status === 422 && errCode(r) === 'invalid_room_password',
        'HTTP ' + r.status + '/' + errCode(r));

    r = await httpReq('GET', '/api/rooms', { headers: { Authorization: 'Bearer ' + tokenA } });
    const allIds = (r.json.rooms || []).map((x) => x.id);
    check('9. 房间列表包含新建的两个',
        r.status === 200 && allIds.indexOf(roomOpen.id) >= 0 && allIds.indexOf(roomLocked.id) >= 0,
        '共 ' + allIds.length + ' 个房间');

    // B 加入 open 房间后再看"我的房间"
    tokenB = await registerUser(userB, '客人小明');
    r = await httpReq('POST', '/api/rooms/' + roomOpen.id + '/join', {
        headers: { Authorization: 'Bearer ' + tokenB }, body: { password: '' },
    });
    const bOpenToken = r.json && r.json.roomToken;
    check('10. B 加入无密码房间', r.status === 200 && !!bOpenToken, 'HTTP ' + r.status);

    r = await httpReq('GET', '/api/rooms/mine', { headers: { Authorization: 'Bearer ' + tokenB } });
    const mineIds = (r.json.rooms || []).map((x) => x.id);
    check('11. 我的房间记录了加入过的房间',
        r.status === 200 && mineIds.indexOf(roomOpen.id) >= 0,
        'B 记录 ' + mineIds.length + ' 个');

    r = await httpReq('POST', '/api/rooms/ZZZZZZ/join', {
        headers: { Authorization: 'Bearer ' + tokenB }, body: { password: '' },
    });
    check('12. 加入不存在的房间被拒', r.status === 404 && errCode(r) === 'room_not_found',
        'HTTP ' + r.status + '/' + errCode(r));

    r = await httpReq('POST', '/api/rooms/' + roomLocked.id + '/join', {
        headers: { Authorization: 'Bearer ' + tokenB }, body: { password: 'xx99' },
    });
    check('13. 房间密码错误被拒', r.status === 403 && errCode(r) === 'bad_password',
        'HTTP ' + r.status + '/' + errCode(r));

    r = await httpReq('POST', '/api/rooms/' + roomLocked.id + '/join', {
        headers: { Authorization: 'Bearer ' + tokenB }, body: { password: 'ab12' },
    });
    const bLockedToken = r.json && r.json.roomToken;
    check('14. 房间密码正确可加入', r.status === 200 && !!bLockedToken, 'HTTP ' + r.status);

    // ============ 信令 ============
    console.log('\n--- 信令鉴权与入房 ---');

    const c1 = await WsClient.connect();
    c1.send({ type: 'auth', token: tokenA });
    let m = await c1.waitFor((x) => x.type === 'auth_ok' || x.type === 'error');
    check('15. WS 鉴权', m.type === 'auth_ok' && m.displayName === '房主阿伟',
        m.type === 'auth_ok' ? m.displayName : (m.code || ''));

    // 用登录凭证冒充进房凭证，必须被拒（类型隔离）
    c1.send({ type: 'join_room', roomToken: tokenA });
    m = await c1.waitForNew((x) => x.type === 'joined' || x.type === 'error');
    check('16. 登录凭证不能当进房凭证', m.type === 'error' && m.code === 'invalid_room_token',
        m.type + '/' + (m.code || ''));

    c1.send({ type: 'join_room', roomToken: openToken });
    m = await c1.waitForNew((x) => x.type === 'joined' || x.type === 'error');
    check('17. 用 roomToken 入房', m.type === 'joined' && m.roomId === roomOpen.id && m.inCall === false,
        m.type === 'joined' ? 'roomId=' + m.roomId + ' inCall=' + m.inCall : (m.code || ''));

    c1.send({ type: 'list_rooms' });
    m = await c1.waitForNew((x) => x.type === 'live_rooms');
    const live = (m.rooms || []).find((x) => x.roomId === roomOpen.id);
    check('18. 实时房间列表显示在线人数', !!live && live.peers === 1,
        live ? 'peers=' + live.peers : '未出现在列表');

    // 第二个人进来 → 双方 start_call
    const c2 = await WsClient.connect();
    c2.send({ type: 'auth', token: tokenB });
    await c2.waitFor((x) => x.type === 'auth_ok' || x.type === 'error');
    c2.send({ type: 'join_room', roomToken: bOpenToken });

    const sc1 = await c1.waitForNew((x) => x.type === 'start_call', 3000).catch(() => null);
    const sc2 = await c2.waitFor((x) => x.type === 'start_call', 3000).catch(() => null);
    check('19. 房间满后双方收到 start_call', !!sc1 && !!sc2, sc1 ? '双方均收到' : '未收到');
    check('19b. start_call 带对端身份',
        !!sc1 && sc1.peer && sc1.peer.displayName === '客人小明' && sc1.initiator === true,
        sc1 && sc1.peer ? 'peer=' + sc1.peer.displayName : '无 peer');

    // 重复入房：幂等，且不能再触发 start_call
    const before = c1.messages.filter((x) => x.type === 'start_call').length;
    c1.send({ type: 'join_room', roomToken: openToken });
    m = await c1.waitForNew((x) => x.type === 'joined' || x.type === 'error');
    await sleep(300);
    const after = c1.messages.filter((x) => x.type === 'start_call').length;
    check('20. 重复入房返回 inCall=true', m.type === 'joined' && m.inCall === true,
        m.type + ' inCall=' + m.inCall);
    check('20b. 重复入房不再触发 start_call', before === after, before + ' → ' + after);

    // 未鉴权就入房
    const c3 = await WsClient.connect();
    c3.send({ type: 'join_room', roomToken: openToken });
    m = await c3.waitFor((x) => x.type === 'error', 3000);
    check('21. 未鉴权入房被拒', m.code === 'unauthorized', 'code=' + m.code);
    c3.close();

    // 鉴权超时
    const t0 = Date.now();
    const c4 = await WsClient.connect();
    m = await c4.waitFor((x) => x.type === 'error', 8000).catch(() => null);
    const elapsed = Date.now() - t0;
    check('22. 未鉴权超时被断开',
        !!m && m.code === 'auth_timeout' && elapsed >= 4500 && elapsed <= 7500,
        m ? m.code + ' 用时 ' + elapsed + 'ms' : '未收到错误');
    c4.close();

    // ============ 解散 ============
    console.log('\n--- 解散房间 ---');

    r = await httpReq('DELETE', '/api/rooms/' + roomOpen.id, {
        headers: { Authorization: 'Bearer ' + tokenB },
    });
    check('23. 非创建者不能解散', r.status === 403 && errCode(r) === 'not_owner',
        'HTTP ' + r.status + '/' + errCode(r));

    r = await httpReq('DELETE', '/api/rooms/' + roomOpen.id, {
        headers: { Authorization: 'Bearer ' + tokenA },
    });
    check('24. 创建者可以解散', r.status === 204, 'HTTP ' + r.status);

    r = await httpReq('POST', '/api/rooms/' + roomOpen.id + '/join', {
        headers: { Authorization: 'Bearer ' + tokenB }, body: { password: '' },
    });
    check('25. 解散后房间不存在', r.status === 404, 'HTTP ' + r.status);

    c1.close();
    c2.close();

    // ============ 登出 ============
    console.log('\n--- 登出 ---');
    r = await httpReq('POST', '/api/logout', { headers: { Cookie: cookieA } });
    check('26. 登出', r.status === 204, 'HTTP ' + r.status);
    r = await httpReq('POST', '/api/refresh', { headers: { Cookie: cookieA } });
    check('27. 登出后刷新被拒', r.status === 401, 'HTTP ' + r.status);

    // ============ 汇总 ============
    console.log('\n=========================================');
    console.log('结果: ' + passed + ' 通过, ' + failed + ' 失败');
    if (failed > 0) {
        console.log('失败项:');
        for (const f of failures) console.log('  · ' + f);
    }
    console.log('=========================================');
    process.exit(failed === 0 ? 0 : 1);
}

let tokenB = null;

main().catch((e) => {
    console.error('\n测试异常终止: ' + e.message);
    process.exit(2);
});
