#!/usr/bin/env bash
# The full scenario, on real Basecamp nodes joined to logos.test, driven through
# the forum's own interface and checked in every node's own store.
#
#   A · Alice      topic as her account            → B, C, D store it, same author key
#   B · Bob        reply under the alias "Ghost"   → others store mode=alias, alias, Bob's key
#   C             two anonymous replies           → two keys, none of them C's, not equal
#   A · Alice2     a second account, a new topic   → a different key from Alice
#   A · rotation   Alice2 rotates, then replies    → a key that is neither Alice's nor Alice2's old one
#   D · offline    posts with no network, closes,  → the post reaches everyone after restart
#                  reopens online
#   E · newcomer   a blank install joins at the end → recovers every post
#
# macOS; Basecamp 0.3.0, cliclick, sqlite3, Accessibility permission.
#   scripts/e2e-full.sh <logos_forum.lgx> <delivery_module.lgx> <storage_module.lgx>
set -euo pipefail
# Every node runs on this machine, so they may name and dial local addresses
# (LOGOS_FORUM_LOCAL_PEERS); on the real network only public ones are used.
BASECAMP=${BASECAMP:-$HOME/Applications/LogosBasecamp-0.3.0.app/Contents/MacOS/LogosBasecamp}
here=$(cd "$(dirname "$0")" && pwd)
FORUM="e2e-full-$(date +%s)"
R=/tmp/forum-full
A=$R-a B=$R-b C=$R-c D=$R-d E=$R-e
PASS=0
fail() { echo "FAIL: $*" >&2; exit 1; }
ok() { echo "ok   $*"; PASS=$((PASS + 1)); }
ui() { local pid=$1; shift; osascript -e "tell application \"System Events\" to tell (first process whose unix id is $pid)" -e "$*" -e "end tell"; }
front() {
  ui "$1" 'set frontmost to true' > /dev/null
  for _ in 1 2 3 4 5 6 7 8; do
    [ "$(osascript -e 'tell application "System Events" to get unix id of first process whose frontmost is true')" = "$1" ] && return 0
    sleep 0.5; ui "$1" 'set frontmost to true' > /dev/null
  done
  fail "Basecamp $1 is not in front; not clicking into another window"
}
click_ax() {
  local pos; pos=$(ui "$1" "get {position, size} of $2" 2> /dev/null | tr -d ' ') || return 1
  [ -n "$pos" ] || return 1
  front "$1"; IFS=, read -r x y w h <<< "$pos"; cliclick "c:$((x + w / 2)),$((y + h / 2))"
}
click_in_window() { front "$1"; local pos; pos=$(ui "$1" 'get position of window 1' | tr -d ' '); IFS=, read -r x y <<< "$pos"; cliclick "c:$((x + $2)),$((y + $3))"; }
key() { osascript -e "tell application \"System Events\" to key code $1"; }
db() { sqlite3 "$1/module_data/logos_forum/forum.db" "$2" 2> /dev/null || true; }
start() {  # start <dir> [offline]
  if [ "${2:-}" = offline ]; then
    (LOGOS_FORUM_LOCAL_PEERS=1 LOGOS_FORUM_NAME="$FORUM" sandbox-exec -f "$R-offline.sb" "$BASECAMP" --user-dir "$1" > "$1.log" 2>&1 &)
  else
    (LOGOS_FORUM_LOCAL_PEERS=1 LOGOS_FORUM_NAME="$FORUM" "$BASECAMP" --user-dir "$1" > "$1.log" 2>&1 &)
  fi
  local pid=""; for _ in $(seq 40); do pid=$(pgrep -n -f "LogosBasecamp.bin --user-dir $1\$" || true); [ -n "$pid" ] && break; sleep 1; done
  [ -n "$pid" ] || fail "Basecamp did not start for $1"; sleep 10
  ui "$pid" 'set position of window 1 to {0, 33}' > /dev/null; ui "$pid" 'set size of window 1 to {1512, 900}' > /dev/null
  ui "$pid" 'click button "Logos Forum" of window 1' > /dev/null
  for _ in $(seq 90); do grep -q "subscribed" "$1/module_data/logos_forum/forum.log" 2> /dev/null && break; sleep 1; done
  echo "$pid"
}
stop() { pkill -f "user-dir $1\$" || true; for _ in $(seq 20); do pgrep -f "user-dir $1\$" > /dev/null || break; sleep 1; done; pkill -f "$1/" || true; sleep 1; }
wait_for() { local t=$1 what=$2; shift 2; for _ in $(seq "$t"); do "$@" && { ok "$what"; return; }; sleep 1; done; fail "$what"; }
has_posts() { [ "$(db "$1" "select count(*) from posts")" -ge "$2" ]; }
has_title() { [ "$(db "$1" "select count(*) from posts where title='$2'")" = 1 ]; }
has_body() { [ "$(db "$1" "select count(*) from posts where body='$2'")" = 1 ]; }
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
open_dialog() {  # open_dialog <pid> <button> <element the dialog shows>: click until the dialog is really open
  # A click can land before the view is ready (a node just restarted) or be
  # lost; typing into a dialog that did not open types into the search field.
  local i
  for i in $(seq 15); do
    click_ax "$1" "button \"$2\" of group 1 of window 1" > /dev/null 2>&1 || true
    sleep 1
    [ "$(ui "$1" "exists $3 of group 1 of window 1" 2> /dev/null)" = true ] && return 0
  done
  fail "the $2 dialog did not open"
}
new_topic() {  # new_topic <pid> <title> <body> <mode> [alias]
  open_dialog "$1" "New topic" 'button "Post"' 
  ui "$1" "set value of text field -2 of group 1 of window 1 to \"$2\"" > /dev/null
  ui "$1" "set value of text field -1 of group 1 of window 1 to \"$3\"" > /dev/null
  pick_mode "$1" "$4"
  [ "$4" = 1 ] && ui "$1" "set value of text field -1 of group 1 of window 1 to \"$5\"" > /dev/null
  sleep 0.5; click_ax "$1" 'button "Post" of group 1 of window 1' || fail "no Post button"; sleep 2
}
reply_top() {  # reply_top <pid> <body> <mode> [alias]: reply to the most recently active topic
  click_in_window "$1" 200 250; sleep 1.5
  ui "$1" "set value of text field -1 of group 1 of window 1 to \"$2\"" > /dev/null
  pick_mode "$1" "$3"
  [ "$3" = 1 ] && ui "$1" "set value of text field -1 of group 1 of window 1 to \"$4\"" > /dev/null
  sleep 0.5; click_ax "$1" 'button "Reply" of group 1 of window 1' || fail "no Reply button"; sleep 2
}
new_account() {  # new_account <pid> <label> <user dir>: created and selected, checked in the store
  local i
  for i in 1 2 3; do
    open_dialog "$1" "Accounts…" 'button "Create"'
    ui "$1" "set value of text field -1 of group 1 of window 1 to \"$2\"" > /dev/null; sleep 0.3
    click_ax "$1" 'button "Create" of group 1 of window 1' || fail "no Create button"; sleep 1.5
    click_ax "$1" 'button "Close" of group 1 of window 1'; sleep 0.6
    [ "$(db "$3" "select count(*) from accounts where label='$2' and selected=1")" = 1 ] && return 0
  done
  fail "account $2 was not created"
}
rotate() {
  open_dialog "$1" "Accounts…" 'button "Rotate now"' 
  click_ax "$1" 'button "Rotate now" of group 1 of window 1' || fail "no Rotate button"; sleep 1
  click_ax "$1" 'button "Close" of group 1 of window 1'; sleep 0.6
}
key_of() { db "$1" "select hex(pk) from accounts where label='$2'"; }
author_of() { db "$1" "select hex(author) from posts where title='$2' or body='$2'"; }

