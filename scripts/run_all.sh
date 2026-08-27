#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNS="${1:-3}"
DURATION="${2:-15}"
NCPU="${3:-$(nproc)}"

# 对三种调度器、三类负载做同参数重复实验，避免单次测量的偶然性。
for scheduler in normal simple mlfq; do
	for workload in cpu interactive mixed; do
		for ((run = 1; run <= RUNS; run++)); do
			printf '\n=== %s / %s / repetition %d of %d ===\n' \
				"$scheduler" "$workload" "$run" "$RUNS"
			"$ROOT/scripts/run_once.sh" "$scheduler" "$workload" "$DURATION" "$NCPU"
			sleep 2
		done
	done
done

# 所有原始数据保留在独立目录，最后统一生成可用于制表的 summary.csv。
python3 "$ROOT/scripts/summarize.py" "$ROOT/results"
