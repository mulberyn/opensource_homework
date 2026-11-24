/*
 * bench.c -- 链表线性搜索基准 + PMC 指标采集
 *
 * 用法：
 *   ./llsearch_bench [-n 节点数] [-q 查询数] [-r 重复次数] [-s 规模扫描] [-o 结果CSV]
 *
 * 测量方式：
 *   1. 先在测量区间之外把链表建好、把查询向量生成好；
 *   2. pmc_reset() 清计数器 -> pmc_enable() -> 跑完全部查询 -> pmc_disable()
 *      -> pmc_read() 读结果；
 *   3. 除以查询数得到"每条查询"的平均指标。
 *
 * 注意：单条查询的指令数只有几十~几百条，用 perf 单独测一条会被
 * 读写计数器的系统调用开销完全淹没，所以必须"批量查询 + 一次读数"。
 */
#include "list.h"
#include "pmc.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

/* ---------- 计时 ---------- */
static double
now_sec(void)
{
#if defined(__APPLE__)
	static mach_timebase_info_data_t tb = {0, 0};

	if (tb.denom == 0)
		mach_timebase_info(&tb);
	return (double) mach_absolute_time() * tb.numer / tb.denom / 1e9;
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
#endif
}

/* 进程 CPU 时间：受机器负载影响远小于墙钟时间，适合做性能对比 */
static double
cpu_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static uint64_t
xorshift64(uint64_t *s)
{
	uint64_t	x = *s;

	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*s = x;
	return x * 2685821657736338717ULL;
}

/* 一条查询 */
typedef struct
{
	uint64_t	key;
	int			expect_hit;		/* 1 表示该 key 在链表中，0 表示不在 */
} query_t;

static query_t *
make_queries(uint64_t n, uint64_t q, uint64_t *out_misses)
{
	query_t    *qs = (query_t *) malloc((size_t) q * sizeof(query_t));
	uint64_t	seed = 0xDEADBEEFCAFEBABEULL;
	uint64_t	i;
	uint64_t	misses = 0;

	if (qs == NULL)
		return NULL;

	for (i = 0; i < q; i++)
	{
		if ((i & 1) == 0)
		{
			/* 命中：key 一定在链表中，位置随机 */
			qs[i].key = xorshift64(&seed) % n;
			qs[i].expect_hit = 1;
		}
		else
		{
			/* 未命中：key 一定不在 [0, n) 内，必须走完整条链表 */
			qs[i].key = n + 1 + xorshift64(&seed) % n;
			qs[i].expect_hit = 0;
			misses++;
		}
	}
	if (out_misses)
		*out_misses = misses;
	return qs;
}

/* 跑一遍全部查询，返回校验和（防止编译器把查询优化掉） */
static uint64_t
run_queries(list_t *L, const query_t *qs, uint64_t q, uint64_t *total_probes,
			uint64_t *hits, uint64_t *misses)
{
	uint64_t	ck = 0;
	uint64_t	probes_all = 0;
	uint64_t	h = 0,
				m = 0;
	uint64_t	i;

	for (i = 0; i < q; i++)
	{
		uint64_t	pr = 0;
		list_node  *p = list_search(L, qs[i].key, &pr);

		probes_all += pr;
		if (p != NULL)
		{
			h++;
			ck ^= p->key + i;
		}
		else
		{
			m++;
			ck += i;
		}
	}
	if (total_probes)
		*total_probes = probes_all;
	if (hits)
		*hits = h;
	if (misses)
		*misses = m;
	return ck;
}

typedef struct
{
	uint64_t	n;
	uint64_t	q;
	uint64_t	probes;
	double		sec;
	double		cpu;
	uint64_t	hits;
	uint64_t	misses;
	uint64_t	ck;
	bool		pmc_ok;
	uint64_t	pmc[PMC_NUM_EVENTS];
	double		l1_hit;
} result_t;

