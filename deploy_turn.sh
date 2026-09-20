#!/bin/bash

# WebRTC TURN服务器快速部署脚本
# 使用方式: chmod +x deploy_turn.sh && ./deploy_turn.sh

set -e

echo "=========================================="
echo "WebRTC TURN服务器一键部署脚本"
echo "=========================================="
echo ""

# 检查是否为root
if [[ $EUID -ne 0 ]]; then
   echo "❌ 错误：本脚本需要root权限"
   echo "请使用: sudo ./deploy_turn.sh"
   exit 1
fi

# 获取用户输入
read -p "请输入你的公网IP地址: " PUBLIC_IP
read -p "请输入你的域名 (例如: turn.example.com): " DOMAIN
read -p "请输入TURN服务用户名 (默认: webrtc): " USERNAME
USERNAME=${USERNAME:-webrtc}
read -sp "请输入TURN服务密码: " PASSWORD
echo ""

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

# 备份原配置
echo "💾 备份原配置文件..."
cp /etc/coturn/turnserver.conf /etc/coturn/turnserver.conf.backup

# 生成新配置
echo "⚙️  生成TURN配置..."
cat > /etc/coturn/turnserver.conf << EOF
# ===== 基础配置 =====
listening-port=3478
listening-ip=0.0.0.0
relay-ip=$PUBLIC_IP
external-ip=$PUBLIC_IP
server-name=$DOMAIN
realm=webrtc-call

# ===== TURN协议 =====
user=$USERNAME:$PASSWORD
userdb=/var/lib/coturn/turnuserdb.conf

# ===== 性能优化 =====
max-bps=0
bps-capacity=0
mc-sec=600
min-port=49152
max-port=65535
thread-number=$(nproc)

# ===== 日志 =====
log-file=/var/log/coturn/turnserver.log
pidfile=/var/run/coturn/turnserver.pid
verbose

# ===== 安全 =====
no-multicast
no-rfc5780
fingerprint
EOF

# 创建用户数据库
echo "👤 创建用户认证..."
turnadmin -a -u $USERNAME -r $DOMAIN -p $PASSWORD

# 启动服务
echo "🚀 启动TURN服务..."
systemctl start coturn
systemctl enable coturn

# 验证
echo ""
echo "✅ TURN服务部署完成！"
echo ""
echo "=========================================="
echo "配置信息："
echo "  STUN/TURN: turn:$PUBLIC_IP:3478"
echo "  域名: turn:$DOMAIN:3478"
echo "  用户名: $USERNAME"
echo "  密码: $PASSWORD"
echo "=========================================="
echo ""
echo "📝 下一步："
echo ""
echo "1️⃣  更新前端配置 (static/index.html):"
echo ""
echo "   const ICE_CONFIG = {"
echo "       iceServers: ["
echo "           { urls: 'stun:stun.qq.com:3478' },"
echo "           {"
echo "               urls: 'turn:$PUBLIC_IP:3478',"
echo "               username: '$USERNAME',"
echo "               credential: '$PASSWORD'"
echo "           }"
echo "       ]"
echo "   };"
echo ""
echo "2️⃣  测试连接:"
echo ""
echo "   turnutils_uclient -v -u $USERNAME -w $PASSWORD $PUBLIC_IP"
echo ""
echo "3️⃣  查看日志:"
echo ""
echo "   tail -f /var/log/coturn/turnserver.log"
echo ""
echo "4️⃣  监听端口状态:"
echo ""
echo "   netstat -anup | grep turnserver"
echo ""

# 输出JSON配置（方便复制）
echo "📋 JSON配置（前端使用）:"
echo ""
cat << EOF_JSON
{
    "iceServers": [
        {
            "urls": "stun:stun.qq.com:3478"
        },
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
echo "✨ 部署完成！祝您使用愉快！"
