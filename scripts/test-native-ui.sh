#!/bin/sh
set -eu

output_dir=dist/native-ui
mkdir -p "$output_dir/1264x1680" "$output_dir/758x1024"
cc -std=c11 -O0 -w $(pkg-config --cflags gtk+-2.0 libcurl gthread-2.0 gdk-pixbuf-2.0) \
  client/tests/ui_smoke.c client/src/api.c client/src/config.c \
  -o "$output_dir/ui-smoke" \
  $(pkg-config --libs gtk+-2.0 libcurl gthread-2.0 gdk-pixbuf-2.0)
xvfb-run -a -s '-screen 0 1264x1680x24' "$output_dir/ui-smoke" "$output_dir/1264x1680"
xvfb-run -a -s '-screen 0 758x1024x24' "$output_dir/ui-smoke" "$output_dir/758x1024"
