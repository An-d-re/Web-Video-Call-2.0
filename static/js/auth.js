/**
 * 登录与令牌管理（login.html 与 index.html 共用）
 *
 * 安全模型：
 *   - accessToken 只存在内存里 → XSS 拿不到长效凭证
 *   - refreshToken 由服务端写在 httpOnly Cookie 里 → JS 根本读不到
 *   - 页面刷新后，用 Cookie 换回新的 accessToken（不再要求用户重新登录）
 */
(function () {
    'use strict';

    // 生产环境经 nginx 同源走 /api；本地开发时 C++ 静态服务在 8888，auth 服务在 8080。
    // 也可以在页面里提前设置 window.__API_BASE__ 覆盖。
    var API_BASE = window.__API_BASE__ ||
        (location.port === '8888' ? 'http://127.0.0.1:8080' : '/api');

    var accessToken = null;
    var currentUser = null;
    var refreshing = null;  // 单飞：并发 401 时只发一次刷新

    function setSession(data) {
        if (!data) return;
        if (data.accessToken) accessToken = data.accessToken;
        if (data.user) currentUser = data.user;
    }

    function clearSession() {
        accessToken = null;
        currentUser = null;
    }

    /**
     * 底层请求。返回 { status, data }，不抛异常（网络错误也归一成 status=0）。
     */
    function request(path, opts) {
        opts = opts || {};
        var method = opts.method || 'GET';
        var body = (opts.body === undefined) ? null : opts.body;
        var useAuth = opts.auth === true;

        var headers = {};
        if (body !== null) headers['Content-Type'] = 'application/json';
        if (useAuth && accessToken) headers['Authorization'] = 'Bearer ' + accessToken;

        return fetch(API_BASE + path, {
            method: method,
            headers: headers,
            credentials: 'include',   // 带上 httpOnly 的 refresh Cookie
            body: body === null ? undefined : JSON.stringify(body),
        }).then(function (resp) {
            return resp.text().then(function (text) {
                var data = null;
                if (text) {
                    try { data = JSON.parse(text); } catch (e) { data = null; }
                }
                return { status: resp.status, data: data };
            });
        }).catch(function () {
            return { status: 0, data: null };
        });
    }

    function errorOf(res) {
        var e = res.data && res.data.error;
        if (e && e.code) return { code: e.code, message: e.message || '请求失败' };
        if (res.status === 0) return { code: 'network', message: '无法连接服务器' };
        return { code: 'unknown', message: '请求失败（HTTP ' + res.status + '）' };
    }

    /** 用 refresh Cookie 换取新的 accessToken。并发调用共用同一个请求。 */
    function refresh() {
        if (refreshing) return refreshing;

        refreshing = request('/refresh', { method: 'POST' }).then(function (res) {
            if (res.status === 200) {
                setSession(res.data);
                return true;
            }
            clearSession();
            return false;
        }).then(function (ok) {
            refreshing = null;
            return ok;
        }, function () {
            refreshing = null;
            clearSession();
            return false;
        });

        return refreshing;
    }

    /** 带鉴权的请求：401 时自动刷新一次并重试。 */
    function apiFetch(path, opts) {
        opts = opts || {};
        opts.auth = true;
        return request(path, opts).then(function (res) {
            if (res.status !== 401) return res;
            return refresh().then(function (ok) {
                if (!ok) return res;
                return request(path, opts);
            });
        });
    }

    function login(username, password) {
        return request('/login', {
            method: 'POST',
            body: { username: username, password: password },
        }).then(function (res) {
            if (res.status === 200) {
                setSession(res.data);
                return { ok: true };
            }
            return { ok: false, error: errorOf(res) };
        });
    }

    function register(username, password, displayName) {
        return request('/register', {
            method: 'POST',
            body: { username: username, password: password, displayName: displayName },
        }).then(function (res) {
            if (res.status === 200 || res.status === 201) {
                setSession(res.data);
                return { ok: true };
            }
            return { ok: false, error: errorOf(res) };
        });
    }

    function logout() {
        return request('/logout', { method: 'POST' }).then(function () {
            clearSession();
        }, function () {
            clearSession();
        });
    }

    /** 页面加载时恢复会话。返回 Promise<boolean>。 */
    function ensureSession() {
        if (accessToken) return Promise.resolve(true);
        return refresh();
    }

    /** 当前用户信息（从 /me 拉一次，确保是最新的展示名） */
    function fetchMe() {
        return apiFetch('/me').then(function (res) {
            if (res.status === 200 && res.data && res.data.user) {
                currentUser = res.data.user;
                return currentUser;
            }
            return null;
        });
    }

    window.Auth = {
        API_BASE: API_BASE,
        get token() { return accessToken; },
        get user() { return currentUser; },
        login: login,
        register: register,
        logout: logout,
        refresh: refresh,
        ensureSession: ensureSession,
        fetchMe: fetchMe,
        apiFetch: apiFetch,
    };
})();
