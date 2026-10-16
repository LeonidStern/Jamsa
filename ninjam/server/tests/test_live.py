"""Tests for Live mode: MESSAGE_SERVER_LIVE, !live and the UDP relay (see MESSAGE_SERVER_LIVE in ../../mpb.h).

Run:  NINJAMSRV=../ninjamsrv python3 -m unittest -v test_live
Like test_server.py, it also runs against ninjamsrv_embedded_runner.
"""
import os
import signal
import socket
import struct
import subprocess
import time
import unittest

from njclient import (Client, chat_fields, live_info, udp_hello, udp_ack, udp_data, udp_parse, CHAT, LIVE, CAP_LIVE,
                      CAP_OPUS, LIVE_ROOM_ON, LIVE_UDP_OPEN, LIVE_MAY_SEND, LIVE_PEER_BOUND, UDP_HELLO, UDP_DATA)
from test_server import SERVER, Server, ServerTest, PortMappingTestCase, closed_url, free_port
from fake_router import FakeIGD, FakeNatPmp

EMBEDDED = "embedded" in os.path.basename(SERVER)


class LiveTest(ServerTest):
    extra_config = "Live yes\n"
    env = {}

    def setUp(self):
        # the relay's traffic summary every second instead of every minute
        self.server = Server(self.extra_config, env=dict({"NINJAM_LIVE_STATS_SECONDS": "1"}, **self.env)).wait_ready()
        self.clients = []

    def live_client(self, name, caps=CAP_LIVE | CAP_OPUS, bind_host="127.0.0.1"):
        """A logged-in player with a Live client, its first Live message read and a UDP socket open."""
        c = self.client(name, caps)
        self.assertIsNotNone(c.wait_live(), "no Live message")
        c.udp_open(bind_host)
        return c

    def bind(self, *clients):
        """Each says HELLO, and again with the cookie it gets back, after which the server forwards audio to it."""
        for c in clients:
            self.assertIsNotNone(c.udp_bind(), "no HELLO_ACK")

    def socket_at(self, host):
        """A UDP socket at another address of this machine (skips the test where there is none)."""
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(s.close)
        try:
            s.bind((host, 0))
        except OSError:
            self.skipTest("can't use " + host)
        return s

    def ack(self, s, timeout=2.0):
        """The HELLO_ACK that s gets: (timestamp, cookie)."""
        s.settimeout(timeout)
        d = s.recvfrom(100)[0]
        self.assertEqual((d[:4], len(d)), (b"NL\x01\x01", 24))
        return udp_ack(d)

    def turn_on(self, c, *others):
        c.chat("MSG", "!live on")
        for x in (c,) + others:
            self.assertIsNotNone(x.wait_live(lambda i: i["flags"] & LIVE_ROOM_ON), "Live did not turn on")

    def server_says(self, c, timeout=0.5):
        """Chat lines from the server that c got."""
        return [chat_fields(m[1])[2] for m in c.drain(timeout)
                if m[0] == CHAT and chat_fields(m[1])[:2] == ["MSG", ""]]

    def got(self, c, timeout=0.3):
        """(from_id, audio) of each DATA datagram c got."""
        out = []
        for d in c.udp_drain(timeout):
            t, frm, rest = udp_parse(d)
            self.assertEqual(t, UDP_DATA)
            out.append((frm, rest))
        return out


