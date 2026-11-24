/*
 * list.h -- 单链表（无 HASH 索引）与线性搜索
 *
 * 按作业要求：**不使用 HASH、不建索引**，搜索就是最朴素的"从头顺着 next 找"。
 * 为了让 PMC 指标有意义，节点被设计成恰好 64 字节（一条 cache line），
 * 这样"访问一个节点"约等于"访问一条 cache line"，L1D 缺失率可以直接对应
 * 到"指针追逐（pointer chasing）"的代价上。
 */
#ifndef LIST_H
#define LIST_H

#include <stddef.h>
#include <stdint.h>

#define NODE_PAYLOAD_WORDS 6	/* 8(key) + 6*8(payload) + 8(next) = 64 字节 */

typedef struct list_node
{
	uint64_t	key;							/* 搜索键 */
	uint64_t	payload[NODE_PAYLOAD_WORDS];	/* 模拟行数据，占满 cache line */
	struct list_node *next;
} list_node;

/* 链表对象：nodes 是连续内存块，head 是逻辑链首 */
typedef struct list
{
	list_node  *nodes;
	list_node  *head;
	uint64_t	n;
} list_t;

/*
 * 创建长度为 n 的单链表，节点 key 为 0..n-1，
 * 但节点在内存中的顺序被随机打乱（模拟数据的自然插入顺序），
 * 因此按 key 搜索时平均要走过 n/2 个节点。
 */
list_t	   *list_create(uint64_t n);

/*
 * 线性搜索：从头节点开始逐个比较 key，直到命中或走到尾。
 * probes（可为 NULL）返回本次搜索实际比较过的节点数。
 */
list_node  *list_search(list_t *L, uint64_t key, uint64_t *probes);

void		list_destroy(list_t *L);

#endif							/* LIST_H */
