#!/usr/bin/env python3
# Copyright (C) 2026 Andreas Baierl <ichgeh@imkreisrum.de>
# SPDX-License-Identifier: GPL-2.0-only
"""Fake SAT>IP UPnP device: answers M-SEARCH on UDP and serves the device description over HTTP."""
import socket, sys, threading, http.server

SSDP_PORT, HTTP_PORT, RTSP_PORT = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
CAPS = sys.argv[4] if len(sys.argv) > 4 else "DVBS2-2,DVBT2-1"

DESC = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0" xmlns:satip="urn:ses-com:satip">
  <device>
    <deviceType>urn:ses-com:device:SatIPServer:1</deviceType>
    <friendlyName>Fake &amp; Test SAT&gt;IP</friendlyName>
    <modelName>FakeBox</modelName>
    <UDN>uuid:11111111-2222-3333-4444-555555555555</UDN>
    <satip:X_SATIPCAP xmlns:satip="urn:ses-com:satip">%s</satip:X_SATIPCAP>
  </device>
</root>""" % CAPS

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def do_GET(self):
        body = DESC.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/xml")
        self.send_header("X-SATIP-RTSP-Port", str(RTSP_PORT))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *a):
        pass

threading.Thread(target=lambda: http.server.HTTPServer(("0.0.0.0", HTTP_PORT), H).serve_forever(), daemon=True).start()

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", SSDP_PORT))
RESP = ("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nLOCATION: http://127.0.0.1:%d/desc.xml\r\n"
        "SERVER: Linux/1.0 UPnP/1.1 fake/1.0\r\nST: urn:ses-com:device:SatIPServer:1\r\n"
        "USN: uuid:11111111-2222-3333-4444-555555555555::urn:ses-com:device:SatIPServer:1\r\n\r\n") % HTTP_PORT
while True:
    data, addr = s.recvfrom(2048)
    if b"M-SEARCH" in data:
        s.sendto(RESP.encode(), addr)
