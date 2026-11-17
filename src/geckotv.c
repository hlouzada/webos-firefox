#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "media-helper.h"

/* Entry point. webOS runs this ELF, which sets up the environment and execs
 * the native 32-bit Firefox in firefox-runtime. */

static void mkdir_p(const char *path)
{
    char buf[PATH_MAX];
    size_t len;
    size_t i;

    snprintf(buf, sizeof buf, "%s", path);
    len = strlen(buf);
    for (i = 1; i < len; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        mkdir(buf, 0755);
        buf[i] = '/';
    }
    mkdir(buf, 0755);
}

/* A root shell leaves the profile owned by root. The home-screen jail
 * runs as another user and Firefox then refuses the directory. */
static void give_profile(const char *path, uid_t uid, gid_t gid)
{
    DIR *dir;
    struct dirent *ent;
    struct stat st;
    char child[PATH_MAX];

    if (lstat(path, &st) != 0)
        return;
    lchown(path, uid, gid);
    if (!S_ISDIR(st.st_mode))
        return;
    dir = opendir(path);
    if (!dir)
        return;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof child, "%s/%s", path, ent->d_name);
        give_profile(child, uid, gid);
    }
    closedir(dir);
}

/* The "target" string of the webOS launch parameters, e.g.
 * {"target":"https://example.org/"}. Only http(s) URLs are accepted; JSON
 * escapes are not decoded, and a value containing any is rejected. */