cat > "$R-offline.sb" <<'SB'
(version 1)
(allow default)
(deny network-outbound (remote ip "*:*"))
(allow network-outbound (remote ip "localhost:*"))
(allow network-outbound (remote unix-socket))
SB
for d in $A $B $C $D $E; do stop $d; rm -rf $d; "$here/install-local.sh" "$d" "$@" > /dev/null; done
echo "forum: $FORUM"

pa=$(start $A); pb=$(start $B); pc=$(start $C); pd=$(start $D)
ok "four nodes joined the forum"

# ── Alice: a topic under her account
new_account $pa "Alice" $A
new_topic $pa "Welcome to the test forum" "Alice here, posting under my account." 0
for n in $B $C $D; do wait_for 60 "$(basename $n) received Alice's topic" has_title $n "Welcome to the test forum"; done
ka=$(key_of $A Alice)
for n in $B $C $D; do [ "$(author_of $n "Welcome to the test forum")" = "$ka" ] || fail "author key differs on $n"; done
ok "the topic carries Alice's key on every node"

# ── Bob: an alias reply
new_account $pb "Bob" $B
reply_top $pb "Replying under a name I chose." 1 "Ghost"
for n in $A $C $D; do wait_for 60 "$(basename $n) received Bob's alias reply" has_body $n "Replying under a name I chose."; done
[ "$(db $A "select mode||'|'||alias from posts where body='Replying under a name I chose.'")" = "1|Ghost" ] || fail "alias reply not stored as alias Ghost"
[ "$(author_of $A "Replying under a name I chose.")" = "$(key_of $B Bob)" ] || fail "alias reply not signed by Bob's key"
ok "the alias reply shows as Ghost and is signed by Bob's key"

