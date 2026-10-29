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
import hmac
import json
import os
import signal
import socket
import struct
import zlib
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
    # round XXIV: real DESCRIBE SDPs routinely exceed 8 KiB (R7 run-5
    # died rc=-2 on the old per-message scratch); pad with inert
    # session-level attributes BEFORE the m= line so every parser path
    # (sdp_attribute scans, media_control, negotiate) sees a >8 KiB body.
    + "".join("a=x-kn-pad-%04d:%s\r\n" % (i, "P" * 60) for i in range(150))
    + "m=video 5004 RTP/AVP 96\r\n"
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


STUN_MAGIC = 0x2112A442
STUN_FP_XOR = 0x5354554E


def stun_build(msg_type, tid, key, attr_type, attr):
    """Independent Python mirror of the C kn_sock_stun_build (RFC 5389
    short-term-credential shape: USERNAME/XOR-MAPPED + MESSAGE-INTEGRITY
    + FINGERPRINT)."""
    p = struct.pack(">HH", msg_type, 0) + struct.pack(">I", STUN_MAGIC) + tid
    p += struct.pack(">HH", attr_type, len(attr)) + attr
    p += b"\x00" * ((4 - len(attr) % 4) % 4)
    p = p[:2] + struct.pack(">H", len(p) - 20 + 24) + p[4:]
    p += struct.pack(">HH", 0x0008, 20) + hmac.new(key, p,
                                                   hashlib.sha1).digest()
    p = p[:2] + struct.pack(">H", len(p) - 20 + 8) + p[4:]
    crc = (zlib.crc32(p) ^ STUN_FP_XOR) & 0xFFFFFFFF
    p += struct.pack(">HH", 0x8028, 4) + struct.pack(">I", crc)
    return p


