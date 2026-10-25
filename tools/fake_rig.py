#!/usr/bin/env python3
"""fake_rig.py — the sandbox stand-in for the CloudMatch + NVST rig pair.

Serves the server side of docs/RTSP.md over PLAIN sockets (loopback):
  * an HTTP endpoint answering POST /v2/session and GET /v2/session/{id}
    with a ready session fixture (connectionInfo usage 16 -> this rig's
    WS port, usage 2 -> the media port), and
  * a WebSocket endpoint at /rtsp speaking the RTSP sequence
    (OPTIONS -> DESCRIBE -> SETUP -> ANNOUNCE -> PLAY) with shapes
    transcribed from the reference's tests.

This is the round-XXI E2E counterpart of tools/sandbox_session_e2e.c.
Production crypto is NEVER bypassed by this file: it exists so the
framing/state machines run against a real second implementation over real
sockets in the sandbox; the TLS transport itself is proven by T12/T10 on
the target, and the first REAL rig is the true test (R32's rule).

usage:  python3 tools/fake_rig.py [--http-port 0] [--ws-port 0]
        [--media-port 0] [--ready-file PATH]
With port 0 the OS assigns; the chosen ports are written as one JSON line
to stdout (and to --ready-file when given):
  {"mode":"fake-rig","http_port":P,"ws_port":P,"media_port":P,"pid":N}
The rig runs until killed (SIGINT/SIGTERM) or --exit-after-play N (it
exits after N completed PLAY exchanges; the E2E harness uses 1).
"""
import argparse
import base64
import hashlib
import json
import os
import signal
import socket
import struct
import sys
import threading

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

DESCRIBE_SDP = (
    "v=0\r\n"
    "o=- 0 0 IN IP4 127.0.0.1\r\n"
    "s=NVIDIA Streaming Server\r\n"
    "t=0 0\r\n"
    "a=x-nv-general.pingVersion:6\r\n"
    "a=x-nv-general.iceUserNameFragmentV2:RIGUFRAG\r\n"
    "a=x-nv-general.icePasswordV2:RIGPASSWORD\r\n"
    "a=x-nv-general.dtlsFingerprintV2:RIG:FINGER:PRINT\r\n"
    "a=x-nv-general.nativeRtcOnBundlePort:1\r\n"
    "a=x-nv-general.rtcpOnSctp:1\r\n"
    "a=x-nv-general.enableUnifiedSocket:1\r\n"
    "a=x-nv-runtime.encryptionKey:"
    "000102030405060708090A0B0C0D0E0F"
    "000102030405060708090A0B0C0D0E0F\r\n"
    "a=x-nv-runtime.encryptionKeyId:1194884\r\n"
    "m=video 5004 RTP/AVP 96\r\n"
    "a=control:streamid=video/0\r\n"
)

STOP = threading.Event()


def ws_accept(key):
    return base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode()).digest()).decode()


def ws_frame(payload, opcode=0x1):
    """Server frame: unmasked, per RFC 6455."""
    b = payload.encode() if isinstance(payload, str) else payload
    hdr = bytes([0x80 | opcode])
    n = len(b)
    if n < 126:
        hdr += bytes([n])
    elif n <= 0xFFFF:
        hdr += bytes([126]) + struct.pack(">H", n)
    else:
        hdr += bytes([127]) + struct.pack(">Q", n)
    return hdr + b


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def ws_read_message(sock):
    """One client message (fragments joined); returns (opcode, bytes)."""
    data = b""
    while True:
        hdr = recv_exact(sock, 2)
        if hdr is None:
            return None, None
        fin = hdr[0] & 0x80
        op = hdr[0] & 0x0F
        masked = hdr[1] & 0x80
        ln = hdr[1] & 0x7F
        if ln == 126:
            ln = struct.unpack(">H", recv_exact(sock, 2))[0]
        elif ln == 127:
            ln = struct.unpack(">Q", recv_exact(sock, 8))[0]
        mask = recv_exact(sock, 4) if masked else None
        payload = recv_exact(sock, ln) if ln else b""
        if payload is None:
            return None, None
        if masked:
            payload = bytes(c ^ mask[i & 3] for i, c in enumerate(payload))
        if op == 0x8:
            return 0x8, payload
        if op == 0x9:                       # ping -> pong
            sock.sendall(ws_frame(payload, 0xA))
            continue
        if op == 0xA:
            continue
        data += payload
        if fin:
            return op, data


