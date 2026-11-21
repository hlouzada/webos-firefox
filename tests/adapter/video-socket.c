/* Host test of the adapter's video socket: one engine at a time, the newest
 * connection takes over from the one before it, and a client that hangs up
 * makes room. Built by run.sh with the whole adapter included. */
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

static int connect_client(const char *path)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        perror("connect");
        exit(2);
    }
    return fd;
}

/* What the client has received within 200 ms, as a string. */
static const char *received(int fd)
{
    static char buf[256];
    struct pollfd p = { fd, POLLIN, 0 };
    ssize_t n;

    if (poll(&p, 1, 200) <= 0)
        return "<nothing>";
    n = recv(fd, buf, sizeof buf - 1, 0);
    if (n == 0)
        return "<eof>";
    if (n < 0)
        return strerror(errno);
    buf[n] = '\0';
    return buf;
}

int main(void)
{
    char dir[] = "/tmp/adapter-video-XXXXXX";
    char path[sizeof dir + 8];
    int a, b;

    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(path, sizeof path, "%s/sock", dir);
    setenv("GECKOTV_VIDEO_SOCKET", path, 1);
    video_socket_ensure();
    CHECK(video_listen_fd >= 0, "the adapter listens on GECKOTV_VIDEO_SOCKET");
    strcpy(video_window_id, "_Window_Id_1");

    a = connect_client(path);
    video_accept();
    CHECK(!strcmp(received(a), "windowid _Window_Id_1\n"), "the first engine gets the window id");

    b = connect_client(path);
    video_accept();
    CHECK(!strcmp(received(b), "windowid _Window_Id_1\n"),
          "an engine connecting while the first is still around gets the window id");
    CHECK(!strcmp(received(a), "<eof>"), "the first engine is hung up on");

    close(b);
    video_accept();
    CHECK(video_client_fd < 0, "a client that hangs up leaves the slot free");

    close(a);
    unlink(path);
    rmdir(dir);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
