#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINUX_SRC="${1:-${LINUX_SRC:-$HOME/schedlab/kernel/linux-6.17}}"
TOOLS="$LINUX_SRC/tools/sched_ext"

if [[ ! -f "$TOOLS/Makefile" ]]; then
	printf 'ERROR: %s is not a Linux tools/sched_ext directory\n' "$TOOLS" >&2
	exit 1
fi

# 将项目源码放到官方示例目录，复用内核自带的 BPF skeleton 构建流程。
cp "$ROOT/src/scx_mlfq.bpf.c" "$TOOLS/scx_mlfq.bpf.c"
cp "$ROOT/src/scx_mlfq.c" "$TOOLS/scx_mlfq.c"
cp "$ROOT/src/scx_mlfq.h" "$TOOLS/scx_mlfq.h"

# 仅在第一次安装时修改目标列表；重复执行不会重复添加 scx_mlfq。
if ! grep -Eq '^c-sched-targets[[:space:]]*=.*\bscx_mlfq\b' "$TOOLS/Makefile"; then
	[[ -e "$TOOLS/Makefile.before_mlfq" ]] || cp "$TOOLS/Makefile" "$TOOLS/Makefile.before_mlfq"
	sed -i '/^c-sched-targets[[:space:]]*=/ s/$/ scx_mlfq/' "$TOOLS/Makefile"
fi

printf 'Installed source files into %s\n' "$TOOLS"
grep '^c-sched-targets' "$TOOLS/Makefile"
