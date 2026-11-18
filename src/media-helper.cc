/* geckotv --media-helper plays one stream through LG's Starfish media
 * pipeline (libplayerAPIs.so.1). Firefox feeds it compressed audio and video
 * over the socket on fd 3 (src/media-protocol.h). It runs as a mode of the
 * launcher binary so that the TV's media stack sees the app's own
 * executable, and with a clean environment so that it loads the TV's
 * libraries rather than the ones bundled with Firefox. It exits when that
 * socket closes.
 *
 * The pipeline decodes, paces by presentation time, plays the audio and
 * reports the time of the frame on screen (FRAMEREADY). Firefox's clock
 * follows that time. The picture goes to the hardware video plane under the
 * window, inside the window region the adapter exports through
 * wl_webos_foreign. The exported window's id arrives with GM_OPEN.
 *
 * The Starfish object is driven through dlsym'd member functions called as
 * free functions. On ARM EABI a member takes `this` in r0, and a member
 * returning a std::string takes the return slot in r0 and `this` in r1.
 * The strings LG's library returns are read as the cxx11 std::string layout
 * and their buffers are released with free() to match its libstdc++.
 * Load() runs on a thread of its own because it can block for seconds and
 * blocks for ever when the bus refuses the registration. The socket loop
 * must keep answering so that Firefox can give up and kill the helper.
 *
 * GECKOTV_MEDIA_LOG=1 keeps every PmLog line of LG's libraries. Errors and
 * warnings are always kept. */

#include "media-helper.h"
#include "media-protocol.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

void logmsg(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "media-helper: %s\n", buf);
    fflush(stderr);
}

int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);

    return v && v[0] ? atoi(v) : dflt;
}

} // namespace

/* LG's media libraries report through PmLog and its output is not readable
 * from the app. A shared library's imports bind first to the executable's
 * definitions. src/geckotv.dynlist exports these four, so they catch every
 * message and put it in geckotv.log. The context's first field is the
 * enabled level and 7 lets everything through. With key-value pairs
 * (kv_count > 0) the varargs start with the pairs, so only the compile-time
 * format summary is printed. */
struct pmlog_context_info {
    int enabledLevel;
    int flags;
};

/* Exported from a launcher otherwise built with hidden visibility. */
#define PMLOG_EXPORT extern "C" __attribute__((visibility("default")))

static struct pmlog_context_info pmlog_all = {7, 0};

static int pmlog_keep(int level)
{
    static int keep = -1;

    if (keep < 0)
        keep = env_int("GECKOTV_MEDIA_LOG", 0) ? 7 : 4;
    return level <= keep;
}

PMLOG_EXPORT int PmLogGetContext(const char *name, const struct pmlog_context_info **ctx)
{
    (void)name;
    *ctx = &pmlog_all;
    return 0;
}

PMLOG_EXPORT int PmLogSetContextLevel(const void *ctx, int level)
{
    (void)ctx;
    (void)level;
    return 0;
}

