/* Host test of the crash handler's /proc/self/maps reader. A Firefox parent
 * has thousands of mappings, so the reader must stream the file and keep
 * only the code mappings, and resolve a late-mapped library to the same
 * library+offset that dladdr gives. Built by run.sh with the whole adapter
 * included. */
#include "../../src/webos-xdg.c"
#include <dlfcn.h>

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

/* What crash_resolve() prints for an address, captured from stderr. */
static void resolve_text(unsigned long a, char *out, size_t n)
{
    char path[] = "/tmp/crash-maps-XXXXXX";
    int fd = mkstemp(path), saved = dup(STDERR_FILENO);
    ssize_t r;

    fflush(stderr);
    dup2(fd, STDERR_FILENO);
    crash_resolve("test", a);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    lseek(fd, 0, SEEK_SET);
    r = read(fd, out, n - 1);
    out[r > 0 ? r : 0] = '\0';
    close(fd);
    unlink(path);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');

    return s ? s + 1 : p;
}

static void check_symbol(const char *what, void *sym)
{
    Dl_info info;
    char text[300], want[300];

    CHECK(dladdr(sym, &info) && info.dli_fname, "%s: dladdr knows it", what);
    resolve_text((unsigned long)sym, text, sizeof text);
    if (info.dli_fbase)
        snprintf(want, sizeof want, "%s+%#lx", base_name(info.dli_fname),
                 (unsigned long)sym - (unsigned long)info.dli_fbase);
    else
        snprintf(want, sizeof want, "%s+", base_name(info.dli_fname));
    CHECK(strstr(text, want) != NULL, "%s resolves to %s (got %s)", what, want,
          strchr(text, '\n') ? strtok(text, "\n") : text);
}

int main(void)
{
    /* Thousands of small mappings, mapped after the libraries: on Linux
     * they sort before libc in /proc/self/maps and push its lines past
     * where a fixed buffer ends. Alternating protections keep the kernel
     * from merging them. */
    size_t page = (size_t)sysconf(_SC_PAGESIZE), n = 6000, i;
    char *region = mmap(NULL, n * page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    volatile int local = 0;
    char line[600];
    int fd, before, k;
    off_t size = 0;

    CHECK(region != MAP_FAILED, "mapped %zu pages", n);
    for (i = 1; i < n; i += 2)
        mprotect(region + i * page, page, PROT_NONE);
    fd = open("/proc/self/maps", O_RDONLY);
    while ((k = read(fd, line, sizeof line)) > 0)
        size += k;
    close(fd);
    CHECK(size > 192 * 1024, "/proc/self/maps is %ld bytes, more than the old 192 KB buffer",
          (long)size);

    crash_load_maps((unsigned long)&local);
    CHECK(crash_nexec > 0 && !crash_nexec_lost, "%d code mappings, none lost", crash_nexec);
    CHECK(crash_stack_lo <= (unsigned long)&local && (unsigned long)&local < crash_stack_hi,
          "the stack mapping holds a local variable");
    check_symbol("main", (void *)main);
    check_symbol("a libc function", (void *)strlen);
    CHECK(crash_in_libc((unsigned long)strlen), "strlen is in libc");
    CHECK(!crash_in_libc((unsigned long)main), "main is not");
    CHECK(!crash_in_code((unsigned long)region), "a data mapping is not code");

    /* Parsing, on synthetic lines. */
    crash_nexec = crash_nfile = crash_nexec_lost = 0;
    crash_map_line(strcpy(line, "b6f00000-b6f10000 r-xp 00000000 b3:35 123    /usr/lib/libmali.so.0.1"),
                   0);
    crash_map_line(strcpy(line, "b6f10000-b6f20000 r--p 00010000 b3:35 123    /usr/lib/libmali.so.0.1"),
                   0);
    crash_map_line(strcpy(line, "b7000000-b7001000 r-xp 00000000 00:00 0 "), 0);
    crash_map_line(strcpy(line, "b7001000-b7002000 rw-p 00000000 00:00 0    [heap]"), 0);
    CHECK(crash_nexec == 2 && crash_nfile == 1, "two code mappings, one file (%d, %d)",
          crash_nexec, crash_nfile);
    resolve_text(0xb6f08000, line, sizeof line);
    CHECK(strstr(line, "libmali.so.0.1+0x8000"), "a library offset counts from its first mapping");
    resolve_text(0xb7000800, line, sizeof line);
    CHECK(strstr(line, "[anon]+0x800"), "an anonymous code mapping is named [anon]");
    resolve_text(0xb7001800, line, sizeof line);
    CHECK(!strchr(line, '+'), "the heap is not code");
    before = crash_nexec;
    for (k = 0; k < CRASH_EXEC + 10; k++) {
        snprintf(line, sizeof line, "%08x-%08x r-xp 00000000 00:00 0 ", 0x40000000 + k * 0x2000,
                 0x40001000 + k * 0x2000);
        crash_map_line(line, 0);
    }
    CHECK(crash_nexec == CRASH_EXEC && crash_nexec_lost == before + 10,
          "a full table counts what it lost (%d, %d)", crash_nexec, crash_nexec_lost);
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
