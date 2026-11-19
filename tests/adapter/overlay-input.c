/* Host test of the pop-up overlay's input region in GL window mode. The
 * compositor hands pointer events to the topmost window under the cursor by
 * rectangle and drops them where that window's input region does not contain
 * the point, so the overlay's region must cover the whole window or the page
 * below gets no hover. Built by run.sh with the whole adapter included. The
 * proxies live on a wl_display over a socketpair; the test reads the
 * marshalled requests from the other end. */
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

/* wl_compositor.create_region, wl_region.add, wl_surface.set_input_region. */
#define OP_CREATE_REGION 1
#define OP_REGION_ADD 1
#define OP_SET_INPUT_REGION 5

int main(void)
{
    int sv[2];
    struct wl_display *display;
    static uint32_t wire[65536];
    ssize_t got = 0, n;
    size_t i;
    uint32_t regions[64];
    int regions_full[64];
    int nregions = 0;
    uint32_t compositor_id, ov_id;
    int set_seen = 0, set_full = 0, set_empty = 0;

    setenv("WEBOS_XDG_GL", "1", 1);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return 2;
    display = wl_display_connect_to_fd(sv[0]);
    g.display = display;
    g.registry = (struct wl_registry *)wl_proxy_create((struct wl_proxy *)display,
                                                       &wl_registry_interface);
    g.shell = (struct wl_shell *)wl_proxy_create((struct wl_proxy *)display, &wl_shell_interface);
    g.webos = (struct wl_webos_shell *)wl_proxy_create((struct wl_proxy *)display,
                                                       &wl_webos_shell_interface);
    host_foreign_name = 6;          /* flat mode, as on a webOS 6.5 TV */
    host_compositor_name = 1;
    host_shm_name = 2;
    win_w = 1920;
    win_h = 1080;
    gl_window_shown = 1;            /* the card is up, the overlay may map */

    CHECK(gl_mode(), "GL window mode is on");
    CHECK(ov_ensure() == 1, "the overlay window is made");
    CHECK(ov_surface != NULL, "the overlay has a surface");
    wl_display_flush(display);

    while ((n = recv(sv[1], (char *)wire + got, sizeof wire - (size_t)got, MSG_DONTWAIT)) > 0)
        got += n;
    CHECK(got > 0 && got % 4 == 0, "%zd bytes of requests read back", got);

    compositor_id = wl_proxy_get_id((struct wl_proxy *)ov_compositor);
    ov_id = wl_proxy_get_id((struct wl_proxy *)ov_surface);
    for (i = 0; i + 1 < (size_t)got / 4;) {
        uint32_t id = wire[i], opcode = wire[i + 1] & 0xffff, size = wire[i + 1] >> 16;
        size_t words = size / 4;
        int r;

        if (size < 8 || size % 4 || i + words > (size_t)got / 4)
            break;
        if (id == compositor_id && opcode == OP_CREATE_REGION && words == 3 && nregions < 64) {
            regions[nregions] = wire[i + 2];
            regions_full[nregions++] = 0;
        }
        for (r = 0; r < nregions; r++)
            if (id == regions[r] && opcode == OP_REGION_ADD && words == 6 &&
                (int32_t)wire[i + 2] == 0 && (int32_t)wire[i + 3] == 0 &&
                (int32_t)wire[i + 4] == ov_w && (int32_t)wire[i + 5] == ov_h)
                regions_full[r] = 1;
        if (id == ov_id && opcode == OP_SET_INPUT_REGION && words == 3) {
            set_seen++;
            for (r = 0; r < nregions; r++)
                if (regions[r] == wire[i + 2]) {
                    if (regions_full[r])
                        set_full++;
                    else
                        set_empty++;
                }
        }
        i += words;
    }
    CHECK(set_seen == 1, "the overlay's input region is set once (%d)", set_seen);
    CHECK(set_full == 1, "the region covers the whole %dx%d window", ov_w, ov_h);
    CHECK(set_empty == 0, "no empty region was set on it (%d)", set_empty);

    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
