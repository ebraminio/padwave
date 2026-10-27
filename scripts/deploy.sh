#!/usr/bin/env bash
# Deploy from the Mac to the Pi and restart the service.
#
#   PADWAVE_HOST=user@host ./scripts/deploy.sh
set -euo pipefail

HOST="${PADWAVE_HOST:?set PADWAVE_HOST=user@host}"
TARGET="${PADWAVE_DIR:-padwave}"

cd "$(dirname "$0")/.."

echo "==> syncing to $HOST:~/$TARGET"
rsync -az --delete \
  --exclude 'build' --exclude 'bin' --exclude '.git' --exclude 'var' \
  ./ "$HOST:~/$TARGET/"

echo "==> installing remotely"
ssh "$HOST" "cd ~/$TARGET && ./scripts/install.sh"
