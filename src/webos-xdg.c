#define _GNU_SOURCE

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-util.h>

#include "xdg-shell-client-protocol.h"
#include "wayland-webos-shell-client-protocol.h"
#include "webos-input-manager-client-protocol.h"
#include "text-model-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "webos-surface-group-client-protocol.h"
#include "webos-foreign-client-protocol.h"

/* Compiled into libwayland-client. Requests are intercepted after
 * libwayland has parsed the argument list, then translated onto the
 * wl_shell and wl_webos_shell globals this compositor actually exports.
 */

#define MAX_VIRT 96

extern struct wl_proxy *webos_xdg_create_virtual(struct wl_proxy *factory,
                                                  const struct wl_interface *interface,
                                                  uint32_t version);
extern void webos_xdg_destroy_virtual(struct wl_proxy *proxy);
void webos_xdg_virtual_destroyed(struct wl_proxy *proxy);
void webos_xdg_null_queue_report(struct wl_proxy *proxy, void *caller);
struct wl_display *webos_xdg_proxy_display(struct wl_proxy *proxy);
int webos_xdg_on_default_queue(struct wl_proxy *proxy);

struct peek {
    const struct wl_interface *interface;
};

enum virt_kind {
    V_WM = 1,
    V_POSITIONER,
    V_SURFACE,
    V_TOPLEVEL,
    V_POPUP,
    V_TEXT_INPUT_MANAGER,
    V_TEXT_INPUT,
    /* Stand-ins for compositors without these globals (webOS 4). */
    V_DATA_DEVICE_MANAGER,
    V_DATA_SOURCE,
    V_DATA_DEVICE,
    V_SUBCOMPOSITOR,
    V_SUBSURFACE,
    /* A wl_callback for wl_surface.frame that the adapter answers (flat mode). */
    V_FRAME
};

struct virt {
    struct wl_proxy *proxy;
    enum virt_kind kind;
    const void *listener;
    void *data;
    struct wl_proxy *wl_surface;
    struct wl_shell_surface *shell_surface;
    struct wl_webos_shell_surface *webos_surface;
    struct wl_subsurface *subsurface;
    uint32_t version;
    int injected;
    int injected_ddm;
    int injected_sub;
    int configured;
    /* For V_TOPLEVEL, set between set_fullscreen and unset_fullscreen. */
    int fullscreen;
    /* V_SUBSURFACE: the surfaces it joins, as Firefox asked. */
    struct wl_proxy *sub_child;
    struct wl_proxy *sub_parent;
    /* xdg_positioner state, and the resolved popup geometry. */
    int32_t p_w, p_h;
    int32_t p_ax, p_ay, p_aw, p_ah;
    uint32_t p_anchor, p_gravity;
    int32_t p_ox, p_oy;
    int32_t popup_x, popup_y;
    int32_t abs_x, abs_y;
};

static struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_shell *shell;
    struct wl_webos_shell *webos;
    struct wl_webos_input_manager *input;
    uint32_t xdg_name;
    struct virt virts[MAX_VIRT];
    uint32_t serial;
} g;

static uint32_t host_shell_name;
static uint32_t host_webos_name;
static uint32_t host_input_name;
static uint32_t host_text_name;
static uint32_t host_seat_name;
static uint32_t host_seat_version;
/* Every wl_seat the compositor currently offers. webOS 4 TVs offer several
 * and can add and remove them while Firefox runs (a new seat appeared just
 * before a crash on clicking the address bar), so the seat the adapter binds
 * for the webOS keyboard must follow removals. */
#define MAX_SEATS 8
static struct { uint32_t name, version; } seats[MAX_SEATS];
static uint32_t host_shm_name;
static uint32_t host_compositor_name;
static uint32_t host_subcompositor_name;
static uint32_t host_data_device_manager_name;
static uint32_t host_group_name;
/* webOS 5 and later offer wl_webos_foreign; webOS 4 does not (see flat_mode). */
static uint32_t host_foreign_name;
/* webOS 4 has no wl_subcompositor: Firefox's content surface is shown as a
 * layer of a webOS surface group rooted at the main window instead. */
static struct wl_webos_surface_group_compositor *group_compositor;
static struct wl_webos_surface_group *main_group;
static struct wl_surface *group_root;
static int groups_made;
/* One slot per surface shown through the group: the surface and whether it
 * is attached.
 * Slot i uses layer "content<i+1>". Layers and groups are never destroyed:
 * webOS 4's versions of these interfaces have fewer requests than
 * webos-surface-group.xml, so destroy has another opcode there ("invalid
 * method 2", seen in LG's webOS TV 4.0 emulator). Only create_surface_group,
 * create_layer, attach and detach are used, which were tested there. */
#define MAX_LAYERED 8
static struct {
    struct wl_proxy *surface;
    int attached;
    int committed;       /* has drawn: attach decisions wait for this */
    int32_t x, y;        /* wl_subsurface.set_position */
} layered[MAX_LAYERED];
/* webOS 4 does not free a layer when its surface is detached (attaching to it
 * again is a fatal "Layer already attached"), so every attach gets a new
 * layer with a new name and z-index. */
static int layers_made;
/* webOS 4: wl_shell roles for surfaces shown without subsurfaces (group
 * layers, pop-ups). A surface keeps its role while it lives, since a second
 * get_shell_surface is a protocol error, so roles are cached here and
 * dropped when the surface is destroyed. */
#define MAX_SHELLED 24
static struct {
    struct wl_proxy *surface;
    struct wl_shell_surface *shell;
} shelled[MAX_SHELLED];
static struct wl_webos_surface_group_layer *layers[MAX_LAYERED];
/* webOS 4 pop-up overlay (see ov_redraw). */
static struct wl_surface *ov_surface;
#define DATA_DEVICE_MANAGER_NAME 0x7f000003
#define SUBCOMPOSITOR_NAME 0x7f000004
static struct wl_subcompositor *our_subcompositor;
/* Surfaces GDK names in wl_pointer.set_cursor. LSM maps any surface that
 * gets content as a card (FullscreenView.checkFullscreen checks neither
 * role nor appId) and minimizes the browser window for it, so these
 * surfaces must never carry content. */
#define MAX_CURSOR 8
static struct wl_proxy *cursor_surfaces[MAX_CURSOR];

/* LSM makes a WebOSSurfaceItem for every wl_surface that gets content, and
 * FullscreenView fullscreens any of them whose type is still the default
 * _WEBOS_WINDOW_TYPE_CARD, which minimizes the browser. Subsurfaces are
 * ours and Firefox's own (MozContainer), and none of them should be a card,
 * so give each one a type no window model claims. The surface keeps being
 * composited inside its parent. */
#define MAX_TAGGED 32
static struct wl_proxy *tagged_surfaces[MAX_TAGGED];

#define MAX_HOOK 24

struct hook {
    struct wl_proxy *proxy;
    const void *listener;
    void *data;
    /* wl_keyboard only. A leave of the main window is held back from GTK
     * while the webOS keyboard is open and passed on when it closes. */
    int held_leave;
    uint32_t held_serial;
};

static struct hook pointer_hooks[MAX_HOOK];
static struct hook keyboard_hooks[MAX_HOOK];
static struct hook *focused_keys;
/* GDK's wl_keyboard, the one whose key handler turns keys into typed text. */
static struct hook *gdk_keys;
/* The surface our synthetic pointer events are currently inside. GDK drops
 * motion and button unless the seat has a focus, and that focus only moves
 * on enter/leave, so we have to send those ourselves. */
static struct wl_surface *synth_focus;
/* The compositor does deliver real pointer enter/motion/button to our
 * surfaces, including subsurfaces, with correct surface-local coordinates.
 * Injecting our own from evdev on top of that delivered every click twice
 * and dragged the seat focus onto the wrong surface, which is why menu
 * items never activated. Inject only while no real pointer is arriving. */
static struct timespec last_real_pointer;

static int real_pointer_active(void)
{
    struct timespec now;
    long ms;

    if (last_real_pointer.tv_sec == 0)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (now.tv_sec - last_real_pointer.tv_sec) * 1000
       + (now.tv_nsec - last_real_pointer.tv_nsec) / 1000000;
    return ms < 10000;
}

static void note_real_pointer(void)
{
    clock_gettime(CLOCK_MONOTONIC, &last_real_pointer);
}
static struct wl_seat *our_seat;
static struct wl_surface *main_surface;
static struct text_model *text_input;
static int saw_motion;
static int last_x;
static int last_y;
static int keyboard_up;

#define PTR_QUEUE 64
struct ptr_ev {
    int button;
    int down;
    int x;
    int y;
};
static struct ptr_ev ptr_q[PTR_QUEUE];
static int ptr_q_head;
static int ptr_q_tail;
static pthread_mutex_t ptr_mu = PTHREAD_MUTEX_INITIALIZER;
static struct wl_display *wake_display;
static int wake_pending_flag;

static void show_keyboard(void);
static int hide_keyboard(void);
static void keyboard_closed(void);
static void text_input_enter_main(void);
static void ensure_text_model(void);
static void start_remote_pointer(void);
static uint32_t server_compositor_version;
static uint32_t server_output_version;
static int emitting;
/* Size the browser is told it has. LSM scales a fullscreen surface to the
 * panel, so a 1280x720 window costs 2.25x less to rasterise, composite
 * and copy through shared memory than 1920x1080, at the price of a softer
 * picture. Measured on YouTube 720p: dropped frames fell from ~25% to
 * under 1% and the parent process from ~170% to ~80% CPU. Override with
 * WEBOS_XDG_SIZE=WxH in the launcher's env file, e.g. 1920x1080. */
static int win_w = 1280;
static int win_h = 720;
/* The main window's size as GTK declares it (xdg_surface.set_window_geometry).
 * A fullscreen Firefox takes the output's size, which can differ from
 * win_w x win_h (1920x1080 in LG's webOS 4 emulator), so pop-ups are kept
 * inside this when known. */
static int geo_w, geo_h;

static void read_window_size(void)
{
    const char *s = getenv("WEBOS_XDG_SIZE");
    int w, h;

    if (s && sscanf(s, "%dx%d", &w, &h) == 2 && w >= 640 && h >= 360 && w <= 3840 && h <= 2160) {
        win_w = w;
        win_h = h;
    }
}
static volatile int mapped_one;

/* GECKOTV_LOGLEVEL is set by the build (LOGLEVEL in the Makefile and
 * build/build-adapter.sh). Level 0 logs decisions, state changes and
 * errors. Level 1 adds the per-event trace of every listener, key, pointer
 * and focus event, several hundred lines per start. */
#ifndef GECKOTV_LOGLEVEL
#define GECKOTV_LOGLEVEL 0
#endif
#define log_trace(...) \
    do { \
        if (GECKOTV_LOGLEVEL >= 1) \
            log_msg(__VA_ARGS__); \
    } while (0)

