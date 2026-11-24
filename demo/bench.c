/*
 * bench.c - fixed-work benchmark for measuring jprof's end-to-end overhead.
 *
 * Unlike demo.c (which runs for a fixed amount of CPU time), bench does a
 * fixed amount of WORK and reports how long it took in wall-clock time, so
 * any slowdown caused by the profiler shows up directly.
 *
 * Usage: bench [threads] [rounds] [delay_ms]
 *   threads  worker threads, default 4 (one per CPU)
 *   rounds   hash rounds per thread, default 10000
 *   delay_ms wait before starting work, so a profiler can attach, default 300
 * Prints: elapsed seconds (CLOCK_MONOTONIC) for the work phase only.
 *
 * Build: gcc -O1 -g -fno-omit-frame-pointer -pthread -Wall bench.c -o bench
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static volatile uint64_t sink;
static long rounds = 10000;

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);	/* wall clock, not CPU time */
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

__attribute__((noinline)) static uint64_t hash_round(uint64_t h)
{
	for (int i = 0; i < 100000; i++)
		h = (h ^ (uint64_t)i) * 1099511628211ULL;
	return h;
}

static void *worker(void *arg)
{
	uint64_t h = 1469598103934665603ULL + (uintptr_t)arg;

	for (long r = 0; r < rounds; r++)
		h = hash_round(h);
	sink += h;
	return NULL;
}

int main(int argc, char **argv)
{
	int threads = argc > 1 ? atoi(argv[1]) : 4;
	int delay_ms = argc > 3 ? atoi(argv[3]) : 300;
	pthread_t t[64];
	double t0;

	if (argc > 2)
		rounds = atol(argv[2]);
	if (threads < 1 || threads > 64)
		threads = 4;
	usleep(delay_ms * 1000);

	t0 = now();
	for (int i = 0; i < threads; i++)
		pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)i);
	for (int i = 0; i < threads; i++)
		pthread_join(t[i], NULL);
	printf("%.4f\n", now() - t0);
	return 0;
}