PMLOG_EXPORT int _PmLogMsgKV(const void *ctx, int level, unsigned flags, const char *msgid,
                           size_t kv_count, const char *keywords, const char *formats,
                           const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    (void)ctx;
    (void)flags;
    (void)keywords;
    if (!pmlog_keep(level))
        return 0;
    if (kv_count > 0) {
        logmsg("pmlog %d %s: %zu pairs, formats %s", level, msgid ? msgid : "-", kv_count,
               formats ? formats : "-");
        return 0;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
    va_end(ap);
    logmsg("pmlog %d %s: %s", level, msgid ? msgid : "-", buf);
    return 0;
}

PMLOG_EXPORT int PmLogString_(const void *ctx, int level, const char *msgid, const char *kvpairs,
                            const char *message)
{
    (void)ctx;
    if (!pmlog_keep(level))
        return 0;
    logmsg("pmlog %d %s: %s %s", level, msgid ? msgid : "-", kvpairs ? kvpairs : "",
           message ? message : "");
    return 0;
}

namespace {

/* The cxx11 std::string layout of GCC's libstdc++. LG's library is built
 * with the cxx11 ABI on webOS 5 and later and returns it from Feed. */
struct CxxString {
    char *ptr;
    size_t len;
    union {
        char local[16];
        size_t capacity;
    };
};

typedef void (*StarfishCallback)(int type, int64_t num, const char *str, void *data);

/* Entry points of libplayerAPIs.so.1, by their mangled names. */
struct Starfish {
    void *lib = nullptr;
    void (*ctor)(void *self, const char *uid) = nullptr;
    void (*dtor)(void *self) = nullptr;
    bool (*load)(void *self, const char *payload, StarfishCallback cb, void *data) = nullptr;
    void (*feed)(CxxString *ret, void *self, const char *payload) = nullptr;
    bool (*play)(void *self) = nullptr;
    bool (*pause)(void *self) = nullptr;
    bool (*seek)(void *self, const char *millis) = nullptr;
    bool (*flush)(void *self) = nullptr;
    bool (*push_eos)(void *self) = nullptr;
    bool (*unload)(void *self) = nullptr;
    bool (*set_volume)(void *self, const char *payload) = nullptr;
    bool (*notify_foreground)(void *self) = nullptr;
    const char *(*media_id)(void *self) = nullptr;

    bool open()
    {
        lib = dlopen("libplayerAPIs.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) {
            logmsg("libplayerAPIs.so.1: %s", dlerror());
            return false;
        }
        struct {
            const char *name;
            void **slot;
        } syms[] = {
            {"_ZN17StarfishMediaAPIsC1EPKc", (void **)&ctor},
            {"_ZN17StarfishMediaAPIsD1Ev", (void **)&dtor},
            {"_ZN17StarfishMediaAPIs4LoadEPKcPFvixS1_PvES2_", (void **)&load},
            {"_ZN17StarfishMediaAPIs4FeedB5cxx11EPKc", (void **)&feed},
            {"_ZN17StarfishMediaAPIs4PlayEv", (void **)&play},
            {"_ZN17StarfishMediaAPIs5PauseEv", (void **)&pause},
            {"_ZN17StarfishMediaAPIs4SeekEPKc", (void **)&seek},
            {"_ZN17StarfishMediaAPIs5flushEv", (void **)&flush},
            {"_ZN17StarfishMediaAPIs7pushEOSEv", (void **)&push_eos},
            {"_ZN17StarfishMediaAPIs6UnloadEv", (void **)&unload},
            {"_ZN17StarfishMediaAPIs9setVolumeEPKc", (void **)&set_volume},
            {"_ZN17StarfishMediaAPIs16notifyForegroundEv", (void **)&notify_foreground},
            {"_ZN17StarfishMediaAPIs10getMediaIDEv", (void **)&media_id},
        };
        for (auto &s : syms) {
            *s.slot = dlsym(lib, s.name);
            if (!*s.slot) {
                logmsg("libplayerAPIs.so.1 lacks %s", s.name);
                return false;
            }
        }
        return true;
    }
};

/* GLib's default main context. The loop pumps it in case the library
 * attaches sources to it. Absent on the test host. */
struct Glib {
    int (*iteration)(void *context, int may_block) = nullptr;

    void open()
    {
        void *lib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);

        if (lib)
            iteration = (int (*)(void *, int))dlsym(lib, "g_main_context_iteration");
        if (!iteration)
            logmsg("no GLib main context to pump");
    }

    void pump()
    {
        if (!iteration)
            return;
        for (int n = 0; n < 32 && iteration(nullptr, 0); n++) {
        }
    }
};

enum { PF_FRAMEREADY = 0x0, PF_INT_ERROR = 0x12, PF_STR_ERROR = 0x13, PF_LOADCOMPLETED = 0x16,
       PF_SEEKDONE = 0x19, PF_PLAYING = 0x1a, PF_PAUSED = 0x1b, PF_ENDOFSTREAM = 0x1c };

struct Config {
    int vcodec = 0, width = 0, height = 0, fps_num = 0, fps_den = 1;
    int acodec = 0;
    gm_open open = {};
};

bool valid_window_id(const char *id)
{
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '_' || *p == '-' || *p == '.'))
            return false;
    return true;
}

