"""Benchmark of the server's Live relay: how many Live players one core can forward.

  NINJAMSRV=../ninjamsrv LIVE_BENCH=path/to/live_bench python3 bench_live.py [options]

Not a test: nothing passes or fails, and it takes minutes (each server's temporary folder is made
where TMPDIR says). For each layout it starts the
server (Live on, LiveKbps 128, LiveFrame 240, LiveMaxPeers 64 so that every player may send, the
relay's summary every second), logs the players in over TCP (and keeps their connections alive), then hands them
to `live_bench relay`, which sends what a Live player sends over UDP (a HELLO every 2 s, 200
DATA datagrams a second to the room, pings to the others) and times each copy the server
forwards. Meanwhile it reads the server's CPU per thread (/proc/<pid>/task/*/schedstat) and,
from the server's log, what the relay dropped and why.

Options (lists are comma-separated):
  --one-room 2,3,4,5,6,7,8    players, all in one room (8: what a Live room holds, the plug-in's 8 slots)
  --rooms-of 8                players a room, for the layouts with several rooms (0: none)
  --rooms-total 16,32,64,96,128  players in all, in rooms of --rooms-of (PrivateGroupMode): how many
                              rooms of 8 one core carries
  --reps 3 --seconds 20 --warm 3
  --out FILE                  CSV, a line a run (default: stdout)
  --hist-dir DIR              each run's latency histogram (live_bench relay --hist)
  --relay-args "..."          more options for live_bench relay (e.g. "--aoo-bytes 130")
  --server-cpus 3 --bench-cpus 1,2
                              the server's threads on these CPUs, live_bench's on those (default: the
                              system decides), so that the load generator doesn't take the server's core

A run is "ok" while the relay forwards everything (lost < 0.1 %, nothing dropped for being
over the rate limit or unsendable) and fast (p99 < 2 ms, p99.9 < 10 ms, loopback both ways
included).
"""
import argparse
import csv
import io
import os
import re
import select
import shlex
import shutil
import struct
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True
from njclient import Client, LIVE_MAY_SEND, CAP_LIVE, CAP_OPUS, KEEPALIVE  # noqa: E402
from test_server import Server  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
LIVE_BENCH = os.environ.get("LIVE_BENCH", "live_bench")

DROPS = ["malformed", "too big", "unknown sender", "wrong key", "wrong address", "over the rate limit",
         "unverified address (answered)", "room's Live is off", "sender may not send", "no such player",
         "could not send"]
STATS = re.compile(r"\[live\] in the last (\d+) s: (\d+) players?, (\d+) datagrams/s in, (\d+) out(.*)")


class Pump(threading.Thread):
    """Reads (and throws away) what the server sends the players that are set up, and keeps their
    connections alive: the server drops a connection it hasn't heard from in 9 s."""

    def __init__(self):
        super().__init__(daemon=True)
        self.lock = threading.Lock()
        self.socks = []
        self.last = {}
        self.quit = False
        self.closed = 0

    def add(self, c):
        c.sock.setblocking(False)
        with self.lock:
            self.socks.append(c.sock)
            self.last[c.sock] = time.time()

    def run(self):
        while not self.quit:
            with self.lock:
                socks = list(self.socks)
            if not socks:
                time.sleep(0.1)
                continue
            r, _, _ = select.select(socks, [], [], 0.2)
            for s in r:
                try:
                    if not s.recv(1 << 16):
                        self.drop(s)
                except BlockingIOError:
                    pass
                except OSError:
                    self.drop(s)
            now = time.time()
            for s in socks:
                if now - self.last.get(s, now) > 1.5:
                    try:
                        s.send(struct.pack("<BI", KEEPALIVE, 0))
                        self.last[s] = now
                    except BlockingIOError:
                        pass
                    except OSError:
                        self.drop(s)

    def drop(self, s):
        with self.lock:
            if s in self.socks:
                self.socks.remove(s)
                self.closed += 1


def task_cpu_ns(pid):
    """{tid: CPU time in ns} of each of the process's threads."""
    out = {}
    try:
        tids = os.listdir("/proc/%d/task" % pid)
    except OSError:
        return out
    for t in tids:
        try:
            with open("/proc/%d/task/%s/schedstat" % (pid, t)) as f:
                out[int(t)] = int(f.read().split()[0])
        except (OSError, ValueError, IndexError):
            pass
    return out


def udp_snmp():
    with open("/proc/net/snmp") as f:
        lines = [l.split() for l in f if l.startswith("Udp:")]
    return dict(zip(lines[0][1:], (int(x) for x in lines[1][1:])))


