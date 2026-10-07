#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Zach Dunton
# SPDX-License-Identifier: MIT

# Test the manual fill flow without the UI. A dev picker process stands in
# for the UI app and selects an item. Usage: run-e2e-manual.sh [item id]
# The default item id is the GitHub item of the seed data. Print the ids
# with seed-server.sh.
# Expect: "E2E resolved: octocat-dev github-test-pw".
set -e
cd "$(dirname "$0")"
ITEM_ID="${1:-06087d9d-bd56-4297-9847-73b5605633d5}"
KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_OPTS="-q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
scp -P 2223 -i "$KEY" $SSH_OPTS e2e-manual.qml defaultuser@127.0.0.1:/tmp/e2e-manual.qml
ssh $SSH_OPTS -p 2223 -i "$KEY" root@127.0.0.1 'sh -s' "$ITEM_ID" <<'REMOTE'
ITEM_ID="$1"
U="runuser -u defaultuser -- env XDG_RUNTIME_DIR=/run/user/100000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket"
pkill -9 -f "harbour[-]bitsailor --"; pkill -x -9 harbour-bitsailor-dev 2>/dev/null
# Start the dev picker. It owns the UI bus name.
$U env BITSAILOR_MODE=autofill-dev-picker BITSAILOR_DEV_PICK_ID="$ITEM_ID" setsid /usr/bin/harbour-bitsailor >/dev/null 2>&1 &
sleep 2
$U env QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY=../../display/wayland-0 \
    timeout 40 qmlscene /tmp/e2e-manual.qml >/dev/null 2>&1
journalctl --no-pager --since "-1 min" | grep -E "qml: E2E|dev picker|autofill provider: manual" | cut -d' ' -f5- | cut -c1-200
REMOTE
