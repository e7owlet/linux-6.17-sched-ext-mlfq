#!/usr/bin/env python3
import csv
import math
import re
import statistics
import sys
from pathlib import Path


def percentile(values, pct):
    """按线性插值计算延迟百分位，用于 P50/P95/P99 尾延迟比较。"""
    if not values:
        return math.nan
    values = sorted(values)
    pos = (len(values) - 1) * pct / 100.0
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return values[lo]
    return values[lo] * (hi - pos) + values[hi] * (pos - lo)


def read_metadata(path):
    data = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                data[key] = value
    return data


def parse_perf(path):
    """从 perf 文本中提取最重要的调度开销指标。"""
    result = {"context_switches": math.nan, "cpu_migrations": math.nan}
    if not path.exists():
        return result
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        clean = line.replace(",", "")
        match = re.search(r"^\s*([0-9.]+)\s+(context-switches|cpu-migrations)\b", clean)
        if match:
            result[match.group(2).replace("-", "_")] = float(match.group(1))
    return result


def analyze_run(run_dir):
    """合并一次实验中的元数据、CPU吞吐量、延迟、perf和MLFQ统计。"""
    meta = read_metadata(run_dir / "metadata.txt")
    iterations = 0
    cpu_seconds = 0.0
    # 多个 CPU 工作进程的迭代量相加，得到本次实验的总吞吐量。
    for path in run_dir.glob("cpu-*.csv"):
        with path.open(newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                iterations += int(row["iterations"])
                cpu_seconds += float(row["cpu_seconds"])

    latencies = []
    latency_path = run_dir / "latency.csv"
    # 周期任务的全部样本用于计算中位数及尾部延迟。
    if latency_path.exists():
        with latency_path.open(newline="", encoding="utf-8") as handle:
            latencies = [float(row["latency_us"]) for row in csv.DictReader(handle)]

    promotions = demotions = errors = math.nan
    # MLFQ 统计是累计值，因此读取最后一行即可得到整次实验总计。
    stats_path = run_dir / "scheduler-stats.csv"
    if stats_path.exists():
        with stats_path.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        if rows:
            promotions = int(rows[-1]["promotions"])
            demotions = int(rows[-1]["demotions"])
            errors = int(rows[-1]["errors"])

    perf = parse_perf(run_dir / "perf.txt")
    return {
        "run_dir": run_dir.name,
        "scheduler": meta.get("scheduler", "unknown"),
        "workload": meta.get("workload", "unknown"),
        "duration_s": meta.get("duration", ""),
        "workers": meta.get("workers", ""),
        "total_iterations": iterations,
        "total_cpu_seconds": round(cpu_seconds, 6),
        "latency_samples": len(latencies),
        "latency_p50_us": round(percentile(latencies, 50), 3),
        "latency_p95_us": round(percentile(latencies, 95), 3),
        "latency_p99_us": round(percentile(latencies, 99), 3),
        "latency_max_us": round(max(latencies), 3) if latencies else math.nan,
        "context_switches": perf["context_switches"],
        "cpu_migrations": perf["cpu_migrations"],
        "mlfq_promotions": promotions,
        "mlfq_demotions": demotions,
        "mlfq_errors": errors,
    }


def main():
    results_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "results").resolve()
    runs = sorted(p for p in results_dir.glob("run-*") if p.is_dir())
    if not runs:
        raise SystemExit(f"No run-* directories found under {results_dir}")
    rows = [analyze_run(path) for path in runs]
    output = results_dir / "summary.csv"
    with output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    print(f"Wrote {len(rows)} run(s) to {output}")

    # 按“调度器 + 负载”分组计算多次重复实验的平均结果。
    groups = {}
    for row in rows:
        key = (row["scheduler"], row["workload"])
        groups.setdefault(key, []).append(row)
    print("\nMean results by scheduler/workload:")
    print("scheduler workload       iterations       p99_latency_us")
    for key, values in sorted(groups.items()):
        mean_iter = statistics.fmean(v["total_iterations"] for v in values)
        valid_p99 = [v["latency_p99_us"] for v in values if not math.isnan(v["latency_p99_us"])]
        mean_p99 = statistics.fmean(valid_p99) if valid_p99 else math.nan
        print(f"{key[0]:9} {key[1]:12} {mean_iter:14.0f} {mean_p99:20.3f}")


if __name__ == "__main__":
    main()