static void
bench_one(pmc_group *g, uint64_t n, uint64_t q, uint64_t repeats,
		  FILE *csv, result_t *out)
{
	list_t	   *L;
	query_t    *qs;
	uint64_t	misses_gen = 0;
	uint64_t	r;
	double		best = 1e30;
	double		best_wall = 0;
	uint64_t	best_probes = 0,
				best_hits = 0,
				best_misses = 0,
				best_ck = 0;
	bool		best_pmc_ok = false;
	uint64_t	best_pmc[PMC_NUM_EVENTS] = {0};
	double		best_l1 = -1;

	L = list_create(n);
	qs = make_queries(n, q, &misses_gen);
	if (L == NULL || qs == NULL)
	{
		fprintf(stderr, "内存不足：n=%" PRIu64 " q=%" PRIu64 "\n", n, q);
		exit(1);
	}

	for (r = 0; r < repeats; r++)
	{
		uint64_t	probes = 0,
					hits = 0,
					miss = 0,
					ck;
		double		t0,
					t1,
					c0,
					c1;
		bool		ok = false;
		uint64_t	vals[PMC_NUM_EVENTS] = {0};

		/* 热身：让链表进入 cache / TLB（不计入测量） */
		ck = run_queries(L, qs, q, NULL, NULL, NULL);
		(void) ck;

		pmc_reset(g);
		pmc_enable(g);
		t0 = now_sec();
		c0 = cpu_sec();
		ck = run_queries(L, qs, q, &probes, &hits, &miss);
		c1 = cpu_sec();
		t1 = now_sec();
		pmc_disable(g);
		if (g->available)
		{
			ok = pmc_read(g);
			for (int i = 0; i < PMC_NUM_EVENTS; i++)
				vals[i] = g->values[i];
		}

		if (c1 - c0 < best)
		{
			best = c1 - c0;
			best_wall = t1 - t0;
			best_probes = probes;
			best_hits = hits;
			best_misses = miss;
			best_ck = ck;
			best_pmc_ok = ok;
			memcpy(best_pmc, vals, sizeof(best_pmc));
			best_l1 = pmc_l1d_hit_rate(g);
		}

		fprintf(stderr,
				"  repeat %" PRIu64 "/%" PRIu64 ": %" PRIu64 " 条查询, %.3f ms, "
				"平均探测 %.2f 个节点\n",
				r + 1, repeats, q, (t1 - t0) * 1e3,
				q ? (double) probes / (double) q : 0.0);
	}

	out->n = n;
	out->q = q;
	out->probes = best_probes;
	out->sec = best_wall;
	out->cpu = best;
	out->hits = best_hits;
	out->misses = best_misses;
	out->ck = best_ck;
	out->pmc_ok = best_pmc_ok;
	memcpy(out->pmc, best_pmc, sizeof(best_pmc));
	out->l1_hit = best_l1;

	if (csv != NULL)
	{
		fprintf(csv,
				"%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.9f,%.9f,%.4f,%.6f",
				n, q, out->probes, out->sec, out->cpu,
				q ? (double) out->probes / (double) q : 0.0,
				q ? out->sec * 1e9 / (double) q : 0.0);
		for (int i = 0; i < PMC_NUM_EVENTS; i++)
			fprintf(csv, ",%" PRIu64, out->pmc[i]);
		if (out->l1_hit >= 0)
			fprintf(csv, ",%.4f\n", out->l1_hit);
		else
			fprintf(csv, ",\n");
	}

	list_destroy(L);
	free(qs);
}

static void
print_report(const result_t *r, const pmc_group *g)
{
	double		per_q = r->q ? (double) r->q : 1.0;

	printf("\n=== 链表线性搜索：N=%" PRIu64 " 节点，%" PRIu64 " 条查询"
		   "（命中 %" PRIu64 " / 未命中 %" PRIu64 "），取最快一轮 ===\n",
		   r->n, r->q, r->hits, r->misses);
	printf("墙钟耗时          : %.3f ms（受机器负载影响，仅供参考）\n", r->sec * 1e3);
	printf("CPU 时间          : %.3f ms\n", r->cpu * 1e3);
	printf("单条查询 CPU 时间 : %.1f ns\n", r->cpu * 1e9 / per_q);
	printf("平均探测节点数    : %.2f 个/条（理论值：命中 %.1f，未命中 %" PRIu64 "）\n",
		   (double) r->probes / per_q, (double) r->n / 2.0, r->n);
	printf("折算访存字节数    : %.2f MiB（每个节点 64B）\n",
		   (double) r->probes * 64.0 / 1048576.0);
	printf("校验和            : 0x%" PRIx64 "（仅用于防止死代码消除）\n\n", r->ck);

	if (!r->pmc_ok)
	{
		printf("[PMC 不可用] %s\n\n", g->reason);
		return;
	}

	printf("%-22s %16s %16s\n", "指标", "总计", "每查询平均");
	printf("%-22s %16s %16s\n", "----------------------",
		   "----------------", "----------------");
	for (int i = 0; i < PMC_NUM_EVENTS; i++)
	{
		if (!g->valid[i])
		{
			printf("%-22s %16s %16s\n", pmc_name(i), "N/A", "N/A");
			continue;
		}
		printf("%-22s %16" PRIu64 " %16.2f  %s\n",
			   pmc_name(i), r->pmc[i],
			   (double) r->pmc[i] / per_q, pmc_unit(i));
	}

	if (r->l1_hit >= 0)
		printf("\nL1D 读命中率      : %.2f%%（miss %" PRIu64 " / access %" PRIu64 "）\n",
			   r->l1_hit, r->pmc[PMC_L1D_MISS], r->pmc[PMC_L1D_ACCESS]);
	if (r->pmc[PMC_CYCLES] && r->pmc[PMC_INSTRUCTIONS])
		printf("IPC               : %.3f 条/周期\n",
			   (double) r->pmc[PMC_INSTRUCTIONS] / (double) r->pmc[PMC_CYCLES]);
	if (r->pmc[PMC_CYCLES])
		printf("CPI               : %.3f 周期/条\n",
			   (double) r->pmc[PMC_CYCLES] / (double) r->pmc[PMC_INSTRUCTIONS]);
	if (r->pmc[PMC_L1D_ACCESS])
		printf("每次访存平均指令数: %.2f 条（越小说明访存越密集、越受 cache 限制）\n",
			   (double) r->pmc[PMC_INSTRUCTIONS] / (double) r->pmc[PMC_L1D_ACCESS]);
	printf("\n");
}

