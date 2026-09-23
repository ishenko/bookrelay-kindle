#!/bin/sh
set -eu

output_dir=dist/native-ui
mkdir -p "$output_dir/1264x1680" "$output_dir/758x1024" \
  "$output_dir/1264x1680-300dpi" "$output_dir/758x1024-300dpi" \
  "$output_dir/native-keyboard"
cc -std=c11 -O0 -w $(pkg-config --cflags gtk+-2.0 libcurl gthread-2.0 gdk-pixbuf-2.0) \
  client/tests/ui_smoke.c client/src/api.c client/src/config.c client/src/favorites.c \
  -o "$output_dir/ui-smoke" \
  $(pkg-config --libs gtk+-2.0 libcurl gthread-2.0 gdk-pixbuf-2.0)
python3 client/tests/test_ui_pairing.py "$output_dir/ui-smoke"
xvfb-run -a -s '-screen 0 1264x1680x24' "$output_dir/ui-smoke" "$output_dir/1264x1680"
xvfb-run -a -s '-screen 0 758x1024x24' "$output_dir/ui-smoke" "$output_dir/758x1024"
BOOKRELAY_TEST_DPI=300 xvfb-run -a -s '-screen 0 1264x1680x24' "$output_dir/ui-smoke" "$output_dir/1264x1680-300dpi"
python3 client/tests/test_cover_ui.py "$output_dir/ui-smoke" "$output_dir/1264x1680-300dpi"
BOOKRELAY_TEST_DPI=300 xvfb-run -a -s '-screen 0 758x1024x24' "$output_dir/ui-smoke" "$output_dir/758x1024-300dpi"
BOOKRELAY_KEYBOARD=kindle BOOKRELAY_LIPC_SET_PROP=/no/such/lipc-set-prop \
  xvfb-run -a -s '-screen 0 758x1024x24' "$output_dir/ui-smoke" "$output_dir/758x1024"
cat > "$output_dir/lipc-mock.sh" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$BOOKRELAY_LIPC_TRACE"
EOF
chmod +x "$output_dir/lipc-mock.sh"
: > "$output_dir/lipc-trace.txt"
BOOKRELAY_TEST_DPI=300 BOOKRELAY_TEST_NATIVE=1 BOOKRELAY_KEYBOARD=kindle \
  BOOKRELAY_LIPC_SET_PROP="$output_dir/lipc-mock.sh" \
  BOOKRELAY_LIPC_TRACE="$output_dir/lipc-trace.txt" \
  xvfb-run -a -s '-screen 0 1264x1680x24' "$output_dir/ui-smoke" "$output_dir/native-keyboard"
grep -Fqx -- '-s com.lab126.keyboard open bookrelay.kindle:abc:0' "$output_dir/lipc-trace.txt"
grep -Fqx -- '-s com.lab126.keyboard close bookrelay.kindle' "$output_dir/lipc-trace.txt"