def udp_socket_drops(port):
    """The drops counter of the UDP socket bound to port (the server's), from /proc/net/udp."""
    want = ":%04X" % port
    with open("/proc/net/udp") as f:
        for line in f.readlines()[1:]:
            fields = line.split()
            if fields[1].endswith(want) and fields[2] == "00000000:0000":
                return int(fields[-1])
    return 0


def parse_stats(lines):
    """The relay's summary lines: datagrams in and out a second (mean) and the drops by reason (sum)."""
    ins, outs, drops = [], [], {d: 0 for d in DROPS}
    for line in lines:
        m = STATS.search(line)
        if not m:
            continue
        ins.append(int(m.group(3)))
        outs.append(int(m.group(4)))
        rest = m.group(5)
        if ", dropped " in rest and ":" in rest:
            for part in rest.split(":", 1)[1].split(","):
                part = part.strip()
                n, _, reason = part.partition(" ")
                if reason in drops and n.isdigit():
                    drops[reason] += int(n)
    mean = (lambda v: sum(v) / len(v) if v else 0.0)
    return mean(ins), mean(outs), len(ins), drops


def config(players, rooms, room_size):
    cfg = "Live on\nLiveKbps 128\nLiveFrame 240\nLiveMaxPeers 64\nLogFile $DIR/server.log\n"
    if rooms == 1:
        cfg += "MaxUsers 0\n"
    else:
        # the lobby (where everybody logs in) has no limit; each room holds room_size
        cfg += "PrivateGroupMode %d\nPrivateGroupAllowChat yes\nMaxUsers %d\n" % (rooms, room_size)
    return cfg


def log_in(server, players, rooms, room_size, pump):
    """Logs the players in (in rooms when there are several) and returns [(peer_id, key, room)]."""
    out = []
    for i in range(players):
        room = i // room_size if rooms > 1 else 0
        c = Client(server.port, timeout=10)
        ok, _ = c.login("anonymous:p%d" % i, caps=CAP_LIVE | CAP_OPUS)
        if not ok:
            raise RuntimeError("player %d could not log in" % i)
        if rooms > 1:
            c.chat("MSG", "!join r%d" % room)
        info = c.wait_live(lambda live: live["flags"] & LIVE_MAY_SEND, timeout=10)
        if info is None:
            raise RuntimeError("player %d got no Live message that lets it send" % i)
        out.append((info["peer_id"], info["key"], room))
        pump.add(c)
    return out


def pin(pid, cpus):
    """Every thread of the process on these CPUs."""
    for t in os.listdir("/proc/%d/task" % pid):
        try:
            os.sched_setaffinity(int(t), cpus)
        except OSError:
            pass


def cpu_set(text):
    return {int(c) for c in text.split(",") if c != ""}