static void
usage(const char *prog)
{
	printf("用法: %s [-n 节点数] [-q 查询数] [-r 重复次数] [-s 规模列表] [-o CSV]\n"
		   "  -n  链表节点数，默认 100000\n"
		   "  -q  查询条数，默认自动（每档规模总探测次数约 50 万）\n"
		   "  -r  重复次数（取最快一轮），默认 5\n"
		   "  -s  规模扫描，如 1000,10000,100000,1000000（指定后忽略 -n）\n"
		   "  -o  把每档规模的汇总结果追加写入 CSV 文件\n", prog);
}

int
main(int argc, char **argv)
{
	uint64_t	n = 100000;
	uint64_t	q = 0;              /* 0 = 按规模自动 */
	uint64_t	repeats = 5;
	const char *sizes = NULL;
	const char *csv_path = NULL;
	pmc_group	g;
	int			opt;

	while ((opt = getopt(argc, argv, "n:q:r:s:o:h")) != -1)
	{
		switch (opt)
		{
			case 'n':
				n = strtoull(optarg, NULL, 10);
				break;
			case 'q':
				q = strtoull(optarg, NULL, 10);
				break;
			case 'r':
				repeats = strtoull(optarg, NULL, 10);
				break;
			case 's':
				sizes = optarg;
				break;
			case 'o':
				csv_path = optarg;
				break;
			default:
				usage(argv[0]);
				return opt == 'h' ? 0 : 1;
		}
	}
	if (repeats == 0)
		repeats = 1;

	if (pmc_open(&g) != 0)
	{
		/* PMC 不可用不是致命错误：算法部分照常测 */
		fprintf(stderr, "[警告] PMC 不可用：%s\n", g.reason);
	}

	FILE	   *csv = NULL;

	if (csv_path != NULL)
	{
		csv = fopen(csv_path, "w");
		if (csv == NULL)
		{
			perror("fopen");
			return 1;
		}
		fprintf(csv, "nodes,queries,probes,wall_sec,cpu_sec,probes_per_query,ns_cpu_per_query");
		for (int i = 0; i < PMC_NUM_EVENTS; i++)
			fprintf(csv, ",%s", pmc_name(i));
		fprintf(csv, ",l1d_hit_rate_pct\n");
	}

	if (sizes != NULL)
	{
		char	   *buf = strdup(sizes);
		char	   *tok,
				   *save = NULL;
		uint64_t	step = 0;
		uint64_t	qq;

		for (tok = strtok_r(buf, ",", &save); tok != NULL;
			 tok = strtok_r(NULL, ",", &save))
		{
			result_t	r;

			n = strtoull(tok, NULL, 10);
			if (n < 2)
				continue;
			step++;
			/* 自动定查询数：让各档规模的"总探测节点数"大致相同（约 2000 万），
			 * 既保证每档都能被充分"摸到"，又不会让 1e6 规模的跑几十分钟。
			 * 平均探测 N/2，故 q ≈ 2 * 目标探测数 / N。 */
			qq = q;
			if (qq == 0)
			{
				/* 一次探测 = 一次随机的指针追逐，在 1e6 规模下约 100ns，
				 * 所以每档规模的总探测次数控制在 50 万左右，跑得快且稳定。 */
				double		target = 5.0e5;
				double		want = target / ((double) n / 2.0);

				if (want < 100)
					want = 100;
				if (want > 2000000)
					want = 2000000;
				qq = (uint64_t) want;
			}
			fprintf(stderr, "\n[%llu] N=%" PRIu64 ", Q=%" PRIu64 "\n",
					(unsigned long long) step, n, qq);
			bench_one(&g, n, qq, repeats, csv, &r);
			print_report(&r, &g);
		}
		free(buf);
	}
	else
	{
		result_t	r;
		uint64_t	qq = q;

		if (qq == 0)
			qq = 100000;
		bench_one(&g, n, qq, repeats, csv, &r);
		print_report(&r, &g);
	}

	if (csv != NULL)
	{
		fclose(csv);
		printf("CSV 已写入: %s\n", csv_path);
	}
	pmc_close(&g);
	return 0;
}
