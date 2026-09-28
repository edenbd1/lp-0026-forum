# Basecamp for Linux in Docker, as used for docs/e2e.md#linux. Unpack the official
# x86_64 AppImage into sq/ (readelf offset + unsquashfs) and put the .lgx files in lgx/.

#!/bin/bash
# Inside the container: install the forum + deps, start Basecamp Linux 0.3.0 on a virtual display.
set -e
rm -rf /w/u && mkdir -p /w/u
bash /w/install-local.sh /w/u /w/lgx/*.lgx
Xvfb :99 -screen 0 1600x1000x24 >/w/xvfb.log 2>&1 &
export DISPLAY=:99
sleep 2
cd /w/sq
# A pty, or Basecamp's log redirector fails on a bad descriptor.
script -qefc "dbus-run-session -- ./AppRun --user-dir /w/u" /w/basecamp-typescript > /dev/null 2>&1 &
sleep infinity