__attribute__((format(printf, 1, 2)))
static void log_msg(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    struct tm tm;

    /* UTC, so lines can be lined up against /var/log/messages. */
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    fprintf(stderr, "%02d:%02d:%02d.%03ld webos-xdg: ",
            tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* Reads our own memory without faulting: fails with EFAULT on unmapped or
 * reserved pages. Called through syscall() because glibc only added the
 * process_vm_readv() wrapper in 2.15 and older webOS has 2.12. */
static ssize_t read_own_memory(void *dst, const void *src, size_t n)
{
    struct iovec local = { dst, n };
    struct iovec remote = { (void *)src, n };

    return syscall(SYS_process_vm_readv, getpid(), &local, 1UL, &remote, 1UL, 0UL);
}

/* Executable mappings, read once in the crash handler without stdio or
 * malloc (the fault may be inside malloc). */
static char crash_maps[192 * 1024];
static struct { unsigned long lo, hi, pgoff; const char *name; } crash_exec[512];
/* Each mapped file's first mapping (see crash_load_maps). */
static struct { const char *name; unsigned long lo; } crash_base[64];
static int crash_nbase;
static int crash_nexec;
static unsigned long crash_stack_hi;

static void crash_load_maps(unsigned long sp)
{
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    size_t len = 0;
    ssize_t r;
    char *p, *end;

    if (fd < 0)
        return;
    while (len < sizeof crash_maps - 1 &&
           (r = read(fd, crash_maps + len, sizeof crash_maps - 1 - len)) > 0)
        len += r;
    close(fd);
    crash_maps[len] = '\0';
    for (p = crash_maps; *p && crash_nexec < 512; p = end + 1) {
        unsigned long lo, hi, pgoff;
        char *q, *slash;

        end = strchr(p, '\n');
        if (!end)
            break;
        *end = '\0';
        lo = strtoul(p, &q, 16);
        hi = strtoul(q + 1, &q, 16);
        if (sp >= lo && sp < hi)
            crash_stack_hi = hi;
        pgoff = strtoul(q + 6, &q, 16);
        slash = strrchr(q, '/');
        /* The file's first mapping (offset 0) is where its address 0 is:
         * offsets printed from it match nm and addr2line. A file offset
         * does not: lld puts code 64 KB higher in memory than in the file
         * (the firefox binary), GNU ld does not (GTK's libraries). */
        if (pgoff == 0 && slash && crash_nbase < 64) {
            int k;

            for (k = 0; k < crash_nbase && strcmp(crash_base[k].name, slash + 1); k++)
                ;
            if (k == crash_nbase) {
                crash_base[k].name = slash + 1;
                crash_base[k].lo = lo;
                crash_nbase++;
            }
        }
        if (q[3] != 'x')                       /* " r-xp" */
            continue;
        crash_exec[crash_nexec].lo = lo;
        crash_exec[crash_nexec].hi = hi;
        crash_exec[crash_nexec].pgoff = pgoff;
        crash_exec[crash_nexec].name = slash ? slash + 1 : "[anon]";
        crash_nexec++;
    }
}

static unsigned long crash_module_offset(int i, unsigned long a)
{
    int k;

    for (k = 0; k < crash_nbase; k++)
        if (!strcmp(crash_base[k].name, crash_exec[i].name) && crash_base[k].lo <= a)
            return a - crash_base[k].lo;
    return a - crash_exec[i].lo + crash_exec[i].pgoff;
}

static int crash_in_code(unsigned long a)
{
    int i;

    for (i = 0; i < crash_nexec; i++)
        if (a >= crash_exec[i].lo && a < crash_exec[i].hi)
            return 1;
    return 0;
}

static int crash_in_libc(unsigned long a)
{
    int i;

    for (i = 0; i < crash_nexec; i++)
        if (a >= crash_exec[i].lo && a < crash_exec[i].hi)
            return !strcmp(crash_exec[i].name, "libc.so.6");
    return 0;
}

/* Prints "what 0x... lib+0xoff"; returns 1 if the address is in code. */
static int crash_resolve(const char *what, unsigned long a)
{
    int i;

    for (i = 0; i < crash_nexec; i++)
        if (a >= crash_exec[i].lo && a < crash_exec[i].hi) {
            dprintf(STDERR_FILENO, "webos-xdg:   %s %#lx %s+%#lx\n", what, a,
                    crash_exec[i].name, crash_module_offset(i, a));
            return 1;
        }
    dprintf(STDERR_FILENO, "webos-xdg:   %s %#lx\n", what, a);
    return 0;
}

/* From experiments/printf-trace when it is preloaded; NULL otherwise. */
struct printf_trace {
    void *caller;
    const char *fmt;
    const char *fn;
};
extern struct printf_trace *webos_printf_trace_get(void) __attribute__((weak));

static void on_crash(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = ctx;
    unsigned long pc = 0, lr = 0, sp = 0;
    int i, found = 0;
    struct printf_trace last = { 0 };

    /* Copy before our own dprintf calls overwrite it. */
    if (webos_printf_trace_get)
        last = *webos_printf_trace_get();

    dprintf(STDERR_FILENO, "\nwebos-xdg: signal %d addr=%p thread %ld\n", sig,
            info ? info->si_addr : NULL, (long)syscall(SYS_gettid));
#if defined(__arm__)
    pc = uc->uc_mcontext.arm_pc;
    lr = uc->uc_mcontext.arm_lr;
    sp = uc->uc_mcontext.arm_sp;
#elif defined(__aarch64__)
    pc = uc->uc_mcontext.pc;
    lr = uc->uc_mcontext.regs[30];
    sp = uc->uc_mcontext.sp;
#endif
    crash_load_maps(sp);
    crash_resolve("pc", pc);
    crash_resolve("lr", lr);
    if (webos_printf_trace_get) {
        struct printf_trace *t = &last;
        char fmt[160] = "";
        size_t len = sizeof fmt - 1;

        /* The format may sit at the end of a mapping: fall back to less. */
        while (t->fmt && len > 8 && read_own_memory(fmt, t->fmt, len) < 0)
            len /= 2;
        fmt[t->fmt ? len : 0] = '\0';
        for (i = 0; fmt[i]; i++)
            if (fmt[i] == '\n')
                fmt[i] = '|';
        dprintf(STDERR_FILENO, "webos-xdg:   last %s format \"%s\"\n", t->fn ? t->fn : "?", fmt);
        crash_resolve("called from", (unsigned long)t->caller);
    }
    /* No unwind tables to trust here, so list stack words that point into
     * code outside libc: the real return addresses are among them, in call
     * order, mixed with stale values. */
    for (i = 0; sp && i < 16384 && found < 32; i++) {
        unsigned long *w = (unsigned long *)sp + i;

        if ((unsigned long)(w + 1) > crash_stack_hi)
            break;
        if (crash_in_code(*w) && !crash_in_libc(*w))
            found += crash_resolve("stack", *w);
    }
    _exit(128 + sig);
}

__attribute__((constructor)) static void webos_xdg_init(void)
{
    struct sigaction sa;

    setvbuf(stderr, NULL, _IONBF, 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    read_window_size();
    log_msg("ready, window %dx%d", win_w, win_h);
    start_remote_pointer();
}

static const struct wl_interface *proxy_iface(struct wl_proxy *proxy)
{
    return ((struct peek *)proxy)->interface;
}

static struct virt *virt_get(struct wl_proxy *proxy)
{
    int i;
    if (!proxy)
        return NULL;
    for (i = 0; i < MAX_VIRT; i++) {
        if (g.virts[i].proxy == proxy)
            return &g.virts[i];
    }
    return NULL;
}

static struct virt *virt_add(struct wl_proxy *proxy, enum virt_kind kind)
{
    int i;
    for (i = 0; i < MAX_VIRT; i++) {
        if (!g.virts[i].proxy) {
            memset(&g.virts[i], 0, sizeof g.virts[i]);
            g.virts[i].proxy = proxy;
            g.virts[i].kind = kind;
            return &g.virts[i];
        }
    }
    log_msg("virt table full");
    return NULL;
}

static struct wl_proxy *new_virt(struct wl_proxy *factory, const struct wl_interface *iface,
                                 uint32_t version, enum virt_kind kind, struct virt **out)
{
    struct wl_proxy *created = webos_xdg_create_virtual(factory, iface, version);
    struct virt *child;

    if (out)
        *out = NULL;
    if (!created) {
        log_msg("virtual proxy failed");
        return NULL;
    }
    if (!version)
        version = 1;
    child = virt_add(created, kind);
    if (child)
        child->version = version;
    if (out)
        *out = child;
    return created;
}

static void shell_ping(void *data, struct wl_shell_surface *surface, uint32_t serial)
{
    (void)data;
    wl_shell_surface_pong(surface, serial);
}

static void shell_ignore_configure(void *data, struct wl_shell_surface *surface,
                                   uint32_t edges, int32_t width, int32_t height)
{
    (void)data;
    (void)surface;
    (void)edges;
    (void)width;
    (void)height;
}

static void shell_popup_done(void *data, struct wl_shell_surface *surface)
{
    (void)data;
    (void)surface;
}

static const struct wl_shell_surface_listener shell_listener = {
    shell_ping,
    shell_ignore_configure,
    shell_popup_done,
};

static void bind_named(void)
{
    if (!g.registry)
        return;
    if (!g.shell && host_shell_name) {
        g.shell = wl_registry_bind(g.registry, host_shell_name, &wl_shell_interface, 1);
        log_msg("shell bind %p", (void *)g.shell);
    }
    if (!g.webos && host_webos_name) {
        g.webos = wl_registry_bind(g.registry, host_webos_name, &wl_webos_shell_interface, 1);
        log_msg("webos bind %p", (void *)g.webos);
    }
    if (!g.input && host_input_name) {
        g.input = wl_registry_bind(g.registry, host_input_name,
                                   &wl_webos_input_manager_interface, 1);
        log_msg("input manager %p", (void *)g.input);
    }
    if (!our_subcompositor && host_subcompositor_name) {
        our_subcompositor = wl_registry_bind(g.registry, host_subcompositor_name,
                                             &wl_subcompositor_interface, 1);
        log_msg("subcompositor %p", (void *)our_subcompositor);
    }
}

/* xdg_positioner anchor/gravity enums share this numbering. */
enum { A_NONE = 0, A_TOP, A_BOTTOM, A_LEFT, A_RIGHT,
       A_TOP_LEFT, A_BOTTOM_LEFT, A_TOP_RIGHT, A_BOTTOM_RIGHT };

static void anchor_point(const struct virt *pos, int32_t *out_x, int32_t *out_y)
{
    int32_t x = pos->p_ax;
    int32_t y = pos->p_ay;

    switch (pos->p_anchor) {
    case A_TOP:          x += pos->p_aw / 2; break;
    case A_BOTTOM:       x += pos->p_aw / 2; y += pos->p_ah; break;
    case A_LEFT:         y += pos->p_ah / 2; break;
    case A_RIGHT:        x += pos->p_aw;     y += pos->p_ah / 2; break;
    case A_TOP_LEFT:     break;
    case A_BOTTOM_LEFT:  y += pos->p_ah; break;
    case A_TOP_RIGHT:    x += pos->p_aw; break;
    case A_BOTTOM_RIGHT: x += pos->p_aw;     y += pos->p_ah; break;
    default:             x += pos->p_aw / 2; y += pos->p_ah / 2; break;
    }
    *out_x = x;
    *out_y = y;
}

/* Resolve the positioner into a parent-relative x,y for the subsurface. */
static void place_popup(struct virt *popup, const struct virt *pos)
{
    int32_t ax, ay, x, y;
    int32_t w = pos->p_w > 0 ? pos->p_w : 1;
    int32_t h = pos->p_h > 0 ? pos->p_h : 1;

    anchor_point(pos, &ax, &ay);
    switch (pos->p_gravity) {
    case A_TOP:          x = ax - w / 2; y = ay - h;     break;
    case A_BOTTOM:       x = ax - w / 2; y = ay;         break;
    case A_LEFT:         x = ax - w;     y = ay - h / 2; break;
    case A_RIGHT:        x = ax;         y = ay - h / 2; break;
    case A_TOP_LEFT:     x = ax - w;     y = ay - h;     break;
    case A_BOTTOM_LEFT:  x = ax - w;     y = ay;         break;
    case A_TOP_RIGHT:    x = ax;         y = ay - h;     break;
    case A_BOTTOM_RIGHT: x = ax;         y = ay;         break;
    default:             x = ax - w / 2; y = ay - h / 2; break;
    }
    x += pos->p_ox;
    y += pos->p_oy;
    /* Stand in for constraint_adjustment: keep it on screen. */
    if (x + w > (geo_w > 0 ? geo_w : win_w))
        x = (geo_w > 0 ? geo_w : win_w) - w;
    if (y + h > (geo_h > 0 ? geo_h : win_h))
        y = (geo_h > 0 ? geo_h : win_h) - h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    popup->popup_x = x;
    popup->popup_y = y;
    popup->p_w = w;
    popup->p_h = h;
}

/* A popup surface with no role would be mapped by LSM as a nameless card,
 * which minimizes the browser. Make it a subsurface of the parent instead:
 * the compositor composites it inside our window, so it takes no card. */
static void emit_popup_configure(struct virt *popup)
{
    struct xdg_popup_listener *pl;
    struct virt *surface = NULL;
    struct xdg_surface_listener *sl;
    int i;

    if (!popup || !popup->listener || popup->configured || emitting)
        return;
    emitting = 1;
    for (i = 0; i < MAX_VIRT; i++) {
        if (g.virts[i].proxy && g.virts[i].kind == V_SURFACE &&
            g.virts[i].wl_surface == popup->wl_surface) {
            surface = &g.virts[i];
            break;
        }
    }
    pl = (struct xdg_popup_listener *)popup->listener;
    if (pl->configure) {
        pl->configure(popup->data, (struct xdg_popup *)popup->proxy,
                      popup->popup_x, popup->popup_y, popup->p_w, popup->p_h);
    }
    if (surface && surface->listener) {
        sl = (struct xdg_surface_listener *)surface->listener;
        if (sl->configure)
            sl->configure(surface->data, (struct xdg_surface *)surface->proxy, ++g.serial);
    }
    popup->configured = 1;
    emitting = 0;
    log_trace("popup configure %d,%d %dx%d", popup->popup_x, popup->popup_y,
            popup->p_w, popup->p_h);
}

static const char *our_app_id(void)
{
    const char *appid = getenv("APPID");

    if (!appid || !appid[0])
        appid = "com.github.gprot42.geckotv";
    return appid;
}

static void tag_surface_as(struct wl_proxy *surface, const char *type);

static void tag_child_surface(struct wl_proxy *surface)
{
    tag_surface_as(surface, "_WEBOS_WINDOW_TYPE_SUBSURFACE");
}

static void tag_surface_as(struct wl_proxy *surface, const char *type)
{
    struct wl_webos_shell_surface *ss;
    int i;
    int slot = -1;

    if (!surface)
        return;
    for (i = 0; i < MAX_TAGGED; i++) {
        if (tagged_surfaces[i] == surface)
            return;
        if (!tagged_surfaces[i] && slot < 0)
            slot = i;
    }
    if (slot < 0) {
        log_msg("tag table full");
        return;
    }
    bind_named();
    if (!g.webos)
        return;
    ss = wl_webos_shell_get_shell_surface(g.webos, (struct wl_surface *)surface);
    if (!ss)
        return;
    tagged_surfaces[slot] = surface;
    /* An appId keeps it out of the nameless-card path, and the type keeps
     * it out of FullscreenView. No model claims this type. */
    wl_webos_shell_surface_set_property(ss, "appId", our_app_id());
    wl_webos_shell_surface_set_property(ss, "_WEBOS_WINDOW_TYPE", type);
    wl_webos_shell_surface_set_property(ss, "displayAffinity", "0");
    log_trace("tagged child surface %p as %s", (void *)surface, type);
}

static void emit_toplevel_configure(struct virt *toplevel)
{
    struct virt *surface = NULL;
    struct xdg_toplevel_listener *tl;
    struct xdg_surface_listener *sl;
    struct wl_array states;
    uint32_t *state;
    int i;

    if (!toplevel || !toplevel->listener || emitting)
        return;
    emitting = 1;
    for (i = 0; i < MAX_VIRT; i++) {
        if (g.virts[i].proxy && g.virts[i].kind == V_SURFACE &&
            g.virts[i].wl_surface == toplevel->wl_surface) {
            surface = &g.virts[i];
            break;
        }
    }
    g.serial++;
    /* These states do not change what LSM shows, which is always the full
     * card. FULLSCREEN is needed because GDK turns it into the window-state
     * event Firefox waits for before requestFullscreen() completes. */
    wl_array_init(&states);
    state = wl_array_add(&states, sizeof *state);
    if (state)
        *state = toplevel->fullscreen ? XDG_TOPLEVEL_STATE_FULLSCREEN
                                      : XDG_TOPLEVEL_STATE_MAXIMIZED;
    state = wl_array_add(&states, sizeof *state);
    if (state)
        *state = XDG_TOPLEVEL_STATE_ACTIVATED;
    tl = (struct xdg_toplevel_listener *)toplevel->listener;
    if (tl->configure) {
        tl->configure(toplevel->data, (struct xdg_toplevel *)toplevel->proxy,
                      win_w, win_h, &states);
    }
    wl_array_release(&states);
    if (surface && surface->listener) {
        sl = (struct xdg_surface_listener *)surface->listener;
        if (sl->configure)
            sl->configure(surface->data, (struct xdg_surface *)surface->proxy, g.serial);
    }
    toplevel->configured = 1;
    emitting = 0;
    log_trace("configure serial %u fullscreen=%d", g.serial, toplevel->fullscreen);
}

static int flat_mode(void);
static void flat_pump_start(void);
static struct wl_proxy *flat_window_for(struct wl_proxy *gtk_surface);
static void flat_window_shown(struct wl_proxy *gtk_surface, struct wl_webos_shell_surface *ws);
static int gl_mode(void);
static void gl_main_mapped(struct virt *xdg_surface, struct wl_proxy *gtk_surface);
static void gl_window_set(struct wl_proxy *child);
static void gl_surface_destroyed(struct wl_proxy *surface);
static struct wl_proxy *gl_window;  /* in GL window mode Firefox's content surface, the card */
static int gl_window_shown;
static int gl_window_commit(struct wl_proxy *surface, int attached);
static struct wl_proxy *gl_main_surface(void);
static void gl_pending_add(struct wl_proxy *child, struct wl_proxy *parent);
static struct wl_proxy *gp_input_popup(struct wl_proxy *surface, int32_t *off_x, int32_t *off_y);
static int gp_reparent(struct wl_proxy *child, struct wl_proxy *popup);
static int gp_offset(struct wl_proxy *child, int32_t x, int32_t y);
static void gp_popup_moved(struct wl_proxy *popup);
static void gp_popup_gone(struct wl_proxy *popup);

static void map_main_surface(struct virt *xdg_surface, struct wl_proxy *wl_surface)
{
    const char *appid;
    int fullscreen;
    struct wl_proxy *target;

    if (!xdg_surface || xdg_surface->shell_surface)
        return;
    bind_named();
    xdg_surface->wl_surface = wl_surface;
    if (!g.shell || !wl_surface) {
        log_msg("cannot map shell=%p surface=%p", (void *)g.shell, (void *)wl_surface);
        return;
    }
    if (gl_mode() && !mapped_one) {
        gl_main_mapped(xdg_surface, wl_surface);
        return;
    }
    /* In flat mode the window the compositor sees is the overlay surface. */
    target = !mapped_one ? flat_window_for(wl_surface) : NULL;
    if (!target)
        target = wl_surface;
    xdg_surface->shell_surface = wl_shell_get_shell_surface(g.shell, (struct wl_surface *)target);
    if (!xdg_surface->shell_surface) {
        log_msg("wl_shell_get_shell_surface failed");
        return;
    }
    wl_shell_surface_add_listener(xdg_surface->shell_surface, &shell_listener, NULL);
    wl_shell_surface_set_toplevel(xdg_surface->shell_surface);
    fullscreen = !mapped_one;
    main_surface = (struct wl_surface *)wl_surface;
    mapped_one = 1;
    text_input_enter_main();
    if (g.webos) {
        xdg_surface->webos_surface = wl_webos_shell_get_shell_surface(
            g.webos, (struct wl_surface *)target);
        appid = our_app_id();
        if (xdg_surface->webos_surface) {
            wl_webos_shell_surface_set_property(xdg_surface->webos_surface, "appId", appid);
            wl_webos_shell_surface_set_property(xdg_surface->webos_surface, "title", "Firefox");
            wl_webos_shell_surface_set_property(xdg_surface->webos_surface, "displayAffinity", "0");
            wl_webos_shell_surface_set_property(xdg_surface->webos_surface,
                                                "_WEBOS_ACCESS_POLICY_KEYS_BACK", "true");
            /* Back is omitted from the default mask, so a wheel-down becomes
             * the TV's exit arrow. Claim Back for this window. */
            wl_webos_shell_surface_set_key_mask(xdg_surface->webos_surface, 0xFFFFFFFAu);
            /* -1 keeps the Magic Remote pointer on screen. */
            wl_webos_shell_surface_set_property(xdg_surface->webos_surface,
                                                "_WEBOS_CURSOR_SLEEP_TIME", "-1");
            if (fullscreen) {
                wl_webos_shell_surface_set_state(xdg_surface->webos_surface,
                                                 WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);
            }
        }
    }
    log_msg("mapped shell=%p webos=%p fullscreen=%d", (void *)xdg_surface->shell_surface,
            (void *)xdg_surface->webos_surface, fullscreen);
    if (target != wl_surface)
        flat_window_shown(wl_surface, xdg_surface->webos_surface);
}

/* Undo what the adapter created for a virtual object and free its slot. */
static void virt_teardown(struct virt *v)
{
    /* The synthetic pointer focus must never outlive its surface. A leave
     * sent for a destroyed surface is a use after free in GDK. */
    if (v->wl_surface && synth_focus == (struct wl_surface *)v->wl_surface)
        synth_focus = NULL;
    if (v->subsurface) {
        wl_subsurface_destroy(v->subsurface);
        v->subsurface = NULL;
    }
    if (v->shell_surface) {
        wl_shell_surface_destroy(v->shell_surface);
        v->shell_surface = NULL;
    }
    v->proxy = NULL;
}

/* text-input-v3, offered to GTK by the adapter. GTK enables text input when
 * an editable element gains focus and disables it when focus leaves, which is
 * exactly when the webOS keyboard should come and go; the webOS compositor
 * does not offer the protocol itself. The keyboard only opens for a focus
 * that follows a click, so address-bar focus on a new tab or a page's
 * autofocus does not pop it up. Text still arrives through text_model and
 * inject_text(). */
#define TEXT_INPUT_NAME 0x7f000002
static struct wl_proxy *text_input_v3;
static int text_input_bound;
static int text_input_pending, text_input_enabled;
static uint32_t text_input_commits;
static int text_input_entered;
static struct timespec last_click;

static void note_click(void)
{
    clock_gettime(CLOCK_MONOTONIC, &last_click);
}

static int clicked_recently(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - last_click.tv_sec) * 1000 +
           (now.tv_nsec - last_click.tv_nsec) / 1000000 < 1500;
}

/* When the Back key read from evdev closed the webOS keyboard. The same
 * press may also arrive through wl_keyboard and must do nothing more. */
static struct timespec back_hid_keyboard;

static void note_back_hid_keyboard(void)
{
    clock_gettime(CLOCK_MONOTONIC, &back_hid_keyboard);
}

static int back_hid_keyboard_recently(void)
{
    struct timespec now;

    if (back_hid_keyboard.tv_sec == 0)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - back_hid_keyboard.tv_sec) * 1000 +
           (now.tv_nsec - back_hid_keyboard.tv_nsec) / 1000000 < 500;
}

/* GTK only enables text input on a surface it has been told it entered. */
static void text_input_enter_main(void)
{
    struct virt *v = text_input_v3 ? virt_get(text_input_v3) : NULL;
    const struct zwp_text_input_v3_listener *l;

    if (!v || !v->listener || !main_surface || text_input_entered)
        return;
    l = v->listener;
    text_input_entered = 1;
    if (l->enter)
        l->enter(v->data, (struct zwp_text_input_v3 *)v->proxy, main_surface);
    log_msg("text-input enter main surface");
}

static void text_input_commit(struct virt *v)
{
    const struct zwp_text_input_v3_listener *l = v->listener;

    text_input_commits++;
    if (text_input_pending != text_input_enabled) {
        text_input_enabled = text_input_pending;
        log_trace("text-input %s", text_input_enabled ? "enabled" : "disabled");
        if (!text_input_enabled)
            hide_keyboard();
        else if (clicked_recently())
            show_keyboard();
    } else if (text_input_enabled && !keyboard_up && clicked_recently()) {
        /* A click on the field that already has focus (after Back closed
         * the keyboard) only moves the cursor: reopen the keyboard. */
        show_keyboard();
    }
    if (l && l->done)
        l->done(v->data, (struct zwp_text_input_v3 *)v->proxy, text_input_commits);
}

static int shelled_slot(struct wl_proxy *surface)
{
    int i;

    for (i = 0; surface && i < MAX_SHELLED; i++)
        if (shelled[i].surface == surface)
            return i;
    return -1;
}

static struct wl_shell_surface *shell_role(struct wl_proxy *surface)
{
    int i = shelled_slot(surface);

    if (i >= 0)
        return shelled[i].shell;
    bind_named();
    if (!surface || !g.shell)
        return NULL;
    for (i = 0; i < MAX_SHELLED && shelled[i].surface; i++)
        ;
    if (i == MAX_SHELLED) {
        log_msg("shell role table full");
        return NULL;
    }
    shelled[i].surface = surface;
    shelled[i].shell = wl_shell_get_shell_surface(g.shell, (struct wl_surface *)surface);
    wl_shell_surface_add_listener(shelled[i].shell, &shell_listener, NULL);
    return shelled[i].shell;
}

static void forget_shell_role(struct wl_proxy *surface)
{
    int i = shelled_slot(surface);

    if (i < 0)
        return;
    wl_shell_surface_destroy(shelled[i].shell);
    shelled[i].surface = NULL;
    shelled[i].shell = NULL;
}

/* A surface Firefox made a subsurface of the main window, shown instead as
 * a surface-group layer (webOS 4). Input the compositor sends to it goes to
 * the main window, which is the surface GTK knows. */
static int layered_slot(struct wl_proxy *surface)
{
    int i;

    for (i = 0; surface && i < MAX_LAYERED; i++)
        if (layered[i].surface == surface)
            return i;
    return -1;
}

static int is_layered(struct wl_surface *surface)
{
    int i = layered_slot((struct wl_proxy *)surface);

    return i >= 0 && layered[i].attached;
}

static struct wl_surface *input_surface(struct wl_surface *surface)
{
    if (ov_surface && surface == ov_surface && main_surface)
        return main_surface;
    if (gl_mode() && surface && surface == (struct wl_surface *)gl_window && main_surface)
        return main_surface;
    if (gl_mode() && gp_input_popup((struct wl_proxy *)surface, NULL, NULL) && main_surface)
        return main_surface;
    return is_layered(surface) && main_surface ? main_surface : surface;
}

static int ov_enabled(void);
static int ov_is_popup(struct wl_proxy *surface);
static void ov_child(struct wl_proxy *child, struct wl_proxy *popup);
static int ov_child_offset(struct wl_proxy *child, int32_t x, int32_t y);
static void ov_popup_at(struct wl_proxy *surface, int32_t x, int32_t y);

static void attach_slot(int i)
{
    char name[24];

    if (!main_group || layered[i].attached || layers_made >= 999)
        return;
    /* webOS 4 only shows a group member that is a window in its own right: a
     * bare surface stays invisible (tested in the emulator). Being in the
     * group keeps it from taking over the screen as a card. Only surfaces
     * that become layers get the role: a stand-alone window with content
     * made webOS rearrange the whole screen. */
    if (shelled_slot(layered[i].surface) < 0 && shell_role(layered[i].surface))
        wl_shell_surface_set_toplevel(shell_role(layered[i].surface));
    /* z stays below the pop-up overlay's 1000. */
    snprintf(name, sizeof name, "content%d", ++layers_made);
    layers[i] = wl_webos_surface_group_create_layer(main_group, name, layers_made);
    wl_webos_surface_group_attach(main_group, (struct wl_surface *)layered[i].surface, name);
    layered[i].attached = 1;
    log_msg("subsurface %p shown as group layer %s", (void *)layered[i].surface, name);
}

static void detach_slot(int i)
{
    if (!layered[i].attached)
        return;
    if (main_group)
        wl_webos_surface_group_detach(main_group, (struct wl_surface *)layered[i].surface);
    layered[i].attached = 0;
    log_msg("subsurface %p no longer shown", (void *)layered[i].surface);
}

/* The surface itself is going away. */
static void forget_slot(int i)
{
    detach_slot(i);
    layered[i].surface = NULL;
}

/* The main window changed: its group stays with the old window. */
static void drop_group(void)
{
    int i;

    for (i = 0; i < MAX_LAYERED; i++) {
        detach_slot(i);
        layers[i] = NULL;
    }
    main_group = NULL;
    group_root = NULL;
}

/* webOS 4: show a subsurface of the main window as a layer above it. Layers
 * are laid out like the window itself (the group protocol has no positions),
 * which suits Firefox's content surface. Other subsurfaces, and pop-ups GTK
 * places away from the origin, are not shown yet. */
static void emulate_subsurface(struct wl_proxy *child, struct wl_proxy *parent)
{
    int i;

    if (gl_mode() && child && parent) {
        /* The first window's content surface becomes the card, untagged. A
         * pop-up's content is copied into the overlay as in flat mode. */
        if (parent == gl_main_surface()) {
            gl_window_set(child);
        } else if (!gl_main_surface()) {
            gl_pending_add(child, parent);
        } else if (!gp_reparent(child, parent)) {
            ov_child(child, parent);    /* tagged at its first attach by gp_attach */
        }
        return;
    }
    tag_child_surface(child);
    if (child && parent && flat_mode()) {   /* copied into the flat window */
        ov_child(child, parent);
        return;
    }
    /* Firefox makes its content surface before the pop-up gets its role, so
     * remember it whatever the parent is; it is drawn once that is a pop-up. */
    if (child && parent && ov_enabled() && parent != (struct wl_proxy *)main_surface) {
        ov_child(child, parent);
        return;
    }
    if (!child || !parent || parent != (struct wl_proxy *)main_surface) {
        log_msg("subsurface %p of %p not shown (no wl_subcompositor)", (void *)child,
                (void *)parent);
        return;
    }
    if (!group_compositor && host_group_name && g.registry) {
        group_compositor = wl_registry_bind(g.registry, host_group_name,
                                            &wl_webos_surface_group_compositor_interface, 1);
        log_msg("surface group compositor %p", (void *)group_compositor);
    }
    if (!group_compositor) {
        log_msg("no surface groups either: subsurface %p not shown", (void *)child);
        return;
    }
    if (main_group && group_root != main_surface)
        drop_group();
    if (!main_group) {
        /* Group names must be unique; the old window's group is not destroyed. */
        char name[128];

        if (groups_made++)
            snprintf(name, sizeof name, "%s-%d", our_app_id(), groups_made);
        else
            snprintf(name, sizeof name, "%s", our_app_id());
        main_group = wl_webos_surface_group_compositor_create_surface_group(
            group_compositor, main_surface, name);
        group_root = main_surface;
        log_msg("surface group %p for the main window", (void *)main_group);
    }
    i = layered_slot(child);
    for (int j = 0; i < 0 && j < MAX_LAYERED; j++)
        if (!layered[j].surface)
            i = j;
    if (i < 0) {
        log_msg("too many layers: subsurface %p not shown", (void *)child);
        return;
    }
    if (layered[i].surface != child) {
        layered[i].surface = child;
        layered[i].committed = 0;
        layered[i].x = layered[i].y = 0;
    }
    /* Attached on its first commit, when GTK has placed it: GTK shows some
     * pop-ups as subsurfaces of the main window, and those go to the pop-up
     * overlay instead (a layer would cover the whole window). */
    if (layered[i].committed)
        attach_slot(i);
}

/* wl_subsurface.set_position: a layer cannot be moved, so an offset surface
 * (a pop-up) is taken out of view rather than shown in the wrong place. */
static void layered_position(struct wl_proxy *child, int32_t x, int32_t y)
{
    int i = layered_slot(child);

    if (ov_child_offset(child, x, y))
        return;
    if (ov_is_popup(child)) {             /* a GTK pop-up in the overlay */
        ov_popup_at(child, x, y);
        return;
    }
    if (i < 0)
        return;
    layered[i].x = x;
    layered[i].y = y;
    if ((x || y) && layered[i].committed) {
        log_trace("subsurface %p moved to %d,%d: drawn as a pop-up", (void *)child, x, y);
        detach_slot(i);
        layered[i].surface = NULL;
        ov_popup_at(child, x, y);
    }
}

/* First commit of a subsurface of the main window: a layer if it sits at
 * the origin (Firefox's content), otherwise a pop-up for the overlay. */
static void layered_commit(struct wl_proxy *surface)
{
    int i = layered_slot(surface);

    if (i < 0 || layered[i].committed)
        return;
    layered[i].committed = 1;
    if (layered[i].x || layered[i].y) {
        int32_t x = layered[i].x, y = layered[i].y;

        log_trace("subsurface %p at %d,%d: drawn as a pop-up", (void *)surface, x, y);
        layered[i].surface = NULL;
        ov_popup_at(surface, x, y);
        return;
    }
    attach_slot(i);
}

static void ov_popup_gone(struct wl_proxy *surface);

static void unlayer_surface(struct wl_proxy *child)
{
    int i = layered_slot(child);

    if (i >= 0)
        detach_slot(i);
    if (ov_is_popup(child))
        ov_popup_gone(child);
}

/* webOS 4 pop-ups. That compositor has no wl_subcompositor and shows no
 * small window at a place the app picks: a window group stretches every
 * member over the owner window, and POPUP and FLOATING windows are centred
 * or pinned to a corner (read from its QML in LG's webOS TV 4.0 emulator).
 * So the adapter draws pop-ups itself, into one transparent full-window
 * overlay that is a group layer above the page. When a pop-up surface
 * commits, its shared-memory buffer (webOS 4 TVs render in software) is
 * copied to the overlay at the pop-up's place; pointer input on the overlay
 * goes back to the pop-up under it. Used only without wl_subcompositor. */
#define OV_POOLS 64
#define OV_BUFS 128
#define OV_ITEMS 32

static struct ov_pool {
    struct wl_proxy *proxy;
    int fd;
    void *map;
    size_t size;
    int refs;
    int dead;
} ov_pools[OV_POOLS];

static struct ov_buf {
    struct wl_proxy *proxy;
    struct ov_pool *pool;
    int32_t offset, w, h, stride;
    uint32_t format;
} ov_bufs[OV_BUFS];

/* A surface drawn into the overlay: a pop-up's own surface (popup == surface)
 * or Firefox's content surface inside it, offset by off_x/off_y. */
static struct ov_item {
    struct wl_proxy *surface;
    struct wl_proxy *popup;
    int32_t off_x, off_y;
    struct wl_proxy *pending;      /* attached, not yet committed */
    int has_pending;
    struct wl_proxy *current;      /* committed */
    uint32_t *pix;
    int32_t w, h;
} ov_items[OV_ITEMS];

/* Where each pop-up's surface sits in the main window. */
static struct { struct wl_proxy *surface; int32_t x, y; unsigned seq; } ov_popups[OV_ITEMS];
static unsigned ov_seq;

static pthread_mutex_t ov_mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static __thread int ov_inside;     /* our own requests, or re-entered marshal */
static struct wl_compositor *ov_compositor;
static struct wl_shm *ov_shm;
static struct wl_webos_surface_group_layer *ov_layer;
static int ov_shown;
static struct wl_buffer *ov_buffer[2];
static uint32_t *ov_mem[2];
static int ov_free[2];
static int ov_dirty;
/* When each overlay buffer was last committed (ms, CLOCK_MONOTONIC). */
static long long ov_committed_at[2];

static long long now_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* WEBOS_XDG_FLAT_TEST=1: the flat window draws a grey background and a white
 * bar that moves with every frame (build/emu, app/diagnose.sh): grey and a
 * moving bar mean the TV shows the window and its new frames. */
static int ov_flat_test(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("WEBOS_XDG_FLAT_TEST");

        on = e && e[0] == '1';
    }
    return on;
}
static uint32_t *ov_canvas;
static int32_t ov_w, ov_h;

