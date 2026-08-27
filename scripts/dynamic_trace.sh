#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DURATION="${1:-10}"
OUT_DIR="${2:-$ROOT/results/trace-$(date +%Y%m%d-%H%M%S)}"

if ! command -v trace-cmd >/dev/null 2>&1; then
	printf 'ERROR: trace-cmd is not installed\n' >&2
	exit 1
fi
if [[ ! -x "$ROOT/build/latency_worker" ]]; then
	printf 'ERROR: run make bench first\n' >&2
	exit 1
fi

mkdir -p "$OUT_DIR"
printf 'Current sched_ext state: %s\n' "$(cat /sys/kernel/sched_ext/state 2>/dev/null || echo unknown)"
printf 'Recording sched_switch/wakeup events for %ss...\n' "$DURATION"

# 同时记录唤醒和切换事件，之后可以计算“被唤醒到真正获得 CPU”的路径。
sudo trace-cmd record -o "$OUT_DIR/trace.dat" \
	-e sched:sched_switch \
	-e sched:sched_wakeup \
	-e sched:sched_wakeup_new \
	-e sched:sched_process_exit \
	-- "$ROOT/build/latency_worker" -t "$DURATION" -p 1000 -o "$OUT_DIR/latency.csv"

# trace.dat 保留给 KernelShark，trace.txt 便于直接放入报告或使用 grep 分析。
sudo trace-cmd report -i "$OUT_DIR/trace.dat" > "$OUT_DIR/trace.txt"
sudo chown -R "$(id -u):$(id -g)" "$OUT_DIR" 2>/dev/null || true

printf 'Dynamic trace saved under %s\n' "$OUT_DIR"
printf 'Useful check: grep latency_worker "%s/trace.txt" | head\n' "$OUT_DIR"
