"""End-to-end codec tests: two NJClients (built with NINJAM_OPUS and NINJAM_FLAC) talk through a real server.

Run:  make -C ninjam/tests && NINJAMSRV=../server/ninjamsrv python3 -m unittest -v test_codecs_e2e
The server must be built from this tree (it needs codec negotiation and !codec).
"""
import os
import re
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "server", "tests"))

from njclient import Client, CAP_OPUS, CAP_FLAC, CHAT, chat_fields, DOWNLOAD_BEGIN  # noqa: E402
import test_server  # noqa: E402

HARNESS = os.path.join(HERE, "njclient_e2e")
FAST_INTERVALS = "DefaultBPM 240\nDefaultBPI 4\nAllowCodecs opus flac\n"  # 1 second intervals


class CodecEndToEndTests(unittest.TestCase):
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

    def watcher(self, codecs, room_codec=None):
        """A protocol-level client that subscribes to the sender and records the codec of each interval.
        With room_codec, it first switches the room's codec with !codec."""
        c = Client(self.server.port)
        self.assertTrue(c.login("anonymous:watch", codecs=codecs)[0])
        if room_codec:
            c.chat("MSG", "!codec " + room_codec)
            m = c.wait_for(lambda m: m[0] == CHAT and " chose " in " ".join(chat_fields(m[1]))
                           or m[0] == CHAT and "switched the room's audio" in " ".join(chat_fields(m[1])), 3.0)
            self.assertIsNotNone(m, "the server did not switch the codec")
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

    def test_flac_room(self):
        w = self.watcher(CAP_OPUS | CAP_FLAC, room_codec="flac")
        fourccs, snr, out = self.run_clients(w)
        self.assertEqual(fourccs, {b"FLAC"})
        self.assertGreater(snr, 60, out)  # lossless, 16 bit

    def test_flac_room_at_44100(self):
        w = self.watcher(CAP_OPUS | CAP_FLAC, room_codec="flac")
        fourccs, snr, out = self.run_clients(w, srate=44100)
        self.assertEqual(fourccs, {b"FLAC"})
        self.assertGreater(snr, 60, out)

    def test_vorbis_room_by_choice(self):
        w = self.watcher(CAP_OPUS | CAP_FLAC, room_codec="vorbis")
        fourccs, snr, out = self.run_clients(w)
        self.assertEqual(fourccs, {b"OGGv"})
        self.assertGreater(snr, 20, out)

    def test_flac_room_falls_back_to_opus_for_a_client_without_flac(self):
        w = self.watcher(CAP_OPUS, room_codec="flac")
        fourccs, snr, out = self.run_clients(w)
        self.assertEqual(fourccs, {b"OPUS"})
        self.assertGreater(snr, 30, out)

    def test_falls_back_to_vorbis_when_someone_cannot_decode_opus(self):
        w = self.watcher(0)  # an old client in the room
        fourccs, snr, out = self.run_clients(w)
        self.assertEqual(fourccs, {b"OGGv"})
        self.assertGreater(snr, 20, out)


if __name__ == "__main__":
    unittest.main()
