CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
LINUX_SRC ?= $(HOME)/schedlab/kernel/linux-6.17

BENCH_BIN := build/cpu_worker build/latency_worker

.PHONY: all bench scheduler clean help

all: bench

bench: $(BENCH_BIN)

build:
	mkdir -p build bin results

build/cpu_worker: bench/cpu_worker.c | build
	$(CC) $(CFLAGS) $< -o $@

build/latency_worker: bench/latency_worker.c | build
	$(CC) $(CFLAGS) $< -o $@

scheduler:
	bash scripts/build_scheduler.sh "$(LINUX_SRC)"

clean:
	rm -f $(BENCH_BIN) bin/scx_mlfq

help:
	@echo "make bench                         build workload programs"
	@echo "make scheduler LINUX_SRC=/path    install and build scx_mlfq"
	@echo "make all                           same as make bench"