/* Hardware video in GL window mode. LG's media pipeline is fed by
 * geckotv --media-helper for Firefox's webOS media engine. It shows the
 * picture on a plane under the graphics, inside a region of the window
 * exported through wl_webos_foreign. Firefox clears the window to alpha 0
 * where the video is, so the plane shows through. The engine runs in
 * Firefox's content process. It connects on the unix socket
 * GECKOTV_VIDEO_SOCKET, gets "windowid <id>" and sends "rect x y w h vw vh"
 * with the video's place in window pixels and its natural size. The
 * destination region is the rect clipped to the window and the source
 * region is the matching part of the natural video. "rect 0 0 0 0 0 0" or
 * a closed socket clears the region. */
static struct wl_webos_exported *video_exported;
static char video_window_id[128];
static int video_listen_fd = -1;
static int video_client_fd = -1;
static int video_id_unsent;        /* window id to send under ov_mu */
static int32_t video_rect[4];      /* the last region set as x y w h, all 0 for none */
static char video_line[256];
static size_t video_line_len;

static struct wl_webos_foreign *gl_foreign_get(void);
static struct wl_compositor *gl_compositor_get(void);
static struct wl_proxy *gl_window;  /* defined with the GL window state below */

/* The regions for a video of vw x vh shown at rect, given as x y w h in
 * window pixels. dst is rect clipped to the win_w x win_h window and src is
 * the part of the video that lands in it. Returns 0 when nothing of it is
 * visible. */
static int video_clip(const int32_t rect[4], int32_t vw, int32_t vh, int32_t src[4],
                      int32_t dst[4])
{
    int32_t x0 = rect[0], y0 = rect[1], x1 = rect[0] + rect[2], y1 = rect[1] + rect[3];

    if (rect[2] <= 0 || rect[3] <= 0 || vw <= 0 || vh <= 0)
        return 0;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > win_w)
        x1 = win_w;
    if (y1 > win_h)
        y1 = win_h;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    dst[0] = x0;
    dst[1] = y0;
    dst[2] = x1 - x0;
    dst[3] = y1 - y0;
    src[0] = (int32_t)((int64_t)(x0 - rect[0]) * vw / rect[2]);
    src[1] = (int32_t)((int64_t)(y0 - rect[1]) * vh / rect[3]);
    src[2] = (int32_t)((int64_t)(x1 - rect[0]) * vw / rect[2]) - src[0];
    src[3] = (int32_t)((int64_t)(y1 - rect[1]) * vh / rect[3]) - src[1];
    if (src[2] < 1)
        src[2] = 1;
    if (src[3] < 1)
        src[3] = 1;
    return 1;
}

/* Under ov_mu. */
static void video_region_set(const int32_t rect[4], int32_t vw, int32_t vh)
{
    int32_t src[4], dst[4] = { 0, 0, 0, 0 };
    struct wl_region *source, *destination;

    if (!video_exported || !gl_compositor_get())
        return;
    if (!video_clip(rect, vw, vh, src, dst))
        memset(dst, 0, sizeof dst);
    if (!memcmp(dst, video_rect, sizeof dst) && dst[2] == 0)
        return;
    memcpy(video_rect, dst, sizeof video_rect);
    source = wl_compositor_create_region(gl_compositor_get());
    destination = wl_compositor_create_region(gl_compositor_get());
    if (dst[2] > 0) {
        wl_region_add(source, src[0], src[1], src[2], src[3]);
        wl_region_add(destination, dst[0], dst[1], dst[2], dst[3]);
    }
    wl_webos_exported_set_exported_window(video_exported, source, destination);
    wl_region_destroy(source);
    wl_region_destroy(destination);
    if (dst[2] > 0)
        log_msg("video plane at %d,%d %dx%d (source %d,%d %dx%d of %dx%d)", dst[0], dst[1],
                dst[2], dst[3], src[0], src[1], src[2], src[3], vw, vh);
    else
        log_msg("video plane hidden");
}

static void video_client_gone(const char *why)
{
    static const int32_t none[4] = { 0, 0, 0, 0 };

    if (video_client_fd < 0)
        return;
    log_msg("video client gone (%s)", why);
    close(video_client_fd);
    video_client_fd = -1;
    video_line_len = 0;
    video_region_set(none, 0, 0);
}

static void video_send(const char *line)
{
    if (video_client_fd < 0)
        return;
    if (send(video_client_fd, line, strlen(line), MSG_NOSIGNAL | MSG_DONTWAIT) < 0 &&
        errno != EAGAIN)
        video_client_gone(strerror(errno));
}

static void video_send_window_id(void)
{
    char line[160];

    if (!video_window_id[0])
        return;
    snprintf(line, sizeof line, "windowid %s\n", video_window_id);
    video_send(line);
}

/* Runs on the thread dispatching Wayland events. The socket is used only
 * under ov_mu, so the id is sent from there. */
static void video_window_id_assigned(void *data, struct wl_webos_exported *exported,
                                     const char *id, uint32_t type)
{
    (void)data;
    (void)exported;
    pthread_mutex_lock(&ov_mu);
    snprintf(video_window_id, sizeof video_window_id, "%s", id ? id : "");
    video_id_unsent = 1;
    pthread_mutex_unlock(&ov_mu);
    log_msg("video window exported as %s (type %u)", video_window_id, type);
}

static const struct wl_webos_exported_listener video_exported_listener = {
    video_window_id_assigned
};

static void video_socket_ensure(void)
{
    const char *path = getenv("GECKOTV_VIDEO_SOCKET");
    struct sockaddr_un addr;
    mode_t old;

    if (video_listen_fd >= 0 || !path || !path[0] || strlen(path) >= sizeof addr.sun_path)
        return;
    video_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (video_listen_fd < 0)
        return;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    unlink(path);
    old = umask(077);
    if (bind(video_listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(video_listen_fd, 1) != 0) {
        log_msg("video socket %s: %s", path, strerror(errno));
        close(video_listen_fd);
        video_listen_fd = -1;
    } else {
        log_msg("video socket %s", path);
    }
    umask(old);
}

/* Under ov_mu, once the GL window exists. */
static void video_export_ensure(void)
{
    if (video_exported || !gl_window || !getenv("GECKOTV_VIDEO_SOCKET") || !gl_foreign_get())
        return;
    video_exported = wl_webos_foreign_export_element(
        gl_foreign_get(), (struct wl_surface *)gl_window,
        WL_WEBOS_FOREIGN_WEBOS_EXPORTED_TYPE_VIDEO_OBJECT);
    wl_webos_exported_add_listener(video_exported, &video_exported_listener, NULL);
}

/* "rect x y w h vw vh" from the client. */
static void video_client_line(char *line)
{
    int32_t v[6];

    if (sscanf(line, "rect %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
        video_region_set(v, v[4], v[5]);
    else
        log_msg("video client said: %s", line);
}

static void video_client_read(void)
{
    ssize_t n;
    char *start, *nl;

    n = recv(video_client_fd, video_line + video_line_len,
             sizeof video_line - 1 - video_line_len, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        video_client_gone("closed");
        return;
    }
    if (n < 0)
        return;
    video_line_len += (size_t)n;
    video_line[video_line_len] = '\0';
    start = video_line;
    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        video_client_line(start);
        start = nl + 1;
    }
    video_line_len = strlen(start);
    memmove(video_line, start, video_line_len + 1);
    if (video_line_len == sizeof video_line - 1)
        video_line_len = 0;
}

/* One client at a time, and only one started by this user. A client that
 * went away makes room for the next one at once. Called every 20 ms under
 * ov_mu from flat_pump. */
static void video_accept(void)
{
    struct ucred cred;
    socklen_t len = sizeof cred;
    int fd;

    if (video_listen_fd < 0)
        return;
    if (video_client_fd >= 0) {
        struct pollfd p = { video_client_fd, POLLIN, 0 };

        if (poll(&p, 1, 0) > 0)
            video_client_read();
    }
    fd = accept4(video_listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0) {
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != getuid()) {
            log_msg("video socket: refused a connection from uid %u", (unsigned)cred.uid);
            close(fd);
        } else if (video_client_fd >= 0) {
            log_msg("video socket: a second client refused");
            close(fd);
        } else {
            video_client_fd = fd;
            log_msg("video client connected (pid %d)", (int)cred.pid);
            video_id_unsent = 1;
        }
    }
    if (video_client_fd >= 0 && video_id_unsent && video_window_id[0]) {
        video_id_unsent = 0;
        video_send_window_id();
    }
}
static int ov_failed;
/* Pointer: the pop-up GTK is told the pointer is in, while it is on the overlay. */
static int ov_ptr_on;
static struct wl_surface *ov_ptr_popup;
static int32_t ov_ptr_x, ov_ptr_y;
static uint32_t ov_ptr_serial;

/* webOS 6 has no wl_subcompositor either, but its compositor does not draw
 * surface-group layers (LG's 6.0 emulator: the layer never gets a parent in
 * the scene; a 6.5 TV never shows the window at all). So there the overlay
 * surface is the window itself: GTK's window, Firefox's content and pop-ups
 * are all copied into it, and none of them is shown on its own. On unless
 * the compositor has wl_subcompositor; WEBOS_XDG_FLAT=0 or 1 overrides. */
static int flat_mode(void)
{
    static int forced = -2;

    if (forced == -2) {
        const char *e = getenv("WEBOS_XDG_FLAT");

        forced = e && e[0] ? atoi(e) != 0 : -1;
    }
    if (host_subcompositor_name)
        return 0;
    return forced >= 0 ? forced : host_foreign_name != 0;
}

static int ov_enabled(void)
{
    return !host_subcompositor_name && (host_group_name || flat_mode());
}

static struct ov_pool *ov_pool_find(struct wl_proxy *proxy)
{
    int i;

    for (i = 0; proxy && i < OV_POOLS; i++)
        if (ov_pools[i].proxy == proxy)
            return &ov_pools[i];
    return NULL;
}

static void ov_pool_release(struct ov_pool *p)
{
    if (!p->dead || p->refs > 0)
        return;
    if (p->map)
        munmap(p->map, p->size);
    if (p->fd >= 0)
        close(p->fd);
    memset(p, 0, sizeof *p);
    p->fd = -1;
}

static void ov_pool_add(struct wl_proxy *proxy, int fd, int32_t size)
{
    int i;

    if (!proxy || size <= 0)
        return;
    for (i = 0; i < OV_POOLS && ov_pools[i].proxy; i++)
        ;
    if (i == OV_POOLS)
        return;
    ov_pools[i].fd = dup(fd);
    if (ov_pools[i].fd < 0)
        return;
    ov_pools[i].map = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, ov_pools[i].fd, 0);
    if (ov_pools[i].map == MAP_FAILED) {
        close(ov_pools[i].fd);
        ov_pools[i].fd = -1;
        ov_pools[i].map = NULL;
        return;
    }
    ov_pools[i].proxy = proxy;
    ov_pools[i].size = (size_t)size;
    ov_pools[i].refs = 0;
    ov_pools[i].dead = 0;
}

static void ov_pool_resize(struct wl_proxy *proxy, int32_t size)
{
    struct ov_pool *p = ov_pool_find(proxy);
    void *map;

    if (!p || size <= 0 || (size_t)size <= p->size)
        return;
    map = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, p->fd, 0);
    if (map == MAP_FAILED)
        return;
    munmap(p->map, p->size);
    p->map = map;
    p->size = (size_t)size;
}

static void ov_pool_destroyed(struct wl_proxy *proxy)
{
    struct ov_pool *p = ov_pool_find(proxy);

    if (!p)
        return;
    p->dead = 1;
    p->proxy = (struct wl_proxy *)p;   /* the id may be reused */
    ov_pool_release(p);
}

static void ov_buf_add(struct wl_proxy *buffer, struct wl_proxy *pool, union wl_argument *args)
{
    struct ov_pool *p = ov_pool_find(pool);
    int i;

    if (!buffer || !p)
        return;
    for (i = 0; i < OV_BUFS && ov_bufs[i].proxy; i++)
        ;
    if (i == OV_BUFS)
        return;
    ov_bufs[i].proxy = buffer;
    ov_bufs[i].pool = p;
    ov_bufs[i].offset = args[1].i;
    ov_bufs[i].w = args[2].i;
    ov_bufs[i].h = args[3].i;
    ov_bufs[i].stride = args[4].i;
    ov_bufs[i].format = args[5].u;
    p->refs++;
}

static struct ov_buf *ov_buf_find(struct wl_proxy *buffer)
{
    int i;

    for (i = 0; buffer && i < OV_BUFS; i++)
        if (ov_bufs[i].proxy == buffer)
            return &ov_bufs[i];
    return NULL;
}

static void ov_buf_destroyed(struct wl_proxy *buffer)
{
    struct ov_buf *b = ov_buf_find(buffer);
    int i;

    if (!b)
        return;
    for (i = 0; i < OV_ITEMS; i++) {
        if (ov_items[i].current == buffer)
            ov_items[i].current = NULL;
        if (ov_items[i].pending == buffer)
            ov_items[i].pending = NULL;
    }
    b->pool->refs--;
    ov_pool_release(b->pool);
    memset(b, 0, sizeof *b);
}

static int ov_popup_slot(struct wl_proxy *surface)
{
    int i;

    for (i = 0; surface && i < OV_ITEMS; i++)
        if (ov_popups[i].surface == surface)
            return i;
    return -1;
}

static struct ov_item *ov_item_find(struct wl_proxy *surface)
{
    int i;

    for (i = 0; surface && i < OV_ITEMS; i++)
        if (ov_items[i].surface == surface)
            return &ov_items[i];
    return NULL;
}

static struct ov_item *ov_item_add(struct wl_proxy *surface, struct wl_proxy *popup)
{
    struct ov_item *it = ov_item_find(surface);
    int i;

    if (it)
        return it;
    for (i = 0; i < OV_ITEMS && ov_items[i].surface; i++)
        ;
    if (i == OV_ITEMS)
        return NULL;
    memset(&ov_items[i], 0, sizeof ov_items[i]);
    ov_items[i].surface = surface;
    ov_items[i].popup = popup;
    return &ov_items[i];
}

static void ov_item_drop(struct ov_item *it)
{
    free(it->pix);
    memset(it, 0, sizeof *it);
}

static void ov_buffer_release(void *data, struct wl_buffer *buffer);
static const struct wl_buffer_listener ov_buffer_listener = { ov_buffer_release };