# ── C: two anonymous replies
reply_top $pc "First anonymous reply." 2
reply_top $pc "Second anonymous reply." 2
for n in $A $B $D; do wait_for 60 "$(basename $n) received both anonymous replies" has_body $n "Second anonymous reply."; done
k1=$(author_of $A "First anonymous reply."); k2=$(author_of $A "Second anonymous reply.")
[ "$k1" != "$k2" ] || fail "two anonymous replies share a key"
for k in $(db $C "select hex(pk) from accounts"); do [ "$k" != "$k1" ] && [ "$k" != "$k2" ] || fail "an anonymous reply is signed by one of C's accounts"; done
[ "$(db $A "select count(*) from posts where mode=2")" = 2 ] || fail "anonymous replies not stored as anonymous"
ok "the two anonymous replies have two unrelated keys, none of C's"

# ── Alice2: a second account
new_account $pa "Alice2" $A
new_topic $pa "A second identity" "Same person, different account." 0
wait_for 60 "b received Alice2's topic" has_title $B "A second identity"
k_a2=$(key_of $A Alice2)
[ "$(author_of $B "A second identity")" = "$k_a2" ] && [ "$k_a2" != "$ka" ] || fail "second account not signing with its own key"
ok "the second account signs with a key unrelated to the first"

# ── rotation
rotate $pa
k_rot=$(key_of $A Alice2)
[ "$k_rot" != "$k_a2" ] && [ "$k_rot" != "$ka" ] || fail "rotation did not replace the key"
reply_top $pa "Posted after rotating my key." 0
wait_for 60 "b received the post made after rotation" has_body $B "Posted after rotating my key."
[ "$(author_of $B "Posted after rotating my key.")" = "$k_rot" ] || fail "post after rotation not signed by the new key"
ok "after rotation, posts carry the new key and nothing links it to the old one"

# ── D offline: posts with no network, closes, reopens online
stop $D; pd=$(start $D offline)
new_topic $pd "Written offline" "D had no network when this was posted." 0
[ "$(db $D "select count(*) from outbox")" -ge 1 ] || fail "offline post not in D's outbox"
has_title $A "Written offline" && fail "an offline post reached A"
ok "the offline post waits in D's outbox and reached no one"
stop $D; pd=$(start $D)
for n in $A $B $C; do wait_for 120 "$(basename $n) received D's offline post after D reconnected" has_title $n "Written offline"; done
wait_for 60 "D's outbox emptied once the network confirmed it" bash -c "[ \"\$(sqlite3 $D/module_data/logos_forum/forum.db 'select count(*) from outbox')\" = 0 ]"

# ── E: a newcomer
total=$(db $A "select count(*) from posts")
pe=$(start $E)
wait_for 150 "the newcomer recovered all $total posts" has_posts $E "$total"
[ "$(db $E "select group_concat(id) from (select id from posts order by id)")" = "$(db $A "select group_concat(id) from (select id from posts order by id)")" ] || fail "the newcomer's posts differ from A's"
ok "the newcomer holds exactly the same posts as A"

for d in $A $B $C $D $E; do stop $d; done
echo "PASS ($PASS checks)"
