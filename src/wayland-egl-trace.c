/* A libwayland-egl.so.1 that logs every wl_egl_window call with its thread
 * and hands it to the TV's own library, named in GECKOTV_EGL_TRACE_REAL. The
 * launcher puts it first in the library path when GECKOTV_EGL_TRACE=1 is in
 * the app's env file. Firefox reaches these calls through its libmozwayland,
 * so a library in the search path is the only place to see them. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct wl_surface;
struct wl_egl_window;

#define EXPORT __attribute__((visibility("default")))

EXPORT struct wl_egl_window *wl_egl_window_create(struct wl_surface *surface, int width,
                                                  int height);
EXPORT void wl_egl_window_destroy(struct wl_egl_window *egl_window);
EXPORT void wl_egl_window_resize(struct wl_egl_window *egl_window, int width, int height,
                                 int dx, int dy);
EXPORT void wl_egl_window_get_attached_size(struct wl_egl_window *egl_window, int *width,
                                            int *height);

static struct {
    struct wl_egl_window *(*create)(struct wl_surface *, int, int);
    void (*destroy)(struct wl_egl_window *);
    void (*resize)(struct wl_egl_window *, int, int, int, int);
    void (*get_attached_size)(struct wl_egl_window *, int *, int *);
} real;

/* The window structure belongs to the driver, so the surface each window
 * was made for is remembered here. */
#define WINDOWS 64
static struct {
    struct wl_egl_window *window;
    struct wl_surface *surface;
} windows[WINDOWS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static void load_real(void)
{
    const char *path = getenv("GECKOTV_EGL_TRACE_REAL");
    void *h;

    if (real.create)
        return;
    if (!path || !path[0]) {
        fprintf(stderr, "wayland-egl-trace: GECKOTV_EGL_TRACE_REAL is not set\n");
        abort();
    }
    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "wayland-egl-trace: cannot load %s: %s\n", path, dlerror());
        abort();
    }
    real.create = (struct wl_egl_window *(*)(struct wl_surface *, int, int))
        dlsym(h, "wl_egl_window_create");
    real.destroy = (void (*)(struct wl_egl_window *))dlsym(h, "wl_egl_window_destroy");
    real.resize = (void (*)(struct wl_egl_window *, int, int, int, int))
        dlsym(h, "wl_egl_window_resize");
    real.get_attached_size = (void (*)(struct wl_egl_window *, int *, int *))
        dlsym(h, "wl_egl_window_get_attached_size");
    if (!real.create || !real.destroy || !real.resize) {
        fprintf(stderr, "wayland-egl-trace: %s lacks the wl_egl_window functions\n", path);
        abort();
    }
}

__attribute__((format(printf, 1, 2)))
static void trace(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03ld wayland-egl: ", tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " tid %ld\n", (long)syscall(SYS_gettid));
    fflush(stderr);
}

static struct wl_surface *surface_of(struct wl_egl_window *window, int forget)
{
    struct wl_surface *s = NULL;
    int i;

    pthread_mutex_lock(&mu);
    for (i = 0; i < WINDOWS; i++)
        if (windows[i].window == window) {
            s = windows[i].surface;
            if (forget)
                windows[i].window = NULL;
            break;
        }
    pthread_mutex_unlock(&mu);
    return s;
}

struct wl_egl_window *wl_egl_window_create(struct wl_surface *surface, int width, int height)
{
    struct wl_egl_window *w;
    int i;

    load_real();
    w = real.create(surface, width, height);
    pthread_mutex_lock(&mu);
    for (i = 0; i < WINDOWS && windows[i].window; i++)
        ;
    if (i < WINDOWS) {
        windows[i].window = w;
        windows[i].surface = surface;
    }
    pthread_mutex_unlock(&mu);
    trace("create window %p for surface %p %dx%d", (void *)w, (void *)surface, width, height);
    return w;
}

void wl_egl_window_destroy(struct wl_egl_window *egl_window)
{
    struct wl_surface *s = surface_of(egl_window, 1);

    load_real();
    trace("destroy window %p of surface %p", (void *)egl_window, (void *)s);
    real.destroy(egl_window);
}

void wl_egl_window_resize(struct wl_egl_window *egl_window, int width, int height, int dx,
                          int dy)
{
    load_real();
    trace("resize window %p of surface %p to %dx%d offset %d,%d", (void *)egl_window,
          (void *)surface_of(egl_window, 0), width, height, dx, dy);
    real.resize(egl_window, width, height, dx, dy);
}

void wl_egl_window_get_attached_size(struct wl_egl_window *egl_window, int *width, int *height)
{
    load_real();
    if (real.get_attached_size) {
        real.get_attached_size(egl_window, width, height);
        return;
    }
    if (width)
        *width = 0;
    if (height)
        *height = 0;
}
