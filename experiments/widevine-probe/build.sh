#!/bin/bash
# Cross-builds cdm-probe for the TV inside the ffbuild container (nc4
# toolchain) against Firefox's copy of the Chromium CDM header. Run on the TV
# as root outside and inside the app jail:
#   HOME=/tmp/probe /tmp/cdm-probe
#   HOME=/tmp/probe chroot /var/palm/jail/com.github.gprot42.geckotv /tmp/cdm-probe
# OnInitialized(true) means the TV CDM can reach the TEE from there.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
docker exec ffbuild bash -c 'mkdir -p /tmp/wvprobe && cp /work/firefox/dom/media/gmp/widevine-adapter/content_decryption_module*.h /tmp/wvprobe/'
docker cp "$HERE/cdm-probe.cc" ffbuild:/tmp/wvprobe/cdm-probe.cc
docker exec ffbuild bash -c 'cd /tmp/wvprobe && /work/nc4/out/host/bin/arm-webos-linux-gnueabi-g++ -std=c++17 -O1 -I. -I/work/obj-nc4/dist/include -o cdm-probe cdm-probe.cc -ldl -lpthread'
docker cp ffbuild:/tmp/wvprobe/cdm-probe "$HERE/cdm-probe"
echo "$HERE/cdm-probe"
