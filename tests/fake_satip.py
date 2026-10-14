#!/usr/bin/env python3
"""Tiny fake SAT>IP server for testing vtsatip (RTSP over TCP + RTP/RTCP over UDP)."""
import socket, threading, time, struct, sys, re, os, signal

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8554
LOG = sys.argv[2] if len(sys.argv) > 2 else "server.log"
TIMEOUT = int(sys.argv[3]) if len(sys.argv) > 3 else 30
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
        if not st.started or not st.client or not st.pids or not st.freq:
            continue
        data = st.packets()
        # at most 7 TS packets per datagram
        for i in range(0, len(data), 188 * 7):
            chunk = data[i:i + 188 * 7]
            st.seq = (st.seq + 1) & 0xffff
            hdr = struct.pack(">BBHII", 0x80, 33, st.seq, int(time.time() * 90000) & 0xffffffff, 1)
            sock.sendto(hdr + chunk, (st.client[0], st.client[1]))
        if time.time() - last_rtcp > 0.3:
            last_rtcp = time.time()
            s = b"ver=1.0;src=1;tuner=1,224,1,15,%d,h,dvbs2,8psk,off,0.35,22000,23;pids=0" % st.freq
            s += b"\x00" * ((4 - len(s) % 4) % 4)
            body = b"SES1" + struct.pack(">HH", 0, len(s)) + s
            pkt = struct.pack(">BBHI", 0x80, 204, (len(body) + 4) // 4 + 0, 1) + body
            # length field = words - 1 including header
            words = (len(pkt)) // 4
            pkt = struct.pack(">BBHI", 0x80, 204, words - 1, 1) + body
            sock.sendto(pkt, (st.client[0], st.client[1] + 1))

def handle(conn, addr):
    st = Stream()
    usock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
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
                if method == "SETUP":
                    m = re.search(r"client_port=(\d+)-(\d+)", hdrs.get("transport", ""))
                    st.client = (addr[0], int(m.group(1)))
                    st.freq = int(q.get("freq", "0"))
                    extra = "Session: ABCD1234;timeout=%d\r\ncom.ses.streamID: 7\r\n" % TIMEOUT
                elif method == "PLAY":
                    if "addpids" in q and "999" in q["addpids"].split(","):
                        log("DROP connection (simulated failure)")
                        conn.close()
                        return
                    if "freq" in q:
                        st.freq = int(q["freq"])
                    if "pids" in q:
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
                conn.sendall(resp.encode())
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
