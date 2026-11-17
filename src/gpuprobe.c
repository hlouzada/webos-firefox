/* GPU probe. Does EGL/GLES work on this TV the way Firefox would use it, and
 * how does the compositor show a second surface of the app?
 *
 * Firefox only reports "blocklisted" when its own GPU probe (glxtest) hangs
 * or fails. This program walks the same path one logged step at a time, with
 * a watchdog per step, so a single run shows where it stops:
 *
 *   window   a full-screen wl_shell toplevel with the app's wl_webos_shell
 *            properties made directly rather than through the adapter's
 *            flat-mode window. That window copies shared-memory buffers
 *            and cannot show what the GPU draws.
 *   EGL      eglGetDisplay (legacy) and eglGetPlatformDisplay (Wayland
 *            platform), each tried in its own child process
 *   GLES     context, strings, a few coloured frames, swap timing
 *   popups   a second surface shown under each webOS window type in turn
 *            with the shell events it gets, to find a type the compositor
 *            draws above the window at its own position. Firefox's menus
 *            and drop-downs need one once the window is drawn by the GPU.
 *
 * libEGL/libGLESv2 are loaded at run time like Firefox does; libwayland-egl
 * is the TV's own, or the runtime's fallback on TVs without one (webOS 4).
 * Run as root on the TV:  <app dir>/gpuprobe   (it logs to stdout)
 * GPUPROBE_SKIP_GL=1 runs only the popup part, GPUPROBE_SKIP_POPUPS=1 only
 * the GL part, GPUPROBE_FOREIGN=1 only the foreign-attach part and
 * GPUPROBE_OVERLAY=1 only the transparent-overlay part. GPUPROBE_SIZE=WxH
 * overrides the window size. The default is the output's current mode and
 * else 1920x1080.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <wayland-client.h>

#include "wayland-webos-shell-client-protocol.h"
#include "webos-foreign-client-protocol.h"

#define STEP_TIMEOUT 8

static const char *step_name = "start";
static char app_dir[512];

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static void on_alarm(int sig)
{
    (void)sig;
    printf("  HUNG: no progress in %d s at step \"%s\"\n", STEP_TIMEOUT, step_name);
    fflush(stdout);
    _exit(3);
}

static void on_crash(int sig)
{
    printf("  CRASHED: signal %d at step \"%s\"\n", sig, step_name);
    fflush(stdout);
    _exit(4);
}

static void step(const char *name)
{
    step_name = name;
    printf("- %s\n", name);
    fflush(stdout);
    alarm(STEP_TIMEOUT);
}

static const char *app_id(void)
{
    const char *id = getenv("APPID");

    return id && id[0] ? id : "com.github.gprot42.geckotv";
}

/* ---- EGL / GLES, loaded at run time ---- */
static void *egl_lib, *gles_lib, *wegl_lib;
static PFNEGLGETPROCADDRESSPROC p_eglGetProcAddress;
#define EGLFN(ret, name, args) static ret (*p_##name) args;
EGLFN(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType))
EGLFN(EGLBoolean, eglInitialize, (EGLDisplay, EGLint *, EGLint *))
EGLFN(const char *, eglQueryString, (EGLDisplay, EGLint))
EGLFN(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))
EGLFN(EGLBoolean, eglBindAPI, (EGLenum))
EGLFN(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint *))
EGLFN(EGLSurface, eglCreateWindowSurface, (EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *))
EGLFN(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
EGLFN(EGLBoolean, eglSwapBuffers, (EGLDisplay, EGLSurface))
EGLFN(EGLBoolean, eglSwapInterval, (EGLDisplay, EGLint))
EGLFN(EGLint, eglGetError, (void))
EGLFN(EGLBoolean, eglTerminate, (EGLDisplay))
static const GLubyte *(*p_glGetString)(GLenum);
static void (*p_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
static void (*p_glClear)(GLbitfield);
static void (*p_glFinish)(void);
struct wl_egl_window;
static struct wl_egl_window *(*p_wl_egl_window_create)(struct wl_surface *, int, int);

static void *open_first(const char *what, const char *const *names)
{
    int i;

    for (i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);

        if (h) {
            say("  %s: %s", what, names[i]);
            return h;
        }
    }
    say("  %s: NOT FOUND (%s)", what, dlerror());
    return NULL;
}

static int load_libraries(void)
{
    static const char *const egl[] = { "libEGL.so.1", "libEGL.so", NULL };
    static const char *const gles[] = { "libGLESv2.so.2", "libGLESv2.so", NULL };
    char fallback[600];
    const char *wegl[4] = { "libwayland-egl.so.1", "libwayland-egl.so.0", NULL, NULL };
    Dl_info info;

    /* The TV's libwayland-egl under either soname and else the runtime's
     * stand-in. webOS 6.5 ships only libwayland-egl.so.0. GPUPROBE_WEGL
     * names one to use instead. */
    snprintf(fallback, sizeof fallback, "%s/firefox-runtime/fallback/libwayland-egl.so.1",
             app_dir);
    wegl[2] = fallback;
    if (getenv("GPUPROBE_WEGL")) {
        wegl[0] = getenv("GPUPROBE_WEGL");
        wegl[1] = NULL;
    }
    step("load libraries");
    egl_lib = open_first("libEGL", egl);
    gles_lib = open_first("libGLESv2", gles);
    wegl_lib = open_first("libwayland-egl", wegl);
    if (!egl_lib || !gles_lib || !wegl_lib)
        return 0;
#define LOAD(lib, name) \
    do { *(void **)&p_##name = dlsym(lib, #name); \
         if (!p_##name) { say("  missing symbol %s", #name); return 0; } } while (0)
    LOAD(egl_lib, eglGetProcAddress);
    LOAD(egl_lib, eglGetDisplay);
    LOAD(egl_lib, eglInitialize);
    LOAD(egl_lib, eglQueryString);
    LOAD(egl_lib, eglChooseConfig);
    LOAD(egl_lib, eglBindAPI);
    LOAD(egl_lib, eglCreateContext);
    LOAD(egl_lib, eglCreateWindowSurface);
    LOAD(egl_lib, eglMakeCurrent);
    LOAD(egl_lib, eglSwapBuffers);
    LOAD(egl_lib, eglSwapInterval);
    LOAD(egl_lib, eglGetError);
    LOAD(egl_lib, eglTerminate);
    LOAD(gles_lib, glGetString);
    LOAD(gles_lib, glClearColor);
    LOAD(gles_lib, glClear);
    LOAD(gles_lib, glFinish);
    LOAD(wegl_lib, wl_egl_window_create);
    if (dladdr((void *)p_eglGetDisplay, &info) && info.dli_fname)
        say("  libEGL resolves to %s", info.dli_fname);
    return 1;
}

/* ---- Wayland window made the way the adapter makes Firefox's GL window ---- */
static struct {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_shell *shell;
    struct wl_webos_shell *webos_shell;
    struct wl_output *output;
    struct wl_webos_foreign *foreign;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_surface *test;    /* the surface under test, for pointer logs */
    struct wl_surface *main;
    struct wl_shell_surface *shell_surface;
    struct wl_webos_shell_surface *webos_surface;
    int width, height;
    int out_w, out_h;
    int frames_done;            /* frame callbacks answered for the window */
    int main_entered;           /* wl_surface.enter seen on the window */
} w;

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
                            const char *iface, uint32_t version)
{
    (void)data;
    if (!strcmp(iface, "wl_compositor"))
        w.compositor = wl_registry_bind(r, name, &wl_compositor_interface,
                                        version < 3 ? version : 3);
    else if (!strcmp(iface, "wl_shm"))
        w.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "wl_shell"))
        w.shell = wl_registry_bind(r, name, &wl_shell_interface, 1);
    else if (!strcmp(iface, "wl_webos_shell"))
        w.webos_shell = wl_registry_bind(r, name, &wl_webos_shell_interface, 1);
    else if (!strcmp(iface, "wl_output") && !w.output)
        w.output = wl_registry_bind(r, name, &wl_output_interface, 1);
    else if (!strcmp(iface, "wl_webos_foreign"))
        w.foreign = wl_registry_bind(r, name, &wl_webos_foreign_interface, 1);
    else if (!strcmp(iface, "wl_seat") && !w.seat)
        w.seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}

