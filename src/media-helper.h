#ifndef MEDIA_HELPER_H
#define MEDIA_HELPER_H

#ifdef __cplusplus
extern "C" {
#endif

/* geckotv --media-helper plays one stream for Firefox through LG's Starfish
 * media pipeline. Firefox feeds it over the socket on fd 3
 * (src/media-protocol.h). */
int media_helper_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif
