#!/bin/sh
# Plays a video through this TV's media server (com.webos.media,
# uMediaServer), the service LG's own apps use for hardware decoding.
#
# Run as root on the TV with   sh media-test.sh [video URL]
# It changes nothing on the TV. Each run loads the video, plays it for
# 15 seconds, unloads it, and writes /tmp/geckotv-media-test.txt.
#
# Round 1 asks as root and round 2 as if from the browser's app id (luna-send
# -a). The events show whether a hardware decoder channel was assigned
# (resourceList VDEC, videoInfo vdec_ch) and whether the clock advanced
# (currentTime). The default clip has no audio track. Without a window id
# the decoded frames may have no video plane, so a missing picture is not a
# failure of the decoder.

URL=${1:-https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/1080/Big_Buck_Bunny_1080_10s_1MB.mp4}
APP=com.github.gprot42.geckotv
R=/tmp/geckotv-media-test.txt
: > "$R"

log() { echo "$*"; echo "$*" >> "$R"; }

grep -hE '"(webos_release|core_os_release|device_name)"' /var/run/nyx/os_info.json >> "$R" 2>/dev/null
log "url: $URL"

round() {
    name=$1; shift   # remaining args: extra luna-send options
    log ""
    log "== round: $name"
    out=$(luna-send -n 1 "$@" luna://com.webos.media/load \
        "{\"uri\":\"$URL\",\"type\":\"media\",\"payload\":{\"option\":{\"appId\":\"$APP\"}}}" 2>&1)
    log "load: $out"
    id=$(echo "$out" | sed -n 's/.*"mediaId"[ ]*:[ ]*"\([^"]*\)".*/\1/p')
    if [ -z "$id" ]; then
        log "result: load refused (no mediaId)"
        return
    fi
    # Pipeline events (loaded, playing, errors, video size) for 20 s.
    luna-send -i "$@" luna://com.webos.media/subscribe "{\"mediaId\":\"$id\"}" \
        > /tmp/geckotv-media-events.txt 2>&1 &
    sub=$!
    sleep 2
    log "play: $(luna-send -n 1 "$@" luna://com.webos.media/play "{\"mediaId\":\"$id\"}" 2>&1)"
    log "playing for 15 s: watch and listen now"
    sleep 15
    log "unload: $(luna-send -n 1 "$@" luna://com.webos.media/unload "{\"mediaId\":\"$id\"}" 2>&1)"
    sleep 1
    kill "$sub" 2>/dev/null
    log "events:"
    cut -c1-300 /tmp/geckotv-media-events.txt | head -n 40 >> "$R"
    log "result: loaded as $id (did you see or hear it? note it for the report)"
}

round "root"
round "as $APP" -a "$APP"

log ""
log "Report written to $R"