static const char *surface_name(struct wl_surface *s)
{
    return s == w.main ? "window" : s == w.test ? "TEST SURFACE" : "other";
}

static void pointer_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                          wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)serial;
    say("  POINTER enter %s at %d,%d", surface_name(s), wl_fixed_to_int(x), wl_fixed_to_int(y));
}

static void pointer_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
    (void)d; (void)p; (void)serial;
    say("  POINTER leave %s", surface_name(s));
}

static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)t; (void)x; (void)y;
}

static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
                           uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)serial; (void)t;
    say("  POINTER button %u %s", button, state ? "pressed" : "released");
}

static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
    (void)d; (void)p; (void)t; (void)axis; (void)v;
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_enter, pointer_leave, pointer_motion, pointer_button, pointer_axis
};

static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
    (void)data;
    (void)r;
    (void)name;
}

static const struct wl_registry_listener registry_listener = { registry_global, registry_remove };

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                            int32_t ph, int32_t sub, const char *make, const char *model,
                            int32_t transform)
{
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)transform;
    say("  output %s %s", make ? make : "?", model ? model : "?");
}

static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t width,
                        int32_t height, int32_t refresh)
{
    (void)d; (void)o;
    say("  output mode %dx%d at %d mHz%s", width, height, refresh,
        flags & WL_OUTPUT_MODE_CURRENT ? " (current)" : "");
    if ((flags & WL_OUTPUT_MODE_CURRENT) && width > 0 && height > 0) {
        w.out_w = width;
        w.out_h = height;
    }
}

static void output_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; (void)f; }

static const struct wl_output_listener output_listener = {
    output_geometry, output_mode, output_done, output_scale
};

