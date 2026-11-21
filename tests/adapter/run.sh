#!/bin/bash
# Host tests of src/webos-xdg.c covering video plane placement, the video
# socket, toplevel
# states, GPU pop-ups, the overlay's input region, the Back key and the wheel. Needs wayland-scanner and
# the wayland-client headers and library on the host (conda's work with
# WAYLAND_PREFIX=$HOME/miniconda3), and xdg-shell.xml plus
# text-input-unstable-v3.xml from wayland-protocols, fetched if missing.
#   bash tests/adapter/run.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$(mktemp -d /tmp/adapter-test-XXXXXX)}
PREFIX=${WAYLAND_PREFIX:-$HOME/miniconda3}
SCANNER=${WAYLAND_SCANNER:-$PREFIX/bin/wayland-scanner}
GEN=$BUILD/gen
mkdir -p "$GEN"
WP=https://gitlab.freedesktop.org/wayland/wayland-protocols/-/raw/1.36
[ -f "$GEN/xdg-shell.xml" ] || curl -sfL -o "$GEN/xdg-shell.xml" $WP/stable/xdg-shell/xdg-shell.xml
[ -f "$GEN/text-input-unstable-v3.xml" ] || curl -sfL -o "$GEN/text-input-unstable-v3.xml" $WP/unstable/text-input/text-input-unstable-v3.xml
"$SCANNER" client-header "$GEN/xdg-shell.xml" "$GEN/xdg-shell-client-protocol.h"
"$SCANNER" private-code "$GEN/xdg-shell.xml" "$GEN/xdg-shell-protocol.c"
"$SCANNER" client-header "$GEN/text-input-unstable-v3.xml" "$GEN/text-input-unstable-v3-client-protocol.h"
"$SCANNER" private-code "$GEN/text-input-unstable-v3.xml" "$GEN/text-input-unstable-v3-protocol.c"
"$SCANNER" client-header "$ROOT/src/webos-shell.xml" "$GEN/wayland-webos-shell-client-protocol.h"
"$SCANNER" private-code "$ROOT/src/webos-shell.xml" "$GEN/webos-shell-protocol.c"
for x in webos-input-manager text-model webos-surface-group webos-foreign; do
    "$SCANNER" client-header "$ROOT/src/$x.xml" "$GEN/$x-client-protocol.h"
    "$SCANNER" private-code "$ROOT/src/$x.xml" "$GEN/$x-protocol.c"
done
# The adapter's hooks into libwayland-client's private side are stubbed, so
# the test exercises the compositing code only.
cat > "$GEN/stubs.c" <<'STUBS'
#include <stdint.h>
struct wl_proxy; struct wl_interface; struct wl_display;
struct wl_proxy *webos_xdg_create_virtual(struct wl_proxy *f, const struct wl_interface *i, uint32_t v) { (void)f; (void)i; (void)v; return 0; }
void webos_xdg_destroy_virtual(struct wl_proxy *p) { (void)p; }
void *webos_xdg_last_caller(void) { return 0; }
struct wl_display *webos_xdg_proxy_display(struct wl_proxy *p) { (void)p; return 0; }
int webos_xdg_on_default_queue(struct wl_proxy *p) { (void)p; return 1; }
STUBS
status=0
for t in video-clip video-socket toplevel-state gpu-popup overlay-input back-key wheel; do
    gcc -std=gnu11 -w -I"$GEN" -I"$PREFIX/include" -o "$BUILD/$t" "$HERE/$t.c" \
        "$GEN"/*-protocol.c "$GEN/stubs.c" -L"$PREFIX/lib" -Wl,-rpath,"$PREFIX/lib" \
        -lwayland-client -lxkbcommon -lpthread -ldl -lm
    "$BUILD/$t" || status=1
done
exit $status
