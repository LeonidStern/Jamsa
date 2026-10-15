"""End-to-end tests of the VST3 plug-in, run by ctest (see CMakeLists.txt).

test_codecs_and_bitrates: plugin_e2e_host loads NINJAM.vst3 into a small VST3 host. The plug-in
starts its built-in server and joins it; a second NJClient joins too. Each plays a tone, and the
host measures how well each side hears the other while the room's codec goes from Opus (the
default) to FLAC and back, switched by typing !codec into the plug-in's chat, and while the
plug-in's Opus bitrate goes from 128 to 256 and 510 kbps, last with 16 second intervals. This
script also joins the server at the protocol level: it checks that a client that only plays
Vorbis is refused, and it watches the codec and size of every interval the plug-in uploads.

test_two_instances: two instances of the plug-in in one process, as two tracks in a DAW. One
hosts the server; the other can't host a second one in the same process, is told to join it,
joins, and they hear each other and chat. Then the first stops its server, the second hosts
one, and the first joins that.

test_vorbis_room_plays_silence: the plug-in joins a server (NINJAMSRV, which may be one from
before Opus) where clients that only know Vorbis play (ninjam/tests/njclient_e2e_vorbis). The
plug-in can't send there, says so in its chat, and plays their Vorbis as silence.

Run by hand:
  NINJAM_VST3_PLUGIN=build/VST3/Release/NINJAM.vst3 \\
  NINJAM_VST3_E2E_HOST=build/bin/Release/plugin_e2e_host \\
  NINJAMSRV=../../server/ninjamsrv NJCLIENT_E2E_VORBIS=../../tests/njclient_e2e_vorbis \\
  python3 -m unittest -v test_plugin_e2e
"""
import os
import queue
import re
import subprocess
import sys
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "server", "tests"))

from njclient import Client, CAP_OPUS, CAP_FLAC, DOWNLOAD_BEGIN, DOWNLOAD_WRITE  # noqa: E402
import test_server  # noqa: E402

PLUGIN = os.environ.get("NINJAM_VST3_PLUGIN", "")
HOST = os.environ.get("NINJAM_VST3_E2E_HOST", "")
VORBIS_CLIENT = os.environ.get("NJCLIENT_E2E_VORBIS", "")

# Opus hears a pure tone well (better at higher bitrates); FLAC is near the 16-bit limit
MIN_SNR = {"opus": 25.0, "opus_again": 25.0, "opus256": 30.0, "opus510": 30.0, "opus510_long": 30.0, "flac": 60.0}
FOURCC = {"opus": b"OPUS", "opus_again": b"OPUS", "opus256": b"OPUS", "opus510": b"OPUS", "opus510_long": b"OPUS",
          "flac": b"FLAC"}
# Opus bitrate (kbps) and interval length (seconds) of each Opus phase, for the interval sizes
OPUS_SIZES = {"opus": (128, 1.0), "opus_again": (128, 1.0), "opus256": (256, 1.0), "opus510": (510, 1.0),
              "opus510_long": (510, 16.0)}