class Player {
public:
    explicit Player(Starfish &api) : api_(api) {}
    ~Player() { Unload(); }

    int Configure(const gm_msg &m, const void *payload)
    {
        Config c;

        c.vcodec = m.arg[0];
        c.width = m.arg[1];
        c.height = m.arg[2];
        c.fps_num = m.arg[3];
        c.fps_den = m.arg[4] > 0 ? m.arg[4] : 1;
        c.acodec = m.arg[5];
        if (m.size < sizeof c.open)
            return GM_ERROR;
        memcpy(&c.open, payload, sizeof c.open);
        c.open.window_id[sizeof c.open.window_id - 1] = '\0';
        if ((c.vcodec != GM_VIDEO_H264 && c.vcodec != GM_VIDEO_VP9) || c.width < 1 ||
            c.width > 4096 || c.height < 1 || c.height > 2304 || c.fps_num < 1 ||
            c.fps_num / c.fps_den > 120)
            return GM_UNSUPPORTED;
        if (c.acodec != GM_AUDIO_NONE) {
            if (c.acodec != GM_AUDIO_AAC || c.open.audio_rate < 8000 || c.open.audio_rate > 96000 ||
                c.open.audio_channels < 1 || c.open.audio_channels > 8)
                return GM_UNSUPPORTED;
        }
        if (c.open.protection != GM_PROTECTION_NONE && c.open.protection != GM_PROTECTION_WIDEVINE)
            return GM_UNSUPPORTED;
        if (!valid_window_id(c.open.window_id)) {
            logmsg("window id refused: %s", c.open.window_id);
            c.open.window_id[0] = '\0';
        }
        config_ = c;
        configured_ = true;
        logmsg("open: video %s %dx%d %d/%d fps, audio %s, window %s%s",
               c.vcodec == GM_VIDEO_H264 ? "H264" : "VP9", c.width, c.height, c.fps_num, c.fps_den,
               c.acodec == GM_AUDIO_AAC ? "AAC" : "none",
               c.open.window_id[0] ? c.open.window_id : "none",
               c.open.protection == GM_PROTECTION_WIDEVINE ? ", Widevine" : "");
        if (!c.open.window_id[0])
            logmsg("no exported window id: the picture has no place on screen");
        return GM_OK;
    }

    /* Loads the pipeline paused at time_us on a thread of its own. */
    int Start(int64_t time_us, int fps_num, int fps_den)
    {
        if (!configured_)
            return GM_ERROR;
        if (fps_num > 0 && fps_den > 0 && fps_num / fps_den <= 120) {
            config_.fps_num = fps_num;
            config_.fps_den = fps_den;
        }
        Unload();
        if (time_us < 0)
            time_us = 0;
        {
            std::lock_guard<std::mutex> guard(lock_);
            flags_ = 0;
            error_ = 0;
            position_us_ = time_us;
            load_event_ = false;
            loading_ = true;
        }
        video_fed_ = 0;
        audio_fed_ = 0;
        eos_video_ = eos_audio_ = eos_pushed_ = false;
        pending_seek_ = false;
        storage_.assign(65536, 0);
        context_.reset(new Context{this, ++generation_});
        std::string payload = LoadPayload(time_us);
        logmsg("load: %s", payload.c_str());
        Context *ctx = context_.get();
        loader_ = std::thread([this, payload, ctx, time_us] {
            bool ok = false;

            try {
                api_.ctor(storage_.data(), nullptr);
                object_ = true;
                if (!api_.notify_foreground(storage_.data()))
                    logmsg("notifyForeground refused");
                ok = api_.load(storage_.data(), payload.c_str(), Event, ctx);
                if (!ok)
                    logmsg("Load refused");
            } catch (...) {
                logmsg("Load threw");
            }
            const char *id = nullptr;
            if (ok) {
                try {
                    id = api_.media_id(storage_.data());
                } catch (...) {
                }
                logmsg("pipeline loading at %lld us, media id %s", (long long)time_us,
                       id ? id : "-");
            }
            std::lock_guard<std::mutex> guard(lock_);
            loading_ = false;
            if (ok) {
                flags_ |= GM_F_PIPELINE;
            } else {
                flags_ |= GM_F_FAILED;
                if (!error_)
                    error_ = -1;
            }
        });
        return GM_OK;
    }

