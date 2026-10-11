#!/usr/bin/env python3
"""
http_get.py -- fetch the WebXR page from a running emulator and print its size.

Used by run-webxr.sh to check that DOSBox-X really serves the page, since the
browser needs it over http://localhost to count as a secure context and be
allowed to ask for WebXR at all.

Exits non-zero if the emulator is not up yet, so the caller can just retry.

  python3 http_get.py <port> [path]
"""

import socket
import sys


def main():
    port = int(sys.argv[1])
    path = sys.argv[2] if len(sys.argv) > 2 else "/"

    with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
        s.sendall(
            f"GET {path} HTTP/1.1\r\nHost: localhost\r\n"
            "Connection: close\r\n\r\n".encode()
        )
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if len(data) > 4 * 1024 * 1024:
                break

    head, _, body = data.partition(b"\r\n\r\n")
    if b"200 OK" not in head:
        print(f"not 200: {head.splitlines()[:1]}", file=sys.stderr)
        return 1
    if b"text/html" not in head:
        print("missing text/html content type", file=sys.stderr)
        return 1
    print(len(body))
    return 0


if __name__ == "__main__":
    sys.exit(main())
