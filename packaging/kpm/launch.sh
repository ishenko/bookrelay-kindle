#!/bin/sh
set -eu
PACKAGE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export BOOKRELAY_HOME=/mnt/us/documents/bookrelay-kindle-data
export XDG_DATA_HOME="$BOOKRELAY_HOME"
export BOOKRELAY_ASSET_DIR="$PACKAGE_DIR/share/covers"
exec "$PACKAGE_DIR/bookrelay-kindle"
