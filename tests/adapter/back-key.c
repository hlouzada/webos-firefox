/* Host test of what the remote's Back key is passed on to GTK as.
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

static uint32_t got_key[8], got_state[8];
static int got;

static void on_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                   uint32_t key, uint32_t state)
{
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)time;
    if (got < 8) {
        got_key[got] = key;
        got_state[got] = state;
    }
    got++;
}

static const struct wl_keyboard_listener listener = { NULL, NULL, NULL, on_key, NULL, NULL };

static void press(struct wl_keyboard *kb, uint32_t key)
{
    got = 0;
    keyboard_key(NULL, kb, ++g.serial, 0, key, WL_KEYBOARD_KEY_STATE_PRESSED);
    keyboard_key(NULL, kb, ++g.serial, 0, key, WL_KEYBOARD_KEY_STATE_RELEASED);
}

static int passed_as(uint32_t key)
{
    return got == 2 && got_key[0] == key && got_state[0] == WL_KEYBOARD_KEY_STATE_PRESSED &&
           got_key[1] == key && got_state[1] == WL_KEYBOARD_KEY_STATE_RELEASED;
}

int main(void)
{
    int fake_kb, fake_tl, fake_popup, fake_sub;
    struct wl_keyboard *kb = (struct wl_keyboard *)&fake_kb;

    keyboard_hooks[0].proxy = (struct wl_proxy *)&fake_kb;
    keyboard_hooks[0].listener = &listener;

    press(kb, KEY_A);
    CHECK(passed_as(KEY_A), "an ordinary key is passed on");

    press(kb, KEY_BACK);
    CHECK(passed_as(KEY_BACK), "Back on a page: passed on as KEY_BACK (XF86Back, the previous page)");

    press(kb, KEY_PREVIOUS);
    CHECK(passed_as(KEY_BACK), "Back as webOS 6.5 sends it (KEY_PREVIOUS, 412): passed on as KEY_BACK");

    press(kb, KEY_EXIT);
    CHECK(got == 0, "Exit is swallowed");

    g.virts[0].proxy = (struct wl_proxy *)&fake_tl;
    g.virts[0].kind = V_TOPLEVEL;
    g.virts[0].fullscreen = 1;
    press(kb, KEY_BACK);
    CHECK(passed_as(KEY_ESC), "Back while the toplevel is fullscreen: Escape");

    g.virts[0].fullscreen = 0;
    press(kb, KEY_BACK);
    CHECK(passed_as(KEY_BACK), "Back after unset_fullscreen: the previous page again");

    g.virts[1].proxy = (struct wl_proxy *)&fake_popup;
    g.virts[1].kind = V_POPUP;
    g.virts[1].subsurface = (struct wl_subsurface *)&fake_sub;
    press(kb, KEY_BACK);
    CHECK(passed_as(KEY_ESC), "Back while a menu is open: Escape");

    /* The release follows the press's choice even if the menu closed between. */
    got = 0;
    keyboard_key(NULL, kb, ++g.serial, 0, KEY_BACK, WL_KEYBOARD_KEY_STATE_PRESSED);
    g.virts[1].proxy = NULL;
    keyboard_key(NULL, kb, ++g.serial, 0, KEY_BACK, WL_KEYBOARD_KEY_STATE_RELEASED);
    CHECK(passed_as(KEY_ESC), "the release matches the press when the menu closes in between");

    keyboard_up = 1;
    press(kb, KEY_BACK);
    CHECK(got == 0, "Back while the webOS keyboard is up: swallowed");
    keyboard_up = 0;

    note_back_hid_keyboard();
    press(kb, KEY_BACK);
    CHECK(got == 0, "Back just after the evdev Back closed the keyboard: swallowed");

    back_hid_keyboard.tv_sec -= 2;
    press(kb, KEY_BACK);
    CHECK(passed_as(KEY_BACK), "and two seconds later: the previous page");

    printf("%d failure(s)\n", failures);
    return failures != 0;
}