def run(args, players, rooms, room_size, rep, writer, header_done):
    label = "%dx%d" % (rooms, room_size) if rooms > 1 else "1x%d" % players
    server = Server(config(players, rooms, room_size), env={"NINJAM_LIVE_STATS_SECONDS": "1"}).wait_ready()
    if args.server_cpus:
        pin(server.proc.pid, cpu_set(args.server_cpus))
    pump = Pump()
    pump.start()
    logpath = os.path.join(server.dir, "server.log")
    try:
        t0 = time.time()
        ids = log_in(server, players, rooms, room_size, pump)
        login_s = time.time() - t0
        csvpath = os.path.join(server.dir, "relay.csv")
        cmd = [LIVE_BENCH, "relay", "--port", str(server.port), "--seconds", str(args.seconds), "--warm",
               str(args.warm), "--label", label, "--out", csvpath] + shlex.split(args.relay_args)
        if args.bench_cpus:
            cmd = ["taskset", "-c", args.bench_cpus] + cmd
        if args.hist_dir:
            os.makedirs(args.hist_dir, exist_ok=True)
            cmd += ["--hist", os.path.join(args.hist_dir, "relay-%s-rep%d.csv" % (label, rep))]
        bench = subprocess.Popen(cmd, stdin=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        bench.stdin.write("".join("%d %d %d\n" % x for x in ids) + "end\n")
        bench.stdin.flush()
        marks = {}
        said = []
        for line in bench.stderr:
            said.append(line.rstrip())
            if line.startswith("measure start") or line.startswith("measure end"):
                key = line.split()[1]
                marks[key] = (time.time(), task_cpu_ns(server.proc.pid), udp_snmp(),
                              udp_socket_drops(server.port), os.path.getsize(logpath))
        bench.wait()
        if "start" not in marks or "end" not in marks or bench.returncode:
            print("live_bench relay failed: %s" % " / ".join(said[-5:]), file=sys.stderr)
            return header_done
        (ta, ca, sa, da, la), (tb, cb, sb, db, lb) = marks["start"], marks["end"]
        wall = tb - ta
        delta = {t: cb.get(t, 0) - ca.get(t, 0) for t in cb}
        pid = server.proc.pid
        main = delta.get(pid, 0)
        others = sorted(((v, t) for t, v in delta.items() if t != pid), reverse=True)
        relay = others[0][0] if others else 0
        with open(logpath, errors="replace") as f:
            f.seek(la)
            window = f.read(max(0, lb - la)).splitlines()
        srv_in, srv_out, nstats, drops = parse_stats(window)
        with open(csvpath) as f:
            rows = list(csv.DictReader(f))
        r = rows[-1]
        lost = float(r["lost_pct"])
        p99, p999 = float(r["lat_p99_us"]), float(r["lat_p999_us"])
        bad = (lost >= 0.1 or drops["over the rate limit"] or drops["could not send"] or p99 >= 2000 or
               p999 >= 10000 or pump.closed)
        row = {"layout": "one room" if rooms == 1 else "rooms of %d" % room_size, "rooms": rooms,
               "room_size": room_size, "rep": rep, "login_s": "%.1f" % login_s}
        row.update(r)
        row.update({
            "server_cpu_pct": "%.1f" % (100.0 * sum(delta.values()) / 1e9 / wall),
            "relay_thread_cpu_pct": "%.1f" % (100.0 * relay / 1e9 / wall),
            "main_thread_cpu_pct": "%.1f" % (100.0 * main / 1e9 / wall),
            "server_threads": len(delta),
            "srv_in_dgram_s": "%.0f" % srv_in, "srv_out_dgram_s": "%.0f" % srv_out, "stats_lines": nstats,
            "drops_total": sum(drops.values()),
        })
        for d in DROPS:
            row["drop_" + d.replace(" ", "_").replace("'", "").replace("(", "").replace(")", "")] = drops[d]
        row.update({
            "udp_rcvbuf_errors": sb["RcvbufErrors"] - sa["RcvbufErrors"],
            "udp_sndbuf_errors": sb["SndbufErrors"] - sa["SndbufErrors"],
            "server_socket_drops": db - da,
            "tcp_closed": pump.closed,
            "server_cpus": args.server_cpus or "any", "bench_cpus": args.bench_cpus or "any",
            "verdict": "bad" if bad else "ok",
        })
        if not header_done:
            writer.writerow(list(row.keys()))
        writer.writerow(list(row.values()))
        print("%s rep %d: %s players, server %s%% CPU (relay thread %s%%), p50 %s us, p99 %s us, p99.9 %s us, "
              "lost %s%%, dropped %d, %s" % (label, rep, r["players"], row["server_cpu_pct"],
                                             row["relay_thread_cpu_pct"], r["lat_p50_us"], r["lat_p99_us"],
                                             r["lat_p999_us"], r["lost_pct"], row["drops_total"], row["verdict"]),
              file=sys.stderr)
        return True
    finally:
        pump.quit = True
        pump.join(2)
        server.stop()
        server.log.close()
        shutil.rmtree(server.dir, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--one-room", default="2,3,4,5,6,7,8")
    ap.add_argument("--rooms-of", type=int, default=8)
    ap.add_argument("--rooms-total", default="16,32,64,96,128")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--warm", type=float, default=3)
    ap.add_argument("--out", default="-")
    ap.add_argument("--hist-dir", default="")
    ap.add_argument("--relay-args", default="")
    ap.add_argument("--server-cpus", default="")
    ap.add_argument("--bench-cpus", default="")
    args = ap.parse_args()
    layouts = [(int(n), 1, int(n)) for n in args.one_room.split(",") if n]
    if args.rooms_of > 0:
        layouts += [(int(n), int(n) // args.rooms_of, args.rooms_of) for n in args.rooms_total.split(",") if n]
    f = sys.stdout if args.out == "-" else open(args.out, "a", newline="")
    header_done = args.out != "-" and os.path.getsize(args.out) > 0
    writer = csv.writer(f)
    for players, rooms, size in layouts:
        for rep in range(1, args.reps + 1):
            header_done = run(args, players, rooms, size, rep, writer, header_done)
            f.flush()
    if f is not sys.stdout:
        f.close()


if __name__ == "__main__":
    main()