    int Feed(int track, int64_t pts_us, const void *data, uint32_t size)
    {
        int f = flags();

        if (loading())
            return GM_RETRY;
        if (!object_ || !(f & GM_F_PIPELINE) || size == 0)
            return GM_ERROR;
        if (pts_us < 0)
            pts_us = 0;
        if (pending_seek_ && track == GM_TRACK_VIDEO) {
            /* After flush() the pipeline takes the next packet's time as the
             * start of a new segment, so Seek(ms) goes right before feeding it. */
            char ms[32];

            snprintf(ms, sizeof ms, "%lld", (long long)(pts_us / 1000));
            {
                std::lock_guard<std::mutex> guard(lock_);
                pending_seek_ = false;
            }
            bool ok = false;
            try {
                ok = api_.seek(storage_.data(), ms);
            } catch (...) {
            }
            logmsg("Seek(%s) %s", ms, ok ? "ok" : "refused");
        }
        char payload[256];
        snprintf(payload, sizeof payload,
                 "{\"bufferAddr\":\"%p\",\"bufferSize\":%u,\"pts\":%lld,\"esData\":%d}", data, size,
                 (long long)pts_us * 1000, track == GM_TRACK_VIDEO ? 1 : 2);
        std::string reply;
        try {
            reply = CallFeed(payload);
        } catch (...) {
            logmsg("Feed threw");
            return GM_ERROR;
        }
        if (reply.find("Ok") != std::string::npos) {
            if (track == GM_TRACK_VIDEO)
                video_fed_++;
            else
                audio_fed_++;
            return GM_OK;
        }
        if (reply.find("BufferFull") != std::string::npos ||
            reply.find("Pending") != std::string::npos)
            return GM_RETRY;
        logmsg("Feed failed: %s", reply.c_str());
        return GM_ERROR;
    }

    int Seek(int64_t time_us)
    {
        eos_video_ = eos_audio_ = eos_pushed_ = false;
        if (loading() || !object_ || !(flags() & GM_F_PIPELINE)) {
            std::lock_guard<std::mutex> guard(lock_);
            position_us_ = time_us < 0 ? 0 : time_us;
            return GM_OK; /* the client reloads at the target */
        }
        bool ok = false;
        try {
            ok = api_.flush(storage_.data());
        } catch (...) {
        }
        if (!ok) {
            logmsg("flush refused, unloading so the client reloads at %lld us",
                   (long long)time_us);
            Unload();
            std::lock_guard<std::mutex> guard(lock_);
            position_us_ = time_us < 0 ? 0 : time_us;
            return GM_OK;
        }
        std::lock_guard<std::mutex> guard(lock_);
        flags_ &= ~(GM_F_HAS_FRAME | GM_F_ENDED | GM_F_SEEKDONE);
        position_us_ = time_us < 0 ? 0 : time_us;
        pending_seek_ = true;
        logmsg("flushed for a seek to %lld us", (long long)time_us);
        return GM_OK;
    }

    int Play()
    {
        want_play_ = true;
        if (!object_ || !(flags() & GM_F_LOADED))
            return GM_OK;
        bool ok = false;
        try {
            ok = api_.play(storage_.data());
        } catch (...) {
        }
        if (!ok)
            logmsg("Play refused");
        return GM_OK;
    }

    int Pause()
    {
        want_play_ = false;
        if (!object_ || !(flags() & GM_F_LOADED))
            return GM_OK;
        bool ok = false;
        try {
            ok = api_.pause(storage_.data());
        } catch (...) {
        }
        if (!ok)
            logmsg("Pause refused");
        return GM_OK;
    }

    int Volume(int volume)
    {
        if (volume < 0 || volume > 100)
            return GM_UNSUPPORTED;
        volume_ = volume;
        if (!object_ || !(flags() & GM_F_LOADED))
            return GM_OK;
        ApplyVolume();
        return GM_OK;
    }

