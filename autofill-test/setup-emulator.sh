#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Zach Dunton
# SPDX-License-Identifier: MIT

# Install the BitSailor RPM on the emulator, log in to the test server,
# start the autofill provider, and point the autofill daemon at it.
# Usage: setup-emulator.sh <path to harbour-bitsailor RPM>
# The rest of the stack comes from autofill-demo/scripts/seed.sh.
set -e
RPM="${1:?usage: setup-emulator.sh <rpm>}"
KEY="$HOME/SailfishOS/vmshare/ssh/private_keys/sdk"
SSH_OPTS="-q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
scp -P 2223 -i "$KEY" $SSH_OPTS "$RPM" defaultuser@127.0.0.1:/tmp/bitsailor.rpm

# The remote script comes over stdin, so that pkill patterns do not match
# the remote shell.
ssh $SSH_OPTS -p 2223 -i "$KEY" root@127.0.0.1 'sh -s' <<'REMOTE'
export XDG_RUNTIME_DIR=/run/user/100000
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/100000/dbus/user_bus_socket
U="runuser -u defaultuser -- env XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS=$DBUS_SESSION_BUS_ADDRESS"

$U systemctl --user stop harbour-bitsailor-autofill.service 2>/dev/null
pkill -9 -f "harbour[-]bitsailor"
rpm -Uvh --force /tmp/bitsailor.rpm

# The secrets daemon identifies the app by its command line. Run every mode
# through BITSAILOR_MODE, so that the command line stays /usr/bin/harbour-bitsailor.
$U env BITSAILOR_MODE=autofill-dev-login BITSAILOR_DEV_RESET=1 /usr/bin/harbour-bitsailor
$U env BITSAILOR_MODE=autofill-dev-login \
    BITSAILOR_DEV_SERVER=https://10.0.2.2:8222 \
    BITSAILOR_DEV_EMAIL=autofill@example.test \
    BITSAILOR_DEV_PASSWORD='Autofill-Test-1!' \
    BITSAILOR_DEV_INSECURE=1 \
    timeout 120 /usr/bin/harbour-bitsailor

$U systemctl --user daemon-reload
$U systemctl --user restart harbour-bitsailor-autofill.service

# Point the daemon at BitSailor. The mock provider unit stops, so that the
# two providers do not both answer.
$U systemctl --user stop autofill-mockprovider.service 2>/dev/null
printf 'provider=cz.chrastecky.bitsailor.Autofill\n' > /tmp/autofill-daemonrc
pkill -9 -f "autofill[-]daemon"
$U env AUTOFILL_DAEMON_CONFIG=/tmp/autofill-daemonrc QT_FORCE_STDERR_LOGGING=1 \
    setsid /usr/bin/autofill-daemon >/tmp/autofilld.log 2>&1 &
sleep 4
$U busctl --user list | grep -i -E "autofill|bitsailor"
REMOTE