static int ov_memfd(size_t size)
{
    int fd = -1;
#ifdef __NR_memfd_create
    fd = syscall(__NR_memfd_create, "webos-xdg-popups", 1u /* MFD_CLOEXEC */);
#endif
    if (fd < 0) {
        char path[] = "/tmp/webos-xdg-popups-XXXXXX";

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

/* The overlay: two full-window buffers, a surface with a wl_shell role (a
 * group member needs one to be shown) and a layer high above the page. */
static int ov_ensure(void)
{
    struct wl_shm_pool *pool;
    struct wl_shell_surface *ss;
    size_t frame;
    char *mem;
    int fd, i;

    if (ov_surface)
        return 1;
    /* The overlay maps after the card, or it would become the card. */
    if (gl_mode() && !gl_window_shown)
        return 0;
    if (ov_failed || !g.registry || (!main_group && !flat_mode()) || !host_compositor_name ||
        !host_shm_name)
        return 0;
    ov_w = geo_w > 0 ? geo_w : win_w;
    ov_h = geo_h > 0 ? geo_h : win_h;
    frame = (size_t)ov_w * (size_t)ov_h * 4;
    fd = ov_memfd(frame * 2);
    mem = fd < 0 ? MAP_FAILED : mmap(NULL, frame * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ov_canvas = calloc((size_t)ov_w * (size_t)ov_h, 4);
    if (mem == MAP_FAILED || !ov_canvas) {
        log_msg("pop-up overlay: no memory");
        if (fd >= 0)
            close(fd);
        ov_failed = 1;
        return 0;
    }
    ov_compositor = wl_registry_bind(g.registry, host_compositor_name, &wl_compositor_interface,
                                     server_compositor_version && server_compositor_version < 3
                                         ? server_compositor_version : 3);
    ov_shm = wl_registry_bind(g.registry, host_shm_name, &wl_shm_interface, 1);
    pool = wl_shm_create_pool(ov_shm, fd, (int32_t)(frame * 2));
    for (i = 0; i < 2; i++) {
        ov_buffer[i] = wl_shm_pool_create_buffer(pool, (int32_t)(i * frame), ov_w, ov_h, ov_w * 4,
                                                 WL_SHM_FORMAT_ARGB8888);
        wl_buffer_add_listener(ov_buffer[i], &ov_buffer_listener, (void *)(intptr_t)i);
        ov_mem[i] = (uint32_t *)(mem + i * frame);
        ov_free[i] = 1;
    }
    wl_shm_pool_destroy(pool);
    close(fd);
    ov_surface = wl_compositor_create_surface(ov_compositor);
    if (gl_mode()) {
        /* A second toplevel of the app typed OVERLAY. The compositor of a
         * webOS 6.5 TV draws it over the card, blended (findings.md section
         * 19). It takes no input, so the pointer stays on the GL window.
         * It is never destroyed because the card is minimized the moment a
         * second surface of the app goes away. */
        struct wl_region *none;

        ss = shell_role((struct wl_proxy *)ov_surface);
        if (ss)
            wl_shell_surface_set_toplevel(ss);
        if (g.webos) {
            struct wl_webos_shell_surface *ws =
                wl_webos_shell_get_shell_surface(g.webos, ov_surface);

            if (ws) {
                wl_webos_shell_surface_set_property(ws, "appId", our_app_id());
                wl_webos_shell_surface_set_property(ws, "displayAffinity", "0");
                wl_webos_shell_surface_set_property(ws, "_WEBOS_WINDOW_TYPE",
                                                    "_WEBOS_WINDOW_TYPE_OVERLAY");
            }
        }
        none = wl_compositor_create_region(ov_compositor);
        wl_surface_set_input_region(ov_surface, none);
        wl_region_destroy(none);
        ov_shown = 1;
        log_msg("pop-up overlay %p, %dx%d: an OVERLAY window over the GL window",
                (void *)ov_surface, ov_w, ov_h);
        return 1;
    }
    if (flat_mode()) {
        /* The window itself: map_toplevel gives it the window's roles. */
        ov_shown = 1;
        log_msg("flat window %p, %dx%d", (void *)ov_surface, ov_w, ov_h);
        video_socket_ensure();
        return 1;
    }
    tag_surface_as((struct wl_proxy *)ov_surface, "_WEBOS_WINDOW_TYPE_SUBSURFACE");
    ss = shell_role((struct wl_proxy *)ov_surface);
    if (ss)
        wl_shell_surface_set_toplevel(ss);
    /* Attached once and never detached (see layers_made); with no pop-up it
     * shows a transparent frame and takes no input. */
    ov_layer = wl_webos_surface_group_create_layer(main_group, "popups", 1000);
    wl_webos_surface_group_attach(main_group, ov_surface, "popups");
    ov_shown = 1;
    log_msg("pop-up overlay %p, %dx%d", (void *)ov_surface, ov_w, ov_h);
    return 1;
}

/* Compose every pop-up into the canvas and show it. Called with ov_mu held. */
static void ov_blit(const struct ov_item *it, int32_t x0, int32_t y0)
{
    int32_t row;

    for (row = 0; row < it->h; row++) {
        int32_t y = y0 + row, x = x0, n = it->w, skip = 0;

        if (y < 0 || y >= ov_h)
            continue;
        if (x < 0) {
            skip = -x;
            n -= skip;
            x = 0;
        }
        if (x + n > ov_w)
            n = ov_w - x;
        if (n > 0) {
            /* Source over (wl_shm ARGB is premultiplied): a menu's shadow
             * and rounded corners show what lies below instead of black. */
            uint32_t *d = ov_canvas + (size_t)y * ov_w + x;
            const uint32_t *p = it->pix + (size_t)row * it->w + skip;
            int32_t k;

            for (k = 0; k < n; k++) {
                uint32_t sp = p[k], a = sp >> 24, ia, rb, ag;

                if (a == 255) {
                    d[k] = sp;
                } else if (a) {
                    ia = 255 - a;
                    rb = (d[k] & 0x00ff00ffu) * ia;
                    ag = ((d[k] >> 8) & 0x00ff00ffu) * ia;
                    rb = ((rb + 0x00800080u + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
                    ag = (ag + 0x00800080u + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
                    d[k] = sp + (rb | ag);
                }
            }
        }
    }
}

static void ov_redraw(void)
{
    struct wl_region *region;
    unsigned done = 0;
    int i, b, shown = 0;

    if (!ov_ensure())
        return;
    if (flat_mode() && !gl_mode()) {
        /* The window itself: opaque. A webOS 6.5 TV seems never to draw a
         * fully transparent frame (the first ones, before Firefox has drawn)
         * nor answer its frame callbacks, and Firefox waits for those. */
        uint32_t bg = ov_flat_test() ? 0xff404040u : 0xff000000u;
        size_t k, n = (size_t)ov_w * (size_t)ov_h;

        for (k = 0; k < n; k++)
            ov_canvas[k] = bg;
    } else {
        memset(ov_canvas, 0, (size_t)ov_w * (size_t)ov_h * 4);
    }
    region = wl_compositor_create_region(ov_compositor);
    /* Pop-ups oldest first; within one, GTK's own surface (the window
     * background) first and Firefox's content on top of it. */
    for (;;) {
        int p = -1, k, pass;

        for (k = 0; k < OV_ITEMS; k++)
            if (ov_popups[k].surface && ov_popups[k].seq > done &&
                (p < 0 || ov_popups[k].seq < ov_popups[p].seq))
                p = k;
        if (p < 0)
            break;
        done = ov_popups[p].seq;
        for (pass = 0; pass < 2; pass++) {
            for (i = 0; i < OV_ITEMS; i++) {
                struct ov_item *it = &ov_items[i];

                if (!it->surface || it->popup != ov_popups[p].surface || !it->pix ||
                    (it->surface == it->popup) != (pass == 0))
                    continue;
                ov_blit(it, ov_popups[p].x + it->off_x, ov_popups[p].y + it->off_y);
                wl_region_add(region, ov_popups[p].x + it->off_x, ov_popups[p].y + it->off_y,
                              it->w, it->h);
                shown++;
            }
        }
    }
    if (flat_mode() && ov_flat_test()) {
        static unsigned frame;
        int32_t bx = (int32_t)((frame++ * 24u) % (unsigned)(ov_w - 96)), by = ov_h - 48, x, y;

        for (y = by; y < by + 32 && y < ov_h; y++)
            for (x = bx; x < bx + 96 && x < ov_w; x++)
                ov_canvas[(size_t)y * ov_w + x] = 0xffffffffu;
    }
    if (!gl_mode())
        wl_surface_set_input_region(ov_surface, region);
    wl_region_destroy(region);
    b = ov_free[0] ? 0 : ov_free[1] ? 1 : -1;
    /* A webOS 6.5 TV never releases the flat window's buffers (0 releases
     * in a 30 s trace). Waiting for one deadlocks: no commit, so no frame
     * callbacks for Firefox, so no drawing. Its compositor has what it
     * needs by then, so reuse the older buffer once it is 20 ms old
     * (about 50 frames a second); the worst case is a torn frame. */
    if (b < 0 && flat_mode() && !(getenv("WEBOS_XDG_OV_NO_RELEASE") &&
                                   !strcmp(getenv("WEBOS_XDG_OV_NO_RELEASE"), "2"))) {
        struct timespec now;
        long long ms;
        int older = ov_committed_at[0] <= ov_committed_at[1] ? 0 : 1;

        clock_gettime(CLOCK_MONOTONIC, &now);
        ms = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        if (ms - ov_committed_at[older] >= 20) {
            static int told;

            if (!told++)
                log_msg("flat window: the compositor keeps its buffers; reusing them");
            b = older;
        }
    }
    {
        static int last_shown = -1;

        if (shown != last_shown)
            log_trace("pop-up overlay: %d surface(s) drawn%s", shown,
                    b < 0 ? ", waiting for a free buffer" : "");
        last_shown = shown;
    }
    if (b < 0) {
        ov_dirty = 1;     /* redrawn when the compositor frees a buffer */
        return;
    }
    ov_dirty = 0;
    memcpy(ov_mem[b], ov_canvas, (size_t)ov_w * (size_t)ov_h * 4);
    ov_free[b] = 0;
    {
        struct timespec now;

        clock_gettime(CLOCK_MONOTONIC, &now);
        ov_committed_at[b] = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    }
    if (getenv("WEBOS_XDG_OV_DEBUG"))
        log_msg("overlay commit buffer %d (%d drawn)", b, shown);
    wl_surface_attach(ov_surface, ov_buffer[b], 0, 0);
    wl_surface_damage(ov_surface, 0, 0, ov_w, ov_h);
    wl_surface_commit(ov_surface);
    (void)shown;
}

static void ov_update(void)
{
    ov_inside = 1;
    ov_redraw();
    ov_inside = 0;
}

static void ov_buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    /* Test switch: act like the webOS 6.5 TV, which never releases; 2 also
     * turns off the reuse in ov_redraw (the TV's black screen). */
    if (getenv("WEBOS_XDG_OV_NO_RELEASE"))
        return;
    if (getenv("WEBOS_XDG_OV_DEBUG"))
        log_msg("overlay buffer %d released", (int)(intptr_t)data);
    pthread_mutex_lock(&ov_mu);
    ov_free[(intptr_t)data] = 1;
    if (ov_dirty)
        ov_update();
    pthread_mutex_unlock(&ov_mu);
}

/* A pop-up appeared at x,y of the main window (xdg_popup without
 * wl_subcompositor), or moved there. */
static void ov_popup_at(struct wl_proxy *surface, int32_t x, int32_t y)
{
    int i;

    pthread_mutex_lock(&ov_mu);
    i = ov_popup_slot(surface);
    if (i < 0)
        for (i = 0; i < OV_ITEMS && ov_popups[i].surface; i++)
            ;
    if (i < OV_ITEMS) {
        if (ov_popups[i].surface != surface)
            ov_popups[i].seq = ++ov_seq;     /* newer pop-ups are drawn on top */
        ov_popups[i].surface = surface;
        ov_popups[i].x = x;
        ov_popups[i].y = y;
        ov_item_add(surface, surface);
        gp_popup_moved(surface);
        ov_update();
    }
    pthread_mutex_unlock(&ov_mu);
}

static void ov_popup_gone(struct wl_proxy *surface)
{
    int i, p;

    pthread_mutex_lock(&ov_mu);
    p = ov_popup_slot(surface);
    if (p >= 0) {
        for (i = 0; i < OV_ITEMS; i++)
            if (ov_items[i].surface && ov_items[i].popup == surface)
                ov_item_drop(&ov_items[i]);
        ov_popups[p].surface = NULL;
        if (ov_ptr_popup == (struct wl_surface *)surface)
            ov_ptr_popup = NULL;
        gp_popup_gone(surface);
        ov_update();
    }
    pthread_mutex_unlock(&ov_mu);
}

static int ov_is_popup(struct wl_proxy *surface)
{
    int r;

    pthread_mutex_lock(&ov_mu);
    r = ov_popup_slot(surface) >= 0;
    pthread_mutex_unlock(&ov_mu);
    return r;
}

/* Firefox's content surface inside a pop-up (it was a subsurface). */
static void ov_child(struct wl_proxy *child, struct wl_proxy *popup)
{
    struct ov_item *it;

    pthread_mutex_lock(&ov_mu);
    it = ov_item_add(child, popup);
    if (it && it->popup != popup) {   /* Firefox moved it to another pop-up */
        it->popup = popup;
        it->off_x = it->off_y = 0;
        ov_update();
    }
    pthread_mutex_unlock(&ov_mu);
    log_trace("subsurface %p of %p kept for the pop-up overlay", (void *)child, (void *)popup);
}

static int ov_child_offset(struct wl_proxy *child, int32_t x, int32_t y)
{
    struct ov_item *it;

    if (gp_offset(child, x, y))
        return 1;
    pthread_mutex_lock(&ov_mu);
    it = ov_item_find(child);
    if (it && it->popup != child) {
        it->off_x = x;
        it->off_y = y;
        ov_update();
    }
    pthread_mutex_unlock(&ov_mu);
    return it != NULL;
}

/* Flat mode (see flat_mode): the overlay surface for GTK's main window,
 * which is then never shown itself. NULL outside flat mode. */
static struct wl_proxy *flat_window_for(struct wl_proxy *gtk_surface)
{
    int ok;

    if (!flat_mode())
        return NULL;
    main_surface = (struct wl_surface *)gtk_surface;
    pthread_mutex_lock(&ov_mu);
    ov_inside = 1;
    ok = ov_ensure();
    ov_inside = 0;
    pthread_mutex_unlock(&ov_mu);
    if (!ok)
        return NULL;
    flat_pump_start();
    tag_surface_as(gtk_surface, "_WEBOS_WINDOW_TYPE_SUBSURFACE");
    return (struct wl_proxy *)ov_surface;
}

/* Flat mode: what a webOS 6.5 TV's compositor never sends back for the
 * window, although it shows every frame committed to it (a 40 s trace: no
 * frame callback answered, no buffer released, no output enter). GTK freezes
 * its frame clock until a frame callback returns, and Firefox goes from its
 * own timer to frame callbacks once its first buffer is attached, so neither
 * painted. So in flat mode the adapter answers them itself:
 *  - wl_surface.frame on a copied surface makes a virtual wl_callback (V_FRAME),
 *    answered after the window's next frame, at most every 16 ms;
 *  - a buffer committed to a copied surface is copied at once and released
 *    to the client the same way. The compositor gets none of those surfaces'
 *    content, so no second release can come from it.
 * Both go out from a wl_display.sync's done, i.e. on the thread that
 * dispatches the display, where real events arrive. */
#define FLAT_PENDING 128
static struct wl_proxy *flat_frames[FLAT_PENDING];     /* virtual wl_callbacks */
static int flat_frames_n;
static struct wl_proxy *flat_releases[FLAT_PENDING];   /* client buffers, one per attach */
static int flat_releases_n;
/* What the round trip in flight will answer: a handler asks for its next
 * frame callback from inside done, and that one waits for the next round. */
static struct wl_proxy *flat_frames_sent[FLAT_PENDING], *flat_releases_sent[FLAT_PENDING];
static int flat_frames_sent_n, flat_releases_sent_n;
static int flat_sync_inflight;
static long long flat_last_answer_ms;
static unsigned flat_frames_answered, flat_buffers_released, flat_frames_copied;

/* Both with ov_mu held. */
static void flat_pending_add(struct wl_proxy **list, int *n, struct wl_proxy *p)
{
    if (*n < FLAT_PENDING)
        list[(*n)++] = p;
    else
        log_msg("flat window: too many pending answers, one lost");
}

static void flat_pending_forget(struct wl_proxy **list, int *n, struct wl_proxy *p)
{
    int i, j = 0;

    for (i = 0; i < *n; i++)
        if (list[i] != p)
            list[j++] = list[i];
    *n = j;
}

/* A destroyed buffer or callback, off both lists. With ov_mu held. */
static void flat_forget(struct wl_proxy *p)
{
    flat_pending_forget(flat_frames, &flat_frames_n, p);
    flat_pending_forget(flat_frames_sent, &flat_frames_sent_n, p);
    flat_pending_forget(flat_releases, &flat_releases_n, p);
    flat_pending_forget(flat_releases_sent, &flat_releases_sent_n, p);
}

/* On the dispatching thread. Entries are taken one at a time, not under
 * ov_mu: a handler may destroy a buffer or callback still on the list
 * (flat_pending_forget then drops it), and may wait for a Firefox lock
 * held by a thread that is inside wl_surface.commit with ov_mu. */
static void flat_sync_done(void *data, struct wl_callback *cb, uint32_t serial)
{
    uint32_t t;

    (void)data;
    (void)serial;
    wl_callback_destroy(cb);
    pthread_mutex_lock(&ov_mu);
    flat_sync_inflight = 0;
    flat_last_answer_ms = now_ms();
    pthread_mutex_unlock(&ov_mu);
    for (;;) {
        struct wl_proxy *buffer;
        const struct wl_buffer_listener *l;

        pthread_mutex_lock(&ov_mu);
        if (!flat_releases_sent_n) {
            pthread_mutex_unlock(&ov_mu);
            break;
        }
        buffer = flat_releases_sent[0];
        flat_pending_forget(flat_releases_sent, &flat_releases_sent_n, buffer);
        pthread_mutex_unlock(&ov_mu);
        l = wl_proxy_get_listener(buffer);
        if (l && l->release)
            l->release(wl_proxy_get_user_data(buffer), (struct wl_buffer *)buffer);
        flat_buffers_released++;
    }
    t = (uint32_t)now_ms();      /* CLOCK_MONOTONIC ms, as compositors send */
    for (;;) {
        struct wl_proxy *frame;
        struct virt *v;
        const struct wl_callback_listener *l;

        pthread_mutex_lock(&ov_mu);
        if (!flat_frames_sent_n) {
            pthread_mutex_unlock(&ov_mu);
            break;
        }
        frame = flat_frames_sent[0];
        flat_pending_forget(flat_frames_sent, &flat_frames_sent_n, frame);
        pthread_mutex_unlock(&ov_mu);
        v = virt_get(frame);
        if (!v || v->kind != V_FRAME || !v->listener)
            continue;
        l = v->listener;
        if (l->done)
            l->done(v->data, (struct wl_callback *)frame, t);
        flat_frames_answered++;
    }
}

static const struct wl_callback_listener flat_sync_listener = { flat_sync_done };

/* With ov_mu held: start a round trip that answers what is pending. */
static void flat_answer_maybe(void)
{
    struct wl_callback *cb;

    if (flat_sync_inflight || (!flat_frames_n && !flat_releases_n) || !g.display)
        return;
    if (now_ms() - flat_last_answer_ms < 16)
        return;
    memcpy(flat_frames_sent, flat_frames, (size_t)flat_frames_n * sizeof *flat_frames);
    flat_frames_sent_n = flat_frames_n;
    flat_frames_n = 0;
    memcpy(flat_releases_sent, flat_releases, (size_t)flat_releases_n * sizeof *flat_releases);
    flat_releases_sent_n = flat_releases_n;
    flat_releases_n = 0;
    flat_sync_inflight = 1;
    ov_inside = 1;
    cb = wl_display_sync(g.display);
    if (cb)
        wl_callback_add_listener(cb, &flat_sync_listener, NULL);
    wl_display_flush(g.display);
    ov_inside = 0;
    if (!cb)
        flat_sync_inflight = 0;
}

/* Flat mode: a surface the window copies; the compositor gets none of its
 * content. */
static int flat_hidden(struct wl_proxy *surface)
{
    int r;

    if (!flat_mode() || !surface || surface == (struct wl_proxy *)ov_surface)
        return 0;
    pthread_mutex_lock(&ov_mu);
    r = ov_item_find(surface) != NULL;
    pthread_mutex_unlock(&ov_mu);
    return r;
}

/* Every 20 ms: a redraw waiting for a buffer is retried (when the compositor
 * keeps its buffers no release event comes to retry it, see ov_redraw), and
 * pending frame callbacks and releases are answered. */
static void *flat_pump(void *unused)
{
    long long reported = 0;

    (void)unused;
    for (;;) {
        usleep(20 * 1000);
        pthread_mutex_lock(&ov_mu);
        video_accept();
        if ((ov_dirty || ov_flat_test()) && ov_surface)
            ov_update();
        flat_answer_maybe();
        if (getenv("WEBOS_XDG_OV_DEBUG") && now_ms() - reported >= 5000) {
            reported = now_ms();
            log_msg("flat: %u frame callbacks answered, %u buffers released, %u frames copied",
                    flat_frames_answered, flat_buffers_released, flat_frames_copied);
        }
        pthread_mutex_unlock(&ov_mu);
    }
    return NULL;
}

static void flat_pump_start(void)
{
    static int started;
    pthread_t thread;

    pthread_mutex_lock(&ov_mu);
    if (!started && pthread_create(&thread, NULL, flat_pump, NULL) == 0) {
        pthread_detach(thread);
        started = 1;
    }
    pthread_mutex_unlock(&ov_mu);
}

/* The flat window has its roles: GTK's window becomes its bottom picture,
 * and the first frame maps it. webOS 6 ignores set_state on a surface
 * without content, so full screen is asked for again now. */
static void flat_window_shown(struct wl_proxy *gtk_surface, struct wl_webos_shell_surface *ws)
{
    flat_pump_start();
    ov_popup_at(gtk_surface, 0, 0);
    if (ws)
        wl_webos_shell_surface_set_state(ws, WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);
    log_msg("flat window shown for %p", (void *)gtk_surface);
}

/* GL window mode (WEBOS_XDG_GL=1 on a flat-mode compositor). Firefox draws
 * with the GPU, and the Mali driver's buffers cannot be copied the way the
 * flat window copies shared memory. So Firefox's content surface (the
 * subsurface under GTK's first window) is the window the compositor sees.
 * It gets the wl_shell toplevel and the app's wl_webos_shell properties,
 * and its requests pass through. GTK's own window surface is never shown.
 * As in flat mode its frame callbacks are answered here and its buffers
 * released, without copying. Pop-ups are copied into the overlay
 * (ov_ensure). That is a second window of the app over the card, since the
 * compositor of a webOS 6.5 TV never draws a surface imported into a region
 * of the window through wl_webos_foreign (findings.md section 19). Pointer
 * and keyboard events come on the window and go to GTK's window, or to the
 * pop-up under the pointer. The GL state is read and written under ov_mu. */
#define GL_PENDING 8
static struct wl_proxy *gl_main;    /* GTK's first window surface */
static struct wl_shell_surface *gl_shell;
static struct wl_webos_shell_surface *gl_webos;
static struct wl_webos_foreign *gl_foreign;
static struct wl_compositor *gl_compositor;
static int gl_window_buffer;       /* a buffer was attached since the last commit */
/* Content surfaces seen before their parent had a role. */
static struct { struct wl_proxy *child, *parent; } gl_pending[GL_PENDING];

static int gl_mode(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("WEBOS_XDG_GL");
        int want = e && *e == '1';

        /* flat_mode is only known once the registry has been read. */
        if (!want)
            on = 0;
        else if (host_foreign_name || host_subcompositor_name)
            on = flat_mode();
        else
            return 0;
        if (on)
            log_msg("GL window mode: Firefox's content surface is the window");
    }
    return on;
}

static struct wl_proxy *gl_main_surface(void)
{
    return gl_main;
}

static void gl_pending_add(struct wl_proxy *child, struct wl_proxy *parent)
{
    int i;

    for (i = 0; i < GL_PENDING && gl_pending[i].child; i++)
        ;
    if (i == GL_PENDING) {
        log_msg("GL mode: too many content surfaces waiting for a parent, %p dropped",
                (void *)child);
        return;
    }
    gl_pending[i].child = child;
    gl_pending[i].parent = parent;
    log_msg("GL mode: content surface %p of %p kept until its parent has a role", (void *)child,
            (void *)parent);
}

static struct wl_proxy *gl_pending_take(struct wl_proxy *parent)
{
    int i;

    for (i = 0; i < GL_PENDING; i++)
        if (gl_pending[i].child && gl_pending[i].parent == parent) {
            struct wl_proxy *child = gl_pending[i].child;

            gl_pending[i].child = gl_pending[i].parent = NULL;
            return child;
        }
    return NULL;
}

static void gl_pending_drop(struct wl_proxy *surface)
{
    int i;

    for (i = 0; i < GL_PENDING; i++)
        if (gl_pending[i].child == surface || gl_pending[i].parent == surface)
            gl_pending[i].child = gl_pending[i].parent = NULL;
}

static void gl_main_mapped(struct virt *xdg_surface, struct wl_proxy *gtk_surface)
{
    struct wl_proxy *child;

    (void)xdg_surface;
    pthread_mutex_lock(&ov_mu);
    gl_main = gtk_surface;
    main_surface = (struct wl_surface *)gtk_surface;
    mapped_one = 1;
    pthread_mutex_unlock(&ov_mu);
    text_input_enter_main();
    flat_pump_start();
    /* Never shown. No card, frames answered and buffers released here. */
    tag_surface_as(gtk_surface, "_WEBOS_WINDOW_TYPE_SUBSURFACE");
    ov_popup_at(gtk_surface, 0, 0);
    log_msg("GL mode: GTK's window %p mapped, waiting for Firefox's content surface",
            (void *)gtk_surface);
    pthread_mutex_lock(&ov_mu);
    child = gl_pending_take(gtk_surface);
    pthread_mutex_unlock(&ov_mu);
    if (child)
        gl_window_set(child);
}

/* Firefox's content surface, the card. The first content committed to it
 * maps it. Full screen is asked for again right after that commit because
 * webOS 6 ignores it on an empty surface. */
static void gl_window_set(struct wl_proxy *child)
{
    pthread_mutex_lock(&ov_mu);
    if (gl_window == child) {
        pthread_mutex_unlock(&ov_mu);
        return;
    }
    if (gl_window) {
        log_msg("GL window %p replaced by %p", (void *)gl_window, (void *)child);
        if (gl_webos) {
            wl_webos_shell_surface_destroy(gl_webos);
            gl_webos = NULL;
        }
        forget_shell_role(gl_window);
        gl_shell = NULL;
    }
    gl_window = child;
    gl_window_buffer = gl_window_shown = 0;
    bind_named();
    gl_shell = shell_role(child);
    if (gl_shell)
        wl_shell_surface_set_toplevel(gl_shell);
    if (g.webos) {
        gl_webos = wl_webos_shell_get_shell_surface(g.webos, (struct wl_surface *)child);
        if (gl_webos) {
            wl_webos_shell_surface_set_property(gl_webos, "appId", our_app_id());
            wl_webos_shell_surface_set_property(gl_webos, "title", "Firefox");
            wl_webos_shell_surface_set_property(gl_webos, "displayAffinity", "0");
            wl_webos_shell_surface_set_property(gl_webos, "_WEBOS_ACCESS_POLICY_KEYS_BACK",
                                                "true");
            wl_webos_shell_surface_set_key_mask(gl_webos, 0xFFFFFFFAu);
            wl_webos_shell_surface_set_property(gl_webos, "_WEBOS_CURSOR_SLEEP_TIME", "-1");
            video_socket_ensure();
            video_export_ensure();
            wl_webos_shell_surface_set_state(gl_webos, WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);
        }
    }
    log_msg("GL window %p: shell=%p webos=%p", (void *)child, (void *)gl_shell, (void *)gl_webos);
    pthread_mutex_unlock(&ov_mu);
}

/* attached is 1 for an attach request with a buffer, 0 for one without and
 * -1 for a commit. Returns 1 when the request was sent here. That happens
 * for the first content commit, followed by the fullscreen request. */
static int gl_window_commit(struct wl_proxy *surface, int attached)
{
    int sent = 0;

    pthread_mutex_lock(&ov_mu);
    if (gl_window && surface == gl_window) {
        if (attached >= 0) {
            gl_window_buffer = attached;
        } else if (gl_window_buffer && !gl_window_shown) {
            gl_window_shown = 1;
            ov_inside = 1;
            wl_surface_commit((struct wl_surface *)surface);
            if (gl_webos)
                wl_webos_shell_surface_set_state(gl_webos,
                                                 WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN);
            ov_inside = 0;
            log_msg("GL window %p shown", (void *)surface);
            sent = 1;
        } else {
            gl_window_buffer = 0;
        }
    }
    pthread_mutex_unlock(&ov_mu);
    return sent;
}

/* GL window mode, pop-ups Firefox draws with the GPU. A pop-up widget is
 * accelerated when its panel holds remote content. The popup notifications
 * panel with the add-on install prompt is one. Their Mali buffers cannot
 * be copied into the overlay, so such a content surface becomes a toplevel
 * of its own with its requests passed through. A webOS 6.5 TV imposes four
 * constraints. The window is typed FLOATING because an OVERLAY toplevel
 * mapped after the launch takes the keyboard focus, the card is hidden and
 * the app manager closes the app. It is never unmapped or destroyed
 * because either hides the card the same way. A hidden pop-up carries a
 * transparent 1x1 buffer, Firefox's destroy of the surface is swallowed,
 * and the next show re-attaches the surviving window to GTK's new pop-up
 * surface (gp_reparent). It has an empty input region, so the compositor
 * never picks it for a click. Clicks are injected from evdev because the
 * compositor sends no pointer events, and they hit the window where the
 * compositor shows it. That is the top left here rather than where the
 * pop-up asked to be. WEBOS_XDG_GPU_POPUP_PLACED=asked takes the clicks at
 * the asked place, for a compositor that honours it. */
#define GP_ITEMS 16
static struct gp_item {
    struct wl_proxy *surface;      /* Firefox's content surface, the toplevel */
    struct wl_proxy *popup;        /* GTK's pop-up surface, the one GTK knows */
    int32_t x, y;                  /* the pop-up's place in the window, as asked */
    int32_t off_x, off_y;          /* the content's offset in the pop-up */
    int32_t px, py;                /* where the compositor shows the window */
    int32_t w, h;
    int shown;                     /* a buffer of Firefox's is attached */
    int gone;                      /* destroyed by Firefox, kept hidden */
    int placed;                    /* px,py came from position_changed */
} gp_items[GP_ITEMS];
static struct wl_buffer *gp_blank;  /* 1x1 transparent, attached while hidden */

static struct gp_item *gp_find(struct wl_proxy *surface)
{
    int i;

    for (i = 0; surface && i < GP_ITEMS; i++)
        if (gp_items[i].surface == surface)
            return &gp_items[i];
    return NULL;
}

/* The pop-up's size, from its xdg_positioner. */
static void gp_popup_size(struct wl_proxy *popup, int32_t *w, int32_t *h)
{
    int i;

    *w = *h = 0;
    for (i = 0; popup && i < MAX_VIRT; i++)
        if (g.virts[i].proxy && g.virts[i].kind == V_POPUP && g.virts[i].wl_surface == popup) {
            *w = g.virts[i].p_w;
            *h = g.virts[i].p_h;
            return;
        }
}

/* With ov_mu held. */
static void gp_place(struct gp_item *it)
{
    int p = ov_popup_slot(it->popup);

    it->x = p >= 0 ? ov_popups[p].x : 0;
    it->y = p >= 0 ? ov_popups[p].y : 0;
    gp_popup_size(it->popup, &it->w, &it->h);
    if (!it->placed) {
        const char *placed = getenv("WEBOS_XDG_GPU_POPUP_PLACED");
        int asked = placed && !strcmp(placed, "asked");

        it->px = asked ? it->x + it->off_x : 0;
        it->py = asked ? it->y + it->off_y : 0;
    }
}

static void gp_webos_position(void *data, struct wl_webos_shell_surface *ws, int32_t x,
                              int32_t y)
{
    struct gp_item *it = data;

    (void)ws;
    log_trace("GPU pop-up %p: position_changed %d,%d (asked %d,%d)", (void *)it->surface, x, y,
            it->x + it->off_x, it->y + it->off_y);
    pthread_mutex_lock(&ov_mu);
    it->px = x;
    it->py = y;
    it->placed = 1;
    pthread_mutex_unlock(&ov_mu);
}

/* libwayland skips the events this leaves NULL. */
static const struct wl_webos_shell_surface_listener gp_webos_listener = {
    .position_changed = gp_webos_position,
};

/* With ov_mu held. */
static struct wl_buffer *gp_blank_ensure(void)
{
    struct wl_shm_pool *pool;
    int fd;

    if (gp_blank)
        return gp_blank;
    if (!ov_shm)        /* ov_shm is bound with the overlay, up before any pop-up */
        return NULL;
    fd = ov_memfd(4);
    if (fd < 0)
        return NULL;
    ov_inside = 1;
    pool = wl_shm_create_pool(ov_shm, fd, 4);
    gp_blank = wl_shm_pool_create_buffer(pool, 0, 1, 1, 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    ov_inside = 0;
    close(fd);
    return gp_blank;
}

/* A pop-up's content surface committed a buffer that is not shared memory
 * of this process, so it gets a window of its own. With ov_mu held. */
static void gp_promote(struct wl_proxy *surface, struct ov_item *ov)
{
    struct gp_item *it = NULL;
    struct wl_shell_surface *ss;
    struct wl_webos_shell_surface *ws;
    int i;

    for (i = 0; i < GP_ITEMS && !it; i++)
        if (!gp_items[i].surface)
            it = &gp_items[i];
    if (!it) {
        log_msg("GPU pop-up %p: table full, left to the overlay (not drawn)", (void *)surface);
        return;
    }
    memset(it, 0, sizeof *it);
    it->surface = surface;
    it->popup = ov->popup;
    it->off_x = ov->off_x;
    it->off_y = ov->off_y;
    it->shown = 1;
    ov_item_drop(ov);
    gp_place(it);
    bind_named();
    ov_inside = 1;
    ss = shell_role(surface);
    if (ss)
        wl_shell_surface_set_toplevel(ss);
    ws = g.webos ? wl_webos_shell_get_shell_surface(g.webos, (struct wl_surface *)surface) : NULL;
    if (ws) {
        wl_webos_shell_surface_add_listener(ws, &gp_webos_listener, it);
        wl_webos_shell_surface_set_property(ws, "appId", our_app_id());
        wl_webos_shell_surface_set_property(ws, "displayAffinity", "0");
        wl_webos_shell_surface_set_property(ws, "_WEBOS_WINDOW_TYPE",
                                            "_WEBOS_WINDOW_TYPE_FLOATING");
    }
    if (ov_compositor) {
        struct wl_region *none = wl_compositor_create_region(ov_compositor);

        wl_surface_set_input_region((struct wl_surface *)surface, none);
        wl_region_destroy(none);
    }
    ov_inside = 0;
    log_msg("GPU pop-up %p of %p: its own FLOATING window, asked %d,%d %dx%d, clicks taken "
            "at %d,%d (shell=%p webos=%p)",
            (void *)surface, (void *)it->popup, it->x + it->off_x, it->y + it->off_y, it->w,
            it->h, it->px, it->py, (void *)ss, (void *)ws);
}

/* wl_surface.attach on any surface, before the overlay sees it. A pop-up's
 * content surface is tagged at its first shared-memory buffer, or promoted
 * at its first GPU buffer. Firefox hides a pop-up by attaching nothing, and
 * a promoted surface then gets the transparent buffer instead. */
static void gp_attach(struct wl_proxy *surface, struct wl_proxy **buffer)
{
    struct gp_item *it;
    struct ov_item *ov;

    pthread_mutex_lock(&ov_mu);
    it = gp_find(surface);
    if (it) {
        if (*buffer) {
            it->shown = 1;
        } else {
            *buffer = (struct wl_proxy *)gp_blank_ensure();
            it->shown = 0;
            if (*buffer)
                log_msg("GPU pop-up %p hidden behind the transparent buffer", (void *)surface);
            else
                log_msg("GPU pop-up %p: no transparent buffer (no wl_shm), unmapped instead",
                        (void *)surface);
        }
        pthread_mutex_unlock(&ov_mu);
        return;
    }
    ov = ov_item_find(surface);
    if (ov && ov->popup != surface && *buffer) {
        int32_t w, h;

        gp_popup_size(ov->popup, &w, &h);
        if (ov_buf_find(*buffer)) {
            tag_child_surface(surface);
        } else if (ov_popup_slot(ov->popup) >= 0 && w > 0 && h > 0) {
            gp_promote(surface, ov);
        } else {
            static int told;

            /* A second GTK toplevel's content is one case. Its card is not
             * shown here, and a window of its own would hide the card. */
            if (told++ < 4)
                log_msg("GPU buffer on %p, a child of %p that is no pop-up: left alone",
                        (void *)surface, (void *)ov->popup);
        }
    }
    pthread_mutex_unlock(&ov_mu);
}

/* wl_surface.destroy on a GPU pop-up is swallowed and the surface stays
 * hidden. Returns 1 when the request must not go out. */
static int gp_destroy(struct wl_proxy *surface)
{
    struct gp_item *it;
    struct wl_buffer *blank;

    pthread_mutex_lock(&ov_mu);
    it = gp_find(surface);
    if (!it) {
        pthread_mutex_unlock(&ov_mu);
        return 0;
    }
    blank = gp_blank_ensure();
    ov_inside = 1;
    wl_surface_attach((struct wl_surface *)surface, blank, 0, 0);
    if (blank)
        wl_surface_damage((struct wl_surface *)surface, 0, 0, 1, 1);
    wl_surface_commit((struct wl_surface *)surface);
    ov_inside = 0;
    it->shown = 0;
    it->gone = 1;
    log_msg("GPU pop-up %p destroyed by Firefox: kept %s", (void *)surface,
            blank ? "behind the transparent buffer" : "unmapped");
    pthread_mutex_unlock(&ov_mu);
    return 1;
}

/* Firefox shows a pop-up again. Its content surface keeps its window and
 * moves to GTK's new pop-up surface. Returns 1 if it was one. */
static int gp_reparent(struct wl_proxy *child, struct wl_proxy *popup)
{
    struct gp_item *it;

    pthread_mutex_lock(&ov_mu);
    it = gp_find(child);
    if (it && !it->gone) {
        it->popup = popup;
        it->off_x = it->off_y = 0;
        gp_place(it);
        log_msg("GPU pop-up %p shown again in pop-up %p", (void *)child, (void *)popup);
    }
    pthread_mutex_unlock(&ov_mu);
    return it && !it->gone;
}

static int gp_offset(struct wl_proxy *child, int32_t x, int32_t y)
{
    struct gp_item *it;

    pthread_mutex_lock(&ov_mu);
    it = gp_find(child);
    if (it) {
        it->off_x = x;
        it->off_y = y;
        gp_place(it);
    }
    pthread_mutex_unlock(&ov_mu);
    return it != NULL;
}

/* With ov_mu held. */
static void gp_popup_moved(struct wl_proxy *popup)
{
    int i;

    for (i = 0; i < GP_ITEMS; i++)
        if (gp_items[i].surface && gp_items[i].popup == popup)
            gp_place(&gp_items[i]);
}

/* With ov_mu held. The xdg_popup is gone and Firefox hides the content next. */
static void gp_popup_gone(struct wl_proxy *popup)
{
    int i;

    for (i = 0; i < GP_ITEMS; i++)
        if (gp_items[i].surface && gp_items[i].popup == popup)
            gp_items[i].shown = 0;
}

/* The GTK pop-up whose GPU content is at x,y of the window, and its origin.
 * With ov_mu held. */
static struct wl_surface *gp_hit(int32_t x, int32_t y, int32_t *ox, int32_t *oy)
{
    struct wl_surface *hit = NULL;
    int i;

    for (i = 0; i < GP_ITEMS; i++) {
        struct gp_item *it = &gp_items[i];

        if (!it->surface || !it->shown || !it->popup)
            continue;
        if (x >= it->px && x < it->px + it->w && y >= it->py && y < it->py + it->h) {
            hit = (struct wl_surface *)it->popup;
            *ox = it->px - it->off_x;
            *oy = it->py - it->off_y;
        }
    }
    return hit;
}

/* Input the compositor sends to a GPU pop-up's surface belongs to GTK's
 * pop-up surface, offset by the content's place in it. */
static struct wl_proxy *gp_input_popup(struct wl_proxy *surface, int32_t *off_x, int32_t *off_y)
{
    struct gp_item *it;
    struct wl_proxy *popup = NULL;

    pthread_mutex_lock(&ov_mu);
    it = gp_find(surface);
    if (it) {
        popup = it->popup;
        if (off_x)
            *off_x = it->off_x;
        if (off_y)
            *off_y = it->off_y;
    }
    pthread_mutex_unlock(&ov_mu);
    return popup;
}

static struct wl_webos_foreign *gl_foreign_get(void)
{
    if (!gl_foreign && host_foreign_name && g.registry)
        gl_foreign = wl_registry_bind(g.registry, host_foreign_name, &wl_webos_foreign_interface,
                                      1);
    return gl_foreign;
}

static struct wl_compositor *gl_compositor_get(void)
{
    if (!gl_compositor && g.registry && host_compositor_name)
        gl_compositor = wl_registry_bind(g.registry, host_compositor_name,
                                         &wl_compositor_interface,
                                         server_compositor_version &&
                                                 server_compositor_version < 3
                                             ? server_compositor_version : 3);
    return gl_compositor;
}

static void gl_surface_destroyed(struct wl_proxy *surface)
{
    pthread_mutex_lock(&ov_mu);
    if (surface == gl_window) {
        if (gl_webos) {
            wl_webos_shell_surface_destroy(gl_webos);
            gl_webos = NULL;
        }
        gl_shell = NULL;
        gl_window = NULL;
        log_msg("GL window %p destroyed", (void *)surface);
    }
    if (surface == gl_main)
        gl_main = NULL;
    gl_pending_drop(surface);
    pthread_mutex_unlock(&ov_mu);
}

static void ov_surface_destroyed(struct wl_proxy *surface)
{
    struct ov_item *it;

    if (ov_is_popup(surface)) {
        ov_popup_gone(surface);
        return;
    }
    pthread_mutex_lock(&ov_mu);
    it = ov_item_find(surface);
    if (it) {
        ov_item_drop(it);
        ov_update();
    }
    pthread_mutex_unlock(&ov_mu);
}

static void ov_attach(struct wl_proxy *surface, struct wl_proxy *buffer)
{
    struct ov_item *it;

    pthread_mutex_lock(&ov_mu);
    it = ov_item_find(surface);
    if (it) {
        it->pending = buffer;
        it->has_pending = 1;
    }
    pthread_mutex_unlock(&ov_mu);
}

/* Copy the committed buffer: its memory can be reused once released. */
static void ov_commit(struct wl_proxy *surface)
{
    struct ov_item *it;
    struct ov_buf *b;
    int32_t row, col;
    int consumed = 0;

    pthread_mutex_lock(&ov_mu);
    it = ov_item_find(surface);
    if (!it) {
        pthread_mutex_unlock(&ov_mu);
        return;
    }
    if (it->has_pending) {
        it->current = it->pending;
        it->has_pending = 0;
        consumed = 1;
    }
    b = ov_buf_find(it->current);
    if (gl_mode() && surface == gl_main) {
        /* GTK's window is hidden from the compositor and the GL window shows
         * in its place. Its buffers only go back to the client. */
    } else if (!consumed && flat_mode()) {
        /* No new buffer: the surface still shows the picture copied when its
         * buffer was attached. That buffer went back to the client right
         * after the copy (flat_releases), and the client may be drawing its
         * next frame into it, so copying it again showed half-drawn and
         * stale frames: the flicker while scrolling on a webOS 6.5 TV
         * (GTK and Firefox commit without attaching to ask for a frame
         * callback or move a surface). */
    } else if (!it->current) {
        free(it->pix);
        it->pix = NULL;
        it->w = it->h = 0;
    } else if (b && b->pool->map && b->w > 0 && b->h > 0 && b->stride >= b->w * 4 &&
               (b->format == WL_SHM_FORMAT_ARGB8888 || b->format == WL_SHM_FORMAT_XRGB8888) &&
               (size_t)b->offset + (size_t)b->stride * (size_t)b->h <= b->pool->size) {
        if (it->w != b->w || it->h != b->h) {
            log_trace("pop-up surface %p draws %dx%d", (void *)surface, b->w, b->h);
            free(it->pix);
            it->pix = malloc((size_t)b->w * (size_t)b->h * 4);
            it->w = it->pix ? b->w : 0;
            it->h = it->pix ? b->h : 0;
        }
        for (row = 0; it->pix && row < b->h; row++) {
            const uint32_t *src =
                (const uint32_t *)((const char *)b->pool->map + b->offset + (size_t)row * b->stride);
            uint32_t *dst = it->pix + (size_t)row * b->w;

            if (b->format == WL_SHM_FORMAT_XRGB8888)
                for (col = 0; col < b->w; col++)
                    dst[col] = src[col] | 0xff000000u;
            else
                memcpy(dst, src, (size_t)b->w * 4);
        }
        flat_frames_copied++;
    } else if (flat_mode()) {
        static struct wl_proxy *told[8];
        static int ntold;
        int k;

        for (k = 0; k < ntold && told[k] != surface; k++)
            ;
        if (k == ntold && ntold < 8) {
            told[ntold++] = surface;
            log_msg("flat: surface %p committed buffer %p the window cannot copy (%s)",
                    (void *)surface, (void *)it->current,
                    b ? "size or format" : "not a wl_shm buffer of this process");
        }
    }
    /* Flat mode: copied, or of no use to anyone, and the compositor never
     * gets it, so the client gets it back. One release per attach. */
    if (consumed && it->current && flat_mode())
        flat_pending_add(flat_releases, &flat_releases_n, it->current);
    ov_update();
    pthread_mutex_unlock(&ov_mu);
}

/* Requests the overlay needs to see; returns 1 if it sent the request itself. */
static int ov_intercept(struct wl_proxy *proxy, const char *name, uint32_t opcode,
                        const struct wl_interface *interface, uint32_t version, uint32_t flags,
                        union wl_argument *args, struct wl_proxy **out)
{
    struct wl_proxy *created;

    if (ov_inside || !args || !ov_enabled())
        return 0;
    if (!strcmp(name, "wl_shm") && opcode == WL_SHM_CREATE_POOL) {
        int fd = args[1].h;
        int32_t size = args[2].i;

        ov_inside = 1;
        created = wl_proxy_marshal_array_flags(proxy, opcode, interface, version, flags, args);
        ov_inside = 0;
        pthread_mutex_lock(&ov_mu);
        ov_pool_add(created, fd, size);
        pthread_mutex_unlock(&ov_mu);
        *out = created;
        return 1;
    }
    if (!strcmp(name, "wl_shm_pool")) {
        pthread_mutex_lock(&ov_mu);
        if (opcode == WL_SHM_POOL_CREATE_BUFFER && ov_pool_find(proxy)) {
            ov_inside = 1;
            created = wl_proxy_marshal_array_flags(proxy, opcode, interface, version, flags, args);
            ov_inside = 0;
            ov_buf_add(created, proxy, args);
            pthread_mutex_unlock(&ov_mu);
            *out = created;
            return 1;
        }
        if (opcode == WL_SHM_POOL_RESIZE)
            ov_pool_resize(proxy, args[0].i);
        pthread_mutex_unlock(&ov_mu);
        return 0;
    }
    /* Firefox marks the chrome's opaque parts as the window's opaque
     * region. The compositor would then skip blending there and the video
     * hole (alpha 0 pixels over the hardware plane) would come out black,
     * so the GL window declares no opaque region at all. */
    if (!strcmp(name, "wl_surface") && opcode == WL_SURFACE_SET_OPAQUE_REGION && args[0].o &&
        gl_mode() && proxy == gl_window) {
        static int told;

        if (!told++)
            log_msg("GL window: opaque region dropped, the window stays blendable");
        args[0].o = NULL;
        return 0;
    }
    /* Flat mode: the adapter answers frame callbacks itself (flat_sync_done);
     * the copied surfaces are never shown, and a webOS 6.5 TV answers not even
     * the window's. The window is made now if need be and a frame of it
     * committed, so the client's picture goes out before the answer. */
    if (!strcmp(name, "wl_surface") && opcode == WL_SURFACE_SET_INPUT_REGION && gl_mode() &&
        gp_input_popup(proxy, NULL, NULL)) {
        *out = NULL;    /* the window keeps its empty input region */
        return 1;
    }
    if (!strcmp(name, "wl_surface") && opcode == WL_SURFACE_FRAME && flat_mode() &&
        proxy != (struct wl_proxy *)ov_surface && !(gl_mode() && proxy == gl_window) &&
        !(gl_mode() && gp_input_popup(proxy, NULL, NULL))) {
        created = new_virt(proxy, interface ? interface : &wl_callback_interface, 1, V_FRAME,
                           NULL);
        if (created) {
            pthread_mutex_lock(&ov_mu);
            ov_inside = 1;
            ov_ensure();
            ov_inside = 0;
            ov_dirty = 1;
            flat_pending_add(flat_frames, &flat_frames_n, created);
            pthread_mutex_unlock(&ov_mu);
            flat_pump_start();
            *out = created;
            return 1;
        }
    }
    if (!strcmp(name, "wl_surface") && opcode == WL_SURFACE_ATTACH) {
        if (gl_mode())
            gp_attach(proxy, (struct wl_proxy **)&args[0].o);
        ov_attach(proxy, (struct wl_proxy *)args[0].o);
        if (gl_mode() && gl_window_commit(proxy, args[0].o != NULL)) {
            *out = NULL;
            return 1;
        }
        if (flat_hidden(proxy)) {
            *out = NULL;
            return 1;
        }
    } else if (!strcmp(name, "wl_surface") &&
               (opcode == WL_SURFACE_DAMAGE || opcode == WL_SURFACE_DAMAGE_BUFFER)) {
        if (flat_hidden(proxy)) {
            *out = NULL;
            return 1;
        }
    } else if (!strcmp(name, "wl_surface") && opcode == WL_SURFACE_COMMIT) {
        layered_commit(proxy);
        ov_commit(proxy);
        if (gl_mode() && gl_window_commit(proxy, -1)) {
            *out = NULL;
            return 1;
        }
        if (flat_hidden(proxy)) {
            *out = NULL;
            return 1;
        }
    }
    return 0;
}

/* Destructors carry no arguments, so they are handled apart. */
static void ov_destroying(struct wl_proxy *proxy, const char *name, uint32_t opcode)
{
    if (ov_inside || !ov_enabled())
        return;
    pthread_mutex_lock(&ov_mu);
    if (!strcmp(name, "wl_shm_pool") && opcode == WL_SHM_POOL_DESTROY)
        ov_pool_destroyed(proxy);
    else if (!strcmp(name, "wl_buffer") && opcode == WL_BUFFER_DESTROY) {
        flat_forget(proxy);
        ov_buf_destroyed(proxy);
    }
    pthread_mutex_unlock(&ov_mu);
}

/* The pop-up under x,y of the main window, and its origin. */
static struct wl_surface *ov_hit(int32_t x, int32_t y, int32_t *ox, int32_t *oy)
{
    struct wl_surface *hit = NULL;
    unsigned top = 0;
    int i;

    pthread_mutex_lock(&ov_mu);
    hit = gp_hit(x, y, ox, oy);
    for (i = 0; i < OV_ITEMS; i++) {
        struct ov_item *it = &ov_items[i];
        int p = it->surface ? ov_popup_slot(it->popup) : -1;
        int32_t x0, y0;

        if (p < 0 || !it->pix || ov_popups[p].seq < top)
            continue;
        x0 = ov_popups[p].x + it->off_x;
        y0 = ov_popups[p].y + it->off_y;
        if (x >= x0 && x < x0 + it->w && y >= y0 && y < y0 + it->h) {
            hit = (struct wl_surface *)it->popup;   /* the newest pop-up is on top */
            top = ov_popups[p].seq;
            *ox = ov_popups[p].x;
            *oy = ov_popups[p].y;
        }
    }
    pthread_mutex_unlock(&ov_mu);
    return hit;
}

static struct wl_proxy *virt_request(struct virt *v, uint32_t opcode, uint32_t flags,
                                     const struct wl_interface *interface,
                                     union wl_argument *args)
{
    struct wl_proxy *created;
    struct virt *child;

    /* Stand-ins for missing globals. Their destructors are not opcode 0
     * (wl_data_source.offer is), so they skip the generic handling below. */
    if (v->kind == V_DATA_DEVICE_MANAGER || v->kind == V_DATA_SOURCE ||
        v->kind == V_DATA_DEVICE || v->kind == V_SUBCOMPOSITOR || v->kind == V_SUBSURFACE) {
        int destructor = 0;

        if (v->kind == V_DATA_DEVICE_MANAGER) {
            if (opcode == WL_DATA_DEVICE_MANAGER_CREATE_DATA_SOURCE)
                return new_virt(v->proxy, interface, v->version, V_DATA_SOURCE, NULL);
            if (opcode == WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE)
                return new_virt(v->proxy, interface, v->version, V_DATA_DEVICE, NULL);
        } else if (v->kind == V_DATA_SOURCE) {
            destructor = opcode == WL_DATA_SOURCE_DESTROY;
        } else if (v->kind == V_DATA_DEVICE) {
            destructor = opcode == WL_DATA_DEVICE_RELEASE;
        } else if (v->kind == V_SUBCOMPOSITOR) {
            if (opcode == WL_SUBCOMPOSITOR_GET_SUBSURFACE && args) {
                created = new_virt(v->proxy, interface, v->version, V_SUBSURFACE, &child);
                if (child) {
                    child->sub_child = (struct wl_proxy *)args[1].o;
                    child->sub_parent = (struct wl_proxy *)args[2].o;
                    emulate_subsurface(child->sub_child, child->sub_parent);
                }
                return created;
            }
            destructor = opcode == WL_SUBCOMPOSITOR_DESTROY;
        } else if (v->kind == V_SUBSURFACE) {
            if (opcode == WL_SUBSURFACE_SET_POSITION && args)
                layered_position(v->sub_child, args[0].i, args[1].i);
            if (opcode == WL_SUBSURFACE_DESTROY) {
                unlayer_surface(v->sub_child);
                destructor = 1;
            }
        }
        if (destructor && v->proxy) {
            struct wl_proxy *dying = v->proxy;

            virt_teardown(v);
            if (flags & WL_MARSHAL_FLAG_DESTROY)
                webos_xdg_destroy_virtual(dying);
        }
        return NULL;
    }

    if (v->kind == V_WM && opcode == XDG_WM_BASE_GET_XDG_SURFACE) {
        created = new_virt(v->proxy, interface, v->version, V_SURFACE, &child);
        if (child && args)
            child->wl_surface = (struct wl_proxy *)args[1].o;
        return created;
    }
    if (v->kind == V_WM && opcode == XDG_WM_BASE_CREATE_POSITIONER) {
        return new_virt(v->proxy, interface, v->version, V_POSITIONER, NULL);
    }
    if (v->kind == V_TEXT_INPUT_MANAGER && opcode == ZWP_TEXT_INPUT_MANAGER_V3_GET_TEXT_INPUT) {
        created = new_virt(v->proxy, interface, v->version, V_TEXT_INPUT, NULL);
        text_input_v3 = created;
        text_input_entered = 0;
        log_msg("text-input created");
        return created;
    }
    if (v->kind == V_TEXT_INPUT) {
        if (opcode == ZWP_TEXT_INPUT_V3_ENABLE)
            text_input_pending = 1;
        else if (opcode == ZWP_TEXT_INPUT_V3_DISABLE)
            text_input_pending = 0;
        else if (opcode == ZWP_TEXT_INPUT_V3_COMMIT)
            text_input_commit(v);
        else if (opcode == 0 && v->proxy == text_input_v3)
            text_input_v3 = NULL;
    }
    if (v->kind == V_POSITIONER && args) {
        switch (opcode) {
        case XDG_POSITIONER_SET_SIZE:
            v->p_w = args[0].i; v->p_h = args[1].i; break;
        case XDG_POSITIONER_SET_ANCHOR_RECT:
            v->p_ax = args[0].i; v->p_ay = args[1].i;
            v->p_aw = args[2].i; v->p_ah = args[3].i; break;
        case XDG_POSITIONER_SET_ANCHOR:
            v->p_anchor = args[0].u; break;
        case XDG_POSITIONER_SET_GRAVITY:
            v->p_gravity = args[0].u; break;
        case XDG_POSITIONER_SET_OFFSET:
            v->p_ox = args[0].i; v->p_oy = args[1].i; break;
        default:
            break;
        }
    }
    if (v->kind == V_SURFACE && opcode == XDG_SURFACE_GET_TOPLEVEL) {
        /* Only a toplevel takes the card role. A popup surface gets no
         * shell surface at all, so LSM cannot mistake it for a card. */
        map_main_surface(v, v->wl_surface);
        created = new_virt(v->proxy, interface, v->version, V_TOPLEVEL, &child);
        if (child)
            child->wl_surface = v->wl_surface;
        return created;
    }
    if (v->kind == V_SURFACE && opcode == XDG_SURFACE_GET_POPUP) {
        struct virt *parent = args ? virt_get((struct wl_proxy *)args[1].o) : NULL;
        struct virt *pos = args ? virt_get((struct wl_proxy *)args[2].o) : NULL;

        created = new_virt(v->proxy, interface, v->version, V_POPUP, &child);
        /* webOS 6 makes any untagged surface with content its fullscreen
         * card, which takes the flat window off the screen. */
        if (flat_mode() && v->wl_surface)
            tag_child_surface(v->wl_surface);
        if (child) {
            child->wl_surface = v->wl_surface;
            if (pos)
                place_popup(child, pos);
            {
                int i;
                int32_t ox = 0, oy = 0;
                /* A submenu's parent is another popup, so add its offset. */
                for (i = 0; parent && i < MAX_VIRT; i++) {
                    if (g.virts[i].proxy && g.virts[i].kind == V_POPUP &&
                        g.virts[i].wl_surface == parent->wl_surface) {
                        ox = g.virts[i].abs_x;
                        oy = g.virts[i].abs_y;
                        break;
                    }
                }
                child->abs_x = ox + child->popup_x;
                child->abs_y = oy + child->popup_y;
            }
            bind_named();
            if (our_subcompositor && parent && parent->wl_surface && v->wl_surface) {
                child->subsurface = wl_subcompositor_get_subsurface(
                    our_subcompositor,
                    (struct wl_surface *)v->wl_surface,
                    (struct wl_surface *)parent->wl_surface);
                if (child->subsurface) {
                    wl_subsurface_set_position(child->subsurface,
                                               child->popup_x, child->popup_y);
                    wl_subsurface_set_desync(child->subsurface);
                }
            } else if (!our_subcompositor && ov_enabled() && v->wl_surface) {
                ov_popup_at(v->wl_surface, child->abs_x, child->abs_y);
            }
            log_trace("popup surface=%p sub=%p at %d,%d %dx%d parent=%p",
                    (void *)child->wl_surface,
                    (void *)child->subsurface, child->popup_x, child->popup_y,
                    child->p_w, child->p_h, (void *)(parent ? parent->wl_surface : NULL));
        }
        return created;
    }
    if (v->kind == V_SURFACE && opcode == XDG_SURFACE_SET_WINDOW_GEOMETRY && args &&
        v->wl_surface == (struct wl_proxy *)main_surface &&
        (args[2].i != geo_w || args[3].i != geo_h) && args[2].i > 0 && args[3].i > 0) {
        geo_w = args[2].i;
        geo_h = args[3].i;
        log_msg("main window is %dx%d", geo_w, geo_h);
    }
    if (v->kind == V_POPUP && opcode == XDG_POPUP_REPOSITION && args) {
        struct virt *pos = virt_get((struct wl_proxy *)args[0].o);
        if (pos) {
            int32_t old_x = v->popup_x, old_y = v->popup_y;

            place_popup(v, pos);
            v->abs_x += v->popup_x - old_x;
            v->abs_y += v->popup_y - old_y;
            if (!v->subsurface && ov_is_popup(v->wl_surface)) {
                ov_popup_at(v->wl_surface, v->abs_x, v->abs_y);
            }
            if (v->subsurface)
                wl_subsurface_set_position(v->subsurface, v->popup_x, v->popup_y);

            v->configured = 0;
            emit_popup_configure(v);
        }
        return NULL;
    }
    if (v->kind == V_TOPLEVEL && opcode == XDG_TOPLEVEL_SET_TITLE && args && args[0].s) {
        int i;
        log_msg("toplevel %p (surface %p) titled \"%s\"", (void *)v->proxy, (void *)v->wl_surface,
                args[0].s);
        if (gl_webos)
            wl_webos_shell_surface_set_property(gl_webos, "title", args[0].s);
        for (i = 0; i < MAX_VIRT; i++) {
            if (g.virts[i].kind == V_SURFACE && g.virts[i].wl_surface == v->wl_surface &&
                g.virts[i].webos_surface) {
                wl_webos_shell_surface_set_property(g.virts[i].webos_surface, "title", args[0].s);
                break;
            }
        }
    }
    if (v->kind == V_TOPLEVEL) {
        switch (opcode) {
        case XDG_TOPLEVEL_SET_FULLSCREEN:
        case XDG_TOPLEVEL_UNSET_FULLSCREEN:
            v->fullscreen = opcode == XDG_TOPLEVEL_SET_FULLSCREEN;
            /* fallthrough */
        case XDG_TOPLEVEL_SET_MAXIMIZED:
        case XDG_TOPLEVEL_UNSET_MAXIMIZED:
            emit_toplevel_configure(v);
            break;
        default:
            break;
        }
    }
    if (opcode == 0 && v->proxy) {
        struct wl_proxy *dying = v->proxy;

        if (v->kind == V_POPUP && v->wl_surface)
            ov_popup_gone(v->wl_surface);

        virt_teardown(v);
        /* Code generated by wayland-scanner 1.20 and later destroys in the
         * same call (WL_MARSHAL_FLAG_DESTROY). Older code, such as Debian
         * 11's GTK, sends the request and then calls wl_proxy_destroy(),
         * which frees the proxy in webos_xdg_virtual_destroyed(). */
        if (flags & WL_MARSHAL_FLAG_DESTROY)
            webos_xdg_destroy_virtual(dying);
    }
    return NULL;
}

/* Called by our libwayland for wl_proxy_destroy() on a proxy with id 0,
 * which only virtual proxies have. */
void webos_xdg_virtual_destroyed(struct wl_proxy *proxy)
{
    struct virt *v = virt_get(proxy);

    if (v && v->proxy == proxy) {
        if (v->kind == V_FRAME) {
            pthread_mutex_lock(&ov_mu);
            flat_forget(proxy);
            pthread_mutex_unlock(&ov_mu);
        }
        virt_teardown(v);
    }
    webos_xdg_destroy_virtual(proxy);
}

/* Testing aid: WEBOS_XDG_AS_WEBOS4=1 hides the globals webOS 4's compositor
 * lacks, so the stand-ins below can be tried on a newer TV. */
static int as_webos4(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("WEBOS_XDG_AS_WEBOS4");

        on = e && *e == '1';
        if (on)
            log_msg("acting as on webOS 4: no wl_subcompositor, no wl_data_device_manager");
    }
    return on;
}

static void wrapped_global(void *data, struct wl_registry *registry, uint32_t name,
                           const char *interface, uint32_t version)
{
    struct virt *reg = virt_get((struct wl_proxy *)registry);
    void (*user_global)(void *, struct wl_registry *, uint32_t, const char *, uint32_t);
    uint32_t client_version = version;
    static int outputs;

    user_global = (reg && reg->listener)
        ? ((void (**)(void *, struct wl_registry *, uint32_t, const char *, uint32_t))reg->listener)[0]
        : NULL;
    /* List what this compositor offers once, from the first registry: which
     * globals exist differs by webOS version (webOS 4 lacks wl_subcompositor
     * and wl_seat, which Firefox and GTK need). */
    {
        /* By name: registry objects are reused after Firefox and the Mali
         * driver destroy their temporary ones, so "first registry" is not
         * a reliable test. */
        static char seen[48][40];
        static int nseen;
        int i;

        for (i = 0; i < nseen && strcmp(seen[i], interface) != 0; i++)
            ;
        if (i == nseen && nseen < 48) {
            snprintf(seen[nseen++], sizeof seen[0], "%s", interface);
            log_msg("compositor offers %s v%u (name %u)", interface, version, name);
        }
    }
    if (as_webos4() && (!strcmp(interface, "wl_subcompositor") ||
                        !strcmp(interface, "wl_data_device_manager")))
        return;
    if (!strcmp(interface, "wl_shell"))
        host_shell_name = name;
    if (!strcmp(interface, "wl_webos_shell"))
        host_webos_name = name;
    if (!strcmp(interface, "wl_webos_input_manager"))
        host_input_name = name;
    if (!strcmp(interface, "text_model_factory"))
        host_text_name = name;
    if (!strcmp(interface, "wl_seat")) {
        int i, free_slot = -1;

        for (i = 0; i < MAX_SEATS && seats[i].name != name; i++)
            if (!seats[i].name && free_slot < 0)
                free_slot = i;
        if (i == MAX_SEATS && free_slot >= 0) {
            seats[free_slot].name = name;
            seats[free_slot].version = version;
            log_msg("seat added: wl_seat v%u (name %u)", version, name);
        }
        if (!host_seat_name) {
            host_seat_name = name;
            host_seat_version = version;
        }
    }
    if (!strcmp(interface, "wl_shm") && !host_shm_name)
        host_shm_name = name;
    if (!strcmp(interface, "wl_subcompositor") && !host_subcompositor_name)
        host_subcompositor_name = name;
    if (!strcmp(interface, "wl_data_device_manager") && !host_data_device_manager_name)
        host_data_device_manager_name = name;
    if (!strcmp(interface, "wl_webos_surface_group_compositor") && !host_group_name)
        host_group_name = name;
    if (!strcmp(interface, "wl_webos_foreign") && !host_foreign_name)
        host_foreign_name = name;
    /* GTK sets up no seat until wl_data_device_manager exists; webOS 4 has
     * none, so without this GTK has no keyboard or pointer at all. */
    if (!strcmp(interface, "wl_seat") && !host_data_device_manager_name && user_global &&
        reg && !reg->injected_ddm) {
        reg->injected_ddm = 1;
        log_msg("inject wl_data_device_manager (the compositor has none)");
        user_global(data, registry, DATA_DEVICE_MANAGER_NAME, "wl_data_device_manager", 3);
    }
    /* Firefox refuses to start without wl_subcompositor. Compositors list it
     * right after wl_compositor; if the next global is something else, this
     * one has none (webOS 4). */
    if (strcmp(interface, "wl_compositor") && strcmp(interface, "wl_subcompositor") &&
        !host_subcompositor_name && user_global && reg && reg->injected && !reg->injected_sub) {
        reg->injected_sub = 1;
        log_msg("inject wl_subcompositor (the compositor has none)");
        user_global(data, registry, SUBCOMPOSITOR_NAME, "wl_subcompositor", 1);
    }
    if (!strcmp(interface, "wl_compositor")) {
        if (!host_compositor_name)
            host_compositor_name = name;
        server_compositor_version = version;
        if (client_version < 4)
            client_version = 4;
        log_msg("global wl_compositor server=%u client=%u", version, client_version);
    }
    if (!strcmp(interface, "wl_output")) {
        server_output_version = version;
        if (client_version < 3)
            client_version = 3;
        log_msg("global wl_output server=%u client=%u", version, client_version);
    }
    if (!strcmp(interface, "wl_output") && outputs++ > 0)
        return;
    if (user_global)
        user_global(data, registry, name, interface, client_version);
    if (!strcmp(interface, "wl_compositor") && user_global && (!reg || !reg->injected)) {
        if (reg)
            reg->injected = 1;
        g.xdg_name = 0x7f000001;
        log_msg("inject xdg_wm_base");
        user_global(data, registry, g.xdg_name, "xdg_wm_base", 4);
        log_msg("inject zwp_text_input_manager_v3");
        user_global(data, registry, TEXT_INPUT_NAME, "zwp_text_input_manager_v3", 1);
    }
}

static void wrapped_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    struct virt *reg = virt_get((struct wl_proxy *)registry);
    void (*user_remove)(void *, struct wl_registry *, uint32_t);
    int i;

    for (i = 0; i < MAX_SEATS; i++) {
        if (seats[i].name != name)
            continue;
        seats[i].name = 0;
        log_msg("seat removed: wl_seat (name %u)", name);
        if (name == host_seat_name) {
            /* Our binding of it is dead on the compositor's side: naming it
             * in a request is a fatal "invalid object" error. Rebind to
             * another seat the next time the keyboard is needed. */
            if (our_seat)
                wl_proxy_destroy((struct wl_proxy *)our_seat);
            our_seat = NULL;
            host_seat_name = 0;
            /* The text model was activated on that seat and is gone with it. */
            if (text_input) {
                wl_proxy_destroy((struct wl_proxy *)text_input);
                text_input = NULL;
            }
            keyboard_closed();
            for (i = 0; i < MAX_SEATS; i++)
                if (seats[i].name) {
                    host_seat_name = seats[i].name;
                    host_seat_version = seats[i].version;
                    break;
                }
            log_msg("keyboard seat now name %u", host_seat_name);
        }
        break;
    }

    user_remove = (reg && reg->listener)
        ? ((void (**)(void *, struct wl_registry *, uint32_t))reg->listener)[1]
        : NULL;
    if (user_remove)
        user_remove(data, registry, name);
}

static const struct wl_registry_listener registry_wrapper = {
    wrapped_global,
    wrapped_remove,
};

static struct hook *hook_slot(struct hook *table, struct wl_proxy *proxy)
{
    int i;
    struct hook *free_slot = NULL;

    for (i = 0; i < MAX_HOOK; i++) {
        if (table[i].proxy == proxy) {
            return &table[i];
        }
        if (!table[i].proxy && !free_slot)
            free_slot = &table[i];
    }
    return free_slot;
}

static struct hook *hook_find(struct hook *table, const void *proxy)
{
    int i;

    for (i = 0; i < MAX_HOOK; i++) {
        if (table[i].proxy == proxy)
            return &table[i];
    }
    return NULL;
}

/* US keyboard evdev codes. The TV keymap is evdev/us, so Firefox turns
 * these plus the shift modifier into the typed character. */
static uint32_t letter_code(char lower)
{
    const char *row1 = "qwertyuiop";
    const char *row2 = "asdfghjkl";
    const char *row3 = "zxcvbnm";
    const char *found;

    found = strchr(row1, lower);
    if (found)
        return 16 + (uint32_t)(found - row1);
    found = strchr(row2, lower);
    if (found)
        return 30 + (uint32_t)(found - row2);
    found = strchr(row3, lower);
    if (found)
        return 44 + (uint32_t)(found - row3);
    return 0;
}

static int ascii_key(char ch, uint32_t *code, int *shift)
{
    *shift = 0;
    if (ch >= 'a' && ch <= 'z') {
        *code = letter_code(ch);
        return *code != 0;
    }
    if (ch >= 'A' && ch <= 'Z') {
        *code = letter_code((char)(ch - 'A' + 'a'));
        *shift = 1;
        return *code != 0;
    }
    if (ch >= '1' && ch <= '9') {
        *code = 2 + (uint32_t)(ch - '1');
        return 1;
    }
    switch (ch) {
    case '0': *code = 11; return 1;
    case '-': *code = 12; return 1;
    case '_': *code = 12; *shift = 1; return 1;
    case '=': *code = 13; return 1;
    case '+': *code = 13; *shift = 1; return 1;
    case '\n':
    case '\r': *code = 28; return 1;
    case '\b': *code = 14; return 1;
    case '\t': *code = 15; return 1;
    case '[': *code = 26; return 1;
    case '{': *code = 26; *shift = 1; return 1;
    case ']': *code = 27; return 1;
    case '}': *code = 27; *shift = 1; return 1;
    case ';': *code = 39; return 1;
    case ':': *code = 39; *shift = 1; return 1;
    case '\'': *code = 40; return 1;
    case '"': *code = 40; *shift = 1; return 1;
    case '`': *code = 41; return 1;
    case '~': *code = 41; *shift = 1; return 1;
    case '\\': *code = 43; return 1;
    case '|': *code = 43; *shift = 1; return 1;
    case ',': *code = 51; return 1;
    case '<': *code = 51; *shift = 1; return 1;
    case '.': *code = 52; return 1;
    case '>': *code = 52; *shift = 1; return 1;
    case '/': *code = 53; return 1;
    case '?': *code = 53; *shift = 1; return 1;
    case ' ': *code = 57; return 1;
    case '!': *code = 2; *shift = 1; return 1;
    case '@': *code = 3; *shift = 1; return 1;
    case '#': *code = 4; *shift = 1; return 1;
    case '$': *code = 5; *shift = 1; return 1;
    case '%': *code = 6; *shift = 1; return 1;
    case '^': *code = 7; *shift = 1; return 1;
    case '&': *code = 8; *shift = 1; return 1;
    case '*': *code = 9; *shift = 1; return 1;
    case '(': *code = 10; *shift = 1; return 1;
    case ')': *code = 11; *shift = 1; return 1;
    default: return 0;
    }
}

/* Firefox binds a wl_keyboard of its own next to GDK's, with a key handler
 * that does nothing, and the one entered last may be either. */
static struct hook *typing_keys(void)
{
    return gdk_keys ? gdk_keys : focused_keys;
}

/* mods is the xkb depressed modifier mask. 1 is Shift and 4 is Control. */
static void inject_key(uint32_t code, uint32_t mods)
{
    const struct wl_keyboard_listener *keys;
    struct hook *to = typing_keys();
    uint32_t serial;

    if (!to || !to->listener) {
        log_trace("key %u dropped: no keyboard to type into", code);
        return;
    }
    keys = to->listener;
    if (!keys->key)
        return;
    serial = ++g.serial;
    if (mods && keys->modifiers)
        keys->modifiers(to->data, (struct wl_keyboard *)to->proxy, serial, mods, 0, 0, 0);
    keys->key(to->data, (struct wl_keyboard *)to->proxy, serial, 0, code,
              WL_KEYBOARD_KEY_STATE_PRESSED);
    keys->key(to->data, (struct wl_keyboard *)to->proxy, serial, 0, code,
              WL_KEYBOARD_KEY_STATE_RELEASED);
    if (mods && keys->modifiers)
        keys->modifiers(to->data, (struct wl_keyboard *)to->proxy, serial, 0, 0, 0, 0);
}

static void inject_text(const char *text)
{
    const char *p;

    if (!text)
        return;
    log_trace("type \"%s\"", text);
    for (p = text; *p; p++) {
        uint32_t code;
        int shift;
        if (ascii_key(*p, &code, &shift))
            inject_key(code, shift);
    }
}

static void focus_address_bar(void)
{
    /* Control+L, evdev 38, selects the URL. */
    inject_key(38, 4);
    log_msg("focus address bar");
}

/* text_model is version 1 everywhere, but webOS 6 orders its requests
 * differently from text-model.xml (LG's 6.0 libwayland-webos-client):
 * invoke_action 6, commit 7, show_input_panel 8, hide_input_panel 9,
 * set_max_text_length 10, set_platform_data 11, set_enter_key_type 12.
 * A request sent by the other numbering is a fatal "invalid arguments", and
 * libwayland encodes a request from the proxy's own interface, so on webOS 6
 * the text model is created with this table. The first six requests and all
 * events agree. The layout goes with flat mode (webOS 6);
 * WEBOS_XDG_TEXT_MODEL=4 or 6 overrides. */
static const struct wl_interface *tm6_activate_types[] = { NULL, &wl_seat_interface,
                                                           &wl_surface_interface };
static const struct wl_interface *tm6_deactivate_types[] = { &wl_seat_interface };
static const struct wl_interface *tm6_none[] = { NULL, NULL, NULL, NULL };
static const struct wl_message tm6_requests[] = {
    { "set_surrounding_text", "suu", tm6_none },
    { "activate", "uoo", tm6_activate_types },
    { "deactivate", "o", tm6_deactivate_types },
    { "reset", "u", tm6_none },
    { "set_cursor_rectangle", "iiii", tm6_none },
    { "set_content_type", "uu", tm6_none },
    { "invoke_action", "uu", tm6_none },
    { "commit", "", tm6_none },
    { "show_input_panel", "", tm6_none },
    { "hide_input_panel", "", tm6_none },
    { "set_max_text_length", "u", tm6_none },
    { "set_platform_data", "s", tm6_none },
    { "set_enter_key_type", "u", tm6_none },
    { "set_input_panel_rect", "iiuu", tm6_none },
    { "reset_input_panel_rect", "", tm6_none },
};
static int tm_six;      /* the text model uses the webOS 6 table */
/* Only requests 0-5 (set_surrounding_text .. set_content_type), the ones
 * every layout numbers alike; set_enter_key_type, show_input_panel and
 * hide_input_panel are not sent. A webOS 6.5 TV (6.5.3-47) numbers its
 * requests like text-model.xml, not like LG's 6.0 emulator: our hide (9 in
 * the 6.0 table) reached it as set_max_text_length without its argument, a
 * fatal "invalid arguments" that took Firefox down whenever a text field
 * lost focus. activate and deactivate show and hide webOS's keyboard on
 * their own. The default in flat mode, as no layout can be told apart from
 * the client side; WEBOS_XDG_TEXT_MODEL=4|6 sends the full table again. */
static int tm_shared_only;

static int text_model_layout_six(void)
{
    const char *e = getenv("WEBOS_XDG_TEXT_MODEL");

    if (e && e[0]) {
        tm_shared_only = 0;
        return atoi(e) == 6;
    }
    tm_shared_only = flat_mode();
    return flat_mode();
}

static struct text_model *create_text_model(struct text_model_factory *factory)
{
    static struct wl_interface tm6;

    tm_six = text_model_layout_six();
    if (!tm_six)
        return text_model_factory_create_text_model(factory);
    if (!tm6.name) {
        tm6 = text_model_interface;
        tm6.method_count = sizeof tm6_requests / sizeof tm6_requests[0];
        tm6.methods = tm6_requests;
    }
    return (struct text_model *)wl_proxy_marshal_flags(
        (struct wl_proxy *)factory, TEXT_MODEL_FACTORY_CREATE_TEXT_MODEL, &tm6,
        wl_proxy_get_version((struct wl_proxy *)factory), 0, NULL);
}

enum { TM_ENTER_KEY_TYPE, TM_SHOW_PANEL, TM_HIDE_PANEL };

static void text_model_send(int which, uint32_t arg)
{
    static const uint32_t webos4[] = { 6, 11, 12 }, webos6[] = { 12, 8, 9 };
    uint32_t opcode = (tm_six ? webos6 : webos4)[which];
    struct wl_proxy *p = (struct wl_proxy *)text_input;

    if (tm_shared_only)
        return;

    if (which == TM_ENTER_KEY_TYPE)
        wl_proxy_marshal_flags(p, opcode, NULL, wl_proxy_get_version(p), 0, arg);
    else
        wl_proxy_marshal_flags(p, opcode, NULL, wl_proxy_get_version(p), 0);
}

/* The surface the compositor last gave the keyboard to, as it named it. */
static struct wl_surface *keyboard_focus_raw;

static void show_keyboard(void)
{
    struct wl_surface *target;

    ensure_text_model();
    if (!text_input || !our_seat || !main_surface)
        return;
    /* The panel comes up only for the surface that has the keyboard. In GL
     * mode that is the pop-up overlay once it has mapped, not the window. */
    if (gl_mode() && keyboard_focus_raw &&
        (keyboard_focus_raw == ov_surface || keyboard_focus_raw == (struct wl_surface *)gl_window))
        target = keyboard_focus_raw;
    else if (gl_mode() && gl_window)
        target = (struct wl_surface *)gl_window;
    else if (flat_mode() && ov_surface)
        target = ov_surface;
    else
        target = main_surface;
    text_model_set_content_type(text_input, 0x100, 5);
    text_model_send(TM_ENTER_KEY_TYPE, 3);
    text_model_set_surrounding_text(text_input, "", 0, 0);
    text_model_activate(text_input, ++g.serial, our_seat, target);
    text_model_send(TM_SHOW_PANEL, 0);
    keyboard_up = 1;
    log_msg("keyboard shown");
}

/* Nothing else dismisses the panel. The IME keeps it up until the client
 * asks, so Back, a click outside the address bar, and text_leave all land
 * here. Deactivating as well as hiding stops the IME reopening it on the
 * next focus event. */
static int hide_keyboard(void)
{
    if (!text_input || !keyboard_up)
        return 0;
    text_model_send(TM_HIDE_PANEL, 0);
    if (our_seat)
        text_model_deactivate(text_input, our_seat);
    /* The compositor destroys the text model on deactivate, and any later
     * request on it is a fatal "invalid object": seen on webOS 4 (the
     * emulator: the second keyboard crashed Firefox) and on webOS 25 (a TV:
     * hide the keyboard, click a field, Firefox gone). So start a new one
     * next time. Should a compositor keep it, one object per keyboard is
     * left behind, which is harmless. */
    wl_proxy_destroy((struct wl_proxy *)text_input);
    text_input = NULL;
    keyboard_closed();
    log_msg("keyboard hidden");
    return 1;
}

static void text_commit_string(void *data, struct text_model *model, uint32_t serial,
                               const char *text)
{
    (void)data;
    (void)model;
    (void)serial;
    inject_text(text);
}

static void text_preedit_string(void *data, struct text_model *model, uint32_t serial,
                                const char *text, const char *commit)
{
    (void)data;
    (void)model;
    (void)serial;
    /* Not typed. The keyboard sends the text as commit_string when done. */
    log_trace("preedit %zu chars (commit %zu)", text ? strlen(text) : 0,
            commit ? strlen(commit) : 0);
}

static void text_delete_surrounding(void *data, struct text_model *model, uint32_t serial,
                                    int32_t index, uint32_t length)
{
    uint32_t i;

    (void)data;
    (void)model;
    (void)serial;
    if (index < 0)
        length = length ? length : (uint32_t)(-index);
    for (i = 0; i < length; i++)
        inject_key(14, 0);
}

static void text_cursor_position(void *data, struct text_model *model, uint32_t serial,
                                 int32_t index, int32_t anchor)
{
    (void)data;
    (void)model;
    (void)serial;
    (void)index;
    (void)anchor;
}

static void text_preedit_styling(void *data, struct text_model *model, uint32_t serial,
                                 uint32_t index, uint32_t length, uint32_t style)
{
    (void)data;
    (void)model;
    (void)serial;
    (void)index;
    (void)length;
    (void)style;
}

static void text_preedit_cursor(void *data, struct text_model *model, uint32_t serial,
                                int32_t index)
{
    (void)data;
    (void)model;
    (void)serial;
    (void)index;
}

static void text_modifiers_map(void *data, struct text_model *model, struct wl_array *map)
{
    (void)data;
    (void)model;
    (void)map;
}

static void text_keysym(void *data, struct text_model *model, uint32_t serial, uint32_t time,
                        uint32_t sym, uint32_t state, uint32_t modifiers)
{
    (void)data;
    (void)model;
    (void)serial;
    (void)time;
    (void)modifiers;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
        return;
    /* The webOS keyboard sends editing keys as X11 keysyms rather than as
     * text or delete_surrounding_text (it has no surrounding text from us):
     * BackSpace arrives as 0xff08. Press the matching evdev key. */
    switch (sym) {
    case 0xff08: inject_key(KEY_BACKSPACE, 0); break;
    case 0xff09: inject_key(KEY_TAB, 0); break;
    case 0xff0d: /* Return */
    case 0xff8d: inject_key(KEY_ENTER, 0); break; /* KP_Enter */
    case 0xff1b: inject_key(KEY_ESC, 0); break;
    case 0xff50: inject_key(KEY_HOME, 0); break;
    case 0xff51: inject_key(KEY_LEFT, 0); break;
    case 0xff52: inject_key(KEY_UP, 0); break;
    case 0xff53: inject_key(KEY_RIGHT, 0); break;
    case 0xff54: inject_key(KEY_DOWN, 0); break;
    case 0xff57: inject_key(KEY_END, 0); break;
    case 0xffff: inject_key(KEY_DELETE, 0); break;
    default: {
        /* A printable character as a Latin-1 keysym (the character itself)
         * or a Unicode keysym (0x1000000 plus the code point). Some webOS
         * keyboards send typed characters this way instead of as
         * commit_string. */
        uint32_t ch = sym >= 0x1000000 ? sym - 0x1000000 : sym;
        char text[2];

        if (ch < 0x20 || ch > 0x7e) {
            log_msg("keysym %#x not mapped", sym);
            return;
        }
        text[0] = (char)ch;
        text[1] = 0;
        inject_text(text);
        return;
    }
    }
    log_trace("keysym %#x", sym);
}

static void text_enter(void *data, struct text_model *model, struct wl_surface *surface)
{
    (void)data;
    (void)model;
    (void)surface;
}

static void text_leave(void *data, struct text_model *model)
{
    (void)data;
    (void)model;
    log_trace("text model leave");
    keyboard_closed();
}

/* Where the webOS keyboard sits on screen, in the remote's 1920x1080 grid;
 * zero size until the keyboard reports it. */
static int32_t panel_x, panel_y, panel_w, panel_h;

static void text_panel_state(void *data, struct text_model *model, uint32_t state)
{
    (void)data;
    (void)model;
    log_msg("keyboard panel %u", state);
    /* Closed by its own button, not by us. */
    if (state == 0)
        keyboard_closed();
}

static void text_panel_rect(void *data, struct text_model *model, int32_t x, int32_t y,
                            uint32_t width, uint32_t height)
{
    (void)data;
    (void)model;
    panel_x = x;
    panel_y = y;
    panel_w = (int32_t)width;
    panel_h = (int32_t)height;
    log_msg("keyboard panel at %d,%d %ux%u", x, y, width, height);
}

/* True if a remote position (1920x1080 grid) is on the open keyboard. Clicks
 * there go to the keyboard: the compositor routes them to its surface, not
 * ours, so they must not be injected into Firefox as well. Before the
 * keyboard reports its area, assume it covers the bottom 45%. */
static int on_keyboard(int32_t x, int32_t y)
{
    if (!keyboard_up)
        return 0;
    if (panel_w > 0 && panel_h > 0)
        return x >= panel_x && x < panel_x + panel_w && y >= panel_y && y < panel_y + panel_h;
    return y >= 1080 * 55 / 100;
}

static const struct text_model_listener text_listener = {
    text_commit_string,
    text_preedit_string,
    text_delete_surrounding,
    text_cursor_position,
    text_preedit_styling,
    text_preedit_cursor,
    text_modifiers_map,
    text_keysym,
    text_enter,
    text_leave,
    text_panel_state,
    text_panel_rect,
};

static void ensure_text_model(void)
{
    static struct text_model_factory *factory;

    if (!g.registry)
        return;
    if (!our_seat && host_seat_name) {
        /* webOS 4's seat is version 2; binding above that is a protocol error. */
        our_seat = wl_registry_bind(g.registry, host_seat_name, &wl_seat_interface,
                                    host_seat_version < 4 ? host_seat_version : 4);
        log_msg("our seat %p", (void *)our_seat);
    }
    if (text_input || !host_text_name)
        return;
    if (!factory)
        factory = wl_registry_bind(g.registry, host_text_name, &text_model_factory_interface, 1);
    if (!factory)
        return;
    text_input = create_text_model(factory);
    if (text_input)
        text_model_add_listener(text_input, &text_listener, NULL);
    log_msg("text model %p", (void *)text_input);
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    /* On the pop-up overlay or the GL window, GTK is told about the
     * pop-up under the pointer. */
    ov_ptr_on = (ov_surface && surface == ov_surface) ||
                (gl_mode() && surface == (struct wl_surface *)gl_window);
    if (ov_ptr_on) {
        int32_t ox = 0, oy = 0;
        struct wl_surface *hit;

        /* webOS 4 sometimes gives this enter on the remote's 1920x1080 grid
         * (1488,324 for 999,220 of a 1280x720 window); a position outside the
         * window can only be that. */
        if (ov_w > 0 && ov_w < 1920 && ((x >> 8) >= ov_w || (y >> 8) >= ov_h)) {
            x = (wl_fixed_t)((int64_t)x * ov_w / 1920);
            y = (wl_fixed_t)((int64_t)y * ov_h / 1080);
        }
        hit = ov_hit(x >> 8, y >> 8, &ox, &oy);
        ov_ptr_serial = serial;
        ov_ptr_popup = hit;
        ov_ptr_x = ox;
        ov_ptr_y = oy;
        last_x = x >> 8;
        last_y = y >> 8;
        saw_motion = 1;
        note_real_pointer();
        log_trace("POINTER enter pop-up overlay at %d,%d: pop-up %p", last_x, last_y, (void *)hit);
        if (hit && orig && orig->enter)
            orig->enter(data, pointer, serial, hit, x - wl_fixed_from_int(ox),
                        y - wl_fixed_from_int(oy));
        else if (!hit && gl_mode() && main_surface && orig && orig->enter)
            orig->enter(data, pointer, serial, main_surface, x, y);
        return;
    }
    {
        int32_t ox = 0, oy = 0;
        struct wl_proxy *popup =
            gl_mode() ? gp_input_popup((struct wl_proxy *)surface, &ox, &oy) : NULL;

        if (popup) {
            surface = (struct wl_surface *)popup;
            x += wl_fixed_from_int(ox);
            y += wl_fixed_from_int(oy);
        } else {
            surface = input_surface(surface);
        }
    }
    last_x = x >> 8;
    last_y = y >> 8;
    saw_motion = 1;
    synth_focus = surface;
    note_real_pointer();
    log_trace("POINTER enter surface=%p at %d,%d", (void *)surface, last_x, last_y);
    if (orig && orig->enter)
        orig->enter(data, pointer, serial, surface, x, y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if ((ov_surface && surface == ov_surface) ||
        (gl_mode() && surface == (struct wl_surface *)gl_window)) {
        if (ov_ptr_popup && orig && orig->leave)
            orig->leave(data, pointer, serial, ov_ptr_popup);
        else if (!ov_ptr_popup && gl_mode() && main_surface && orig && orig->leave)
            orig->leave(data, pointer, serial, main_surface);
        ov_ptr_popup = NULL;
        ov_ptr_on = 0;
        return;
    }
    {
        struct wl_proxy *popup =
            gl_mode() ? gp_input_popup((struct wl_proxy *)surface, NULL, NULL) : NULL;

        surface = popup ? (struct wl_surface *)popup : input_surface(surface);
    }
    if (synth_focus == surface)
        synth_focus = NULL;
    log_trace("POINTER leave surface=%p", (void *)surface);
    if (orig && orig->leave)
        orig->leave(data, pointer, serial, surface);
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    last_x = x >> 8;
    last_y = y >> 8;
    saw_motion = 1;
    note_real_pointer();
    if (ov_ptr_on) {
        int32_t ox = 0, oy = 0;
        struct wl_surface *hit = ov_hit(x >> 8, y >> 8, &ox, &oy);

        if (hit != ov_ptr_popup) {
            log_trace("POINTER on pop-up %p at %d,%d", (void *)hit, last_x, last_y);
            if (ov_ptr_popup && orig && orig->leave)
                orig->leave(data, pointer, ov_ptr_serial, ov_ptr_popup);
            else if (!ov_ptr_popup && gl_mode() && main_surface && orig && orig->leave)
                orig->leave(data, pointer, ov_ptr_serial, main_surface);
            if (hit && orig && orig->enter)
                orig->enter(data, pointer, ov_ptr_serial, hit, x - wl_fixed_from_int(ox),
                            y - wl_fixed_from_int(oy));
            else if (!hit && gl_mode() && main_surface && orig && orig->enter)
                orig->enter(data, pointer, ov_ptr_serial, main_surface, x, y);
            ov_ptr_popup = hit;
            ov_ptr_x = ox;
            ov_ptr_y = oy;
        }
        if (hit && orig && orig->motion)
            orig->motion(data, pointer, time, x - wl_fixed_from_int(ov_ptr_x),
                         y - wl_fixed_from_int(ov_ptr_y));
        else if (!hit && gl_mode() && orig && orig->motion)
            orig->motion(data, pointer, time, x, y);
        return;
    }
    if (orig && orig->motion)
        orig->motion(data, pointer, time, x, y);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    note_real_pointer();
    log_trace("click button %u state %u x %d y %d", button, state, last_x, last_y);
    if (orig && orig->button)
        orig->button(data, pointer, serial, time, button, state);
    /* Address field sits under the tab strip and left of the menu button.
     * Coordinates are surface-local, so only judge them on the main window;
     * inside a menu they mean something else entirely. */
    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        note_click();
    /* Without text-input-v3 (GTK did not bind it), guess from the position. */
    if (!text_input_bound && state == WL_POINTER_BUTTON_STATE_PRESSED && saw_motion &&
        (!synth_focus || synth_focus == main_surface)) {
        if (last_y >= 30 && last_y < 110 && last_x < 1700)
            show_keyboard();
        else
            hide_keyboard();
    }
}

static struct hook *primary_pointer(void)
{
    int i;

    for (i = 0; i < MAX_HOOK; i++) {
        if (pointer_hooks[i].proxy && pointer_hooks[i].listener)
            return &pointer_hooks[i];
    }
    return NULL;
}

static void deliver_wheel(struct hook *hook, int linux_value, wl_fixed_t fx, wl_fixed_t fy)
{
    const struct wl_pointer_listener *keys = hook->listener;
    int notch = linux_value > 0 ? -1 : 1;
    wl_fixed_t step = (wl_fixed_t)(notch * 60) << 8;

    if (!keys)
        return;
    /* No motion here: it would move the focus off an open menu. The wheel
     * is delivered wherever the compositor has already put the pointer. */
    (void)fx;
    (void)fy;
    if (keys->axis_source)
        keys->axis_source(hook->data, (struct wl_pointer *)hook->proxy, 0);
    if (keys->axis_discrete)
        keys->axis_discrete(hook->data, (struct wl_pointer *)hook->proxy, 0, notch);
    if (keys->axis_value120)
        keys->axis_value120(hook->data, (struct wl_pointer *)hook->proxy, 0, notch * 120);
    if (keys->axis)
        keys->axis(hook->data, (struct wl_pointer *)hook->proxy, 0, 0, step);
    if (keys->frame)
        keys->frame(hook->data, (struct wl_pointer *)hook->proxy);
    log_trace("wheel seat %p axis %d frame %d discrete %d v120 %d",
            hook->proxy, keys->axis != NULL, keys->frame != NULL,
            keys->axis_discrete != NULL, keys->axis_value120 != NULL);
}

/* Topmost live popup containing the point, in screen coordinates. */
static struct virt *popup_at(int x, int y)
{
    struct virt *best = NULL;
    int i;

    for (i = 0; i < MAX_VIRT; i++) {
        struct virt *v = &g.virts[i];

        if (!v->proxy || v->kind != V_POPUP || !v->subsurface || !v->configured)
            continue;
        if (x >= v->abs_x && x < v->abs_x + v->p_w &&
            y >= v->abs_y && y < v->abs_y + v->p_h)
            best = v;
    }
    return best;
}

/* Move the seat's focus to `target` if it is not already there, then report
 * the position in that surface's own coordinates. */
static void focus_and_move(struct hook *hook, struct wl_surface *target,
                           int lx, int ly)
{
    const struct wl_pointer_listener *keys = hook->listener;

    if (!keys)
        return;
    if (target != synth_focus) {
        if (synth_focus && keys->leave)
            keys->leave(hook->data, (struct wl_pointer *)hook->proxy,
                        ++g.serial, synth_focus);
        if (target && keys->enter)
            keys->enter(hook->data, (struct wl_pointer *)hook->proxy,
                        ++g.serial, target, lx << 8, ly << 8);
        synth_focus = target;
        log_trace("pointer focus -> %p at %d,%d", (void *)target, lx, ly);
    }
    if (keys->motion)
        keys->motion(hook->data, (struct wl_pointer *)hook->proxy, 0,
                     lx << 8, ly << 8);
}

static void apply_remote_event(struct ptr_ev ev)
{
    struct hook *hook = primary_pointer();
    const struct wl_pointer_listener *keys;
    wl_fixed_t fx;
    wl_fixed_t fy;

    /* The keyboard handles its own clicks; Back and the wheel still apply. */
    if (ev.button >= 0 && on_keyboard(ev.x, ev.y))
        return;
    /* The Magic Remote reports positions on a 1920x1080 grid. */
    ev.x = ev.x * win_w / 1920;
    ev.y = ev.y * win_h / 1080;
    fx = ev.x < 0 ? 0 : ev.x;
    fy = ev.y < 0 ? 0 : ev.y;
    if (fx > win_w - 1)
        fx = win_w - 1;
    if (fy > win_h - 1)
        fy = win_h - 1;
    fx <<= 8;
    fy <<= 8;
    /* Back, read straight from evdev so it works even while the IME
     * panel holds Wayland focus. */
    if (ev.button == -3) {
        if (hide_keyboard())
            note_back_hid_keyboard();
        return;
    }
    /* The compositor closes the window when it sees this notch.
     * Hand it to every seat as a scroll instead. */
    if (ev.button == -2) {
        int i;

        for (i = 0; i < MAX_HOOK; i++) {
            if (pointer_hooks[i].proxy && pointer_hooks[i].listener) {
                deliver_wheel(&pointer_hooks[i], ev.down, fx, fy);
                break;
            }
        }
        log_trace("remote wheel %d", ev.down);
        return;
    }
    if (!hook)
        return;
    keys = hook->listener;
    /* The compositor is already sending these. A second copy breaks menus. */
    if (real_pointer_active()) {
        static int quiet;
        if (!quiet) {
            quiet = 1;
            log_msg("real pointer is live, not injecting clicks");
        }
        return;
    }
    if (ev.x < 0)
        ev.x = 0;
    if (ev.y < 0)
        ev.y = 0;
    if (ev.x > win_w - 1)
        ev.x = win_w - 1;
    if (ev.y > win_h - 1)
        ev.y = win_h - 1;
    last_x = ev.x;
    last_y = ev.y;
    saw_motion = 1;
    {
        struct virt *over = popup_at(ev.x, ev.y);
        struct wl_surface *target = over ? (struct wl_surface *)over->wl_surface
                                         : main_surface;
        int lx = over ? ev.x - over->abs_x : ev.x;
        int ly = over ? ev.y - over->abs_y : ev.y;

        if (!over && ov_enabled()) {
            int32_t ox = 0, oy = 0;
            struct wl_surface *hit = ov_hit(ev.x, ev.y, &ox, &oy);

            if (hit) {
                target = hit;
                lx = ev.x - ox;
                ly = ev.y - oy;
            }
        }
        focus_and_move(hook, target, lx, ly);
        if (ev.button < 0 || !keys->button)
            return;
        keys->button(hook->data, (struct wl_pointer *)hook->proxy, ++g.serial, 0,
                     BTN_LEFT, ev.down ? WL_POINTER_BUTTON_STATE_PRESSED
                                       : WL_POINTER_BUTTON_STATE_RELEASED);
        if (keys->frame)
            keys->frame(hook->data, (struct wl_pointer *)hook->proxy);
        log_trace("remote click %d at %d,%d%s", ev.down, ev.x, ev.y,
                target != main_surface ? " (in popup)" : "");
        if (ev.down)
            note_click();
        /* A click inside a menu must not open or close the keyboard. */
        if (!text_input_bound && ev.down && target == main_surface) {
            if (ev.y >= 30 && ev.y < 110 && ev.x < 1700)
                show_keyboard();
            else
                hide_keyboard();
        }
    }
    (void)fx;
    (void)fy;
}

static void drain_remote(void)
{
    for (;;) {
        struct ptr_ev ev;

        pthread_mutex_lock(&ptr_mu);
        if (ptr_q_head == ptr_q_tail) {
            pthread_mutex_unlock(&ptr_mu);
            return;
        }
        ev = ptr_q[ptr_q_head];
        ptr_q_head = (ptr_q_head + 1) % PTR_QUEUE;
        pthread_mutex_unlock(&ptr_mu);
        apply_remote_event(ev);
    }
}

static void remote_sync_done(void *data, struct wl_callback *callback, uint32_t serial)
{
    (void)data;
    (void)serial;
    wl_callback_destroy(callback);
    pthread_mutex_lock(&ptr_mu);
    wake_pending_flag = 0;
    pthread_mutex_unlock(&ptr_mu);
    drain_remote();
}

static const struct wl_callback_listener remote_sync_listener = {
    remote_sync_done,
};

static void enqueue_remote(int x, int y, int button, int down)
{
    int wake = 0;

    pthread_mutex_lock(&ptr_mu);
    {
        int next = (ptr_q_tail + 1) % PTR_QUEUE;
        if (next != ptr_q_head) {
            ptr_q[ptr_q_tail].button = button;
            ptr_q[ptr_q_tail].down = down;
            ptr_q[ptr_q_tail].x = x;
            ptr_q[ptr_q_tail].y = y;
            ptr_q_tail = next;
        }
        if (!wake_pending_flag && wake_display) {
            wake_pending_flag = 1;
            wake = 1;
        }
    }
    pthread_mutex_unlock(&ptr_mu);
    if (!wake)
        return;
    {
        struct wl_callback *callback = wl_display_sync(wake_display);
        if (callback)
            wl_callback_add_listener(callback, &remote_sync_listener, NULL);
        wl_display_flush(wake_display);
    }
}

static int center_button(unsigned code)
{
    /* The wheel click and the pointer click show up as Enter, OK, or left button. */
    return code == KEY_ENTER || code == KEY_OK || code == BTN_LEFT;
}

static void *remote_thread(void *arg)
{
    const char *paths[] = {
        "/dev/input/event0",
        "/dev/input/event2",
        "/dev/input/event3",
        "/dev/input/event4",
    };
    struct pollfd pfds[4];
    int abs_x = 960;
    int abs_y = 540;
    int i;

    (void)arg;
    for (i = 0; i < 4; i++) {
        pfds[i].fd = open(paths[i], O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        pfds[i].events = POLLIN;
        if (pfds[i].fd < 0)
            log_msg("remote %s errno %d", paths[i], errno);
    }
    if (pfds[0].fd < 0 && pfds[1].fd < 0)
        return NULL;
    log_msg("remote open");
    for (;;) {
        if (poll(pfds, 4, 1000) <= 0)
            continue;
        for (i = 0; i < 4; i++) {
            struct input_event ev;

            if (pfds[i].fd < 0 || !(pfds[i].revents & POLLIN))
                continue;
            while (read(pfds[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
                if (ev.type == EV_ABS && ev.code == ABS_X)
                    abs_x = ev.value;
                else if (ev.type == EV_ABS && ev.code == ABS_Y)
                    abs_y = ev.value;
                else if (!mapped_one)
                    continue;
                else if (ev.type == EV_REL && ev.code == REL_WHEEL && ev.value)
                    enqueue_remote(abs_x, abs_y, -2, ev.value);
                else if (ev.type == EV_KEY && ev.code == KEY_BACK && ev.value == 1)
                    enqueue_remote(abs_x, abs_y, -3, 1);
                else if (ev.type == EV_KEY && center_button(ev.code))
                    enqueue_remote(abs_x, abs_y, BTN_LEFT, ev.value ? 1 : 0);
            }
        }
    }
    return NULL;
}

static void start_remote_pointer(void)
{
    pthread_t thread;

    if (pthread_create(&thread, NULL, remote_thread, NULL) == 0)
        pthread_detach(thread);
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis)
        orig->axis(data, pointer, time, axis, value);
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->frame)
        orig->frame(data, pointer);
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis_source)
        orig->axis_source(data, pointer, source);
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                              uint32_t axis)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis_stop)
        orig->axis_stop(data, pointer, time, axis);
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t discrete)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis_discrete)
        orig->axis_discrete(data, pointer, axis, discrete);
}