class LiveMessageTests(LiveTest):
    def test_old_clients_never_get_it(self):
        old = self.client("old", CAP_OPUS)
        older = self.client("older")  # Vorbis only
        a = self.live_client("alice")
        b = self.live_client("bob")
        self.bind(a, b)
        self.turn_on(a, b)
        for c in (old, older):
            msgs = c.drain(0.5)
            self.assertNotIn(LIVE, [m[0] for m in msgs])
            # they do see the room's chat about it
            self.assertTrue(any("turned Live on" in chat_fields(m[1])[2] for m in msgs if m[0] == CHAT))

    def test_live_client_gets_its_id_and_key(self):
        a = self.client("alice", CAP_LIVE)
        info = a.wait_live()
        self.assertIsNotNone(info)
        key = info["key"]
        self.assertEqual((info["version"], info["fixed_len"], info["flags"], info["udp_port"], info["kbps"],
                          info["frame"], info["max_peers"], info["peers"]),
                         (1, 32, LIVE_UDP_OPEN | LIVE_MAY_SEND, 0, 128, 240, 8, []))
        self.assertNotEqual(info["peer_id"], 0)
        self.assertNotEqual(info["key"], 0)

        b = self.client("bob", CAP_LIVE | CAP_OPUS)
        binfo = b.wait_live()
        self.assertEqual(binfo["peers"], [(info["peer_id"], 0, a.username)])
        self.assertNotEqual(binfo["peer_id"], info["peer_id"])
        self.assertNotEqual(binfo["key"], info["key"])
        info = a.wait_live()
        self.assertEqual(info["peers"], [(binfo["peer_id"], 0, b.username)])
        self.assertEqual(info["key"], key)

        # sent only when something changed
        self.assertNotIn(LIVE, [m[0] for m in a.drain(0.5) + b.drain(0.1)])

    def test_peers_show_who_the_server_hears(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        a.wait_live(lambda i: i["peers"])
        self.bind(b)
        info = a.wait_live(lambda i: i["peers"] and i["peers"][0][1] & LIVE_PEER_BOUND)
        self.assertEqual(info["peers"], [(b.live["peer_id"], LIVE_PEER_BOUND, b.username)])

    def test_peer_list_drops_who_leaves(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        self.assertIsNotNone(a.wait_live(lambda i: i["peers"]))
        b.close()
        self.clients.remove(b)
        self.assertIsNotNone(a.wait_live(lambda i: not i["peers"]))


class LiveSettingsTests(LiveTest):
    extra_config = "Live on\nLiveKbps 96\nLiveFrame 120\nLiveMaxPeers 2\nLivePort 4000\n"

    def test_settings_reach_clients(self):
        a = self.live_client("alice")
        self.assertEqual((a.live["flags"], a.live["kbps"], a.live["frame"], a.live["max_peers"], a.live["udp_port"]),
                         (LIVE_ROOM_ON | LIVE_UDP_OPEN | LIVE_MAY_SEND, 96, 120, 2, 4000))

    def test_players_over_the_limit_stay_on_intervals(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        c = self.live_client("carol")
        self.bind(a, b, c)
        self.assertEqual(c.live["flags"], LIVE_ROOM_ON | LIVE_UDP_OPEN)  # may not send
        self.assertEqual([p[0] for p in c.live["peers"]], [a.live["peer_id"], b.live["peer_id"]])
        for x in (a, b):
            infos = [live_info(m) for m in x.drain(0.3) if m[0] == LIVE]
            self.assertTrue(infos)
            for i in infos:
                self.assertNotIn(c.live["peer_id"], [p[0] for p in i["peers"]])  # carol isn't listed

        a.send_live(b"to everyone")
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"to everyone")])
        self.assertEqual(self.got(c, 0.1), [])
        c.send_live(b"from carol")
        self.assertEqual(self.got(a), [])
        self.assertEqual(self.got(b, 0.1), [])

        a.chat("MSG", "!live")
        self.assertEqual(self.server_says(a), [
            "[server] Live is on. Players who can play Live: %s and %s. Live has room for 2, so %s stays on intervals. "
            "Turn it off with !live off" % (a.username, b.username, c.username)])

        # when someone leaves, the next one in line plays Live
        b.close()
        self.clients.remove(b)
        self.assertIsNotNone(c.wait_live(lambda i: i["flags"] & LIVE_MAY_SEND))
        a.wait_live(lambda i: [p[0] for p in i["peers"]] == [c.live["peer_id"]])
        c.send_live(b"carol now")
        self.assertEqual(self.got(a), [(c.live["peer_id"], b"carol now")])


