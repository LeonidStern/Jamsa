"""Protocol-level tests for ninjamsrv.

Run:  NINJAMSRV=../ninjamsrv python3 -m unittest -v test_server
Each test starts its own server on a free port with a temporary config.
"""
import os
import socket
import subprocess
import tempfile
import time
import unittest
import uuid

from njclient import (Client, chat_fields, CHAT, DOWNLOAD_BEGIN, DOWNLOAD_WRITE)

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
    def __init__(self, extra_config="", port=None):
        self.dir = tempfile.mkdtemp(prefix="njtest")
        self.port = port or free_port()
        os.mkdir(os.path.join(self.dir, "archive"))
        cfg = os.path.join(self.dir, "test.cfg")
        with open(cfg, "w") as f:
            f.write("Port %d\n" % self.port + BASE_CONFIG + extra_config.replace("$DIR", self.dir))
        self.log = open(os.path.join(self.dir, "stdout.log"), "w+")
        self.proc = subprocess.Popen([SERVER, cfg], stdout=self.log, stderr=subprocess.STDOUT, cwd=self.dir)

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

    def client(self, name):
        c = Client(self.server.port)
        ok, _ = c.login("anonymous:" + name)
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


class StartupTests(unittest.TestCase):
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
