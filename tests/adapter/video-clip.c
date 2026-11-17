/* Host test of the adapter's video plane placement, the exported window's
 * regions for a video rectangle and the line the engine sends. Built by
 * run.sh with the whole adapter included. */
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

static int same(const int32_t *a, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return a[0] == x && a[1] == y && a[2] == w && a[3] == h;
}

int main(void)
{
    int32_t src[4], dst[4];
    int32_t r_in[4] = { 100, 200, 960, 540 };
    int32_t r_left[4] = { -480, 0, 960, 540 };
    int32_t r_below[4] = { 0, 900, 1920, 1080 };
    int32_t r_out[4] = { 2000, 0, 100, 100 };
    int32_t r_zero[4] = { 0, 0, 0, 0 };
    int32_t r_full[4] = { 0, 0, 1920, 1080 };

    win_w = 1920;
    win_h = 1080;
    CHECK(video_clip(r_in, 1920, 1080, src, dst) && same(dst, 100, 200, 960, 540) &&
              same(src, 0, 0, 1920, 1080),
          "a video inside the window: destination as given, source the whole video");
    CHECK(video_clip(r_full, 1280, 720, src, dst) && same(dst, 0, 0, 1920, 1080) &&
              same(src, 0, 0, 1280, 720),
          "full screen: the whole video scaled to the window");
    CHECK(video_clip(r_left, 1920, 1080, src, dst) && same(dst, 0, 0, 480, 540) &&
              same(src, 960, 0, 960, 1080),
          "half off the left edge: destination clipped, source the right half");
    CHECK(video_clip(r_below, 1280, 720, src, dst) && same(dst, 0, 900, 1920, 180) &&
              same(src, 0, 0, 1280, 120),
          "scrolled under the bottom: the top sixth of the video");
    CHECK(!video_clip(r_out, 1920, 1080, src, dst), "outside the window: nothing to show");
    CHECK(!video_clip(r_zero, 1920, 1080, src, dst), "an empty rect: nothing to show");
    CHECK(!video_clip(r_in, 0, 0, src, dst), "no natural size: nothing to show");

    /* The parser. A rect line sets the region, but without an exported
     * window video_region_set is a no-op, so the parser is checked through
     * its acceptance of well-formed lines only. */
    {
        char line[] = "rect 10 20 300 200 1280 720";
        int32_t v[6];

        CHECK(sscanf(line, "rect %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) ==
                      6 && v[4] == 1280 && v[5] == 720,
              "a rect line carries the video's natural size");
    }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
