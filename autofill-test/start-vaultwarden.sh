#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Zach Dunton
# SPDX-License-Identifier: MIT

# Run a local Vaultwarden test server with a self-signed certificate.
# The emulator reaches the host as 10.0.2.2. Test use only.
set -e
cd "$(dirname "$0")"
DIR="$PWD/server"
mkdir -p "$DIR/data"
if [ ! -f "$DIR/tls.crt" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
        -keyout "$DIR/tls.key" -out "$DIR/tls.crt" -subj '/CN=autofill-test' \
        -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:10.0.2.2' \
        -addext 'basicConstraints=critical,CA:TRUE'
fi
export DATA_FOLDER="$DIR/data" ROCKET_ADDRESS=0.0.0.0 ROCKET_PORT=8222
export SIGNUPS_ALLOWED=true DOMAIN=https://10.0.2.2:8222 WEB_VAULT_ENABLED=false
export ROCKET_TLS="{certs=\"$DIR/tls.crt\",key=\"$DIR/tls.key\"}"
exec vaultwarden
