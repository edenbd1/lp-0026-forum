# Sourced by the e2e scripts: RLN memberships for nodes on logos.test, the
# default network (LOGOS_FORUM_PRESET=logos.dev runs without RLN).
#
# A node that posts needs an active RLN membership. The forum asks its sponsor
# (gifter/, an open rln_gifter_module) for one by itself: a fresh node with no
# funds gets one in a few minutes, with nothing to do here. Pass every package:
#   <script> <logos_forum.lgx> <delivery.lgx> <storage.lgx> <rln.lgx> <lez_rln.lgx> <libp2p.lgx> <rln_gifter.lgx>
#
#   E2E_RLN_HOME=<dir>   keeps each node's RLN state in <dir>/<node name>, moved
#                        into the user dir before the node starts and back out
#                        when the run ends (moved, never copied: two copies of
#                        one keystore would reuse rate-limit slots), so a rerun
#                        does not cost the sponsor a membership per node
#   E2E_GIFTER_ACCOUNT=<base58>  the sponsor's paying account: its balance is
#                        printed as the run goes, which measures what a gifted
#                        membership costs
#   E2E_FUND=<command>   with the sponsor off ("gifter": "off"), run as
#                        `<command> <payer base58> <amount>` to fund a node
#   E2E_RLN_WAIT=<s>     how long to wait for a membership (default 900)

rln_dirs="module_data/liblogos_rln_module module_data/liblogos_lez_rln_module"

rln_restore() {  # rln_restore <user dir>
  [ -n "${E2E_RLN_HOME:-}" ] || return 0
  local keep="$E2E_RLN_HOME/$(basename "$1")" d
  for d in $rln_dirs; do
    [ -d "$keep/$d" ] || continue
    mkdir -p "$1/$(dirname "$d")"; rm -rf "${1:?}/$d"; mv "$keep/$d" "$1/$d"
  done
}

rln_save() {  # rln_save <user dir>: after the node has stopped
  [ -n "${E2E_RLN_HOME:-}" ] || return 0
  local keep="$E2E_RLN_HOME/$(basename "$1")" d
  for d in $rln_dirs; do
    [ -d "$1/$d" ] || continue
    mkdir -p "$keep/$(dirname "$d")"; rm -rf "${keep:?}/$d"; mv "$1/$d" "$keep/$d"
  done
}

b58() { python3 -c '
import sys
A="123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
b=bytes.fromhex(sys.argv[1]); n=int.from_bytes(b,"big"); s=""
while n: n,r=divmod(n,58); s=A[r]+s
print("1"*(len(b)-len(b.lstrip(b"\0")))+s)' "$1"; }

gifter_balance() {  # gifter_balance <label>: the sponsor's balance, when E2E_GIFTER_ACCOUNT is set
  [ -n "${E2E_GIFTER_ACCOUNT:-}" ] || return 0
  local b; b=$(curl -s -m 10 "${E2E_GIFTER_SEQUENCER:-http://209.38.241.182:3240}" -H 'content-type: application/json' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getAccountBalance\",\"params\":[\"$E2E_GIFTER_ACCOUNT\"]}" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["result"])' 2> /dev/null)
  echo "     sponsor balance $1: ${b:-unreadable}"
}

rln_ready() {  # rln_ready <user dir>: wait until the node can post
  local log="$1/module_data/logos_forum/forum.log" funded="" line payer amount
  for _ in $(seq 60); do grep -q "rln membership: \|network preset logos.dev" "$log" 2> /dev/null && break; sleep 1; done
  grep -q "rln membership: " "$log" 2> /dev/null || { echo "ok   $(basename "$1"): no RLN on this network"; return 0; }
  for _ in $(seq "${E2E_RLN_WAIT:-900}"); do
    if grep -q "rln membership: active" "$log"; then
      echo "ok   $(basename "$1"): RLN membership active"; return 0
    fi
    grep -q "rln membership: missing" "$log" && fail "$(basename "$1"): the RLN modules did not load (pass their .lgx after the others)"
    line=$(grep "rln membership: funding" "$log" | tail -1)
    if [ -n "$line" ] && [ -z "$funded" ]; then
      payer=$(sed -E 's/.*funding \(([0-9a-f]{64}) needs ([0-9]+).*/\1/' <<< "$line")
      amount=$(sed -E 's/.*funding \(([0-9a-f]{64}) needs ([0-9]+).*/\2/' <<< "$line")
      [ -n "${E2E_FUND:-}" ] || fail "$(basename "$1"): no RLN membership; fund $(b58 "$payer") with $amount native units on the registry's zone, or set E2E_FUND"
      echo "..   $(basename "$1"): funding $(b58 "$payer") with $amount"
      $E2E_FUND "$(b58 "$payer")" "$amount" || fail "E2E_FUND failed"
      funded=1
    fi
    sleep 1
  done
  fail "$(basename "$1"): no active RLN membership after ${E2E_RLN_WAIT:-900}s ($(grep 'rln membership:' "$log" | tail -1))"
}
