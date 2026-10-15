#!/usr/bin/env python3
"""Tiny fake SAT>IP server for testing vtsatip (RTSP over TCP + RTP/RTCP over UDP)."""
import socket, threading, time, struct, sys, re, os, signal

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8554
LOG = sys.argv[2] if len(sys.argv) > 2 else "server.log"
TIMEOUT = int(sys.argv[3]) if len(sys.argv) > 3 else 30
FLAGS = set(sys.argv[4:])   # zeros nodelta notune busy mcast nomcast refusemcast tcp refusetcp
MCAST_GROUP, MCAST_PORT = "239.255.77.7", int(os.environ.get("FAKE_MCAST_PORT", "5004"))
lock = threading.Lock()

def log(msg):
    with lock:
        with open(LOG, "a") as f:
            f.write("%.3f %s\n" % (time.time(), msg))

class Stream:
    def __init__(self):
        self.freq = 0
        self.pids = set()
        self.client = None
        self.alive = True
        self.seq = 0
        self.cc = {}
        self.started = False
        self.mcast = False
        self.tcp = False
        self.conn = None
        self.sendlock = threading.Lock()

    def send(self, data):
        with self.sendlock:
            self.conn.sendall(data)

    def packets(self):
        out = b""
        for pid in sorted(self.pids):
            cc = self.cc.get(pid, 0)
            self.cc[pid] = (cc + 1) & 15
            out += bytes([0x47, (pid >> 8) & 0x1f, pid & 0xff, 0x10 | cc]) + struct.pack(">I", self.freq) + b"\xff" * 180
        return out