static void pointer_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t value120)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis_value120)
        orig->axis_value120(data, pointer, axis, value120);
}

static void pointer_axis_relative_direction(void *data, struct wl_pointer *pointer,
                                            uint32_t axis, uint32_t direction)
{
    struct hook *hook = hook_find(pointer_hooks, pointer);
    const struct wl_pointer_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->axis_relative_direction)
        orig->axis_relative_direction(data, pointer, axis, direction);
}

static const struct wl_pointer_listener pointer_wrapper = {
    pointer_enter,
    pointer_leave,
    pointer_motion,
    pointer_button,
    pointer_axis,
    pointer_frame,
    pointer_axis_source,
    pointer_axis_stop,
    pointer_axis_discrete,
    pointer_axis_value120,
    pointer_axis_relative_direction,
};

/* True while a menu is on screen. */
static int popup_is_open(void)
{
    int i;

    for (i = 0; i < MAX_VIRT; i++) {
        /* On webOS 4 pop-ups are drawn into the overlay instead, and showing
         * the overlay moves the keyboard focus to it and back. */
        if (g.virts[i].proxy && g.virts[i].kind == V_POPUP &&
            (g.virts[i].subsurface || ov_enabled()))
            return 1;
    }
    return 0;
}

/* True while Firefox has asked for a fullscreen window. It asks when a page
 * uses the fullscreen API, as a video player does. */