class LiveCommandTests(LiveTest):
    extra_config = "Live yes\nUser mute secret C\n"  # mute may chat but not vote

    def test_turning_live_on_and_off(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        old = self.client("old", CAP_OPUS)
        for c in (a, b, old):
            c.drain(0.3)
        a.chat("MSG", "!live on")
        for c in (a, b):
            self.assertEqual(c.wait_live().get("flags"), LIVE_ROOM_ON | LIVE_UDP_OPEN | LIVE_MAY_SEND)
        on = ("[server] %s turned Live on: Jamsa players now hear each other in real time. "
              "%s's client can't play Live and stays on intervals." % (a.username, old.username))
        for c in (a, b, old):
            self.assertEqual(self.server_says(c), [on])

        b.chat("MSG", "!live off ")
        for c in (a, b):
            self.assertEqual(c.wait_live().get("flags"), LIVE_UDP_OPEN | LIVE_MAY_SEND)
        off = "[server] %s turned Live off: everyone hears each other through intervals again." % b.username
        for c in (a, b, old):
            self.assertEqual(self.server_says(c), [off])

    def test_two_players_who_cannot_play_live(self):
        a = self.live_client("alice")
        old = self.client("old", CAP_OPUS)
        older = self.client("older")
        a.drain(0.3)
        a.chat("MSG", "!live on")
        self.assertIn("The clients of %s and %s can't play Live, so they stay on intervals." % (old.username, older.username),
                      self.server_says(a)[0])

    def test_status_goes_to_the_sender_only(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        for c in (a, b):
            c.drain(0.3)
        for arg in ("", " maybe"):
            a.chat("MSG", "!live" + arg)
            self.assertEqual(self.server_says(a), [
                "[server] Live is off. Players who can play Live: %s and %s. Turn it on with !live on" % (a.username, b.username)])
        self.assertEqual(b.drain(0.3), [])
        # asking for what it already is says so, to the sender only
        self.turn_on(a, b)
        for c in (a, b):
            c.drain(0.3)
        b.chat("MSG", "!live on")
        self.assertEqual(self.server_says(b), [
            "[server] Live is on. Players who can play Live: %s and %s. Turn it off with !live off" % (a.username, b.username)])
        self.assertEqual(a.drain(0.3), [])

    def test_changing_it_needs_vote_permission(self):
        a = self.live_client("alice")
        m = Client(self.server.port)
        self.clients.append(m)
        self.assertTrue(m.login("mute", "secret", caps=CAP_LIVE)[0])
        self.assertIsNotNone(m.wait_live())
        a.drain(0.3)
        m.drain(0.3)
        m.chat("MSG", "!live on")
        self.assertEqual(self.server_says(m), ["[server] you don't have permission to change the room's audio"])
        self.assertEqual(a.drain(0.3), [])
        m.chat("MSG", "!live")  # anyone can ask
        self.assertTrue(self.server_says(m)[0].startswith("[server] Live is off."))

    def test_unknown_command_lists_live(self):
        a = self.live_client("alice")
        a.drain(0.3)
        a.chat("MSG", "!livestream")  # not !live
        self.assertIn("Unknown !command. Commands available: !vote, !topic, !codec, !bitrate, !live",
                      [chat_fields(m[1])[2] for m in a.drain(0.5) if m[0] == CHAT])


class NoLiveTests(LiveTest):
    extra_config = ""  # Live no, the default

    def test_server_without_live_is_like_an_old_one(self):
        a = self.client("alice", CAP_LIVE | CAP_OPUS)
        a.udp_open()
        self.assertNotIn(LIVE, [m[0] for m in a.drain(0.5)])
        for cmd in ("!live", "!live on"):
            a.chat("MSG", cmd)
            self.assertEqual(self.server_says(a), ["[server] this server doesn't have Live"])
        a.udp_send(udp_hello(1, 1))
        self.assertIsNone(a.udp_recv(0.3))


class RelayTests(LiveTest):
    extra_config = "Live on\n"

    def three(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        c = self.live_client("carol")
        self.bind(a, b, c)
        return a, b, c

    def test_hello_is_answered_with_its_timestamp(self):
        a = self.live_client("alice")
        a.hello(0x1122334455667788)
        ack = a.udp_recv(2.0)
        # and a cookie for the address and port it came from, smaller than the HELLO
        self.assertEqual(ack[:16], b"NL\x01\x01" + bytes(4) + struct.pack("<Q", 0x1122334455667788))
        self.assertEqual(len(ack), 24)
        cookie = udp_ack(ack)[1]
        a.hello(2)
        self.assertEqual(udp_ack(a.udp_recv(2.0)), (2, cookie))
        a.udp_open()  # another port
        a.hello(3)
        self.assertNotEqual(udp_ack(a.udp_recv(2.0))[1], cookie)

    def test_audio_reaches_one_player_or_everyone_else(self):
        a, b, c = self.three()
        a.send_live(b"to bob", dest=b.live["peer_id"])
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"to bob")])
        self.assertEqual(self.got(c, 0.1) + self.got(a, 0.1), [])

        a.send_live(b"to all")
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"to all")])
        self.assertEqual(self.got(c), [(a.live["peer_id"], b"to all")])
        self.assertEqual(self.got(a, 0.1), [])

        # never to yourself, nor to someone the server doesn't know
        a.send_live(b"to me", dest=a.live["peer_id"])
        a.send_live(b"to nobody", dest=0x7fffffff)
        self.assertEqual(self.got(a) + self.got(b, 0.1) + self.got(c, 0.1), [])

    def test_the_biggest_audio_packet_fits(self):
        a, b, c = self.three()
        a.send_live(b"x" * 1452, dest=b.live["peer_id"])
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"x" * 1452)])
        a.send_live(b"x" * 1453, dest=b.live["peer_id"])
        self.assertEqual(self.got(b), [])

    def test_bad_datagrams_are_dropped(self):
        a, b, c = self.three()
        me, key, bob = a.live["peer_id"], a.live["key"], b.live["peer_id"]
        good = udp_data(me, key, bob, b"ok")
        bad = [
            udp_data(me, key ^ 1, bob, b"wrong key"),
            udp_data(me ^ 0x40000000, key, bob, b"unknown sender"),
            udp_data(b.live["peer_id"], b.live["key"] ^ 2, me, b"bob's id, not his key"),
            b"XL" + good[2:],
            b"NL\x02" + good[3:],  # a newer version
            b"NL\x01\x03" + good[4:],  # unknown type
            good[:20],  # no audio
            good[:19],
            udp_hello(me, key)[:39],
            udp_hello(me, key)[:32],  # the size it had before cookies
            udp_hello(me, key) + b"\0",
            b"",
            b"N",
            udp_data(me, key, bob, b"y" * 3000),
            udp_data(me, key, bob, b"z" * 60000),
        ]
        for d in bad:
            a.udp_send(d)
        self.assertEqual(self.got(b) + self.got(c, 0.1), [])
        self.assertEqual(a.udp_drain(0.1), [])  # nothing is answered
        a.udp_send(good)
        self.assertEqual(self.got(b), [(me, b"ok")])

    def test_audio_from_another_address_is_dropped(self):
        a, b, c = self.three()
        other = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(other.close)
        try:
            other.bind(("127.0.0.2", 0))  # this machine, but not the address alice connected from
        except OSError:
            self.skipTest("can't use 127.0.0.2")
        other.sendto(udp_data(a.live["peer_id"], a.live["key"], 0, b"spoofed"), ("127.0.0.1", self.server.port))
        other.sendto(udp_hello(a.live["peer_id"], a.live["key"]), ("127.0.0.1", self.server.port))
        self.assertEqual(self.got(b) + self.got(c, 0.1), [])
        other.settimeout(0.2)
        self.assertRaises(socket.timeout, other.recvfrom, 100)
        # and alice is still where she was
        b.send_live(b"to alice", dest=a.live["peer_id"])
        self.assertEqual(self.got(a), [(b.live["peer_id"], b"to alice")])

    def test_the_latest_address_wins(self):
        # a router may give the player another port (NAT rebinding); it counts once it echoes its cookie
        a, b, c = self.three()
        moved = self.socket_at("127.0.0.1")
        server = ("127.0.0.1", self.server.port)
        moved.sendto(udp_hello(b.live["peer_id"], b.live["key"], 7, b.cookie), server)  # the old port's cookie
        self.assertEqual(self.ack(moved)[0], 7)
        moved.sendto(udp_hello(b.live["peer_id"], b.live["key"], 8), server)
        timestamp, cookie = self.ack(moved)
        self.assertEqual(timestamp, 8)
        a.send_live(b"before")
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"before")])  # still the old port
        moved.sendto(udp_hello(b.live["peer_id"], b.live["key"], 9, cookie), server)
        self.assertEqual(self.ack(moved), (9, cookie))
        a.send_live(b"after the move")
        moved.settimeout(1.0)
        self.assertEqual(udp_parse(moved.recvfrom(2000)[0])[2], b"after the move")
        self.assertEqual(self.got(b, 0.2), [])  # not to the old port

    def test_someone_elses_hello_moves_nobody(self):
        # in the same household or behind the same carrier NAT, someone may send from the player's address
        a, b, c = self.three()
        other = self.socket_at("127.0.0.1")
        other.sendto(udp_hello(b.live["peer_id"], b.live["key"], 1), ("127.0.0.1", self.server.port))
        self.ack(other)  # it isn't echoed: other stands for someone who doesn't get it
        a.send_live(b"to bob")
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"to bob")])
        other.settimeout(0.2)
        self.assertRaises(socket.timeout, other.recvfrom, 2000)

    def test_players_not_heard_from_get_nothing(self):
        a, b, c = self.three()
        d = self.live_client("dave")  # no HELLO yet
        a.send_live(b"before")
        self.assertEqual(len(self.got(b)), 1)
        self.assertEqual(self.got(d, 0.1), [])
        self.bind(d)
        a.send_live(b"after")
        self.assertEqual(self.got(d), [(a.live["peer_id"], b"after")])

    def test_player_who_leaves_is_forgotten(self):
        a, b, c = self.three()
        gone_udp, gone = a.udp, dict(a.live)
        a.udp = None
        self.addCleanup(gone_udp.close)
        a.close()
        self.clients.remove(a)
        self.assertIsNotNone(b.wait_live(lambda i: gone["peer_id"] not in [p[0] for p in i["peers"]]))
        gone_udp.sendto(udp_data(gone["peer_id"], gone["key"], 0, b"ghost"), ("127.0.0.1", self.server.port))
        self.assertEqual(self.got(b) + self.got(c, 0.1), [])
        b.send_live(b"to alice", dest=gone["peer_id"])
        gone_udp.settimeout(0.3)
        self.assertRaises(socket.timeout, gone_udp.recvfrom, 100)

    def test_old_clients_get_no_audio(self):
        a, b, c = self.three()
        old = self.client("old", CAP_OPUS)
        old.udp_open()
        # an old client knows no id or key; guessing the next ids gets nowhere
        for guess in range(1, 64):
            old.udp_send(udp_hello(guess, 0))
        self.assertEqual(old.udp_drain(0.3), [])
        a.send_live(b"live")
        self.assertEqual(len(self.got(b)), 1)
        self.assertEqual(old.udp_drain(0.1), [])

    def test_a_flood_is_cut_to_the_rate_limit(self):
        a, b, c = self.three()
        b.udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)  # so that it's the server that drops them

        def flood(count, size):
            """How many of count datagrams of size bytes, sent as fast as possible, get through; and how long
            sending them took."""
            t0 = time.perf_counter()
            for i in range(count):
                a.send_live(struct.pack("<I", i) + b"f" * (size - 24), dest=b.live["peer_id"])
            elapsed = time.perf_counter() - t0
            return len(self.got(b, 0.5)), elapsed

        # 128 kbps and 240-sample frames allow 65600 bytes/s and 450 datagrams/s, a quarter second ahead
        time.sleep(0.3)  # all of that
        n, elapsed = flood(1000, 120)  # the datagrams run out first: 112 at once, then 450 a second
        self.assertGreaterEqual(n, 110)
        self.assertLessEqual(n, 112.5 + 450 * elapsed + 2)
        time.sleep(1.2)  # a long pause saves up a quarter second's worth, and no more
        n, elapsed = flood(200, 1472)  # the bytes run out first: 16400 bytes is 11 of these, then 44.6 a second
        self.assertGreaterEqual(n, 10)
        self.assertLessEqual(n, 11.2 + 44.6 * elapsed + 2)
        time.sleep(0.3)
        a.send_live(b"later", dest=b.live["peer_id"])
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"later")])
        # the summary in the log (every second here) counts what was dropped, without a line per datagram
        a.recv(1.5)
        self.server.stop()  # flushes its output
        out = self.server.output()
        self.assertIn("over the rate limit", out)
        self.assertLess(out.count("[live]"), 10)


