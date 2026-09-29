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
#   scripts/e2e-two-nodes.sh <logos_forum.lgx> <delivery_module.lgx> <storage_module.lgx>
set -euo pipefail
BASECAMP=${BASECAMP:-$HOME/Applications/LogosBasecamp-0.3.0.app/Contents/MacOS/LogosBasecamp}
here=$(cd "$(dirname "$0")" && pwd)
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
start() {
  (LOGOS_FORUM_NAME="e2e $$" "$BASECAMP" --user-dir "$1" > "$1.log" 2>&1 &)
  local pid=""; for _ in $(seq 30); do pid=$(pgrep -n -f "LogosBasecamp.bin --user-dir $1\$" || true); [ -n "$pid" ] && break; sleep 1; done
  [ -n "$pid" ] || fail "Basecamp did not start for $1"; sleep 10
  ui "$pid" 'click button "Logos Forum" of window 1' > /dev/null
  echo "$pid"
}
wait_for() {  # wait_for <seconds> <description> <command…>
  local t=$1 what=$2; shift 2
  for _ in $(seq "$t"); do "$@" && { echo "ok   $what"; return; }; sleep 1; done
  fail "$what"
}
stop() { pkill -f "user-dir $1\$" || true; for _ in $(seq 20); do pgrep -f "user-dir $1\$" > /dev/null || break; sleep 1; done; pkill -f "$1/" || true; sleep 1; }

stop $A; stop $B; rm -rf $A $B
for d in $A $B; do "$here/install-local.sh" "$d" "$@" > /dev/null; done

pa=$(start $A); pb=$(start $B)
wait_for 90 "both nodes joined the forum" bash -c "grep -q subscribed $A/module_data/logos_forum/forum.log && grep -q subscribed $B/module_data/logos_forum/forum.log"
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
click_in_window "$pb" 200 250   # the first row of the topic list (under the search field)
sleep 1.5
ui "$pb" 'set value of text field -1 of group 1 of window 1 to "An anonymous reply from B."' > /dev/null
click_ax "$pb" 'menu button 2 of group 1 of window 1' 2> /dev/null || click_ax "$pb" 'menu button "Account 1" of group 1 of window 1'
sleep 0.7
osascript -e 'tell application "System Events" to key code 125' -e 'tell application "System Events" to key code 125' -e 'tell application "System Events" to key code 36'
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
