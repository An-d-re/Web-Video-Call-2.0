#!/bin/bash

# WebRTC TURN 服务器快速部署脚本
# 使用方式: chmod +x deploy_turn.sh && sudo ./deploy_turn.sh
#
# 注意：本文件必须保持 LF 换行（.gitattributes 已固定 *.sh eol=lf）。
#       CRLF 会让 Linux 报 "bad interpreter: /bin/bash^M"。

set -e

echo "=========================================="
echo "WebRTC TURN 服务器一键部署脚本"
echo "=========================================="
echo ""

# 检查是否为root
if [[ $EUID -ne 0 ]]; then
   echo "❌ 错误：本脚本需要root权限"
   echo "请使用: sudo ./deploy_turn.sh"
   exit 1
fi

# ---------- 收集参数 ----------
read -p "请输入你的公网IP地址: " PUBLIC_IP
if [[ -z "$PUBLIC_IP" ]]; then
    echo "❌ 公网IP不能为空"
    exit 1
fi

# 自动探测本机网卡地址，作为内网IP的默认值
LOCAL_IP=$(ip route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="src"){print $(i+1); exit}}')
LOCAL_IP=${LOCAL_IP:-$(hostname -I 2>/dev/null | awk '{print $1}')}

read -p "请输入本机内网IP（云主机 EIP 经 NAT 映射时填内网IP；公网IP直接绑在网卡上则与公网IP相同）[${LOCAL_IP}]: " PRIVATE_IP
PRIVATE_IP=${PRIVATE_IP:-$LOCAL_IP}
if [[ -z "$PRIVATE_IP" ]]; then
    echo "❌ 无法自动探测内网IP，请手动输入"
    exit 1
fi

read -p "请输入你的域名 (例如: turn.example.com): " DOMAIN
DOMAIN=${DOMAIN:-$PUBLIC_IP}

read -p "请输入TURN服务用户名 (默认: webrtc): " USERNAME
USERNAME=${USERNAME:-webrtc}
read -sp "请输入TURN服务密码: " PASSWORD
echo ""
if [[ -z "$PASSWORD" ]]; then
    echo "❌ 密码不能为空。注意：这个凭据会被下发给每一个客户端，请使用强口令。"
    exit 1
fi

# relay-ip 必须是本机网卡上真实存在的地址；external-ip 是外界看到的地址。
# 云主机 EIP 通过 NAT 映射时两者不同，coturn 支持 external-ip=public/private 的写法。
if [[ "$PUBLIC_IP" == "$PRIVATE_IP" ]]; then
    RELAY_IP="$PUBLIC_IP"
    EXTERNAL_IP="$PUBLIC_IP"
else
    RELAY_IP="$PRIVATE_IP"
    EXTERNAL_IP="$PUBLIC_IP/$PRIVATE_IP"
fi

# 检测操作系统
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
fi

echo ""
echo "=========================================="
echo "系统信息："
echo "  OS: $OS"
echo "  公网IP: $PUBLIC_IP"
echo "  内网IP: $PRIVATE_IP"
echo "  relay-ip: $RELAY_IP"
echo "  external-ip: $EXTERNAL_IP"
echo "  域名: $DOMAIN"
echo "  用户: $USERNAME"
echo "=========================================="
echo ""

# 安装coturn
echo "📦 安装coturn..."
if [[ "$OS" == "ubuntu" ]] || [[ "$OS" == "debian" ]]; then
    apt update
    apt install -y coturn
elif [[ "$OS" == "centos" ]] || [[ "$OS" == "rhel" ]] || [[ "$OS" == "fedora" ]]; then
    yum install -y coturn
else
    echo "❌ 不支持的操作系统: $OS"
    exit 1
fi

# Debian/Ubuntu 的 coturn 默认在 /etc/default/coturn 里被禁用，
# 不打开这个开关 systemctl start coturn 会"成功"但服务并不运行。
if [ -f /etc/default/coturn ]; then
    echo "🔓 启用 coturn 服务开关 (/etc/default/coturn)..."
    if grep -qE '^[[:space:]]*#?[[:space:]]*TURNSERVER_ENABLED=' /etc/default/coturn; then
        sed -i 's/^[[:space:]]*#\?[[:space:]]*TURNSERVER_ENABLED=.*/TURNSERVER_ENABLED=1/' /etc/default/coturn
    else
        echo 'TURNSERVER_ENABLED=1' >> /etc/default/coturn
    fi
fi

# 备份原配置（带时间戳，重复执行不会覆盖最初的那份备份）
if [ -f /etc/coturn/turnserver.conf ]; then
    BACKUP="/etc/coturn/turnserver.conf.bak.$(date +%Y%m%d-%H%M%S)"
    echo "💾 备份原配置文件到 $BACKUP"
    cp -a /etc/coturn/turnserver.conf "$BACKUP"
fi

# 生成新配置
echo "⚙️  生成TURN配置..."
cat > /etc/coturn/turnserver.conf << EOF
# ===== 基础配置 =====
listening-port=3478
listening-ip=0.0.0.0
# relay-ip 必须是本机网卡上真实存在的地址；填错了 coturn 会起不来
relay-ip=$RELAY_IP
# external-ip 支持 public/private 形式，coturn 会自动做地址映射
external-ip=$EXTERNAL_IP
server-name=$DOMAIN
realm=webrtc-call

