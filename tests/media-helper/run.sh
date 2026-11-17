#!/bin/bash
# Host test of src/media-helper.cc against a stub libplayerAPIs.so.1 that
# exports the TV's symbol names (tests/media-helper/stub-starfish.cc). The
# helper is built for the host with its real main() wrapper and driven by
# tests/media-helper/driver.c through the socket on fd 3 the way Firefox does.
#   bash tests/media-helper/run.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BUILD=${BUILD:-$(mktemp -d /tmp/media-helper-test-XXXXXX)}
CXXFLAGS="-std=gnu++17 -Wall -Wextra -g -I$ROOT/src"
g++ $CXXFLAGS -shared -fPIC -o "$BUILD/libplayerAPIs.so.1" "$HERE/stub-starfish.cc" -lpthread
cat > "$BUILD/main.cc" <<'MAIN'
#include "media-helper.h"
int main(int argc, char **argv) { return media_helper_main(argc, argv); }
MAIN
g++ $CXXFLAGS -o "$BUILD/helper" "$BUILD/main.cc" "$ROOT/src/media-helper.cc" \
    -Wl,--dynamic-list="$ROOT/src/geckotv.dynlist" -ldl -lpthread
gcc -std=gnu11 -Wall -Wextra -g -I"$ROOT/src" -o "$BUILD/driver" "$HERE/driver.c"

export LD_LIBRARY_PATH=$BUILD APPID=com.example.test
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
run() { # name, env..., scenario
    local name=$1; shift
    : > "$BUILD/stub.log"
    env STUB_LOG="$BUILD/stub.log" "$@" "$BUILD/driver" "$BUILD/helper" "$name" > "$BUILD/$name.out" \
        2> "$BUILD/$name.err" || { echo "driver failed in $name"; cat "$BUILD/$name.out" "$BUILD/$name.err"; exit 1; }
    cp "$BUILD/stub.log" "$BUILD/$name.stub"
}
field() { grep "^$2 " "$BUILD/$1.out" | sed -n "s/.*$3=\([^ ]*\).*/\1/p" | head -1; }

echo "== play: open, load, feed, play, end of stream"
run play
check "open accepted" '[ "$(field play open result)" = 0 ]'
check "pipeline flag after load" '[ $(( $(field play pipeline flags) & 1 )) = 1 ]'
check "Load payload carries the window id, codecs, fps and adts audio" \
    'grep -q "load .*\"windowId\":\"w123\".*\"video\":\"H264\",\"audio\":\"AAC\".*\"videoFpsValue\":25,\"videoFpsScale\":1.*\"aacInfo\":{\"channels\":2,\"format\":\"adts\",\"frequency\":48" "$BUILD/play.stub"'
check "foreground before load" '[ "$(grep -o " foreground\| load" "$BUILD/play.stub" | head -2 | tr -d " " | tr "\n" ",")" = "foreground,load," ]'
check "20 feeds, pts in ns, video then audio per step" \
    '[ "$(grep -c " feed " "$BUILD/play.stub")" = 20 ] && grep -q " feed 1 40000000 64" "$BUILD/play.stub" && grep -q " feed 2 40000000 64" "$BUILD/play.stub"'
check "Play re-issued and volume applied at load completed" \
    '[ "$(grep -c " play" "$BUILD/play.stub")" -ge 1 ] && grep -q "volume {\"volume\":50" "$BUILD/play.stub"'
check "first frame reported, time advances to 200 ms" \
    '[ $(( $(field play first-frame flags) & 4 )) = 4 ] && [ "$(field play advanced time)" -ge 200000 ]'
check "pushEOS once both tracks ended, then ENDED flag" \
    '[ "$(grep -c " eos" "$BUILD/play.stub")" = 1 ] && [ $(( $(field play ended flags) & 8 )) = 8 ]'
check "unload and dtor at close, helper exits 0" \
    'grep -q " unload" "$BUILD/play.stub" && grep -q " dtor" "$BUILD/play.stub" && grep -q "helper exit 0" "$BUILD/play.out"'

echo "== seek: flush, then Seek with the next packet's time"
run seek
check "flush at SEEK, Seek(5000) right before the next video feed" \
    '[ "$(grep -o " flush\| seek 5000\| feed 1 5000000000" "$BUILD/seek.stub" | tr -d " " | tr "\n" ",")" = "flush,seek5000,feed15000000000," ]'
check "SEEKDONE and a new frame after the seek, time at 5 s" \
    '[ $(( $(field seek after-seek flags) & 36 )) = 36 ] && [ "$(field seek after-seek time)" -ge 5000000 ]'

echo "== retry: BufferFull is reported, not fatal"
run retry STUB_BUFFER_FULL_AT=3
check "third feed answers RETRY, the repeat answers OK" \
    '[ "$(field retry feed3 result)" = 1 ] && [ "$(field retry feed3-again result)" = 0 ]'

echo "== failload: a refused Load ends in the FAILED flag"
run failload STUB_FAIL_LOAD=1
check "LOAD accepted, status then FAILED without PIPELINE" \
    '[ "$(field failload load result)" = 0 ] && [ $(( $(field failload status flags) & 17 )) = 16 ]'

echo "== refusals"
run badopen
check "unknown video codec refused" '[ "$(field badopen open result)" = 2 ]'
run unsupported
check "non-AAC audio refused" '[ "$(field unsupported open result)" = 2 ]'

echo "build dir: $BUILD"
exit $fail
