/*
 * cinema_worker.c -> /setting/mode/libcinema_worker.so
 *
 * Loaded ONLY inside im.elf (via /setting/mode/libjit_warm.so).
 * Spawns a single detached worker thread that waits for appFw.so readiness
 * and loads menu_repair.so -> high_res_display.so -> og_menu_daemon.so
 * (+ doom_a7s3_v17.so if DOOM WAD + binary are staged in /tmp/jiritsu/).
 */
#include <stdint.h>

extern void *dlopen(const char *, int);
extern int pthread_create(unsigned long *thread, const void *attr,
                          void *(*start_routine)(void *), void *arg);
extern int pthread_detach(unsigned long thread);

#define RTLD_NOW      2
#define RTLD_NODELETE 0x1000
#define AT_FDCWD      (-100)
#define O_RDONLY      0
#define O_WRONLY      1
#define O_CREAT       0x40
#define O_APPEND      0x400

static char g_maps[524288];
static char g_ver[64];
static volatile int g_started = 0;

static inline long sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc 0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory", "cc");
    return x0;
}

static inline long sc4(long n, long a, long b, long c, long d) {
    return sc6(n, a, b, c, d, 0, 0);
}

static long read_file(const char *path, char *buf, long cap) {
    long fd = sc4(56, AT_FDCWD, (long)path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long used = 0;
    while (used < cap - 1) {
        long got = sc4(63, fd, (long)(buf + used), cap - 1 - used, 0);
        if (got <= 0) break;
        used += got;
    }
    sc4(57, fd, 0, 0, 0);
    buf[used] = 0;
    return used;
}

static int file_exists(const char *path) {
    long fd = sc4(56, AT_FDCWD, (long)path, O_RDONLY, 0);
    if (fd < 0) return 0;
    sc4(57, fd, 0, 0, 0);
    return 1;
}

static int contains(const char *text, const char *needle) {
    while (*text) {
        const char *a = text, *b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return 1;
        ++text;
    }
    return 0;
}

static uint64_t hex_addr(const char *s) {
    uint64_t v = 0;
    while (*s && *s != '-') {
        uint32_t d;
        if (*s >= '0' && *s <= '9') d = (uint32_t)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (uint32_t)(*s - 'a' + 10);
        else return 0;
        v = (v << 4) | d;
        ++s;
    }
    return v;
}

static uint64_t find_mod_base(const char *needle) {
    char *line = g_maps;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        int match = contains(line, needle) && contains(line, "r-xp");
        uint64_t base = match ? hex_addr(line) : 0;
        *end = saved;
        if (base) return base;
        if (!*end) break;
        line = end + 1;
    }
    return 0;
}

static uint64_t find_appfw_base(void) {
    return find_mod_base("/usr/lib/appFw.so");
}

static void wlog(const char *msg) {
    char line[256];
    int p = 0;
    char up[32];
    long un = read_file("/proc/uptime", up, sizeof(up));
    line[p++] = '[';
    if (un > 0) {
        for (int i = 0; i < un && up[i] && up[i] != ' ' && up[i] != '\n' && p < 24; ++i) {
            line[p++] = up[i];
        }
    } else {
        line[p++] = '?';
    }
    line[p++] = ']';
    line[p++] = ' ';
    for (int i = 0; msg[i] && p < (int)sizeof(line) - 1; ++i) {
        line[p++] = msg[i];
    }
    long fd = sc4(56, AT_FDCWD, (long)"/tmp_wbshd/persist_v2.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        sc4(64, fd, (long)line, p, 0);
        sc4(57, fd, 0, 0, 0);
    }
}

static void sleep_ms(long ms) {
    struct { long sec; long nsec; } ts = { ms / 1000, (ms % 1000) * 1000000L };
    sc4(101, (long)&ts, 0, 0, 0);
}

