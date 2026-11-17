#ifndef MEDIA_PROTOCOL_H
#define MEDIA_PROTOCOL_H

#include <stdint.h>

/* Wire format between Firefox's WebOSEngine (dom/media/platforms/webos in
 * build/patches/webos-media-engine.patch) and geckotv --media-helper over
 * the unix stream socket the helper finds on fd 3. Every request gets one
 * GM_REPLY. The first request is GM_OPEN and its reply tells the client that
 * the helper is up. The patch carries a copy of these definitions. Keep that
 * copy in step with this file. */

#define GM_MAGIC 0x4d565447u /* "GTVM" little-endian */
#define GM_MAX_PAYLOAD (16u * 1024u * 1024u)
#define GM_HELPER_FD 3

enum gm_type {
    GM_REPLY = 2,   /* helper -> client, one per request */
    GM_OPEN = 10,   /* codecs and stream parameters, payload struct gm_open */
    GM_LOAD = 11,   /* load or reload the pipeline at time_us, arg0/arg1 fps numerator/denominator, 0 keeps OPEN's */
    GM_FEED = 12,   /* arg0 track, arg1 keyframe, time_us pts, payload access unit */
    GM_SEEK = 13,   /* time_us target */
    GM_PLAY = 14,
    GM_PAUSE = 15,
    GM_VOLUME = 16, /* arg0 0..100 */
    GM_EOS = 17,    /* arg0 track */
    GM_STATUS = 19,
    GM_CLOSE = 20
};

enum gm_track { GM_TRACK_VIDEO = 1, GM_TRACK_AUDIO = 2 };
enum gm_video_codec { GM_VIDEO_NONE = 0, GM_VIDEO_H264 = 1, GM_VIDEO_VP9 = 2 };
enum gm_audio_codec { GM_AUDIO_NONE = 0, GM_AUDIO_AAC = 3 };

enum gm_result { GM_OK = 0, GM_RETRY = 1, GM_UNSUPPORTED = 2, GM_ERROR = -1 };

/* Reply arg1 */
enum gm_flags {
    GM_F_PIPELINE = 1,  /* Load() succeeded and the pipeline was not unloaded since */
    GM_F_LOADED = 2,    /* LOADCOMPLETED seen */
    GM_F_HAS_FRAME = 4, /* a FRAMEREADY since the load or the last seek */
    GM_F_ENDED = 8,     /* ENDOFSTREAM */
    GM_F_FAILED = 16,   /* the pipeline reported an error, arg2 has it */
    GM_F_SEEKDONE = 32, /* SEEKDONE since the last seek */
    GM_F_PLAYING = 64
};

struct gm_msg {
    uint32_t magic;
    uint32_t type;
    uint32_t size;      /* payload bytes following the header */
    uint32_t reserved;
    int32_t arg[6];     /* request arguments, in a reply result, flags, error, video fed, 0, 0 */
    int64_t time_us;    /* pts or target in a request, last FRAMEREADY time in a reply */
};

/* GM_OPEN carries arg0 video codec, arg1 width, arg2 height, arg3 fps
 * numerator, arg4 fps denominator, arg5 audio codec and this payload. */
struct gm_open {
    uint32_t audio_rate;
    uint32_t audio_channels;
    uint32_t audio_profile;         /* AAC object type, 2 for LC */
    uint32_t audio_frequency_index; /* AAC sampling frequency index */
    char window_id[128];            /* exported window from the adapter, NUL-terminated, may be empty */
};

#endif
