#!/usr/bin/env bash
# WebRTC 视频通话 —— 一键安装（Ubuntu 22.04/24.04）
#
# 做什么：
#   1. 建系统用户 webrtc、部署目录 /opt/webrtc
#   2. 放两个二进制（C++ 信令、Go 认证）+ 前端静态文件
#   3. 生成 /opt/webrtc/{auth,signaling}.env（自动生成 JWT_SECRET，权限 600）
#   4. 生成带 IP SAN 的自签证书（浏览器点"继续前往"即可用摄像头）
#   5. 装 nginx 站点（80 跳 443；/ → 静态页，/api/ → Go，/ws → C++）
#   6. 装 coturn 并生成 /opt/webrtc/turn_config.json（动态 TURN 凭据）
#   7. 跑数据库迁移（需要 DATABASE_URL）
#   8. systemd 拉起 webrtc-auth + webrtc-signaling，开机自启
#
# 用法（在仓库根目录）：
#   sudo DATABASE_URL='postgres://webrtc:pw@127.0.0.1:5432/webrtc?sslmode=disable' \
#        ./deploy/install.sh --server ./server --auth ./auth
#
# 幂等：重复执行会更新二进制与配置，但**不会**覆盖已存在的 env 文件里的密钥
#        （否则每次重装都会让所有已登录用户掉线）。
#
# 可选环境变量：
#   DATABASE_URL  数据库连接串（缺省则 auth.env 写占位值、跳过迁移）
#   TURN_SECRET   coturn 的 static-auth-secret（缺省自动生成，重装时复用旧的）
#   TURN_PUBLIC_IP  云主机在 NAT 后面时填公网 IP（写进 coturn 的 external-ip，
#                   并作为 turn_config.json 里的地址；不填则探测本机网卡）
#
# 可选参数：--server <bin> --auth <bin> --static <dir> --prefix <dir> --skip-db --no-turn
set -euo pipefail

PREFIX=/opt/webrtc
SERVER_BIN=""
AUTH_BIN=""
STATIC_DIR=""
SKIP_DB=0
ENABLE_TURN=1

log()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)   SERVER_BIN="$2"; shift 2 ;;
        --auth)     AUTH_BIN="$2"; shift 2 ;;
        --static)   STATIC_DIR="$2"; shift 2 ;;
        --prefix)   PREFIX="$2"; shift 2 ;;
        --skip-db)  SKIP_DB=1; shift ;;
        --no-turn)  ENABLE_TURN=0; shift ;;
        -h|--help)  sed -n '2,/^set -euo/p' "$0"; exit 0 ;;
        *) die "未知参数：$1" ;;
    esac
done

[[ $EUID -eq 0 ]] || die "需要 root：sudo $0 ..."

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
[[ -n "$SERVER_BIN" ]] || SERVER_BIN="$REPO_DIR/server"
[[ -n "$AUTH_BIN"   ]] || AUTH_BIN="$REPO_DIR/auth"
[[ -n "$STATIC_DIR" ]] || STATIC_DIR="$REPO_DIR/static"

[[ -f "$SERVER_BIN" ]] || die "找不到信令二进制：$SERVER_BIN（先在仓库根目录 cmake --build build）"
[[ -f "$AUTH_BIN"   ]] || die "找不到认证二进制：$AUTH_BIN（GOOS=linux GOARCH=amd64 go build -o auth ./auth）"
[[ -d "$STATIC_DIR" ]] || die "找不到静态目录：$STATIC_DIR"

# ---------- 1. 系统用户与目录 ----------
log "准备系统用户与目录 $PREFIX"
if ! id webrtc >/dev/null 2>&1; then
    useradd --system --home-dir "$PREFIX" --shell /usr/sbin/nologin webrtc
    log "已创建系统用户 webrtc"
fi
install -d -o webrtc -g webrtc -m 0755 "$PREFIX"

# ---------- 2. 二进制与静态文件 ----------
log "部署二进制与静态文件"
install -o webrtc -g webrtc -m 0755 "$SERVER_BIN" "$PREFIX/server"
install -o webrtc -g webrtc -m 0755 "$AUTH_BIN"   "$PREFIX/auth"
rm -rf "$PREFIX/static.new"
cp -a "$STATIC_DIR" "$PREFIX/static.new"
chown -R webrtc:webrtc "$PREFIX/static.new"
rm -rf "$PREFIX/static"
mv "$PREFIX/static.new" "$PREFIX/static"

