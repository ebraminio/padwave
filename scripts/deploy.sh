#!/usr/bin/env bash
# Deploy from the Mac to the Pi and restart the service.
#
#   PADWAVE_HOST=ebrahim@192.168.1.175 ./scripts/deploy.sh
set -euo pipefail

HOST="${PADWAVE_HOST:-ebrahim@192.168.1.175}"
TARGET="${PADWAVE_DIR:-padwave}"

cd "$(dirname "$0")/.."

echo "==> syncing to $HOST:~/$TARGET"
rsync -az --delete \
  --exclude '_build' --exclude 'deps' --exclude '.git' --exclude 'var' \
  ./ "$HOST:~/$TARGET/"

echo "==> installing remotely"
ssh "$HOST" "cd ~/$TARGET && ./scripts/install.sh"
