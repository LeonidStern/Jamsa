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
CLIENT_AUTH_USER = 0x80
CLIENT_SET_USERMASK = 0x81
CLIENT_SET_CHANNEL_INFO = 0x82
CLIENT_UPLOAD_BEGIN = 0x83
CLIENT_UPLOAD_WRITE = 0x84
CHAT = 0xC0
KEEPALIVE = 0xFD

PROTO_VER = 0x00020000


class Client:
    def __init__(self, port, host="127.0.0.1", timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.username = None
        self.inbox = []  # messages read while waiting for something else
        self.last_send = time.time()

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
    def login(self, user, password="", agree_license=True):
        mtype, p = self.recv(5.0)
        assert mtype == AUTH_CHALLENGE, mtype
        challenge = p[:8]
        h = hashlib.sha1((user + ":" + password).encode()).digest()
        passhash = hashlib.sha1(h + challenge).digest()
        caps = 1 if agree_license else 0
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

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def chat_fields(payload):
    return [f.decode(errors="replace") for f in payload.split(b"\0")]
