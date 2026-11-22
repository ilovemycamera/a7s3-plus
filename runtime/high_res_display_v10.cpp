#include <stdint.h>

static char maps[131072];
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
extern "C" __attribute__((visibility("default"))) void _init(void) {
    uint64_t base = appfw_base();
    uint32_t flag_result = 0xffffffff;
    uint32_t wheel_result = 0xffffffff;
    if (base) {
        // Undo translator_probe's out-of-range branch and its stray neighboring patch.
        uint64_t probe_site = base + 0x1d9c7f8;
        if (sc4(226, probe_site & ~0xfffUL, 0x1000, 7, 0) == 0) {
            *reinterpret_cast<volatile uint32_t *>(probe_site) = 0x94197f79;
            flush_icache(reinterpret_cast<void *>(probe_site), reinterpret_cast<void *>(probe_site + 4));
        }

        // Native CP widget, with decimal format only while Variable Shutter is ON.
        uint64_t site = base + 0x23fc680;
        if (sc4(226, site & ~0xfffUL, 0x1000, 7, 0) == 0) {
            *reinterpret_cast<volatile uint32_t *>(site + 0) = 0xb9422281; // ldr w1, [x20, #0x220]
            *reinterpret_cast<volatile uint32_t *>(site + 4) = 0x7100003f; // cmp w1, #0
            *reinterpret_cast<volatile uint32_t *>(site + 8) = 0x1a9f17e1; // cset w1, eq
            *reinterpret_cast<volatile uint32_t *>(site + 0x10) = 0xb940ae81;
            *reinterpret_cast<volatile uint32_t *>(site + 0x14) = 0x7100043f;
            *reinterpret_cast<volatile uint32_t *>(site + 0x18) = 0x1a9f0021;
            *reinterpret_cast<volatile uint32_t *>(base + 0x23fc620) = 0x52800000;
            *reinterpret_cast<volatile uint32_t *>(base + 0x23fc6b8) = 0x52800021;
            *reinterpret_cast<volatile uint32_t *>(base + 0x23fc6c0) = 0xb9415a81;
            *reinterpret_cast<volatile uint32_t *>(base + 0x23fc740) = 0xb941fa81;
            flush_icache(reinterpret_cast<void *>(base + 0x23fc620), reinterpret_cast<void *>(base + 0x23fc744));
            flag_result = *reinterpret_cast<volatile uint32_t *>(site);
        }
        uint64_t renderer_site = base + 0x18786b8;
        if (sc4(226, renderer_site & ~0xfffUL, 0x1000, 7, 0) == 0) {
            *reinterpret_cast<volatile uint32_t *>(renderer_site) = 0x54ffe461;
            *reinterpret_cast<volatile uint32_t *>(base + 0x18786fc) = 0x528c04c0;
            *reinterpret_cast<volatile uint32_t *>(base + 0x1878704) = 0x72a040c0;
            // Hide only the shutter widget in the hardware MP OSD queue.
            *reinterpret_cast<volatile uint32_t *>(base + 0x18789f8) = 0x79041c1f;
            *reinterpret_cast<volatile uint32_t *>(base + 0x18789fc) = 0x52800000;
            *reinterpret_cast<volatile uint32_t *>(base + 0x1878a00) = 0x52800001;
            *reinterpret_cast<volatile uint32_t *>(base + 0x1878a04) = 0x52800002;
            *reinterpret_cast<volatile uint32_t *>(base + 0x1878a08) = 0x52800003;
            *reinterpret_cast<volatile uint32_t *>(base + 0x1878a0c) = 0xd65f03c0;
            *reinterpret_cast<volatile uint32_t *>(base + 0x1878a28) = 0x79041c1f;
            *reinterpret_cast<volatile uint32_t *>(base + 0x1878a2c) = 0x52800000;
            flush_icache(reinterpret_cast<void *>(renderer_site), reinterpret_cast<void *>(base + 0x1878a30));
        }
        uint64_t string_site = base + 0x23dedc8;
        if (sc4(226, string_site & ~0xfffUL, 0x1000, 7, 0) == 0) {
            *reinterpret_cast<volatile uint32_t *>(string_site) = 0x34000a60;
            flush_icache(reinterpret_cast<void *>(string_site), reinterpret_cast<void *>(string_site + 4));
        }

        // Unlock Variable Shutter wheel step in Movie mode (0x1b6e1a4)
        *reinterpret_cast<volatile uint8_t *>(base + 0x8455f38) = 0;
        uint64_t wheel_site = base + 0x1b6e1e8;
        uint32_t wheel_w0 = *reinterpret_cast<volatile uint32_t *>(wheel_site);
        if (wheel_w0 == 0x35000340 || wheel_w0 == 0xd503201f) {
            if (sc4(226, wheel_site & ~0xfffUL, 0x1000, 7, 0) == 0) {
                *reinterpret_cast<volatile uint32_t *>(wheel_site) = 0xd503201f; // nop
                flush_icache(reinterpret_cast<void *>(wheel_site), reinterpret_cast<void *>(wheel_site + 4));
                wheel_result = *reinterpret_cast<volatile uint32_t *>(wheel_site);
            }
        } else wheel_result = wheel_w0;
    }
    uint32_t value611 = base ? *reinterpret_cast<volatile uint32_t *>(base + 0x7f844b0) : 0;
    char message[] = "HIGH_RES_DISPLAY flag=00000000 wheel=00000000 value611=00000000\n";
    for (int i = 0; i < 8; ++i) {
        message[22 + i] = "0123456789abcdef"[(flag_result >> ((7 - i) * 4)) & 15];
        message[37 + i] = "0123456789abcdef"[(wheel_result >> ((7 - i) * 4)) & 15];
        message[55 + i] = "0123456789abcdef"[(value611 >> ((7 - i) * 4)) & 15];
    }
    long fd = sc4(56, -100, (long)"/tmp/jiritsu/high-res-display.txt", 1 | 64 | 512, 0644);
    if (fd >= 0) { sc4(64, fd, (long)message, sizeof(message) - 1, 0); sc4(57, fd, 0, 0, 0); }
}
