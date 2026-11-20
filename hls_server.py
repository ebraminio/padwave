#!/usr/bin/env python3
"""Serve padwave's local HLS segments to local/LAN browsers and VLC."""
import http.server
import os


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


os.chdir("/tmp/padwave-live")
http.server.ThreadingHTTPServer(("0.0.0.0", 8090), Handler).serve_forever()
