/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 基于 sched_ext 的教学型三级多级反馈队列调度器。
 *
 * 核心策略：
 *   1. 新任务从高优先级队列开始运行；
 *   2. 任务用完整个时间片后仍处于可运行状态，说明偏向 CPU 密集型，降一级；
 *   3. 任务在半个时间片内主动阻塞，说明偏向交互型或 I/O 型，升一级；
 *   4. CPU 按可配置配额从高、中、低三个 DSQ 中取任务，防止低队列饥饿。
 *
 * 本实现用于课程实验和机制分析，不应直接用于生产环境。
 */
#include <scx/common.bpf.h>
#include "scx_mlfq.h"

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

#define MLFQ_DSQ_HIGH 0ULL
#define MLFQ_DSQ_MID  1ULL
#define MLFQ_DSQ_LOW  2ULL

/*
 * 默认时间片依次为 4 ms、12 ms、32 ms。
 * const volatile 变量可由用户态加载器在 BPF 程序加载前修改。
 */
const volatile u64 slice_ns[MLFQ_NR_LEVELS] = {
	4ULL * 1000 * 1000,
	12ULL * 1000 * 1000,
	32ULL * 1000 * 1000,
};

const volatile u32 queue_quota[MLFQ_NR_LEVELS] = { 8, 4, 1 };

/* 每个任务独立保存的调度状态，任务退出时由 task storage 自动释放。 */
struct task_ctx {
	u32 level;          /* 当前所在队列级别 */
	u64 started_at;     /* 本次获得 CPU 的起始时刻 */
	u64 total_runtime;  /* 累计运行时间，便于后续扩展统计 */
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

struct cpu_ctx {
	u32 next_level;                   /* 下一次优先尝试的队列 */
	u32 credit[MLFQ_NR_LEVELS];       /* 本轮三个队列剩余的分派配额 */
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct cpu_ctx);
} cpu_ctx_stor SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, MLFQ_NR_STATS);
	__type(key, u32);
	__type(value, u64);
} stats SEC(".maps");

static __always_inline void stat_inc(u32 idx)
{
	u64 *cnt;

	/* 使用 per-CPU 计数器，避免多个 CPU 更新同一个计数器时发生锁竞争。 */
	cnt = bpf_map_lookup_elem(&stats, &idx);
	if (cnt)
		(*cnt)++;
}

static __always_inline struct task_ctx *lookup_task_ctx(struct task_struct *p)
{
	struct task_ctx *tctx;

	/* init_task() 已为任务创建状态；调度热路径这里只做查询。 */
	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0, 0);
	if (!tctx) {
		stat_inc(MLFQ_STAT_ERRORS);
		scx_bpf_error("MLFQ: task context lookup failed");
	}
	return tctx;
}

static __always_inline u64 level_to_dsq(u32 level)
{
	if (level == MLFQ_HIGH)
		return MLFQ_DSQ_HIGH;
	if (level == MLFQ_MID)
		return MLFQ_DSQ_MID;
	return MLFQ_DSQ_LOW;
}

s32 BPF_STRUCT_OPS(mlfq_select_cpu, struct task_struct *p,
			   s32 prev_cpu, u64 wake_flags)
{
	bool is_idle = false;

	/*
	 * 复用内核默认的 CPU 选择逻辑，但不在这里直接放入本地 DSQ。
	 * 这样所有任务都必须经过 enqueue()，队列级别和统计才不会被绕过。
	 */
	return scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
}

void BPF_STRUCT_OPS(mlfq_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx;
	u32 level;

	tctx = lookup_task_ctx(p);
	if (!tctx)
		return;

	level = tctx->level;
	if (level >= MLFQ_NR_LEVELS) {
		/* 防御性检查：状态异常时重置为高优先级，避免访问数组越界。 */
		level = MLFQ_HIGH;
		tctx->level = level;
	}

	/* 根据任务当前级别选择 DSQ，并为该级别重新分配对应时间片。 */
	scx_bpf_dsq_insert(p, level_to_dsq(level), slice_ns[level], enq_flags);
	stat_inc(MLFQ_STAT_ENQ_HIGH + level);
}

static __always_inline bool try_dispatch(u32 level)
{
	if (scx_bpf_dsq_move_to_local(level_to_dsq(level))) {
		stat_inc(MLFQ_STAT_DSP_HIGH + level);
		return true;
	}
	return false;
}