class LooseAddressTests(LiveTest):
    """LiveStrictAddress no: a player's UDP may come from another address than its TCP connection."""
    extra_config = "Live on\nLiveStrictAddress no\n"

    def three(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        c = self.live_client("carol")
        self.bind(a, b, c)
        return a, b, c

    def test_audio_from_another_address_is_forwarded(self):
        a, b, c = self.three()
        other = self.socket_at("127.0.0.2")  # not the address alice connected from
        server = ("127.0.0.1", self.server.port)
        other.sendto(udp_data(a.live["peer_id"], a.live["key"], 0, b"from elsewhere"), server)
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"from elsewhere")])
        self.assertEqual(self.got(c), [(a.live["peer_id"], b"from elsewhere")])
        # the key still has to match
        other.sendto(udp_data(a.live["peer_id"], a.live["key"] ^ 1, 0, b"wrong key"), server)
        other.sendto(udp_hello(a.live["peer_id"], a.live["key"] ^ 1), server)
        self.assertEqual(self.got(b) + self.got(c, 0.1), [])
        other.settimeout(0.2)
        self.assertRaises(socket.timeout, other.recvfrom, 100)

    def test_a_forged_hello_sends_the_room_nowhere(self):
        # Mallory writes another address (the victim's) on a HELLO with her own id and key: the victim gets
        # the answer, which is smaller, but not the room's audio
        a, b, c = self.three()
        victim = self.socket_at("127.0.0.3")
        server = ("127.0.0.1", self.server.port)
        victim.sendto(udp_hello(a.live["peer_id"], a.live["key"], 5), server)
        self.assertEqual(self.ack(victim)[0], 5)
        victim.sendto(udp_hello(a.live["peer_id"], a.live["key"], 6, a.cookie), server)  # alice's cookie is no good there
        self.assertEqual(self.ack(victim)[0], 6)
        for x in (b, c):
            x.send_live(b"to everyone")
        self.assertEqual(sorted(self.got(a)), sorted([(b.live["peer_id"], b"to everyone"), (c.live["peer_id"], b"to everyone")]))
        b.send_live(b"to alice", dest=a.live["peer_id"])
        self.assertEqual(self.got(a), [(b.live["peer_id"], b"to alice")])
        victim.settimeout(0.2)
        self.assertRaises(socket.timeout, victim.recvfrom, 2000)

    def test_a_forged_hello_binds_nobody(self):
        a = self.live_client("alice")
        d = self.live_client("dave")  # never says HELLO
        self.bind(a)
        self.assertIsNotNone(a.wait_live(lambda i: i["peers"]))
        victim = self.socket_at("127.0.0.3")
        victim.sendto(udp_hello(d.live["peer_id"], d.live["key"], 5), ("127.0.0.1", self.server.port))
        self.ack(victim)
        self.assertIsNone(a.wait_live(lambda i: i["peers"] and i["peers"][0][1] & LIVE_PEER_BOUND, timeout=0.5))
        a.send_live(b"to everyone")
        a.send_live(b"to dave", dest=d.live["peer_id"])
        victim.settimeout(0.3)
        self.assertRaises(socket.timeout, victim.recvfrom, 2000)

    def test_an_address_that_echoes_its_cookie_is_the_players(self):
        # the player moved (from Wi-Fi to mobile data): its new address counts once it echoes its cookie
        a, b, c = self.three()
        moved = self.socket_at("127.0.0.2")
        server = ("127.0.0.1", self.server.port)
        moved.sendto(udp_hello(a.live["peer_id"], a.live["key"], 1), server)
        cookie = self.ack(moved)[1]
        moved.sendto(udp_hello(a.live["peer_id"], a.live["key"], 2, cookie), server)
        self.assertEqual(self.ack(moved), (2, cookie))
        b.send_live(b"to the new address")
        moved.settimeout(1.0)
        self.assertEqual(udp_parse(moved.recvfrom(2000)[0])[1:], (b.live["peer_id"], b"to the new address"))
        self.assertEqual(self.got(a, 0.2), [])


