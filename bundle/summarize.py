#!/usr/bin/env python3
"""Medians over run-bench.sh's repeated runs, one row per clip; the MMR modes
and real-time playback beside them.

    summarize.py RESULTS_DIR

Reads TAG-runN.json (+ .log for throttling), TAG-mode-M.json, TAG-play.json.
A run whose after-snapshot shows current throttling (vcgencmd get_throttled
bits 0-3: under-voltage, ARM capped, throttled, soft temperature limit) is
marked; throttled runs are left out of the medians when unthrottled ones exist.
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


def load(path):
    try:
        return json.load(open(path))
    except (OSError, ValueError):
        return None


def main(out):
    runs = {}
    for path in sorted(glob.glob(os.path.join(out, "*-run*.json"))):
        m = re.match(r"(.+)-run\d+\.json$", os.path.basename(path))
        data = load(path)
        if m and data:
            runs.setdefault(m.group(1), []).append((throttled(path[:-5] + ".log"), data))
    if not runs:
        print("no performance runs found")
        return
    print(f"\n{'clip':30} {'runs':>4} {'decode fps':>10} {'median ms':>10} {'p95 ms':>7} {'fps':>6} "
          f"{'thru fps':>9}  {'exact / split / float ms':>24}  {'play: late of shown':>20}  note")
    for tag, rs in sorted(runs.items()):
        cool = [d for hot, d in rs if not hot]
        use = cool or [d for _, d in rs]
        med = lambda k: statistics.median(d.get(k, 0) for d in use)
        median = med("median_ms")
        modes = []
        for m in ("exact", "split", "float"):
            d = load(os.path.join(out, f"{tag}-mode-{m}.json"))
            modes.append(f"{d['median_ms']:.1f}" if d and d.get("median_ms") else "-")
        p = load(os.path.join(out, f"{tag}-play.json"))
        play = f"{p['play_late']} of {p['play_shown']}" if p and "play_shown" in p else "-"
        hot = sum(1 for h, _ in rs if h)
        note = f"{hot} throttled run(s){' excluded' if cool else ', ALL throttled'}" if hot else ""
        if any(d.get("failed") for _, d in rs):
            note += " failures"
        print(f"{tag[:30]:30} {len(use):>4} {med('decode_fps'):>10.1f} {median:>10.2f} {med('p95_ms'):>7.2f} "
              f"{1000 / median if median else 0:>6.1f} {med('throughput_fps'):>9.1f}  {' / '.join(modes):>24}  "
              f"{play:>20}  {note}")
    print("\nfps = 1000 / median per-picture time (plan + render + GPU finish, one at a time); thru fps = three "
          "frames in flight; decode fps = the hardware decoder alone (BL + EL).")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")
