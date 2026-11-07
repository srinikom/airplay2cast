#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Srini K
# idle.sh LABEL: 10 x 10 s samples of the deployed container's bridge (CPU %, context
# switches/s), on the test host while no AirPlay session is active.
label=$1; hz=$(getconf CLK_TCK)
sleep 20  # let start-up settle
pid=$(docker top airplay2cast -eo pid,comm | awk '$2=="airplay2cast"{print $1}')
sw() { cat /proc/$pid/task/*/status | awk '/ctxt_switches/{s+=$2} END{print s}'; }
for i in $(seq 10); do
  a=$(awk '{print $14+$15}' /proc/$pid/stat); ca=$(sw); sleep 10
  b=$(awk '{print $14+$15}' /proc/$pid/stat); cb=$(sw)
  awk -v l=$label -v i=$i -v a=$a -v b=$b -v hz=$hz -v c=$((cb - ca)) \
    'BEGIN{printf "%s,%d,%.2f,%.1f\n", l, i, (b-a)*100/hz/10, c/10}'
done
