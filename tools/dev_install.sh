#!/usr/bin/env bash
# dev-install: the DELIBERATE crash-loop installer.
# Bare-overwrites ~/.clap/muhammad.clap on the inode Bitwig has mmap'd, so
# the running plugin host takes a SIGBUS on the next code-page touch and
# Bitwig respawns it on the NEW build. Fast live-iteration for development;
# loud by design: NEVER while performing. The polite default stays the
# atomic cp .new + mv -T dance used by normal tooling.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake --build build -j8 --target muhammad_clap
strings build/muhammad.clap | grep -m1 -oE "1\.[0-9]+\.[0-9]+-[a-z0-9]+" || true
cp build/muhammad.clap "$HOME/.clap/muhammad.clap"
echo "dev-installed: a live Bitwig plugin host will fault + reload onto this build"
