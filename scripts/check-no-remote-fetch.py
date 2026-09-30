#!/usr/bin/env python3
"""Peer-supplied text must never make the view fetch anything.

Renders the real view (tests/qml/Harness.qml, state "inject") with a title, an
alias and a body carrying <img src="http://127.0.0.1:8977/…"> tags, while a
local HTTP server logs every request. Any request is a failure: it would tell
the post's author the reader's IP address. Needs Qt 6.9 (`qml`, or $QML).

    scripts/check-no-remote-fetch.py
"""
import http.server, os, socketserver, subprocess, sys, threading, time

hits = []
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        hits.append(self.path); self.send_response(404); self.end_headers()
    def log_message(self, *a): pass

socketserver.TCPServer.allow_reuse_address = True
srv = socketserver.TCPServer(("127.0.0.1", 8977), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
here = os.path.dirname(os.path.abspath(__file__))
env = dict(os.environ, QT_QUICK_CONTROLS_STYLE="Basic")
for w, h in ((1200, 800), (400, 800)):
    subprocess.run([os.environ.get("QML", "qml"), os.path.join(here, "..", "tests", "qml", "Harness.qml"), "--",
                    str(w), str(h), "inject", "/tmp/forum-inject.png"], env=env, capture_output=True, timeout=60)
time.sleep(1)
if hits:
    print("FAIL: the view fetched", hits); sys.exit(1)
print("ok   no request left the view for peer-supplied titles, aliases or bodies")