class FreshnessTests(LiveTest):
    """A player that stops saying HELLO gets no more audio (after 20 seconds, 1 here)."""
    extra_config = "Live on\n"
    env = {"NINJAM_LIVE_FRESH_SECONDS": "1"}

    def test_players_who_stop_saying_hello_get_nothing(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        self.bind(a, b)
        self.assertIsNotNone(a.wait_live(lambda i: i["peers"] and i["peers"][0][1] & LIVE_PEER_BOUND))
        a.send_live(b"while bob says HELLO")
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"while bob says HELLO")])
        # bob keeps playing, but his audio doesn't count as a HELLO
        deadline = time.time() + 3
        info = None
        while info is None and time.time() < deadline:
            b.send_live(b"bob plays")
            info = a.wait_live(lambda i: i["peers"] and not i["peers"][0][1] & LIVE_PEER_BOUND, timeout=0.1)
        self.assertIsNotNone(info, "bob's audio kept him bound")
        b.udp_drain(0.1)
        a.send_live(b"to everyone")
        a.send_live(b"to bob", dest=b.live["peer_id"])
        self.assertEqual(self.got(b), [])
        # his next HELLO (with his cookie) brings him back
        self.bind(b)
        self.assertIsNotNone(a.wait_live(lambda i: i["peers"] and i["peers"][0][1] & LIVE_PEER_BOUND))
        a.send_live(b"again")
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"again")])