static void shell_ping(void *d, struct wl_shell_surface *s, uint32_t serial)
{
    (void)d;
    wl_shell_surface_pong(s, serial);
}

static void shell_configure(void *d, struct wl_shell_surface *s, uint32_t edges, int32_t width,
                            int32_t height)
{
    (void)d; (void)s; (void)edges;
    say("  [%s] wl_shell configure %dx%d", (const char *)d, width, height);
}

static void shell_popup_done(void *d, struct wl_shell_surface *s)
{
    (void)s;
    say("  [%s] wl_shell popup_done", (const char *)d);
}

static const struct wl_shell_surface_listener shell_listener = {
    shell_ping, shell_configure, shell_popup_done
};

static void webos_state(void *d, struct wl_webos_shell_surface *s, uint32_t state)
{
    (void)s;
    say("  [%s] webos state_changed %u (0 default, 1 minimized, 2 maximized, 3 fullscreen)",
        (const char *)d, state);
}

static void webos_position(void *d, struct wl_webos_shell_surface *s, int32_t x, int32_t y)
{
    (void)s;
    say("  [%s] webos position_changed %d,%d", (const char *)d, x, y);
}

static void webos_close(void *d, struct wl_webos_shell_surface *s)
{
    (void)s;
    say("  [%s] webos close", (const char *)d);
}

static void webos_exposed(void *d, struct wl_webos_shell_surface *s, struct wl_array *rects)
{
    (void)s;
    say("  [%s] webos exposed (%zu bytes of rectangles)", (const char *)d, rects->size);
}

static void webos_about(void *d, struct wl_webos_shell_surface *s, uint32_t state)
{
    (void)s;
    say("  [%s] webos state_about_to_change %u", (const char *)d, state);
}

static void webos_addon(void *d, struct wl_webos_shell_surface *s, uint32_t status)
{
    (void)d; (void)s; (void)status;
}

static const struct wl_webos_shell_surface_listener webos_listener = {
    webos_state, webos_position, webos_close, webos_exposed, webos_about, webos_addon
};

static void surface_enter(void *d, struct wl_surface *s, struct wl_output *o)
{
    (void)s; (void)o;
    say("  [%s] wl_surface enter output (the compositor shows it)", (const char *)d);
    if (!strcmp((const char *)d, "window"))
        w.main_entered = 1;
}

static void surface_leave(void *d, struct wl_surface *s, struct wl_output *o)
{
    (void)s; (void)o;
    say("  [%s] wl_surface leave output", (const char *)d);
}

static const struct wl_surface_listener surface_listener = { surface_enter, surface_leave };

static void frame_done(void *d, struct wl_callback *cb, uint32_t time)
{
    (void)d; (void)time;
    wl_callback_destroy(cb);
    w.frames_done++;
}

static const struct wl_callback_listener frame_listener = { frame_done };

