#!/usr/bin/env bash
# 恢复演练：证明"备份能恢复"，而不只是"备份文件存在"
#
# 做法：把最新一份 pg_dump 恢复进一个临时库，逐表比对行数，比完把临时库删掉。
# 为什么不直接往生产库恢复：演练的目的就是验证流程，不能拿正在跑数据的库当试验场。
#
# 用法（在服务器上，需要 root）：
#   sudo bash tools/ops/restore-drill.sh [备份文件]
#   不传参数则用 /var/backups/webrtc 下最新的一份。
set -euo pipefail

BACKUP_DIR="${BACKUP_DIR:-/var/backups/webrtc}"
ENV_FILE="${ENV_FILE:-/opt/webrtc/auth.env}"
TABLES=(users rooms room_members auth_tokens)

[[ $EUID -eq 0 ]] || { echo "需要 root：sudo bash $0" >&2; exit 1; }

latest="${1:-}"
if [[ -z "$latest" ]]; then
    latest="$(ls -1t "$BACKUP_DIR"/webrtc-*.dump 2>/dev/null | head -1 || true)"
fi
[[ -n "$latest" ]] || { echo "找不到备份文件（$BACKUP_DIR 下没有 webrtc-*.dump）" >&2; exit 1; }
[[ -f "$latest" ]] || { echo "备份文件不存在：$latest" >&2; exit 1; }
echo "==> 备份文件: $latest（$(du -h "$latest" | cut -f1)）"

DB_URL="${DATABASE_URL:-}"
if [[ -z "$DB_URL" ]]; then
    DB_URL="$(grep -oP '^DATABASE_URL=\K.*' "$ENV_FILE" 2>/dev/null | head -1 || true)"
fi
[[ -n "$DB_URL" ]] || { echo "找不到 DATABASE_URL（可设环境变量，或在 $ENV_FILE 里）" >&2; exit 1; }

# postgres://user:pw@host:port/dbname?params -> dbname
src_db="$(printf '%s' "$DB_URL" | sed -E 's#^[^/]+//[^/]+/([^/?]+).*#\1#')"
[[ -n "$src_db" ]] || { echo "无法从连接串里解析出库名：$DB_URL" >&2; exit 1; }

tmp_db="webrtc_restore_$(date +%H%M%S)"
echo "==> 源库: $src_db    临时库: $tmp_db"

cleanup() {
    sudo -u postgres dropdb --if-exists "$tmp_db" >/dev/null 2>&1 || true
    echo "==> 已删除临时库 $tmp_db"
}
trap cleanup EXIT

sudo -u postgres createdb "$tmp_db"
echo "==> 恢复中（pg_restore）"
# 用标准输入喂给 pg_restore，而不是把路径交给它：
# 备份文件是 0600 root、目录是 0700，postgres 用户读不到，
# 让 root 的 shell 做重定向、pg_restore 只读 stdin，就不用改文件的属主或权限。
sudo -u postgres pg_restore --no-owner --no-acl --dbname="$tmp_db" < "$latest"

echo "==> 逐表比对行数"
fail=0
for t in "${TABLES[@]}"; do
    src="$(sudo -u postgres psql -d "$src_db" -tAc "select count(*) from $t" 2>/dev/null || echo 'ERR')"
    dst="$(sudo -u postgres psql -d "$tmp_db" -tAc "select count(*) from $t" 2>/dev/null || echo 'ERR')"
    if [[ "$src" == "$dst" && "$src" != 'ERR' ]]; then
        printf '    PASS  %-14s %s 行\n' "$t" "$src"
    else
        printf '    FAIL  %-14s 源=%s 恢复=%s\n' "$t" "$src" "$dst"
        fail=1
    fi
done

if [[ $fail -eq 0 ]]; then
    echo "==> 恢复演练通过：备份是可用的"
else
    echo "==> 恢复演练失败：这份备份恢复不出与源库一致的数据" >&2
    exit 1
fi