class ServerAddressTests(LiveTest):
    """A machine with more than one address answers each player from the one it sends to."""
    extra_config = "Live on\n"

    def setUp(self):
        super().setUp()
        try:
            socket.create_connection(("127.0.0.2", self.server.port), timeout=1.0).close()
        except OSError:
            self.skipTest("can't reach this machine at 127.0.0.2")  # e.g. macOS, whose lo0 has only 127.0.0.1

    def player(self, name):
        """A Live player that connects to the server at 127.0.0.2, whose UDP socket is connect()ed there like the
        plug-in's, so it drops whatever comes from another address (this machine's routing table picks 127.0.0.1)."""
        c = Client(self.server.port, host="127.0.0.2")
        self.clients.append(c)
        self.assertTrue(c.login("anonymous:" + name, caps=CAP_LIVE | CAP_OPUS)[0])
        self.assertIsNotNone(c.wait_live(), "no Live message")
        c.udp_open(connect=True)
        return c

    def test_answers_come_from_the_address_players_send_to(self):
        a = self.player("alice")
        b = self.player("bob")
        self.bind(a, b)
        a.send_live(b"to everyone")
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"to everyone")])
        b.send_live(b"to alice", dest=a.live["peer_id"])
        self.assertEqual(self.got(a), [(b.live["peer_id"], b"to alice")])


class SmallFramesTests(LiveTest):
    # the smallest frames at the lowest bitrate: datagrams whose headers are bigger than their audio
    extra_config = "Live on\nLiveFrame 120\nLiveKbps 32\n"

    def test_a_steady_stream_is_never_cut(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        self.bind(a, b)
        b.udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        b.udp.setblocking(False)
        # what a Jamsa sends for each 2.5 ms of Opus at 32 kbps: AOO's headers (16 bytes, and 8 for the sample
        # rate) and about 13 bytes of audio, behind our 20; 400 a second, for 5 seconds
        sent, got = 2000, 0
        next_send = time.perf_counter()
        for i in range(sent):
            while time.perf_counter() < next_send:
                pass
            next_send = time.perf_counter() + 1 / 400  # never faster, even to catch up after a stall
            a.send_live(struct.pack("<I", i) + b"a" * 33)
            if i % 400 == 0:
                a.keepalive()
                b.keepalive()
            while True:
                try:
                    b.udp.recv(2048)
                    got += 1
                except BlockingIOError:
                    break
        got += len(b.udp_drain(0.3))
        self.assertEqual(got, sent)
        self.server.stop()  # flushes its output
        self.assertNotIn("over the rate limit", self.server.output())


class RelayOffTests(LiveTest):
    def test_nothing_is_forwarded_while_live_is_off(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        self.bind(a, b)
        a.send_live(b"too soon")
        self.assertEqual(self.got(b), [])
        self.turn_on(a, b)
        a.send_live(b"now")
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"now")])
        a.chat("MSG", "!live off")
        self.assertIsNotNone(b.wait_live(lambda i: not i["flags"] & LIVE_ROOM_ON))
        a.send_live(b"too late")
        self.assertEqual(self.got(b), [])
        # HELLOs are answered either way, so the clients can measure and keep their router open
        a.hello(3)
        self.assertEqual(udp_parse(a.udp_recv(2.0))[0], UDP_HELLO)


class LatencyTests(LiveTest):
    extra_config = "Live on\nLiveFrame 120\n"  # up to 850 datagrams/s

    def measure(self, a, b, dest):
        """Milliseconds from sending each of 1000 datagrams to receiving it: (median, 99th percentile)."""
        lat = []
        next_send = time.perf_counter()
        for i in range(1000):
            # at most 500 a second, well under the rate limit
            while time.perf_counter() < next_send:
                pass
            next_send = time.perf_counter() + 0.002  # from now: no catching up (a burst) after a stall
            t0 = time.perf_counter()
            a.send_live(struct.pack("<I", i), dest=dest)
            while True:
                try:
                    d, _ = b.udp.recvfrom(100)
                except socket.timeout:
                    d = None  # lost: counts as a very slow one
                    break
                if struct.unpack("<I", udp_parse(d)[2])[0] >= i:  # skips one that came after its timeout
                    break
            lat.append(time.perf_counter() - t0)
            if d is not None:
                self.assertEqual(udp_parse(d)[2], struct.pack("<I", i))
            if i % 200 == 0:
                a.keepalive()
                b.keepalive()
        lat.sort()
        return lat[499] * 1000, lat[989] * 1000

    def test_forwarding_is_fast(self):
        a = self.live_client("alice")
        b = self.live_client("bob")
        self.bind(a, b)
        b.udp.settimeout(1.0)
        p50, p99 = self.measure(a, b, b.live["peer_id"])
        if p99 >= 2.0:
            # once more, in case something else was keeping this machine busy (a slow relay stays slow)
            p50, p99 = self.measure(a, b, b.live["peer_id"])
        self.assertLess(p99, 2.0, "forwarding took %.3f ms (median %.3f ms) for 1%% of datagrams" % (p99, p50))