static int launch_target(const char *json, char *out, size_t n)
{
    const char *p = strstr(json, "\"target\"");
    size_t len = 0;

    if (!p)
        return 0;
    p += strlen("\"target\"");
    while (*p == ' ' || *p == ':')
        p++;
    if (*p++ != '"')
        return 0;
    while (p[len] && p[len] != '"' && p[len] != '\\')
        len++;
    if (p[len] != '"' || len == 0 || len >= n)
        return 0;
    if (strncmp(p, "http://", 7) != 0 && strncmp(p, "https://", 8) != 0)
        return 0;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/* A folder the launcher's user can write in, made if missing. */
static int usable_dir(const char *path)
{
    mkdir_p(path);
    return access(path, W_OK | X_OK) == 0;
}

/* Which compositor this is, asked before Firefox starts. The TV's
 * libwayland-client is loaded at run time, so the launcher keeps no
 * link-time dependency on it. */
struct compositor_probe {
    int subcompositor;
    int webos_foreign;
};

struct registry_listener {
    void (*global)(void *data, void *registry, unsigned name, const char *interface,
                   unsigned version);
    void (*global_remove)(void *data, void *registry, unsigned name);
};

static void probe_global(void *data, void *registry, unsigned name, const char *interface,
                         unsigned version)
{
    struct compositor_probe *p = data;

    (void)registry;
    (void)name;
    (void)version;
    if (strcmp(interface, "wl_subcompositor") == 0)
        p->subcompositor = 1;
    else if (strcmp(interface, "wl_webos_foreign") == 0)
        p->webos_foreign = 1;
}

static void probe_global_remove(void *data, void *registry, unsigned name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct registry_listener probe_listener = {probe_global, probe_global_remove};

/* 1 when the adapter will use flat mode (webos-xdg.c flat_mode(): no
 * wl_subcompositor, and wl_webos_foreign or WEBOS_XDG_FLAT=1), 0 when it
 * won't, -1 when the compositor couldn't be asked. */
static int flat_mode_compositor(void)
{
    struct compositor_probe probe = {0, 0};
    void *lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    void *(*display_connect)(const char *);
    void *(*proxy_marshal_constructor)(void *, unsigned, const void *, ...);
    int (*proxy_add_listener)(void *, void (**)(void), void *);
    int (*display_roundtrip)(void *);
    void (*proxy_destroy)(void *);
    void (*display_disconnect)(void *);
    const void *registry_interface;
    const char *forced;
    void *display;
    void *registry;
    int result = -1;

    if (!lib)
        return -1;
    display_connect = (void *(*)(const char *))dlsym(lib, "wl_display_connect");
    proxy_marshal_constructor =
        (void *(*)(void *, unsigned, const void *, ...))dlsym(lib, "wl_proxy_marshal_constructor");
    proxy_add_listener = (int (*)(void *, void (**)(void), void *))dlsym(lib, "wl_proxy_add_listener");
    display_roundtrip = (int (*)(void *))dlsym(lib, "wl_display_roundtrip");
    proxy_destroy = (void (*)(void *))dlsym(lib, "wl_proxy_destroy");
    display_disconnect = (void (*)(void *))dlsym(lib, "wl_display_disconnect");
    registry_interface = dlsym(lib, "wl_registry_interface");
    if (display_connect && proxy_marshal_constructor && proxy_add_listener && display_roundtrip &&
        proxy_destroy && display_disconnect && registry_interface) {
        display = display_connect(NULL);
        if (display) {
            /* wl_display_get_registry (opcode 1), spelled out. */
            registry = proxy_marshal_constructor(display, 1, registry_interface, NULL);
            if (registry) {
                proxy_add_listener(registry, (void (**)(void))&probe_listener, &probe);
                if (display_roundtrip(display) >= 0) {
                    forced = getenv("WEBOS_XDG_FLAT");
                    if (probe.subcompositor)
                        result = 0;
                    else if (forced && forced[0])
                        result = atoi(forced) != 0;
                    else
                        result = probe.webos_foreign;
                }
                proxy_destroy(registry);
            }
            display_disconnect(display);
        }
    }
    dlclose(lib);
    return result;
}

/* Prefs the launcher decides at each start. They go into the profile's
 * user.js and Firefox applies that file at every start.
 *  - WebRender on the GPU or in software. In flat mode (webOS 5 and 6) the
 *    adapter composes Firefox's surfaces into one window from their
 *    shared-memory buffers and the GPU path never gets a frame out. A
 *    webOS 6.5 TV's Mali driver waits forever at the first swap with a
 *    black screen and the GPU probe hangs. Software rendering there and
 *    Firefox's default everywhere else.
 *  - Console output into geckotv.log. This unofficial build dumps every
 *    chrome console message and CSS parse error to stderr by default.
 *    The build's GECKOTV_LOGLEVEL (LOGLEVEL in the Makefile) decides.
 * Every line is rewritten each start and a false value is written out like
 * a true one. Firefox keeps a user.js value in prefs.js, so dropping a line
 * would leave it set. The marker lines start with "// geckotv:" and the
 * user_pref lines that follow one are replaced as a block. */
#ifndef GECKOTV_LOGLEVEL
#define GECKOTV_LOGLEVEL 0
#endif
#define GECKOTV_MARK "// geckotv:"

static void write_launcher_prefs(const char *profile, int software)
{
    char path[PATH_MAX];
    char tmp[PATH_MAX];
    char line[1024];
    FILE *in;
    FILE *out;
    int in_block = 0;
    const char *console = GECKOTV_LOGLEVEL >= 1 ? "true" : "false";

    snprintf(path, sizeof path, "%s/user.js", profile);
    snprintf(tmp, sizeof tmp, "%s/user.js.geckotv-new", profile);
    out = fopen(tmp, "w");
    if (!out)
        return;
    in = fopen(path, "r");
    if (in) {
        while (fgets(line, sizeof line, in)) {
            if (strncmp(line, GECKOTV_MARK, strlen(GECKOTV_MARK)) == 0) {
                in_block = 1;
                continue;
            }
            if (in_block && strncmp(line, "user_pref(", 10) == 0)
                continue;
            in_block = 0;
            fputs(line, out);
        }
        fclose(in);
    }
    fprintf(out,
            "%s WebRender on the GPU or in software, set at each start\n"
            "user_pref(\"gfx.webrender.software\", %s);\n",
            GECKOTV_MARK, software ? "true" : "false");
    fprintf(out,
            "%s console output into geckotv.log, from the build's log level\n"
            "user_pref(\"browser.dom.window.dump.enabled\", %s);\n"
            "user_pref(\"devtools.console.stdout.chrome\", %s);\n"
            "user_pref(\"layout.css.report_errors\", %s);\n",
            GECKOTV_MARK, console, console, console);
    if (fclose(out) == 0)
        rename(tmp, path);
    else
        unlink(tmp);
}

int main(int argc, char **argv)
{
    char exe[PATH_MAX];
    char dir[PATH_MAX];
    char home[PATH_MAX];
    char firefox[PATH_MAX];
    char libpath[PATH_MAX * 2];
    char logpath[PATH_MAX];
    char marker[PATH_MAX];
    char target[2048];
    char *args[12];
    int fd;
    int narg = 0;
    ssize_t n;
    char *slash;
    int gl_window_mode = 0;

    if (argc > 1 && strcmp(argv[1], "--media-helper") == 0)
        return media_helper_main(argc, argv);
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0)
        return 1;
    exe[n] = '\0';
    snprintf(dir, sizeof dir, "%s", exe);
    slash = strrchr(dir, '/');
    if (!slash)
        return 1;
    *slash = '\0';

    /* The profile lives in the app folder. Installed as root (a webOS 6 TV:
     * the folder is root's, mode 755) the app's own user can create nothing
     * there, and Firefox then shows a black window. So fall back to the
     * user's own home, and last to /tmp, which is lost on reboot. */
    {
        const char *user_home = getenv("HOME");

        snprintf(home, sizeof home, "%s/profile", dir);
        if (!usable_dir(home) && user_home && user_home[0] == '/') {
            snprintf(home, sizeof home, "%s/.geckotv-profile", user_home);
            if (!usable_dir(home))
                home[0] = '\0';
        }
        if (!home[0] || !usable_dir(home))
            snprintf(home, sizeof home, "/tmp/com.github.gprot42.geckotv-profile");
        usable_dir(home);
    }

    snprintf(logpath, sizeof logpath, "%s/geckotv.log", dir);
    fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        snprintf(logpath, sizeof logpath, "%s/geckotv.log", home);
        fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }
    fprintf(stderr, "geckotv: profile %s (HOME was %s, uid %u)\n", home,
            getenv("HOME") ? getenv("HOME") : "unset", (unsigned)getuid());
    mkdir_p("/tmp/xdg");
    if (geteuid() == 0) {
        struct stat jail;
        if (stat("/var/palm/jail/com.github.gprot42.geckotv", &jail) == 0 &&
            jail.st_uid != 0)
            give_profile(home, jail.st_uid, jail.st_gid);
    }
    /* User prefs beat the browser's own default-pref files, which is the
     * only way to override a value that browser/omni.ja also sets, such as
     * the extension process. Written once; the user can edit it after. */
    snprintf(marker, sizeof marker, "%s/user.js", home);
    if (access(marker, F_OK) != 0) {
        FILE *uf = fopen(marker, "w");
        if (uf) {
            fputs("// Written by geckotv on first run. Values here override defaults/pref.\n"
                  "user_pref(\"extensions.webextensions.remote\", false);\n", uf);
            fclose(uf);
        }
    }
    snprintf(libpath, sizeof libpath, "%s/.parentlock", home);
    unlink(libpath);
    snprintf(libpath, sizeof libpath, "%s/parent.lock", home);
    unlink(libpath);
    snprintf(firefox, sizeof firefox, "%s/firefox-runtime/firefox", dir);
    /* The runtime's own libraries come first; its copy of libwayland-client
     * carries the webOS shell adapter. Everything else is the TV's. */
    snprintf(libpath, sizeof libpath, "%s/firefox-runtime", dir);
    /* GTK needs libwayland-egl.so.1 to load, and the Mali driver only draws
     * into windows made by the library it was built with. Newer webOS has
     * it under that name. webOS 6.5 ships it as libwayland-egl.so.0 only, so
     * a link under the name GTK asks for goes first in the library path.
     * glibc maps the file once, so Mali and GTK share it. webOS 4 has none
     * and gets the generic one bundled in firefox-runtime/fallback. */
    if (access("/usr/lib/libwayland-egl.so.1", F_OK) != 0 &&
        access("/lib/libwayland-egl.so.1", F_OK) != 0) {
        size_t len = strlen(libpath);
        const char *tv = access("/usr/lib/libwayland-egl.so.0", F_OK) == 0
                             ? "/usr/lib/libwayland-egl.so.0"
                             : access("/lib/libwayland-egl.so.0", F_OK) == 0
                                   ? "/lib/libwayland-egl.so.0" : NULL;
        char linkdir[PATH_MAX];
        char link[PATH_MAX];

        snprintf(linkdir, sizeof linkdir, "%s/.wayland-egl", home);
        snprintf(link, sizeof link, "%s/libwayland-egl.so.1", linkdir);
        if (tv && len + strlen(linkdir) + 2 < sizeof libpath &&
            (mkdir(linkdir, 0755) == 0 || errno == EEXIST) &&
            (unlink(link) == 0 || errno == ENOENT) && symlink(tv, link) == 0) {
            memmove(libpath + strlen(linkdir) + 1, libpath, len + 1);
            memcpy(libpath, linkdir, strlen(linkdir));
            libpath[strlen(linkdir)] = ':';
            fprintf(stderr, "geckotv: libwayland-egl.so.1 is the TV's %s\n", tv);
        } else {
            snprintf(libpath + len, sizeof libpath - len, ":%s/firefox-runtime/fallback", dir);
            fprintf(stderr, "geckotv: no libwayland-egl.so.1 on this TV, using the bundled one\n");
        }
    }
    /* The runtime's GTK finds its image decoders through this cache. */
    snprintf(marker, sizeof marker, "%s/firefox-runtime/gdk-pixbuf/loaders.cache", dir);
    if (access(marker, R_OK) == 0)
        setenv("GDK_PIXBUF_MODULE_FILE", marker, 1);
    /* GTK's Wayland input-method module: it tells the adapter when an
     * editable field gains or loses focus, which opens and closes the webOS
     * keyboard for text fields in pages, not only the address bar. */
    snprintf(marker, sizeof marker, "%s/firefox-runtime/gtk-immodules/immodules.cache", dir);
    if (access(marker, R_OK) == 0)
        setenv("GTK_IM_MODULE_FILE", marker, 1);
    /* GLib data bundled with the runtime rather than taken from the TV, whose
     * copies may be missing or built for another GLib: compiled GSettings
     * schemas (GTK aborts on a missing one), no GIO plug-ins, settings kept
     * in memory, and keyboard-map data for xkbcommon's fallback keymap. */
    snprintf(marker, sizeof marker, "%s/firefox-runtime/glib-schemas/gschemas.compiled", dir);
    if (access(marker, R_OK) == 0) {
        snprintf(marker, sizeof marker, "%s/firefox-runtime/glib-schemas", dir);
        setenv("GSETTINGS_SCHEMA_DIR", marker, 1);
        setenv("GSETTINGS_BACKEND", "memory", 1);
    }
    snprintf(marker, sizeof marker, "%s/firefox-runtime/gio-modules", dir);
    if (access(marker, R_OK) == 0)
        setenv("GIO_MODULE_DIR", marker, 1);
    snprintf(marker, sizeof marker, "%s/firefox-runtime/xkb/rules", dir);
    if (access(marker, R_OK) == 0) {
        snprintf(marker, sizeof marker, "%s/firefox-runtime/xkb", dir);
        setenv("XKB_CONFIG_ROOT", marker, 1);
    }

    setenv("HOME", home, 1);
    /* webOS's app manager passes XDG_CACHE_HOME=/var/cache/xdg, which is
     * root's: as the app's own user Firefox cannot make its cache there and
     * says "Your Firefox profile cannot be loaded". Keep an inherited folder
     * only if this user can write in it. */
    {
        static const char *const vars[] = { "XDG_CONFIG_HOME", "XDG_CACHE_HOME" };
        size_t v;

        for (v = 0; v < sizeof vars / sizeof vars[0]; v++) {
            const char *cur = getenv(vars[v]);

            if (!cur || !cur[0] || !usable_dir(cur)) {
                if (cur && cur[0])
                    fprintf(stderr, "geckotv: %s=%s is not writable, using %s\n", vars[v],
                            cur, home);
                setenv(vars[v], home, 1);
            }
        }
    }
    setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 0);
    setenv("WAYLAND_DISPLAY", "wayland-0", 0);
    setenv("XKB_CONFIG_ROOT", "/usr/share/X11/xkb", 0);
    setenv("GDK_BACKEND", "wayland", 1);
    setenv("MOZ_ENABLE_WAYLAND", "1", 1);
    setenv("MOZ_DBUS_REMOTE", "0", 1);
    setenv("MOZ_DISABLE_CONTENT_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_GMP_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_RDD_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_SOCKET_PROCESS_SANDBOX", "1", 1);
    setenv("MOZ_DISABLE_GPU_SANDBOX", "1", 1);
    setenv("LD_LIBRARY_PATH", libpath, 1);
    unsetenv("LD_PRELOAD");
    /* glibc before 2.25 (webOS 4 has 2.24) has no getrandom(). Firefox's Rust
     * code then waits on /dev/random, which webOS 4's app jail lacks, and
     * aborts; preload one that makes the system call (src/getrandom-compat.c). */
    {
        void *libc = dlopen("libc.so.6", RTLD_LAZY | RTLD_NOLOAD);

        if (!libc || !dlsym(libc, "getrandom")) {
            snprintf(marker, sizeof marker, "%s/firefox-runtime/fallback/libgetrandom-compat.so",
                     dir);
            if (access(marker, R_OK) == 0) {
                setenv("LD_PRELOAD", marker, 1);
                fprintf(stderr, "geckotv: no getrandom() in this glibc, preloading %s\n", marker);
            }
        }
        if (libc)
            dlclose(libc);
    }
    /* Wire-level trace: touch <app dir>/wayland-debug, launch once, then
     * delete the file. libwayland prints every message to stderr, which
     * is geckotv.log. */
    snprintf(marker, sizeof marker, "%s/wayland-debug", dir);
    if (access(marker, F_OK) == 0) {
        setenv("WAYLAND_DEBUG", "1", 1);
        fprintf(stderr, "geckotv WAYLAND_DEBUG=1 because %s exists\n", marker);
    }
    /* Extra environment for one run, e.g. MOZ_LOG=cubeb:4 to trace a media
     * problem: one KEY=VALUE per line in <app dir>/env. Delete the file
     * afterwards; the log it produces can grow fast. */
    snprintf(marker, sizeof marker, "%s/env", dir);
    {
        FILE *ef = fopen(marker, "r");
        if (ef) {
            char line[1024];
            while (fgets(line, sizeof line, ef)) {
                char *eq = strchr(line, '=');
                char *nl = strchr(line, '\n');
                if (nl)
                    *nl = '\0';
                if (!eq || line[0] == '#' || eq == line)
                    continue;
                *eq = '\0';
                setenv(line, eq + 1, 1);
                fprintf(stderr, "geckotv env %s=%s\n", line, eq + 1);
            }
            fclose(ef);
        }
    }
    setenv("APPID", "com.github.gprot42.geckotv", 0);

    /* GPU or software WebRender for this compositor (write_launcher_prefs).
     * GECKOTV_SOFTWARE=0|1 in <app dir>/env overrides the choice. */
    {
        const char *forced = getenv("GECKOTV_SOFTWARE");
        int flat = flat_mode_compositor();
        int software;

        /* A flat-mode compositor shows what the GPU draws only through the
         * adapter's GL window mode (src/webos-xdg.c, gl_mode). That mode
         * needs the TV's own wayland-egl library for the Mali driver. */
        int wegl = access("/usr/lib/libwayland-egl.so.1", F_OK) == 0 ||
                   access("/usr/lib/libwayland-egl.so.0", F_OK) == 0 ||
                   access("/lib/libwayland-egl.so.1", F_OK) == 0 ||
                   access("/lib/libwayland-egl.so.0", F_OK) == 0;
        int gl = 0;

        if (forced && forced[0])
            software = atoi(forced) != 0;
        else
            software = flat == 1 && !wegl;
        if (!software && flat == 1) {
            gl = 1;
            gl_window_mode = 1;
            setenv("WEBOS_XDG_GL", "1", 1);
            setenv("WEBOS_XDG_SIZE", "1920x1080", 0);
        } else {
            unsetenv("WEBOS_XDG_GL");
        }
        if (flat >= 0 || (forced && forced[0])) {
            write_launcher_prefs(home, software);
            fprintf(stderr, "geckotv: %s rendering (%s)%s\n", software ? "software" : "GPU",
                    forced && forced[0] ? "GECKOTV_SOFTWARE"
                    : flat != 1         ? "compositor has wl_subcompositor or no wl_webos_foreign"
                    : wegl              ? "flat-mode compositor with wayland-egl"
                                        : "flat-mode compositor without wayland-egl",
                    gl ? ", GL window mode" : "");
        } else {
            fprintf(stderr, "geckotv: compositor not reachable before start; rendering left as is\n");
        }
    }

    /* Hardware video (src/media-helper.cc, findings.md section 21) in GL
     * window mode on a TV with LG's Starfish pipeline library. Firefox's
     * media engine starts the helper itself and the adapter listens on the
     * socket for the video's place. GECKOTV_HWVIDEO=0|1 in the env file
     * overrides. The socket lives in the profile folder under a name no
     * other process knows, and only the app's user can read that folder. */
    {
        const char *forced = getenv("GECKOTV_HWVIDEO");
        int starfish = access("/usr/lib/libplayerAPIs.so.1", R_OK) == 0;
        int on = forced && forced[0] ? atoi(forced) != 0 : gl_window_mode && starfish;
        DIR *dir = opendir(home);

        if (dir) {
            struct dirent *ent;

            while ((ent = readdir(dir)) != NULL)
                if (strncmp(ent->d_name, ".geckotv-video-", 15) == 0) {
                    snprintf(marker, sizeof marker, "%s/%s", home, ent->d_name);
                    unlink(marker);
                }
            closedir(dir);
        }
        if (on) {
            unsigned tag = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16);
            int rfd = open("/dev/urandom", O_RDONLY);

            if (rfd >= 0) {
                if (read(rfd, &tag, sizeof tag) != (ssize_t)sizeof tag)
                    tag ^= (unsigned)getpid();
                close(rfd);
            }
            snprintf(marker, sizeof marker, "%s/.geckotv-video-%08x", home, tag);
            setenv("GECKOTV_MEDIA_HELPER", exe, 1);
            setenv("GECKOTV_VIDEO_SOCKET", marker, 1);
            fprintf(stderr, "geckotv: hardware video on (%s)\n",
                    forced && forced[0] ? "GECKOTV_HWVIDEO" : "GL window mode and Starfish present");
        } else {
            unsetenv("GECKOTV_MEDIA_HELPER");
            unsetenv("GECKOTV_VIDEO_SOCKET");
            fprintf(stderr, "geckotv: hardware video off (%s)\n",
                    forced && forced[0] ? "GECKOTV_HWVIDEO"
                    : !starfish        ? "no libplayerAPIs.so.1"
                                       : "not in GL window mode");
        }
    }

    /* Started by hand as root: Firefox refuses to run as root with a profile
     * owned by the app's own user and only says "Running Nightly as root". */
    {
        struct stat st;
        if (geteuid() == 0 && stat(home, &st) == 0 && st.st_uid != 0)
            fprintf(stderr, "geckotv: running as root, but the profile belongs to uid %u; "
                            "Firefox will refuse. Start the app from the TV (or ares-launch) "
                            "instead of running geckotv from a root shell.\n",
                    (unsigned)st.st_uid);
    }
    fprintf(stderr, "geckotv exec %s\n", firefox);
    args[narg++] = firefox;
    args[narg++] = "--profile";
    args[narg++] = home;
    args[narg++] = "--no-remote";
    /* Remote control for debugging from the Mac: touch <app dir>/marionette.
     * Off by default because it sets navigator.webdriver on every page, and
     * YouTube then stops sending video after about a minute. */
    snprintf(marker, sizeof marker, "%s/marionette", dir);
    if (access(marker, F_OK) == 0) {
        args[narg++] = "--marionette";
        /* Lets a Marionette client switch to chrome context, which is how
         * prefs are read and set on the live browser from the Mac. */
        args[narg++] = "-remote-allow-system-access";
        fprintf(stderr, "geckotv marionette on because %s exists\n", marker);
    }
    if (argc > 1 && argv[1][0] != '{') {
        int i;
        for (i = 1; i < argc && narg < 10; i++)
            args[narg++] = argv[i];
    } else if (argc > 1 && launch_target(argv[1], target, sizeof target)) {
        args[narg++] = target;
    } else {
        args[narg++] = "https://example.com";
    }
    args[narg] = NULL;
    /* Drop the Magic Remote wheel on the compositor so it scrolls this
     * window instead of leaving the app. Needs root; the boot script
     * covers the normal jail. */
    if (geteuid() == 0) {
        char mask[PATH_MAX];
        snprintf(mask, sizeof mask, "%s/mask-wheel", dir);
        if (access(mask, X_OK) == 0)
            system(mask);
    }
    if (access("/usr/bin/luna-send", X_OK) == 0) {
        system("/usr/bin/luna-send -n 1 -w 2000 "
               "luna://com.webos.service.mrcu/setHotkeyCursorPolicy "
               "'{\"button\":19,\"cursorPolicy\":2}' >/tmp/mrcu-policy.log 2>&1");
    }
    execv(firefox, args);
    perror("execv firefox");
    return 1;
}
