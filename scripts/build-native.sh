#!/usr/bin/env bash
# Builds ezinject and libinputhookpp.so (the native hook) for LG webOS (ARM) and copies
# both into app/service/inputhook/, where the app build picks them up.
#
# Needs: bash, git, cmake (>= 3.28 is fine, the toolchain ships its own), wget, tar, sha256sum.
# Downloads the openlgtv buildroot toolchain (~300 MB) on the first run.
#
# Environment overrides:
#   WORK=<dir>           build directory (default: <repo>/.build)
#   TOOLCHAIN_DIR=<dir>  use an already unpacked and relocated toolchain
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${WORK:-$ROOT/.build}"

TOOLCHAIN_URL="https://github.com/openlgtv/buildroot-nc4/releases/download/2026.08-webos/arm-lgtv-linux-gnueabi_sdk-buildroot-x86_64.tar.gz"
TOOLCHAIN_SHA256="a8d82460b3a96a96cb2f3847df19d2c641672a9d8facf7141502c29dd86f9000"
EZINJECT_COMMIT="607055c06b037eadc3992008940d99bdc4a14f53"

TC="${TOOLCHAIN_DIR:-$WORK/arm-lgtv-linux-gnueabi_sdk-buildroot}"
mkdir -p "$WORK"

if [ ! -x "$TC/bin/cmake" ]; then
    echo "==> Downloading toolchain"
    wget -q -O "$WORK/toolchain.tar.gz" "$TOOLCHAIN_URL"
    echo "$TOOLCHAIN_SHA256  $WORK/toolchain.tar.gz" | sha256sum -c -
    tar -C "$WORK" -xf "$WORK/toolchain.tar.gz"
    (cd "$TC" && ./relocate-sdk.sh)
fi

export PATH="$TC/bin:$PATH"
TOOLCHAIN_FILE="$TC/share/buildroot/toolchainfile.cmake"
SYSROOT="$TC/arm-lgtv-linux-gnueabi/sysroot"

echo "==> Building ezinject ($EZINJECT_COMMIT)"
if [ ! -d "$WORK/ezinject/.git" ]; then
    git clone -q https://github.com/smx-smx/ezinject "$WORK/ezinject"
fi
git -C "$WORK/ezinject" checkout -q "$EZINJECT_COMMIT"
git -C "$WORK/ezinject" submodule update --init --recursive -q
cmake -S "$WORK/ezinject" -B "$WORK/ezinject/build" \
    -DEZ_LIBC=glibc -DUSE_FRIDA_GUM=1 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
cmake --build "$WORK/ezinject/build" --parallel
cmake --install "$WORK/ezinject/build" > /dev/null

echo "==> Building libinputhookpp.so"
cmake -S "$ROOT/native" -B "$WORK/native-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE"
cmake --build "$WORK/native-build" --parallel

OUT="$ROOT/app/service/inputhook"
mkdir -p "$OUT"
cp "$SYSROOT/bin/ezinject" "$OUT/ezinject"
cp "$WORK/native-build/libinputhookpp.so" "$OUT/libinputhookpp.so"
echo "==> Done: $OUT"
ls -l "$OUT"
