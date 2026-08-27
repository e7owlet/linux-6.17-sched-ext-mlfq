#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKLOAD="${1:-interactive}"
DURATION="${2:-15}"
OUT_DIR="${3:-$ROOT/results/manual-workload}"
NCPU="${4:-$(nproc)}"

[[ -x "$ROOT/build/cpu_worker" && -x "$ROOT/build/latency_worker" ]] || {
	printf 'ERROR: run make bench first\n' >&2
	exit 1
}
[[ "$NCPU" =~ ^[0-9]+$ ]] && (( NCPU >= 1 )) || {
	printf 'ERROR: CPU count must be a positive integer\n' >&2
	exit 2
}

mkdir -p "$OUT_DIR"
pids=()

start_cpu_workers() {
	local count="$1" i cpu online
	online="$(nproc)"
	for ((i = 0; i < count; i++)); do
		# 将工作进程轮流绑定到逻辑 CPU，使不同实验的竞争关系保持一致。
		cpu=$((i % online))
		taskset -c "$cpu" "$ROOT/build/cpu_worker" -t "$DURATION" -i "$i" \
			-o "$OUT_DIR/cpu-$i.csv" &
		pids+=("$!")
	done
}

case "$WORKLOAD" in
	cpu)
		start_cpu_workers "$NCPU"
		;;
	interactive)
		# latency_worker 与 CPU 0 上的计算任务竞争，用于测量交互响应性。
		start_cpu_workers "$NCPU"
		taskset -c 0 "$ROOT/build/latency_worker" -t "$DURATION" -p 1000 \
			-o "$OUT_DIR/latency.csv" &
		pids+=("$!")
		;;
	mixed)
		# 混合负载同时包含 CPU 计算、周期唤醒以及同步 I/O 压力。
		start_cpu_workers "$NCPU"
		taskset -c 0 "$ROOT/build/latency_worker" -t "$DURATION" -p 1000 \
			-o "$OUT_DIR/latency.csv" &
		pids+=("$!")
		if command -v stress-ng >/dev/null 2>&1; then
			stress-ng --io 1 --timeout "${DURATION}s" --metrics-brief \
				> "$OUT_DIR/stress-ng.txt" 2>&1 &
			pids+=("$!")
		else
			printf 'WARN: stress-ng missing; mixed run has CPU + latency only\n' >&2
		fi
		;;
	*)
		printf 'ERROR: workload must be cpu, interactive, or mixed\n' >&2
		exit 2
		;;
esac

status=0
for pid in "${pids[@]}"; do
	wait "$pid" || status=1
done
exit "$status"
