/* Host test of the Magic Remote wheel notch read from evdev. Built by run.sh
 * with the whole adapter included. */
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

static int got_axis_calls;

static void on_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                    wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
    got_axis_calls++;
}

static const struct wl_pointer_listener listener = {
    NULL, NULL, NULL, NULL, on_axis, NULL, NULL, NULL, NULL, NULL, NULL,
};

int main(void)
{
    int fake_ptr;

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

    printf("%d failure(s)\n", failures);
    return failures != 0;
}
