# 在 Linux 上跑 perf_event_open，需要 --privileged 或至少放开 perf_event_paranoid
FROM --platform=linux/amd64 gcc:13-bookworm

WORKDIR /work
COPY . .

RUN apt-get update && apt-get install -y --no-install-recommends bsdmainutils \
    && rm -rf /var/lib/apt/lists/*

RUN make

# 容器内的 perf_event_paranoid 由宿主机内核决定，宿主机需:
#   docker run --rm --privileged llsearch-pmc
CMD ["sh", "-c", "cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null; ./llsearch_bench -s 1000,10000,100000,1000000 -r 5 -o results_linux.csv && column -s, -t < results_linux.csv"]
