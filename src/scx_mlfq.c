/* SPDX-License-Identifier: GPL-2.0 */
#include <errno.h>
#include <inttypes.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <scx/common.h>

#include "scx_mlfq.h"
#include "scx_mlfq.bpf.skel.h"

static bool verbose;
static volatile sig_atomic_t exit_req;

static const char help_fmt[] =
"Three-level MLFQ scheduler implemented with Linux sched_ext.\n"
"\n"
"Usage: %s [options]\n"
"\n"
"  -s H,M,L      queue slices in microseconds (default: 4000,12000,32000)\n"
"  -q H,M,L      dispatch quotas (default: 8,4,1)\n"
"  -i SEC        statistics interval (default: 1)\n"
"  -o FILE       append statistics as CSV\n"
"  -v            print libbpf debug messages\n"
"  -h            display this help\n";

static int libbpf_print_fn(enum libbpf_print_level level,
			   const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void signal_handler(int signo)
{
	(void)signo;
	/* 只设置退出标志，实际卸载在主循环外完成，避免信号处理函数执行复杂操作。 */
	exit_req = 1;
}

/* 解析形如 4000,12000,32000 的三个无符号整数。 */
static int parse_u64_triplet(const char *arg, __u64 out[3])
{
	unsigned long long a, b, c;
	char tail;

	if (sscanf(arg, "%llu,%llu,%llu%c", &a, &b, &c, &tail) != 3)
		return -EINVAL;
	out[0] = a;
	out[1] = b;
	out[2] = c;
	return 0;
}

static int parse_u32_triplet(const char *arg, __u32 out[3])
{
	unsigned int a, b, c;
	char tail;

	if (sscanf(arg, "%u,%u,%u%c", &a, &b, &c, &tail) != 3)
		return -EINVAL;
	out[0] = a;
	out[1] = b;
	out[2] = c;
	return 0;
}

static int read_stats(struct scx_mlfq *skel, __u64 totals[MLFQ_NR_STATS])
{
	int nr_cpus = libbpf_num_possible_cpus();
	__u64 *percpu;
	__u32 idx;

	if (nr_cpus <= 0)
		return -EINVAL;
	percpu = calloc(nr_cpus, sizeof(*percpu));
	if (!percpu)
		return -ENOMEM;

	/*
	 * BPF 侧使用 per-CPU map：每个逻辑统计项在每个 CPU 上各有一份。
	 * 用户态逐项读取，并把所有 CPU 的值相加为系统总计数。
	 */
	memset(totals, 0, sizeof(*totals) * MLFQ_NR_STATS);
	for (idx = 0; idx < MLFQ_NR_STATS; idx++) {
		int cpu;

		if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats),
					&idx, percpu))
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			totals[idx] += percpu[cpu];
	}

	free(percpu);
	return 0;
}

static void write_csv_header_if_empty(FILE *fp)
{
	long pos;

	fseek(fp, 0, SEEK_END);
	pos = ftell(fp);
	if (pos == 0) {
		/* 只有新文件写表头，使用 -o 追加实验时不会重复表头。 */
		fprintf(fp, "unix_time,enq_high,enq_mid,enq_low,dispatch_high,"
			"dispatch_mid,dispatch_low,promotions,demotions,errors\n");
		fflush(fp);
	}
}

