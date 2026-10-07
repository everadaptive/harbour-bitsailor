#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Zach Dunton
# SPDX-License-Identifier: MIT
#
# Build and run the host tests of the BitSailor autofill provider: the unit
# test of the helpers, and the integration test of the provider on a stub
# core. They need Nix (Qt6, CMake, PyGObject, dbus). From this directory:
#
#   ./run-host-tests.sh [build directory]
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${1:-$HERE/build-host}"

nix-shell -p cmake gcc kdePackages.qtbase \
    --run "cmake -S '$HERE' -B '$BUILD' > /dev/null && cmake --build '$BUILD' -j4"

"$BUILD/autofillsave_test"
nix-shell -p 'python3.withPackages (p: [p.pygobject3])' dbus glib gobject-introspection kdePackages.qtbase \
    --run "dbus-run-session -- python3 '$HERE/provider_v2.py' '$BUILD'"
