#!/bin/bash
# Build gta5_savemirror.elf on macOS (Apple Silicon or Intel).
# Follows the ps4-payload-dev SDK README: Homebrew llvm@18 + SDK release zip.
set -e
command -v brew >/dev/null || { echo "Install Homebrew first: https://brew.sh"; exit 1; }
brew install llvm@18
export LLVM_CONFIG="$(brew --prefix llvm@18)/bin/llvm-config"
SDK="$HOME/ps4-payload-sdk"
if [ ! -d "$SDK" ]; then
  curl -L -o /tmp/ps4-payload-sdk.zip \
    https://github.com/ps4-payload-dev/sdk/releases/latest/download/ps4-payload-sdk.zip
  unzip -q /tmp/ps4-payload-sdk.zip -d "$HOME"
fi
export PS4_PAYLOAD_SDK="$SDK"
cd "$(dirname "$0")"
make clean && make
shasum -a 256 gta5_savemirror.elf