int main(int argc, char **argv)
{
	__u64 slices_us[3] = { 4000, 12000, 32000 };
	__u32 quotas[3] = { 8, 4, 1 };
	unsigned int interval = 1;
	const char *csv_path = NULL;
	struct scx_mlfq *skel;
	struct bpf_link *link;
	FILE *csv = NULL;
	__u64 ecode;
	int opt, i, ret;

	while ((opt = getopt(argc, argv, "s:q:i:o:vh")) != -1) {
		/* 在 BPF 加载之前完成所有参数解析，因为 rodata 加载后不能再修改。 */
		switch (opt) {
		case 's':
			if (parse_u64_triplet(optarg, slices_us)) {
				fprintf(stderr, "invalid slice triplet: %s\n", optarg);
				return 2;
			}
			break;
		case 'q':
			if (parse_u32_triplet(optarg, quotas)) {
				fprintf(stderr, "invalid quota triplet: %s\n", optarg);
				return 2;
			}
			break;
		case 'i':
			interval = strtoul(optarg, NULL, 10);
			if (!interval || interval > 3600) {
				fprintf(stderr, "interval must be 1..3600 seconds\n");
				return 2;
			}
			break;
		case 'o':
			csv_path = optarg;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	for (i = 0; i < 3; i++) {
		/* 限制极端参数，防止过短时间片造成调度风暴。 */
		if (slices_us[i] < 100 || slices_us[i] > 1000000) {
			fprintf(stderr, "each slice must be 100..1000000 us\n");
			return 2;
		}
		if (!quotas[i] || quotas[i] > 1000) {
			fprintf(stderr, "each quota must be 1..1000\n");
			return 2;
		}
	}

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* 打开由 bpftool 自动生成的 BPF skeleton，此时程序尚未挂到内核。 */
	skel = SCX_OPS_OPEN(mlfq_ops, scx_mlfq);
	for (i = 0; i < 3; i++) {
		/* 把命令行中的微秒转换为纳秒，并写入 BPF 只读配置区。 */
		skel->rodata->slice_ns[i] = slices_us[i] * 1000ULL;
		skel->rodata->queue_quota[i] = quotas[i];
	}

	/* LOAD 负责 verifier 校验，ATTACH 成功后 MLFQ 才正式接管 sched_ext 任务。 */
	SCX_OPS_LOAD(skel, mlfq_ops, scx_mlfq, uei);
	link = SCX_OPS_ATTACH(skel, mlfq_ops, scx_mlfq);

	if (csv_path) {
		csv = fopen(csv_path, "a+");
		if (!csv) {
			fprintf(stderr, "cannot open %s: %s\n", csv_path,
				strerror(errno));
			exit_req = 1;
		} else {
			write_csv_header_if_empty(csv);
		}
	}

	/*
	 * __u64 在不同头文件组合下可能是 unsigned long 或 unsigned long long；
	 * 转成 uint64_t 后再配合 PRIu64，保证格式检查在不同发行版上都通过。
	 */
	printf("MLFQ attached: slices(us)=%" PRIu64 ",%" PRIu64 ",%" PRIu64
	       " quotas=%u,%u,%u\n",
	       (uint64_t)slices_us[0], (uint64_t)slices_us[1],
	       (uint64_t)slices_us[2],
	       quotas[0], quotas[1], quotas[2]);
	printf("Press Ctrl+C to stop and return to the normal Linux scheduler.\n");

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 s[MLFQ_NR_STATS];
		time_t now;

		/* 周期读取累计统计；该循环不参与每一次进程调度决策。 */
		sleep(interval);
		ret = read_stats(skel, s);
		if (ret) {
			fprintf(stderr, "failed to read stats: %s\n", strerror(-ret));
			continue;
		}
		now = time(NULL);
		printf("enq=%" PRIu64 "/%" PRIu64 "/%" PRIu64
		       " dispatch=%" PRIu64 "/%" PRIu64 "/%" PRIu64
		       " promote=%" PRIu64 " demote=%" PRIu64 " errors=%" PRIu64 "\n",
		       (uint64_t)s[0], (uint64_t)s[1], (uint64_t)s[2],
		       (uint64_t)s[3], (uint64_t)s[4], (uint64_t)s[5],
		       (uint64_t)s[6], (uint64_t)s[7], (uint64_t)s[8]);
		fflush(stdout);
		if (csv) {
			fprintf(csv, "%lld,%" PRIu64 ",%" PRIu64 ",%" PRIu64
				",%" PRIu64 ",%" PRIu64 ",%" PRIu64
				",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
				(long long)now,
				(uint64_t)s[0], (uint64_t)s[1], (uint64_t)s[2],
				(uint64_t)s[3], (uint64_t)s[4], (uint64_t)s[5],
				(uint64_t)s[6], (uint64_t)s[7], (uint64_t)s[8]);
			fflush(csv);
		}
	}

	if (csv)
		fclose(csv);
	/*
	 * 销毁 link 会从内核卸载 struct_ops，内核随即恢复普通调度器。
	 * UEI_REPORT 同时输出正常退出、verifier 错误或 watchdog 超时等原因。
	 */
	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_mlfq__destroy(skel);
	return UEI_ECODE_RESTART(ecode) ? 75 : 0;
}