class PrivateRoomTests(LiveTest):
    # (with MaxUsers 0, the default, nobody can join a room someone is already in)
    extra_config = "Live on\nPrivateGroupMode 4\nPrivateGroupAllowChat yes\nMaxUsers 8\n"

    def join(self, name, room):
        c = self.client(name, CAP_LIVE | CAP_OPUS)
        self.assertNotIn(LIVE, [m[0] for m in c.drain(0.3)])  # never in the lobby
        c.chat("MSG", "!join " + room)
        self.assertIsNotNone(c.wait_live(), "no Live message in room " + room)
        c.udp_open()
        return c

    def test_audio_stays_in_its_room(self):
        a = self.join("alice", "red")
        b = self.join("bob", "red")
        c = self.join("carol", "blue")
        self.bind(a, b, c)
        self.assertEqual(c.live["peers"], [])
        self.assertEqual([p[0] for p in b.live["peers"]], [a.live["peer_id"]])
        a.send_live(b"red room")
        self.assertEqual(self.got(b), [(a.live["peer_id"], b"red room")])
        self.assertEqual(self.got(c, 0.1), [])
        a.send_live(b"to carol", dest=c.live["peer_id"])
        c.send_live(b"to alice", dest=a.live["peer_id"])
        self.assertEqual(self.got(c) + self.got(a, 0.1), [])


class UdpPortTakenTests(unittest.TestCase):
    def start(self, live):
        # a port that is free for TCP and taken for UDP
        for _ in range(20):
            port = free_port()
            blocker = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                blocker.bind(("0.0.0.0", port))
                break
            except OSError:
                blocker.close()
        self.addCleanup(blocker.close)
        # a sanitizer that stops the server exits with 99, not with the 1 of a port that is taken
        asan = ":".join(o for o in (os.environ.get("ASAN_OPTIONS", ""), "exitcode=99") if o)
        s = Server("Live %s\n" % live, port=port, env={"ASAN_OPTIONS": asan})
        self.addCleanup(s.log.close)
        self.addCleanup(s.stop)
        return s

    def assertNoSanitizerReports(self, out):
        for report in ("runtime error", "AddressSanitizer", "ThreadSanitizer"):
            self.assertNotIn(report, out)

    def test_live_yes_warns_and_runs_without_live(self):
        s = self.start("yes").wait_ready()
        c = Client(s.port)
        self.addCleanup(c.close)
        self.assertTrue(c.login("anonymous:alice", caps=CAP_LIVE)[0])
        self.assertEqual(c.wait_live().get("flags"), LIVE_MAY_SEND)  # no UDP
        c.chat("MSG", "!live on")
        self.assertIn("[server] Live is off: the server could not open its UDP port",
                      [chat_fields(m[1])[2] for m in c.drain(0.5) if m[0] == CHAT])
        s.stop()  # flushes its output
        out = s.output()
        self.assertIn("warning: Live is off, could not open UDP port %d" % s.port, out)
        self.assertNotIn("Error listening", out)
        self.assertNoSanitizerReports(out)

    def test_live_require_fails_like_a_taken_port(self):
        s = self.start("require")
        try:
            code = s.proc.wait(5)
        except subprocess.TimeoutExpired:
            code = None
        out = s.output()
        self.assertEqual(code, 1, out)
        self.assertIn("Error listening on UDP port %d for Live" % s.port, out)
        self.assertNoSanitizerReports(out)


class ReloadTests(LiveTest):
    def set_live(self, value):
        with open(self.server.cfg) as f:
            cfg = f.read()
        with open(self.server.cfg, "w") as f:
            f.write(cfg.replace("Live yes", "Live " + value).replace("Live no", "Live " + value)
                    .replace("Live on", "Live " + value))
        self.server.proc.send_signal(signal.SIGHUP)  # the embedded runner restarts the server instead

    def fresh_client(self, pred, name):
        """Logs in again until the Live message (or None, if there is none) matches pred."""
        deadline = time.time() + 5
        while time.time() < deadline:
            try:
                c = Client(self.server.port)
                self.clients.append(c)
                ok, _ = c.login("anonymous:" + name, caps=CAP_LIVE)
                info = c.wait_live(timeout=0.5)
                if ok and pred(info):
                    return c
            except (OSError, ConnectionError, AssertionError):
                pass  # the embedded runner may have dropped it while restarting
            time.sleep(0.1)
        self.fail("the server's Live never changed")

    def test_live_follows_the_config(self):
        a = self.live_client("alice")
        self.bind(a)
        self.set_live("no")
        c = self.fresh_client(lambda i: i is None, "bob")
        c.udp_open()
        c.udp_send(udp_hello(a.live["peer_id"], a.live["key"]))
        self.assertIsNone(c.udp_recv(0.3))
        # the server closed its UDP port: this machine can take it
        deadline = time.time() + 5
        while True:
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                u.bind(("0.0.0.0", self.server.port))
                break
            except OSError:
                if time.time() > deadline:
                    self.fail("the server kept its UDP port")
                time.sleep(0.1)
            finally:
                u.close()

        self.set_live("on")
        c = self.fresh_client(lambda i: i is not None and i["flags"] == LIVE_ROOM_ON | LIVE_UDP_OPEN | LIVE_MAY_SEND, "carol")
        c.udp_open()
        self.bind(c)

    @unittest.skipIf(EMBEDDED, "the embedded runner restarts the server, which drops everyone")
    def test_turning_live_off_tells_live_clients(self):
        a = self.live_client("alice")
        self.bind(a)
        self.set_live("no")
        info = a.wait_live(timeout=5)
        self.assertEqual((info["flags"], info["peers"]), (0, []))
        a.hello(2)
        self.assertIsNone(a.udp_recv(0.3))


    @unittest.skipIf(EMBEDDED, "the embedded runner restarts the server, which drops everyone")
    def test_a_new_port_reaches_live_clients(self):
        a = self.live_client("alice")
        self.bind(a)
        new = free_port()
        with open(self.server.cfg) as f:
            cfg = f.read()
        with open(self.server.cfg, "w") as f:
            f.write(cfg.replace("Port %d\n" % self.server.port, "Port %d\n" % new, 1))
        self.server.proc.send_signal(signal.SIGHUP)
        # alice is still connected to the old port, which has no UDP any more
        info = a.wait_live(lambda i: i["udp_port"] == new, timeout=5)
        self.assertIsNotNone(info, "alice wasn't told the new UDP port")
        a.port = new  # where her UDP goes now
        self.bind(a)
        # who connects to the new port gets 0: the port it connected to
        c = Client(new)
        self.clients.append(c)
        self.assertTrue(c.login("anonymous:carol", caps=CAP_LIVE)[0])
        self.assertEqual(c.wait_live()["udp_port"], 0)