static int memfd(size_t size)
{
    int fd = -1;
#ifdef __NR_memfd_create
    fd = syscall(__NR_memfd_create, "gpuprobe", 1u /* MFD_CLOEXEC */);
#endif
    if (fd < 0) {
        char path[] = "/tmp/gpuprobe-XXXXXX";

        fd = mkstemp(path);
        if (fd >= 0)
            unlink(path);
    }
    if (fd >= 0 && ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

/* A shared-memory buffer of one colour with a white border. *pix maps it. */
static struct wl_buffer *solid_buffer(int width, int height, uint32_t colour, uint32_t **pix)
{
    size_t size = (size_t)width * (size_t)height * 4;
    int fd = memfd(size);
    struct wl_shm_pool *pool;
    struct wl_buffer *buffer;
    uint32_t *p;
    int x, y;

    if (fd < 0)
        return NULL;
    p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++) {
            int edge = x < 8 || y < 8 || x >= width - 8 || y >= height - 8;

            p[(size_t)y * width + x] = edge ? 0xffffffffu : colour;
        }
    pool = wl_shm_create_pool(w.shm, fd, (int32_t)size);
    buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    *pix = p;
    return buffer;
}

static void set_app_properties(struct wl_webos_shell_surface *ws)
{
    wl_webos_shell_surface_set_property(ws, "appId", app_id());
    wl_webos_shell_surface_set_property(ws, "title", "Firefox");
    wl_webos_shell_surface_set_property(ws, "displayAffinity", "0");
    wl_webos_shell_surface_set_property(ws, "_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
}

/* The window is a full-screen wl_shell toplevel with the app's identity. The
 * first content committed to it makes it the app's card. */
static int make_window(int with_content)
{
    struct wl_registry *registry;
    const char *size = getenv("GPUPROBE_SIZE");

    step("connect to the compositor");
    w.display = wl_display_connect(NULL);
    if (!w.display) {
        say("  FAILED: wl_display_connect (XDG_RUNTIME_DIR=%s)", getenv("XDG_RUNTIME_DIR"));
        return 0;
    }
    registry = wl_display_get_registry(w.display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(w.display);
    say("  compositor %s, shm %s, wl_shell %s, wl_webos_shell %s, output %s",
        w.compositor ? "yes" : "NO", w.shm ? "yes" : "NO", w.shell ? "yes" : "NO",
        w.webos_shell ? "yes" : "NO", w.output ? "yes" : "NO");
    if (!w.compositor || !w.shm || !w.shell || !w.webos_shell)
        return 0;
    if (w.output)
        wl_output_add_listener(w.output, &output_listener, NULL);
    wl_display_roundtrip(w.display);
    w.width = w.out_w > 0 ? w.out_w : 1920;
    w.height = w.out_h > 0 ? w.out_h : 1080;
    if (size && sscanf(size, "%dx%d", &w.width, &w.height) != 2) {
        say("  GPUPROBE_SIZE must be WxH");
        return 0;
    }

    step("map the window");
    w.main = wl_compositor_create_surface(w.compositor);
    wl_surface_add_listener(w.main, &surface_listener, "window");
    w.shell_surface = wl_shell_get_shell_surface(w.shell, w.main);
    wl_shell_surface_add_listener(w.shell_surface, &shell_listener, "window");
    wl_shell_surface_set_toplevel(w.shell_surface);
    w.webos_surface = wl_webos_shell_get_shell_surface(w.webos_shell, w.main);
    wl_webos_shell_surface_add_listener(w.webos_surface, &webos_listener, "window");
    set_app_properties(w.webos_surface);
    wl_surface_commit(w.main);
    wl_display_roundtrip(w.display);
    wl_webos_shell_surface_set_state(w.webos_surface, WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);
    if (with_content) {
        uint32_t *pix;
        struct wl_buffer *b = solid_buffer(w.width, w.height, 0xff203050u, &pix);

        if (!b) {
            say("  FAILED: no memory for the window buffer");
            return 0;
        }
        wl_surface_attach(w.main, b, 0, 0);
        wl_surface_damage(w.main, 0, 0, w.width, w.height);
        wl_surface_commit(w.main);
    }
    wl_display_roundtrip(w.display);
    say("  window %dx%d mapped%s", w.width, w.height, with_content ? " with a dark blue frame" : "");
    return 1;
}

/* Dispatches what the compositor sent and flushes without blocking. */
static void pump(void)
{
    wl_display_dispatch_pending(w.display);
    wl_display_flush(w.display);
    while (wl_display_prepare_read(w.display) != 0)
        wl_display_dispatch_pending(w.display);
    {
        struct pollfd pfd = { wl_display_get_fd(w.display), POLLIN, 0 };

        if (poll(&pfd, 1, 0) > 0)
            wl_display_read_events(w.display);
        else
            wl_display_cancel_read(w.display);
    }
    wl_display_dispatch_pending(w.display);
}

static const char *egl_err(void)
{
    static char buf[16];

    snprintf(buf, sizeof buf, "0x%04x", p_eglGetError());
    return buf;
}

/* One EGL variant, in a child process: display, context, frames. */
static int run_variant(int platform)
{
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext ctx;
    EGLSurface surf;
    struct wl_egl_window *ew;
    EGLint major = 0, minor = 0, n = 0;
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    double t0, t;
    int i;

    if (!load_libraries() || !make_window(0))
        return 1;

    if (platform) {
        PFNEGLGETPLATFORMDISPLAYEXTPROC gpd;

        step("eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND)");
        gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)p_eglGetProcAddress("eglGetPlatformDisplay");
        if (!gpd)
            gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)p_eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (!gpd) {
            say("  not available (no eglGetPlatformDisplay[EXT])");
            return 1;
        }
        dpy = gpd(EGL_PLATFORM_WAYLAND_KHR, w.display, NULL);
    } else {
        step("eglGetDisplay(wl_display)");
        dpy = p_eglGetDisplay((EGLNativeDisplayType)w.display);
    }
    if (dpy == EGL_NO_DISPLAY) {
        say("  FAILED: no display (%s)", egl_err());
        return 1;
    }
    say("  OK");

    step("eglInitialize");
    if (!p_eglInitialize(dpy, &major, &minor)) {
        say("  FAILED: %s", egl_err());
        return 1;
    }
    say("  OK: EGL %d.%d", major, minor);
    say("  vendor:     %s", p_eglQueryString(dpy, EGL_VENDOR));
    say("  version:    %s", p_eglQueryString(dpy, EGL_VERSION));
    say("  client API: %s", p_eglQueryString(dpy, EGL_CLIENT_APIS));
    say("  extensions: %s", p_eglQueryString(dpy, EGL_EXTENSIONS));

    step("eglChooseConfig (RGBA8888, GLES2, window)");
    if (!p_eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        say("  FAILED: %d configs (%s)", n, egl_err());
        return 1;
    }
    say("  OK");

    step("eglCreateContext (GLES 2)");
    p_eglBindAPI(EGL_OPENGL_ES_API);
    ctx = p_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        say("  FAILED: %s", egl_err());
        return 1;
    }
    say("  OK");

    step("wl_egl_window_create + eglCreateWindowSurface on the window itself");
    ew = p_wl_egl_window_create(w.main, w.width, w.height);
    if (!ew) {
        say("  FAILED: wl_egl_window_create returned NULL");
        return 1;
    }
    surf = p_eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)ew, NULL);
    if (surf == EGL_NO_SURFACE) {
        say("  FAILED: %s", egl_err());
        return 1;
    }
    say("  OK");

    step("eglMakeCurrent");
    if (!p_eglMakeCurrent(dpy, surf, surf, ctx)) {
        say("  FAILED: %s", egl_err());
        return 1;
    }
    say("  OK");
    say("  GL vendor:   %s", (const char *)p_glGetString(GL_VENDOR));
    say("  GL renderer: %s", (const char *)p_glGetString(GL_RENDERER));
    say("  GL version:  %s", (const char *)p_glGetString(GL_VERSION));
    say("  GLSL:        %s", (const char *)p_glGetString(GL_SHADING_LANGUAGE_VERSION));

    /* Firefox renders with swap interval 0 and paces itself. */
    step("render 180 frames (red, green, blue for about a second each)");
    p_eglSwapInterval(dpy, 0);
    t0 = now_ms();
    for (i = 0; i < 180; i++) {
        float c = (float)(i % 60) / 60.0f;

        alarm(STEP_TIMEOUT);
        if (i < 60)
            p_glClearColor(0.4f + 0.6f * c, 0.0f, 0.0f, 1.0f);
        else if (i < 120)
            p_glClearColor(0.0f, 0.4f + 0.6f * c, 0.0f, 1.0f);
        else
            p_glClearColor(0.0f, 0.0f, 0.4f + 0.6f * c, 1.0f);
        p_glClear(GL_COLOR_BUFFER_BIT);
        if (!p_eglSwapBuffers(dpy, surf)) {
            say("  FAILED at frame %d: eglSwapBuffers %s", i, egl_err());
            return 1;
        }
        if (i == 0)
            say("  first swap returned after %.0f ms", now_ms() - t0);
        wl_display_dispatch_pending(w.display);
        wl_display_flush(w.display);
        usleep(16000);
    }
    p_glFinish();
    t = now_ms() - t0;
    say("  OK: 180 frames in %.0f ms (%.1f fps with a 16 ms pause per frame), window entered "
        "an output: %s", t, 180000.0 / t, w.main_entered ? "yes" : "no");
    say("  (on screen: the window turned red, then green, then blue)");

    step("clean up");
    p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    p_eglTerminate(dpy);
    alarm(0);
    say("  OK");
    return 0;
}

