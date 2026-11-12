"""Minimal obs-websocket v5 client using only the Python standard library.

Used by tools/acceptance_test.py. Supports unauthenticated or password
auth, requests, and vendor requests to the "output-mapping" vendor.
"""

import base64
import hashlib
import json
import os
import socket
import struct
import time


class OBSWebSocket:
    def __init__(self, host="127.0.0.1", port=4455, password=None, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET / HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Protocol: obswebsocket.json\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("handshake failed")
            resp += chunk
        head, _, self._buf = resp.partition(b"\r\n\r\n")
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise ConnectionError(head.decode(errors="replace"))
        self._counter = 0
        hello = self._recv_op(0)
        ident = {"rpcVersion": 1, "eventSubscriptions": 0}
        auth = hello["d"].get("authentication")
        if auth:
            if password is None:
                raise ConnectionError("server requires a password")
            secret = base64.b64encode(hashlib.sha256((password + auth["salt"]).encode()).digest()).decode()
            ident["authentication"] = base64.b64encode(
                hashlib.sha256((secret + auth["challenge"]).encode()).digest()
            ).decode()
        self._send({"op": 1, "d": ident})
        self._recv_op(2)

    # -- framing --------------------------------------------------------
    def _send(self, obj):
        data = json.dumps(obj).encode()
        hdr = bytearray([0x81])
        n = len(data)
        if n < 126:
            hdr.append(0x80 | n)
        elif n < 65536:
            hdr.append(0x80 | 126)
            hdr += struct.pack(">H", n)
        else:
            hdr.append(0x80 | 127)
            hdr += struct.pack(">Q", n)
        mask = os.urandom(4)
        hdr += mask
        self.sock.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, n):
        while len(self._buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed")
            self._buf += chunk
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def _recv_frame(self):
        b0, b1 = self._read(2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._read(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._read(8))[0]
        payload = self._read(n)
        return b0 & 0x0F, payload

    def _recv_op(self, op):
        while True:
            opcode, payload = self._recv_frame()
            if opcode == 8:
                raise ConnectionError(f"closed by server: {payload[2:].decode(errors='replace')}")
            if opcode != 1:
                continue
            msg = json.loads(payload)
            if msg.get("op") == op:
                return msg

    # -- API ------------------------------------------------------------
    def call(self, request_type, data=None, check=True):
        self._counter += 1
        rid = str(self._counter)
        d = {"requestType": request_type, "requestId": rid}
        if data is not None:
            d["requestData"] = data
        self._send({"op": 6, "d": d})
        while True:
            msg = self._recv_op(7)
            if msg["d"]["requestId"] == rid:
                st = msg["d"]["requestStatus"]
                if check and not st["result"]:
                    raise RuntimeError(f"{request_type} failed: {st}")
                return msg["d"].get("responseData", {})

    def vendor(self, request_type, data=None, vendor="output-mapping"):
        res = self.call(
            "CallVendorRequest",
            {"vendorName": vendor, "requestType": request_type, "requestData": data or {}},
        )
        return res.get("responseData", {})

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def connect(retries=60, delay=1.0, **kw):
    """Connect and wait until OBS has finished loading (GetVersion succeeds)."""
    last = None
    for _ in range(retries):
        ws = None
        try:
            ws = OBSWebSocket(**kw)
            ws.call("GetVersion")
            return ws
        except (OSError, ConnectionError, RuntimeError) as e:
            last = e
            if ws:
                ws.close()
            time.sleep(delay)
    raise ConnectionError(f"could not connect to obs-websocket: {last}")
