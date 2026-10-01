#!/bin/bash
# The forum's RLN membership sponsor ("gifter"), run by the LaunchAgent
# co.logos.forum-gifter (gifter/install.sh).
#
# logos.test accepts a message only with an RLN proof, and a proof needs a
# membership in the network's registry, which costs native balance on the
# registry's LEZ zone. Forum users hold none: this service registers a
# membership for them and pays for it. It is an open gifter (LIP-158,
# logos-co/logos-rln-gifter): any identity commitment that asks gets one,
# with no allowlist and no cap.
#
# A headless logosctl daemon (its own session in $GIFTER_HOME/session) loads
#   liblogos_lez_rln_module  its LEZ wallet pays for every registration
#   libp2p_module            listens on $GIFTER_PORT with a fixed key, so the
#                            peer id the forum dials never changes
#   rln_gifter_module        serve(): mounts /logos/rln/membership/1.0.0 and
#                            registers each commitment it is sent
# The port is mapped on the router by UPnP so clients outside can dial it.
#
# Configured by environment (set in the plist).
set -u
GIFTER_HOME="${GIFTER_HOME:?GIFTER_HOME is not set}"
PORT="${GIFTER_PORT:-24026}"
NETWORK_REF="${GIFTER_NETWORK:-testnet}"      # the LEZ RLN network table's name for the logos.test registry's zone
CONFIG_ACCOUNT="${GIFTER_CONFIG_ACCOUNT:-9tZgjoUVHHWuE9D1cgQSXbYu2gm6uN9baTSERtTa9Str}"
MAX_RATE="${GIFTER_MAX_RATE:-100}"            # messages per epoch granted per membership (the registry's minimum)
L="$GIFTER_HOME/logosctl/bin/logosctl"
MAP="$GIFTER_HOME/bin/upnp-map.py"
STATUS="$GIFTER_HOME/status.json"
LEASE=7200
export LOGOSCTL_CONFIG_DIR="$GIFTER_HOME/session"