def sender(st, sock):
    last_rtcp = 0
    while st.alive:
        time.sleep(0.005)
        if not st.started or not (st.client or st.mcast or st.tcp) or not st.pids or not st.freq:
            continue
        data = st.packets()
        # at most 7 TS packets per datagram
        for i in range(0, len(data), 188 * 7):
            chunk = data[i:i + 188 * 7]
            st.seq = (st.seq + 1) & 0xffff
            hdr = struct.pack(">BBHII", 0x80, 33, st.seq, int(time.time() * 90000) & 0xffffffff, 1)
            if "lose" in FLAGS and st.seq % 10 == 0:
                continue  # simulate a lost datagram
            if st.tcp:
                st.send(b"$" + bytes([0]) + struct.pack(">H", len(hdr + chunk)) + hdr + chunk)
            else:
                sock.sendto(hdr + chunk, (MCAST_GROUP, MCAST_PORT) if st.mcast else (st.client[0], st.client[1]))
        if time.time() - last_rtcp > 0.3:
            last_rtcp = time.time()
            s = b"ver=1.0;src=1;tuner=1,224,1,15,%d,h,dvbs2,8psk,off,0.35,22000,23;pids=0" % st.freq
            s += b"\x00" * ((4 - len(s) % 4) % 4)
            body = b"SES1" + struct.pack(">HH", 0, len(s)) + s
            pkt = struct.pack(">BBHI", 0x80, 204, (len(body) + 4) // 4 + 0, 1) + body
            # length field = words - 1 including header
            words = (len(pkt)) // 4
            pkt = struct.pack(">BBHI", 0x80, 204, words - 1, 1) + body
            if st.tcp:
                st.send(b"$" + bytes([1]) + struct.pack(">H", len(pkt)) + pkt)
            else:
                sock.sendto(pkt, (MCAST_GROUP, MCAST_PORT + 1) if st.mcast else (st.client[0], st.client[1] + 1))

def handle(conn, addr):
    st = Stream()
    st.conn = conn
    usock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    usock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton("127.0.0.1"))
    usock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
    t = threading.Thread(target=sender, args=(st, usock), daemon=True)
    t.start()
    buf = b""
    try:
        while True:
            d = conn.recv(4096)
            if not d:
                break
            buf += d
            while b"\r\n\r\n" in buf:
                req, buf = buf.split(b"\r\n\r\n", 1)
                lines = req.decode().split("\r\n")
                method, url, _ = lines[0].split(" ")
                hdrs = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
                query = url.split("?", 1)[1] if "?" in url else ""
                log("REQ %s %s session=%s" % (method, query, "yes" if "session" in hdrs else "no"))
                cseq = hdrs.get("cseq", "0")
                q = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
                extra = ""
                def refuse(code, text):
                    log("REFUSED %s %d" % (method, code))
                    st.send(("RTSP/1.0 %d %s\r\nCSeq: %s\r\n\r\n" % (code, text, cseq)).encode())
                if "busy" in FLAGS and method == "SETUP":
                    refuse(503, "Service Unavailable")
                    continue
                if "zeros" in FLAGS and method != "SETUP" and "session" in hdrs and hdrs["session"].split(";")[0] != "ABCD1234":
                    refuse(454, "Session Not Found")
                    continue
                if "nodelta" in FLAGS and method == "PLAY" and ("addpids" in q or "delpids" in q):
                    refuse(400, "Bad Request")
                    continue
                if "notune" in FLAGS and method == "PLAY" and "freq" in q and st.started:
                    refuse(454, "Session Not Found")
                    continue
                if method == "SETUP":
                    want_mc = "multicast" in hdrs.get("transport", "")
                    if want_mc and "refusemcast" in FLAGS:
                        refuse(461, "Unsupported Transport")
                        continue
                    want_tcp = "RTP/AVP/TCP" in hdrs.get("transport", "")
                    if want_tcp and "refusetcp" in FLAGS:
                        refuse(461, "Unsupported Transport")
                        continue
                    st.tcp = want_tcp and "tcp" in FLAGS
                    m = re.search(r"client_port=(\d+)-(\d+)", hdrs.get("transport", ""))
                    st.client = (addr[0], int(m.group(1))) if m else None
                    st.mcast = want_mc and "nomcast" not in FLAGS
                    st.freq = int(q.get("freq", "0"))
                    extra = "Session: %s;timeout=%d\r\ncom.ses.streamID: 7\r\n" % ("00ABCD1234" if "zeros" in FLAGS else "ABCD1234", TIMEOUT)
                    if st.tcp:
                        extra += "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                    elif want_tcp:
                        extra += "Transport: RTP/AVP;unicast;server_port=5000-5001\r\n"
                    elif st.mcast:
                        extra += "Transport: RTP/AVP;multicast;destination=%s;port=%d-%d;ttl=2\r\n" % (MCAST_GROUP, MCAST_PORT, MCAST_PORT + 1)
                    elif want_mc:
                        extra += "Transport: RTP/AVP;unicast;server_port=5000-5001\r\n"
                elif method == "PLAY":
                    if "addpids" in q and "999" in q["addpids"].split(","):
                        log("DROP connection (simulated failure)")
                        conn.close()
                        return
                    if "freq" in q:
                        st.freq = int(q["freq"])
                    if "pids" in q:
                        if q["pids"] == "all":
                            st.pids = {0, 16, 17, 18, 100, 200}  # "everything" of the fake transponder
                        else:
                            st.pids = set() if q["pids"] == "none" else set(int(x) for x in q["pids"].split(","))
                    if "addpids" in q:
                        st.pids |= set(int(x) for x in q["addpids"].split(","))
                    if "delpids" in q:
                        st.pids -= set(int(x) for x in q["delpids"].split(","))
                    st.started = True
                    extra = "Session: ABCD1234\r\n"
                elif method == "OPTIONS":
                    extra = "Public: OPTIONS, SETUP, PLAY, TEARDOWN\r\n"
                elif method == "TEARDOWN":
                    st.started = False
                resp = "RTSP/1.0 200 OK\r\nCSeq: %s\r\n%s\r\n" % (cseq, extra)
                st.send(resp.encode())
    except Exception as e:
        log("ERR %r" % e)
    finally:
        st.alive = False
        try:
            conn.close()
        except Exception:
            pass
        log("CONN closed")

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(5)
log("SERVER up")
while True:
    c, a = srv.accept()
    log("CONN opened")
    threading.Thread(target=handle, args=(c, a), daemon=True).start()
