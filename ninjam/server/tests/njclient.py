"""Minimal NINJAM protocol client, used by the server tests and the load benchmark."""
import hashlib
import socket
import struct
import time

AUTH_CHALLENGE = 0x00
AUTH_REPLY = 0x01
CONFIG_CHANGE = 0x02
USERINFO_CHANGE = 0x03
DOWNLOAD_BEGIN = 0x04
DOWNLOAD_WRITE = 0x05
CODEC_MASK = 0x06
LIVE = 0x07
CLIENT_AUTH_USER = 0x80
CLIENT_SET_USERMASK = 0x81
CLIENT_SET_CHANNEL_INFO = 0x82
CLIENT_UPLOAD_BEGIN = 0x83
CLIENT_UPLOAD_WRITE = 0x84
CHAT = 0xC0
KEEPALIVE = 0xFD

PROTO_VER = 0x00020000

# client_caps bits for codecs the client can decode besides Vorbis
CAP_OPUS = 0x100
CAP_FLAC = 0x200
# client_caps bit of clients that can play Live
CAP_LIVE = 0x10000

# Live (see MESSAGE_SERVER_LIVE in ../../mpb.h): flags, peer flags, datagram types
LIVE_ROOM_ON = 1
LIVE_UDP_OPEN = 2
LIVE_MAY_SEND = 4
LIVE_PEER_BOUND = 1
UDP_HELLO = 1  # and HELLO_ACK
UDP_DATA = 2


