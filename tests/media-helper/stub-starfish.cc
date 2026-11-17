/* A stand-in for the TV's libplayerAPIs.so.1 for host tests of
 * src/media-helper.cc. It exports the StarfishMediaAPIs members the helper
 * resolves under the TV's mangled names. The callback's time is declared
 * long long so that the name matches on a 64-bit host. It logs every call to
 * $STUB_LOG and plays the part of the pipeline with LOADCOMPLETED after Load,
 * FRAMEREADY for fed video frames at their own pace once Play was called,
 * SEEKDONE after Seek and ENDOFSTREAM once the frames fed before pushEOS have
 * played. STUB_FAIL_LOAD=1 makes Load return false and
 * STUB_BUFFER_FULL_AT=N makes the Nth Feed answer BufferFull. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

long long now_ms()
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void stub_log(const char *fmt, ...)
{
    static FILE *f;
    const char *path = getenv("STUB_LOG");
    va_list ap;

    if (!f && path)
        f = fopen(path, "a");
    if (!f)
        return;
    fprintf(f, "%lld ", now_ms());
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

} // namespace

typedef void (*Callback)(int type, long long num, const char *str, void *data);

class StarfishMediaAPIs {
public:
    StarfishMediaAPIs(const char *uid);
    ~StarfishMediaAPIs();
    bool Load(const char *payload, Callback cb, void *data);
    std::string Feed(const char *payload);
    bool Play();
    bool Pause();
    bool Seek(const char *millis);
    bool flush();
    bool pushEOS();
    bool Unload();
    bool setVolume(const char *payload);
    bool notifyForeground();
    const char *getMediaID();

private:
    void Tick();

    std::mutex lock_;
    Callback cb_ = nullptr;
    void *data_ = nullptr;
    bool loaded_ = false;
    bool playing_ = false;
    bool eos_ = false;
    bool stop_ = false;
    long long play_started_ms_ = 0;
    long long base_ns_ = 0;        /* media time at play_started_ms_ */
    std::vector<long long> frames_;   /* fed video pts, ns, not yet shown */
    long long shown_ns_ = -1;
    int feeds_ = 0;
    std::thread thread_;
    char padding_[4096];
};

StarfishMediaAPIs::StarfishMediaAPIs(const char *uid)
{
    stub_log("ctor %s", uid ? uid : "null");
}

StarfishMediaAPIs::~StarfishMediaAPIs()
{
    stub_log("dtor");
    {
        std::lock_guard<std::mutex> g(lock_);
        stop_ = true;
    }
    if (thread_.joinable())
        thread_.join();
}

bool StarfishMediaAPIs::Load(const char *payload, Callback cb, void *data)
{
    stub_log("load %s", payload);
    if (getenv("STUB_FAIL_LOAD"))
        return false;
    cb_ = cb;
    data_ = data;
    thread_ = std::thread([this] {
        long long complete_at = now_ms() + 50;
        bool completed = false;

        for (;;) {
            {
                std::lock_guard<std::mutex> g(lock_);
                if (stop_)
                    return;
            }
            if (!completed && now_ms() >= complete_at) {
                completed = true;
                {
                    std::lock_guard<std::mutex> g(lock_);
                    loaded_ = true;
                }
                cb_(0x16, 0, "loadCompleted", data_);
            }
            if (completed)
                Tick();
            struct timespec ts = {0, 5 * 1000000};
            nanosleep(&ts, nullptr);
        }
    });
    return true;
}

/* Shows the fed frames whose time has come, one FRAMEREADY each. */
void StarfishMediaAPIs::Tick()
{
    std::vector<long long> due;
    bool ended = false;

    {
        std::lock_guard<std::mutex> g(lock_);
        if (!playing_)
            return;
        long long media_ns = base_ns_ + (now_ms() - play_started_ms_) * 1000000;
        std::sort(frames_.begin(), frames_.end());
        while (!frames_.empty() && frames_.front() <= media_ns) {
            due.push_back(frames_.front());
            frames_.erase(frames_.begin());
        }
        if (!due.empty())
            shown_ns_ = due.back();
        if (eos_ && frames_.empty()) {
            ended = true;
            eos_ = false;
        }
    }
    for (long long pts : due)
        cb_(0x0, pts, nullptr, data_);
    if (ended)
        cb_(0x1c, 0, "endOfStream", data_);
}

std::string StarfishMediaAPIs::Feed(const char *payload)
{
    long long pts = 0;
    int es = 0;
    unsigned size = 0;
    const char *p;

    if ((p = strstr(payload, "\"pts\":")) != nullptr)
        pts = atoll(p + 6);
    if ((p = strstr(payload, "\"esData\":")) != nullptr)
        es = atoi(p + 9);
    if ((p = strstr(payload, "\"bufferSize\":")) != nullptr)
        size = (unsigned)atoi(p + 13);
    feeds_++;
    stub_log("feed %d %lld %u", es, pts, size);
    const char *full = getenv("STUB_BUFFER_FULL_AT");
    if (full && atoi(full) == feeds_)
        return "{\"returnValue\":false,\"mediaStreamingStatus\":\"BufferFull\"}";
    if (es == 1) {
        std::lock_guard<std::mutex> g(lock_);
        frames_.push_back(pts);
    }
    return "{\"returnValue\":true,\"mediaStreamingStatus\":\"Ok\"}";
}

bool StarfishMediaAPIs::Play()
{
    stub_log("play");
    std::lock_guard<std::mutex> g(lock_);
    if (!loaded_)
        return true; /* queued, as on the TV */
    if (!playing_) {
        playing_ = true;
        play_started_ms_ = now_ms();
        if (!frames_.empty())
            base_ns_ = *std::min_element(frames_.begin(), frames_.end());
    }
    return true;
}

bool StarfishMediaAPIs::Pause()
{
    stub_log("pause");
    std::lock_guard<std::mutex> g(lock_);
    if (playing_) {
        base_ns_ += (now_ms() - play_started_ms_) * 1000000;
        playing_ = false;
    }
    return true;
}

bool StarfishMediaAPIs::Seek(const char *millis)
{
    stub_log("seek %s", millis);
    {
        std::lock_guard<std::mutex> g(lock_);
        base_ns_ = atoll(millis) * 1000000;
        play_started_ms_ = now_ms();
    }
    cb_(0x19, 0, "seekDone", data_);
    return true;
}

bool StarfishMediaAPIs::flush()
{
    stub_log("flush");
    std::lock_guard<std::mutex> g(lock_);
    frames_.clear();
    eos_ = false;
    return true;
}

bool StarfishMediaAPIs::pushEOS()
{
    stub_log("eos");
    std::lock_guard<std::mutex> g(lock_);
    eos_ = true;
    return true;
}

bool StarfishMediaAPIs::Unload()
{
    stub_log("unload");
    std::lock_guard<std::mutex> g(lock_);
    playing_ = false;
    loaded_ = false;
    return true;
}

bool StarfishMediaAPIs::setVolume(const char *payload)
{
    stub_log("volume %s", payload);
    return true;
}

bool StarfishMediaAPIs::notifyForeground()
{
    stub_log("foreground");
    return true;
}

const char *StarfishMediaAPIs::getMediaID()
{
    return "stub-media-id";
}
