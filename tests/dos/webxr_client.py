#!/usr/bin/env python3
"""
webxr_client.py -- stand in for the browser, so the WebXR path can be tested
without a headset.

Speaks exactly what integration/dosbox-x/overlay/share/vfx1-webxr.html speaks:
connect to the emulator's WebSocket, then stream "P w x y z t" pose lines. What
it cannot stand in for is WebXR itself, so a pass here means the socket, the
framing, the wire format and the mouse injection all work, and only the browser
half is unverified.

No third-party modules on purpose: this has to run wherever the tests do.

  python3 webxr_client.py --port 8085 --seconds 12
"""

import argparse
import base64
import hashlib
import math
import os
import socket
import struct
import sys
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def handshake(sock, host, port, target="/pose"):
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {target} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(req.encode())

    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("server closed during handshake")
        buf += chunk
        if len(buf) > 65536:
            raise RuntimeError("handshake response too large")

    head = buf.split(b"\r\n\r\n", 1)[0].decode(errors="replace")
    if "101" not in head.split("\r\n")[0]:
        raise RuntimeError(f"expected 101, got: {head.splitlines()[0]}")

    expect = base64.b64encode(
        hashlib.sha1((key + GUID).encode()).digest()
    ).decode()
    for line in head.split("\r\n")[1:]:
        if line.lower().startswith("sec-websocket-accept:"):
            got = line.split(":", 1)[1].strip()
            if got != expect:
                raise RuntimeError(f"bad accept key: {got} != {expect}")
            return
    raise RuntimeError("no Sec-WebSocket-Accept in the response")


def send_text(sock, text):
    """One masked client-to-server text frame, as RFC 6455 requires."""
    payload = text.encode()
    n = len(payload)
    hdr = bytearray([0x81])
    if n < 126:
        hdr.append(0x80 | n)
    elif n <= 0xFFFF:
        hdr.append(0x80 | 126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(0x80 | 127)
        hdr += struct.pack(">Q", n)
    mask = os.urandom(4)
    hdr += mask
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    sock.sendall(bytes(hdr) + masked)


def pose_line(t):
    """The same sweep shape the scripted source uses, so results compare."""
    yaw = 0.9 * math.sin(2 * math.pi * t / 8.0)
    pitch = 0.25 * math.sin(2 * math.pi * t / 5.0)
    cy, sy = math.cos(yaw / 2), math.sin(yaw / 2)
    cp, sp = math.cos(pitch / 2), math.sin(pitch / 2)
    # yaw about +Y then pitch about +X, matching vfxcore_api.h
    w, x, y, z = cy * cp, cy * sp, sy * cp, -sy * sp
    return f"P {w:.6f} {x:.6f} {y:.6f} {z:.6f} {t * 1000.0:.1f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8085)
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--hz", type=float, default=90.0)
    ap.add_argument("--connect-timeout", type=float, default=30.0,
                    help="keep retrying while the emulator boots")
    ap.add_argument("--recenter", action="store_true",
                    help="send one recentre message before streaming")
    args = ap.parse_args()

    deadline = time.time() + args.connect_timeout
    sock = None
    while time.time() < deadline:
        try:
            sock = socket.create_connection((args.host, args.port), timeout=5)
            break
        except OSError:
            time.sleep(0.25)
    if sock is None:
        print(f"could not connect to {args.host}:{args.port}", file=sys.stderr)
        return 1

    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    handshake(sock, args.host, args.port)
    print(f"connected to {args.host}:{args.port}, streaming {args.hz:g} Hz "
          f"for {args.seconds:g}s")

    if args.recenter:
        send_text(sock, "R")

    period = 1.0 / args.hz
    start = time.time()
    sent = 0
    try:
        while True:
            t = time.time() - start
            if t >= args.seconds:
                break
            send_text(sock, pose_line(t))
            sent += 1
            time.sleep(period)
    except (BrokenPipeError, ConnectionResetError, OSError) as exc:
        print(f"connection ended after {sent} poses: {exc}")
        return 0
    finally:
        try:
            sock.close()
        except OSError:
            pass

    print(f"sent {sent} poses")
    return 0


if __name__ == "__main__":
    sys.exit(main())