struct variant {
    const char *name;
    const char *type;        /* _WEBOS_WINDOW_TYPE, NULL for none */
    int transient;           /* wl_shell set_transient instead of set_toplevel */
    int hint;                /* set_location_hint value, 0 for none */
    uint32_t colour;
};

/* ---- A surface attached into an exported region of the window ---- */

static char exported_id[128];

static void exported_window_id(void *d, struct wl_webos_exported *e, const char *id, uint32_t type)
{
    (void)d; (void)e;
    snprintf(exported_id, sizeof exported_id, "%s", id ? id : "");
    say("  exported window id %s (type %u)", exported_id, type);
}

static const struct wl_webos_exported_listener exported_listener = { exported_window_id };

static void imported_region(void *d, struct wl_webos_imported *i, uint32_t width, uint32_t height)
{
    (void)d; (void)i;
    say("  imported: destination region %ux%u", width, height);
}

static const struct wl_webos_imported_listener imported_listener = { imported_region };

/* The window shows a moving bar. With the pointer bound, enter, leave and
 * button events say which surface the compositor gives input to. */
static struct wl_buffer *bar_bufs[2];
static uint32_t *bar_pix[2];
static int bar_cur, bar_x;

static int bar_start(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        bar_bufs[i] = solid_buffer(w.width, w.height, 0xff203050u, &bar_pix[i]);
        if (!bar_bufs[i])
            return 0;
    }
    if (w.seat) {
        w.pointer = wl_seat_get_pointer(w.seat);
        wl_pointer_add_listener(w.pointer, &pointer_listener, NULL);
    }
    return 1;
}

static void bar_hold(int ms)
{
    double t = now_ms();

    while (now_ms() - t < ms) {
        int x, y;

        alarm(STEP_TIMEOUT);
        bar_cur ^= 1;
        bar_x = (bar_x + 24) % (w.width - 48);
        for (y = 40; y < 80; y++)
            for (x = 8; x < w.width - 8; x++)
                bar_pix[bar_cur][(size_t)y * w.width + x] =
                    x >= bar_x && x < bar_x + 48 ? 0xffffffffu : 0xff203050u;
        wl_surface_attach(w.main, bar_bufs[bar_cur], 0, 0);
        wl_surface_damage(w.main, 0, 40, w.width, 40);
        wl_callback_add_listener(wl_surface_frame(w.main), &frame_listener, NULL);
        wl_surface_commit(w.main);
        pump();
        usleep(100000);
    }
}

