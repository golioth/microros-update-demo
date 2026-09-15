#!/usr/bin/env python3
"""Capture proxy for the Golioth Connect Agent snap (gateway service).

The snap allows overriding its cloud endpoint (snap set connect-agent
gateway-server-address=...). Point it at this proxy to see exactly what
the gateway transmits to the pouch server — in particular the
POST /.g/device-cert body it forwards on behalf of a BLE device.

The proxy forwards everything to the real upstream using the gateway's
own mTLS client certificate, and saves every request body to a capture
directory. Harmless to the cloud: .g/server-cert GETs and raw-DER cert
uploads are exactly what the real flow does.

Usage:
    python3 gw-capture-proxy.py \
        --listen 127.0.0.1:8080 \
        --upstream https://gw.golioth.io \
        --cert /path/gateway.crt.pem \
        --key /path/gateway.key.pem \
        --captures /tmp/gw-captures

Then:
    snap set connect-agent gateway-server-address=http://127.0.0.1:8080
    # (the configure hook restarts the gateway)
    # press the device's sync-request button; inspect the capture dir
    # afterwards restore:
    snap set connect-agent gateway-server-address=https://gw.golioth.io
"""
import argparse
import hashlib
import http.server
import os
import re
import threading
import time

import requests

counter_lock = threading.Lock()
counter = 0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--listen", default="127.0.0.1:8080")
    p.add_argument("--upstream", default="https://gw.golioth.io")
    p.add_argument("--cert", required=True, help="gateway client cert PEM")
    p.add_argument("--key", required=True, help="gateway client key PEM")
    p.add_argument("--captures", default="/tmp/gw-captures")
    args = p.parse_args()

    host, port = args.listen.rsplit(":", 1)
    os.makedirs(args.captures, exist_ok=True)
    log_path = os.path.join(args.captures, "proxy.log")

    upstream = requests.Session()
    upstream.cert = (args.cert, args.key)

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, format, *args):
            pass

        def _handle(self):
            global counter
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length) if length else b""

            with counter_lock:
                counter += 1
                n = counter

            safe_path = re.sub(r"[^A-Za-z0-9._-]", "_", self.path.strip("/")) or "root"
            stamp = time.strftime("%H:%M:%S")
            sha = hashlib.sha256(body).hexdigest()[:16] if body else "-"

            with open(log_path, "a") as f:
                f.write(f"[{stamp}] #{n} {self.command} {self.path} "
                        f"len={len(body)} sha16={sha} "
                        f"ct={self.headers.get('Content-Type')}\n")
                if body:
                    f.write("         first-bytes: "
                            + " ".join(f"{b:02x}" for b in body[:40]) + "\n")
            if body:
                with open(os.path.join(args.captures,
                                       f"{n:03d}_{self.command}_{safe_path}.bin"), "wb") as f:
                    f.write(body)

            try:
                resp = upstream.request(
                    self.command,
                    args.upstream + self.path,
                    data=body,
                    headers={"Content-Type":
                             self.headers.get("Content-Type", "application/octet-stream")},
                    timeout=30)
            except Exception as e:
                with open(log_path, "a") as f:
                    f.write(f"[{stamp}] #{n} UPSTREAM ERROR: {e}\n")
                self.send_response(502)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

            with open(log_path, "a") as f:
                f.write(f"[{stamp}] #{n} upstream -> {resp.status_code} "
                        f"({len(resp.content)} B resp)\n")
            self.send_response(resp.status_code)
            self.send_header("Content-Type",
                             resp.headers.get("Content-Type", "application/octet-stream"))
            self.send_header("Content-Length", str(len(resp.content)))
            self.end_headers()
            self.wfile.write(resp.content)

        do_GET = _handle
        do_POST = _handle

    srv = http.server.ThreadingHTTPServer((host, int(port)), Handler)
    print(f"capture proxy on http://{host}:{port} -> {args.upstream}; "
          f"captures in {args.captures}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()