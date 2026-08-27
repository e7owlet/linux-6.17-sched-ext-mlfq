/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SCX_MLFQ_H
#define __SCX_MLFQ_H

enum mlfq_level {
	MLFQ_HIGH = 0, /* 高优先级队列：新任务和交互型任务 */
	MLFQ_MID,      /* 中优先级队列：短时间占用 CPU 的任务 */
	MLFQ_LOW,      /* 低优先级队列：持续占用 CPU 的任务 */
	MLFQ_NR_LEVELS,
};

/* 用户态程序读取这些计数器，用于证明队列分派和升降级确实发生。 */
enum mlfq_stat {
	MLFQ_STAT_ENQ_HIGH = 0,
	MLFQ_STAT_ENQ_MID,
	MLFQ_STAT_ENQ_LOW,
	MLFQ_STAT_DSP_HIGH,
	MLFQ_STAT_DSP_MID,
	MLFQ_STAT_DSP_LOW,
	MLFQ_STAT_PROMOTE,
	MLFQ_STAT_DEMOTE,
	MLFQ_STAT_ERRORS,
	MLFQ_NR_STATS,
};

#endif /* __SCX_MLFQ_H */