class Run:
    """A plugin_e2e_host process, its output lines and a watcher of what the server relays."""

    def __init__(self, args):
        self.proc = subprocess.Popen([HOST, PLUGIN, "48000", "512"] + args, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        self.lines = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.output = []
        self.watcher = None
        self.watching = False
        self.phase = None
        self.intervals = {}  # guid -> {"user", "phase", "fourcc", "bytes", "done"}

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put(None)

    def log(self):
        return "\n".join(self.output)

    def next_line(self, timeout):
        line = self.lines.get(timeout=timeout)
        if line is not None:
            self.output.append(line)
            print(line, flush=True)
        return line

    def wait_for_line(self, pattern, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self.next_line(deadline - time.time())
            except queue.Empty:
                break
            if line is None:
                break
            m = re.match(pattern, line)
            if m:
                return m
        return None

    def watch(self, port, *users):
        self.watcher = Client(port)
        ok, why = self.watcher.login("anonymous:watch", codecs=CAP_OPUS | CAP_FLAC)
        assert ok, why
        for u in users:
            self.watcher.subscribe(u)
        self.watching = True

    def go(self):
        self.proc.stdin.write("go\n")
        self.proc.stdin.flush()

    def poll_watcher(self):
        for _ in range(200 if self.watching else 0):  # what arrived, without waiting for more
            try:
                msg = self.watcher.recv(0.01)
            except (ConnectionError, OSError):
                self.watching = False  # the server stopped
                break
            if msg is None:
                break
            kind, p = msg
            if kind == DOWNLOAD_BEGIN and p[:16] != bytes(16):
                self.intervals[p[:16]] = {"user": p[25:].split(b"\0")[0].decode(), "phase": self.phase,
                                          "fourcc": p[20:24], "bytes": 0, "done": False}
            elif kind == DOWNLOAD_WRITE and p[:16] in self.intervals:
                iv = self.intervals[p[:16]]
                iv["bytes"] += len(p) - 17
                if p[16] & 1:
                    iv["done"] = True

    def run_to_end(self, on_line=None, timeout=300):
        """Reads lines and watches the server until the host exits."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                line = self.next_line(0.05)
            except queue.Empty:
                line = ""
            if line is None:
                break
            m = re.match(r"PHASE (\w+) BEGIN$", line)
            if m:
                self.phase = m.group(1)
            if re.match(r"PHASE (\w+) END ", line):
                self.phase = None
            if on_line:
                on_line(line)
            self.poll_watcher()
        self.proc.wait(timeout=30)

    def close(self):
        if self.watcher:
            self.watcher.close()
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


def fields(line):
    return dict(kv.split("=", 1) for kv in line.split() if "=" in kv)


@unittest.skipUnless(PLUGIN and HOST, "set NINJAM_VST3_PLUGIN and NINJAM_VST3_E2E_HOST")
class PluginEndToEndTest(unittest.TestCase):
    def test_codecs_and_bitrates(self):
        run = Run([])
        results = {}  # phase -> fields of its END line
        try:
            m = run.wait_for_line(r"PORT (\d+)$", 30)
            self.assertIsNotNone(m, run.log())
            port = int(m.group(1))

            # the built-in server requires Opus: a client that only plays Vorbis is refused
            old = Client(port)
            try:
                self.assertEqual(old.login("anonymous:old", codecs=0),
                                 (False, "this server needs a NINJAM client that plays Opus"))
            finally:
                old.close()

            # a watcher that can play every codec the room may use
            run.watch(port, "plugin@127.0.0.1")
            run.go()

            def on_line(line):
                m = re.match(r"PHASE (\w+) END (.*)$", line)
                if m:
                    results[m.group(1)] = fields(m.group(2))
            run.run_to_end(on_line)
        finally:
            run.close()

        log = run.log()
        self.assertEqual(run.proc.returncode, 0, log)
        self.assertIn("DONE", run.output, log)
        sizes = {}
        for phase in FOURCC:
            self.assertIn(phase, results, log)
            r = results[phase]
            self.assertEqual(r["upload"].encode(), FOURCC[phase], log)
            # what reached the server during the phase, as the watcher saw it
            began = [iv for iv in run.intervals.values() if iv["phase"] == phase]
            self.assertEqual({iv["fourcc"] for iv in began}, {FOURCC[phase]}, log)
            self.assertGreater(float(r["plugin_hears_snr"]), MIN_SNR[phase], "the plug-in hears the peer\n" + log)
            self.assertGreater(float(r["peer_hears_snr"]), MIN_SNR[phase], "the peer hears the plug-in\n" + log)
            sizes[phase] = [iv["bytes"] for iv in began if iv["done"]]
            self.assertTrue(sizes[phase], "no whole interval in %s\n%s" % (phase, log))

        # each Opus interval is about as big as its bitrate says (Ogg adds a little)
        print("interval sizes (bytes):", flush=True)
        for phase, (kbps, seconds) in OPUS_SIZES.items():
            expected = kbps * 1000 / 8 * seconds
            print("  %-13s %4d kbps %5.1f s: %s (expected about %d)" % (phase, kbps, seconds, sizes[phase], expected),
                  flush=True)
            for size in sizes[phase]:
                self.assertGreater(size, expected * 0.75, "%s: %s\n%s" % (phase, sizes[phase], log))
                self.assertLess(size, expected * 1.25, "%s: %s\n%s" % (phase, sizes[phase], log))

        def mean(phase):
            return sum(sizes[phase]) / len(sizes[phase])
        self.assertAlmostEqual(mean("opus256") / mean("opus"), 2.0, delta=0.3)
        self.assertAlmostEqual(mean("opus510") / mean("opus"), 510 / 128, delta=0.6)

    def test_two_instances(self):
        run = Run(["two"])
        results = {}
        try:
            def on_line(line):
                m = re.match(r"PHASE (\w+) END (.*)$", line)
                if m:
                    results[m.group(1)] = fields(m.group(2))
            run.run_to_end(on_line, timeout=120)
        finally:
            run.close()

        log = run.log()
        self.assertEqual(run.proc.returncode, 0, log)
        self.assertIn("DONE", run.output, log)
        two, swap = results.get("two", {}), results.get("swap", {})
        self.assertEqual(two.get("b_host_refused"), "1", "told to join the server already in this process\n" + log)
        self.assertGreater(float(two["a_hears_snr"]), MIN_SNR["opus"], log)
        self.assertGreater(float(two["b_hears_snr"]), MIN_SNR["opus"], log)
        self.assertEqual(two.get("b_got_chat"), "1", log)
        self.assertEqual((two.get("a_users"), two.get("b_users")), ("1", "1"), log)
        self.assertEqual(swap.get("b_dropped"), "1", "the joined instance leaves when the server stops\n" + log)
        self.assertEqual(swap.get("b_hosting"), "1", "the other instance can host once the first stopped\n" + log)
        self.assertEqual((swap.get("a_state"), swap.get("a_users")), ("connected", "1"), log)

    @unittest.skipUnless(VORBIS_CLIENT and os.path.exists(test_server.SERVER),
                         "set NINJAMSRV and NJCLIENT_E2E_VORBIS (make -C ninjam/server, make -C ninjam/tests)")
    def test_vorbis_room_plays_silence(self):
        server = test_server.Server("DefaultBPM 240\nDefaultBPI 4\n").wait_ready()
        run = Run(["join", "127.0.0.1:%d" % server.port])
        vorbis = None
        vorbis_out = ""
        result = {}
        try:
            self.assertIsNotNone(run.wait_for_line(r"JOINED$", 30), run.log())
            run.watch(server.port, "plugin@127.0.0.1", "sender@127.0.0.1")

            # two clients that only know Vorbis: "sender" plays a tone, "listener" records it
            vorbis = subprocess.Popen([VORBIS_CLIENT, "127.0.0.1:%d" % server.port, "48", "48000"],
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            run.go()

            def on_line(line):
                m = re.match(r"PHASE vorbis END (.*)$", line)
                if m:
                    result.update(fields(m.group(1)))
            run.run_to_end(on_line, timeout=120)
            vorbis_out = vorbis.communicate(timeout=60)[0]
            print("njclient_e2e_vorbis:", vorbis_out, flush=True)
        finally:
            run.close()
            if vorbis and vorbis.poll() is None:
                vorbis.kill()
            server.stop()
            server.log.close()

        log = run.log()
        self.assertEqual(run.proc.returncode, 0, log)
        self.assertIn("DONE", run.output, log)
        self.assertEqual(vorbis.returncode, 0, vorbis_out)
        # Vorbis went through the server: the old clients hear each other
        self.assertGreater(float(re.search(r"snr=(-?[\d.]+)", vorbis_out).group(1)), 20, vorbis_out)
        during = [iv for iv in run.intervals.values() if iv["phase"] == "vorbis"]
        sent = [iv for iv in during if iv["user"].startswith("sender@")]
        self.assertTrue(sent, "the watcher saw no interval from the Vorbis sender\n" + log)
        self.assertEqual({iv["fourcc"] for iv in sent}, {b"OGGv"}, log)
        # the plug-in sends nothing there, says why, and plays the Vorbis as silence
        self.assertEqual(result.get("room"), "Vorbis", log)
        self.assertEqual(result.get("upload"), "-", log)
        self.assertEqual([iv for iv in during if iv["user"].startswith("plugin@")], [], log)
        self.assertEqual(result.get("warned"), "1", log)
        self.assertGreaterEqual(int(result.get("others", "0")), 1, log)  # the sender, with its channel
        self.assertLess(float(result["peak"]), 1e-4, log)


if __name__ == "__main__":
    unittest.main()
