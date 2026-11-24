CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
BIN      = llsearch_bench
OBJS     = list.o pmc.o bench.o

.PHONY: all clean run bench csv

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

list.o: list.c list.h
pmc.o: pmc.c pmc.h
bench.o: bench.c list.h pmc.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# 默认规模：1e3 ~ 1e6 四档
run: $(BIN)
	./$(BIN) -s 1000,10000,100000,1000000 -q 200000 -r 5

csv: $(BIN)
	./$(BIN) -s 1000,10000,100000,1000000 -q 200000 -r 5 -o results.csv

clean:
	rm -f $(BIN) $(OBJS) results.csv