struct fvariant {
    const char *name;
    uint32_t type;     /* exported type */
    int z;             /* set_z_index value, -1 for none */
    uint32_t colour;
};

static const struct fvariant fvariants[] = {
    { "transparent_object", WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_TRANSPARENT_OBJECT, -1, 0xffc02020u },
    { "opaque_object", WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_OPAQUE_OBJECT, -1, 0xff20a020u },
    { "transparent_object, z-index 1", WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_TRANSPARENT_OBJECT, 1, 0xffd0a000u },
    { "video_object", WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_VIDEO_OBJECT, -1, 0xffa020a0u },
};

static int foreign_variants(void)
{
    size_t v;

    if (!make_window(1) || !bar_start())
        return 1;
    if (!w.foreign) {
        say("  no wl_webos_foreign");
        return 1;
    }
    bar_hold(1000);
    for (v = 0; v < sizeof fvariants / sizeof fvariants[0]; v++) {
        const struct fvariant *var = &fvariants[v];
        struct wl_webos_exported *exported;
        struct wl_webos_imported *imported = NULL;
        struct wl_region *region;
        struct wl_surface *s;
        struct wl_buffer *b;
        uint32_t *p;
        int pw = 480, ph = 270, i;
        char tag[80];

        snprintf(tag, sizeof tag, "foreign %zu", v + 1);
        step(tag);
        say("  %s: export a %dx%d region at 300,200 as %s, attach a surface of colour 0x%06x",
            tag, pw, ph, var->name, var->colour & 0xffffffu);
        exported_id[0] = '\0';
        exported = wl_webos_foreign_export_element(w.foreign, w.main, var->type);
        wl_webos_exported_add_listener(exported, &exported_listener, NULL);
        region = wl_compositor_create_region(w.compositor);
        wl_region_add(region, 300, 200, pw, ph);
        wl_webos_exported_set_exported_window(exported, region, region);
        wl_region_destroy(region);
        wl_surface_commit(w.main);
        for (i = 0; i < 30 && !exported_id[0]; i++) {
            pump();
            usleep(100000);
        }
        if (!exported_id[0]) {
            say("  no window id within 3 s");
            wl_webos_exported_destroy(exported);
            continue;
        }
        s = wl_compositor_create_surface(w.compositor);
        w.test = s;
        wl_surface_add_listener(s, &surface_listener, (void *)tag);
        b = solid_buffer(pw, ph, var->colour, &p);
        if (!b)
            return 1;
        wl_surface_attach(s, b, 0, 0);
        wl_surface_damage(s, 0, 0, pw, ph);
        wl_surface_commit(s);
        imported = wl_webos_foreign_import_element(w.foreign, exported_id, var->type);
        wl_webos_imported_add_listener(imported, &imported_listener, NULL);
        if (var->z >= 0 && wl_proxy_get_version((struct wl_proxy *)imported) >= 2)
            wl_webos_imported_set_z_index(imported, (uint32_t)var->z);
        wl_webos_imported_attach_surface(imported, s);
        wl_surface_commit(s);
        wl_display_flush(w.display);
        bar_hold(7000);
        say("  %s done: window frame callbacks so far %d", tag, w.frames_done);
        wl_webos_imported_detach_surface(imported, s);
        wl_webos_imported_destroy(imported);
        wl_webos_exported_destroy(exported);
        wl_surface_attach(s, NULL, 0, 0);
        wl_surface_commit(s);
        wl_surface_destroy(s);
        w.test = NULL;
        wl_buffer_destroy(b);
        munmap(p, (size_t)pw * (size_t)ph * 4);
        bar_hold(1000);
    }
    alarm(0);
    say("  all foreign variants shown");
    return 0;
}

/* ---- A full-screen transparent overlay with an opaque box, by window type ---- */

static const struct variant overlays[] = {
    { "SYSTEM_UI overlay", "_WEBOS_WINDOW_TYPE_SYSTEM_UI", 0, 0, 0xffa020a0u },
    { "OVERLAY overlay", "_WEBOS_WINDOW_TYPE_OVERLAY", 0, 0, 0xff20a020u },
    { "FLOATING overlay", "_WEBOS_WINDOW_TYPE_FLOATING", 0, 0, 0xffd0a000u },
};

