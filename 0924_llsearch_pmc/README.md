# 链表搜索 + perf_event_open 性能计数（0924 课1-2 开发与分析题）

按作业要求实现：

1. **不用 HASH**（也不建跳表、不建索引），只做最朴素的单链表线性搜索；
2. 用 `perf_event_open(2)` 直接开硬件性能计数器（PMC），统计
   **指令数、周期数、访存次数、L1 Cache 命中率**，另加 LLC miss、分支预测失败、dTLB miss 做交叉验证；
3. 输出"每条查询"的平均值，并在不同链表规模下做规模扫描。

## 1. 目录结构

```
0924_llsearch_pmc/
├── list.h / list.c     # 单链表构建 + 线性搜索（无 HASH）
├── pmc.h  / pmc.c      # perf_event_open 封装（Linux 真计数 / 非 Linux 优雅降级）
├── bench.c             # 基准：规模扫描、批量查询、一次读数、CSV 输出
├── Makefile
├── run_linux.sh        # Linux 上一键编译 + 运行（含 perf_event_paranoid 检查）
├── Dockerfile          # 用容器拿到 Linux 环境
└── results_macos.csv   # 本机（macOS）跑出来的算法层数据（PMC 列为 0）
```

## 2. 为什么节点要做成 64 字节

```c
typedef struct list_node {
    uint64_t key;                          /*  8 B */
    uint64_t payload[NODE_PAYLOAD_WORDS];  /* 48 B，模拟行数据 */
    struct list_node *next;                /*  8 B */
} list_node;                               /* 合计 64 B = 一条 cache line */
```

这样"访问一个节点"≈"访问一条 cache line"，L1D 缺失率可以直接对应到
**指针追逐（pointer chasing）** 的代价：第 k 个节点的地址来自第 k-1 个节点里的
`next`，CPU 无法提前算出下一个地址，硬件预取器也帮不上忙，只能一级一级等。

## 3. 编译与运行

```bash
make                 # 产出 llsearch_bench
make run             # 1e3 ~ 1e6 四档规模扫描
make csv             # 同上，结果写入 results.csv
```

参数：

| 参数 | 含义 | 默认 |
| --- | --- | --- |
| `-n` | 链表节点数 | 100000 |
| `-q` | 查询条数（一半命中、一半未命中） | 自动（每档总探测约 50 万次） |
| `-r` | 重复次数，取最快一轮 | 5 |
| `-s` | 规模扫描，如 `1000,10000,100000,1000000` | 关 |
| `-o` | 汇总 CSV | 关 |

## 4. PMC 的打开方式（关键代码）

```c
struct perf_event_attr attr = {0};
attr.type   = PERF_TYPE_HARDWARE;
attr.config = PERF_COUNT_HW_INSTRUCTIONS;   /* 指令数 */
attr.disabled = 1;                          /* 先不计数 */
attr.exclude_kernel = 1;                    /* 只看用户态 */
attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
int fd = syscall(SYS_perf_event_open, &attr, 0 /* 本进程 */, -1 /* 任意 CPU */, -1, 0);
```

L1D 访存/缺失属于 **hardware cache event**，`config` 要按位拼：

```
config = cache_id | (op << 8) | (result << 16)
       = PERF_COUNT_HW_CACHE_L1D
       | (PERF_COUNT_HW_CACHE_OP_READ  << 8)
       | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16)     /* 访存次数 */
       ...RESULT_MISS                                  /* 缺失次数 */
```

命中率 = `1 - L1D_MISS / L1D_ACCESS`。

**为什么要按 `time_enabled / time_running` 折算**：一个 PMU 上的计数器只有几个到十几个，
同时打开 7 个事件时内核会让它们分时复用（multiplexing），`time_running < time_enabled`
时读数偏小，必须按比例放大。代码里已经处理。

## 5. Linux 上的权限

`perf_event_open` 受 `kernel.perf_event_paranoid` 限制：

| 值 | 能做什么 |
| --- | --- |
| -1 | 几乎不受限 |
| 0 | 可以统计自己进程的 PMC（**本作业够用**） |
| 1 | 只能统计 CPU 层面的部分事件 |
| 2 及以上 | 基本只能看软件事件，硬件事件会 `EACCES` |

```bash
cat /proc/sys/kernel/perf_event_paranoid
sudo sysctl -w kernel.perf_event_paranoid=0
```

## 6. 在 Linux 上运行

```bash
./run_linux.sh                       # 自动检查 paranoid、编译、跑四档规模
# 或者用容器
docker build -t llsearch-pmc .
docker run --rm --privileged llsearch-pmc
```

## 7. 本机（macOS）能测到什么

`perf_event_open` 是 Linux 专有系统调用，macOS 没有对应接口，所以本机只能测**算法层**
指标：探测节点数、CPU 时间、每查询耗时。`results_macos.csv` 就是本机跑出来的：

| 节点数 N | 每查询平均探测节点数 | 每查询 CPU 时间 | 每探测节点耗时 |
| --- | --- | --- | --- |
| 1 000 | 748.83 | 1 195 ns | 1.6 ns（L1 命中） |
| 10 000 | 7 462.81 | 71 630 ns | 9.6 ns（L2） |
| 100 000 | 72 815.21 | 2 207 310 ns | 30.3 ns（L3/SLC） |
| 1 000 000 | 741 602.52 | 98 042 240 ns | 132.2 ns（DRAM） |

可以看到：**探测节点数严格线性增长（≈0.75N，理论值就是 0.75N）**，而每个节点的
耗时从 1.6 ns 涨到 132 ns —— 这就是"链表搜索随规模变慢"的真正原因：不是比较次数多，
而是每一次比较都要等一次访存。

硬件计数器（指令数/周期数/L1 命中率）需要在 Linux 上跑 `run_linux.sh` 才有数，
本报告 0924 的对应小节里写清了这一点。
