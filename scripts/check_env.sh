#!/usr/bin/env bash
set -u

LINUX_SRC="${1:-${LINUX_SRC:-$HOME/schedlab/kernel/linux-6.17}}"
failures=0

ok()   { printf '[ OK ] %s\n' "$*"; }
warn() { printf '[WARN] %s\n' "$*"; }
bad()  { printf '[FAIL] %s\n' "$*"; failures=$((failures + 1)); }

printf '== sched_ext / MLFQ environment check ==\n'
printf 'kernel       : %s\n' "$(uname -r)"
printf 'kernel source: %s\n\n' "$LINUX_SRC"

if grep -q '^CONFIG_SCHED_CLASS_EXT=y' "/boot/config-$(uname -r)" 2>/dev/null; then
	ok 'CONFIG_SCHED_CLASS_EXT=y'
elif [[ -r /proc/config.gz ]] && zgrep -q '^CONFIG_SCHED_CLASS_EXT=y' /proc/config.gz; then
	ok 'CONFIG_SCHED_CLASS_EXT=y (from /proc/config.gz)'
else
	bad 'CONFIG_SCHED_CLASS_EXT=y was not found'
fi

[[ -r /sys/kernel/btf/vmlinux ]] && ok '/sys/kernel/btf/vmlinux is readable' || bad 'BTF file is missing'
[[ -r /sys/kernel/sched_ext/state ]] && ok "sched_ext state: $(cat /sys/kernel/sched_ext/state)" || bad 'sched_ext state file is missing'
[[ -d "$LINUX_SRC/tools/sched_ext" ]] && ok 'Linux tools/sched_ext source exists' || bad 'tools/sched_ext not found under source path'
[[ -f "$LINUX_SRC/kernel/sched/ext.c" ]] && ok 'kernel/sched/ext.c exists' || bad 'kernel source tree is incomplete'

for cmd in make gcc clang bpftool pkg-config python3; do
	command -v "$cmd" >/dev/null 2>&1 && ok "$cmd: $(command -v "$cmd")" || bad "$cmd is not installed"
done

for cmd in trace-cmd perf stress-ng; do
	command -v "$cmd" >/dev/null 2>&1 && ok "$cmd: $(command -v "$cmd")" || warn "$cmd is missing (only the related optional experiment is affected)"
done

printf '\nDisk free under source tree:\n'
df -h "$LINUX_SRC" 2>/dev/null || true
printf '\nMemory:\n'
free -h 2>/dev/null || true

if (( failures )); then
	printf '\n%d required check(s) failed. Fix them before compiling.\n' "$failures" >&2
	exit 1
fi
printf '\nAll required checks passed.\n'
