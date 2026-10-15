"""A fake home router for the port mapping tests.

FakeIGD answers UPnP-IGD requests (device description over HTTP, SOAP control
for AddPortMapping / DeletePortMapping / GetExternalIPAddress).
FakeNatPmp answers NAT-PMP (RFC 6886) on 127.0.0.1:5351, the fixed NAT-PMP port.
Both record the mappings they hold in `mappings`, keyed by external port.
"""
import http.server
import re
import socket
import struct
import threading
import time

WANIP = "urn:schemas-upnp-org:service:WANIPConnection:1"

ROOT_DESC = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<device>
<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<friendlyName>Fake router</friendlyName>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<serviceList><service>
<serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>
<serviceId>urn:upnp-org:serviceId:WANCommonIFC1</serviceId>
<controlURL>/ctl/CmnIfCfg</controlURL><eventSubURL>/evt/CmnIfCfg</eventSubURL><SCPDURL>/WANCfg.xml</SCPDURL>
</service></serviceList>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<serviceList><service>
<serviceType>%s</serviceType>
<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
<controlURL>/ctl/IPConn</controlURL><eventSubURL>/evt/IPConn</eventSubURL><SCPDURL>/WANIPCn.xml</SCPDURL>
</service></serviceList>
</device></deviceList>
</device></deviceList>
</device>
</root>
""" % WANIP

ENVELOPE = ('<?xml version="1.0"?>\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>%s</s:Body></s:Envelope>')


class FakeIGD:
    def __init__(self, external_ip="203.0.113.7", only_permanent_leases=False):
        self.external_ip = external_ip
        self.only_permanent_leases = only_permanent_leases
        self.mappings = {}
        self.lock = threading.Lock()
        igd = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def send(self, code, body):
                data = body.encode()
                self.send_response(code)
                self.send_header("Content-Type", 'text/xml; charset="utf-8"')
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                if self.path == "/rootDesc.xml":
                    self.send(200, ROOT_DESC)
                else:
                    self.send(404, "")

            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
                action = self.headers.get("SOAPAction", "").strip('"').split("#")[-1]
                args = dict(re.findall(r"<(New\w+)>([^<]*)</\1>", body))
                code, result = igd.handle(action, args)
                if code:
                    self.send(500, ENVELOPE % (
                        '<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>'
                        '<UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>%d</errorCode>'
                        '<errorDescription>error</errorDescription></UPnPError></detail></s:Fault>' % code))
                else:
                    out = "".join("<%s>%s</%s>" % (k, v, k) for k, v in result.items())
                    self.send(200, ENVELOPE % ('<u:%sResponse xmlns:u="%s">%s</u:%sResponse>' % (action, WANIP, out, action)))

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = "http://127.0.0.1:%d/rootDesc.xml" % self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def handle(self, action, args):
        with self.lock:
            if action == "AddPortMapping":
                if self.only_permanent_leases and args.get("NewLeaseDuration") != "0":
                    return 725, None  # OnlyPermanentLeasesSupported
                self.mappings[int(args["NewExternalPort"])] = args
                return 0, {}
            if action == "DeletePortMapping":
                if self.mappings.pop(int(args["NewExternalPort"]), None) is None:
                    return 714, None  # NoSuchEntryInArray
                return 0, {}
            if action == "GetExternalIPAddress":
                return 0, {"NewExternalIPAddress": self.external_ip}
            return 401, None  # Invalid Action

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


class FakeNatPmp:
    PORT = 5351

    def __init__(self, external_ip="198.51.100.9"):
        self.external_ip = external_ip
        self.mappings = {}
        self.started = time.time()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", self.PORT))  # OSError if something else has the port
        self.sock.settimeout(0.2)
        self.running = True
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while self.running:
            try:
                req, addr = self.sock.recvfrom(64)
            except socket.timeout:
                continue
            except OSError:
                return
            epoch = int(time.time() - self.started)
            if len(req) == 2 and req[0] == 0 and req[1] == 0:
                self.sock.sendto(struct.pack("!BBHI4s", 0, 128, 0, epoch, socket.inet_aton(self.external_ip)), addr)
            elif len(req) == 12 and req[0] == 0 and req[1] in (1, 2):
                _, op, _, internal, external, lifetime = struct.unpack("!BBHHHI", req)
                external = external or internal
                if lifetime:
                    self.mappings[external] = {"protocol": "TCP" if op == 2 else "UDP", "internal": internal,
                                               "lifetime": lifetime, "client": addr[0]}
                else:
                    self.mappings.pop(external, None)
                self.sock.sendto(struct.pack("!BBHIHHI", 0, 128 + op, 0, epoch, internal, external, lifetime), addr)

    def close(self):
        self.running = False
        self.thread.join()
        self.sock.close()
