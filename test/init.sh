#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo 8 > /proc/sys/kernel/printk
echo "cpus online: $(cat /sys/devices/system/cpu/online)"

echo "=== T1: load module"
insmod /jprof.ko || { echo "INSMOD FAILED"; poweroff -f; }
ls -l /dev/jprof
cat /proc/jprof/stats

echo "=== T2: whole process, target exits on its own (+ concurrent /proc readers)"
/demo 1 > /dev/null &
D=$!
sleep 0.3
(while kill -0 $D 2>/dev/null; do cat /proc/jprof/threads /proc/jprof/stats > /dev/null; done) &
/jprof_test $D 1
cat /proc/jprof/threads
cat /proc/jprof/stats

echo "=== T3: single-thread filter (hasher only)"
/demo 1 > /dev/null &
D=$!
sleep 1.3
for t in /proc/$D/task/*; do [ "$(cat $t/comm)" = hasher ] && H=${t##*/}; done
echo "hasher tid=$H"
/jprof_test $D 1 0 $H
grep -A5 "^TID" /proc/jprof/threads
wait $D

echo "=== T4: invalid targets"
/jprof_test 99999 1 2>&1 | tail -1
/demo 1 > /dev/null &
D=$!
/jprof_test $D 1 1 1 2>&1 | tail -1          # tid 1 is not a thread of demo
wait $D

echo "=== T5: STOP with time limit while target still running"
/demo 0 > /dev/null &
D=$!
/jprof_test $D 2 1 | head -4
wait $D

echo "=== T6: controller killed mid-run (release path)"
/demo 0 > /dev/null &
D=$!
/jprof_test $D 1 > /dev/null &
T=$!
sleep 1
kill -9 $T
sleep 0.2
grep -E "state|ticks" /proc/jprof/stats
wait $D

echo "=== T7: rmmod while a session is running"
/demo 0 > /dev/null &
D=$!
/jprof_test $D 1 > /dev/null &
T=$!
sleep 0.5
rmmod jprof 2>&1 && echo "rmmod succeeded while open (unexpected)" || echo "rmmod refused while /dev/jprof open: expected"
kill -9 $T
sleep 0.2
rmmod jprof && echo "rmmod ok after close"
ls /proc/jprof 2>&1 | head -1
wait $D
insmod /jprof.ko && rmmod jprof && echo "reload/unload ok"

echo "=== kernel log check"
dmesg | grep -E "BUG|WARNING|circular locking|recursive locking|inconsistent lock|sleeping function|Oops|rcu.*stall|hung_task" || echo "no kernel warnings"
echo "=== DONE"
poweroff -f
