#!/bin/bash
# Install the forum's RLN membership sponsor on this Mac as the LaunchAgent
# co.logos.forum-gifter.
#
#   gifter/install.sh <rln_gifter_module.lgx> [GIFTER_HOME] [PORT]
#
# GIFTER_HOME (default ~/logos-forum-gifter) receives logosctl 0.3.1 (the
# headless Logos runtime), a logosctl session with libp2p_module 1.1.0 and
# liblogos_lez_rln_module 4.2.1 from the official catalog and the given
# rln_gifter_module, the service script and its logs. Then the agent starts;
# scripts/gifter-status.sh says what it is doing and which account to fund.
set -euo pipefail
LGX=${1:?usage: gifter/install.sh <rln_gifter_module.lgx> [GIFTER_HOME] [PORT]}
GH=${2:-$HOME/logos-forum-gifter}
PORT=${3:-24026}
here=$(cd "$(dirname "$0")" && pwd)
LOGOSCTL_URL=https://github.com/logos-co/logos-logoscore-cli/releases/download/0.3.1/logosctl-aarch64-macos.tar.gz

mkdir -p "$GH/bin" "$GH/logs" "$GH/pkg" "$GH/session"
if [ ! -x "$GH/logosctl/bin/logosctl" ]; then
  # curl, not a browser: no quarantine flag on an unsigned binary.
  (cd "$GH" && curl -fsSL "$LOGOSCTL_URL" | tar xz && mv logosctl-aarch64-macos logosctl)
fi
install -m 755 "$here/run.sh" "$GH/bin/run.sh"
install -m 755 "$here/upnp-map.py" "$GH/bin/upnp-map.py"
install -m 644 "$LGX" "$GH/pkg/rln_gifter_module.lgx"

export LOGOSCTL_CONFIG_DIR="$GH/session"
L="$GH/logosctl/bin/logosctl"
launchctl bootout "gui/$(id -u)/co.logos.forum-gifter" 2> /dev/null || true
"$L" daemon stop > /dev/null 2>&1 || true
"$L" daemon start --detach > /dev/null
"$L" catalog source http > /dev/null
"$L" package install libp2p_module --version 1.1.0 -y > /dev/null
"$L" package install liblogos_lez_rln_module --version 4.2.1 -y > /dev/null
"$L" package install "$GH/pkg/rln_gifter_module.lgx" -y > /dev/null
"$L" daemon stop > /dev/null

sed -e "s|@HOME@|$GH|g" -e "s|@PORT@|$PORT|g" "$here/co.logos.forum-gifter.plist" \
  > "$HOME/Library/LaunchAgents/co.logos.forum-gifter.plist"
launchctl bootstrap "gui/$(id -u)" "$HOME/Library/LaunchAgents/co.logos.forum-gifter.plist"
echo "installed: $GH, LaunchAgent co.logos.forum-gifter, log $GH/logs/gifter.log"
