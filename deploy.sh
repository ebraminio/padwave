#!/usr/bin/env bash
# Deploy this source tree from the Mac to the Pi and restart the service.
#
#   PADWAVE_HOST=user@host ./deploy.sh
#   PADWAVE_HOST=user@host PADWAVE_DIR=padwave ./deploy.sh
#
# The Mac binary is never copied: the Pi rebuilds from source so it links
# against the Pi's FFmpeg/Opus.
set -euo pipefail

HOST="${PADWAVE_HOST:?set PADWAVE_HOST=user@host}"
TARGET="${PADWAVE_DIR:-padwave}"

cd "$(dirname "$0")"

echo "==> syncing to $HOST:~/$TARGET"
rsync -az --delete \
  --exclude 'padwave' --exclude '.git' --exclude 'var' \
  ./ "$HOST:~/$TARGET/"

echo "==> building and installing on $HOST"
ssh "$HOST" "cd ~/$TARGET && ./build.sh && ./install.sh"