static int overlay_variants(void)
{
    size_t v;

    if (!make_window(1) || !bar_start())
        return 1;
    bar_hold(1000);
    for (v = 0; v < sizeof overlays / sizeof overlays[0]; v++) {
        const struct variant *var = &overlays[v];
        struct wl_surface *s;
        struct wl_shell_surface *ss;
        struct wl_webos_shell_surface *ws;
        struct wl_buffer *b;
        uint32_t *p;
        int x, y;
        char tag[80];

        snprintf(tag, sizeof tag, "overlay %zu", v + 1);
        step(tag);
        say("  %s: full-screen transparent %s with an opaque box at 300,200, colour 0x%06x",
            tag, var->type, var->colour & 0xffffffu);
        s = wl_compositor_create_surface(w.compositor);
        w.test = s;
        wl_surface_add_listener(s, &surface_listener, (void *)tag);
        ss = wl_shell_get_shell_surface(w.shell, s);
        wl_shell_surface_add_listener(ss, &shell_listener, (void *)tag);
        wl_shell_surface_set_toplevel(ss);
        ws = wl_webos_shell_get_shell_surface(w.webos_shell, s);
        wl_webos_shell_surface_add_listener(ws, &webos_listener, (void *)tag);
        wl_webos_shell_surface_set_property(ws, "appId", app_id());
        wl_webos_shell_surface_set_property(ws, "displayAffinity", "0");
        wl_webos_shell_surface_set_property(ws, "_WEBOS_WINDOW_TYPE", var->type);
        b = solid_buffer(w.width, w.height, 0x00000000u, &p);
        if (!b)
            return 1;
        for (y = 0; y < w.height; y++)
            for (x = 0; x < w.width; x++) {
                int in = x >= 300 && x < 780 && y >= 200 && y < 470;
                int edge = in && (x < 308 || y < 208 || x >= 772 || y >= 462);

                p[(size_t)y * w.width + x] = edge ? 0xffffffffu : in ? var->colour : 0;
            }
        wl_surface_attach(s, b, 0, 0);
        wl_surface_damage(s, 0, 0, w.width, w.height);
        wl_surface_commit(s);
        wl_display_flush(w.display);
        bar_hold(7000);
        say("  %s done: window frame callbacks so far %d", tag, w.frames_done);
        wl_surface_attach(s, NULL, 0, 0);
        wl_surface_commit(s);
        wl_webos_shell_surface_destroy(ws);
        wl_shell_surface_destroy(ss);
        wl_surface_destroy(s);
        w.test = NULL;
        wl_buffer_destroy(b);
        munmap(p, (size_t)w.width * (size_t)w.height * 4);
        bar_hold(1000);
    }
    alarm(0);
    say("  all overlay variants shown");
    return 0;
}

/* ---- A second surface of the app, under each webOS window type ---- */

static const struct variant variants[] = {
    { "POPUP", "_WEBOS_WINDOW_TYPE_POPUP", 0, 0, 0xffc02020u },
    { "OVERLAY", "_WEBOS_WINDOW_TYPE_OVERLAY", 0, 0, 0xff20a020u },
    { "FLOATING", "_WEBOS_WINDOW_TYPE_FLOATING", 0, 0, 0xffd0a000u },
    { "SYSTEM_UI", "_WEBOS_WINDOW_TYPE_SYSTEM_UI", 0, 0, 0xffa020a0u },
    { "POPUP, location hint north|west", "_WEBOS_WINDOW_TYPE_POPUP", 0,
      WL_WEBOS_SHELL_SURFACE_LOCATION_HINT_NORTH | WL_WEBOS_SHELL_SURFACE_LOCATION_HINT_WEST,
      0xffc06020u },
    { "transient of the window at 300,200, no type", NULL, 1, 0, 0xff20a0a0u },
    { "SUBSURFACE type, toplevel", "_WEBOS_WINDOW_TYPE_SUBSURFACE", 0, 0, 0xff8080ffu },
    { "no type (a second card?)", NULL, 0, 0, 0xffe0e020u },
};

