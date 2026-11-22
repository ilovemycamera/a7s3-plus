#include <stdint.h>
extern void *dlopen(const char *, int);
extern int pthread_create(unsigned long *thread, const void *attr, void *(*start_routine)(void *), void *arg);
#define RTLD_NOW 2
#define RTLD_NODELETE 0x1000

static char maps[524288];
static char version[64];
static char exe[128];
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc 0" : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory", "cc");
    return x0;
}
static long sc4(long n, long a, long b, long c, long d) {
    return sc6(n, a, b, c, d, 0, 0);
}
static long read_file(const char *path, char *buffer, long capacity) {
    long fd = sc4(56, -100, (long)path, 0, 0);
    if (fd < 0) return -1;
    long used = 0;
    while (used < capacity - 1) {
        long got = sc4(63, fd, (long)(buffer + used), capacity - 1 - used, 0);
        if (got <= 0) break;
        used += got;
    }
    sc4(57, fd, 0, 0, 0);
    buffer[used] = 0;
    return used;
}
static int equals(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
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
static uint64_t hex_address(const char *s) {
    uint64_t value = 0;
    while (*s && *s != '-') {
        uint32_t d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else return 0;
        value = (value << 4) | d;
        ++s;
    }
    return value;
}
static uint64_t appfw_base(void) {
    char *line = maps;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        int match = contains(line, "/usr/lib/appFw.so") && contains(line, "r-xp");
        uint64_t base = match ? hex_address(line) : 0;
        *end = saved;
        if (base) return base;
        if (!*end) break;
        line = end + 1;
    }
    return 0;
}
static void mark(char stage) {
    char data[2] = {stage, '\n'};
    long fd = sc4(56, -100, (long)"/tmp_wbshd/persistent-preload.log", 1 | 64 | 1024, 0644);
    if (fd >= 0) { sc4(64, fd, (long)data, 2, 0); sc4(57, fd, 0, 0, 0); }
    long fd2 = sc4(56, -100, (long)"/tmp/jiritsu/persistent-preload.log", 1 | 64 | 1024, 0644);
    if (fd2 >= 0) { sc4(64, fd2, (long)data, 2, 0); sc4(57, fd2, 0, 0, 0); }
}
static int im_process(void) {
    char buf[128];
    if (read_file("/proc/self/comm", buf, sizeof(buf)) > 0 &&
        (contains(buf, "im.elf") || contains(buf, "dicam"))) return 1;
    long len = sc4(78, -100, (long)"/proc/self/exe", (long)buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = 0;
        return contains(buf, "im.elf") || contains(buf, "dicam");
    }
    return 0;
}
static int bootin_process(void) {
    char buf[128];
    if (read_file("/proc/self/comm", buf, sizeof(buf)) > 0 && contains(buf, "bootin.elf")) return 1;
    long len = sc4(78, -100, (long)"/proc/self/exe", (long)buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = 0;
        return contains(buf, "bootin.elf");
    }
    return 0;
}
static void *worker(void *arg) {
    (void)arg;
    mark('W');
    struct { long sec; long nsec; } delay = {0, 100000000};
    struct { long sec; long nsec; } settle = {2, 0};
    sc4(34, -100, (long)"/tmp/jiritsu", 0755, 0);
    sc6(40, (long)"/dev/nflasha6", (long)"/tmp/jiritsu", (long)"vfat", 0, 0, 0);
    for (int attempt = 0; attempt < 300; ++attempt) {
        sc4(101, (long)&delay, 0, 0, 0);
        if (read_file("/proc/self/maps", maps, sizeof(maps)) <= 0) break;
        if (contains(maps, "doom_a7s3_v17.so")) { mark('S'); break; }
        uint64_t base = appfw_base();
        if (!base) continue;
        if (*(volatile uint64_t *)(base + 0x59609f8) == 0) continue;
        sc4(101, (long)&settle, 0, 0, 0);
        sc4(34, -100, (long)"/tmp/jiritsu", 0755, 0);
        sc6(40, (long)"/dev/nflasha6", (long)"/tmp/jiritsu", (long)"vfat", 0, 0, 0);
        mark('A');
        struct { long sec; long nsec; } step = {1, 0};
        if (!dlopen("/setting/mode/menu_repair.so", RTLD_NOW | RTLD_NODELETE)) { mark('M'); break; }
        sc4(101, (long)&step, 0, 0, 0);
        mark('B');
        if (!dlopen("/setting/mode/high_res_display.so", RTLD_NOW | RTLD_NODELETE)) { mark('H'); break; }
        sc4(101, (long)&step, 0, 0, 0);
        mark('D');
        if (!dlopen("/setting/mode/og_menu_daemon.so", RTLD_NOW | RTLD_NODELETE)) { mark('O'); break; }
        sc4(101, (long)&step, 0, 0, 0);
        mark('G');
        dlopen("/tmp/jiritsu/doom_a7s3_v17.so", RTLD_NOW | RTLD_NODELETE);
        mark('K');
        break;
    }
    return 0;
}

__attribute__((visibility("default"))) void _init(void) {
    int is_bootin = bootin_process();
    int is_im = !is_bootin && im_process();
    if (!is_bootin && !is_im) return;
    if (read_file("/version.txt", version, sizeof(version)) <= 0) return;
    if (!contains(version, "39183_16.2026041504")) return;
    if (is_bootin) {
        long r1 = sc6(40, (long)"/setting/mode/libcinema.so", (long)"/usr/lib/libHeapMon.so", 0, 4096, 0, 0);
        long r2 = sc6(40, (long)"/setting/mode/libcinema.so", (long)"/usr/lib/libjemalloc.so", 0, 4096, 0, 0);
        mark((r1 == 0 || r2 == 0) ? 'P' : 'F');
        return;
    }
    mark('I');
    unsigned long tid = 0;
    pthread_create(&tid, 0, worker, 0);
}
