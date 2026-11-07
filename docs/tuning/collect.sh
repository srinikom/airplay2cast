#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Srini K
# One CSV row per streaming run of each bridge build, against the speaker in $SPEAKER
# (fake shairport-sync source). Run on the test host from the deploy directory, where
# make docker-test put out/ and tests/:
#   SPEAKER="Office speaker" collect.sh RUNS LABEL=BINARY[:extra args] ...
# The stream.csv here drops the rss_kb column (memory has its own test).
set -u
runs=$1; shift
hz=$(getconf CLK_TCK)
ticks() { awk '{print $14+$15}' /proc/$1/stat; }
switches() { cat /proc/$1/task/*/status 2>/dev/null | awk '/ctxt_switches/{s+=$2} END{print s}'; }
echo "variant,run,cpu_pct,wakeups_per_s,rss_kb,play_after_load_s,backlog_s,underruns"
for i in $(seq "$runs"); do
  for spec in "$@"; do
    label=${spec%%=*}; rest=${spec#*=}; bin=${rest%%:*}; extra=""; [ "$rest" != "$bin" ] && extra=${rest#*:}
    st=$(mktemp -d); log=$(mktemp)
    $bin --shairport "$PWD/tests/fake_sps.py" --state-dir "$st" -i "${SPEAKER:?set SPEAKER}" -t 6 \
      --http-port 0 $extra > "$log" 2>&1 &
    pid=$!
    for _ in $(seq 150); do grep -q "speaker playing" "$log" && break; sleep 0.1; done
    sleep 2.5   # past the start-up burst
    a=$(ticks $pid); ca=$(switches $pid); sleep 4; b=$(ticks $pid); cb=$(switches $pid)
    mem=$(awk '/^VmRSS:/{print $2}' /proc/$pid/status)
    sleep 1
    pal=$(grep -m1 -oE "speaker playing [0-9.]+ s after load" "$log" | grep -oE "[0-9.]+" | head -1)
    bl=$(grep -m1 -oE "\([0-9.]+ s backlog\)" "$log" | grep -oE "[0-9.]+")
    ur=$(grep -c "underrun" "$log")
    kill -TERM $pid; wait $pid 2>/dev/null
    awk -v l="$label" -v i=$i -v a=$a -v b=$b -v hz=$hz -v c=$((cb - ca)) -v p=$mem -v pal="$pal" -v bl="$bl" -v ur=$ur \
      'BEGIN{printf "%s,%d,%.2f,%.1f,%d,%s,%s,%d\n", l, i, (b-a)*100/hz/4, c/4, p, pal, bl, ur}'
    rm -rf "$st" "$log"
  done
done
