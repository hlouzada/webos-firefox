/* Host test of GL window mode's GPU pop-ups. A pop-up content surface that
 * commits a buffer not from this process's wl_shm borrows the overlay's
 * window for its buffers, is hit by rectangle, follows its pop-up, and
 * gives the window back when hidden or destroyed. Built by run.sh with the
 * whole adapter included. The proxies live on a wl_display over a
 * socketpair nobody reads, so requests marshal without a compositor. */
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

static struct virt *add_popup(struct wl_proxy *surface, int32_t w, int32_t h)
{
    int i;

    for (i = 0; i < MAX_VIRT; i++)
        if (!g.virts[i].proxy) {
            memset(&g.virts[i], 0, sizeof g.virts[i]);
            g.virts[i].proxy = surface;   /* any non-NULL pointer */
            g.virts[i].kind = V_POPUP;
            g.virts[i].wl_surface = surface;
            g.virts[i].p_w = w;
            g.virts[i].p_h = h;
            return &g.virts[i];
        }
    return NULL;
}

int main(void)
{
    int sv[2];
    struct wl_display *display;
    struct wl_proxy *compositor, *popup, *popup2, *child, *child2, *gpu_buf, *gpu_buf2, *shm_buf;
    union wl_argument damage[4];
    struct wl_surface *hit;
    int32_t ox, oy;

    setenv("WEBOS_XDG_GL", "1", 1);
    setenv("WEBOS_XDG_GPU_POPUP_PLACED", "asked", 1);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return 2;
    display = wl_display_connect_to_fd(sv[0]);
    compositor = wl_proxy_create((struct wl_proxy *)display, &wl_compositor_interface);
    g.shell = (struct wl_shell *)wl_proxy_create((struct wl_proxy *)display, &wl_shell_interface);
    g.webos = (struct wl_webos_shell *)wl_proxy_create((struct wl_proxy *)display,
                                                       &wl_webos_shell_interface);
    host_foreign_name = 6;          /* flat mode, as on a webOS 6.5 TV */
    host_compositor_name = 1;
    ov_shm = (struct wl_shm *)wl_proxy_create((struct wl_proxy *)display, &wl_shm_interface);
    main_surface = (struct wl_surface *)wl_proxy_create(compositor, &wl_surface_interface);
    win_w = 1920;
    win_h = 1080;
    popup = wl_proxy_create(compositor, &wl_surface_interface);
    popup2 = wl_proxy_create(compositor, &wl_surface_interface);
    child = wl_proxy_create(compositor, &wl_surface_interface);
    child2 = wl_proxy_create(compositor, &wl_surface_interface);
    gpu_buf = wl_proxy_create(compositor, &wl_buffer_interface);
    gpu_buf2 = wl_proxy_create(compositor, &wl_buffer_interface);
    shm_buf = wl_proxy_create(compositor, &wl_buffer_interface);
    ov_bufs[0].proxy = shm_buf;     /* a buffer of this process's wl_shm */
    /* The overlay window, as ov_ensure would have made it. */
    ov_compositor = (struct wl_compositor *)compositor;
    ov_surface = (struct wl_surface *)wl_proxy_create(compositor, &wl_surface_interface);
    ov_w = win_w;
    ov_h = win_h;
    ov_canvas = calloc((size_t)ov_w * (size_t)ov_h, 4);
    ov_buffer[0] = (struct wl_buffer *)wl_proxy_create(compositor, &wl_buffer_interface);
    ov_buffer[1] = (struct wl_buffer *)wl_proxy_create(compositor, &wl_buffer_interface);
    ov_mem[0] = calloc((size_t)ov_w * (size_t)ov_h, 4);
    ov_mem[1] = calloc((size_t)ov_w * (size_t)ov_h, 4);
    ov_free[0] = ov_free[1] = 1;
    ov_shown = 1;
    damage[0].i = damage[1].i = 0;
    damage[2].i = damage[3].i = 10;

    CHECK(gl_mode() && flat_mode(), "GL window mode on a flat-mode compositor");

    /* Firefox makes the content surface, the pop-up gets its role. */
    ov_child(child, popup);
    add_popup(popup, 446, 286);
    ov_popup_at(popup, 1434, 76);
    CHECK(ov_item_find(child) && !gp_find(child), "a content surface starts in the overlay");

    /* Its first buffer is not shared memory, so it goes to the overlay's window. */
    CHECK(gp_attach(child, gpu_buf) == 1 && gp_find(child) && !ov_item_find(child) &&
              gp_find(child)->shown && gp_showing(),
          "a GPU buffer promotes it out of the overlay and onto the window");
    CHECK(gp_find(child)->popup == popup && gp_find(child)->x == 1434 &&
              gp_find(child)->y == 76 && gp_find(child)->w == 446 && gp_find(child)->h == 286,
          "placed where the pop-up is, with the pop-up's size");
    CHECK(shelled_slot(child) < 0, "it gets no role of its own");
    CHECK(gp_request(child, WL_SURFACE_DAMAGE, damage) == 1 &&
              gp_request(child, WL_SURFACE_COMMIT, NULL) == 1,
          "its damage and commit are taken for the window");
    CHECK(gp_request(popup, WL_SURFACE_COMMIT, NULL) == 0, "another surface's commit goes out");
    CHECK(gp_input_popup(child, &ox, &oy) == popup && ox == 0 && oy == 0,
          "input on it belongs to GTK's pop-up surface");
    CHECK(input_surface((struct wl_surface *)child) == main_surface,
          "keyboard focus on it reads as GTK's window");

    hit = ov_hit(1500, 100, &ox, &oy);
    CHECK(hit == (struct wl_surface *)popup && ox == 1434 && oy == 76,
          "a click inside lands on the pop-up, origin 1434,76");
    CHECK(ov_hit(1433, 100, &ox, &oy) == NULL && ov_hit(1500, 362, &ox, &oy) == NULL,
          "clicks beside and below it miss");

    /* A shared-memory content surface stays in the overlay. */
    ov_child(child2, popup);
    CHECK(gp_attach(child2, shm_buf) == 0 && ov_item_find(child2) && !gp_find(child2),
          "a shared-memory buffer keeps a content surface in the overlay");

    /* Firefox hides the pop-up by attaching nothing. */
    CHECK(gp_attach(child, NULL) == 1 && !gp_find(child)->shown && !gp_showing(),
          "attaching nothing hides it and gives the window back");
    CHECK(gp_request(child, WL_SURFACE_COMMIT, NULL) == 1, "its commits are still taken");
    CHECK(ov_hit(1500, 100, &ox, &oy) == NULL, "hidden, it takes no clicks");

    /* Shown again in a new pop-up surface of GTK's, at another place. */
    CHECK(gp_reparent(child, popup2) == 1 && gp_find(child)->popup == popup2,
          "shown again, it belongs to the new pop-up surface");
    add_popup(popup2, 300, 200);
    ov_popup_at(popup2, 10, 20);
    CHECK(gp_attach(child, gpu_buf2) == 1 && gp_find(child)->x == 10 &&
              gp_find(child)->y == 20 && gp_find(child)->w == 300 && gp_find(child)->h == 200 &&
              gp_find(child)->shown,
          "it follows the new pop-up's place and size");
    hit = ov_hit(100, 100, &ox, &oy);
    CHECK(hit == (struct wl_surface *)popup2 && ox == 10 && oy == 20, "and takes clicks there");
    CHECK(gp_offset(child, 5, 7) == 1 && ov_hit(12, 24, &ox, &oy) == NULL &&
              ov_hit(15, 27, &ox, &oy) == (struct wl_surface *)popup2,
          "wl_subsurface.set_position offsets its rectangle");
    CHECK(gp_reparent(child2, popup2) == 0, "an overlay surface is not reparented here");

    /* A content surface under a surface that is no pop-up (a second GTK
     * toplevel) stays in the overlay whatever its buffer. */
    {
        struct wl_proxy *win2 = wl_proxy_create(compositor, &wl_surface_interface);
        struct wl_proxy *child3 = wl_proxy_create(compositor, &wl_surface_interface);

        ov_child(child3, win2);
        CHECK(gp_attach(child3, gpu_buf) == 0 && !gp_find(child3) && ov_item_find(child3),
              "a GPU buffer under a non-pop-up surface promotes nothing");
    }

    /* Where the compositor shows the window decides the click rectangle. */
    setenv("WEBOS_XDG_GPU_POPUP_PLACED", "topleft", 1);
    gp_place(gp_find(child));
    CHECK(gp_find(child)->px == 0 && gp_find(child)->py == 0,
          "by default the window is taken to be at the top left");
    CHECK(ov_hit(100, 100, &ox, &oy) == (struct wl_surface *)popup2 && ox == -5 && oy == -7,
          "a click at the top left lands on the pop-up, origin minus the content offset");
    gp_window_moved(NULL, NULL, 500, 400);
    CHECK(gp_find(child)->px == 500 && ov_hit(100, 100, &ox, &oy) == NULL &&
              ov_hit(510, 410, &ox, &oy) == (struct wl_surface *)popup2 && ox == 495,
          "the window's position_changed moves the click rectangle");

    /* The synthetic pointer focus follows clicks into a pop-up. It must not
     * outlive the pop-up's surface because a leave for it would be a use
     * after free in GDK. */
    {
        struct wl_proxy *out = NULL;
        struct virt *pv = add_popup(wl_proxy_create(compositor, &wl_surface_interface), 10, 10);

        synth_focus = (struct wl_surface *)pv->wl_surface;
        virt_teardown(pv);
        CHECK(synth_focus == NULL, "tearing down the xdg_popup drops the focus on its surface");
        synth_focus = (struct wl_surface *)popup2;
        webos_xdg_intercept(popup2, WL_SURFACE_DESTROY, NULL, 1, 0, NULL, &out);
        CHECK(synth_focus == NULL, "destroying the focused wl_surface drops the focus");
    }

    /* Firefox destroys the surface while it shows: the window is given back. */
    CHECK(gp_attach(child, gpu_buf2) == 1 && gp_showing(), "shown again before the destroy");
    gp_destroy(child);
    CHECK(!gp_find(child) && !gp_showing(), "destroy drops it and gives the window back");
    CHECK(gp_reparent(child, popup) == 0, "a destroyed one is not shown again");
    gp_destroy(child2);
    CHECK(ov_item_find(child2), "destroy of an overlay surface changes nothing here");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
