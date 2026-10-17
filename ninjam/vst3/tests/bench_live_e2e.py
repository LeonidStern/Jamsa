"""Benchmark of Live's delay through the plug-in: two instances of Jamsa.vst3 on this machine.

  NINJAM_VST3_PLUGIN=build/VST3/Release/Jamsa.vst3 \\
  NINJAM_VST3_E2E_HOST=build/bin/Release/plugin_e2e_host \\
  python3 bench_live_e2e.py [--blocks 64,128,256,512] [--rate 48000] [--buffers 5,10,20,30,40]
                            [--also 44100:256:20] [--seconds 30] [--reps 3] [--out bench_live_e2e.csv]

Not a test: nothing passes or fails, and it takes about half an hour with the defaults. For each
host block (and each --also RATE:BLOCK:BUFFERS, buffers separated by "/"), it runs plugin_e2e_host's
livebench mode --reps times (a new process each time): alice hosts the server (the plug-in's own)
and plays chirps, bob joins, alice turns Live on, and for each jitter buffer bob listens for
--seconds. One CSV line per buffer and run: the delay bob heard (median, 95th percentile; from
alice's input to bob's output, in the hosts' samples: the DAWs' audio buffers and the audio
interfaces aren't in it, nor the internet), his estimate of it, his dropouts a minute, how much
of the time alice's stream was healthy and he heard her live, and the buffer the plug-in uses
(`inuse`: it raises what was asked for to fit the DAW's block) for the block it sees. Then a
summary per setting: medians over the runs, and their spread.
"""
import argparse
import csv
import os
import re
import statistics
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_plugin_e2e import HOST, PLUGIN, Run, fields  # noqa: E402

COLUMNS = ["rate", "block", "run", "buffer", "inuse", "hostblock", "settled", "settle_s", "seconds", "chirps",
           "live_chirps", "found", "median_ms", "p95_ms", "min_ms", "max_ms", "estimate_ms", "estimate_min_ms",
           "estimate_max_ms", "dropouts", "underruns", "overruns", "dropped", "resent", "dropouts_per_min",
           "healthy_pct", "live_pct", "late_blocks", "late_ms", "process_max_ms"]


def one_run(rate, block, buffers, seconds):
    """One plugin_e2e_host livebench process: its BENCH lines, as dicts."""
    run = Run(["livebench", ",".join(str(b) for b in buffers), str(seconds), "1"], block=block, rate=rate)
    rows = []

    def on_line(line):
        m = re.match(r"BENCH (buffer=.*)$", line)
        if m:
            rows.append(fields(m.group(1)))
    try:
        run.run_to_end(on_line, timeout=120 + len(buffers) * (seconds + 60))
    finally:
        run.close()
    if run.proc.returncode != 0 or "DONE" not in run.output:
        print("plugin_e2e_host failed (%s):\n%s" % (run.proc.returncode, "\n".join(run.output[-20:])), file=sys.stderr)
    return rows


def spread(values):
    v = [x for x in values if x is not None]
    if not v:
        return "-"
    return "%.1f (%.1f..%.1f)" % (statistics.median(v), min(v), max(v))


def num(row, key):
    try:
        x = float(row[key])
        return None if x < 0 else x
    except (KeyError, ValueError):
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--blocks", default="64,128,256,512")
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--buffers", default="5,10,20,30,40")
    ap.add_argument("--also", default="44100:256:20", help="more settings, RATE:BLOCK:BUFFERS (buffers split by /), "
                                                           "comma-separated; empty for none")
    ap.add_argument("--seconds", type=float, default=30)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--out", default="bench_live_e2e.csv", help="CSV (appended to; the hosts' lines go to stdout)")
    args = ap.parse_args()
    if not (HOST and PLUGIN):
        sys.exit("set NINJAM_VST3_PLUGIN and NINJAM_VST3_E2E_HOST")

    settings = [(args.rate, int(b), [int(x) for x in args.buffers.split(",")]) for b in args.blocks.split(",") if b]
    for s in args.also.split(","):
        if s:
            r, b, bufs = s.split(":")
            settings.append((int(r), int(b), [int(x) for x in bufs.split("/")]))

    f = open(args.out, "a", newline="")
    w = csv.writer(f)
    if os.path.getsize(args.out) == 0:
        w.writerow(COLUMNS)
    summary = {}
    for rate, block, buffers in settings:
        for rep in range(1, args.reps + 1):
            print("bench_live_e2e: %d Hz, %d-sample blocks, run %d of %d" % (rate, block, rep, args.reps), file=sys.stderr)
            for row in one_run(rate, block, buffers, args.seconds):
                row.update({"rate": rate, "block": block, "run": rep})
                w.writerow([row.get(c, "") for c in COLUMNS])
                f.flush()
                summary.setdefault((rate, block, int(row["buffer"])), []).append(row)
    f.close()

    print("\nrate  block  buffer  inuse  heard median ms     p95 ms              estimate ms         "
          "dropouts/min  healthy %  live %  found", file=sys.stderr)
    for (rate, block, buffer), rows in sorted(summary.items()):
        n = sum(int(r.get("found", 0)) for r in rows)
        m = sum(int(r.get("live_chirps", 0)) for r in rows)
        print("%-5d %-6d %-7d %-6s %-19s %-19s %-19s %-13s %-10s %-7s %d/%d" % (
            rate, block, buffer, "/".join(sorted(set(r.get("inuse", "-") for r in rows))),
            spread([num(r, "median_ms") for r in rows]), spread([num(r, "p95_ms") for r in rows]),
            spread([num(r, "estimate_ms") for r in rows]), spread([num(r, "dropouts_per_min") for r in rows]),
            spread([num(r, "healthy_pct") for r in rows]), spread([num(r, "live_pct") for r in rows]), n, m),
            file=sys.stderr)


if __name__ == "__main__":
    main()
