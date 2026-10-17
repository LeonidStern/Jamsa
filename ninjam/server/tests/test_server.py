"""Protocol-level tests for ninjamsrv.

Run:  NINJAMSRV=../ninjamsrv python3 -m unittest -v test_server
The same tests run against the embedded server with NINJAMSRV=path/to/ninjamsrv_embedded_runner
(cmake -DNINJAM_SERVER_TESTS=ON).
Each test starts its own server on a free port with a temporary config.
"""
import os
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest
import uuid

from njclient import (Client, chat_fields, codec_mask, mask_opus_kbps, CHAT, CODEC_MASK, CONFIG_CHANGE, DOWNLOAD_BEGIN,
                      DOWNLOAD_WRITE, CAP_OPUS, CAP_FLAC)
from fake_router import FakeIGD, FakeNatPmp

# absolute, because each server runs with its own temporary directory as cwd
SERVER = os.path.abspath(os.environ.get("NINJAMSRV", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ninjamsrv")))

BASE_CONFIG = """
AnonymousUsers yes
AnonymousUsersCanChat yes
AnonymousMaskIP no
MaxChannels 32 4
DefaultBPM 120
DefaultBPI 8
"""


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server:
    def __init__(self, extra_config="", port=None, env=None):
        self.dir = tempfile.mkdtemp(prefix="njtest")
        self.port = port or free_port()
        os.mkdir(os.path.join(self.dir, "archive"))
        cfg = os.path.join(self.dir, "test.cfg")
        with open(cfg, "w") as f:
            f.write("Port %d\n" % self.port + BASE_CONFIG + extra_config.replace("$DIR", self.dir))
        self.log = open(os.path.join(self.dir, "stdout.log"), "w+")
        self.cfg = cfg
        self.proc = subprocess.Popen([SERVER, cfg], stdout=self.log, stderr=subprocess.STDOUT, cwd=self.dir,
                                     env=dict(os.environ, **(env or {})))

    def wait_ready(self):
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.5).close()
                return self
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def output(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()

    def open_fds(self):
        return len(os.listdir("/proc/%d/fd" % self.proc.pid))

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


class ServerTest(unittest.TestCase):
    extra_config = ""

    def setUp(self):
        self.server = Server(self.extra_config).wait_ready()
        self.clients = []

    def tearDown(self):
        for c in self.clients:
            c.close()
        self.server.stop()
        out = self.server.output()
        self.server.log.close()
        # sanitizer builds report memory errors and undefined behaviour on stdout/stderr
        self.assertNotIn("runtime error", out)
        self.assertNotIn("AddressSanitizer", out)
        self.assertNotIn("ThreadSanitizer", out)

    def client(self, name, codecs=0):
        c = Client(self.server.port)
        ok, _ = c.login("anonymous:" + name, codecs=codecs)
        self.assertTrue(ok)
        self.clients.append(c)
        return c

    def two_users_subscribed(self):
        a = self.client("alice")
        b = self.client("bob")
        a.set_channels(["guitar"])
        b.subscribe(a.username)
        time.sleep(0.2)
        a.drain(0.2)
        b.drain(0.2)
        return a, b


class RelayTests(ServerTest):
    def test_interval_is_relayed_to_subscriber(self):
        a, b = self.two_users_subscribed()
        guid = uuid.uuid4().bytes
        a.upload_begin(guid)
        a.upload_write(guid, b"x" * 1000)
        a.upload_write(guid, b"y" * 10, end=True)
        self.assertIsNotNone(b.wait_for(lambda m: m[0] == DOWNLOAD_BEGIN and m[1][:16] == guid))
        writes = [m for m in b.drain(0.5) if m[0] == DOWNLOAD_WRITE]
        self.assertEqual([len(m[1]) - 17 for m in writes], [1000, 10])

    def test_only_the_uploader_can_write_to_an_interval(self):
        a, b = self.two_users_subscribed()
        other = self.client("carol")
        guid = uuid.uuid4().bytes
        a.upload_begin(guid)
        self.assertIsNotNone(b.wait_for(lambda m: m[0] == DOWNLOAD_BEGIN))
        other.upload_write(guid, b"not alice", end=True)
        a.upload_write(guid, b"alice", end=True)
        writes = [m[1][17:] for m in b.drain(0.5) if m[0] == DOWNLOAD_WRITE]
        self.assertEqual(writes, [b"alice"])



class ChannelLimitTests(ServerTest):
    extra_config = "MaxChannels 64 64\n"  # more than the server supports; must be clamped to 32

    def test_channel_index_beyond_32_is_ignored(self):
        a, b = self.two_users_subscribed()
        guid = uuid.uuid4().bytes
        a.upload_begin(guid, chidx=40)
        self.assertIsNone(b.wait_for(lambda m: m[0] == DOWNLOAD_BEGIN, 0.5))


class ChatTests(ServerTest):
    def test_chat_is_broadcast(self):
        a = self.client("alice")
        b = self.client("bob")
        b.drain(0.2)
        a.chat("MSG", "hello")
        m = b.wait_for(lambda m: m[0] == CHAT and chat_fields(m[1])[0] == "MSG")
        self.assertEqual(chat_fields(m[1])[:3], ["MSG", a.username, "hello"])

    def test_chat_without_final_terminator_is_ignored(self):
        a = self.client("alice")
        b = self.client("bob")
        b.drain(0.2)
        a.chat("MSG", "hello", terminate_last=False)
        self.assertIsNone(b.wait_for(lambda m: m[0] == CHAT and chat_fields(m[1])[0] == "MSG", 0.5))


class AdminCommandTests(ServerTest):
    extra_config = "User admin secret *\n"

    def test_commands_work_without_a_space(self):
        # NINJAM clients send "/bpm 120" as ADMIN "bpm 120", and "/bpm120" as ADMIN "bpm120", which has always worked
        a = Client(self.server.port)
        self.clients.append(a)
        self.assertTrue(a.login("admin", "secret")[0])
        a.drain(0.3)
        for cmd, bpm in (("bpm 100", 100), ("bpm90", 90)):
            a.chat("ADMIN", cmd)
            m = a.wait_for(lambda m: m[0] == CONFIG_CHANGE)
            self.assertIsNotNone(m, cmd)
            self.assertEqual(struct.unpack("<HH", m[1][:4]), (bpm, 8))


class DuplicateLoginTests(ServerTest):
    def test_replaced_login_sends_part(self):
        watcher = self.client("watcher")
        first = self.client("bob")
        watcher.drain(0.2)
        second = Client(self.server.port)
        self.clients.append(second)
        second.login("anonymous:bob")
        msgs = watcher.drain(0.5)
        events = [chat_fields(m[1])[:2] for m in msgs if m[0] == CHAT]
        self.assertIn(["PART", first.username], events)
        self.assertLess(events.index(["PART", first.username]), events.index(["JOIN", second.username]))


class PendingLimitTests(ServerTest):
    extra_config = "MaxPendingConnections 64 4\n"

    def test_connections_that_never_log_in_are_limited_per_address(self):
        idle = [Client(self.server.port) for _ in range(4)]
        self.clients.extend(idle)
        time.sleep(0.3)
        extra = Client(self.server.port)
        self.clients.append(extra)
        self.assertTrue(extra.is_closed(1.0))
        for c in idle:
            c.close()
        time.sleep(0.5)
        self.client("late")  # logging in works again once the idle ones are gone


class ArchiveTests(ServerTest):
    extra_config = "SessionArchive $DIR/archive 1\n"

    def test_unfinished_uploads_do_not_pile_up_open_files(self):
        a = self.client("alice")
        a.set_channels(["guitar"])
        # the session directory is created on the server's first idle check after someone logs in
        archive = os.path.join(self.server.dir, "archive")
        deadline = time.time() + 40
        while time.time() < deadline and not any(n.endswith(".ninjam") for n in os.listdir(archive)):
            a.recv(0.5)  # keeps the connection alive while waiting
        self.assertTrue(any(n.endswith(".ninjam") for n in os.listdir(archive)), "archiving never started")
        before = self.server.open_fds()
        for _ in range(300):
            a.upload_begin(uuid.uuid4().bytes)
        time.sleep(1.0)
        self.assertLess(self.server.open_fds() - before, 80)


class CodecTestCase(ServerTest):
    def mask(self, c):
        m = c.wait_for(lambda m: m[0] == CODEC_MASK)
        self.assertIsNotNone(m, "no codec mask")
        return codec_mask(m)

    def last_mask(self, c):
        masks = [codec_mask(m) for m in c.drain(0.3) if m[0] == CODEC_MASK]
        return masks[-1] if masks else None

    def interval(self, sender, fourcc, data=b"audio"):
        guid = uuid.uuid4().bytes
        sender.upload_begin(guid, fourcc=fourcc)
        sender.upload_write(guid, data, end=True)
        return guid

    def received(self, c):
        """(guid, fourcc) of each interval start, and the data written, for everything c got."""
        msgs = c.drain(0.5)
        begins = [(m[1][:16], m[1][20:24]) for m in msgs if m[0] == DOWNLOAD_BEGIN]
        writes = [m[1][17:] for m in msgs if m[0] == DOWNLOAD_WRITE]
        return begins, writes

    def server_says(self, c, timeout=0.5):
        """Chat lines from the server that c got."""
        return [chat_fields(m[1])[2] for m in c.drain(timeout)
                if m[0] == CHAT and chat_fields(m[1])[:2] == ["MSG", ""]]


class CodecTests(CodecTestCase):
    def test_mask_follows_who_is_in_the_room(self):
        a = self.client("alice", CAP_OPUS)
        self.assertEqual(self.mask(a), CAP_OPUS)
        b = self.client("bob", CAP_OPUS)
        self.assertEqual(self.mask(b), CAP_OPUS)

        old = self.client("old")  # can only decode Vorbis
        self.assertEqual(self.last_mask(a), 0)
        self.assertEqual(self.last_mask(b), 0)
        self.assertNotIn(CODEC_MASK, [m[0] for m in old.drain(0.3)])

        old.close()
        self.clients.remove(old)
        self.assertEqual(self.mask(a), CAP_OPUS)

    def test_opus_reaches_only_clients_that_decode_it(self):
        a = self.client("alice", CAP_OPUS)
        b = self.client("bob", CAP_OPUS)
        old = self.client("old")
        a.set_channels(["mic"])
        for c in (b, old):
            c.subscribe(a.username)
        time.sleep(0.2)
        for c in (a, b, old):
            c.drain(0.2)

        guid = self.interval(a, b"OPUS")
        self.assertEqual(self.received(b), ([(guid, b"OPUS")], [b"audio"]))
        self.assertEqual(self.received(old), ([(bytes(16), bytes(4))], []))  # a silent interval

        guid = self.interval(a, b"OGGv")
        self.assertEqual(self.received(old), ([(guid, b"OGGv")], [b"audio"]))

    def test_flac_is_off_unless_the_server_allows_it(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_OPUS | CAP_FLAC)
        self.assertEqual(self.mask(a), CAP_OPUS)
        b.subscribe(a.username)
        time.sleep(0.2)
        b.drain(0.2)
        self.interval(a, b"FLAC")
        self.assertEqual(self.received(b), ([(bytes(16), bytes(4))], []))


    def test_codec_the_server_does_not_allow_is_refused(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_OPUS | CAP_FLAC)
        self.assertEqual(self.mask(a), CAP_OPUS)
        b.drain(0.2)
        a.chat("MSG", "!codec flac")
        msgs = a.drain(0.5)
        self.assertNotIn(CODEC_MASK, [m[0] for m in msgs])
        self.assertIn("[server] this server doesn't allow FLAC",
                      [chat_fields(m[1])[2] for m in msgs if m[0] == CHAT])
        self.assertEqual(b.drain(0.3), [])


class RoomCodecTests(CodecTestCase):
    extra_config = "AllowCodecs opus flac\n"

    def test_room_uses_opus_until_someone_switches_it(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_OPUS | CAP_FLAC)
        self.assertEqual(self.mask(a), CAP_OPUS)
        self.assertEqual(self.mask(b), CAP_OPUS)

        a.chat("MSG", "!codec flac")
        for c in (a, b):
            msgs = c.drain(0.5)
            self.assertEqual([codec_mask(m) for m in msgs if m[0] == CODEC_MASK], [CAP_FLAC])
            chat = [chat_fields(m[1])[1:3] for m in msgs if m[0] == CHAT and chat_fields(m[1])[0] == "MSG"]
            # the room hears who switched it, not the command itself
            self.assertEqual(chat, [["", "[server] %s switched the room's audio to FLAC" % a.username]])

        b.chat("MSG", "!codec Vorbis")
        self.assertEqual(self.last_mask(a), 0)
        b.chat("MSG", "!codec opus ")
        self.assertEqual(self.last_mask(a), CAP_OPUS)

    def test_room_falls_back_while_someone_cannot_play_its_codec(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_OPUS | CAP_FLAC)
        a.chat("MSG", "!codec flac")
        self.assertEqual(self.last_mask(b), CAP_FLAC)
        a.drain(0.2)

        c = self.client("carol", CAP_OPUS)
        self.assertEqual(self.mask(c), CAP_OPUS)
        self.assertEqual(self.last_mask(b), CAP_OPUS)
        self.assertIn("[server] room audio is now Opus, because %s's client can't play FLAC" % c.username,
                      self.server_says(a))

        b.chat("MSG", "!codec")
        self.assertIn("[server] room audio is Opus (FLAC was chosen, but %s's client can't play it). "
                      "Change it with !codec opus, !codec flac or !codec vorbis" % c.username,
                      self.server_says(b))

        c.close()
        self.clients.remove(c)
        self.assertIn("[server] room audio is back to FLAC", self.server_says(a))
        self.assertEqual(self.last_mask(b), CAP_FLAC)

    def test_choosing_a_codec_someone_cannot_play_says_why(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_OPUS)
        self.assertEqual(self.mask(a), CAP_OPUS)
        b.drain(0.2)
        a.chat("MSG", "!codec flac")
        self.assertEqual(self.server_says(b), [
            "[server] %s chose FLAC, but %s's client can't play it, so the room uses Opus until they leave"
            % (a.username, b.username)])
        self.assertNotIn(CODEC_MASK, [m[0] for m in a.drain(0.3)])

    def test_unknown_codec_shows_the_room_codec_to_the_sender_only(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_OPUS | CAP_FLAC)
        self.assertEqual(self.mask(b), CAP_OPUS)
        b.drain(0.2)
        a.chat("MSG", "!codec mp3")
        self.assertIn("[server] room audio is Opus. Change it with !codec opus, !codec flac or !codec vorbis",
                      self.server_says(a))
        self.assertEqual(b.drain(0.3), [])


class RequireCodecTests(CodecTestCase):
    extra_config = "AllowCodecs opus flac\nRequireCodec opus\n"

    def test_client_that_cannot_play_opus_is_refused(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        self.assertEqual(self.mask(a), CAP_OPUS)
        a.drain(0.2)
        for caps in (0, CAP_FLAC):  # an older client (Vorbis only), and one that plays FLAC but not Opus
            old = Client(self.server.port)
            try:
                self.assertEqual(old.login("anonymous:old", codecs=caps),
                                 (False, "this server needs a NINJAM client that plays Opus"))
            finally:
                old.close()
        # the room never saw them, and still plays Opus
        self.assertEqual(a.drain(0.3), [])
        self.client("bob", CAP_OPUS)

    def test_codec_vorbis_is_refused(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_OPUS | CAP_FLAC)
        self.assertEqual(self.mask(a), CAP_OPUS)
        b.drain(0.2)
        a.chat("MSG", "!codec vorbis")
        msgs = a.drain(0.5)
        self.assertNotIn(CODEC_MASK, [m[0] for m in msgs])
        self.assertIn("[server] this server doesn't allow Vorbis",
                      [chat_fields(m[1])[2] for m in msgs if m[0] == CHAT])
        self.assertEqual(b.drain(0.3), [])

    def test_help_does_not_offer_vorbis(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        self.assertEqual(self.mask(a), CAP_OPUS)
        a.chat("MSG", "!codec")
        self.assertIn("[server] room audio is Opus. Change it with !codec opus or !codec flac", self.server_says(a))

    def test_room_falls_back_between_opus_and_flac_only(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_OPUS | CAP_FLAC)
        a.chat("MSG", "!codec flac")
        self.assertEqual(self.last_mask(b), CAP_FLAC)
        a.drain(0.2)

        # someone who plays Opus but not FLAC: the room falls back to Opus, not further
        c = self.client("carol", CAP_OPUS)
        self.assertEqual(self.mask(c), CAP_OPUS)
        self.assertEqual(self.last_mask(b), CAP_OPUS)
        self.assertIn("[server] room audio is now Opus, because %s's client can't play FLAC" % c.username,
                      self.server_says(a))
        b.chat("MSG", "!codec")
        self.assertIn("[server] room audio is Opus (FLAC was chosen, but %s's client can't play it). "
                      "Change it with !codec opus or !codec flac" % c.username, self.server_says(b))

        c.close()
        self.clients.remove(c)
        self.assertIn("[server] room audio is back to FLAC", self.server_says(a))
        self.assertEqual(self.last_mask(b), CAP_FLAC)
        b.chat("MSG", "!codec opus")
        self.assertEqual(self.last_mask(a), CAP_OPUS)


class RequireCodecWithoutAllowCodecsTests(CodecTestCase):
    extra_config = "AllowCodecs none\nRequireCodec opus\n"

    def test_required_codec_is_allowed(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        self.assertEqual(self.mask(a), CAP_OPUS)
        a.chat("MSG", "!codec flac")
        self.assertIn("[server] this server doesn't allow FLAC", self.server_says(a))


class AllowCodecsTests(ServerTest):
    extra_config = "AllowCodecs opus flac\n"

    def test_flac_is_relayed_when_allowed(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_FLAC)
        # FLAC is the only codec both can decode
        self.assertIsNotNone(a.wait_for(lambda m: m[0] == CODEC_MASK and codec_mask(m) == CAP_FLAC))
        b.subscribe(a.username)
        time.sleep(0.2)
        b.drain(0.2)
        guid = uuid.uuid4().bytes
        a.upload_begin(guid, fourcc=b"FLAC")
        a.upload_write(guid, b"lossless", end=True)
        msgs = b.drain(0.5)
        self.assertIn((guid, b"FLAC"), [(m[1][:16], m[1][20:24]) for m in msgs if m[0] == DOWNLOAD_BEGIN])
        self.assertIn(b"lossless", [m[1][17:] for m in msgs if m[0] == DOWNLOAD_WRITE])


class VorbisOnlyTests(ServerTest):
    extra_config = "AllowCodecs none\n"

    def test_mask_is_empty(self):
        a = self.client("alice", CAP_OPUS)
        m = a.wait_for(lambda m: m[0] == CODEC_MASK)
        self.assertIsNotNone(m)
        self.assertEqual(codec_mask(m), 0)


class BitrateTests(CodecTestCase):
    extra_config = "AllowCodecs opus flac\n"

    def masks(self, c):
        return [(codec_mask(m), mask_opus_kbps(m)) for m in c.drain(0.5) if m[0] == CODEC_MASK]

    def test_room_starts_at_128_kbps(self):
        a = self.client("alice", CAP_OPUS)
        m = a.wait_for(lambda m: m[0] == CODEC_MASK)
        self.assertEqual((codec_mask(m), mask_opus_kbps(m)), (CAP_OPUS, 128))

    def test_anyone_sets_the_bitrate_for_everyone(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_OPUS | CAP_FLAC)
        for c in (a, b):
            c.drain(0.5)
        a.chat("MSG", "!bitrate 256 ")
        for c in (a, b):
            msgs = c.drain(0.5)
            self.assertEqual([(codec_mask(m), mask_opus_kbps(m)) for m in msgs if m[0] == CODEC_MASK], [(CAP_OPUS, 256)])
            self.assertIn("[server] %s set the room's Opus bitrate to 256 kbps" % a.username,
                          [chat_fields(m[1])[2] for m in msgs if m[0] == CHAT and chat_fields(m[1])[:2] == ["MSG", ""]])
        # someone who joins later gets it too
        c = self.client("carol", CAP_OPUS)
        m = c.wait_for(lambda m: m[0] == CODEC_MASK)
        self.assertEqual(mask_opus_kbps(m), 256)

    def test_bitrate_in_a_flac_room_waits_for_opus(self):
        a = self.client("alice", CAP_OPUS | CAP_FLAC)
        b = self.client("bob", CAP_OPUS | CAP_FLAC)
        a.chat("MSG", "!codec flac")
        for c in (a, b):
            c.drain(0.5)
        b.chat("MSG", "!bitrate 96")
        self.assertEqual(self.masks(a), [(CAP_FLAC, 96)])
        self.assertIn("[server] %s set the room's Opus bitrate to 96 kbps, for when the room plays Opus" % b.username,
                      self.server_says(b))
        a.chat("MSG", "!codec opus")
        self.assertEqual(self.masks(b), [(CAP_OPUS, 96)])

    def test_a_bad_bitrate_only_tells_the_sender(self):
        a = self.client("alice", CAP_OPUS)
        b = self.client("bob", CAP_OPUS)
        for c in (a, b):
            c.drain(0.5)
        for arg in ("", " 20", " 511", " fast", " 128k"):
            a.chat("MSG", "!bitrate" + arg)
            msgs = a.drain(0.5)
            self.assertNotIn(CODEC_MASK, [m[0] for m in msgs])
            self.assertIn("[server] the room's Opus bitrate is 128 kbps. Change it with !bitrate N, from 32 to 510",
                          [chat_fields(m[1])[2] for m in msgs if m[0] == CHAT])
        self.assertEqual(b.drain(0.3), [])

    def test_unknown_command_lists_bitrate(self):
        a = self.client("alice", CAP_OPUS)
        a.drain(0.3)
        a.chat("MSG", "!speed 2")
        self.assertIn("Unknown !command. Commands available: !vote, !topic, !codec, !bitrate, !live",
                      [chat_fields(m[1])[2] for m in a.drain(0.5) if m[0] == CHAT])


class NoBitrateTests(CodecTestCase):
    extra_config = "OpusBitrate none\n"

    def test_mask_is_the_old_one_until_someone_sets_a_bitrate(self):
        a = self.client("alice", CAP_OPUS)
        m = a.wait_for(lambda m: m[0] == CODEC_MASK)
        self.assertEqual(len(m[1]), 4)  # what servers sent before there was a room bitrate
        self.assertEqual(codec_mask(m), CAP_OPUS)
        a.chat("MSG", "!bitrate")
        self.assertIn("[server] the room has no Opus bitrate: everyone sends their own. Set one with !bitrate N, from 32 to 510",
                      self.server_says(a))
        a.chat("MSG", "!bitrate 64")
        m = a.wait_for(lambda m: m[0] == CODEC_MASK)
        self.assertEqual((codec_mask(m), mask_opus_kbps(m)), (CAP_OPUS, 64))


class BadBitrateConfigTests(CodecTestCase):
    extra_config = "OpusBitrate 16\n"

    def test_a_bitrate_out_of_range_is_ignored(self):
        a = self.client("alice", CAP_OPUS)
        m = a.wait_for(lambda m: m[0] == CODEC_MASK)
        self.assertEqual(mask_opus_kbps(m), 128)  # a warning, and the default stays


class StartupTests(unittest.TestCase):
    @unittest.skipUnless(os.geteuid() == 0, "needs root to bind a port below 1024")
    @unittest.skipIf("embedded" in os.path.basename(SERVER), "the embedded server never changes user")
    def test_can_use_low_port_with_setuid(self):
        port = 1000 + os.getpid() % 23
        s = Server(extra_config="SetUID 65534\n", port=port)
        try:
            s.wait_ready()
            with open("/proc/%d/status" % s.proc.pid) as f:
                uid_line = [l for l in f if l.startswith("Uid:")][0]
            self.assertEqual(uid_line.split()[1], "65534")
            c = Client(port)
            self.assertTrue(c.login("anonymous:low")[0])
            c.close()
        finally:
            s.stop()
            s.log.close()

    def test_exits_with_error_when_port_is_taken(self):
        blocker = socket.socket()
        blocker.bind(("0.0.0.0", 0))
        blocker.listen(1)
        port = blocker.getsockname()[1]
        s = Server(port=port)
        try:
            code = s.proc.wait(5)
        except subprocess.TimeoutExpired:
            code = None
        finally:
            s.stop()
            blocker.close()
        out = s.output()
        s.log.close()
        self.assertEqual(code, 1)
        self.assertIn("Error listening on port", out)


if __name__ == "__main__":
    unittest.main()


def closed_url():
    return "http://127.0.0.1:%d/rootDesc.xml" % free_port()


class PortMappingTestCase(unittest.TestCase):
    """Starts a server with PortMapping yes against a fake router (and test_live.py does, with Live).
    Skips unless the server was built with PORTMAP=1."""

    def start(self, rootdesc, natpmp_gateway="127.0.0.1", extra_config=""):
        self.server = Server("LogFile $DIR/server.log\nPortMapping yes\n" + extra_config,
                             env={"NINJAM_UPNP_ROOTDESC": rootdesc, "NINJAM_NATPMP_GATEWAY": natpmp_gateway})
        self.server.wait_ready()
        self.addCleanup(self.cleanup)
        self.wait_for_log("[portmap]")
        if "built without port mapping support" in self.log():
            self.skipTest("server built without PORTMAP=1")

    def cleanup(self):
        self.server.stop()
        out = self.server.output()
        self.server.log.close()
        self.assertNotIn("runtime error", out)
        self.assertNotIn("AddressSanitizer", out)
        self.assertNotIn("ThreadSanitizer", out)

    def log(self):
        try:
            with open(os.path.join(self.server.dir, "server.log")) as f:
                return f.read()
        except FileNotFoundError:
            return ""

    def wait_for_log(self, text, timeout=10):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if text in self.log():
                return
            time.sleep(0.05)
        self.fail("%r not in server log:\n%s" % (text, self.log()))

    def shut_down(self):
        self.server.proc.send_signal(signal.SIGTERM)
        self.assertEqual(self.server.proc.wait(10), 0)


class PortMappingTests(PortMappingTestCase):
    """Router port mapping (PortMapping yes) against a fake router."""

    def test_upnp_forwards_the_port_and_removes_it_on_shutdown(self):
        igd = FakeIGD(external_ip="203.0.113.7")
        self.addCleanup(igd.close)
        self.start(igd.url)
        self.wait_for_log("UPnP: people can connect to 203.0.113.7:%d" % self.server.port)
        self.assertEqual(list(igd.mappings), [(self.server.port, "TCP")])  # no UDP without Live
        m = igd.mappings[(self.server.port, "TCP")]
        self.assertEqual((m["NewProtocol"], m["NewInternalPort"], m["NewInternalClient"], m["NewLeaseDuration"]),
                         ("TCP", str(self.server.port), "127.0.0.1", "3600"))

        self.shut_down()
        self.assertEqual(igd.mappings, {})
        self.assertIn("UPnP: removed the forward", self.log())

    def test_upnp_router_that_only_allows_permanent_forwards(self):
        igd = FakeIGD(only_permanent_leases=True)
        self.addCleanup(igd.close)
        self.start(igd.url)
        self.wait_for_log("people can connect to")
        self.assertEqual(igd.mappings[(self.server.port, "TCP")]["NewLeaseDuration"], "0")

    def test_warns_when_the_router_has_no_public_address(self):
        igd = FakeIGD(external_ip="100.64.12.34")  # carrier-grade NAT
        self.addCleanup(igd.close)
        self.start(igd.url)
        self.wait_for_log("which is not public")

    def test_turning_port_mapping_off_on_reload_removes_the_forward(self):
        igd = FakeIGD()
        self.addCleanup(igd.close)
        self.start(igd.url)
        self.wait_for_log("people can connect to")
        with open(self.server.cfg) as f:
            cfg = f.read()
        with open(self.server.cfg, "w") as f:
            f.write(cfg.replace("PortMapping yes", "PortMapping no"))
        self.server.proc.send_signal(signal.SIGHUP)
        self.wait_for_log("UPnP: removed the forward")
        self.assertEqual(igd.mappings, {})

    def test_falls_back_to_nat_pmp(self):
        try:
            natpmp = FakeNatPmp(external_ip="198.51.100.9")
        except OSError:
            self.skipTest("UDP port 5351 is in use")
        self.addCleanup(natpmp.close)
        self.start(closed_url())
        self.wait_for_log("NAT-PMP: people can connect to 198.51.100.9:%d" % self.server.port)
        self.assertEqual(list(natpmp.mappings), [(self.server.port, "TCP")])
        m = natpmp.mappings[(self.server.port, "TCP")]
        self.assertEqual((m["protocol"], m["internal"], m["lifetime"]), ("TCP", self.server.port, 3600))

        self.shut_down()
        self.assertEqual(natpmp.mappings, {})
        self.assertIn("NAT-PMP: removed the forward", self.log())

    def test_explains_manual_forwarding_when_no_router_answers(self):
        self.start(closed_url(), natpmp_gateway="127.0.0.1")
        self.wait_for_log("Forward TCP port %d on your router" % self.server.port)
        c = Client(self.server.port)
        self.assertTrue(c.login("anonymous:still_works")[0])
        c.close()
        self.shut_down()