/* The window keeps a bar moving so the viewer can tell whether it stays. */
static int popup_variants(void)
{
    struct wl_buffer *bufs[2];
    uint32_t *pix[2];
    int cur = 0, bar = 0;
    size_t v;
    double t;

    if (!make_window(1))
        return 1;
    step("window with a moving bar");
    for (v = 0; v < 2; v++) {
        bufs[v] = solid_buffer(w.width, w.height, 0xff203050u, &pix[v]);
        if (!bufs[v])
            return 1;
    }
    for (v = 0; v < sizeof variants / sizeof variants[0]; v++) {
        const struct variant *var = &variants[v];
        struct wl_surface *s;
        struct wl_shell_surface *ss;
        struct wl_webos_shell_surface *ws;
        struct wl_buffer *b;
        uint32_t *p;
        int pw = 480, ph = 270;
        char tag[80];

        snprintf(tag, sizeof tag, "variant %zu", v + 1);
        step(tag);
        say("  %s: type %s%s%s, %dx%d, colour 0x%06x", tag, var->type ? var->type : "none",
            var->transient ? ", wl_shell transient" : ", wl_shell toplevel",
            var->hint ? ", with a location hint" : "", pw, ph, var->colour & 0xffffffu);
        s = wl_compositor_create_surface(w.compositor);
        wl_surface_add_listener(s, &surface_listener, (void *)tag);
        ss = wl_shell_get_shell_surface(w.shell, s);
        wl_shell_surface_add_listener(ss, &shell_listener, (void *)tag);
        if (var->transient)
            wl_shell_surface_set_transient(ss, w.main, 300, 200, 0);
        else
            wl_shell_surface_set_toplevel(ss);
        ws = wl_webos_shell_get_shell_surface(w.webos_shell, s);
        wl_webos_shell_surface_add_listener(ws, &webos_listener, (void *)tag);
        wl_webos_shell_surface_set_property(ws, "appId", app_id());
        wl_webos_shell_surface_set_property(ws, "displayAffinity", "0");
        if (var->type)
            wl_webos_shell_surface_set_property(ws, "_WEBOS_WINDOW_TYPE", var->type);
        if (var->hint)
            wl_webos_shell_surface_set_location_hint(ws, var->hint);
        b = solid_buffer(pw, ph, var->colour, &p);
        if (!b)
            return 1;
        wl_surface_attach(s, b, 0, 0);
        wl_surface_damage(s, 0, 0, pw, ph);
        wl_surface_commit(s);
        wl_display_flush(w.display);
        /* Five seconds with the bar moving on the window. */
        t = now_ms();
        while (now_ms() - t < 5000) {
            int x, y;

            alarm(STEP_TIMEOUT);
            cur ^= 1;
            bar = (bar + 24) % (w.width - 48);
            for (y = 40; y < 80; y++)
                for (x = 8; x < w.width - 8; x++)
                    pix[cur][(size_t)y * w.width + x] =
                        x >= bar && x < bar + 48 ? 0xffffffffu : 0xff203050u;
            wl_surface_attach(w.main, bufs[cur], 0, 0);
            wl_surface_damage(w.main, 0, 40, w.width, 40);
            wl_callback_add_listener(wl_surface_frame(w.main), &frame_listener, NULL);
            wl_surface_commit(w.main);
            pump();
            usleep(100000);
        }
        say("  %s done: window frame callbacks so far %d", tag, w.frames_done);
        wl_surface_attach(s, NULL, 0, 0);
        wl_surface_commit(s);
        wl_webos_shell_surface_destroy(ws);
        wl_shell_surface_destroy(ss);
        wl_surface_destroy(s);
        wl_buffer_destroy(b);
        munmap(p, (size_t)pw * (size_t)ph * 4);
        wl_display_flush(w.display);
        t = now_ms();
        while (now_ms() - t < 1000) {
            pump();
            usleep(50000);
        }
    }
    alarm(0);
    say("  all variants shown; the window entered an output: %s", w.main_entered ? "yes" : "no");
    return 0;
}

static int child(const char *title, int (*fn)(int), int arg)
{
    pid_t pid;
    int status;

    say("");
    say("=== %s ===", title);
    pid = fork();
    if (pid == 0) {
        signal(SIGALRM, on_alarm);
        signal(SIGSEGV, on_crash);
        signal(SIGBUS, on_crash);
        signal(SIGABRT, on_crash);
        signal(SIGILL, on_crash);
        _exit(fn(arg));
    }
    if (pid < 0 || waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status)) {
        switch (WEXITSTATUS(status)) {
        case 0: say("RESULT: works"); break;
        case 3: say("RESULT: hangs"); break;
        case 4: say("RESULT: crashes"); break;
        default: say("RESULT: fails"); break;
        }
        return WEXITSTATUS(status);
    }
    say("RESULT: killed by signal %d", WTERMSIG(status));
    return -1;
}

static int popup_child(int unused)
{
    (void)unused;
    return popup_variants();
}

static int foreign_child(int unused)
{
    (void)unused;
    return foreign_variants();
}

static int overlay_child(int unused)
{
    (void)unused;
    return overlay_variants();
}

static const char *verdict(int r)
{
    return r == 0 ? "works" : r == 3 ? "hangs" : r == 4 ? "crashes" : "fails";
}

int main(void)
{
    struct utsname u;
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    int a = -2, b = -2, c = -2;

    if (n > 0) {
        char *slash;

        exe[n] = '\0';
        slash = strrchr(exe, '/');
        if (slash)
            *slash = '\0';
        snprintf(app_dir, sizeof app_dir, "%s", exe);
    }
    /* The launcher's environment. */
    setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
    setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    setenv("APPID", "com.github.gprot42.geckotv", 0);

    uname(&u);
    say("GPU probe for Firefox on webOS (%s %s %s)", u.sysname, u.release, u.machine);
    if (!getenv("GPUPROBE_SKIP_GL")) {
        a = child("GL on the window, EGL legacy display (eglGetDisplay)", run_variant, 0);
        b = child("GL on the window, EGL Wayland platform (eglGetPlatformDisplay)", run_variant, 1);
    }
    if (getenv("GPUPROBE_FOREIGN"))
        return child("a surface attached into an exported region of the window", foreign_child, 0);
    if (getenv("GPUPROBE_OVERLAY"))
        return child("a full-screen transparent overlay by window type", overlay_child, 0);
    if (!getenv("GPUPROBE_SKIP_POPUPS"))
        c = child("a second surface under each webOS window type", popup_child, 0);
    say("");
    say("SUMMARY: GL legacy display %s, GL platform display %s, popup variants %s",
        a == -2 ? "skipped" : verdict(a), b == -2 ? "skipped" : verdict(b),
        c == -2 ? "skipped" : verdict(c));
    return (a == 0 || b == 0 || a == -2) && (c == 0 || c == -2) ? 0 : 1;
}
