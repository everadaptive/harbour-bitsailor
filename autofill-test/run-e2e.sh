#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Zach Dunton
# SPDX-License-Identifier: MIT

# Run the QML client through the daemon and the BitSailor provider.
# Expect: "E2E resolved: alice demo-app-pass-1 alice@example.com".
set -e
cd "$(dirname "$0")"
KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_OPTS="-q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
scp -P 2223 -i "$KEY" $SSH_OPTS e2e.qml defaultuser@127.0.0.1:/tmp/e2e.qml
ssh $SSH_OPTS -p 2223 -i "$KEY" root@127.0.0.1 'sh -s' <<'REMOTE'
runuser -u defaultuser -- env XDG_RUNTIME_DIR=/run/user/100000 \
    DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket \
    QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY=../../display/wayland-0 \
    timeout 30 qmlscene /tmp/e2e.qml >/dev/null 2>&1
journalctl --no-pager --since "-1 min" | grep -E "qml: E2E" | cut -d' ' -f5-
REMOTE