    int Eos(int track)
    {
        if (track == GM_TRACK_VIDEO)
            eos_video_ = true;
        else if (track == GM_TRACK_AUDIO)
            eos_audio_ = true;
        else
            return GM_ERROR;
        if (loading() || !object_ || !(flags() & GM_F_PIPELINE))
            return GM_OK;
        if (eos_video_ && (eos_audio_ || config_.acodec == GM_AUDIO_NONE) && !eos_pushed_) {
            eos_pushed_ = true;
            bool ok = false;
            try {
                ok = api_.push_eos(storage_.data());
            } catch (...) {
            }
            logmsg("pushEOS %s", ok ? "ok" : "refused");
        }
        return GM_OK;
    }

    /* Play and the volume are applied once the pipeline reports loaded. */
    void Update()
    {
        bool loaded;
        {
            std::lock_guard<std::mutex> guard(lock_);
            loaded = load_event_;
            load_event_ = false;
        }
        if (!loaded || loading() || !object_)
            return;
        logmsg("load completed (%d video, %d audio fed so far)", video_fed_, audio_fed_);
        ApplyVolume();
        if (want_play_)
            Play();
    }

    void Reply(gm_msg *m, int result)
    {
        std::lock_guard<std::mutex> guard(lock_);
        m->type = GM_REPLY;
        m->size = 0;
        memset(m->arg, 0, sizeof m->arg);
        m->arg[0] = result;
        m->arg[1] = flags_;
        m->arg[2] = error_;
        m->arg[3] = video_fed_;
        m->time_us = position_us_;
    }

    void Unload()
    {
        if (loader_.joinable())
            loader_.join();
        {
            std::lock_guard<std::mutex> guard(lock_);
            ++generation_;
            flags_ = 0;
            pending_seek_ = false;
        }
        if (object_) {
            try {
                if (!api_.unload(storage_.data()))
                    logmsg("Unload refused");
                api_.dtor(storage_.data());
            } catch (...) {
                logmsg("Unload threw");
            }
            object_ = false;
            logmsg("unloaded after %d video and %d audio samples", video_fed_, audio_fed_);
        }
        /* Contexts stay allocated because a late callback may still look at one. */
        if (context_)
            retired_.push_back(std::move(context_));
        eos_pushed_ = false;
    }

    int flags()
    {
        std::lock_guard<std::mutex> guard(lock_);
        return flags_;
    }

    bool loading()
    {
        std::lock_guard<std::mutex> guard(lock_);
        return loading_;
    }

private:
    struct Context {
        Player *player;
        unsigned generation;
    };

    /* Secure video path format the pipeline expects the CDM's output in: 40
     * from webOS 5 on, 30 before (the values Kodi's webOS player uses).
     * GECKOTV_SVP_VERSION overrides. */
    static int SvpVersion()
    {
        const char *env = getenv("GECKOTV_SVP_VERSION");
        int v = env && env[0] ? atoi(env) : 0;

        return v > 0 && v < 100 ? v : 40;
    }

