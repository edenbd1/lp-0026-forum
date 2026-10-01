#!/bin/bash
# What the forum's RLN membership sponsor is doing (gifter/run.sh, LaunchAgent
# co.logos.forum-gifter): agent, daemon, listening port, router mapping, the
# account that pays and how many memberships its balance still covers.
#
#   scripts/gifter-status.sh [GIFTER_HOME]
set -u
GH=${1:-$HOME/logos-forum-gifter}
SEQ=${GIFTER_SEQUENCER:-http://209.38.241.182:3240}
ok() { echo "ok   $*"; }
no() { echo "--   $*"; }
J() { /usr/bin/python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d.get(sys.argv[2], ""))' "$GH/status.json" "$1" 2> /dev/null; }

if launchctl print "gui/$(id -u)/co.logos.forum-gifter" > /dev/null 2>&1; then
  ok "LaunchAgent co.logos.forum-gifter loaded ($(launchctl print "gui/$(id -u)/co.logos.forum-gifter" | awk '/^\tstate =/{print $3}'))"
else
  no "LaunchAgent co.logos.forum-gifter not loaded (gifter/install.sh)"
fi
[ -f "$GH/status.json" ] || { no "no $GH/status.json yet: see $GH/logs/gifter.log"; exit 1; }
PID=$(J daemonPid); PORT=$(J port)
if kill -0 "$PID" 2> /dev/null; then ok "logosctl daemon pid $PID"; else no "logosctl daemon $PID not running"; fi
if lsof -nP -iTCP:"$PORT" -sTCP:LISTEN > /dev/null 2>&1; then ok "listening on tcp/$PORT"; else no "nothing listening on tcp/$PORT"; fi
if /usr/bin/python3 "$GH/bin/upnp-map.py" list 2> /dev/null | grep -q "^$PORT/TCP .*logos-forum-gifter"; then
  ok "router maps $(J externalAddress):$PORT to this Mac"
else
  no "no router mapping for tcp/$PORT (clients outside this network cannot reach the gifter)"
fi
ok "gifter address  $(J multiaddr)/p2p/$(J peerId)"

PAYER=$(J payerBase58)
BAL=$(curl -s -m 10 "$SEQ" -H 'content-type: application/json' \
  -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccountBalance\",\"params\":[\"$PAYER\"]}" \
  | /usr/bin/python3 -c 'import json,sys; print(json.load(sys.stdin)["result"])' 2> /dev/null)
echo "     payer          $PAYER (on the LEZ zone behind $SEQ)"
echo "     balance        ${BAL:-unreadable}"
# Each registration pays the price (rate 100 x 10,000 per unit = 1,000,000)
# and its fee (74M to 83M at base fee 8, measured upstream on devnet; 83M here), and the payer must also hold a fee
# reserve of about 181.8M at the moment it registers (refunded down to the fee).
if [ -n "$BAL" ]; then
  /usr/bin/python3 - "$BAL" <<'PY'
import sys
bal = int(sys.argv[1]); reserve = 181_800_000; per = 1_000_000 + 83_000_000
n = 0 if bal < 1_000_000 + reserve else 1 + (bal - 1_000_000 - reserve) // per
print(f"     covers         about {n} more membership(s)")
PY
fi
echo "     log            $GH/logs/gifter.log ($(grep -c ' STATUS ' "$GH/logs/gifter.log" 2> /dev/null) status lines)"
tail -3 "$GH/logs/gifter.log" 2> /dev/null | sed 's/^/     /'
