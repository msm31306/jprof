/*
 * duty.c - bias experiment: 3 workers, each busy 5 ms of every 10 ms,
 * differing only in HOW a worker waits out the idle half of each slot.
 *
 * Usage: duty <mode> [seconds]
 *   sleep  nanosleep() until the slot ends (same as live.c). The worker is
 *          woken by an hrtimer on its own CPU.
 *   futex  wait on a condition variable. A "pacer" thread spins on the
 *          clock and broadcasts at each slot boundary, so the wakeup comes
 *          from another CPU (an IPI), not from a timer on the worker's CPU.
 *
 * Run the "sleep" mode with background CPU hogs (another process) to keep
 * CPUs from going idle; that separates "wakeup timer" from "idle CPU"
 * effects.
 *
 * Build: gcc -O1 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer \
 *            -pthread -Wall duty.c -o duty
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOT_NS	10000000ULL
#define BUSY_NS	5000000ULL
#define NW	3

static int use_futex;
static volatile uint64_t sink;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint64_t slot_no;

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

__attribute__((noinline)) static void burn(uint64_t until)
{
	uint64_t h = 1469598103934665603ULL;

	while (now_ns() < until)
		for (int i = 0; i < 2000; i++)
			h = (h ^ (uint64_t)i) * 1099511628211ULL;
	sink += h;
}

static void *pacer(void *arg)
{
	uint64_t next = *(uint64_t *)arg;

	(void)arg;
	pthread_setname_np(pthread_self(), "pacer");
	for (;;) {
		while (now_ns() < next)
			;			/* spin: never sleeps */
		pthread_mutex_lock(&mu);
		slot_no++;
		pthread_cond_broadcast(&cv);
		pthread_mutex_unlock(&mu);
		next += SLOT_NS;
	}
	return NULL;
}

static void *worker(void *arg)
{
	char name[16];
	uint64_t slot = *(uint64_t *)arg, seen = 0;

	snprintf(name, sizeof(name), "w%d", (int)(intptr_t)((uint64_t *)arg)[1]);
	pthread_setname_np(pthread_self(), name);
	for (;;) {
		burn(slot + BUSY_NS);
		slot += SLOT_NS;
		if (use_futex) {
			pthread_mutex_lock(&mu);
			if (!seen)
				seen = slot_no;
			while (slot_no == seen)
				pthread_cond_wait(&cv, &mu);
			seen = slot_no;
			pthread_mutex_unlock(&mu);
			slot = now_ns();	/* slot starts at the wakeup */
		} else {
			uint64_t t = now_ns();

			if (slot > t) {
				struct timespec d = { 0, (long)(slot - t) };

				nanosleep(&d, NULL);
			} else {
				slot = t;
			}
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t t;
	uint64_t args[NW][2], start, secs;

	if (argc < 2 || (strcmp(argv[1], "sleep") && strcmp(argv[1], "futex"))) {
		fprintf(stderr, "usage: %s sleep|futex [seconds]\n", argv[0]);
		return 2;
	}
	use_futex = !strcmp(argv[1], "futex");
	secs = argc > 2 ? strtoull(argv[2], NULL, 10) : 6;
	pthread_setname_np(pthread_self(), "main");
	start = now_ns();
	if (use_futex) {
		static uint64_t first;

		first = start + SLOT_NS;
		pthread_create(&t, NULL, pacer, &first);
	}
	for (int i = 0; i < NW; i++) {
		args[i][0] = start;
		args[i][1] = i + 1;
		pthread_create(&t, NULL, worker, args[i]);
	}
	struct timespec d = { (time_t)secs, 0 };

	nanosleep(&d, NULL);		/* main sleeps; exit() ends all threads */
	return 0;
}
