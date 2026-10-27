#!/bin/sh
# padwave is the C++ app now: it owns the panel, the radio, the player and the
# input decoder. No Mix, no Hex, nothing to compile at boot.
set -e

cd "$(dirname "$0")/.." || exit 1
export PADWAVE_STANDALONE=1
exec "$HOME/.local/bin/padwave" serve 8787 8788 8890
