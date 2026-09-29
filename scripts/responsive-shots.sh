#!/usr/bin/env bash
# Screenshots of the forum view at every width from a phone to a wide desktop,
# in every state (list, open thread, alias, anonymous, offline, dialogs), using
# tests/qml/Harness.qml and a stand-in backend. Needs Qt 6.9 (`qml`).
#
#   scripts/responsive-shots.sh [out-dir]     # default: docs/responsive
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$here/docs/responsive}; mkdir -p "$out"
QML=${QML:-qml}
export QT_QUICK_CONTROLS_STYLE=Basic
sizes="360x780 414x896 600x900 700x820 900x820 1280x820 1600x1000"
states="list thread alias anonymous offline newtopic accounts welcome stress"
for s in $sizes; do
  w=${s%x*} h=${s#*x}
  for st in $states; do
    "$QML" "$here/tests/qml/Harness.qml" -- "$w" "$h" "$st" "$out/$st-$s.png" 2> /dev/null
  done
done
ls "$out" | wc -l | xargs echo "screenshots:"
