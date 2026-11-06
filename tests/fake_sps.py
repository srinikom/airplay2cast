#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Srini K
"""Stand-in for shairport-sync used by tests/integration.sh.

airplay2cast starts it exactly like shairport-sync ("-c <config>"). It reads the
metadata socket port and output rate from the generated config, then plays a scripted
AirPlay session: play-begin, 8 s audio, flush (pause), 4 s quiet, resume with 6 s audio,
play-end. Events are sent in shairport-sync's UDP metadata format; PCM (S16LE stereo)
goes to stdout in real time, as shairport-sync's stdout backend does.
"""
import math
import re
import socket
import struct
import sys
import time

conf = open(sys.argv[sys.argv.index("-c") + 1]).read()
port = int(re.search(r"socket_port = (\d+);", conf).group(1))
rate = int(re.search(r"output_rate = (\d+);", conf).group(1))
name = re.search(r'name = "(.*)";', conf).group(1)
meta = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


def event(kind, code, data=b""):
    meta.sendto(kind.encode() + code.encode() + data, ("127.0.0.1", port))
    print(f"fake-sps[{name}]: event {kind}/{code}", file=sys.stderr, flush=True)


def play(seconds, freq):
    out = sys.stdout.buffer
    frames = rate // 50  # 20 ms
    start = time.monotonic()
    n = 0
    while n * 0.02 < seconds:
        base = n * frames
        buf = bytearray()
        for i in range(frames):
            v = int(math.sin(2 * math.pi * freq * (base + i) / rate) * 0.05 * 32767)
            buf += struct.pack("<hh", v, v)
        out.write(buf)
        out.flush()
        n += 1
        delay = start + n * 0.02 - time.monotonic()
        if delay > 0:
            time.sleep(delay)


time.sleep(1)
event("ssnc", "abeg")
event("core", "minm", b"Integration test")
event("core", "asar", b"airplay2cast")
event("ssnc", "pbeg")
play(8, 440)
event("ssnc", "pfls")
event("ssnc", "paus")
time.sleep(4)
event("ssnc", "prsm")
play(6, 660)
event("ssnc", "pend")
event("ssnc", "aend")
print(f"fake-sps[{name}]: session finished", file=sys.stderr, flush=True)
time.sleep(3600)
