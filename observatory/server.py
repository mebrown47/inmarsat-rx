#!/usr/bin/env python3
# Copyright (c) 2026 Mike Brown
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Inmarsat Observatory: the dashboard server

A small read-only HTTP server over the database rollup.py builds from inmarsat-rx's archive.
Stdlib only, no external assets, so it runs on a headless receiver with no internet. Adapted
from vdl2-observatory's server.py.

  GET /               -> Overview: traffic by service, satellites and channels, ground stations
  GET /api/overview   -> its data (?hours=N&sat=ID)
  GET /aviation       -> aircraft, per-aircraft history, CPDLC and ADS-C
  GET /api/aviation   -> its data (?hours=N&sat=ID&aes=HEX)
  GET /maritime       -> STD-C EGC broadcasts
  GET /api/maritime   -> its data (?hours=N&sat=ID&prio=P)
  GET /healthz        -> ok

Usage:
    server.py [--db PATH] [--port 8893] [--host 0.0.0.0] [--live-port 8892]

It never talks to inmarsat-rx; run rollup.py on a timer to keep it current.
"""

import argparse
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aviation_view  # noqa: E402
import maritime_view  # noqa: E402
import overview_view  # noqa: E402

DB_PATH = "observatory/observatory.db"
# iridium-observatory uses 8888/8890 and vdl2-observatory 8889/8891, so all three can share a host.
PORT = 8893
LIVE_PORT = 8892


class Handler(BaseHTTPRequestHandler):
    db_path = DB_PATH
    live_port = LIVE_PORT
    server_version = "inmarsat-observatory"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype, cache="no-store"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", cache)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _page(self, html):
        self._send(200, html.replace("__LIVE_PORT__", str(self.live_port)), "text/html; charset=utf-8")

    def _json(self, build, *a):
        try:
            self._send(200, json.dumps(build(*a)), "application/json")
        except Exception as e:  # noqa: keep the server alive
            self._send(500, json.dumps({"error": str(e)}), "application/json")

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        arg = lambda k: (q.get(k, [None])[0] or None)
        if u.path == "/":
            self._page(overview_view.PAGE)
        elif u.path == "/api/overview":
            self._json(overview_view.build_overview, self.db_path, arg("hours"), arg("sat"))
        elif u.path == "/aviation":
            self._page(aviation_view.PAGE)
        elif u.path == "/api/aviation":
            self._json(aviation_view.build_aviation, self.db_path, arg("hours"), arg("sat"), arg("aes"))
        elif u.path == "/maritime":
            self._page(maritime_view.PAGE)
        elif u.path == "/api/maritime":
            self._json(maritime_view.build_maritime, self.db_path, arg("hours"), arg("sat"), arg("prio"))
        elif u.path == "/healthz":
            self._send(200, "ok", "text/plain")
        else:
            self._send(404, "not found", "text/plain")

    do_HEAD = do_GET


def main():
    ap = argparse.ArgumentParser(description="Inmarsat Observatory server")
    ap.add_argument("--db", default=DB_PATH)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--live-port", type=int, default=LIVE_PORT,
                    help="inmarsat-rx's live page on this host, for the link to it (0 = no link)")
    args = ap.parse_args()
    Handler.db_path = args.db
    Handler.live_port = args.live_port
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"inmarsat observatory: http://{args.host}:{args.port}/  db={args.db}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
