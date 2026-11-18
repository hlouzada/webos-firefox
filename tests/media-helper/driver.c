/* Drives a media helper built for the host through src/media-protocol.h.
 * It starts the helper with a socketpair on fd 3 as Firefox's engine does,
 * runs one scenario and prints what it saw for run.sh to check. Exit status
 * 0 means the scenario ran to its end. The checks are in run.sh.
 *   driver <helper> play|seek|reload|retry|failload|badopen|unsupported */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "media-protocol.h"

static int fd;
static pid_t child;

static void die(const char *what)
{
    fprintf(stderr, "driver: %s (%s)\n", what, strerror(errno));
    if (child > 0)
        kill(child, SIGKILL);
    exit(1);
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void start(const char *helper)
{
    int sv[2];
    char *const argv[] = {(char *)helper, (char *)"--media-helper", NULL};

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        die("socketpair");
    child = fork();
    if (child < 0)
        die("fork");
    if (child == 0) {
        close(sv[0]);
        if (sv[1] != GM_HELPER_FD) {
            dup2(sv[1], GM_HELPER_FD);
            close(sv[1]);
        }
        execv(helper, argv);
        _exit(127);
    }
    close(sv[1]);
    fd = sv[0];
}

static int read_exact(void *buf, size_t n, int timeout_ms)
{
    char *p = buf;
    long long until = now_ms() + timeout_ms;

    while (n) {
        struct pollfd pf = {fd, POLLIN, 0};
        int left = (int)(until - now_ms());
        ssize_t r;

        if (left <= 0 || poll(&pf, 1, left) <= 0)
            return -1;
        r = read(fd, p, n);
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* Sends one request and returns its reply. */
static struct gm_msg request(uint32_t type, const int32_t *args, int64_t time_us, const void *payload,
                             uint32_t size, int timeout_ms)
{
    struct gm_msg m, reply;

    memset(&m, 0, sizeof m);
    m.magic = GM_MAGIC;
    m.type = type;
    m.size = size;
    if (args)
        memcpy(m.arg, args, sizeof m.arg);
    m.time_us = time_us;
    if (write(fd, &m, sizeof m) != (ssize_t)sizeof m)
        die("send header");
    if (size && write(fd, payload, size) != (ssize_t)size)
        die("send payload");
    if (read_exact(&reply, sizeof reply, timeout_ms) != 0)
        die("no reply");
    if (reply.magic != GM_MAGIC || reply.type != GM_REPLY)
        die("bad reply");
    return reply;
}

static struct gm_msg simple(uint32_t type, int timeout_ms)
{
    return request(type, NULL, 0, NULL, 0, timeout_ms);
}

static struct gm_msg open_protected(int vcodec, int with_audio, const char *window,
                                    unsigned protection)
{
    int32_t args[6] = {vcodec, 1280, 720, 25, 1, with_audio ? GM_AUDIO_AAC : GM_AUDIO_NONE};
    struct gm_open o;

    memset(&o, 0, sizeof o);
    o.audio_rate = 48000;
    o.audio_channels = 2;
    o.audio_profile = 2;
    o.audio_frequency_index = 3;
    o.protection = protection;
    snprintf(o.window_id, sizeof o.window_id, "%s", window);
    return request(GM_OPEN, args, 0, &o, sizeof o, 10000);
}

static struct gm_msg open_stream(int vcodec, int with_audio, const char *window)
{
    return open_protected(vcodec, with_audio, window, GM_PROTECTION_NONE);
}

static struct gm_msg feed(int track, int64_t pts_us, int key)
{
    int32_t args[6] = {track, key, 0, 0, 0, 0};
    unsigned char au[64];

    memset(au, track, sizeof au);
    return request(GM_FEED, args, pts_us, au, sizeof au, 3000);
}

/* Polls STATUS for up to timeout_ms until the flags include want. When want
 * is 0 it polls until time_us >= min_time. Returns the last reply. */
static struct gm_msg wait_for(int want, int64_t min_time, int timeout_ms)
{
    long long until = now_ms() + timeout_ms;
    struct gm_msg r;

    for (;;) {
        r = simple(GM_STATUS, 3000);
        if (want ? (r.arg[1] & want) == want : r.time_us >= min_time)
            return r;
        if (r.arg[1] & GM_F_FAILED)
            return r;
        if (now_ms() >= until)
            return r;
        usleep(10000);
    }
}

static void show(const char *tag, struct gm_msg r)
{
    printf("%s result=%d flags=0x%x error=%d fed=%d time=%lld\n", tag, r.arg[0], r.arg[1], r.arg[2],
           r.arg[3], (long long)r.time_us);
}

static void load_and_feed(int frames)
{
    int i;
    struct gm_msg r;

    r = request(GM_LOAD, NULL, 0, NULL, 0, 10000);
    show("load", r);
    r = wait_for(GM_F_PIPELINE, 0, 3000);
    show("pipeline", r);
    for (i = 0; i < frames; i++) {
        r = feed(GM_TRACK_VIDEO, (int64_t)i * 40000, i == 0);
        if (r.arg[0] != GM_OK)
            show("feed-video", r);
        r = feed(GM_TRACK_AUDIO, (int64_t)i * 40000, 1);
        if (r.arg[0] != GM_OK)
            show("feed-audio", r);
    }
}

int main(int argc, char **argv)
{
    const char *scenario;
    struct gm_msg r;
    int32_t args[6] = {0};

    if (argc < 3) {
        fprintf(stderr, "usage: driver <helper> <scenario>\n");
        return 2;
    }
    scenario = argv[2];
    start(argv[1]);

    if (!strcmp(scenario, "badopen")) {
        r = open_stream(9, 0, "w1");
        show("open", r);
    } else if (!strcmp(scenario, "unsupported")) {
        /* The helper refuses audio that is not AAC. */
        int32_t a[6] = {GM_VIDEO_H264, 1280, 720, 25, 1, 7};
        struct gm_open o;

        memset(&o, 0, sizeof o);
        o.audio_rate = 48000;
        o.audio_channels = 2;
        r = request(GM_OPEN, a, 0, &o, sizeof o, 10000);
        show("open", r);
    } else if (!strcmp(scenario, "protected") || !strcmp(scenario, "protected-svp")) {
        /* A Widevine stream: the pipeline is loaded for the secure video path. */
        r = open_protected(GM_VIDEO_H264, 1, "w9", GM_PROTECTION_WIDEVINE);
        show("open", r);
        load_and_feed(2);
        r = wait_for(GM_F_PIPELINE, 0, 3000);
        show("pipeline", r);
    } else if (!strcmp(scenario, "badprotection")) {
        r = open_protected(GM_VIDEO_H264, 1, "w9", 7);
        show("open", r);
    } else if (!strcmp(scenario, "failload")) {
        r = open_stream(GM_VIDEO_H264, 1, "w1");
        show("open", r);
        r = request(GM_LOAD, NULL, 0, NULL, 0, 10000);
        show("load", r);
        r = wait_for(GM_F_FAILED, 0, 3000);
        show("status", r);
    } else if (!strcmp(scenario, "retry")) {
        r = open_stream(GM_VIDEO_H264, 0, "w1");
        show("open", r);
        r = request(GM_LOAD, NULL, 0, NULL, 0, 10000);
        r = wait_for(GM_F_PIPELINE, 0, 3000);
        r = feed(GM_TRACK_VIDEO, 0, 1);
        show("feed1", r);
        r = feed(GM_TRACK_VIDEO, 40000, 0);
        show("feed2", r);
        r = feed(GM_TRACK_VIDEO, 80000, 0);
        show("feed3", r);
        r = feed(GM_TRACK_VIDEO, 80000, 0);
        show("feed3-again", r);
    } else if (!strcmp(scenario, "play") || !strcmp(scenario, "seek") ||
               !strcmp(scenario, "reload")) {
        r = open_stream(GM_VIDEO_H264, 1, "w123");
        show("open", r);
        args[0] = 50;
        r = request(GM_VOLUME, args, 0, NULL, 0, 3000);
        load_and_feed(10);
        r = simple(GM_PLAY, 3000);
        show("play", r);
        r = wait_for(GM_F_LOADED, 0, 3000);
        show("loaded", r);
        r = wait_for(GM_F_HAS_FRAME, 0, 3000);
        show("first-frame", r);
        r = wait_for(0, 200000, 3000);
        show("advanced", r);
        if (!strcmp(scenario, "seek")) {
            r = request(GM_SEEK, NULL, 5000000, NULL, 0, 3000);
            show("seek", r);
            r = feed(GM_TRACK_VIDEO, 5000000, 1);
            show("feed-after-seek", r);
            r = feed(GM_TRACK_VIDEO, 5040000, 0);
            r = wait_for(GM_F_HAS_FRAME | GM_F_SEEKDONE, 0, 3000);
            show("after-seek", r);
        } else if (!strcmp(scenario, "reload")) {
            r = simple(GM_UNLOAD, 3000);
            show("unload", r);
            r = request(GM_LOAD, NULL, 7000000, NULL, 0, 10000);
            show("reload", r);
            r = wait_for(GM_F_PIPELINE, 0, 3000);
            show("pipeline-again", r);
            r = feed(GM_TRACK_VIDEO, 7000000, 1);
            show("feed-after-reload", r);
            r = feed(GM_TRACK_VIDEO, 7040000, 0);
            r = wait_for(GM_F_HAS_FRAME, 0, 3000);
            show("after-reload", r);
        } else {
            args[0] = GM_TRACK_VIDEO;
            r = request(GM_EOS, args, 0, NULL, 0, 3000);
            show("eos-video", r);
            args[0] = GM_TRACK_AUDIO;
            r = request(GM_EOS, args, 0, NULL, 0, 3000);
            show("eos-audio", r);
            r = wait_for(GM_F_ENDED, 0, 3000);
            show("ended", r);
        }
        r = simple(GM_PAUSE, 3000);
        show("pause", r);
    } else {
        fprintf(stderr, "unknown scenario\n");
        return 2;
    }
    r = simple(GM_CLOSE, 3000);
    show("close", r);
    close(fd);
    {
        int status = 0;
        long long until = now_ms() + 3000;

        while (now_ms() < until && waitpid(child, &status, WNOHANG) == 0)
            usleep(10000);
        printf("helper exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    return 0;
}