void BPF_STRUCT_OPS(mlfq_dispatch, s32 cpu, struct task_struct *prev)
{
	u32 zero = 0;
	struct cpu_ctx *cpuc;
	u32 i;

	cpuc = bpf_map_lookup_elem(&cpu_ctx_stor, &zero);
	if (!cpuc) {
		stat_inc(MLFQ_STAT_ERRORS);
		scx_bpf_error("MLFQ: CPU context lookup failed");
		return;
	}

	/*
	 * 对三个 DSQ 执行加权轮转。默认每轮最多分派高/中/低队列 8/4/1 次。
	 * 如果当前队列为空，就清空它在本轮的剩余配额并立即检查下一级，
	 * 避免 CPU 因等待空队列而闲置。循环次数固定，便于通过 BPF verifier。
	 */
	for (i = 0; i < 6; i++) {
		u32 level;

		if (!cpuc->credit[0] && !cpuc->credit[1] && !cpuc->credit[2]) {
			/* 一轮配额用尽后，从高优先级队列开始下一轮。 */
			cpuc->credit[0] = queue_quota[0];
			cpuc->credit[1] = queue_quota[1];
			cpuc->credit[2] = queue_quota[2];
			cpuc->next_level = MLFQ_HIGH;
		}

		level = cpuc->next_level;
		if (level >= MLFQ_NR_LEVELS)
			level = MLFQ_HIGH;

		if (cpuc->credit[level] && try_dispatch(level)) {
			/* 成功把一个任务移入当前 CPU 的本地 DSQ。 */
			cpuc->credit[level]--;
			if (!cpuc->credit[level])
				cpuc->next_level = (level + 1) % MLFQ_NR_LEVELS;
			return;
		}

		cpuc->credit[level] = 0;
		cpuc->next_level = (level + 1) % MLFQ_NR_LEVELS;
	}
}

void BPF_STRUCT_OPS(mlfq_running, struct task_struct *p)
{
	struct task_ctx *tctx = lookup_task_ctx(p);

	/* 记录真实开始运行的时刻，不能用入队时刻代替。 */
	if (tctx)
		tctx->started_at = scx_bpf_now();
}

void BPF_STRUCT_OPS(mlfq_stopping, struct task_struct *p, bool runnable)
{
	struct task_ctx *tctx;
	u64 now, runtime, assigned_slice;
	u32 old_level;

	tctx = lookup_task_ctx(p);
	if (!tctx)
		return;

	old_level = tctx->level;
	if (old_level >= MLFQ_NR_LEVELS)
		old_level = MLFQ_HIGH;

	now = scx_bpf_now();
	runtime = now > tctx->started_at ? now - tctx->started_at : 0;
	tctx->total_runtime += runtime;
	assigned_slice = slice_ns[old_level];

	/*
	 * 降级规则：任务停止运行时仍可运行，且剩余时间片为 0，
	 * 表明它完整消耗了时间片，是 CPU 密集型行为。
	 */
	if (runnable && !p->scx.slice && old_level < MLFQ_LOW) {
		tctx->level = old_level + 1;
		stat_inc(MLFQ_STAT_DEMOTE);
		return;
	}

	/*
	 * 升级规则：任务主动睡眠，并且本次运行时间小于该级时间片的一半，
	 * 视为交互型或 I/O 型行为，在下次唤醒时进入更高一级队列。
	 */
	if (!runnable && runtime < assigned_slice / 2 && old_level > MLFQ_HIGH) {
		tctx->level = old_level - 1;
		stat_inc(MLFQ_STAT_PROMOTE);
	}
}

s32 BPF_STRUCT_OPS(mlfq_init_task, struct task_struct *p,
			   struct scx_init_task_args *args)
{
	struct task_ctx *tctx;

	/* 为每个新任务创建私有状态；创建失败时拒绝启用，避免无状态调度。 */
	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
				    BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!tctx)
		return -ENOMEM;

	/* MLFQ 规则：新任务一律从高优先级队列开始。 */
	tctx->level = MLFQ_HIGH;
	tctx->started_at = 0;
	tctx->total_runtime = 0;
	return 0;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(mlfq_init)
{
	s32 ret;

	/*
	 * 调度器挂载时创建三个全局 DSQ。任意一级创建失败都终止加载，
	 * 内核随后继续使用原来的调度器，不会留下不完整的 MLFQ。
	 */
	ret = scx_bpf_create_dsq(MLFQ_DSQ_HIGH, -1);
	if (ret)
		return ret;
	ret = scx_bpf_create_dsq(MLFQ_DSQ_MID, -1);
	if (ret)
		return ret;
	return scx_bpf_create_dsq(MLFQ_DSQ_LOW, -1);
}

void BPF_STRUCT_OPS(mlfq_exit, struct scx_exit_info *ei)
{
	/* 把退出原因记录到 UEI，交给用户态加载器报告。 */
	UEI_RECORD(uei, ei);
}

/* 将上述策略函数注册为 sched_ext 的 struct_ops 回调。 */
SCX_OPS_DEFINE(mlfq_ops,
	       .select_cpu = (void *)mlfq_select_cpu,
	       .enqueue    = (void *)mlfq_enqueue,
	       .dispatch   = (void *)mlfq_dispatch,
	       .running    = (void *)mlfq_running,
	       .stopping   = (void *)mlfq_stopping,
	       .init_task  = (void *)mlfq_init_task,
	       .init       = (void *)mlfq_init,
	       .exit       = (void *)mlfq_exit,
	       .timeout_ms = 5000U,
	       .name       = "mlfq");
