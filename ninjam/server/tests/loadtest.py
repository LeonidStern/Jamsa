"""Simulates a jam and reports the server's CPU use.

  python3 loadtest.py <ninjamsrv> [users] [channels] [kbps] [seconds]

Each user uploads every channel continuously (1 KB writes at the given bitrate,
a new interval every 4 s) and subscribes to everyone else, like a real room.
"""
import asyncio
import hashlib
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid

srv, users, chans, kbps, seconds = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
INTERVAL = 4.0
CHUNK = 1024


def cpu_seconds(pid):
    with open("/proc/%d/stat" % pid) as f:
        fields = f.read().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


async def msg(w, t, p=b""):
    w.write(struct.pack("<BI", t, len(p)) + p)


async def read_msg(r):
    h = await r.readexactly(5)
    t, n = struct.unpack("<BI", h)
    return t, await r.readexactly(n)


async def user(port, idx, names, start, stop, received):
    r, w = await asyncio.open_connection("127.0.0.1", port)
    t, p = await read_msg(r)
    name = "anonymous:u%d" % idx
    ph = hashlib.sha1(hashlib.sha1((name + ":").encode()).digest() + p[:8]).digest()
    await msg(w, 0x80, ph + name.encode() + b"\0" + struct.pack("<II", 1, 0x00020000))
    while True:
        t, p = await read_msg(r)
        if t == 0x01:
            names[idx] = p[1:].split(b"\0")[0].decode()
            break
    info = struct.pack("<H", 4) + b"".join(b"ch%d\0" % c + struct.pack("<hbB", 0, 0, 0) for c in range(chans))
    await msg(w, 0x82, info)

    async def reader():
        try:
            while True:
                t, p = await read_msg(r)
                if t == 0x05:
                    received[0] += len(p)
        except (asyncio.IncompleteReadError, ConnectionError):
            pass

    rt = asyncio.ensure_future(reader())
    await start.wait()
    for other in names:
        if other and other != names[idx]:
            await msg(w, 0x81, other.encode() + b"\0" + struct.pack("<I", 0xFFFFFFFF))
    per_chunk = CHUNK * 8 / (kbps * 1000.0)
    t0 = time.time()
    while not stop.is_set():
        guids = [uuid.uuid4().bytes for _ in range(chans)]
        for c in range(chans):
            await msg(w, 0x83, guids[c] + struct.pack("<I", 0) + b"OGGv" + bytes([c]))
        ivstart = time.time()
        n = 0
        while time.time() - ivstart < INTERVAL and not stop.is_set():
            for c in range(chans):
                await msg(w, 0x84, guids[c] + b"\0" + os.urandom(CHUNK))
            n += 1
            await w.drain()
            await asyncio.sleep(max(0, ivstart + n * per_chunk - time.time()))
        for c in range(chans):
            await msg(w, 0x84, guids[c] + b"\1")
        await msg(w, 0xFD)
    rt.cancel()
    w.close()


async def main():
    d = tempfile.mkdtemp()
    s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
    cfg = os.path.join(d, "c.cfg")
    # all the simulated users come from one address, so turn off the per-address login limit
    open(cfg, "w").write("Port %d\nAnonymousUsers yes\nMaxChannels 32 32\nMaxPendingConnections 0 0\n" % port)
    proc = subprocess.Popen([srv, cfg], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    await asyncio.sleep(0.5)
    idle0 = cpu_seconds(proc.pid); await asyncio.sleep(3); idle = (cpu_seconds(proc.pid) - idle0) / 3

    names = [None] * users
    start, stop = asyncio.Event(), asyncio.Event()
    received = [0]
    tasks = [asyncio.ensure_future(user(port, i, names, start, stop, received)) for i in range(users)]
    while None in names:
        await asyncio.sleep(0.05)
    start.set()
    await asyncio.sleep(2)
    c0, r0, t0 = cpu_seconds(proc.pid), received[0], time.time()
    await asyncio.sleep(seconds)
    c1, r1, t1 = cpu_seconds(proc.pid), received[0], time.time()
    stop.set()
    await asyncio.gather(*tasks, return_exceptions=True)
    proc.terminate(); proc.wait()
    print("%s users=%d ch=%d kbps=%d  idle_cpu=%.1f%%  load_cpu=%.1f%%  relayed=%.2f MB/s"
          % (os.path.basename(srv), users, chans, kbps, idle * 100, (c1 - c0) / (t1 - t0) * 100, (r1 - r0) / (t1 - t0) / 1e6))

asyncio.run(main())
