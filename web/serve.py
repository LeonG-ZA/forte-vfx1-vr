#!/usr/bin/env python3
"""
serve.py -- a small web server for VFX1 game zips and the WebXR build.

Serves one folder over HTTP. The Quest and Windows apps' "Download games from
the PC..." menu lists the zips in that folder and downloads the one picked;
downloads resume after a dropped connection. It also serves the experimental
WebXR build (application/wasm, no caching) and collects log lines the apps and
pages post to /applog and /telemetry.

  python web/serve.py --dir path/to/games --lan
  python web/serve.py [--port 8080] [--dir build/web] [--lan] [--https]

--lan listens on every interface, so a headset on the same network can reach
the server; the address to use is printed at start. --https serves over TLS
with a self-signed certificate, which the WebXR page needs on another device.
"""

import argparse
import functools
import http.server
import json
import os
import shutil
import socket
import ssl
import subprocess
import time


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
        ".COM": "application/octet-stream",
        ".apk": "application/vnd.android.package-archive",
    }

    reports_path = "build/serve/log.txt"

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("Accept-Ranges", "bytes")
        super().end_headers()

    def send_head(self):
        """Serve byte ranges, so a download interrupted on a flaky network can
        resume. The stock handler ignores Range, and a downloader that asks to
        resume and gets the whole file again treats that as a failure."""
        rng = self.headers.get("Range")
        path = self.translate_path(self.path)
        if not rng or not rng.startswith("bytes=") or not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        try:
            first, last = rng[6:].split(",")[0].strip().split("-")
            if first == "":
                start, end = max(0, size - int(last)), size - 1
            else:
                start = int(first)
                end = int(last) if last else size - 1
        except ValueError:
            return super().send_head()
        if start >= size or start > end:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.end_headers()
            return None
        end = min(end, size - 1)
        f = open(path, "rb")
        f.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        self._range_left = end - start + 1
        return f

    def copyfile(self, source, outputfile):
        left = getattr(self, "_range_left", None)
        if left is None:
            return super().copyfile(source, outputfile)
        while left > 0:
            chunk = source.read(min(65536, left))
            if not chunk:
                break
            outputfile.write(chunk)
            left -= len(chunk)
        self._range_left = None

    def do_POST(self):
        """Log lines from the native apps (/applog) and live samples from the
        WebXR page (/telemetry), appended next to --reports. Bodies are capped."""
        route = self.path.rstrip("/").split("?")[0]
        if route == "/applog":
            # Plain-text log lines from the native Quest app, which has no other
            # way to show its log without adb.
            length = int(self.headers.get("Content-Length") or 0)
            if 0 < length <= 16384:
                text = self.rfile.read(length).decode("utf-8", "replace").rstrip()
                stamp = time.strftime("%H:%M:%S")
                path = os.path.join(os.path.dirname(os.path.abspath(self.reports_path)), "applog.txt")
                with open(path, "a", encoding="utf-8") as f:
                    for line in text.splitlines():
                        f.write(f"{stamp} {self.client_address[0]} {line}\n")
                print(f"APPLOG {self.client_address[0]}: {text}", flush=True)
            self.send_response(204)
            self.end_headers()
            return
        if route != "/telemetry":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > 65536:
            self.send_error(413)
            return
        body = self.rfile.read(length)
        try:
            result = json.loads(body)
        except ValueError:
            self.send_error(400, "not JSON")
            return
        result["_from"] = self.client_address[0]
        result["_received"] = time.strftime("%Y-%m-%d %H:%M:%S")

        # Periodic live samples from the WebXR page.
        path = os.path.join(os.path.dirname(os.path.abspath(self.reports_path)),
                            "telemetry.jsonl")
        with open(path, "a", encoding="utf-8") as f:
            f.write(json.dumps(result) + "\n")
        last = (result.get("samples") or [{}])[-1]
        print(f"TELEMETRY from {result['_from']}: {result.get('game')} "
              f"backend={result.get('backend')} cycles={last.get('cycles')} "
              f"load={last.get('cpuLoad')} frames/s={last.get('framesPerSecond')}",
              flush=True)
        self.send_response(204)
        self.end_headers()
        return


