#include <stdint.h>

extern "C" {
void *dlopen(const char *filename, int flags);
}

#define RTLD_NOW 2
#define RTLD_NODELETE 0x1000

static long sc4(long n, long a, long b, long c, long d) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
    return x0;
}

extern "C" __attribute__((visibility("default"))) void _init(void) {
    void *h = dlopen("/tmp/jiritsu/og_menu_daemon_v15.so", RTLD_NOW | RTLD_NODELETE);
    const char *msg = h ? "OG_MENU_LOADER=OK_V15\n" : "OG_MENU_LOADER=ERR_V15\n";
    long len = 0;
    while (msg[len]) ++len;
    long fd = sc4(56, -100, (long)"/tmp/jiritsu/og-menu-loader.txt", 1 | 64 | 512, 0644);
    if (fd >= 0) {
        sc4(64, fd, (long)msg, len, 0);
        sc4(57, fd, 0, 0, 0);
    }
}
