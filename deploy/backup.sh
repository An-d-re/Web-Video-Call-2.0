#!/usr/bin/env bash
# PostgreSQL 每日备份（pg_dump 自定义格式，可直接 pg_restore）
#
# 装到 /usr/local/bin/webrtc-backup，由 webrtc-backup.timer 每天 03:15 触发。
# 手动跑一次：sudo webrtc-backup
#
# 为什么用自定义格式（-Fc）而不是纯 SQL：
#   1. 压缩过，体积小；
#   2. pg_restore 能按表/按 schema 挑着恢复，出事时比啃 SQL 文件快。
#
# 为什么先写 .part 再 mv：
#   备份中途被 kill（比如磁盘满、机器重启）时，目录里不会留下一个"看起来正常、
#   其实截断"的 dump —— 那种文件在真要恢复的那天才发现是坏的，最要命。
set -euo pipefail

BACKUP_DIR="${BACKUP_DIR:-/var/backups/webrtc}"
KEEP_DAYS="${KEEP_DAYS:-7}"
ENV_FILE="${ENV_FILE:-/opt/webrtc/auth.env}"

[[ $EUID -eq 0 ]] || { echo "需要 root：sudo webrtc-backup" >&2; exit 1; }

DB_URL="${DATABASE_URL:-}"
if [[ -z "$DB_URL" ]]; then
    DB_URL="$(grep -oP '^DATABASE_URL=\K.*' "$ENV_FILE" 2>/dev/null | head -1 || true)"
fi
[[ -n "$DB_URL" ]] || { echo "找不到 DATABASE_URL（可设环境变量，或在 $ENV_FILE 里）" >&2; exit 1; }

command -v pg_dump >/dev/null 2>&1 || { echo "没装 pg_dump（apt install postgresql-client）" >&2; exit 1; }

install -d -m 0700 "$BACKUP_DIR"
stamp="$(date +%Y%m%d-%H%M%S)"
out="$BACKUP_DIR/webrtc-$stamp.dump"
tmp="$out.part"

echo "==> 备份到 $out"
pg_dump --format=custom --no-owner --no-acl --dbname="$DB_URL" --file="$tmp"
chmod 600 "$tmp"
mv "$tmp" "$out"
echo "    大小 $(du -h "$out" | cut -f1)"

# 清理过期备份
deleted=0
while IFS= read -r old; do
    [[ -n "$old" ]] || continue
    rm -f "$old"
    echo "    清理过期备份 $old"
    deleted=$((deleted + 1))
done < <(find "$BACKUP_DIR" -maxdepth 1 -name 'webrtc-*.dump' -type f -mtime +"$KEEP_DAYS" | sort)

# 顺便清掉上次异常退出留下的 .part
find "$BACKUP_DIR" -maxdepth 1 -name '*.part' -type f -mmin +60 -delete 2>/dev/null || true

count="$(find "$BACKUP_DIR" -maxdepth 1 -name 'webrtc-*.dump' -type f | wc -l)"
echo "==> 完成：当前保留 $count 个备份（清理了 $deleted 个）"
[[ "$count" -ge 1 ]] || { echo "备份目录里一个文件都没有，路径或权限有问题" >&2; exit 1; }
