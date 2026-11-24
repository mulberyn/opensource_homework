#!/usr/bin/env bash
# 在 Linux 上一键编译并运行链表搜索 PMC 基准
# 用法: ./run_linux.sh [规模列表]
set -euo pipefail

SIZES="${1:-1000,10000,100000,1000000}"

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "[错误] 本脚本只适用于 Linux；当前是 $(uname -s)，没有 perf_event_open。" >&2
  exit 1
fi

PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "unknown")
echo "[环境] 内核 $(uname -r)，perf_event_paranoid = ${PARANOID}"
case "${PARANOID}" in
  -1|0) ;;
  *)
    echo "[提示] perf_event_paranoid=${PARANOID} 时打开硬件事件可能被拒绝(EACCES)，建议："
    echo "       sudo sysctl -w kernel.perf_event_paranoid=0"
    ;;
esac

echo "[编译]"
make clean >/dev/null 2>&1 || true
make

echo
echo "[运行] 规模: ${SIZES}"
./llsearch_bench -s "${SIZES}" -r 5 -o results_linux.csv

echo
echo "[完成] 结果已写入 results_linux.csv"
column -s, -t < results_linux.csv 2>/dev/null | head -20 || cat results_linux.csv