static int toplevel_fullscreen(void)
{
    int i;

    for (i = 0; i < MAX_VIRT; i++)
        if (g.virts[i].proxy && g.virts[i].kind == V_TOPLEVEL && g.virts[i].fullscreen)
            return 1;
    return 0;
}

static int is_popup_surface(struct wl_surface *surface)
{
    int i;

    if (!surface)
        return 0;
    for (i = 0; i < MAX_VIRT; i++) {
        if (g.virts[i].proxy && g.virts[i].kind == V_POPUP &&
            g.virts[i].subsurface &&
            (struct wl_surface *)g.virts[i].wl_surface == surface)
            return 1;
    }
    return 0;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    focused_keys = hook;
    keyboard_focus_raw = surface;
    surface = input_surface(surface);
    log_trace("KEYBOARD enter surface=%p", (void *)surface);
    /* LSM treats a popup subsurface as separately focusable and hands it the
     * keyboard. GTK reads that as its toplevel losing focus and deactivates
     * the menu, destroying the popup between the press and the release, so
     * no item ever activates. Keep GTK believing the toplevel still has it. */
    if (is_popup_surface(surface)) {
        log_trace("  swallowed: popup keyboard focus stays hidden from GTK");
        return;
    }
    /* The focus is back after the webOS keyboard had it. GTK never knew. */
    if (hook && hook->held_leave && surface == main_surface) {
        hook->held_leave = 0;
        log_trace("  swallowed: the held-back leave is cancelled");
        return;
    }
    if (orig && orig->enter)
        orig->enter(data, keyboard, serial, surface, keys);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    if (keyboard_focus_raw == surface)
        keyboard_focus_raw = NULL;
    surface = input_surface(surface);
    log_trace("KEYBOARD leave surface=%p", (void *)surface);
    if (is_popup_surface(surface)) {
        log_trace("  swallowed: popup keyboard leave");
        return;
    }
    if (surface == main_surface && popup_is_open()) {
        log_trace("  swallowed: toplevel keeps focus while a menu is open");
        return;
    }
    /* The webOS keyboard is open and has taken the keyboard focus. Passed on,
     * the leave makes GTK's field lose focus and disable text input, the
     * adapter then drops the text model, and the keyboard still on screen
     * types into nothing. Hold it back until the keyboard closes. */
    if (hook && surface == main_surface && keyboard_up) {
        hook->held_leave = 1;
        hook->held_serial = serial;
        log_trace("  held back: the webOS keyboard has the focus while it is open");
        return;
    }
    if (focused_keys == hook)
        focused_keys = NULL;
    if (orig && orig->leave)
        orig->leave(data, keyboard, serial, surface);
}

