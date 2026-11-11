#!/usr/bin/env bash
# 执行所有数据库迁移（按文件名顺序）。幂等：可重复运行。
#
# 用法：
#   export DATABASE_URL='postgres://postgres@127.0.0.1:5432/webrtc?sslmode=disable'
#   ./db/migrate.sh
set -euo pipefail

if [[ -z "${DATABASE_URL:-}" ]]; then
    echo "错误：请先设置 DATABASE_URL" >&2
    echo "  例：export DATABASE_URL='postgres://user:pass@127.0.0.1:5432/webrtc?sslmode=disable'" >&2
    exit 1
fi

cd "$(dirname "$0")/.."

shopt -s nullglob
files=(db/migrations/*.sql)
if [[ ${#files[@]} -eq 0 ]]; then
    echo "没有找到 db/migrations/*.sql" >&2
    exit 1
fi

for f in "${files[@]}"; do
    echo "==> $f"
    psql "$DATABASE_URL" -v ON_ERROR_STOP=1 -q -f "$f"
done

echo "迁移完成，当前表："
psql "$DATABASE_URL" -c '\dt'
