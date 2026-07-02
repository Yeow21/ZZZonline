#!/usr/bin/env bash
# Build the Z Online wasm module with plain clang (no Emscripten needed).
set -euo pipefail
cd "$(dirname "$0")"

clang --target=wasm32 -O2 -nostdlib -ffreestanding -fvisibility=hidden \
    -Wall -Wextra -Wno-unused-parameter \
    -Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all \
    -o web/game.wasm src/game.c

ls -la web/game.wasm
echo "Build OK. Serve the web/ directory, e.g.:  python3 -m http.server -d web 8000"
