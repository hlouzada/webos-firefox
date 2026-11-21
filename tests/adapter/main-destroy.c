/* Host test of what happens when GTK destroys its toplevel wl_surface, as it
 * does when the window is hidden or closed. The adapter names that surface
 * in every pointer enter and leave it synthesizes for the overlay and the GL
 * window, in the text-input enter and in the held-back keyboard leave. Once
 * the proxy is freed, a GDK handler reading the surface's user data is a use
 * after free (GDK_IS_WINDOW on 0xe5e5e5e5 poison). Built by run.sh with the
 * whole adapter included; the proxies live on a wl_display over a socketpair
 * nobody reads. */
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

/* GDK's pointer listener, recording the surface of every enter and leave. */
static struct wl_surface *entered[16], *left[16];
static int nenter, nleave;

static void rec_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                      struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)pointer; (void)serial; (void)x; (void)y;
    if (nenter < 16)
        entered[nenter] = surface;
    nenter++;
}

static void rec_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                      struct wl_surface *surface)
{
    (void)data; (void)pointer; (void)serial;
    if (nleave < 16)
        left[nleave] = surface;
    nleave++;
}

static void rec_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x,
                       wl_fixed_t y)
{
    (void)data; (void)pointer; (void)time; (void)x; (void)y;
}

static const struct wl_pointer_listener gdk_pointer = {
    .enter = rec_enter,
    .leave = rec_leave,
    .motion = rec_motion,
};

static int named(struct wl_surface **list, int n, struct wl_surface *s)
{
    int i, k = 0;

    for (i = 0; i < n && i < 16; i++)
        if (list[i] == s)
            k++;
    return k;
}

int main(void)
{
    int sv[2];
    struct wl_display *display;
    struct wl_proxy *compositor, *gtk_window, *content, *seat, *pointer, *out = NULL;
    struct wl_surface *main_was;

    setenv("WEBOS_XDG_GL", "1", 1);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        return 2;
    display = wl_display_connect_to_fd(sv[0]);
    compositor = wl_proxy_create((struct wl_proxy *)display, &wl_compositor_interface);
    g.shell = (struct wl_shell *)wl_proxy_create((struct wl_proxy *)display, &wl_shell_interface);
    g.webos = (struct wl_webos_shell *)wl_proxy_create((struct wl_proxy *)display,
                                                       &wl_webos_shell_interface);
    host_foreign_name = 6;          /* flat mode, as on a webOS 6.5 TV */
    host_compositor_name = 1;
    win_w = 1920;
    win_h = 1080;
    gtk_window = wl_proxy_create(compositor, &wl_surface_interface);
    content = wl_proxy_create(compositor, &wl_surface_interface);
    seat = wl_proxy_create((struct wl_proxy *)display, &wl_seat_interface);
    pointer = wl_proxy_create(seat, &wl_pointer_interface);
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
    /* GDK's wl_pointer on the primary seat, hooked by the adapter. */
    pointer_hooks[0].proxy = pointer;
    pointer_hooks[0].listener = &gdk_pointer;
    keyboard_hooks[0].proxy = wl_proxy_create(seat, &wl_keyboard_interface);
    keyboard_hooks[0].listener = &gdk_pointer;      /* any non-NULL listener */

    CHECK(gl_mode() && flat_mode(), "GL window mode on a flat-mode compositor");

    /* GTK maps its window; Firefox's content surface becomes the card. */
    gl_main_mapped(NULL, gtk_window);
    gl_window = content;
    main_was = main_surface;
    CHECK(main_surface == (struct wl_surface *)gtk_window && gl_main == gtk_window && mapped_one,
          "GTK's window is the main surface");

    /* The pointer comes onto the overlay: GTK is told it entered its window. */
    pointer_enter(NULL, (struct wl_pointer *)pointer, 1, ov_surface, wl_fixed_from_int(100),
                  wl_fixed_from_int(100));
    CHECK(nenter == 1 && entered[0] == main_was, "a real enter on the overlay names GTK's window");
    CHECK(synth_focus == main_was, "and the synthetic focus is on it");
    keyboard_focus_raw = main_was;
    keyboard_hooks[0].held_leave = 1;
    text_input_entered = 1;

    /* GTK hides the window: xdg roles go first, then the wl_surface. */
    webos_xdg_intercept(gtk_window, WL_SURFACE_DESTROY, NULL, 1, 0, NULL, &out);
    CHECK(main_surface == NULL, "destroying GTK's window surface forgets the main surface");
    CHECK(synth_focus == NULL, "the synthetic pointer focus is dropped");
    CHECK(keyboard_focus_raw == NULL, "the raw keyboard focus is dropped");
    CHECK(keyboard_hooks[0].held_leave == 0, "a held-back keyboard leave is dropped");
    CHECK(text_input_entered == 0, "the next window gets a text-input enter again");
    CHECK(mapped_one == 0 && gl_main == NULL, "the next GTK window maps as the first one did");

    /* Events keep coming for the overlay and the card; none may name the
     * freed surface. */
    nenter = nleave = 0;
    pointer_enter(NULL, (struct wl_pointer *)pointer, 2, ov_surface, wl_fixed_from_int(100),
                  wl_fixed_from_int(100));
    pointer_motion(NULL, (struct wl_pointer *)pointer, 3, wl_fixed_from_int(120),
                   wl_fixed_from_int(120));
    pointer_leave(NULL, (struct wl_pointer *)pointer, 4, ov_surface);
    pointer_enter(NULL, (struct wl_pointer *)pointer, 5, (struct wl_surface *)content,
                  wl_fixed_from_int(10), wl_fixed_from_int(10));
    pointer_leave(NULL, (struct wl_pointer *)pointer, 6, (struct wl_surface *)content);
    ov_route_primary(300, 300, 1, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    text_input_enter_main();
    keyboard_hooks[0].held_leave = 1;
    keyboard_closed();
    CHECK(named(entered, nenter, main_was) == 0 && named(left, nleave, main_was) == 0,
          "no enter or leave names the destroyed surface (%d enters, %d leaves)", nenter,
          nleave);
    CHECK(synth_focus == NULL, "the synthetic focus stays off the destroyed surface");

    /* GTK shows the window again with a new surface. */
    gtk_window = wl_proxy_create(compositor, &wl_surface_interface);
    gl_main_mapped(NULL, gtk_window);
    nenter = 0;
    pointer_enter(NULL, (struct wl_pointer *)pointer, 7, ov_surface, wl_fixed_from_int(100),
                  wl_fixed_from_int(100));
    CHECK(main_surface == (struct wl_surface *)gtk_window && nenter == 1 &&
              entered[0] == (struct wl_surface *)gtk_window,
          "the new window is the main surface and gets the enters");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
