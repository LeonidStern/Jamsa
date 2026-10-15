"""End-to-end Opus test: two NJClients (built with NINJAM_OPUS) talk through a real server.

Run:  make -C ninjam/tests && NINJAMSRV=../server/ninjamsrv python3 -m unittest -v test_opus_e2e
The server must be built from this tree (it needs codec negotiation).
"""
import os
import re
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "server", "tests"))

from njclient import Client, CAP_OPUS, DOWNLOAD_BEGIN  # noqa: E402
import test_server  # noqa: E402

HARNESS = os.path.join(HERE, "njclient_e2e")
FAST_INTERVALS = "DefaultBPM 240\nDefaultBPI 4\n"  # 1 second intervals


class OpusEndToEndTests(unittest.TestCase):
    def setUp(self):
        self.server = test_server.Server(FAST_INTERVALS).wait_ready()
        self.clients = []

    def tearDown(self):
        for c in self.clients:
            c.close()
        self.server.stop()
        out = self.server.output()
        self.server.log.close()
        self.assertNotIn("AddressSanitizer", out)

    def watcher(self, codecs):
        """A protocol-level client that subscribes to the sender and records the codec of each interval."""
        c = Client(self.server.port)
        self.assertTrue(c.login("anonymous:watch", codecs=codecs)[0])
        c.subscribe("sender@127.0.0.1")
        self.clients.append(c)
        return c

    def run_clients(self, watcher, srate=48000, seconds=8):
        proc = subprocess.Popen([HARNESS, "127.0.0.1:%d" % self.server.port, str(seconds), str(srate)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        fourccs = []
        while proc.poll() is None:
            for m in watcher.drain(0.1):
                if m[0] == DOWNLOAD_BEGIN and m[1][:16] != bytes(16):
                    fourccs.append(m[1][20:24])
        out = proc.communicate(timeout=30)[0]
        for m in watcher.drain(0.3):
            if m[0] == DOWNLOAD_BEGIN and m[1][:16] != bytes(16):
                fourccs.append(m[1][20:24])
        self.assertEqual(proc.returncode, 0, out)
        snr = float(re.search(r"snr=(-?[\d.]+)", out).group(1))
        return set(fourccs), snr, out

    def test_opus_room(self):
        w = self.watcher(CAP_OPUS)
        fourccs, snr, out = self.run_clients(w)
        self.assertEqual(fourccs, {b"OPUS"})
        self.assertGreater(snr, 30, out)

    def test_opus_room_at_44100(self):
        w = self.watcher(CAP_OPUS)
        fourccs, snr, out = self.run_clients(w, srate=44100)
        self.assertEqual(fourccs, {b"OPUS"})
        self.assertGreater(snr, 30, out)

    def test_falls_back_to_vorbis_when_someone_cannot_decode_opus(self):
        w = self.watcher(0)  # an old client in the room
        fourccs, snr, out = self.run_clients(w)
        self.assertEqual(fourccs, {b"OGGv"})
        self.assertGreater(snr, 20, out)


if __name__ == "__main__":
    unittest.main()
