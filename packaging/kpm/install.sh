#!/bin/sh
set -eu
PACKAGE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SCRIPTLET=/mnt/us/documents/bookrelay-kindle.sh
case "$SCRIPTLET" in
  /mnt/us/*) ;;
  *) echo "refusing to install outside user storage" >&2; exit 1 ;;
esac
cat > "$SCRIPTLET" <<SCRIPTLET_EOF
#!/bin/sh
exec /var/local/kmc/bin/kpm launch bookrelay-kindle
SCRIPTLET_EOF
chmod 0755 "$SCRIPTLET"
chmod 0755 "$PACKAGE_DIR/launch.sh"
printf '%s\n' "BookRelay Kindle installed; launcher: $SCRIPTLET"