ts() { date -u +%Y-%m-%dT%H:%M:%SZ; }
say() { echo "$(ts) $*"; }
# The "result" of a logosctl call, as a JSON string (modules answer either a
# JSON string or an object).
result() { /usr/bin/python3 -c '
import json, sys
try: d = json.load(sys.stdin)
except Exception: sys.exit(1)
r = d.get("result")
if r is None: sys.exit(1)
print(r if isinstance(r, str) else json.dumps(r))'; }
field() { /usr/bin/python3 -c '
import json, sys
try: d = json.loads(sys.stdin.read())
except Exception: sys.exit(1)
for k in sys.argv[1].split("."):
    d = d.get(k) if isinstance(d, dict) else None
print("" if d is None else d)' "$1"; }
b58() { /usr/bin/python3 -c '
import sys
A = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
b = bytes.fromhex(sys.argv[1]); n = int.from_bytes(b, "big"); s = ""
while n: n, r = divmod(n, 58); s = A[r] + s
print("1" * (len(b) - len(b.lstrip(b"\0"))) + s)' "$1"; }
call() { "$L" call "$@" 2>&1; }
fail() { say "FAIL $*"; "$L" daemon stop > /dev/null 2>&1; exit 1; }

# 1. Reachability.
EXT=""
if EXT=$(/usr/bin/python3 "$MAP" ensure "$PORT" "$LEASE"); then
  say "RUN mapped $EXT:$PORT -> this host:$PORT"
else
  say "RUN no UPnP mapping: the gifter is reachable on the local network only"
fi

# 2. A fresh daemon every start: a module's protocol mount and accept loop do
#    not survive being re-served inside a running one.
"$L" daemon stop > /dev/null 2>&1
"$L" daemon start --detach > /dev/null 2>&1 || fail "logosctl daemon did not start ($(tail -3 "$LOGOSCTL_CONFIG_DIR/daemon/startup.err" 2>/dev/null))"
PID=$("$L" daemon status 2>/dev/null | field daemon.pid)
[ -n "$PID" ] || fail "no daemon pid"
say "RUN logosctl daemon pid $PID"
trap '"$L" daemon stop > /dev/null 2>&1; exit 0' TERM INT
/usr/bin/caffeinate -i -w "$PID" &
# Bundled with logosctl and of no use here: it would join the storage network.
"$L" module unload storage_module --no-dependents > /dev/null 2>&1

# 3. The wallet that pays.
"$L" module load liblogos_lez_rln_module > /dev/null 2>&1 || fail "liblogos_lez_rln_module did not load"
for _ in $(seq 30); do
  r=$(call liblogos_lez_rln_module use_network "$NETWORK_REF" | result)
  [ "$(field accepted <<< "$r")" = True ] && break
  [ "$(field retry <<< "$r")" = False ] && fail "use_network $NETWORK_REF refused: $r"
  sleep 2
done
PAYER=""
for _ in $(seq 300); do
  w=$(call liblogos_lez_rln_module wallet_status | result)
  case "$(field state <<< "$w")" in
    ready) PAYER=$(field payer <<< "$w"); break ;;
    failed) fail "wallet failed: $w" ;;
  esac
  sleep 2
done
[ -n "$PAYER" ] || fail "the wallet never came up: $w"
PAYER58=$(b58 "$PAYER")
say "RUN payer $PAYER58 ($PAYER)"

# 4. The libp2p node, with the fixed key.
"$L" module load libp2p_module > /dev/null 2>&1 || fail "libp2p_module did not load"
if [ ! -s "$GIFTER_HOME/node.key" ]; then
  k=$(call libp2p_module newPrivateKey secp256k1 | result | field value)
  [ -n "$k" ] || fail "could not make a node key"
  (umask 077; printf '%s' "$k" > "$GIFTER_HOME/node.key")
  "$L" module reload libp2p_module > /dev/null 2>&1   # newPrivateKey brought up a default node
  say "RUN made a new node key: the peer id changes, update the forum's default"
fi
CFG=$(printf '{"addrs":["/ip4/0.0.0.0/tcp/%s"],"transport":"tcp","privKey":"%s","mountGossipsub":false,"mountKad":false,"mountServiceDiscovery":false,"maxConnections":400,"maxInConnections":380,"maxOutConnections":20,"maxConnsPerPeer":2}' "$PORT" "$(cat "$GIFTER_HOME/node.key")")
r=$(call libp2p_module createNode "str:$CFG" | result)
[ "$(field success <<< "$r")" = True ] || fail "createNode: $(field error <<< "$r")"
r=$(call libp2p_module start | result)
[ "$(field success <<< "$r")" = True ] || fail "libp2p start: $(field error <<< "$r")"
PEER=$(call libp2p_module peerInfo | result | field value.peerId)
[ -n "$PEER" ] || fail "no peer id"
say "RUN peer $PEER listening on tcp/$PORT"

# 5. Serve, open: no authVerifiers. `wallet` "" is the registry module's own
#    payer, the account above.
"$L" module load rln_gifter_module > /dev/null 2>&1 || fail "rln_gifter_module did not load"
r=$(call rln_gifter_module serve "str:{\"config\":\"$CONFIG_ACCOUNT\",\"wallet\":\"\",\"maxRateLimit\":$MAX_RATE}" | result)
[ "$(field mounted <<< "$r")" = True ] || fail "serve: $r"
say "RUN serving /logos/rln/membership/1.0.0 (open, rate $MAX_RATE, registry $CONFIG_ACCOUNT)"

status() {
  local bal
  bal=$(call liblogos_lez_rln_module get_native_balance str: | result | field balance)
  /usr/bin/python3 - "$STATUS" "$PEER" "$PORT" "$EXT" "$PAYER" "$PAYER58" "$bal" "$PID" <<'PY'
import json, sys, time
out, peer, port, ext, payer, payer58, bal, pid = sys.argv[1:]
json.dump({"peerId": peer, "port": int(port), "externalAddress": ext,
           "multiaddr": f"/ip4/{ext}/tcp/{port}" if ext else "",
           "payer": payer, "payerBase58": payer58, "balance": bal, "daemonPid": int(pid),
           "updatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())},
          open(out, "w"), indent=2)
PY
  say "STATUS balance=${bal:-?}"
}
status

# 6. Stay up: renew the mapping, notice an address change, record the balance.
while sleep 600; do
  kill -0 "$PID" 2> /dev/null || { say "FAIL the logosctl daemon exited"; exit 1; }
  if NEW=$(/usr/bin/python3 "$MAP" ensure "$PORT" "$LEASE"); then
    if [ -n "$EXT" ] && [ "$NEW" != "$EXT" ]; then
      say "WARN external address changed $EXT -> $NEW: forum clients dialling /ip4/$EXT need the new address"
    fi
    EXT="$NEW"
  fi
  status
done