def lan_addresses():
    """The address other devices should use first, then every other IPv4.

    gethostbyname(gethostname()) is not good enough: on a machine with WSL or
    Hyper-V it happily returns the virtual switch address, which no phone or
    headset can reach. Connecting a UDP socket sends nothing, but makes the OS
    pick the interface it would route real traffic through, which is the one
    on the actual LAN.
    """
    primary = None
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 80))        # TEST-NET-1, never contacted
            primary = s.getsockname()[0]
    except OSError:
        pass
    others = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            others.add(info[4][0])
    except OSError:
        pass
    others.discard(primary)
    others.discard("127.0.0.1")
    return primary, sorted(others)


def find_openssl():
    """openssl on PATH, or the copy that ships with Git for Windows."""
    found = shutil.which("openssl")
    if found:
        return found
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"),
                 os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")):
        for sub in (r"Git\mingw64\bin\openssl.exe", r"Git\usr\bin\openssl.exe"):
            candidate = os.path.join(base, sub)
            if os.path.isfile(candidate):
                return candidate
    return None


def ensure_certificate(tls_dir, lan_ip):
    """A self-signed certificate covering localhost and this machine's LAN IP.

    WebXR only runs in a secure context. http://localhost qualifies, but a
    headset reaching this PC over the LAN uses an IP address, and plain http
    to an IP does not, so the browser hides WebXR entirely. HTTPS with a
    self-signed certificate is the least intrusive way round that: accept the
    warning once on the headset. The certificate is regenerated when the LAN
    address changes, since it is only valid for the addresses it names.
    """
    os.makedirs(tls_dir, exist_ok=True)
    cert = os.path.join(tls_dir, "cert.pem")
    key = os.path.join(tls_dir, "key.pem")
    stamp = os.path.join(tls_dir, "addresses.txt")
    names = f"DNS:localhost,IP:127.0.0.1" + (f",IP:{lan_ip}" if lan_ip else "")
    if os.path.isfile(cert) and os.path.isfile(key) and os.path.isfile(stamp):
        with open(stamp, encoding="utf-8") as f:
            if f.read().strip() == names:
                return cert, key
    openssl = find_openssl()
    if not openssl:
        raise SystemExit("--https needs openssl (Git for Windows includes one)")
    subprocess.run([openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                    "-days", "825", "-keyout", key, "-out", cert,
                    "-subj", "/CN=VFX1 game server",
                    "-addext", f"subjectAltName={names}"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(stamp, "w", encoding="utf-8") as f:
        f.write(names)
    print(f"generated a self-signed certificate for {names}")
    return cert, key


def main():
    # Defaults are anchored to the repository, not the current directory, so
    # the server serves the right folder whichever directory it starts in.
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--dir", default=os.path.join(root, "build", "web"))
    ap.add_argument("--lan", action="store_true")
    ap.add_argument("--https", action="store_true",
                    help="serve over TLS with a self-signed certificate; "
                         "needed for WebXR on another device")
    ap.add_argument("--reports", default=os.path.join(root, "build", "serve", "log.txt"),
                    help="the folder of this file gets applog.txt, telemetry.jsonl and the certificate")
    args = ap.parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(args.reports)), exist_ok=True)
    Handler.reports_path = args.reports

    host = "0.0.0.0" if args.lan else "127.0.0.1"
    scheme = "https" if args.https else "http"
    primary, others = lan_addresses()
    handler = functools.partial(Handler, directory=args.dir)
    with http.server.ThreadingHTTPServer((host, args.port), handler) as httpd:
        if args.https:
            cert, key = ensure_certificate(
                os.path.join(os.path.dirname(os.path.abspath(args.reports)), "tls"), primary)
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert, key)
            httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
        print(f"serving {args.dir} on {scheme}://{host}:{args.port}/")
        if args.lan:
            if primary:
                print(f"from another device: {scheme}://{primary}:{args.port}/")
            if others:
                print("other addresses on this machine, usually virtual adapters: "
                      + ", ".join(others))
            print("every request is logged below; if nothing appears when a "
                  "device tries to connect, its traffic is not reaching this machine")
        httpd.serve_forever()


if __name__ == "__main__":
    main()
