#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINUX_SRC="${1:-${LINUX_SRC:-$HOME/schedlab/kernel/linux-6.17}}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$ROOT/results/source-analysis-$STAMP.txt"

if [[ ! -f "$LINUX_SRC/kernel/sched/ext.c" ]]; then
	printf 'ERROR: kernel/sched/ext.c not found in %s\n' "$LINUX_SRC" >&2
	exit 1
fi

{
	# 将关键函数及源码行号一次性保存，作为报告中“静态调用路径”的证据。
	printf 'Linux sched_ext static source analysis\n'
	printf 'Generated: %s\n' "$(date --iso-8601=seconds)"
	printf 'Kernel source: %s\n\n' "$LINUX_SRC"

	printf '== Core scheduler entry points (kernel/sched/core.c) ==\n'
	grep -nE '(__schedule\(|pick_next_task\(|context_switch\()' "$LINUX_SRC/kernel/sched/core.c" | head -80 || true

	printf '\n== Fair scheduler entry points (kernel/sched/fair.c) ==\n'
	grep -nE '(enqueue_task_fair|dequeue_task_fair|pick_next_task_fair|task_tick_fair)' "$LINUX_SRC/kernel/sched/fair.c" | head -80 || true

	printf '\n== sched_ext bridge and callbacks (kernel/sched/ext.c) ==\n'
	grep -nE '(enqueue_task_scx|dequeue_task_scx|pick_next_task_scx|task_tick_scx|scx_ops_enqueue|scx_ops_dispatch)' "$LINUX_SRC/kernel/sched/ext.c" | head -140 || true

	printf '\n== sched_ext ABI (include/linux/sched/ext.h) ==\n'
	grep -nE '(struct sched_ext_ops|SCX_DSQ_|scx_entity)' "$LINUX_SRC/include/linux/sched/ext.h" | head -100 || true

	printf '\n== Our MLFQ callbacks ==\n'
	# 最后一段把内核入口与本项目注册的 struct_ops 回调对应起来。
	grep -nE 'BPF_STRUCT_OPS|SCX_OPS_DEFINE|scx_bpf_(create_dsq|dsq_insert|dsq_move_to_local)' "$ROOT/src/scx_mlfq.bpf.c" || true
} > "$OUT"

printf 'Static analysis saved to %s\n' "$OUT"
printf 'Read it with: less "%s"\n' "$OUT"