def http_read_request(conn):
    """Read one HTTP request; returns (head_lines, body_bytes)."""
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            return None, None
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    content_length = 0
    for line in lines[1:]:
        if line.lower().startswith("content-length:"):
            content_length = int(line.split(":", 1)[1].strip())
    while len(rest) < content_length:
        chunk = conn.recv(4096)
        if not chunk:
            break
        rest += chunk
    return lines, rest


def http_respond(conn, status, body_obj):
    body = json.dumps(body_obj).encode()
    conn.sendall(
        ("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
         "Content-Length: %d\r\nConnection: close\r\n\r\n"
         % (status, "OK" if status == 200 else "Error", len(body))).encode()
        + body)


class Rig:
    def __init__(self, args):
        self.args = args
        self.session_id = "FAKE-SESS-0001"
        self.plays = 0
        self.announce_seen = None
        self.http = socket.socket()
        self.http.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.http.bind(("127.0.0.1", args.http_port))
        self.http.listen(8)
        self.ws = socket.socket()
        self.ws.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ws.bind(("127.0.0.1", args.ws_port))
        self.ws.listen(8)
        self.media = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.media.bind(("127.0.0.1", args.media_port))
        self.http_port = self.http.getsockname()[1]
        self.ws_port = self.ws.getsockname()[1]
        self.media_port = self.media.getsockname()[1]

    def session_payload(self):
        return {
            "requestStatus": {"statusCode": 1},
            "session": {
                "sessionId": self.session_id,
                "status": 2,
                "sessionControlInfo": {"ip": "127.0.0.1", "port": 443},
                "connectionInfo": [
                    {"usage": 16, "ip": "127.0.0.1", "port": self.ws_port},
                    {"usage": 2, "ip": "127.0.0.1", "port": self.media_port},
                ],
                "seatSetupInfo": {"seatSetupStep": 4},
                "subSessionId": "sub-fake",
            },
        }

    def serve_http(self):
        self.http.settimeout(0.5)
        while not STOP.is_set():
            try:
                conn, _ = self.http.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with conn:
                lines, body = http_read_request(conn)
                if not lines:
                    continue
                req = lines[0]
                if req.startswith("POST /v2/session "):
                    assert b"sessionRequestData" in body, "create body"
                    http_respond(conn, 200, self.session_payload())
                elif req.startswith("GET /v2/session/"):
                    http_respond(conn, 200, self.session_payload())
                else:
                    http_respond(conn, 404, {"error": "unknown"})

    def rtsp_response(self, method, cseq, extra_headers=("",), body=""):
        head = "RTSP/1.0 200 OK\r\nCSeq: %s\r\n" % cseq
        for h in extra_headers:
            if h:
                head += h + "\r\n"
        if body:
            head += "Content-Length: %d\r\n" % len(body.encode())
        return head + "\r\n" + body

    def serve_ws(self):
        self.ws.settimeout(0.5)
        while not STOP.is_set():
            try:
                conn, _ = self.ws.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            threading.Thread(target=self.ws_client, args=(conn,),
                             daemon=True).start()

    def ws_client(self, conn):
        with conn:
            lines, _ = http_read_request(conn)
            if not lines:
                return
            key = None
            session_hdr = None
            for line in lines[1:]:
                low = line.lower()
                if low.startswith("sec-websocket-key:"):
                    key = line.split(":", 1)[1].strip()
                if low.startswith("x-nv-sessionid:"):
                    session_hdr = line.split(":", 1)[1].strip()
            if key is None or session_hdr != self.session_id:
                conn.sendall(b"HTTP/1.1 403 Forbidden\r\n\r\n")
                return
            conn.sendall((
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Accept: %s\r\n\r\n" % ws_accept(key)).encode())
            while not STOP.is_set():
                op, msg = ws_read_message(conn)
                if op is None or op == 0x8:
                    return
                text = msg.decode("utf-8", "replace")
                first = text.split(" ", 1)[0]
                cseq = "0"
                for line in text.split("\r\n"):
                    if line.lower().startswith("cseq:"):
                        cseq = line.split(":", 1)[1].strip()
                if first == "OPTIONS":
                    conn.sendall(ws_frame(self.rtsp_response(
                        first, cseq,
                        ["Public: OPTIONS, DESCRIBE, SETUP, ANNOUNCE, "
                         "PLAY, TEARDOWN"])))
                elif first == "DESCRIBE":
                    conn.sendall(ws_frame(self.rtsp_response(
                        first, cseq,
                        ["Session: %s" % self.session_id,
                         "Content-Type: application/sdp"], DESCRIBE_SDP)))
                elif first == "SETUP":
                    conn.sendall(ws_frame(self.rtsp_response(
                        first, cseq,
                        ["Session: %s" % self.session_id,
                         "Transport: RTP/AVP/UDP;unicast;source=127.0.0.1;"
                         "X-GS-ServerPort=%d-%d" % (self.media_port,
                                                     self.media_port),
                         "x-nv-ping: 6",
                         "x-nv-ping-payload: FAKEPING"])))
                elif first == "ANNOUNCE":
                    self.announce_seen = text
                    conn.sendall(ws_frame(self.rtsp_response(
                        first, cseq, ["Session: %s" % self.session_id])))
                elif first == "PLAY":
                    conn.sendall(ws_frame(self.rtsp_response(
                        first, cseq, ["Session: %s" % self.session_id])))
                    self.plays += 1
                    if self.args.exit_after_play and \
                            self.plays >= self.args.exit_after_play:
                        STOP.set()
                        return
                elif first == "TEARDOWN":
                    conn.sendall(ws_frame(self.rtsp_response(first, cseq)))
                    return
                else:
                    conn.sendall(ws_frame(
                        "RTSP/1.0 501 Not Implemented\r\nCSeq: %s\r\n\r\n"
                        % cseq))

    def run(self):
        info = {"mode": "fake-rig", "http_port": self.http_port,
                "ws_port": self.ws_port, "media_port": self.media_port,
                "pid": os.getpid()}
        line = json.dumps(info)
        print(line, flush=True)
        if self.args.ready_file:
            with open(self.args.ready_file, "w") as f:
                f.write(line + "\n")
        t_http = threading.Thread(target=self.serve_http, daemon=True)
        t_ws = threading.Thread(target=self.serve_ws, daemon=True)
        t_http.start()
        t_ws.start()
        while not STOP.is_set():
            STOP.wait(0.2)
        # emit the evidence line (the harness reads stdout after exit)
        print(json.dumps({"mode": "fake-rig", "event": "done",
                          "plays": self.plays,
                          "announce_bytes":
                              len(self.announce_seen or ""),
                          "announce_has_key":
                              1 if (self.announce_seen and
                                    "encryptionKey:000102030405060708090A0B"
                                    "0C0D0E0F" in self.announce_seen)
                              else 0}),
              flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--http-port", type=int, default=0)
    ap.add_argument("--ws-port", type=int, default=0)
    ap.add_argument("--media-port", type=int, default=0)
    ap.add_argument("--ready-file", default=None)
    ap.add_argument("--exit-after-play", type=int, default=0)
    args = ap.parse_args()
    rig = Rig(args)

    def stop(signum, frame):
                STOP.set()
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    rig.run()


if __name__ == "__main__":
    main()
