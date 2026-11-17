/* Host test of the states the adapter reports in xdg_toplevel.configure.
 * Built by run.sh with the whole adapter included. */
#include "../../src/webos-xdg.c"

static int failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) {                                                   \
            printf("ok   " __VA_ARGS__);                              \
            printf("\n");                                             \
        } else {                                                      \
            printf("FAIL " __VA_ARGS__);                              \
            printf("\n");                                             \
            failures++;                                               \
        }                                                             \
    } while (0)

static int configures, got_fullscreen, got_maximized, got_activated, got_other;
static int32_t got_w, got_h;

static void on_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h,
                         struct wl_array *states)
{
    uint32_t *s;

    (void)data;
    (void)toplevel;
    configures++;
    got_fullscreen = got_maximized = got_activated = got_other = 0;
    got_w = w;
    got_h = h;
    wl_array_for_each(s, states) {
        if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN)
            got_fullscreen = 1;
        else if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED)
            got_maximized = 1;
        else if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
            got_activated = 1;
        else
            got_other = 1;
    }
}

static const struct xdg_toplevel_listener listener = { on_configure, NULL, NULL, NULL };

int main(void)
{
    struct virt tl;
    int fake_proxy, fake_surface;

    memset(&tl, 0, sizeof tl);
    tl.kind = V_TOPLEVEL;
    tl.proxy = (struct wl_proxy *)&fake_proxy;
    tl.wl_surface = (struct wl_proxy *)&fake_surface;
    tl.listener = &listener;
    win_w = 1920;
    win_h = 1080;

    emit_toplevel_configure(&tl);
    CHECK(configures == 1 && got_maximized && got_activated && !got_fullscreen && !got_other &&
              got_w == 1920 && got_h == 1080,
          "at first: maximized and activated, the window's size");

    virt_request(&tl, XDG_TOPLEVEL_SET_FULLSCREEN, 0, NULL, NULL);
    CHECK(configures == 2 && got_fullscreen && got_activated,
          "set_fullscreen: a configure with the fullscreen state");

    emit_toplevel_configure(&tl);
    CHECK(configures == 3 && got_fullscreen, "fullscreen stays across later configures");

    virt_request(&tl, XDG_TOPLEVEL_UNSET_FULLSCREEN, 0, NULL, NULL);
    CHECK(configures == 4 && !got_fullscreen && got_maximized && got_activated,
          "unset_fullscreen: back to a maximized window");

    virt_request(&tl, XDG_TOPLEVEL_SET_MAXIMIZED, 0, NULL, NULL);
    CHECK(configures == 5 && !got_fullscreen && got_maximized,
          "set_maximized: still maximized, not fullscreen");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
