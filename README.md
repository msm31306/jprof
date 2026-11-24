# jprof: a timer-driven sampling profiler as a Linux kernel module

jprof answers one question about a running program: **which thread uses the CPU, in which function, and what is each thread doing the rest of the time** (running, waiting for a CPU, sleeping, blocked).

A per-CPU timer interrupts every CPU about 1000 times a second. In interrupt context the module checks whether the interrupted task belongs to the target process; if so it records the thread and walks its user stack through frame pointers. Samples go into a lock-free per-CPU ring buffer that user space streams through `/dev/jprof`. One CPU also scans every thread's scheduler state each tick.

Validated against the scheduler's own accounting and against `perf` on x86-64 (WSL2) and arm64 (Apple M4, Ubuntu 24.04 VM). See [RESULTS.md](RESULTS.md) for all measurements and the 15 findings.

## Layout

| Path | What it is |
|---|---|
| `kernel/jprof.c` | the module: timers (jiffies or hrtimer), sampler, stack walk, per-CPU kfifo, thread table, `/dev/jprof`, `/proc/jprof/*` |
| `kernel/jprof_uapi.h` | ioctl numbers and struct layouts shared with user space |
| `kernel/Makefile` | Kbuild; strips debug info from the `.ko` |
| `tool/jprof.py` | report tool: per-thread table, thread states, functions with source lines |
| `tool/jprof_live.py` | live terminal dashboard |
| `demo/demo.c` | known workload: threads doing 3 s, 2 s, 1 s of work, plus a sleeper |
| `demo/live.c` | interactive workload for the dashboard (keys 0-3) |
| `demo/bench.c` | fixed-work benchmark for overhead measurement |
| `demo/duty.c` | bias experiment (nanosleep vs futex wakeups) |
| `test/jprof_test.c`, `test/overhead.sh` | minimal client and overhead benchmark |

## Build and run (Ubuntu 24.04, x86-64 or arm64)

```bash
sudo apt install build-essential linux-headers-$(uname -r) python3 binutils
cd kernel && make && sudo insmod jprof.ko && cd ..
sudo dmesg | tail -1                      # jprof: loaded: /dev/jprof ready, HZ=...

F="-O1 -g -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -pthread -Wall"
for p in demo live bench duty; do gcc $F demo/$p.c -o demo/$p; done

./demo/demo 1 > /dev/null &
sudo python3 tool/jprof.py demo -n 3      # report
sudo python3 tool/jprof_live.py --spawn ./demo/live   # live dashboard
```

Profile any program: `sudo python3 tool/jprof.py <name-or-pid> -d 10 -n 5`. Full call chains need frame pointers (`-fno-omit-frame-pointer`); Ubuntu 24.04 packages already have them.

Module parameters: `clock` (`hrtimer`, default, or `jiffies`) and `wakeup_watermark` (default 64), both writable under `/sys/module/jprof/parameters/`.

Unload with `sudo rmmod jprof`. A module only loads into the kernel it was built against: after a kernel update, run `make` again.

## Requirements and limits

Linux only (a kernel module cannot run on macOS or Windows; use a VM or WSL2). One profiled process at a time. On-CPU stacks only. See the limitations section of RESULTS.md.
