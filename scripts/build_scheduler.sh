#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINUX_SRC="${1:-${LINUX_SRC:-$HOME/schedlab/kernel/linux-6.17}}"
TOOLS="$LINUX_SRC/tools/sched_ext"
JOBS="${JOBS:-2}"

"$ROOT/scripts/install_scheduler.sh" "$LINUX_SRC"
# 4 GB 虚拟机默认只开两个并行任务，减少编译期间的内存峰值。
printf 'Building tools/sched_ext with %s parallel job(s)...\n' "$JOBS"
make -C "$TOOLS" -j"$JOBS"

if [[ ! -x "$TOOLS/build/bin/scx_mlfq" ]]; then
	printf 'ERROR: build finished but build/bin/scx_mlfq was not generated\n' >&2
	exit 1
fi
mkdir -p "$ROOT/bin"
# 把最终程序复制回项目目录，之后运行实验不必再进入内核源码树。
cp "$TOOLS/build/bin/scx_mlfq" "$ROOT/bin/scx_mlfq"
printf '\nBuild succeeded: %s\n' "$ROOT/bin/scx_mlfq"
printf 'Dry help check:\n'
"$ROOT/bin/scx_mlfq" -h