    std::string LoadPayload(int64_t time_us) const
    {
        const Config &c = config_;
        const char *app = getenv("APPID");
        int max_w = c.width > 1920 ? c.width : 1920;
        int max_h = c.height > 1080 ? c.height : 1080;
        int fps = (c.fps_num + c.fps_den - 1) / c.fps_den;
        std::string s;
        char buf[512];

        s = "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{";
        if (app && app[0] && valid_window_id(app)) {
            snprintf(buf, sizeof buf, "\"appId\":\"%s\",", app);
            s += buf;
        }
        if (c.open.window_id[0]) {
            snprintf(buf, sizeof buf, "\"windowId\":\"%s\",", c.open.window_id);
            s += buf;
        }
        if (c.open.protection == GM_PROTECTION_WIDEVINE)
            s += "\"drm\":{\"type\":\"WIDEVINE_MODULAR\"},";
        s += "\"queryPosition\":false,\"externalStreamingInfo\":{";
        if (c.open.protection == GM_PROTECTION_WIDEVINE) {
            snprintf(buf, sizeof buf, "\"svpVersion\":%d,", SvpVersion());
            s += buf;
        }
        s += "\"contents\":{\"format\":\"RAW\","
             "\"provider\":\"Firefox\",\"codec\":{";
        snprintf(buf, sizeof buf, "\"video\":\"%s\"", c.vcodec == GM_VIDEO_H264 ? "H264" : "VP9");
        s += buf;
        if (c.acodec == GM_AUDIO_AAC)
            s += ",\"audio\":\"AAC\"";
        snprintf(buf, sizeof buf,
                 "},\"esInfo\":{\"pauseAtDecodeTime\":true,\"ptsToDecode\":%lld,"
                 "\"seperatedPTS\":true,\"videoFpsValue\":%d,\"videoFpsScale\":%d}",
                 (long long)time_us * 1000, c.fps_num, c.fps_den);
        s += buf;
        if (c.acodec == GM_AUDIO_AAC) {
            snprintf(buf, sizeof buf,
                     ",\"aacInfo\":{\"channels\":%u,\"format\":\"adts\",\"frequency\":%g,"
                     "\"profile\":%u}",
                     c.open.audio_channels, c.open.audio_rate / 1000.0,
                     c.open.audio_profile ? c.open.audio_profile : 2);
            s += buf;
        }
        snprintf(buf, sizeof buf,
                 "},\"restartStreaming\":false,\"audioSync\":true,\"streamQualityInfo\":true,"
                 "\"bufferingCtrInfo\":{\"srcBufferLevelAudio\":{\"minimum\":1024,\"maximum\":1048576},"
                 "\"srcBufferLevelVideo\":{\"minimum\":1024,\"maximum\":8388608}}},"
                 "\"transmission\":{\"contentsType\":\"LIVE\"},\"lowDelayMode\":false,"
                 "\"adaptiveStreaming\":{\"audioOnly\":false,\"maxWidth\":%d,\"maxHeight\":%d,"
                 "\"maxFrameRate\":%d}}}]}",
                 max_w, max_h, fps > 60 ? fps : (fps < 24 ? 24 : fps));
        s += buf;
        return s;
    }

    std::string CallFeed(const char *payload)
    {
        CxxString ret;

        memset(&ret, 0, sizeof ret);
        api_.feed(&ret, storage_.data(), payload);
        std::string reply(ret.ptr ? ret.ptr : "", ret.ptr ? ret.len : 0);
        if (ret.ptr && ret.ptr != ret.local)
            free(ret.ptr);
        return reply;
    }

    void ApplyVolume()
    {
        char payload[128];

        snprintf(payload, sizeof payload, "{\"volume\":%d,\"duration\":0,\"easeType\":\"Linear\"}",
                 volume_);
        bool ok = false;
        try {
            ok = api_.set_volume(storage_.data(), payload);
        } catch (...) {
        }
        if (!ok)
            logmsg("setVolume(%d) refused", volume_);
    }

    /* Runs on the library's thread and only records state. A FRAMEREADY
     * between a flush and the Seek that follows it belongs to the old
     * position and is dropped. */
    static void Event(int type, int64_t num, const char *str, void *data)
    {
        Context *ctx = static_cast<Context *>(data);
        Player *p = ctx->player;
        std::lock_guard<std::mutex> guard(p->lock_);

        if (ctx->generation != p->generation_)
            return;
        switch (type) {
        case PF_FRAMEREADY:
            if (!p->pending_seek_) {
                p->position_us_ = num / 1000;
                p->flags_ |= GM_F_HAS_FRAME;
            }
            break;
        case PF_LOADCOMPLETED:
            p->flags_ |= GM_F_LOADED;
            p->load_event_ = true;
            break;
        case PF_SEEKDONE:
            p->flags_ |= GM_F_SEEKDONE;
            break;
        case PF_PLAYING:
            p->flags_ |= GM_F_PLAYING;
            break;
        case PF_PAUSED:
            p->flags_ &= ~GM_F_PLAYING;
            break;
        case PF_ENDOFSTREAM:
            p->flags_ |= GM_F_ENDED;
            break;
        case PF_INT_ERROR:
        case PF_STR_ERROR:
            p->flags_ |= GM_F_FAILED;
            p->error_ = (int)num;
            break;
        default:
            break;
        }
        if (type != PF_FRAMEREADY)
            fprintf(stderr, "media-helper: event 0x%x %lld %s\n", type, (long long)num,
                    str ? str : "");
    }