# ===== 认证 =====
# 静态账号：coturn 用「用户名:realm:密码」校验，realm 就是上面那一行，
# 不需要再跑 turnadmin/userdb（那才是之前 realm 对不上、认证失败的根源）。
# 注意：这份口令会通过 turn_config.json 下发给每一个客户端，务必用强口令，
#      更稳妥的做法是改成 use-auth-secret + REST 临时凭据（见脚本末尾说明）。
user=$USERNAME:$PASSWORD

# ===== 性能优化 =====
# 0 表示不限制带宽。凭据一旦泄露就可能被刷流量，建议配合临时凭据使用。
max-bps=0
bps-capacity=0
min-port=49152
max-port=65535
thread-number=$(nproc)

# ===== 日志 =====
log-file=/var/log/coturn/turnserver.log
pidfile=/var/run/coturn/turnserver.pid
verbose

# ===== 安全 =====
# 正确选项名是 no-multicast-peers（写成 no-multicast 会导致配置读取失败）
no-multicast-peers
no-rfc5780
fingerprint
EOF

mkdir -p /var/log/coturn /var/run/coturn
chown turnserver:turnserver /var/log/coturn /var/run/coturn 2>/dev/null || true

# 启动服务
echo "🚀 启动TURN服务..."
systemctl daemon-reload
systemctl restart coturn
systemctl enable coturn

# 启动校验：配置写错、relay-ip 绑不上、端口被占用都会在这里暴露
if ! systemctl is-active --quiet coturn; then
    echo ""
    echo "❌ coturn 启动失败，最近日志："
    echo "------------------------------------------"
    journalctl -u coturn -n 30 --no-pager || true
    tail -n 30 /var/log/coturn/turnserver.log 2>/dev/null || true
    echo "------------------------------------------"
    echo "常见原因：relay-ip 不是本机网卡地址 / 3478 端口被占用 / 配置文件有非法选项"
    exit 1
fi
echo "✅ coturn 已启动"

if ss -lunp 2>/dev/null | grep -q ':3478'; then
    echo "✅ 3478/udp 正在监听"
else
    echo "⚠️  未检测到 3478/udp 监听，请检查日志"
fi

# ---------- 放行防火墙 ----------
echo ""
read -p "是否自动放行本机防火墙端口 3478/udp、3478/tcp、49152-65535/udp? (y/N): " OPEN_FW
if [[ "$OPEN_FW" =~ ^[Yy]$ ]]; then
    if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "Status: active"; then
        ufw allow 3478/udp
        ufw allow 3478/tcp
        ufw allow 49152:65535/udp
        echo "✅ 已放行 ufw 端口"
    elif command -v firewall-cmd >/dev/null 2>&1 && systemctl is-active --quiet firewalld; then
        firewall-cmd --permanent --add-port=3478/udp
        firewall-cmd --permanent --add-port=3478/tcp
        firewall-cmd --permanent --add-port=49152-65535/udp
        firewall-cmd --reload
        echo "✅ 已放行 firewalld 端口"
    else
        echo "⚠️  未检测到运行中的 ufw/firewalld，请自行放行 3478/udp、3478/tcp、49152-65535/udp"
    fi
else
    echo "⏭️  跳过防火墙配置"
fi

echo ""
echo "=========================================="
echo "✅ TURN服务部署完成！"
echo ""
echo "配置信息："
echo "  STUN/TURN: turn:$PUBLIC_IP:3478"
echo "  内网中继地址: $RELAY_IP"
echo "  用户名: $USERNAME"
echo "=========================================="
echo ""
echo "⚠️  必做：云厂商安全组/网络ACL 必须放行"
echo "     3478/udp、3478/tcp 和 49152-65535/udp，"
echo "     这一步脚本改不了，漏了会表现为「部署成功但通话不通」。"
echo ""
echo "📝 下一步："
echo ""
echo "1️⃣  把下面的 JSON 写入服务器上的 turn_config.json（该文件已从 git 移除，不要提交）："
echo ""
cat << EOF_JSON
{
    "iceServers": [
        { "urls": "stun:stun.qq.com:3478" },
        {
            "urls": [
                "turn:$PUBLIC_IP:3478?transport=udp",
                "turn:$PUBLIC_IP:3478?transport=tcp"
            ],
            "username": "$USERNAME",
            "credential": "$PASSWORD"
        }
    ]
}
EOF_JSON
echo ""
echo "   信令服务启动时会读取该文件，并在 start_call 时下发 ICE 配置给前端，"
echo "   前端 static/index.html 里不需要再硬编码凭据。"
echo ""
echo "2️⃣  测试连接:"
echo ""
echo "   turnutils_uclient -v -u $USERNAME -w '$PASSWORD' -p 3478 $PUBLIC_IP"
echo ""
echo "3️⃣  查看日志:"
echo ""
echo "   tail -f /var/log/coturn/turnserver.log"
echo ""
echo "4️⃣  监听端口状态:"
echo ""
echo "   ss -lunp | grep turnserver"
echo ""
echo "5️⃣  更安全（强烈建议后续改造）：REST 临时凭据"
echo "   在 turnserver.conf 中改用："
echo "       use-auth-secret"
echo "       static-auth-secret=<随机长字符串>"
echo "   由信令服务为每次通话生成短时效凭据："
echo "       username   = \"<过期unix时间戳>:<任意标识>\""
echo "       credential = base64(HMAC-SHA1(static-auth-secret, username))"
echo "   这样即使前端拿到凭据也会很快过期，无法被盗用刷流量。"
echo ""
echo "✨ 部署完成！祝您使用愉快！"