/* The webOS keyboard is closed. The leaves held back while it was open are
 * passed on, unless the focus came back meanwhile. */
static void keyboard_closed(void)
{
    int i;

    keyboard_up = 0;
    for (i = 0; i < MAX_HOOK; i++) {
        struct hook *hook = &keyboard_hooks[i];
        const struct wl_keyboard_listener *orig = hook->listener;

        if (!hook->held_leave)
            continue;
        hook->held_leave = 0;
        log_trace("keyboard leave passed on after the webOS keyboard closed");
        if (focused_keys == hook)
            focused_keys = NULL;
        if (orig && orig->leave)
            orig->leave(hook->data, (struct wl_keyboard *)hook->proxy, hook->held_serial,
                        main_surface);
    }
}

#define KEYMAP_CODE_MAX 0xfffu

/* Blanks, with spaces so the size stays, each "<NAME> = code;" line of the
 * xkb_keycodes section whose code is above 0xfff. xkbcommon 1.9 refuses the
 * whole keymap over one such line ("Keycode too big"), and LG's keymap has
 * them for remote-control keys. The compositor sends xkbcommon's own dump,
 * one statement per line. Returns the number of lines blanked. */
static size_t keymap_drop_big_keycodes(char *s)
{
    char *line = strstr(s, "xkb_keycodes");
    size_t blanked = 0;

    while (line && (line = strchr(line, '\n')) && *++line != '}') {
        char *p = line + strspn(line, " \t"), *eq, *end;
        size_t n = strcspn(p, "\n");

        if (*p != '<' || !(eq = memchr(p, '=', n)))
            continue;
        if (strtoul(eq + 1, &end, 0) > KEYMAP_CODE_MAX && end > eq + 1) {
            memset(p, ' ', n);
            blanked++;
        }
    }
    return blanked;
}