class Client:
    def __init__(self, port, host="127.0.0.1", timeout=5.0):
        self.host = host
        self.port = port
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.username = None
        self.inbox = []  # messages read while waiting for something else
        self.last_send = time.time()
        self.udp = None  # the Live socket, see udp_open()
        self.udp_connected = False
        self.cookie = 0  # the latest cookie the server sent to that socket, see udp_bind()
        self.live = None  # the latest Live state from the server, see wait_live()

    # framing
    def send(self, mtype, payload=b""):
        self.sock.sendall(struct.pack("<BI", mtype, len(payload)) + payload)
        self.last_send = time.time()

    def keepalive(self):
        # the server drops connections it hasn't heard from in 3 keepalive periods (9 s by default)
        if time.time() - self.last_send > 2.0:
            self.send(KEEPALIVE)

    def _read_one(self, timeout):
        self.sock.settimeout(timeout)
        while True:
            if len(self.buf) >= 5:
                mtype, size = struct.unpack("<BI", self.buf[:5])
                if len(self.buf) >= 5 + size:
                    payload = self.buf[5:5 + size]
                    self.buf = self.buf[5 + size:]
                    return mtype, payload
            data = self.sock.recv(65536)
            if not data:
                raise ConnectionError("closed by server")
            self.buf += data

    def recv(self, timeout=2.0, skip_keepalive=True):
        """Next message, or None on timeout."""
        if self.inbox:
            return self.inbox.pop(0)
        deadline = time.time() + timeout
        while True:
            self.keepalive()
            left = deadline - time.time()
            if left <= 0:
                return None
            try:
                m = self._read_one(min(left, 1.0))
            except socket.timeout:
                continue
            if skip_keepalive and m[0] == KEEPALIVE:
                continue
            return m

    def wait_for(self, pred, timeout=3.0):
        """Read until a message matches pred; keeps the others for later. Returns the match or None."""
        deadline = time.time() + timeout
        kept = []
        try:
            while time.time() < deadline:
                m = self.recv(deadline - time.time())
                if m is None:
                    return None
                if pred(m):
                    return m
                kept.append(m)
            return None
        finally:
            self.inbox = kept + self.inbox

    def drain(self, timeout=0.5):
        out = []
        while True:
            m = self.recv(timeout)
            if m is None:
                return out
            out.append(m)

    def is_closed(self, timeout=2.0):
        try:
            while self.recv(timeout) is not None:
                pass
            return False
        except (ConnectionError, OSError):
            return True

    # login
    def login(self, user, password="", agree_license=True, codecs=0, caps=0):
        """codecs and caps are client_caps bits (CAP_*), ORed together."""
        mtype, p = self.recv(5.0)
        assert mtype == AUTH_CHALLENGE, mtype
        challenge = p[:8]
        h = hashlib.sha1((user + ":" + password).encode()).digest()
        passhash = hashlib.sha1(h + challenge).digest()
        caps = (1 if agree_license else 0) | codecs | caps
        self.send(CLIENT_AUTH_USER, passhash + user.encode() + b"\0" + struct.pack("<II", caps, PROTO_VER))
        m = self.wait_for(lambda m: m[0] == AUTH_REPLY, 5.0)
        assert m is not None, "no auth reply"
        flag = m[1][0]
        rest = m[1][1:]
        name = rest.split(b"\0", 1)[0].decode(errors="replace")
        if flag & 1:
            self.username = name
        return bool(flag & 1), name

    # messages
    def set_channels(self, names):
        payload = struct.pack("<H", 4)
        for n in names:
            payload += n.encode() + b"\0" + struct.pack("<hbB", 0, 0, 0)
        self.send(CLIENT_SET_CHANNEL_INFO, payload)

    def subscribe(self, username, mask=0xFFFFFFFF):
        self.send(CLIENT_SET_USERMASK, username.encode() + b"\0" + struct.pack("<I", mask))

    def upload_begin(self, guid, chidx=0, fourcc=b"OGGv", estsize=0):
        self.send(CLIENT_UPLOAD_BEGIN, guid + struct.pack("<I", estsize) + fourcc + bytes([chidx]))

    def upload_write(self, guid, data, end=False):
        self.send(CLIENT_UPLOAD_WRITE, guid + bytes([1 if end else 0]) + data)

    def chat(self, *fields, terminate_last=True):
        payload = b"\0".join(f.encode() for f in fields)
        if terminate_last:
            payload += b"\0"
        self.send(CHAT, payload)

    # Live
    def wait_live(self, pred=None, timeout=3.0):
        """The next Live message (for which pred(info) holds), as live_info() reads it; also kept in self.live.
        Older Live messages that didn't match are dropped: the new one replaces them."""
        deadline = time.time() + timeout
        kept = []
        try:
            while time.time() < deadline:
                m = self.recv(deadline - time.time())
                if m is None:
                    return None
                if m[0] == LIVE and (pred is None or pred(live_info(m))):
                    kept = [k for k in kept if k[0] != LIVE]
                    self.live = live_info(m)
                    return self.live
                kept.append(m)
            return None
        finally:
            self.inbox = kept + self.inbox

    def udp_open(self, bind_host="127.0.0.1", connect=False):
        """A UDP socket for Live, from bind_host (any 127.x address is this machine). With connect, it is
        connect()ed to the server's address and port, as the plug-in's is: it then gets only what comes from there."""
        if self.udp:
            self.udp.close()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind((bind_host, 0))
        self.udp_connected = connect
        self.cookie = 0
        if connect:
            self.udp.connect((self.host, self.port))
        return self.udp

    def udp_send(self, data, port=None):
        """Sends a datagram to the server's UDP port (the TCP port unless given)."""
        if not self.udp:
            self.udp_open()
        if self.udp_connected and port in (None, self.port):
            self.udp.send(data)
        else:
            self.udp.sendto(data, (self.host, port or self.port))

    def udp_recv(self, timeout=1.0):
        """The next datagram from the server, or None. Keeps the TCP connection alive meanwhile."""
        deadline = time.time() + timeout
        while True:
            self.keepalive()
            left = deadline - time.time()
            if left <= 0:
                return None
            self.udp.settimeout(min(left, 0.05))
            try:
                data, _ = self.udp.recvfrom(4096)
                return data
            except socket.timeout:
                continue

    def udp_drain(self, timeout=0.3):
        """Every datagram that arrives until none has for timeout seconds."""
        out = []
        while True:
            d = self.udp_recv(timeout)
            if d is None:
                return out
            out.append(d)

    def hello(self, timestamp=0, peer_id=None, key=None, cookie=None):
        """Sends a HELLO with this client's Live id, key and latest cookie (or others, to test the server)."""
        self.udp_send(udp_hello(self.live["peer_id"] if peer_id is None else peer_id,
                                self.live["key"] if key is None else key, timestamp,
                                self.cookie if cookie is None else cookie))

    def hello_ack(self, timestamp, timeout=2.0):
        """Waits for the HELLO_ACK that answers the HELLO with this timestamp (skipping anything else), keeps its
        cookie, and returns it; None if it doesn't come."""
        deadline = time.time() + timeout
        while True:
            d = self.udp_recv(deadline - time.time())
            if d is None:
                return None
            if d[:4] == b"NL\x01\x01" and len(d) == 24 and udp_ack(d)[0] == timestamp:
                self.cookie = udp_ack(d)[1]
                return d

    def udp_bind(self, timeout=2.0):
        """Says HELLO, then again with the cookie the server answers with, as a Live client does: after that,
        the server sends Live audio to this socket. Returns the last HELLO_ACK, or None if one didn't come."""
        ack = None
        for timestamp in (0x6e6a01, 0x6e6a02):
            self.hello(timestamp)
            ack = self.hello_ack(timestamp, timeout)
            if ack is None:
                return None
        return ack

    def send_live(self, payload, dest=0, peer_id=None, key=None):
        """Sends audio (any bytes) to one player (dest = its peer id) or to everyone else in the room (0)."""
        self.udp_send(udp_data(self.live["peer_id"] if peer_id is None else peer_id,
                               self.live["key"] if key is None else key, dest, payload))

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
        if self.udp:
            self.udp.close()
            self.udp = None


