/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void add_us(struct timespec *ts, uint64_t us)
{
	/* 在绝对时间点上累加周期，避免相对 sleep 产生逐次累计漂移。 */
	ts->tv_sec += us / 1000000;
	ts->tv_nsec += (long)(us % 1000000) * 1000L;
	if (ts->tv_nsec >= 1000000000L) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

static double lateness_us(struct timespec expected, struct timespec actual)
{
	return (actual.tv_sec - expected.tv_sec) * 1e6 +
	       (actual.tv_nsec - expected.tv_nsec) / 1e3;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-t seconds] [-p period_us] [-o output.csv]\n", prog);
}

int main(int argc, char **argv)
{
	const char *output = "latency.csv";
	uint64_t period_us = 1000;
	unsigned int seconds = 15;
	struct timespec next, now, end;
	uint64_t seq = 0;
	FILE *fp;
	int opt, ret;

	while ((opt = getopt(argc, argv, "t:p:o:h")) != -1) {
		switch (opt) {
		case 't': seconds = strtoul(optarg, NULL, 10); break;
		case 'p': period_us = strtoull(optarg, NULL, 10); break;
		case 'o': output = optarg; break;
		default: usage(argv[0]); return opt != 'h';
		}
	}
	if (!seconds || seconds > 3600 || period_us < 100 || period_us > 1000000) {
		fprintf(stderr, "seconds=1..3600, period_us=100..1000000\n");
		return 2;
	}

	fp = fopen(output, "w");
	if (!fp) {
		fprintf(stderr, "cannot open %s: %s\n", output, strerror(errno));
		return 1;
	}
	fprintf(fp, "sequence,expected_sec,expected_nsec,actual_sec,actual_nsec,latency_us\n");

	/*
	 * 使用 CLOCK_MONOTONIC + TIMER_ABSTIME 周期唤醒。
	 * “实际唤醒时刻 - 计划唤醒时刻”就是调度唤醒延迟。
	 */
	clock_gettime(CLOCK_MONOTONIC, &next);
	add_us(&next, 100000); /* 预留 100 ms，让并发 CPU 负载先启动。 */
	end = next;
	end.tv_sec += seconds;

	while (next.tv_sec < end.tv_sec ||
	       (next.tv_sec == end.tv_sec && next.tv_nsec < end.tv_nsec)) {
		/* 绝对时间休眠不会把上一次的调度延迟叠加到下一周期。 */
		ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		if (ret && ret != EINTR) {
			fprintf(stderr, "clock_nanosleep: %s\n", strerror(ret));
			fclose(fp);
			return 1;
		}
		if (ret == EINTR)
			continue;
		clock_gettime(CLOCK_MONOTONIC, &now);
		fprintf(fp, "%" PRIu64 ",%ld,%ld,%ld,%ld,%.3f\n", seq++,
			next.tv_sec, next.tv_nsec, now.tv_sec, now.tv_nsec,
			lateness_us(next, now));
		add_us(&next, period_us);
	}

	fclose(fp);
	return 0;
}
