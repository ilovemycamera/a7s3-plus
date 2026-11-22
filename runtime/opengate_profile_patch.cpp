#include <stdint.h>

#ifndef PROFILE_WORD
#define PROFILE_WORD 0x528000e0u
#endif

static long sc4(long n, long a, long b, long c, long d) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
    return x0;
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
static uint64_t appfw_base() {
    static char maps[131072];
    if (read_file("/proc/self/maps", maps, sizeof(maps)) <= 0) return 0;
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
static void flush_icache(void *begin, void *end) {
    for (uintptr_t a = (uintptr_t)begin & ~31UL; a < (uintptr_t)end; a += 32)
        __asm__ volatile("dc cvau, %0" :: "r"(a));
    __asm__ volatile("dsb ish" ::: "memory");
    for (uintptr_t a = (uintptr_t)begin & ~31UL; a < (uintptr_t)end; a += 32)
        __asm__ volatile("ic ivau, %0" :: "r"(a));
    __asm__ volatile("dsb ish; isb" ::: "memory");
}
static void put_hex(char *&p, uint32_t value) {
    for (int i = 0; i < 8; ++i) {
        uint32_t d = (value >> ((7 - i) * 4)) & 15;
        *p++ = d < 10 ? '0' + d : 'a' + d - 10;
    }
}
static void log_result(uint32_t status, uint32_t b1, uint32_t a1, uint32_t b2, uint32_t a2) {
    char message[128] = {0};
    char *p = message;
    *p++='S'; *p++='='; put_hex(p, status); *p++=' ';
    *p++='I'; *p++='='; put_hex(p, b1); *p++='>'; put_hex(p, a1); *p++=' ';
    *p++='V'; *p++='='; put_hex(p, b2); *p++='>'; put_hex(p, a2); *p++='\n';
    char path[] = "/tmp/jiritsu/opengate-profile-patch.txt";
    long fd = sc4(56, -100, (long)path, 1 | 64 | 512, 0644);
    if (fd >= 0) { sc4(64, fd, (long)message, p - message, 0); sc4(57, fd, 0, 0, 0); }
}
#ifndef VEF_WORD
#define VEF_WORD ((660u << 21) | (2u << 5) | 1u)
#endif
extern "C" __attribute__((visibility("default"))) void _init(void) {
    const uint32_t desired_img = PROFILE_WORD;
    const uint32_t desired_vef = VEF_WORD;
#ifndef SITE_IMG_OFFSET
#define SITE_IMG_OFFSET 0x355ef84
#endif
#ifndef SITE_VEF_OFFSET
#define SITE_VEF_OFFSET 0x351b1a4
#endif
    uint64_t base = appfw_base();
    if (!base) { log_result(1, 0, 0, 0, 0); return; }
    volatile uint32_t *site_img = reinterpret_cast<volatile uint32_t *>(base + SITE_IMG_OFFSET);
    volatile uint32_t *site_vef = reinterpret_cast<volatile uint32_t *>(base + SITE_VEF_OFFSET);
    uint32_t b1 = *site_img;
    uint32_t b2 = *site_vef;
    uint64_t page1 = (base + SITE_IMG_OFFSET) & ~0xfffUL;
    uint64_t page2 = (base + SITE_VEF_OFFSET) & ~0xfffUL;
    if (sc4(226, page1, 0x1000, 3, 0) != 0) { log_result(3, b1, b1, b2, b2); return; }
    *site_img = desired_img;
    flush_icache(reinterpret_cast<void *>(base + SITE_IMG_OFFSET), reinterpret_cast<void *>(base + SITE_IMG_OFFSET + 4));
    uint32_t a1 = *site_img;
    sc4(226, page1, 0x1000, 5, 0);
    if (sc4(226, page2, 0x1000, 3, 0) != 0) { log_result(4, b1, a1, b2, b2); return; }
    *site_vef = desired_vef;
    flush_icache(reinterpret_cast<void *>(base + SITE_VEF_OFFSET), reinterpret_cast<void *>(base + SITE_VEF_OFFSET + 4));
    uint32_t a2 = *site_vef;
    sc4(226, page2, 0x1000, 5, 0);
    log_result((a1 == desired_img && a2 == desired_vef) ? 0 : 5, b1, a1, b2, a2);
}
