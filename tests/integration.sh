#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Srini K
# Bridge integration test against a real Cast device, without an AirPlay sender:
# tests/fake_sps.py replaces shairport-sync and scripts play / flush / resume / end.
#   tests/integration.sh "Hallway speaker" [path/to/airplay2cast]
# Expects: launch once, load, stop on flush, reload (no relaunch) on resume, release on idle.
set -u
dev=${1:?Cast device name}
bin=${2:-./airplay2cast}
here=$(cd "$(dirname "$0")" && pwd)
log=$(mktemp)
state=$(mktemp -d)

"$bin" --shairport "$here/fake_sps.py" --state-dir "$state" -i "$dev" -t 6 \
  --http-port 0 >"$log" 2>&1 &
pid=$!
sleep 40
kill -TERM "$pid"
wait "$pid"
rc=$?

fail=0
expect() { # count pattern description
  n=$(grep -c -- "$2" "$log")
  if [ "$n" -eq "$1" ]; then echo "ok   $3 ($n)"; else echo "FAIL $3: expected $1, got $n"; fail=1; fi
}
expect 1 "launched CC1AD845" "receiver app launched once"
expect 2 ": loaded http" "media loaded for the initial play and after resume"
expect 1 "AirPlay flush" "flush event received"
expect 1 "releasing speaker (idle)" "speaker released after idle timeout"
expect 1 "lowering speaker volume" "volume lowered for the launch chime"
expect 1 "restoring speaker volume" "volume restored after launch"
expect 1 "preparing speaker for the AirPlay session" "receiver prepared at play begin"
expect 2 "speaker playing [0-9.]* s after load" "speaker reported playing after each load"
expect 0 "speaker not playing\\|underrun" "no slow start or underrun"
grep -q "streaming /stream/" "$log" && echo "ok   speaker fetched the HTTP stream" ||
  { echo "FAIL speaker never fetched the stream"; fail=1; }
[ "$rc" -eq 0 ] && echo "ok   clean shutdown" || { echo "FAIL exit status $rc"; fail=1; }
if [ "$fail" -ne 0 ]; then
  echo "--- log"; cat "$log"
fi
rm -rf "$log" "$state"
exit "$fail"
