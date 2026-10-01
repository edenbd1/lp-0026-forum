#!/usr/bin/env bash
# Two real Basecamp nodes on the logos.test network, driven through the forum's
# own interface, checked in each node's own store.
#
#   1. A posts a topic                      -> B receives it live, signature verified
#   2. B replies anonymously                -> A receives it, under a key that is not B's
#   3. B is wiped and restarted (a fresh    -> B asks its peers, A answers with a snapshot
#      install with no history)                on Logos Storage, B downloads and merges it
#
# macOS; needs Basecamp 0.3.0 ($BASECAMP), cliclick, sqlite3, and Accessibility
# permission for the terminal (System Settings → Privacy → Accessibility).
#
#   scripts/e2e-two-nodes.sh <logos_forum.lgx> <delivery_module.lgx> <storage_module.lgx> \
#       [<liblogos_rln_module.lgx> <liblogos_lez_rln_module.lgx>]
#
# With LOGOS_FORUM_PRESET=logos.test posting nodes need RLN memberships: see
# scripts/e2e-rln.sh (E2E_RLN_HOME, E2E_FUND).
set -euo pipefail
# Every node runs on this machine, so they may name and dial local addresses
# (LOGOS_FORUM_LOCAL_PEERS); on the real network only public ones are used.
BASECAMP=${BASECAMP:-$HOME/Applications/LogosBasecamp-0.3.0.app/Contents/MacOS/LogosBasecamp}
here=$(cd "$(dirname "$0")" && pwd)
. "$here/e2e-rln.sh"
A=/tmp/forum-e2e-a B=/tmp/forum-e2e-b
fail() { echo "FAIL: $*" >&2; exit 1; }
ui() { local pid=$1; shift; osascript -e "tell application \"System Events\" to tell (first process whose unix id is $pid)" -e "$*" -e "end tell"; }
front() {  # bring a Basecamp to the front, and refuse to click anything else
  ui "$1" 'set frontmost to true' > /dev/null
  for _ in 1 2 3 4 5 6; do
    [ "$(osascript -e 'tell application "System Events" to get unix id of first process whose frontmost is true')" = "$1" ] && return 0
    sleep 0.5; ui "$1" 'set frontmost to true' > /dev/null
  done
  fail "Basecamp $1 is not in front; not clicking into another window"
}
click_ax() {  # real mouse click on an accessibility element: QML ignores AX presses
  local pos; pos=$(ui "$1" "get {position, size} of $2" 2> /dev/null | tr -d ' ') || return 1
  [ -n "$pos" ] || return 1
  front "$1"
  IFS=, read -r x y w h <<< "$pos"
  cliclick "c:$((x + w / 2)),$((y + h / 2))"
}
click_in_window() {  # click_in_window <pid> <dx> <dy>: a point relative to the window's corner
  front "$1"
  local pos; pos=$(ui "$1" 'get position of window 1' | tr -d ' '); IFS=, read -r x y <<< "$pos"
  cliclick "c:$((x + $2)),$((y + $3))"
}
db() { sqlite3 "$1/module_data/logos_forum/forum.db" "$2"; }
pick_mode() {  # pick_mode <pid> <0 account | 1 alias | 2 anonymous>: click the item itself, then check it took
  local attempt cur
  # Already on that mode: leave the picker alone (opening it for nothing can
  # leave its popup open, and the next click then only closes the popup).
  cur=$(ui "$1" 'get name of menu button -1 of group 1 of window 1' 2> /dev/null)
  case "$2:$cur" in 1:Alias|2:Anonymous) return 0;; 0:Alias|0:Anonymous) ;; 0:*) return 0;; esac
  for attempt in 1 2 3 4; do
    local pos; pos=$(ui "$1" 'get {position, size} of menu button -1 of group 1 of window 1' | tr -d ' ')
    IFS=, read -r cx cy cw ch <<< "$pos"
    local wpos; wpos=$(ui "$1" 'get {position, size} of window 1' | tr -d ' '); IFS=, read -r wx wy ww wh <<< "$wpos"
    click_ax "$1" 'menu button -1 of group 1 of window 1' || fail "no Post-as picker"; sleep 0.8
    local y
    if [ $((cy + ch + 3 * ch)) -gt $((wy + wh)) ]; then y=$((cy - (3 - $2) * ch + ch / 2))   # opens upward
    else y=$((cy + ch + $2 * ch + ch / 2)); fi                                           # opens downward
    front "$1"; cliclick "c:$((cx + cw / 2)),$y"; sleep 0.6
    # The picker names its choice: "Alias", "Anonymous", or the account's label.
    local now; now=$(ui "$1" 'get name of menu button -1 of group 1 of window 1' 2> /dev/null)
    case "$2:$now" in 1:Alias|2:Anonymous) return 0;; 0:Alias|0:Anonymous) ;; 0:*) return 0;; esac
    osascript -e 'tell application "System Events" to key code 53' > /dev/null 2>&1   # close a popup left open
    sleep 0.5
  done
  fail "could not pick posting mode $2"
}
start() {
  rln_restore "$1"
  (LOGOS_FORUM_LOCAL_PEERS=1 LOGOS_FORUM_FETCH_SNAPSHOTS=1 LOGOS_FORUM_NAME="e2e $$" "$BASECAMP" --user-dir "$1" > "$1.log" 2>&1 &)
  local pid=""; for _ in $(seq 30); do pid=$(pgrep -n -f "LogosBasecamp.bin --user-dir $1\$" || true); [ -n "$pid" ] && break; sleep 1; done
  [ -n "$pid" ] || fail "Basecamp did not start for $1"; sleep 10
  # Basecamp may still be loading its sidebar: retry until the forum has started.
  for _ in $(seq 10); do
    ui "$pid" 'click button "Logos Forum" of window 1' > /dev/null 2>&1 || true
    sleep 3; [ -f "$1/module_data/logos_forum/forum.log" ] && break
  done
  echo "$pid"
}
wait_for() {  # wait_for <seconds> <description> <command…>
  local t=$1 what=$2; shift 2
  for _ in $(seq "$t"); do "$@" && { echo "ok   $what"; return; }; sleep 1; done
  fail "$what"
}
stop() { pkill -f "user-dir $1\$" || true; for _ in $(seq 20); do pgrep -f "user-dir $1\$" > /dev/null || break; sleep 1; done; pkill -f "$1/" || true; sleep 1; }
# Never leave a node behind: one left running fills the disk with its logs.
cleanup() { for d in $A $B; do stop "$d"; rln_save "$d"; done; }
trap cleanup EXIT