# ---------- 2.5 TURN 中继（coturn）----------
# 为什么需要：两端不在同一网段（比如一端连手机热点）时 P2P 打洞经常失败，
# 媒体必须靠中继转发。凭据用 coturn 的 TURN REST API（use-auth-secret）：
# 长期密钥只留在服务器上，信令服务在每次 start_call 时签发
# "<过期时间戳>:<userId>" + HMAC-SHA1 凭据，浏览器拿到的最多存活 ttlSeconds。
IP="${TURN_PUBLIC_IP:-$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)}"
[[ -n "$IP" ]] || IP="127.0.0.1"

if [[ $ENABLE_TURN -eq 1 ]] && command -v apt-get >/dev/null 2>&1; then
    if ! dpkg -s coturn >/dev/null 2>&1; then
        log "安装 coturn（TURN 中继）"
        : > /tmp/apt-turn.log
        DEBIAN_FRONTEND=noninteractive apt-get update -qq >>/tmp/apt-turn.log 2>&1 || true
        turn_apt_ok=0
        for attempt in 1 2 3 4 5; do
            if DEBIAN_FRONTEND=noninteractive apt-get install -y -qq coturn >>/tmp/apt-turn.log 2>&1; then
                turn_apt_ok=1
                break
            fi
            # 无人值守升级（unattended-upgrades）经常正好占着 dpkg 锁，等它释放再试
            if grep -qE 'lock-frontend|dpkg 前端锁' /tmp/apt-turn.log 2>/dev/null && [[ $attempt -lt 5 ]]; then
                warn "dpkg 锁被占用（unattended-upgrades？），20 秒后重试（第 $attempt 次）"
                sleep 20
            else
                break
            fi
        done
        if [[ $turn_apt_ok -ne 1 ]]; then
            warn "coturn 安装失败（详见 /tmp/apt-turn.log），跳过 TURN；跨网段通话将无法建立"
            tail -3 /tmp/apt-turn.log >&2 || true
            ENABLE_TURN=0
        fi
    fi
fi

