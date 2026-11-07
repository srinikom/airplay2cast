#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Srini K
# Bridge RSS after N seconds of a test tone to $SPEAKER, per build:
#   SPEAKER="Office speaker" collect_mem.sh LABEL=BINARY ...
# variant,seconds_played,rss_kb
set -u
echo "variant,seconds_played,rss_kb"
for n in 2 4 6 8 10 12 14; do
  for spec in "$@"; do
    label=${spec%%=*}; bin=${spec#*=}; log=$(mktemp)
    $bin tone "${SPEAKER:?set SPEAKER}" --seconds $((n + 2)) --http-port 0 > "$log" 2>&1 &
    pid=$!
    for _ in $(seq 200); do grep -q "streaming" "$log" && break; sleep 0.05; done
    sleep $n
    awk -v l=$label -v n=$n '/^VmRSS:/{printf "%s,%d,%d\n", l, n, $2}' /proc/$pid/status
    wait $pid 2>/dev/null; rm -f "$log"; sleep 1
  done
done
