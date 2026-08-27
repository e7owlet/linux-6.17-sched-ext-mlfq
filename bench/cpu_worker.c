/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double seconds_between(struct timespec a, struct timespec b)
{
	return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s [-t seconds] [-i worker_id] [-o output.csv]\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *output = NULL;
	unsigned int seconds = 15, worker_id = 0;
	struct timespec wall_start, wall_now, cpu_start, cpu_end;
	volatile uint64_t sink = 0;
	uint64_t state, iterations = 0;
	FILE *fp = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "t:i:o:h")) != -1) {
		switch (opt) {
		case 't': seconds = strtoul(optarg, NULL, 10); break;
		case 'i': worker_id = strtoul(optarg, NULL, 10); break;
		case 'o': output = optarg; break;
		default: usage(argv[0]); return opt != 'h';
		}
	}
	if (!seconds || seconds > 3600) {
		fprintf(stderr, "seconds must be 1..3600\n");
		return 2;
	}

	/*
	 * 使用纯计算的 xorshift 循环持续占用 CPU，不进行磁盘或网络访问。
	 * 不同 PID 和 worker_id 生成不同初始状态，避免编译器合并计算。
	 */
	state = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)getpid() << 16) ^ worker_id;
	clock_gettime(CLOCK_MONOTONIC, &wall_start);
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_start);

	do {
		unsigned int i;

		/* 每完成一批计算再读取时间，降低计时系统调用对吞吐量的干扰。 */
		for (i = 0; i < 100000; i++) {
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			sink += state * 0x2545f4914f6cdd1dULL;
		}
		iterations += 100000;
		clock_gettime(CLOCK_MONOTONIC, &wall_now);
	} while (seconds_between(wall_start, wall_now) < seconds);

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_end);
	clock_gettime(CLOCK_MONOTONIC, &wall_now);

	if (output) {
		fp = fopen(output, "w");
		if (!fp) {
			fprintf(stderr, "cannot open %s: %s\n", output, strerror(errno));
			return 1;
		}
	} else {
		fp = stdout;
	}

	/* 迭代总数作为 CPU 吞吐量指标，越大通常表示获得的 CPU 时间越多。 */
	fprintf(fp, "worker_id,pid,iterations,wall_seconds,cpu_seconds,sink\n");
	fprintf(fp, "%u,%d,%" PRIu64 ",%.6f,%.6f,%" PRIu64 "\n",
		worker_id, getpid(), iterations, seconds_between(wall_start, wall_now),
		seconds_between(cpu_start, cpu_end), (uint64_t)sink);
	if (fp != stdout)
		fclose(fp);
	return 0;
}