stop $A; stop $B; rm -rf $A $B
for d in $A $B; do "$here/install-local.sh" "$d" "$@" > /dev/null; done

pa=$(start $A); pb=$(start $B)
wait_for 90 "both nodes joined the forum" bash -c "grep -q subscribed $A/module_data/logos_forum/forum.log && grep -q subscribed $B/module_data/logos_forum/forum.log"
rln_ready $A; rln_ready $B
sleep 5

# 1. A posts a topic
ui "$pa" 'set frontmost to true' > /dev/null; sleep 0.5
ui "$pa" 'click button "New topic" of group 1 of window 1' > /dev/null; sleep 1
# Fields counted from the end: the topic search field comes first on screen.
ui "$pa" 'set value of text field -2 of group 1 of window 1 to "Hello from node A"' > /dev/null
ui "$pa" 'set value of text field -1 of group 1 of window 1 to "Signed by A, verified by B."' > /dev/null
click_ax "$pa" 'button "Post" of group 1 of window 1'
wait_for 60 "B received A's topic" bash -c "[ \"\$(sqlite3 $B/module_data/logos_forum/forum.db 'select count(*) from posts where kind=0')\" = 1 ]"
[ "$(db $A 'select hex(author) from posts where kind=0')" = "$(db $B 'select hex(author) from posts where kind=0')" ] \
  || fail "the author key differs between the two stores"
echo "ok   same author key on both nodes"

# 2. B replies anonymously
ui "$pb" 'set frontmost to true' > /dev/null; sleep 0.5
# Topic rows are accessible buttons named after the topic: open it by name.
ui "$pb" 'perform action "AXPress" of button "Hello from node A" of group 1 of window 1' > /dev/null
sleep 1.5
pick_mode "$pb" 2
ui "$pb" 'set value of text field -1 of group 1 of window 1 to "An anonymous reply from B."' > /dev/null
sleep 0.5
click_ax "$pb" 'button "Reply" of group 1 of window 1'
wait_for 60 "A received B's anonymous reply" bash -c "[ \"\$(sqlite3 $A/module_data/logos_forum/forum.db 'select count(*) from posts where kind=1 and mode=2')\" = 1 ]"
reply_key=$(db $A 'select hex(author) from posts where kind=1')
while read -r k; do [ "$k" != "$reply_key" ] || fail "the anonymous reply is signed by one of B's accounts"; done < <(db $B 'select hex(pk) from accounts')
echo "ok   the anonymous key is none of B's accounts"

# 3. B as a fresh install recovers history from A through Logos Storage
stop $B; rm -f $B/module_data/logos_forum/forum.db*
pb=$(start $B)
wait_for 120 "fresh B loaded history from a peer's snapshot" bash -c "[ \"\$(sqlite3 $B/module_data/logos_forum/forum.db 'select count(*) from posts' 2>/dev/null)\" = 2 ]"
grep -q "snapshot imported" $B/module_data/logos_forum/forum.log || fail "the posts did not come from a snapshot"
echo "ok   through Logos Storage: $(grep 'fetching snapshot' $B/module_data/logos_forum/forum.log | tail -1 | awk '{print $NF}')"

stop $A; stop $B
echo "PASS"