/* The xkb_symbols name of a keymap dump, "pc+us+inet(evdev)" for example,
 * and whether it names the us layout. ascii_key and letter_code assume that
 * layout, and a keymap GTK cannot compile leaves it with its own us one. */
static const char *keymap_symbols(const char *text, int *len, int *us)
{
    const char *sym = strstr(text, "xkb_symbols \"");
    const char *q, *end;

    *len = 0;
    *us = 0;
    if (!sym)
        return "";
    sym += 13;
    end = sym + strcspn(sym, "\"");
    *len = (int)(end - sym);
    for (q = sym; q + 2 <= end; q++)
        if (q[0] == 'u' && q[1] == 's' && (q == sym || q[-1] == '+') &&
            (q + 2 == end || q[2] == '+'))
            *us = 1;
    return sym;
}

/* A copy of the compositor's keymap without the keycodes xkbcommon refuses,
 * as a new fd of the same size. Returns -1 to pass the original on.
 * WEBOS_XDG_RAW_KEYMAP=1 always passes the original on. */
static int keymap_fixed_copy(uint32_t format, int fd, uint32_t size)
{
    const char *raw = getenv("WEBOS_XDG_RAW_KEYMAP");
    char *map, *text;
    size_t blanked;
    int out, n, us;
    size_t done = 0;
    struct stat st;
    const char *sym;

    if ((raw && raw[0] == '1') || format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 ||
        size > (16u << 20))
        return -1;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)size)
        return -1;
    map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED)
        return -1;
    text = malloc(size + 1);
    if (!text) {
        munmap(map, size);
        return -1;
    }
    memcpy(text, map, size);
    munmap(map, size);
    text[size] = 0;
    blanked = keymap_drop_big_keycodes(text);
    sym = keymap_symbols(text, &n, &us);
    if (blanked && !us)
        log_msg("keymap: symbols \"%.*s\" are not us, passed on as sent", n, sym);
    if (!blanked || !us) {
        free(text);
        return -1;
    }
    out = ov_memfd(size);
    while (out >= 0 && done < size) {
        ssize_t w = write(out, text + done, size - done);

        if (w <= 0) {
            close(out);
            out = -1;
        } else {
            done += (size_t)w;
        }
    }
    if (out >= 0)
        log_msg("keymap: %zu keycodes above 0xfff left out for xkbcommon, symbols \"%.*s\"",
                blanked, n, sym);
    free(text);
    return out;
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
                            int32_t fd, uint32_t size)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;
    int fixed = keymap_fixed_copy(format, fd, size);

    if (fixed >= 0) {
        close(fd);
        fd = fixed;
    }
    if (orig && orig->keymap)
        orig->keymap(data, keyboard, format, fd, size);
    else
        close(fd);
}

/* The key a press of the remote's Back key is passed on as, or 0 for none.
 * The webOS keyboard closes first. Then Escape leaves a menu or a
 * fullscreen video, as it does on a desktop. Otherwise KEY_BACK goes out.
 * GTK's keymap turns it into XF86Back and Firefox into the Back command
 * that opens the previous page. */
static uint32_t back_key_code(void)
{
    if (keyboard_up || back_hid_keyboard_recently()) {
        hide_keyboard();
        log_trace("back: keyboard");
        return 0;
    }
    if (popup_is_open() || toplevel_fullscreen()) {
        log_trace("back: escape");
        return KEY_ESC;
    }
    log_trace("back: previous page");
    return KEY_BACK;
}

/* What the last Back press became, so its release matches. */
static uint32_t back_as;

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
        log_trace("key %u", key);
    if (key == KEY_BACK) {
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
            back_as = back_key_code();
        key = back_as;
        if (!key)
            return;
    } else if (key == KEY_EXIT || key == 1198 || key == 1199) {
        /* Exit, and the wheel's cursor-show/hide keys. */
        return;
    }
    if (orig && orig->key)
        orig->key(data, keyboard, serial, time, key, state);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->modifiers)
        orig->modifiers(data, keyboard, serial, depressed, latched, locked, group);
}

static void keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate,
                            int32_t delay)
{
    struct hook *hook = hook_find(keyboard_hooks, keyboard);
    const struct wl_keyboard_listener *orig = hook ? hook->listener : NULL;

    if (orig && orig->repeat_info)
        orig->repeat_info(data, keyboard, rate, delay);
}

static const struct wl_keyboard_listener keyboard_wrapper = {
    keyboard_keymap,
    keyboard_enter,
    keyboard_leave,
    keyboard_key,
    keyboard_modifiers,
    keyboard_repeat,
};

/* Writes "library+0xoffset" (or "[heap]+0x..", or "unmapped") for addr. */
static void where_is(const void *addr, char *out, size_t n)
{
    char line[512];
    FILE *maps = fopen("/proc/self/maps", "r");

    snprintf(out, n, "unmapped");
    if (!maps)
        return;
    while (fgets(line, sizeof line, maps)) {
        unsigned long lo, hi, pgoff;
        char path[256] = "[anon]";
        const char *base;

        if (sscanf(line, "%lx-%lx %*s %lx %*s %*s %255s", &lo, &hi, &pgoff, path) >= 3 &&
            (unsigned long)addr >= lo && (unsigned long)addr < hi) {
            base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
            snprintf(out, n, "%s+0x%lx", base, (unsigned long)addr - lo + pgoff);
            break;
        }
    }
    fclose(maps);
}

/* True if n bytes at p can be read: read_own_memory() fails with EFAULT on
 * unmapped and on reserved (PROT_NONE) memory, where mincore would not. */
static int is_readable(const void *p, size_t n)
{
    char buf[64];
    ssize_t r;

    if (!p)
        return 0;
    if (n > sizeof buf)
        n = sizeof buf;
    r = read_own_memory(buf, p, n);
    return r == (ssize_t)n || (r < 0 && errno != EFAULT);
}

/* Interface name for a report, without trusting a possibly freed proxy. */
static const char *proxy_name(struct wl_proxy *proxy)
{
    const struct wl_interface *iface = proxy_iface(proxy);

    if (iface && is_readable(iface, sizeof *iface) && is_readable(iface->name, 1))
        return iface->name;
    return "?";
}

void *webos_xdg_last_caller(void);

void webos_xdg_prepare_listener(struct wl_proxy *proxy, void (***implementation)(void), void **data)
{
    const struct wl_interface *iface = proxy_iface(proxy);
    const char *name;
    struct virt *v;

    if (iface && (!is_readable(iface, sizeof *iface) || !is_readable(iface->name, 1))) {
        char at[300], by[300];

        where_is(iface, at, sizeof at);
        where_is(webos_xdg_last_caller(), by, sizeof by);
        log_msg("listener on proxy %p id %u: interface %p (%s) is not a wl_interface, added by %s",
                (void *)proxy, wl_proxy_get_id(proxy), (void *)iface, at, by);
        return;
    }
    name = iface ? iface->name : "";
    v = virt_get(proxy);
    log_trace("listener %s", name);
    if (!strcmp(name, "wl_pointer") || !strcmp(name, "wl_keyboard")) {
        struct hook *hook = hook_slot(!strcmp(name, "wl_pointer") ? pointer_hooks : keyboard_hooks,
                                      proxy);
        if (hook) {
            hook->proxy = proxy;
            hook->listener = *implementation;
            hook->data = *data;
            hook->held_leave = 0;
            if (!strcmp(name, "wl_keyboard")) {
                const struct wl_keyboard_listener *kl = (const void *)*implementation;
                char at[300];

                where_is(kl && kl->key ? (const void *)kl->key : NULL, at, sizeof at);
                if (strstr(at, "libgdk"))
                    gdk_keys = hook;
                log_trace("  wl_keyboard %u, key handler in %s", wl_proxy_get_id(proxy), at);
            }
            if (!strcmp(name, "wl_keyboard") && !focused_keys)
                focused_keys = hook;
            *implementation = (void (**)(void))(!strcmp(name, "wl_pointer")
                                                    ? (void *)&pointer_wrapper
                                                    : (void *)&keyboard_wrapper);
        } else {
            log_msg("  no free hook slot: this %s is not wrapped", name);
        }
    }
    if (v && v->kind != 0) {
        v->listener = *implementation;
        v->data = *data;
        return;
    }
    if (strcmp(name, "wl_registry") != 0)
        return;
    /* GDK's registry. Mali's EGL makes its own on a private queue and
     * destroys that queue; objects bound through it would be orphaned. */
    if (!g.registry && webos_xdg_on_default_queue(proxy))
        g.registry = (struct wl_registry *)proxy;
    if (!v)
        v = virt_add(proxy, 0);
    if (!v)
        return;
    v->listener = *implementation;
    v->data = *data;
    *implementation = (void (**)(void))&registry_wrapper;
}

void webos_xdg_listener_ready(struct wl_proxy *proxy)
{
    struct virt *v = virt_get(proxy);
    if (v && v->kind == V_TOPLEVEL)
        emit_toplevel_configure(v);
    if (v && v->kind == V_POPUP)
        emit_popup_configure(v);
    if (v && v->kind == V_TEXT_INPUT)
        text_input_enter_main();
}

static int cursor_surface_slot(struct wl_proxy *surface)
{
    int i;

    for (i = 0; i < MAX_CURSOR; i++) {
        if (cursor_surfaces[i] == surface)
            return i;
    }
    return -1;
}

static void cursor_surface_add(struct wl_proxy *surface)
{
    int i;

    if (!surface || cursor_surface_slot(surface) >= 0)
        return;
    for (i = 0; i < MAX_CURSOR; i++) {
        if (!cursor_surfaces[i]) {
            cursor_surfaces[i] = surface;
            log_trace("cursor surface %p: its content stays client-side", (void *)surface);
            return;
        }
    }
    log_msg("cursor surface table full");
}

/* 1: the request must not reach the compositor. */
static int cursor_surface_request(struct wl_proxy *surface, uint32_t opcode)
{
    static int dropped;
    int slot = cursor_surface_slot(surface);

    if (slot < 0)
        return 0;
    switch (opcode) {
    case WL_SURFACE_DESTROY:
        cursor_surfaces[slot] = NULL;
        return 0;
    case WL_SURFACE_ATTACH:
    case WL_SURFACE_DAMAGE:
    case WL_SURFACE_SET_OPAQUE_REGION:
    case WL_SURFACE_SET_INPUT_REGION:
    case WL_SURFACE_COMMIT:
    case WL_SURFACE_SET_BUFFER_TRANSFORM:
    case WL_SURFACE_SET_BUFFER_SCALE:
    case WL_SURFACE_DAMAGE_BUFFER:
    case WL_SURFACE_OFFSET:
        if (dropped < 8) {
            dropped++;
            log_trace("dropped cursor surface request %u", opcode);
        }
        return 1;
    default:
        return 0;
    }
}

/* Our libwayland calls this when asked to wrap a proxy whose event queue
 * has been destroyed (queue == NULL), before substituting the default queue. */
void webos_xdg_null_queue_report(struct wl_proxy *proxy, void *caller)
{
    static int reports;
    char by[300];

    if (reports++ >= 12)
        return;
    where_is(caller, by, sizeof by);
    log_msg("wrapping %s@%u whose queue was destroyed, for %s (default queue used)",
            proxy_name(proxy), wl_proxy_get_id(proxy), by);
}

/* libwayland aborts when wl_proxy_destroy() is called on a wrapper proxy.
 * Our patched libwayland frees the wrapper instead and calls this, with the
 * return address recorded at the public entry point, so the offending
 * library can be found: resolve the offset with nm/addr2line on that file. */
void webos_xdg_wrapper_destroy_report(struct wl_proxy *proxy, void *caller)
{
    static int reports;
    char by[300];

    if (reports++ >= 12)
        return;
    where_is(caller, by, sizeof by);
    log_msg("WRAPPER destroyed with wl_proxy_destroy: %s, caller %s (recovered)",
            proxy_name(proxy), by);
}

int webos_xdg_intercept(struct wl_proxy *proxy, uint32_t opcode,
                        const struct wl_interface *interface, uint32_t version,
                        uint32_t flags, union wl_argument *args, struct wl_proxy **out)
{
    const char *name = proxy_iface(proxy) ? proxy_iface(proxy)->name : "";
    struct virt *v;

    if (!strcmp(name, "wl_display")) {
        /* The proxy may be a wrapper that Mali's EGL made for its private
         * queue and frees soon after; keep the display itself. */
        g.display = webos_xdg_proxy_display(proxy);
        wake_display = g.display;
        /* Queued remote events are applied by remote_sync_done, on the
         * thread that dispatches the display. Draining here ran GDK
         * handlers on the evdev reader thread. */
    }

    if (ov_intercept(proxy, name, opcode, interface, version, flags, args, out))
        return 1;
    if ((opcode == WL_BUFFER_DESTROY && !strcmp(name, "wl_buffer")) ||
        (opcode == WL_SHM_POOL_DESTROY && !strcmp(name, "wl_shm_pool")))
        ov_destroying(proxy, name, opcode);

    if (!strcmp(name, "wl_registry") && opcode == WL_REGISTRY_BIND && interface && args) {
        if (args[0].u == DATA_DEVICE_MANAGER_NAME &&
            !strcmp(interface->name, "wl_data_device_manager")) {
            *out = new_virt(proxy, interface, version, V_DATA_DEVICE_MANAGER, NULL);
            log_msg("bound virtual wl_data_device_manager");
            return 1;
        }
        if (args[0].u == SUBCOMPOSITOR_NAME && !strcmp(interface->name, "wl_subcompositor")) {
            *out = new_virt(proxy, interface, version, V_SUBCOMPOSITOR, NULL);
            log_msg("bound virtual wl_subcompositor");
            return 1;
        }
        if (!strcmp(interface->name, "zwp_text_input_manager_v3")) {
            *out = new_virt(proxy, interface, version, V_TEXT_INPUT_MANAGER, NULL);
            text_input_bound = 1;
            log_msg("bound virtual zwp_text_input_manager_v3");
            return 1;
        }
        if (!strcmp(interface->name, "xdg_wm_base")) {
            struct wl_proxy *created = new_virt(proxy, interface, version, V_WM, NULL);
            log_msg("bound virtual xdg_wm_base version %u", version);
            bind_named();
            *out = created;
            return 1;
        }
        if (!strcmp(interface->name, "wl_compositor") && server_compositor_version &&
            args[2].u > server_compositor_version) {
            log_msg("clamp compositor %u -> %u", args[2].u, server_compositor_version);
            args[2].u = server_compositor_version;
        }
        if (!strcmp(interface->name, "wl_output") && server_output_version &&
            args[2].u > server_output_version) {
            log_msg("clamp output %u -> %u", args[2].u, server_output_version);
            args[2].u = server_output_version;
        }
        return 0;
    }

    /* Every subsurface, whether ours for a popup or Firefox's own
     * MozContainer, must be tagged before it ever carries content. */
    if (!strcmp(name, "wl_subcompositor") && opcode == WL_SUBCOMPOSITOR_GET_SUBSURFACE &&
        args) {
        /* Our stand-in (webOS 4) is handled below, not by the compositor. */
        if (!virt_get(proxy)) {
            tag_child_surface((struct wl_proxy *)args[1].o);
            return 0;
        }
    }

    /* The TV draws the Magic Remote pointer. Remember the surface GDK
     * uses for its cursor and keep it empty: with content on it LSM
     * would map a nameless card and minimize the browser window. */
    if (!strcmp(name, "wl_pointer") && opcode == WL_POINTER_SET_CURSOR) {
        if (args)
            cursor_surface_add((struct wl_proxy *)args[1].o);
        *out = NULL;
        return 1;
    }
    /* webOS 4: take a surface out of its group while its proxy can still be
     * named in the request. */
    if (!strcmp(name, "wl_surface") && opcode == WL_SURFACE_DESTROY) {
        int slot = layered_slot(proxy);

        if (synth_focus == (struct wl_surface *)proxy)
            synth_focus = NULL;
        if (gl_mode() && gp_destroy(proxy)) {
            *out = NULL;
            return 1;
        }
        if (slot >= 0)
            forget_slot(slot);
        if (gl_mode())
            gl_surface_destroyed(proxy);
        forget_shell_role(proxy);
        if (ov_enabled())
            ov_surface_destroyed(proxy);
        if (main_group && (struct wl_surface *)proxy == group_root)
            drop_group();
    }
    if (!strcmp(name, "wl_surface") && cursor_surface_request(proxy, opcode)) {
        *out = NULL;
        return 1;
    }

    /* wl_surface v4 request. The TV compositor is v3, so send the
     * surface-coordinate damage request instead. At scale 1 they match.
     */
    if (!strcmp(name, "wl_surface") && opcode == WL_SURFACE_DAMAGE_BUFFER &&
        server_compositor_version && server_compositor_version < 4 && args) {
        static int once;
        if (!once) {
            once = 1;
            log_trace("translate damage_buffer");
        }
        wl_proxy_marshal(proxy, WL_SURFACE_DAMAGE,
                         args[0].i, args[1].i, args[2].i, args[3].i);
        *out = NULL;
        return 1;
    }

    if (!strcmp(name, "wl_output") && opcode == WL_OUTPUT_RELEASE &&
        server_output_version && server_output_version < 3) {
        wl_proxy_destroy(proxy);
        *out = NULL;
        return 1;
    }

    v = virt_get(proxy);
    if (!v || v->kind == 0)
        return 0;
    log_trace("virt kind %d opcode %u", v->kind, opcode);
    *out = virt_request(v, opcode, flags, interface, args);
    return 1;
}
