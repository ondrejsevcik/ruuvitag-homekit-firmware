#!/usr/bin/env bash
# Build the native HomeKit firmware for RuuviTag (ruuvitag_b) and package it for
# over-the-air update with the stock Ruuvi bootloader.
#
# Usage: tools/build_homekit.sh
#
# Outputs (dist/):
#   ruuvitag_b_homekit_<version>_dfu_app.zip   Update package for nRF Connect / nRF Device Firmware Update
#   ruuvitag_b_homekit_<version>_app.hex       Application only, for SWD flashing
# and prints the HomeKit setup code (kept in homekit-setup.json, created on first build).
#
# Requirements: arm-none-eabi-gcc, make, python3 with the 'cryptography' package, curl, unzip.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SDK_DIR="nRF5_SDK_15.3.0_59ac345"
SDK_URL="https://nsscprodmedia.blob.core.windows.net/prod/software-and-other-downloads/sdks/nrf5/binaries/nrf5sdk153059ac345.zip"
SDK_SHA256="9c9307d757d109007f2fcc4e547ac10b1a9fd9faa477e8dbf3f7e1624c4801a5"
KEY_URL="https://github.com/ruuvi/ruuvi.nrf5_sdk15_bootloader.c/releases/download/3.0.0/ruuvi_open_private.pem"
KEY_SHA256="abe4f398b298337c46bf522dee7e67977970dfbaba7aa38b8d7e097fe08cfdca"
CACHE=".cache"
TARGET="src/targets/ruuvitag_b/armgcc"

fetch() { # url file sha256
    if [[ ! -f "$2" ]]; then
        echo "Downloading $1"
        curl -fsSL -o "$2.tmp" "$1"
        mv "$2.tmp" "$2"
    fi
    echo "$3  $2" | sha256sum -c --quiet - || { echo "Checksum mismatch for $2" >&2; exit 1; }
}

mkdir -p "$CACHE" dist

git submodule update --init --recursive

if [[ ! -d "$SDK_DIR" ]]; then
    fetch "$SDK_URL" "$CACHE/nrf5_sdk_15.3.0.zip" "$SDK_SHA256"
    unzip -q "$CACHE/nrf5_sdk_15.3.0.zip" -d .
fi
fetch "$KEY_URL" "$CACHE/ruuvi_open_private.pem" "$KEY_SHA256"

GCC="$(command -v arm-none-eabi-gcc)" || { echo "arm-none-eabi-gcc not found" >&2; exit 1; }
GNU_INSTALL_ROOT="$(dirname "$GCC")/"
GNU_VERSION="$(arm-none-eabi-gcc -dumpfullversion)"

VERSION="$(git describe --tags --always --dirty 2>/dev/null || echo dev)"

make -C "$TARGET" clean >/dev/null
make -C "$TARGET" -j"$(nproc)" HOMEKIT=1 \
    GNU_INSTALL_ROOT="$GNU_INSTALL_ROOT" GNU_VERSION="$GNU_VERSION" \
    FW_VERSION="-DAPP_FW_VERSION=\\\"v3.31.1-$VERSION\\\"" \
    DEBUG=-DNDEBUG MODE=-DAPPLICATION_MODE_HOMEKIT OPT="-Os -g3"

NAME="ruuvitag_b_homekit_${VERSION}"
cp "$TARGET/_build/nrf52832_xxaa.hex" "dist/${NAME}_app.hex"
# Same parameters as the official Ruuvi packages (src/targets/ruuvitag_b/armgcc/package.sh).
python3 tools/dfu_package.py "dist/${NAME}_app.hex" "dist/${NAME}_dfu_app.zip" \
    --key-file "$CACHE/ruuvi_open_private.pem" --application-version 1 --hw-version 0xB0 --sd-req 0xB7

echo
python3 tools/homekit_provision.py show
echo
echo "Update package: dist/${NAME}_dfu_app.zip"
