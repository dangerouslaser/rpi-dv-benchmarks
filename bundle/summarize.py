#!/usr/bin/env python3
"""Medians over run-bench.sh's repeated runs: one row per clip and engine.

    summarize.py RESULTS_DIR

Reads TAG-hw-ENGINE-repN.json (+ .log for throttling) and TAG-sw.json.
A run whose after-snapshot shows current throttling (vcgencmd get_throttled
bits 0-3: under-voltage, ARM capped, throttled, soft temperature limit) is
marked; throttled runs are excluded from the medians when unthrottled ones exist.
"""
import glob
import json
import os
import re
import statistics
import sys


def throttled(log):
    try:
        text = open(log).read()
    except OSError:
        return False
    m = re.findall(r"== after: .*throttled=(0x[0-9a-fA-F]+)", text)
    return bool(m) and int(m[-1], 16) & 0xF != 0


def main(out):
    rows = {}
    for path in sorted(glob.glob(os.path.join(out, "*-hw-*-rep*.json"))):
        m = re.match(r"(.+)-hw-(cb1|vk)-rep(\d+)\.json$", os.path.basename(path))
        if not m:
            continue
        tag, engine, _ = m.groups()
        try:
            data = json.load(open(path))
        except (OSError, ValueError):
            continue
        name = "cb1" if engine == "cb1" else "yblod-vulkan"
        e = data.get("engines", {}).get(name)
        if not e:
            continue
        hot = throttled(path[:-5] + ".log")
        rows.setdefault((tag, name), []).append((hot, data, e))

    if not rows:
        print("no performance runs found")
        return
    print(f"{'clip':34} {'engine':13} {'runs':>4} {'decode fps':>10} {'median ms':>10} {'p95 ms':>8} "
          f"{'render fps':>10} {'thru fps':>9} {'CPU ms/f':>9}  note")
    for (tag, name), runs in sorted(rows.items()):
        cool = [r for r in runs if not r[0]]
        use = cool or runs
        med = lambda f: statistics.median(f(r) for r in use)
        decode = med(lambda r: r[1].get("decode_fps", 0))
        median = med(lambda r: r[2].get("median_ms", 0))
        p95 = med(lambda r: r[2].get("p95_ms", 0))
        thru = med(lambda r: r[2].get("throughput_fps", 0))
        cpu = med(lambda r: r[2].get("throughput_cpu_ms", 0))
        notes = []
        hot = sum(1 for r in runs if r[0])
        if hot:
            notes.append(f"{hot} throttled run(s){' excluded' if cool else ', ALL throttled'}")
        if any(r[2].get("failed") for r in runs):
            notes.append("failures")
        fps = 1000 / median if median else 0
        print(f"{tag[:34]:34} {name:13} {len(use):>4} {decode:>10.1f} {median:>10.2f} {p95:>8.2f} "
              f"{fps:>10.1f} {thru:>9.1f} {cpu:>9.2f}  {'; '.join(notes)}")
    print("\nrender fps = 1000 / median per-picture time (import + render + GPU finish, one at a time);")
    print("thru fps = flat-out pipelined throughput; decode fps = hardware decode of BL (+EL) alone.")

    plays = []
    for path in sorted(glob.glob(os.path.join(out, "*-play-*.json"))):
        m = re.match(r"(.+)-play-(cb1|vk)\.json$", os.path.basename(path))
        try:
            data = json.load(open(path))
        except (OSError, ValueError):
            continue
        name = "cb1" if m.group(2) == "cb1" else "yblod-vulkan"
        e = data.get("engines", {}).get(name, {})
        if "play_shown" in e:
            plays.append((m.group(1), name, e, throttled(path[:-5] + ".log")))
    if plays:
        print(f"\nreal-time playback (decode thread + stream frame clock)")
        print(f"{'clip':34} {'engine':13} {'shown':>6} {'late':>5} {'late %':>7} {'worst ms':>9} "
              f"{'decode stalls':>13} {'CPU ms/f':>9}  note")
        for tag, name, e, hot in plays:
            shown = e["play_shown"]
            pct = 100.0 * e["play_late"] / shown if shown else 0
            print(f"{tag[:34]:34} {name:13} {shown:>6} {e['play_late']:>5} {pct:>6.1f}% {e['play_worst_late_ms']:>9.1f} "
                  f"{e['play_decode_stalls']:>13} {e['play_cpu_ms']:>9.2f}  {'throttled' if hot else ''}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