static void *worker_fn(void *arg) {
    (void)arg;
    wlog("[worker] entered inside im.elf\n");
    sc4(34, AT_FDCWD, (long)"/tmp/jiritsu", 0755, 0);
    sc6(40, (long)"/dev/nflasha6", (long)"/tmp/jiritsu", (long)"vfat", 0, 0, 0);

    for (int attempt = 0; attempt < 300; ++attempt) {
        if (read_file("/proc/self/maps", g_maps, sizeof(g_maps)) <= 0) break;
        uint64_t base = find_appfw_base();
        if (!base || *(volatile uint64_t *)(base + 0x59609f8) == 0) {
            sleep_ms(100);
            continue;
        }

        sleep_ms(300);
        read_file("/proc/self/maps", g_maps, sizeof(g_maps));

        if (!contains(g_maps, "menu_repair.so")) {
            wlog("[worker] loading menu_repair.so\n");
            if (!dlopen("/setting/mode/menu_repair.so", RTLD_NOW | RTLD_NODELETE)) {
                wlog("[worker] ERR menu_repair.so\n");
                break;
            }
            sleep_ms(250);
        }

        if (!contains(g_maps, "high_res_display")) {
            wlog("[worker] loading high_res_display.so\n");
            if (!dlopen("/setting/mode/high_res_display.so", RTLD_NOW | RTLD_NODELETE)) {
                wlog("[worker] ERR high_res_display.so\n");
                break;
            }
            sleep_ms(250);
        }

        if (!contains(g_maps, "og_menu_daemon")) {
            wlog("[worker] loading og_menu_daemon.so\n");
            if (!dlopen("/setting/mode/og_menu_daemon.so", RTLD_NOW | RTLD_NODELETE)) {
                wlog("[worker] ERR og_menu_daemon.so\n");
                break;
            }
            sleep_ms(250);
        }

        if (!contains(g_maps, "big6_key_hook") &&
            file_exists("/setting/mode/big6_key_hook.so")) {
            wlog("[worker] loading big6_key_hook.so\n");
            dlopen("/setting/mode/big6_key_hook.so", RTLD_NOW | RTLD_NODELETE);
            sleep_ms(200);
        }

        if (!contains(g_maps, "doom_a7s3_v17.so") &&
            file_exists("/tmp/jiritsu/doom1.wad") &&
            file_exists("/tmp/jiritsu/doom_a7s3_v17.so")) {
            wlog("[worker] loading doom_a7s3_v17.so\n");
            dlopen("/tmp/jiritsu/doom_a7s3_v17.so", RTLD_NOW | RTLD_NODELETE);
        }

        /* Mark RAM loaded flag compatible with load_opengate_profile.py */
        long okfd = sc4(56, AT_FDCWD, (long)"/tmp_wbshd/menu_repair_ram.ok",
                        O_WRONLY | O_CREAT | 0x200, 0644);
        if (okfd >= 0) {
            sc4(64, okfd, (long)"1\n", 2, 0);
            sc4(57, okfd, 0, 0, 0);
        }

        wlog("[worker] ALL MODULES ARMED\n");

        /* If we were loaded via zero-DMM JIRITSU_START with a BSS-seeded CImJiritsu
         * slot (jbase + 0x47c00), wait briefly for cmd_start to return, then run
         * CImJiritsu::exit() to dlclose libjit_warm.so, unmount ramdsk_libJit, join
         * jit_thread, and restore g_pImJInst = NULL (100% stock state). */
        sleep_ms(400);
        if (read_file("/proc/self/maps", g_maps, sizeof(g_maps)) > 0) {
            uint64_t jbase = find_mod_base("/usr/lib/libJiritsu.so");
            if (jbase && *(volatile uint64_t *)(jbase + 0x47bd8UL) == (jbase + 0x47c00UL)) {
                if (*(volatile uint64_t *)(jbase + 0x47c10UL) != 0) {
                    void (*jit_exit)(void *) = (void (*)(void *))(jbase + 0xb68cUL);
                    jit_exit((void *)(jbase + 0x47c00UL));
                }
                *(volatile uint64_t *)(jbase + 0x47bd8UL) = 0;
                wlog("[worker] zero-DMM jit_thread & ramdisk cleaned up\n");
            }
        }
        break;
    }
    return 0;
}

__attribute__((visibility("default")))
void _init(void) {
    char comm[64];
    if (read_file("/proc/self/comm", comm, sizeof(comm)) <= 0) return;
    if (!contains(comm, "im.elf")) return;
    if (read_file("/version.txt", g_ver, sizeof(g_ver)) <= 0) return;
    if (!contains(g_ver, "39183_16.2026041504")) return;
    if (!__sync_bool_compare_and_swap(&g_started, 0, 1)) return;

    unsigned long tid = 0;
    if (pthread_create(&tid, 0, worker_fn, 0) == 0) {
        pthread_detach(tid);
    }
}
