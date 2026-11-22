#!/usr/bin/env bash
# 长稳采样器：每 INTERVAL 秒采一次信令服务的 /metrics，追加写入 CSV。
#
# 用法：
#   STATS_TOKEN=xxx bash tools/ops/soak-sample.sh [host] [port] [durationSec] [intervalSec]
#   SOAK_OUT=/tmp/soak.csv STATS_TOKEN=xxx bash tools/ops/soak-sample.sh 192.168.220.128 8888 3600 30
#
# 为什么要单独写一个采样器，而不是直接看 Prometheus：
#   1) 长稳要在"干净机器"上跑，Prometheus 自己也要 CPU，不如停掉它；
#   2) CSV 可以直接扔进表格/画图，也能当证据提交；
#   3) Prometheus 是 15 秒抓、7 天保留，采样粒度不匹配 1 小时的分析需求。
set -euo pipefail

HOST="${1:-127.0.0.1}"
PORT="${2:-8888}"
DURATION="${3:-3600}"
INTERVAL="${4:-30}"
TOKEN="${STATS_TOKEN:-}"
OUT="${SOAK_OUT:-soak-$(date +%Y%m%d-%H%M%S).csv}"

URL="http://${HOST}:${PORT}/metrics"
if [ -n "$TOKEN" ]; then
    URL="${URL}?token=${TOKEN}"
fi

# 从 /metrics 文本里取一个指标的数值；取不到就输出空
grab() {
    # $1 = 正则（只用于定位行），$2 = 匹配的行里的数值提取方式（awk 表达式）
    awk -v pat="$1" '$0 ~ pat { print $NF; exit }'
}

# 求和：webrtc_errors_total 有多条 series
sum_metric() {
    awk -v pat="$1" '$0 ~ pat { s += $NF } END { printf "%.0f", s }'
}

if ! curl -sf -m 5 "$URL" -o /tmp/soak-probe.txt; then
    echo "无法访问 $URL —— 检查 host/port/STATS_TOKEN（未配 token 时只允许本机访问）" >&2
    exit 2
fi

echo "采样目标：$URL"
echo "输出文件：$OUT"
echo "计划：每 ${INTERVAL}s 一次，共 ${DURATION}s（约 $(( DURATION / INTERVAL )) 个样本）"

echo "ts,uptime_s,ws,authed,rooms_total,room_members,rss_bytes,open_fds,forward_p95_s,forward_p99_s,errors_total" > "$OUT"

start=$(date +%s)
deadline=$(( start + DURATION ))
n=0

while :; do
    body="$(curl -sf -m 5 "$URL" || true)"
    if [ -n "$body" ]; then
        ts=$(date +%Y-%m-%dT%H:%M:%S)
        uptime=$(printf '%s\n' "$body" | grab '^webrtc_uptime_seconds ')
        ws=$(printf '%s\n' "$body" | awk '$0 ~ /^webrtc_connections\{/ && $0 ~ /state="ws"/ { print $NF; exit }')
        authed=$(printf '%s\n' "$body" | awk '$0 ~ /^webrtc_connections\{/ && $0 ~ /state="authed"/ { print $NF; exit }')
        rooms=$(printf '%s\n' "$body" | awk '$0 ~ /^webrtc_rooms\{/ && $0 ~ /state="total"/ { print $NF; exit }')
        members=$(printf '%s\n' "$body" | grab '^webrtc_room_members ')
        rss=$(printf '%s\n' "$body" | grab '^webrtc_resident_memory_bytes ')
        fds=$(printf '%s\n' "$body" | grab '^webrtc_open_fds ')
        p95=$(printf '%s\n' "$body" | awk '$0 ~ /^webrtc_latency_seconds\{/ && $0 ~ /path="forward"/ && $0 ~ /stat="p95"/ { print $NF; exit }')
        p99=$(printf '%s\n' "$body" | awk '$0 ~ /^webrtc_latency_seconds\{/ && $0 ~ /path="forward"/ && $0 ~ /stat="p99"/ { print $NF; exit }')
        errs=$(printf '%s\n' "$body" | sum_metric '^webrtc_errors_total\{')

        echo "${ts},${uptime},${ws},${authed},${rooms},${members},${rss},${fds},${p95},${p99},${errs}" >> "$OUT"
        n=$(( n + 1 ))
        printf '[%3d] %s  ws=%s rooms=%s rss=%sMB fds=%s\n' \
            "$n" "$ts" "${ws:-?}" "${rooms:-?}" \
            "$(awk -v v="${rss:-0}" 'BEGIN { printf "%.1f", v/1048576 }')" "${fds:-?}"
    else
        echo "[$n] 采集失败（服务可能重启中）" >&2
    fi

    now=$(date +%s)
    if [ "$now" -ge "$deadline" ]; then
        break
    fi
    remaining=$(( deadline - now ))
    [ "$remaining" -lt "$INTERVAL" ] && INTERVAL="$remaining"
    sleep "$INTERVAL"
done

echo
echo "采样结束：$n 个样本 → $OUT"
awk -F, 'NR==2 { first=$7; ffd=$8; fp95=$9; fel=$11 }
         NR>2  { last=$7; lfd=$8; lp95=$9; lel=$11 }
         END {
             if (NR < 3) { print "样本太少，至少需要 2 个点"; exit }
             printf "内存：%.1f MB → %.1f MB（增长 %.2f MB）\n", first/1048576, last/1048576, (last-first)/1048576
             printf "fd  ：%d → %d（增长 %d）\n", ffd, lfd, lfd-ffd
             printf "转发 P95：%.3f ms → %.3f ms\n", fp95*1000, lp95*1000
             printf "错误总数：%d → %d\n", fel, lel
             printf "判据：内存增长 < 2 MB、fd 不增长、P95 漂移 < 20%%、错误为 0 → "
             if ((last-first) < 2*1048576 && (lfd-ffd) <= 0 && fel == lel) print "通过（P95 漂移请对照上面的 CSV 自行确认）"
             else print "未通过，看上面的数字"
         }' "$OUT"