if [[ $ENABLE_TURN -eq 1 ]]; then
    # 复用已有密钥：重装换密钥会让正在通话的客户端凭据立刻失效
    TURN_SECRET="${TURN_SECRET:-}"
    if [[ -z "$TURN_SECRET" && -f "$PREFIX/turn_config.json" ]]; then
        TURN_SECRET="$(grep -oP '"secret"\s*:\s*"\K[^"]+' "$PREFIX/turn_config.json" || true)"
    fi
    if [[ -z "$TURN_SECRET" ]]; then
        TURN_SECRET="$(openssl rand -base64 36 | tr -d '\n/+=')"
    fi

    TURN_EXTERNAL=""
    if [[ -n "${TURN_PUBLIC_IP:-}" ]]; then
        TURN_EXTERNAL="external-ip=$TURN_PUBLIC_IP"
    fi

    cat > /etc/turnserver.conf <<EOF
# 由 WebRTC 项目的 deploy/install.sh 生成
listening-port=3478
# 不开 TLS 监听：页面用的是自签证书，浏览器会拒绝 TURNS 的证书；
# 媒体本身由 DTLS-SRTP 端到端加密，TURN 只转发字节。
fingerprint
use-auth-secret
static-auth-secret=$TURN_SECRET
realm=webrtc.local
# 中继端口范围，需在防火墙放行
min-port=49160
max-port=49200
no-multicast-peers
no-cli
# 云服务器（机器是内网地址、公网 IP 在 NAT 上）必须填公网 IP，
# 否则浏览器拿到的是内网中继地址，连不上。传 TURN_PUBLIC_IP=1.2.3.4 即可。
$TURN_EXTERNAL
EOF
    chown root:turnserver /etc/turnserver.conf 2>/dev/null || true
    chmod 0640 /etc/turnserver.conf

    # Ubuntu 的 coturn 单元默认被 /etc/default/coturn 关掉
    if [[ -f /etc/default/coturn ]]; then
        sed -i 's/^#*TURNSERVER_ENABLED=.*/TURNSERVER_ENABLED=1/' /etc/default/coturn
    else
        echo 'TURNSERVER_ENABLED=1' > /etc/default/coturn
    fi

    if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q 'Status: active'; then
        ufw allow 3478/tcp  >/dev/null 2>&1 || true
        ufw allow 3478/udp  >/dev/null 2>&1 || true
        ufw allow 49160:49200/udp >/dev/null 2>&1 || true
        log "已在 ufw 放行 3478 与 49160-49200"
    fi

    systemctl enable coturn >/dev/null 2>&1 || true
    systemctl restart coturn || warn "coturn 启动失败，检查 journalctl -u coturn"

    # 信令服务的 ICE 配置：dynamic 条目在每次 start_call 时被注入临时凭据
    cat > "$PREFIX/turn_config.json" <<EOF
{
    "secret": "$TURN_SECRET",
    "ttlSeconds": 7200,
    "iceServers": [
        { "urls": "stun:$IP:3478" },
        {
            "urls": [
                "turn:$IP:3478?transport=udp",
                "turn:$IP:3478?transport=tcp"
            ],
            "dynamic": true
        }
    ]
}
EOF
    chown webrtc:webrtc "$PREFIX/turn_config.json"
    chmod 0640 "$PREFIX/turn_config.json"
    log "TURN 已配置：$IP:3478（凭据动态签发，TTL 7200 秒）"
else
    warn "未启用 TURN（--no-turn 或安装失败）：只有 P2P 直连，同网段可用，跨网段会卡在连接中"
fi

# ---------- 3. 环境变量文件 ----------
log "生成环境变量文件"
if [[ -f "$PREFIX/auth.env" ]]; then
    warn "$PREFIX/auth.env 已存在，保留原有密钥（避免让已登录用户全部掉线）"
    JWT_SECRET="$(grep -oP '^JWT_SECRET=\K.*' "$PREFIX/auth.env" || true)"
else
    JWT_SECRET="$(openssl rand -base64 48 | tr -d '\n')"
fi
[[ -n "$JWT_SECRET" ]] || die "无法确定 JWT_SECRET（$PREFIX/auth.env 里没有 JWT_SECRET= 行）"

DB_URL="${DATABASE_URL:-}"
if [[ -z "$DB_URL" ]]; then
    DB_URL="postgres://webrtc:CHANGE_ME@127.0.0.1:5432/webrtc?sslmode=disable"
    warn "未提供 DATABASE_URL，auth.env 里写的是占位值，认证服务会连不上数据库"
fi

if [[ ! -f "$PREFIX/auth.env" ]]; then
    cat > "$PREFIX/auth.env" <<EOF
AUTH_LISTEN=127.0.0.1:8080
DATABASE_URL=$DB_URL
JWT_SECRET=$JWT_SECRET
ACCESS_TOKEN_TTL=2h
REFRESH_TOKEN_TTL=168h
COOKIE_SECURE=true
ALLOW_REGISTRATION=true
TRUST_PROXY=true
EOF
    chown webrtc:webrtc "$PREFIX/auth.env"
    chmod 600 "$PREFIX/auth.env"
fi

if [[ ! -f "$PREFIX/signaling.env" ]]; then
    cat > "$PREFIX/signaling.env" <<EOF
JWT_SECRET=$JWT_SECRET
AUTH_REQUIRED=true
EOF
    chown webrtc:webrtc "$PREFIX/signaling.env"
    chmod 600 "$PREFIX/signaling.env"
fi

# ---------- 4. 自签证书（带 IP SAN）----------
log "生成自签证书（带 IP SAN，浏览器点「继续前往」即可）"
install -d -m 0700 /etc/nginx/ssl
if [[ ! -f /etc/nginx/ssl/webrtc.crt ]]; then
    SAN_LIST="DNS:localhost,IP:127.0.0.1"
    while read -r ip; do
        [[ -n "$ip" ]] && SAN_LIST="$SAN_LIST,IP:$ip"
    done < <(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1)
    openssl req -x509 -newkey rsa:2048 -sha256 -days 825 -nodes \
        -keyout /etc/nginx/ssl/webrtc.key -out /etc/nginx/ssl/webrtc.crt \
        -subj "/CN=webrtc.local" \
        -addext "subjectAltName=$SAN_LIST" \
        -addext "basicConstraints=CA:FALSE" \
        -addext "keyUsage=digitalSignature,keyEncipherment" \
        -addext "extendedKeyUsage=serverAuth" >/dev/null 2>&1
    chmod 600 /etc/nginx/ssl/webrtc.key
    log "证书 SAN: $SAN_LIST"
else
    warn "/etc/nginx/ssl/webrtc.crt 已存在，保留（换了 IP 就删掉重跑本脚本）"
fi

# ---------- 5. nginx ----------
log "安装 nginx 主配置与站点"
# 主配置统一由仓库提供：老部署把 server 块直接写死在 nginx.conf 里，
# 那种文件里没有 include sites-enabled，站点配置放进去也不会生效。
if ! grep -q 'sites-enabled' /etc/nginx/nginx.conf 2>/dev/null; then
    cp -a /etc/nginx/nginx.conf "/etc/nginx/nginx.conf.bak.$(date +%Y%m%d%H%M%S)" 2>/dev/null || true
    warn "现有 nginx.conf 没有 include sites-enabled，已备份并替换为仓库版本"
    install -m 0644 "$REPO_DIR/nginx.conf" /etc/nginx/nginx.conf
fi

install -m 0644 "$REPO_DIR/deploy/nginx-webrtc.conf" /etc/nginx/sites-available/webrtc
ln -sf /etc/nginx/sites-available/webrtc /etc/nginx/sites-enabled/webrtc
# 旧的默认站点会和 default_server 抢 80/443
for old in default video; do
    if [[ -L "/etc/nginx/sites-enabled/$old" ]]; then
        rm -f "/etc/nginx/sites-enabled/$old"
        log "已停用旧站点 $old"
    fi
done

if ! nginx -t 2>/tmp/nginx-t.err; then
    cat /tmp/nginx-t.err >&2
    die "nginx 配置校验失败，未重载（改回旧配置再排查）"
fi
systemctl reload nginx
systemctl enable nginx >/dev/null 2>&1 || true

# ---------- 6. 数据库迁移 ----------
if [[ $SKIP_DB -eq 0 ]]; then
    if command -v psql >/dev/null 2>&1 && [[ -n "${DATABASE_URL:-}" ]]; then
        log "执行数据库迁移"
        ( cd "$REPO_DIR" && DATABASE_URL="$DATABASE_URL" bash db/migrate.sh )
    else
        warn "跳过迁移（需要 psql 且设置 DATABASE_URL）"
    fi
else
    warn "按要求跳过数据库迁移（--skip-db）"
fi

# ---------- 7. systemd ----------
log "安装并启动 systemd 服务"
install -m 0644 "$REPO_DIR/deploy/webrtc-auth.service"      /etc/systemd/system/webrtc-auth.service
install -m 0644 "$REPO_DIR/deploy/webrtc-signaling.service" /etc/systemd/system/webrtc-signaling.service
systemctl daemon-reload
systemctl enable webrtc-auth.service webrtc-signaling.service >/dev/null 2>&1 || true
# 必须 restart：enable --now 对"已经在跑"的服务是 no-op，换了二进制也不会生效，
# 表现就是"装完新版本但日志里还是旧行为"。
systemctl restart webrtc-auth.service webrtc-signaling.service
sleep 1
systemctl is-active webrtc-auth.service webrtc-signaling.service || true

echo
log "安装完成"
echo "  访问： https://${IP:-<本机IP>}/"
echo "  自签证书：首次打开会告警，点「高级 → 继续前往」即可（不点的话摄像头/麦克风拿不到）"
echo "  日志：   journalctl -u webrtc-signaling -f    journalctl -u webrtc-auth -f"
if [[ $ENABLE_TURN -eq 1 ]]; then
    echo "  TURN：   ${IP}:3478（udp/tcp），中继端口 49160-49200/udp"
fi
echo
echo "  安全提示：8888（信令）与 8080（认证）是内部端口，正常只应被 nginx 反代。"
echo "  如果机器有公网 IP，建议挡掉直连："
echo "    ufw allow 22,80,443/tcp && ufw deny 8888/tcp && ufw deny 8080/tcp && ufw enable"
