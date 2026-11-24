#!/bin/bash
# overhead.sh - measure jprof's end-to-end cost on a fixed-work benchmark.
#
# For each mode (profiler off, then on at several intervals) runs the
# benchmark REPS times, takes the median wall-clock time, and reports the
# slowdown relative to "off". Interval 1 is run twice: waking the reader on
# every sample (watermark 1) and with the default wakeup watermark, to
# isolate the cost of reader wakeups. Run as root:
#     sudo ./test/overhead.sh [reps]
set -u
cd "$(dirname "$0")/.."
REPS=${1:-5}
BENCH=./demo/bench
CTL=./test/jprof_test
WM=/sys/module/jprof/parameters/wakeup_watermark

[ -x "$BENCH" ] || { echo "build demo/bench first"; exit 1; }
[ -x "$CTL" ]   || { echo "build test/jprof_test first"; exit 1; }
[ -e "$WM" ]    || { echo "load the current module first (insmod kernel/jprof.ko)"; exit 1; }
DEFAULT_WM=$(cat "$WM")

median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }

run_once() {            # $1 = interval, or "off"
	local out pid
	out=$(mktemp)
	"$BENCH" 4 "${ROUNDS:-10000}" 300 > "$out" &
	pid=$!
	if [ "$1" != off ]; then
		"$CTL" "$pid" "$1" > /dev/null
	fi
	wait "$pid"
	cat "$out"
	rm -f "$out"
}

overhead_lines() { sed -n '/^overhead/,$p' /proc/jprof/stats | tail -n +2; }

"$BENCH" 4 1000 0 > /dev/null           # warm-up: caches, CPU frequency

printf "%-22s %9s %9s  %s\n" "MODE" "MEDIAN s" "SLOWDOWN" "runs"
base=""
for mode in off 1:1 1:$DEFAULT_WM 2:$DEFAULT_WM 5:$DEFAULT_WM 10:$DEFAULT_WM; do
	interval=${mode%%:*}
	if [ "$mode" = off ]; then
		label="off"
	else
		echo "${mode##*:}" > "$WM"
		label="${interval} jiffy, wake@${mode##*:}"
	fi
	times=$(for _ in $(seq "$REPS"); do run_once "$interval"; done)
	med=$(echo "$times" | median)
	if [ "$mode" = off ]; then
		base=$med
		slow="-"
	else
		slow=$(awk -v m="$med" -v b="$base" 'BEGIN {printf "%+.2f%%", (m - b) / b * 100}')
	fi
	printf "%-22s %9s %9s  %s\n" "$label" "$med" "$slow" "$(echo $times)"
	case "$mode" in
	1:1)           SELF_WM1=$(overhead_lines) ;;
	1:$DEFAULT_WM) SELF_WMD=$(overhead_lines) ;;
	esac
done
echo "$DEFAULT_WM" > "$WM"

echo
echo "module's self-measured cost, interval 1, waking on every sample:"
echo "$SELF_WM1"
echo
echo "module's self-measured cost, interval 1, waking at $DEFAULT_WM buffered samples:"
echo "$SELF_WMD"
