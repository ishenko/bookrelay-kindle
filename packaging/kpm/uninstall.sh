#!/bin/sh
set -eu
SCRIPTLET=/mnt/us/documents/bookrelay-kindle.sh
case "$SCRIPTLET" in
  /mnt/us/*) ;;
  *) echo "refusing to remove outside user storage" >&2; exit 1 ;;
esac
if [ -f "$SCRIPTLET" ]; then
  rm -f -- "$SCRIPTLET"
fi
printf '%s\n' "BookRelay Kindle launcher removed"
