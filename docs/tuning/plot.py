# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Srini K
"""Renders the tuning scatter plots in docs/tuning/ from the CSVs next to this script.

  docker run --rm -v "$PWD/docs/tuning:/t" -w /t <python image with matplotlib> python plot.py

Each chart is written twice, NAME.png (light) and NAME-dark.png, for GitHub's
prefers-color-scheme <picture> switch in docs/TUNING.md.
"""
import csv
import random
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

THEMES = {
    "light": dict(surface="#fcfcfb", text="#0b0b0b", text2="#52514e", muted="#898781",
                  grid="#e1e0d9", before="#2a78d6", after="#eb6834",
                  critical="#d03b3b", suffix=""),
    "dark": dict(surface="#1a1a19", text="#f0efec", text2="#c3c2b7", muted="#898781",
                 grid="#2c2c2a", before="#3987e5", after="#d95926",
                 critical="#d03b3b", suffix="-dark"),
}


def rows(name):
    with open(name) as f:
        return list(csv.DictReader(f))


def jitter(v, amount, seed):
    random.seed(seed)
    return [x + random.uniform(-amount, amount) for x in v]


def axes(t, title, subtitle, xlabel, ylabel, size=(7.2, 4.4)):
    fig, ax = plt.subplots(figsize=size, dpi=150)
    fig.patch.set_facecolor(t["surface"])
    ax.set_facecolor(t["surface"])
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(t["muted"])
        ax.spines[side].set_linewidth(0.8)
    ax.tick_params(colors=t["text2"], labelsize=8, length=3, color=t["muted"])
    ax.grid(True, color=t["grid"], linewidth=0.6)
    ax.set_axisbelow(True)
    ax.set_xlabel(xlabel, color=t["text2"], fontsize=9)
    ax.set_ylabel(ylabel, color=t["text2"], fontsize=9)
    fig.text(0.02, 0.965, title, color=t["text"], fontsize=11.5, weight="bold", va="top")
    fig.text(0.02, 0.905, subtitle, color=t["text2"], fontsize=8.5, va="top")
    fig.subplots_adjust(top=0.80, left=0.10, right=0.97, bottom=0.13)
    return fig, ax


def legend(ax, t, **kw):
    lg = ax.legend(frameon=False, fontsize=8, labelcolor=t["text2"], **kw)
    return lg


def dot(ax, t, x, y, color, label, marker="o"):
    # >= 8 px markers with a surface ring so overlapping runs stay countable
    ax.scatter(x, y, s=42, color=color, edgecolors=t["surface"], linewidths=1.2,
               marker=marker, label=label, zorder=3)


def stream_charts(t):
    data = defaultdict(list)
    for r in rows("stream.csv"):
        data[r["variant"]].append(r)

    # 1. CPU vs wake-ups while streaming
    fig, ax = axes(t, "Bridge cost while streaming to one speaker",
                   f"One dot per 4 s sample on the Pine64, {min(len(v) for v in data.values())}+ runs "
                   "per build. CPU comes in 0.25 % steps (10 ms ticks); dots are jittered.",
                   "context switches per second (all bridge threads)", "bridge CPU (% of one core)")
    groups = [("before", "before (48 kHz, client woken per write)", t["before"], "o"),
              ("after", "after (44.1 kHz, client woken per 40 ms)", t["after"], "o")]
    for key, label, color, m in groups:
        rs = data.get(key, [])
        dot(ax, t, jitter([float(r["wakeups_per_s"]) for r in rs], 2.5, key),
            jitter([float(r["cpu_pct"]) for r in rs], 0.07, key + "y"), color, label, m)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    legend(ax, t, loc="lower right")
    save(fig, t, "stream-cpu")

    # 2. start-up: time to play and underruns, per build
    order = [("before", "before"), ("after", "after"), ("no-batch", "never\nbatched"),
             ("warm-up", "batched after\n3 s warm-up"), ("warm-up-48k", "warm-up\nat 48 kHz")]
    fig, ax = axes(t, "Speaker start-up per build",
                   "One dot per load (0.1 s steps, jittered). A cross marks a load where the "
                   "speaker rebuffered just after starting.",
                   "", "speaker playing after LOAD (s)")
    ticks = []
    for i, (key, name) in enumerate(order):
        rs = data.get(key, [])
        xs = jitter([i] * len(rs), 0.22, key)
        ys = jitter([float(r["play_after_load_s"]) for r in rs], 0.025, key + "y")
        ok = [(x, y) for x, y, r in zip(xs, ys, rs) if r["underruns"] == "0"]
        bad = [(x, y) for x, y, r in zip(xs, ys, rs) if r["underruns"] != "0"]
        color = {"before": t["before"], "after": t["after"]}.get(key, t["muted"])
        if ok:
            dot(ax, t, [p[0] for p in ok], [p[1] for p in ok], color, None)
        if bad:
            ax.scatter([p[0] for p in bad], [p[1] for p in bad], s=60, marker="X",
                       color=t["critical"], edgecolors=t["surface"], linewidths=1.0, zorder=4)
        ticks.append(f"{name}\n{len(bad)} of {len(rs)} underran")
    ax.scatter([], [], s=60, marker="X", color=t["critical"], label="underrun")
    ax.set_xticks(range(len(order)), ticks)
    ax.set_xlim(-0.6, len(order) - 0.4)
    ax.set_ylim(0, max(1.0, ax.get_ylim()[1]))
    legend(ax, t, loc="upper right")
    fig.subplots_adjust(bottom=0.20)
    save(fig, t, "startup")


def idle_chart(t):
    fig, ax = axes(t, "Bridge cost with no AirPlay sessions",
                   "Container on the Pine64 (kernel 6.18), 5 receivers, no AirPlay session. One dot per 10 s sample.",
                   "context switches per second (all bridge threads)", "bridge CPU (% of one core)")
    for name, key, label, color in (("idle_before.csv", "before", "before", t["before"]),
                                    ("idle_after.csv", "after", "after", t["after"])):
        rs = rows(name)
        dot(ax, t, jitter([float(r["wakeups_per_s"]) for r in rs], 0.3, key),
            jitter([float(r["cpu_pct"]) for r in rs], 0.01, key + "y"), color, label)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    legend(ax, t, loc="upper left")
    save(fig, t, "idle")


def memory_chart(t):
    fig, ax = axes(t, "Bridge memory after playing a tone",
                   "Resident memory of the bridge after N seconds of a test tone to one speaker "
                   "(tone subcommand).",
                   "seconds of audio played", "bridge RSS (MiB)")
    for key, label, color in (("before", "before (10 s ring at 48 kHz)", t["before"]),
                              ("after", "after (4 s ring at 44.1 kHz)", t["after"])):
        rs = [r for r in rows("memory.csv") if r["variant"] == key]
        dot(ax, t, [int(r["seconds_played"]) for r in rs],
            [int(r["rss_kb"]) / 1024 for r in rs], color, label)
    ax.set_xlim(left=0)
    ax.set_ylim(bottom=0)
    legend(ax, t, loc="lower right")
    save(fig, t, "memory")


def save(fig, t, name):
    fig.savefig(f"{name}{t['suffix']}.png", facecolor=t["surface"])
    plt.close(fig)


import os  # noqa: E402

for theme in THEMES.values():
    stream_charts(theme)
    if os.path.exists("idle_after.csv"):
        idle_chart(theme)
    if os.path.exists("memory.csv"):
        memory_chart(theme)
