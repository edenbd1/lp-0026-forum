#!/usr/bin/env bash
# The hourly activity lines a node writes to its own forum.log: how many other
# nodes asked for history, how many of them were fresh installs, how many new
# posts arrived. Counts only; nothing identifying is kept.
#   scripts/activity.sh [<Basecamp user dir>]      default: ~/logos-forum-node
dir=${1:-$HOME/logos-forum-node}
cat "$dir/module_data/logos_forum/forum.log.1" "$dir/module_data/logos_forum/forum.log" 2>/dev/null | grep -a "activity (last hour" || echo "no activity line yet (one is written every hour)"
