#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Zach Dunton
# SPDX-License-Identifier: MIT

# Register the test account and add the seed logins. Run it once, with the
# server from start-vaultwarden.sh running. The script is safe to repeat.
set -e
cd "$(dirname "$0")"
URL=https://localhost:8222
EMAIL=autofill@example.test
PASSWORD='Autofill-Test-1!'
PY='python3.withPackages (p: [p.cryptography])'
nix-shell -p "$PY" --run "python3 register.py $URL $EMAIL '$PASSWORD'" || true
nix-shell -p "$PY" --run "python3 seed.py $URL $EMAIL '$PASSWORD'"
