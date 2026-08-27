#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCHEDULER="${1:-mlfq}"
WORKLOAD="${2:-interactive}"
DURATION="${3:-15}"
NCPU="${4:-$(nproc)}"
LINUX_SRC="${LINUX_SRC:-$HOME/schedlab/kernel/linux-6.17}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="$ROOT/results/run-$STAMP-$SCHEDULER-$WORKLOAD"
SCHED_PID=""

mkdir -p "$OUT_DIR"

cleanup() {
	# 无论正常结束还是收到信号，都卸载自定义调度器，避免影响后续操作。
	if [[ -n "$SCHED_PID" ]] && kill -0 "$SCHED_PID" 2>/dev/null; then
		sudo kill -INT "$SCHED_PID" 2>/dev/null || true
		wait "$SCHED_PID" 2>/dev/null || true
	fi
}
trap cleanup EXIT INT TERM

state="$(cat /sys/kernel/sched_ext/state 2>/dev/null || echo unavailable)"
# 不覆盖正在运行的其他 sched_ext 调度器，先要求用户正常退出原程序。
if [[ "$state" != "disabled" ]]; then
	printf 'ERROR: sched_ext state is %s; stop the existing scheduler first\n' "$state" >&2
	exit 1
fi

case "$SCHEDULER" in
	normal)
		printf 'Using the normal in-kernel scheduler.\n'
		;;
	simple)
		bin="$LINUX_SRC/tools/sched_ext/build/bin/scx_simple"
		[[ -x "$bin" ]] || { printf 'ERROR: %s not found\n' "$bin" >&2; exit 1; }
		sudo "$bin" > "$OUT_DIR/scheduler.log" 2>&1 &
		SCHED_PID="$!"
		;;
	mlfq)
		bin="$ROOT/bin/scx_mlfq"
		[[ -x "$bin" ]] || { printf 'ERROR: run make scheduler first\n' >&2; exit 1; }
		# 在后台加载 MLFQ，并把策略计数和加载日志写入本次结果目录。
		sudo "$bin" -s 4000,12000,32000 -q 8,4,1 \
			-o "$OUT_DIR/scheduler-stats.csv" > "$OUT_DIR/scheduler.log" 2>&1 &
		SCHED_PID="$!"
		;;
	*)
		printf 'ERROR: scheduler must be normal, simple, or mlfq\n' >&2
		exit 2
		;;
esac

if [[ -n "$SCHED_PID" ]]; then
	# 最多等待 5 秒确认 struct_ops 已成功挂载，失败时保留日志供排错。
	for _ in 1 2 3 4 5; do
		sleep 1
		[[ "$(cat /sys/kernel/sched_ext/state 2>/dev/null)" == "enabled" ]] && break
	done
	if [[ "$(cat /sys/kernel/sched_ext/state 2>/dev/null)" != "enabled" ]]; then
		printf 'ERROR: scheduler did not attach; inspect %s/scheduler.log\n' "$OUT_DIR" >&2
		exit 1
	fi
fi

{
	printf 'timestamp=%s\n' "$(date --iso-8601=seconds)"
	printf 'kernel=%s\n' "$(uname -r)"
	printf 'scheduler=%s\n' "$SCHEDULER"
	printf 'workload=%s\n' "$WORKLOAD"
	printf 'duration=%s\n' "$DURATION"
	printf 'workers=%s\n' "$NCPU"
} > "$OUT_DIR/metadata.txt"

printf 'Running scheduler=%s workload=%s duration=%ss workers=%s\n' \
	"$SCHEDULER" "$WORKLOAD" "$DURATION" "$NCPU"

if command -v perf >/dev/null 2>&1; then
	# perf 统计上下文切换、迁移和硬件计数；权限不足时仍继续普通负载测试。
	if ! perf stat -o "$OUT_DIR/perf.txt" \
		-e task-clock,context-switches,cpu-migrations,page-faults,cycles,instructions \
		-- "$ROOT/scripts/run_workload.sh" "$WORKLOAD" "$DURATION" "$OUT_DIR" "$NCPU"; then
		printf 'WARN: perf failed (often perf_event_paranoid); rerunning workload without perf\n' >&2
		"$ROOT/scripts/run_workload.sh" "$WORKLOAD" "$DURATION" "$OUT_DIR" "$NCPU"
	fi
else
	"$ROOT/scripts/run_workload.sh" "$WORKLOAD" "$DURATION" "$OUT_DIR" "$NCPU"
fi

cleanup
SCHED_PID=""
trap - EXIT INT TERM
printf 'Run complete: %s\n' "$OUT_DIR"
