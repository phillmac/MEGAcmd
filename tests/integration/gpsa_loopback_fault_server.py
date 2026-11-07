#!/usr/bin/env python3
"""Disposable MEGA SDK gpsa HTTP transport fault responder.

ONLY binds 127.0.0.1. Never use with an authenticated account.
Records request action names, not the request payload. Accepts no credentials.
"""
import argparse
import json
import logging
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18765)
    parser.add_argument("--mode", choices=["stall", "drop", "late", "normal"], default="stall")
    parser.add_argument("--delay", type=float, default=35.0)
    parser.add_argument("--events", type=Path, required=True)
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("port must be 1024..65535")
    args.events.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    args.events.touch(mode=0o600, exist_ok=True)
    lock = threading.Lock()

    class Server(ThreadingHTTPServer):
        daemon_threads = True
        allow_reuse_address = True

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            return

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 256 * 1024:
                self.send_error(413)
                return
            body = self.rfile.read(length)
            try:
                decoded = json.loads(body)
            except Exception:
                decoded = None
            if isinstance(decoded, dict):
                requests = [decoded]
            elif isinstance(decoded, list):
                requests = decoded
            else:
                requests = []
            actions = [str(x.get("a", "?")) for x in requests if isinstance(x, dict)]
            # Only action names: no usernames, tokens, JSON body or URI query.
            event = {"time": time.time(), "method": "POST", "actions": actions,
                     "bytes": len(body), "mode": args.mode}
            with lock:
                with args.events.open("a", encoding="utf-8") as f:
                    f.write(json.dumps(event, sort_keys=True) + "\n")
                    f.flush()
            print("REQUEST " + json.dumps(event, sort_keys=True), flush=True)

            if "gpsa" in actions:
                if args.mode == "stall":
                    # Keep the TCP connection established but never complete the response.
                    while True:
                        time.sleep(1)
                if args.mode == "drop":
                    self.close_connection = True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                if args.mode == "late":
                    time.sleep(args.delay)
            # -9 is MEGA API_ENOENT: no PSA is available.
            payload = b"[-9]"
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            self.wfile.flush()

    with Server(("127.0.0.1", args.port), Handler) as server:
        print(f"READY 127.0.0.1:{args.port} mode={args.mode}", flush=True)
        server.serve_forever(poll_interval=0.25)


if __name__ == "__main__":
    main()
