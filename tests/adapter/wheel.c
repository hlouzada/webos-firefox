/* Host test of what one Magic Remote wheel notch is passed on to GTK as.
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

static int got_source, got_discrete, got_value120, got_frames;
static uint32_t got_source_value, got_axis;
static wl_fixed_t got_axis_value;
static int got_axis_calls;

static void on_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                    wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time;
    got_axis = axis;
    got_axis_value = value;
    got_axis_calls++;
}

static void on_frame(void *data, struct wl_pointer *pointer)
{
    (void)data; (void)pointer;
    got_frames++;
}

static void on_axis_source(void *data, struct wl_pointer *pointer, uint32_t source)
{
    (void)data; (void)pointer;
    got_source++;
    got_source_value = source;
}

static void on_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis;
    got_discrete = discrete;
}

static void on_axis_value120(void *data, struct wl_pointer *pointer, uint32_t axis, int32_t value120)
{
    (void)data; (void)pointer; (void)axis;
    got_value120 = value120;
}

static const struct wl_pointer_listener listener = {
    NULL, NULL, NULL, NULL, on_axis, on_frame, on_axis_source, NULL, on_axis_discrete,
    on_axis_value120, NULL,
};

static void notch(int linux_value)
{
    got_source = got_discrete = got_value120 = got_frames = got_axis_calls = 0;
    deliver_wheel(&pointer_hooks[0], linux_value, 0, 0);
}

int main(void)
{
    int fake_ptr;

    pointer_hooks[0].proxy = (struct wl_proxy *)&fake_ptr;
    pointer_hooks[0].listener = &listener;

    /* GTK divides a wl_pointer.axis value by 10 to get wheel ticks, so 10 is
     * one tick, and Firefox scrolls three lines per tick as for a desktop
     * mouse. Anything larger scrolls that many times further per notch. */
    notch(-1);
    CHECK(got_axis_calls == 1 && got_axis == WL_POINTER_AXIS_VERTICAL_SCROLL,
          "a notch is one vertical axis event");
    CHECK(got_axis_value == wl_fixed_from_int(10),
          "wheel towards the user: axis +10 (one GTK tick), got %g",
          wl_fixed_to_double(got_axis_value));
    CHECK(got_discrete == 1 && got_value120 == 120, "with discrete +1 and value120 +120");
    CHECK(got_source == 1 && got_source_value == WL_POINTER_AXIS_SOURCE_WHEEL,
          "from a wheel source");
    CHECK(got_frames == 1, "and one frame");

    notch(1);
    CHECK(got_axis_value == wl_fixed_from_int(-10),
          "wheel away from the user: axis -10, got %g", wl_fixed_to_double(got_axis_value));
    CHECK(got_discrete == -1 && got_value120 == -120, "with discrete -1 and value120 -120");

    /* While the compositor is sending pointer events, it sends the wheel as
     * its own wl_pointer.axis too, so the evdev notch must not be injected
     * on top of it. */
    win_w = 1920;
    win_h = 1080;
    mapped_one = 1;
    got_axis_calls = 0;
    note_real_pointer();
    apply_remote_event((struct ptr_ev){ .button = -2, .down = 1, .x = 960, .y = 540 });
    CHECK(got_axis_calls == 0, "a notch while the real pointer is live is not injected (%d)",
          got_axis_calls);
    last_real_pointer.tv_sec -= 60;
    apply_remote_event((struct ptr_ev){ .button = -2, .down = 1, .x = 960, .y = 540 });
    CHECK(got_axis_calls == 1, "a notch with the real pointer silent for a minute is injected (%d)",
          got_axis_calls);

    printf("%d failure(s)\n", failures);
    return failures != 0;
}
