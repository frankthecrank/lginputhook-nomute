#!/usr/bin/env bash
# Builds the installable .ipk. Run scripts/build-native.sh first.
#
# Needs: Node.js (>= 18) and npm. Output: app/org.webosbrew.inputhook.nomute_<version>_all.ipk
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT/app"

for f in ezinject libinputhookpp.so; do
    if [ ! -f "service/inputhook/$f" ]; then
        echo "Missing service/inputhook/$f - run scripts/build-native.sh first." >&2
        exit 1
    fi
done

npm install --no-audit --no-fund
rm -rf dist
npx webpack --env production
npx ares-package -n dist
ls -l ./*.ipk