    Starfish &api_;
    Config config_;
    bool configured_ = false;
    std::vector<char> storage_;
    bool object_ = false;
    std::thread loader_;
    std::unique_ptr<Context> context_;
    std::vector<std::unique_ptr<Context>> retired_;
    std::mutex lock_;
    unsigned generation_ = 0;
    int flags_ = 0;
    int error_ = 0;
    int64_t position_us_ = 0;
    bool load_event_ = false;
    bool loading_ = false;
    bool pending_seek_ = false;
    int video_fed_ = 0, audio_fed_ = 0;
    bool eos_video_ = false, eos_audio_ = false, eos_pushed_ = false;
    bool want_play_ = false;
    int volume_ = 100;
};

int read_exact(int fd, void *buf, size_t n)
{
    uint8_t *p = static_cast<uint8_t *>(buf);

    while (n) {
        ssize_t r = read(fd, p, n);

        if (r == 0)
            return -1;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);

    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);

        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

struct Helper {
    Starfish api;
    Glib glib;
    Player player{api};
    int ff = GM_HELPER_FD;
    std::vector<uint8_t> payload;

    /* One request. Returns -1 when the link is gone or CLOSE arrived. */
    int handle()
    {
        gm_msg m;

        if (read_exact(ff, &m, sizeof m) != 0)
            return -1;
        if (m.magic != GM_MAGIC || m.size > GM_MAX_PAYLOAD) {
            logmsg("bad message (magic %08x, size %u)", m.magic, m.size);
            return -1;
        }
        payload.resize(m.size);
        if (m.size && read_exact(ff, payload.data(), m.size) != 0)
            return -1;
        int result = GM_ERROR;
        bool closing = false;
        switch (m.type) {
        case GM_OPEN:
            result = player.Configure(m, payload.data());
            break;
        case GM_LOAD:
            result = player.Start(m.time_us, m.arg[0], m.arg[1]);
            break;
        case GM_FEED:
            result = player.Feed(m.arg[0], m.time_us, payload.data(), m.size);
            break;
        case GM_SEEK:
            result = player.Seek(m.time_us);
            break;
        case GM_PLAY:
            result = player.Play();
            break;
        case GM_PAUSE:
            result = player.Pause();
            break;
        case GM_VOLUME:
            result = player.Volume(m.arg[0]);
            break;
        case GM_EOS:
            result = player.Eos(m.arg[0]);
            break;
        case GM_STATUS:
            result = GM_OK;
            break;
        case GM_UNLOAD:
            player.Unload();
            result = GM_OK;
            break;
        case GM_CLOSE:
            player.Unload();
            result = GM_OK;
            closing = true;
            break;
        default:
            logmsg("unknown request %u", m.type);
            break;
        }
        player.Reply(&m, result);
        m.magic = GM_MAGIC;
        if (write_all(ff, &m, sizeof m) != 0)
            return -1;
        return closing ? -1 : 0;
    }
};

volatile sig_atomic_t stopping = 0;

void on_signal(int sig)
{
    (void)sig;
    stopping = 1;
}

} // namespace

int media_helper_main(int argc, char **argv)
{
    Helper h;

    (void)argc;
    (void)argv;
    if (fcntl(h.ff, F_GETFD) < 0) {
        logmsg("no socket on fd %d", GM_HELPER_FD);
        return 2;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    logmsg("started, pid %d, uid %u", (int)getpid(), (unsigned)getuid());
    if (!h.api.open())
        return 3;
    h.glib.open();
    logmsg("Starfish library loaded");
    while (!stopping) {
        struct pollfd p = {h.ff, POLLIN, 0};

        h.glib.pump();
        if (poll(&p, 1, 20) < 0 && errno != EINTR)
            break;
        if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
            if (h.handle() < 0) {
                logmsg("Firefox link closed");
                break;
            }
        }
        h.player.Update();
    }
    h.player.Unload();
    logmsg("exit");
    return 0;
}
