/* Host test of the Magic Remote wheel notch read from evdev, and of which
 * seat a compositor wheel reaches GTK on while the pointer is on the pop-up
 * overlay. Built by run.sh with the whole adapter included. */
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

static int got_axis_calls, seat0_frames, seat1_axis_calls, seat1_frames;
static const void *seat0_axis_proxy;

static void on_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                    wl_fixed_t value)
{
    (void)data; (void)time; (void)axis; (void)value;
    got_axis_calls++;
    seat0_axis_proxy = pointer;
}

static void seat0_frame(void *data, struct wl_pointer *pointer)
{
    (void)data; (void)pointer;
    seat0_frames++;
}

static const struct wl_pointer_listener listener = {
    NULL, NULL, NULL, NULL, on_axis, seat0_frame, NULL, NULL, NULL, NULL, NULL,
};

static void seat1_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                       wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
    seat1_axis_calls++;
}

static void seat1_frame(void *data, struct wl_pointer *pointer)
{
    (void)data; (void)pointer;
    seat1_frames++;
}

static const struct wl_pointer_listener seat1_listener = {
    NULL, NULL, NULL, NULL, seat1_axis, seat1_frame, NULL, NULL, NULL, NULL, NULL,
};

int main(void)
{
    int fake_ptr, fake_ptr2;

    pointer_hooks[0].proxy = (struct wl_proxy *)&fake_ptr;
    pointer_hooks[0].listener = &listener;
    win_w = 1920;
    win_h = 1080;
    mapped_one = 1;

    /* While the compositor is sending pointer events, it sends the wheel as
     * its own wl_pointer.axis too, so the evdev notch must not be injected
     * on top of it. */
    note_real_pointer();
    apply_remote_event((struct ptr_ev){ .button = -2, .down = 1, .x = 960, .y = 540 });
    CHECK(got_axis_calls == 0, "a notch while the real pointer is live is not injected (%d)",
          got_axis_calls);
    last_real_pointer.tv_sec -= 60;
    apply_remote_event((struct ptr_ev){ .button = -2, .down = 1, .x = 960, .y = 540 });
    CHECK(got_axis_calls == 1, "a notch with the real pointer silent for a minute is injected (%d)",
          got_axis_calls);

    /* The Magic Remote's pointer arrives on a secondary seat. While it is on
     * the overlay, enter, motion and button are delivered on the primary seat
     * (GTK binds new in-process content there only), so the wheel must reach
     * GTK on the same seat or the page scrolls nowhere. */
    pointer_hooks[1].proxy = (struct wl_proxy *)&fake_ptr2;
    pointer_hooks[1].listener = &seat1_listener;
    got_axis_calls = seat0_frames = seat1_axis_calls = seat1_frames = 0;
    ov_ptr_on = 1;
    pointer_axis(NULL, (struct wl_pointer *)&fake_ptr2, 0, WL_POINTER_AXIS_VERTICAL_SCROLL,
                 wl_fixed_from_int(10));
    pointer_frame(NULL, (struct wl_pointer *)&fake_ptr2);
    CHECK(got_axis_calls == 1 && seat0_frames == 1 && seat0_axis_proxy == &fake_ptr,
          "a wheel on seat 1 while on the overlay reaches GTK on the primary seat "
          "(seat0 axis %d frame %d)", got_axis_calls, seat0_frames);
    CHECK(seat1_axis_calls == 0 && seat1_frames == 0,
          "and not on seat 1 (axis %d frame %d)", seat1_axis_calls, seat1_frames);

    got_axis_calls = seat0_frames = seat1_axis_calls = seat1_frames = 0;
    ov_ptr_on = 0;
    pointer_axis(NULL, (struct wl_pointer *)&fake_ptr2, 0, WL_POINTER_AXIS_VERTICAL_SCROLL,
                 wl_fixed_from_int(10));
    pointer_frame(NULL, (struct wl_pointer *)&fake_ptr2);
    CHECK(seat1_axis_calls == 1 && seat1_frames == 1 && got_axis_calls == 0,
          "off the overlay the wheel stays on the seat it arrived on "
          "(seat1 axis %d frame %d, seat0 axis %d)", seat1_axis_calls, seat1_frames,
          got_axis_calls);

    printf("%d failure(s)\n", failures);
    return failures != 0;
}
