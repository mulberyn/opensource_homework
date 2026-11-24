/*
 * pmc.h -- 基于 perf_event_open(2) 的硬件性能计数器（PMC）封装
 *
 * 设计目标：
 *   1. 只用系统调用 perf_event_open 直接开计数器，不依赖 libpfm / PAPI，
 *      以便看清楚 PMC 的真实打开方式；
 *   2. 自动处理"一个 PMU 上的计数器不够用、内核轮流调度"（multiplexing）的情况，
 *      按 time_enabled / time_running 折算；
 *   3. 在非 Linux 平台（如 macOS）编译时退化为"不可用"，并给出原因，
 *      保证同一份 benchmark 代码可跨平台编译运行。
 */
#ifndef PMC_H
#define PMC_H

#include <stdbool.h>
#include <stdint.h>

/* 本作业要求的 4 个指标，另加 3 个辅助指标用于交叉验证 */
enum pmc_index
{
	PMC_INSTRUCTIONS = 0,		/* 指令数 */
	PMC_CYCLES,					/* 周期数（CPU 时钟周期） */
	PMC_L1D_ACCESS,				/* L1D 访存次数（读） */
	PMC_L1D_MISS,				/* L1D 读缺失次数 */
	PMC_LLC_MISS,				/* 末级缓存缺失 */
	PMC_BRANCH_MISS,			/* 分支预测失败 */
	PMC_DTLB_MISS,				/* 数据 TLB 缺失 */
	PMC_NUM_EVENTS
};

typedef struct pmc_group
{
	int			fds[PMC_NUM_EVENTS];
	uint64_t	values[PMC_NUM_EVENTS];
	bool		valid[PMC_NUM_EVENTS];
	bool		available;		/* 是否至少有一个计数器可用 */
	char		reason[256];	/* 不可用时说明原因 */
} pmc_group;

/* 打开并复位全部计数器。返回 0 表示至少有一个可用。 */
int			pmc_open(pmc_group *g);

/* 关闭全部计数器 */
void		pmc_close(pmc_group *g);

/* 把计数器清零（在测量区间开始前调用） */
void		pmc_reset(pmc_group *g);

/* 使能 / 停止计数（把测量区间"夹"起来，避免把准备阶段算进去） */
void		pmc_enable(pmc_group *g);
void		pmc_disable(pmc_group *g);

/* 读取计数结果，已按 time_enabled/time_running 折算 */
bool		pmc_read(pmc_group *g);

const char *pmc_name(int idx);
const char *pmc_unit(int idx);

/* L1D 命中率；无数据时返回 -1 */
double		pmc_l1d_hit_rate(const pmc_group *g);

#endif							/* PMC_H */
