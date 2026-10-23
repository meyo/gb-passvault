#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

if [ ! -x tools/gbdk/bin/lcc ]; then
    if [ ! -f tools/gbdk-macos.tar.gz ]; then
        echo "error: tools/gbdk/bin/lcc is missing and tools/gbdk-macos.tar.gz was not found" >&2
        exit 1
    fi
    echo "Extracting GBDK..."
    tar -xzf tools/gbdk-macos.tar.gz -C tools
fi

if [ ! -d tools/emu/node_modules/serverboy ]; then
    echo "Installing emulator dependencies..."
    (cd tools/emu && npm ci)
fi

echo "Bootstrap complete."
