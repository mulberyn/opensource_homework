/*
 * pmc.c -- perf_event_open(2) 的最小封装
 *
 * perf_event_open 的系统调用原型（内核 >= 2.6.31）：
 *   int perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
 *                       int group_fd, unsigned long flags);
 * 注意它不是 glibc 的标准函数，没有 wrapper，必须自己用 syscall() 调用。
 *
 * 参数含义：
 *   pid  = 0   -> 只统计当前进程（本作业需要的就是这个）
 *   pid  > 0   -> 统计指定进程
 *   pid  = -1  -> 统计所有进程（需要 CAP_PERFMON / CAP_SYS_ADMIN）
 *   cpu  = -1  -> 跟随进程迁移，不绑定到某个 CPU（统计本进程在所有 CPU 上的和）
 *   group_fd   -> -1 表示自己单独成组
 */
#ifdef __linux__

#include "pmc.h"

#include <errno.h>
#include <linux/perf_event.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static int
perf_event_open_wrap(struct perf_event_attr *attr, pid_t pid, int cpu,
					 int group_fd, unsigned long flags)
{
	return (int) syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/*
 * 每个事件读出来的是三元组 {value, time_enabled, time_running}。
 * 当多个事件在同一个 PMU 上争用有限的计数器时，内核会让它们轮流上机统计，
 * 此时 time_running < time_enabled，真实值要按比例放大：
 *     value_scaled = value * time_enabled / time_running
 */
struct read_format
{
	uint64_t	value;
	uint64_t	time_enabled;
	uint64_t	time_running;
};

static int
open_one(int type, uint64_t config, const char *name, char *reason, size_t rlen)
{
	struct perf_event_attr attr;
	int			fd;

	memset(&attr, 0, sizeof(attr));
	attr.type = (uint32_t) type;
	attr.size = sizeof(attr);
	attr.config = config;
	attr.disabled = 1;			/* 先不计数，等 pmc_enable() */
	attr.exclude_kernel = 1;	/* 只统计用户态，避免内核态干扰、也避免权限问题 */
	attr.exclude_hv = 1;
	attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
		PERF_FORMAT_TOTAL_TIME_RUNNING;

	fd = perf_event_open_wrap(&attr, 0 /* self */ , -1 /* any cpu */ ,
							  -1 /* no group */ , 0);
	if (fd < 0)
	{
		snprintf(reason, rlen,
				 "%s: perf_event_open failed: %s (perf_event_paranoid=%s)",
				 name, strerror(errno), "见 /proc/sys/kernel/perf_event_paranoid");
	}
	return fd;
}

int
pmc_open(pmc_group *g)
{
	char		first_err[256] = "";
	char		err[256];
	int			i;

	memset(g, 0, sizeof(*g));
	for (i = 0; i < PMC_NUM_EVENTS; i++)
		g->fds[i] = -1;

#define OPEN(idx, t, c, nm)												\
	do {																\
		g->fds[idx] = open_one((t), (c), (nm), err, sizeof(err));		\
		if (g->fds[idx] < 0 && first_err[0] == '\0')					\
			snprintf(first_err, sizeof(first_err), "%s", err);			\
	} while (0)

	/* 指令数：PERF_TYPE_HARDWARE + PERF_COUNT_HW_INSTRUCTIONS */
	OPEN(PMC_INSTRUCTIONS, PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,
		 "instructions");

	/* 周期数：PERF_COUNT_HW_CPU_CYCLES */
	OPEN(PMC_CYCLES, PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES,
		 "cycles");

	/*
	 * L1D 访存/缺失：类型为 PERF_TYPE_HW_CACHE，
	 * config 由 [cache_id | op << 8 | result << 16] 拼成。
	 *   cache_id = PERF_COUNT_HW_CACHE_L1D
	 *   op       = PERF_COUNT_HW_CACHE_OP_READ
	 *   result   = ACCESS(0) / MISS(1)
	 */
	OPEN(PMC_L1D_ACCESS, PERF_TYPE_HW_CACHE,
		 PERF_COUNT_HW_CACHE_L1D |
		 ((uint64_t) PERF_COUNT_HW_CACHE_OP_READ << 8) |
		 ((uint64_t) PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16),
		 "L1-dcache-loads");

	OPEN(PMC_L1D_MISS, PERF_TYPE_HW_CACHE,
		 PERF_COUNT_HW_CACHE_L1D |
		 ((uint64_t) PERF_COUNT_HW_CACHE_OP_READ << 8) |
		 ((uint64_t) PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
		 "L1-dcache-load-misses");

	OPEN(PMC_LLC_MISS, PERF_TYPE_HW_CACHE,
		 PERF_COUNT_HW_CACHE_LL |
		 ((uint64_t) PERF_COUNT_HW_CACHE_OP_READ << 8) |
		 ((uint64_t) PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
		 "LLC-load-misses");

	OPEN(PMC_BRANCH_MISS, PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES,
		 "branch-misses");

	OPEN(PMC_DTLB_MISS, PERF_TYPE_HW_CACHE,
		 PERF_COUNT_HW_CACHE_DTLB |
		 ((uint64_t) PERF_COUNT_HW_CACHE_OP_READ << 8) |
		 ((uint64_t) PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
		 "dTLB-load-misses");

#undef OPEN

	for (i = 0; i < PMC_NUM_EVENTS; i++)
		g->valid[i] = (g->fds[i] >= 0);
	g->available = false;
	for (i = 0; i < PMC_NUM_EVENTS; i++)
	{
		if (g->valid[i])
		{
			g->available = true;
			break;
		}
	}
	if (!g->available)
	{
		snprintf(g->reason, sizeof(g->reason), "%s",
				 first_err[0] ? first_err : "no PMC event could be opened");
	}
	return g->available ? 0 : -1;
}

void
pmc_close(pmc_group *g)
{
	int			i;

	for (i = 0; i < PMC_NUM_EVENTS; i++)
	{
		if (g->fds[i] >= 0)
		{
			close(g->fds[i]);
			g->fds[i] = -1;
		}
	}
	g->available = false;
}

void
pmc_reset(pmc_group *g)
{
	int			i;

	for (i = 0; i < PMC_NUM_EVENTS; i++)
		if (g->fds[i] >= 0)
			ioctl(g->fds[i], PERF_EVENT_IOC_RESET, 0);
}

void
pmc_enable(pmc_group *g)
{
	int			i;

	for (i = 0; i < PMC_NUM_EVENTS; i++)
		if (g->fds[i] >= 0)
			ioctl(g->fds[i], PERF_EVENT_IOC_ENABLE, 0);
}

void
pmc_disable(pmc_group *g)
{
	int			i;

	for (i = 0; i < PMC_NUM_EVENTS; i++)
		if (g->fds[i] >= 0)
			ioctl(g->fds[i], PERF_EVENT_IOC_DISABLE, 0);
}

bool
pmc_read(pmc_group *g)
{
	struct read_format rf;
	int			i;
	bool		any = false;

	for (i = 0; i < PMC_NUM_EVENTS; i++)
	{
		g->values[i] = 0;
		if (g->fds[i] < 0)
			continue;
		if (read(g->fds[i], &rf, sizeof(rf)) != sizeof(rf))
			continue;
		if (rf.time_running == 0)
			continue;
		/* 按调度比例折算 */
		g->values[i] = (uint64_t) ((double) rf.value *
								   (double) rf.time_enabled /
								   (double) rf.time_running);
		any = true;
	}
	return any;
}

const char *
pmc_name(int idx)
{
	static const char *names[PMC_NUM_EVENTS] = {
		"instructions", "cycles", "L1-dcache-loads", "L1-dcache-load-misses",
		"LLC-load-misses", "branch-misses", "dTLB-load-misses"
	};

	return (idx >= 0 && idx < PMC_NUM_EVENTS) ? names[idx] : "?";
}

const char *
pmc_unit(int idx)
{
	static const char *units[PMC_NUM_EVENTS] = {
		"条", "周期", "次", "次", "次", "次", "次"
	};

	return (idx >= 0 && idx < PMC_NUM_EVENTS) ? units[idx] : "";
}

double
pmc_l1d_hit_rate(const pmc_group *g)
{
	if (!g->valid[PMC_L1D_ACCESS] || !g->valid[PMC_L1D_MISS])
		return -1.0;
	if (g->values[PMC_L1D_ACCESS] == 0)
		return -1.0;
	return 100.0 * (1.0 - (double) g->values[PMC_L1D_MISS] /
					(double) g->values[PMC_L1D_ACCESS]);
}

#else							/* !__linux__ */

#include "pmc.h"

#include <stdio.h>
#include <string.h>

/*
 * 非 Linux 平台（macOS / *BSD / Windows）：
 *   perf_event_open 是 Linux 专有系统调用，macOS 没有对应接口，
 *   这里给出"不可用"的明确原因，让上层 benchmark 仍能跑通算法部分。
 */
#if defined(__APPLE__)
#define PMC_PLATFORM_NAME "macOS"
#elif defined(_WIN32)
#define PMC_PLATFORM_NAME "Windows"
#else
#define PMC_PLATFORM_NAME "非 Linux 平台"
#endif

int
pmc_open(pmc_group *g)
{
	memset(g, 0, sizeof(*g));
	for (int i = 0; i < PMC_NUM_EVENTS; i++)
		g->fds[i] = -1;
	g->available = false;
	snprintf(g->reason, sizeof(g->reason),
			 "perf_event_open() 是 Linux 专有系统调用，当前平台为 "
			 PMC_PLATFORM_NAME "，无此接口");
	return -1;
}

void
pmc_close(pmc_group *g)
{
	(void) g;
}

void
pmc_reset(pmc_group *g)
{
	(void) g;
}

void
pmc_enable(pmc_group *g)
{
	(void) g;
}

void
pmc_disable(pmc_group *g)
{
	(void) g;
}

bool
pmc_read(pmc_group *g)
{
	(void) g;
	return false;
}

const char *
pmc_name(int idx)
{
	static const char *names[PMC_NUM_EVENTS] = {
		"instructions", "cycles", "L1-dcache-loads", "L1-dcache-load-misses",
		"LLC-load-misses", "branch-misses", "dTLB-load-misses"
	};

	return (idx >= 0 && idx < PMC_NUM_EVENTS) ? names[idx] : "?";
}

const char *
pmc_unit(int idx)
{
	(void) idx;
	return "";
}

double
pmc_l1d_hit_rate(const pmc_group *g)
{
	(void) g;
	return -1.0;
}

#endif							/* __linux__ */