def codec_mask(msg):
    """The codec bits of a CODEC_MASK message."""
    return struct.unpack("<I", msg[1][:4])[0]


def mask_opus_kbps(msg):
    """The room's Opus bitrate in a CODEC_MASK message, 0 when it has none."""
    return struct.unpack("<I", msg[1][4:8])[0] if len(msg[1]) >= 8 else 0


def live_info(msg):
    """The fields of a LIVE message, as a dict; peers is a list of (peer_id, peer_flags, name)."""
    p = msg[1]
    version, fixed_len, flags = p[0], p[1], p[2]
    udp_port, kbps, peer_id, key, frame, max_peers = struct.unpack("<HHIQHH", p[4:24])
    peers = []
    rest = p[fixed_len:]
    while rest:
        pid, pflags = struct.unpack("<IB", rest[:5])
        name, rest = rest[5:].split(b"\0", 1)
        peers.append((pid, pflags, name.decode(errors="replace")))
    return {"version": version, "fixed_len": fixed_len, "flags": flags, "udp_port": udp_port, "kbps": kbps,
            "peer_id": peer_id, "key": key, "frame": frame, "max_peers": max_peers, "peers": peers}


def udp_hello(peer_id, key, timestamp=0, cookie=0):
    """A Live HELLO datagram (40 bytes)."""
    return struct.pack("<2sBBIQIQQ4x", b"NL", 1, UDP_HELLO, peer_id, key, 0, timestamp, cookie)


def udp_ack(data):
    """(timestamp, cookie) of a HELLO_ACK."""
    return struct.unpack("<QQ", data[8:24])


def udp_data(peer_id, key, dest, payload):
    """A Live DATA datagram: the 20-byte header, then the audio."""
    return struct.pack("<2sBBIQI", b"NL", 1, UDP_DATA, peer_id, key, dest) + payload


def udp_parse(data):
    """(type, from_id, rest) of a datagram from the server."""
    assert data[:3] == b"NL\x01", data[:3]
    return data[3], struct.unpack("<I", data[4:8])[0], data[8:]


def chat_fields(payload):
    return [f.decode(errors="replace") for f in payload.split(b"\0")]