def stun_check(pkt, mi_key):
    """Validate structure + FINGERPRINT + MESSAGE-INTEGRITY. Returns
    (msg_type, tid, attrs) or None."""
    if len(pkt) < 20:
        return None
    mtype, mlen = struct.unpack(">HH", pkt[:4])
    if struct.unpack(">I", pkt[4:8])[0] != STUN_MAGIC:
        return None
    if 20 + mlen != len(pkt):
        return None
    tid = pkt[8:20]
    attrs, mi_off, fp_off, off = {}, None, None, 20
    while off + 4 <= len(pkt):
        at, al = struct.unpack(">HH", pkt[off:off + 4])
        if off + 4 + al > len(pkt):
            return None
        attrs[at] = pkt[off + 4:off + 4 + al]
        if at == 0x0008:
            mi_off = off
        if at == 0x8028:
            fp_off = off
        off += 4 + al + ((4 - al % 4) % 4)
    if fp_off is None or fp_off + 8 != len(pkt):
        return None                       # FINGERPRINT mandatory + last
                                          # (attr = type2+len2+value4 = 8)
    crc = (zlib.crc32(pkt[:fp_off]) ^ STUN_FP_XOR) & 0xFFFFFFFF
    if struct.unpack(">I", attrs[0x8028])[0] != crc:
        return None
    if mi_off is None or len(attrs.get(0x0008, b"")) != 20:
        return None
    adjusted = len(pkt) - 20 - 8
    head = pkt[:2] + struct.pack(">H", adjusted) + pkt[4:mi_off]
    if not hmac.compare_digest(hmac.new(mi_key, head, hashlib.sha1).digest(),
                               attrs[0x0008]):
        return None
    return mtype, tid, attrs


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
        self.deletes = 0        # round L (F18): DELETE v2/session/{id} count
        self.announce_seen = None
        # round XXV: the PLAY state machine the real rig showed at R7
        # run 6 (PLAY 455 until the client's hole-punch burst arrives)
        self.ice_req = 0
        self.ice_resp = 0
        self.play_455 = 0
        self._ufrag = None
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
                elif req.startswith("DELETE /v2/session/"):
                    # round XXIII: the reference's stop_at answers 2xx/404;
                    # 204 with no body (validate_delete_response shape).
                    # round L (F18): COUNTED, because --session-release now
                    # sends this at teardown and the e2e asserts it arrived
                    # (run 31's orphaned seats came from a DELETE that was
                    # never sent at all).
                    self.deletes += 1
                    conn.sendall(b"HTTP/1.1 204 No Content\r\n\r\n")
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
                    # round XXIV: deliver the (>8 KiB) SDP response as TWO
                    # BINARY messages — the reference reads both frame
                    # types into its accumulation buffer (nvst_rtsp.rs
                    # :620-623) and real rigs fragment large responses;
                    # this exercises binary acceptance + across-message
                    # accumulation over real sockets.
                    raw = self.rtsp_response(
                        first, cseq,
                        ["Session: %s" % self.session_id,
                         "Content-Type: application/sdp"],
                        DESCRIBE_SDP).encode()
                    half = len(raw) // 2
                    conn.sendall(ws_frame(raw[:half], opcode=0x2))
                    conn.sendall(ws_frame(raw[half:], opcode=0x2))
                elif first == "SETUP":
                    conn.sendall(ws_frame(self.rtsp_response(
                        first, cseq,
                        ["Session: %s" % self.session_id,
                         "Transport: RTP/AVP/UDP;unicast;source=127.0.0.1;"
                         "X-GS-ServerPort=%d-%d" % (self.media_port,
                                                     self.media_port),
                         "x-nv-ping: 6",
                         # hex on purpose: the reference ladder increments
                         # it (a1b24800 -> a1b24801, nvst_rtsp.rs:2316)
                         "x-nv-ping-payload: a1b24800"])))
                elif first == "ANNOUNCE":
                    self.announce_seen = text
                    conn.sendall(ws_frame(self.rtsp_response(
                        first, cseq, ["Session: %s" % self.session_id])))
                elif first == "PLAY":
                    # round XXV state machine: the FIRST PLAY always gets
                    # 455 (deterministically exercising the client's
                    # pump-and-retry), later ones need >=1 validated ICE
                    # request on the media socket — the R7 run-6 lesson.
                    if self.play_455 == 0 or self.ice_req == 0:
                        self.play_455 += 1
                        conn.sendall(ws_frame(
                            "RTSP/1.0 455 Method Not Valid in This State"
                            "\r\nCSeq: %s\r\n\r\n" % cseq))
                        continue
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

    def announced_ufrag(self):
        """The local ufrag the client ANNOUNCEd (parsed once)."""
        if self._ufrag is None:
            self._ufrag = ""
            for line in (self.announce_seen or "").split("\r\n"):
                if line.startswith("a=x-nv-general.iceUserNameFragmentV2:"):
                    self._ufrag = line.split(":", 1)[1].strip()
        return self._ufrag

    def serve_media(self):
        """Round XXV: validate the client's pre-PLAY hole-punch burst and
        answer binding requests — the rig half of the run-6 state
        machine. Independent Python STUN (stun_check/stun_build)."""
        self.media.settimeout(0.5)
        while not STOP.is_set():
            try:
                pkt, src = self.media.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            parsed = stun_check(pkt, b"RIGPASSWORD")
            if parsed is None or parsed[0] != 0x0001:
                continue
            _, tid, attrs = parsed
            ufrag = self.announced_ufrag()
            user = attrs.get(0x0006, b"").decode("ascii", "replace")
            if not ufrag or user not in ("a1b24801:%s" % ufrag,
                                         "PING:%s" % ufrag):
                continue                  # wrong USERNAME: stay silent
            self.ice_req += 1
            xma = b"\x00\x01" + struct.pack(
                ">H", src[1] ^ ((STUN_MAGIC >> 16) & 0xFFFF))
            xma += bytes(a ^ b for a, b in
                         zip(socket.inet_aton(src[0]),
                             struct.pack(">I", STUN_MAGIC)))
            try:
                self.media.sendto(
                    stun_build(0x0101, tid, b"RIGPASSWORD", 0x0020, xma),
                    src)
                self.ice_resp += 1
            except OSError:
                pass

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
        t_media = threading.Thread(target=self.serve_media, daemon=True)
        t_http.start()
        t_ws.start()
        t_media.start()
        while not STOP.is_set():
            STOP.wait(0.2)
        # emit the evidence line (the harness reads stdout after exit)
        print(json.dumps({"mode": "fake-rig", "event": "done",
                          "plays": self.plays,
                          "deletes": self.deletes,
                          "ice_req": self.ice_req,
                          "ice_resp": self.ice_resp,
                          "play_455": self.play_455,
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
