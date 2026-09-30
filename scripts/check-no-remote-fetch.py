#!/usr/bin/env python3
"""Text from the network must never make the view fetch anything.

Renders the real view (tests/qml/Harness.qml, state "inject") where a title, an
alias, a body, the node status, the history status and the error line all carry
<img src="http://127.0.0.1:8977/…"> tags, while a local HTTP server logs every
request. Any request other than the harness's own control is a failure: it
would tell whoever wrote that text the reader's IP address.

The harness also shows one deliberately rich-text <img> (/control.png). If that
one is not fetched, the check proved nothing (no server, no Qt, no render) and
fails too. Needs Qt 6.9 (`qml`, or $QML).
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
shot = "/tmp/forum-inject.png"
for w, h in ((1200, 800), (400, 800)):
    if os.path.exists(shot): os.remove(shot)
    r = subprocess.run([os.environ.get("QML", "qml"), os.path.join(here, "..", "tests", "qml", "Harness.qml"), "--",
                        str(w), str(h), "inject", shot], env=env, capture_output=True, timeout=60)
    if r.returncode != 0 or not os.path.exists(shot):
        print(f"FAIL: the view did not render at {w}x{h} (exit {r.returncode})"); sys.exit(1)
time.sleep(1)
leaks = [p for p in hits if p != "/control.png"]
if "/control.png" not in hits:
    print("FAIL: the control image was not fetched, so this check tested nothing"); sys.exit(1)
if leaks:
    print("FAIL: the view fetched", sorted(set(leaks))); sys.exit(1)
print("ok   no request left the view for text from the network (titles, aliases, bodies, status, errors); control fetched")
