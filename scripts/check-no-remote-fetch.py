#!/usr/bin/env python3
"""Text from the network must never make the view fetch anything.

Renders the real view (tests/qml/Harness.qml, state "inject") where a title, an
alias, a body, the node status, the history status and the error line all carry
<img src="http://127.0.0.1:8977/…"> tags, while a local server logs every
connection (an HTTP request, or a TLS handshake an https URL would start). The
title and the bodies also carry https links to that server, some of them
written to break out of the link (a quote, a >, an <img> or an <a> next to the
URL), plain http URLs, and javascript:, file: and data: URLs. Then:

- the https URLs must be rendered as links (the harness lists every href), and
  only https ones: a plain http URL is not a link;
- nothing may be fetched without a click;
- a click on the body's link, as Text reports it (linkAt, then linkActivated),
  opens exactly that https URL (into a stub, so the check opens no browser).

Any connection other than the harness's own controls is a failure: it would
tell whoever wrote that text the reader's IP address.

The harness also shows one deliberately rich-text <img> (/control.png) and one
StyledText <img> (/control-styled.png, the format links are shown in). If those
are not fetched, the check proved nothing (no server, no Qt, no render) and
fails too. Needs Qt 6.9 (`qml`, or $QML).
"""
import http.server, json, os, socketserver, subprocess, sys, threading, time

hits = []
class H(http.server.BaseHTTPRequestHandler):
    def handle(self):
        self.seen = None
        try: super().handle()
        except Exception: pass
        if self.seen is None: hits.append("<a connection with no HTTP request: TLS for an https URL?>")
    def do_GET(self):
        self.seen = self.path; hits.append(self.path); self.send_response(404); self.end_headers()
    def log_message(self, *a): pass

socketserver.TCPServer.allow_reuse_address = True
srv = socketserver.ThreadingTCPServer(("127.0.0.1", 8977), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
here = os.path.dirname(os.path.abspath(__file__))
env = dict(os.environ, QT_QUICK_CONTROLS_STYLE="Basic")
shot = "/tmp/forum-inject.png"
want_click = "https://127.0.0.1:8977/link.html"
for w, h in ((1200, 800), (400, 800)):
    if os.path.exists(shot): os.remove(shot)
    r = subprocess.run([os.environ.get("QML", "qml"), os.path.join(here, "..", "tests", "qml", "Harness.qml"), "--",
                        str(w), str(h), "inject", shot], env=env, capture_output=True, timeout=60)
    if r.returncode != 0 or not os.path.exists(shot):
        print(f"FAIL: the view did not render at {w}x{h} (exit {r.returncode})"); sys.exit(1)
    out = (r.stdout + r.stderr).decode(errors="replace").splitlines()
    linked = [int(l.split()[-2]) for l in out if "links rendered in" in l]
    if not linked or linked[0] < 6:
        print(f"FAIL: at {w}x{h} the links were not rendered as links ({linked}), so they were not tested"); sys.exit(1)
    hrefs = json.loads([l for l in out if "qml: hrefs " in l][0].split("qml: hrefs ", 1)[1])
    bad = [x for x in hrefs if not x.startswith('href="https://')]
    if bad:
        print(f"FAIL: at {w}x{h} something other than an https URL became a link: {bad}"); sys.exit(1)
    if any("/plain-http" in x or x.endswith('/http"') for x in hrefs):
        print(f"FAIL: at {w}x{h} a plain http URL became a link"); sys.exit(1)
    clicks = [l.split("click opens ", 1)[1] for l in out if "click opens " in l]
    if clicks != [want_click]:
        print(f"FAIL: at {w}x{h} a click on the body's link opened {clicks}, not [{want_click}]"); sys.exit(1)
time.sleep(1)
controls = ("/control.png", "/control-styled.png")
leaks = [p for p in hits if p not in controls]
missing = [c for c in controls if c not in hits]
if missing:
    print("FAIL: the control image", missing, "was not fetched, so this check tested nothing"); sys.exit(1)
if leaks:
    print("FAIL: the view fetched", sorted(set(leaks))); sys.exit(1)
print("ok   no connection left the view for text from the network (titles, aliases, bodies, status, errors);")
print("     https links shown, plain http not a link, none fetched without a click; a click opens exactly")
print(f"     {want_click}; both controls fetched")
