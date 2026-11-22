/*
 * big6_key_hook.cpp -> big6_key_hook.so
 *
 * Installs the minimal 0x114 -> 0x187 input-callback shim on libObj.so node 0x0a
 * so any Custom Key assigned to "Home" (NVRAM 0x0b70, e.g. Movie C1) opens
 * Sony's native BIG6 Cinema Home Screen. Idempotent (never hooks twice).
 */
#include <stdint.h>

extern "C" {
void big6_cb_start(void);
void big6_cb_end(void);
}

static char g_maps[131072];

static long sc4(long n, long a, long b, long c, long d) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
    return x0;
}

static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory", "cc");
    return x0;
}

static long read_file(const char *path, char *buf, long cap) {
    long fd = sc4(56, -100, (long)path, 0, 0);
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

static uint64_t lib_base(const char *name) {
    if (read_file("/proc/self/maps", g_maps, sizeof(g_maps)) <= 0) return 0;
    char *line = g_maps;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        int match = contains(line, name) && contains(line, "r-xp");
        uint64_t base = match ? hex_addr(line) : 0;
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

static const uint64_t HOOK_MAGIC = 0x42494736484f4f4bULL; /* "BIG6HOOK" */

extern "C" __attribute__((visibility("default"))) void _init(void) {
    uint64_t obj = lib_base("/usr/lib/libObj.so");
    if (!obj) return;

    void *input = reinterpret_cast<void *(*)()>(obj + 0xc9ac0)();
    if (!input) return;

    uint8_t *node = *reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(input) + 0xf0);
    while (node && *reinterpret_cast<uint32_t *>(node + 0x24) != 0x0a) {
        node = *reinterpret_cast<uint8_t **>(node);
    }
    if (!node) return;

    /* Guard against double-hooking the same node */
    uint64_t existing = *reinterpret_cast<uint64_t *>(node + 0x30);
    if (existing >= 0x10000UL && (existing & 0xfffUL) == 0) {
        if (*reinterpret_cast<volatile uint64_t *>(existing + 0x720) == HOOK_MAGIC) {
            return;
        }
    }

    uint8_t *code = reinterpret_cast<uint8_t *>(sc6(222, 0, 0x1000, 7, 0x22, -1, 0));
    if ((uint64_t)code >= (uint64_t)-4096) return;

    uint8_t *src = reinterpret_cast<uint8_t *>(big6_cb_start);
    uint8_t *end = reinterpret_cast<uint8_t *>(big6_cb_end);
    for (uint64_t i = 0; i < (uint64_t)(end - src); ++i) code[i] = src[i];

    *reinterpret_cast<uint64_t *>(code + 0x718) = existing;
    *reinterpret_cast<uint64_t *>(code + 0x720) = HOOK_MAGIC;
    flush_icache(code, code + 0x1000);

    *reinterpret_cast<uint64_t *>(node + 0x30) = reinterpret_cast<uint64_t>(code);

    char msg[] = "BIG6_KEY_HOOK_ARMED code=0000000000000000\n";
    uint64_t addr = reinterpret_cast<uint64_t>(code);
    for (int i = 15; i >= 0; --i) {
        msg[25 + i] = "0123456789abcdef"[addr & 15];
        addr >>= 4;
    }
    long fd = sc4(56, -100, (long)"/tmp/jiritsu/big6-hook.txt", 1 | 64 | 512, 0644);
    if (fd >= 0) { sc4(64, fd, (long)msg, sizeof(msg) - 1, 0); sc4(57, fd, 0, 0, 0); }
}