class LivePortMappingTests(PortMappingTestCase):
    """With Live, the router is asked to forward the UDP port too."""

    def test_upnp_forwards_udp_too(self):
        igd = FakeIGD(external_ip="203.0.113.7")
        self.addCleanup(igd.close)
        self.start(igd.url, extra_config="Live yes\n")
        port = self.server.port
        self.wait_for_log("UPnP: people can connect to 203.0.113.7:%d" % port)
        self.assertIn("[portmap] asking the router to forward TCP and UDP port %d to this machine" % port, self.log())
        self.assertEqual(sorted(igd.mappings), [(port, "TCP"), (port, "UDP")])
        m = igd.mappings[(port, "UDP")]
        self.assertEqual((m["NewInternalPort"], m["NewInternalClient"], m["NewLeaseDuration"]), (str(port), "127.0.0.1", "3600"))

        self.shut_down()
        self.assertEqual(igd.mappings, {})
        self.assertIn("UPnP: removed the forward for TCP and UDP port %d" % port, self.log())

    def test_nat_pmp_forwards_udp_too(self):
        try:
            natpmp = FakeNatPmp(external_ip="198.51.100.9")
        except OSError:
            self.skipTest("UDP port 5351 is in use")
        self.addCleanup(natpmp.close)
        self.start(closed_url(), extra_config="Live yes\n")
        port = self.server.port
        self.wait_for_log("NAT-PMP: people can connect to 198.51.100.9:%d" % port)
        self.assertEqual(sorted(natpmp.mappings), [(port, "TCP"), (port, "UDP")])
        self.assertEqual(natpmp.mappings[(port, "UDP")]["lifetime"], 3600)

        self.shut_down()
        self.assertEqual(natpmp.mappings, {})
        self.assertIn("NAT-PMP: removed the forward for TCP and UDP port %d" % port, self.log())

    def test_a_router_that_refuses_udp_still_forwards_tcp(self):
        igd = FakeIGD(external_ip="203.0.113.7", refuse_udp=True)
        self.addCleanup(igd.close)
        self.start(igd.url, extra_config="Live yes\n")
        port = self.server.port
        self.wait_for_log("people can connect to 203.0.113.7:%d" % port)
        self.assertEqual(list(igd.mappings), [(port, "TCP")])
        self.assertIn("warning: Live: the router did not forward UDP port %d" % port, self.log())
        # the plug-in that hosts the server shows the address from the last [portmap] line
        portmap_lines = [l for l in self.log().splitlines() if "[portmap]" in l]
        self.assertIn("people can connect to", portmap_lines[-1])

        self.shut_down()
        self.assertEqual(igd.mappings, {})

    def test_nat_pmp_router_that_refuses_udp(self):
        try:
            natpmp = FakeNatPmp(refuse_udp=True)
        except OSError:
            self.skipTest("UDP port 5351 is in use")
        self.addCleanup(natpmp.close)
        self.start(closed_url(), extra_config="Live yes\n")
        port = self.server.port
        self.wait_for_log("NAT-PMP: people can connect to")
        self.assertEqual(list(natpmp.mappings), [(port, "TCP")])
        self.wait_for_log("warning: Live: the router did not forward UDP port %d" % port)

    def test_turning_live_on_adds_udp(self):
        igd = FakeIGD()
        self.addCleanup(igd.close)
        self.start(igd.url)
        port = self.server.port
        self.wait_for_log("people can connect to")
        self.assertEqual(list(igd.mappings), [(port, "TCP")])
        with open(self.server.cfg, "a") as f:
            f.write("Live yes\n")
        self.server.proc.send_signal(signal.SIGHUP)
        deadline = time.time() + 10
        while time.time() < deadline and (port, "UDP") not in igd.mappings:
            time.sleep(0.05)
        self.assertEqual(sorted(igd.mappings), [(port, "TCP"), (port, "UDP")])

    def test_manual_forwarding_mentions_udp(self):
        self.start(closed_url(), natpmp_gateway="127.0.0.1", extra_config="Live yes\n")
        port = self.server.port
        self.wait_for_log("Forward TCP port %d on your router to this machine by hand, and UDP port %d for Live" % (port, port))
