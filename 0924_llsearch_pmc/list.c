/*
 * list.c -- 单链表构建与线性搜索（不使用 HASH）
 */
#include "list.h"

#include <stdlib.h>
#include <string.h>

/* xorshift64*：避免依赖 rand() 的平台差异，保证链表顺序可复现 */
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

list_t *
list_create(uint64_t n)
{
	list_t	   *L;
	uint64_t   *perm;
	uint64_t	seed = 0x9E3779B97F4A7C15ULL;
	uint64_t	i;

	if (n == 0)
		return NULL;

	L = (list_t *) malloc(sizeof(list_t));
	if (L == NULL)
		return NULL;

	/* 64 字节对齐：每个节点正好落在一条 cache line 上 */
	if (posix_memalign((void **) &L->nodes, 64,
					   (size_t) n * sizeof(list_node)) != 0)
	{
		free(L);
		return NULL;
	}

	perm = (uint64_t *) malloc((size_t) n * sizeof(uint64_t));
	if (perm == NULL)
	{
		free(L->nodes);
		free(L);
		return NULL;
	}

	for (i = 0; i < n; i++)
	{
		perm[i] = i;
		memset(&L->nodes[i], 0, sizeof(list_node));
		L->nodes[i].key = i;
	}

	/* Fisher-Yates 洗牌：决定节点的逻辑先后顺序 */
	for (i = n; i > 1; i--)
	{
		uint64_t	j = xorshift64(&seed) % i;
		uint64_t	tmp = perm[i - 1];

		perm[i - 1] = perm[j];
		perm[j] = tmp;
	}

	for (i = 0; i + 1 < n; i++)
		L->nodes[perm[i]].next = &L->nodes[perm[i + 1]];
	L->nodes[perm[n - 1]].next = NULL;

	L->head = &L->nodes[perm[0]];
	L->n = n;
	free(perm);
	return L;
}

list_node *
list_search(list_t *L, uint64_t key, uint64_t *probes)
{
	list_node  *p;
	uint64_t	cnt = 0;

	/* 链表搜索的全部逻辑：无 HASH、无索引，逐个 next 比较 */
	for (p = L->head; p != NULL; p = p->next)
	{
		cnt++;
		if (p->key == key)
		{
			if (probes)
				*probes = cnt;
			return p;
		}
	}
	if (probes)
		*probes = cnt;
	return NULL;
}

void
list_destroy(list_t *L)
{
	if (L == NULL)
		return;
	free(L->nodes);
	free(L);
}
