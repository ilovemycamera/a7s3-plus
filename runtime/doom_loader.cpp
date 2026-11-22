#include <stdint.h>

extern "C" {
void *dlopen(const char *filename, int flags);
char *dlerror(void);
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

static void write_str(long fd, const char *s) {
    if (!s) return;
    long len = 0;
    while (s[len]) {
        __asm__ volatile("" ::: "memory");
        ++len;
    }
    if (len > 0) sc4(64, fd, (long)s, len, 0);
}

extern "C" __attribute__((visibility("default"))) void _init(void) {
    long fd0 = sc4(56, -100, (long)"/tmp/jiritsu/doom-loader.txt", 1 | 64 | 512, 0644);
    if (fd0 >= 0) {
        write_str(fd0, "STAGE1_ENTER_INIT\n");
        sc4(57, fd0, 0, 0, 0);
    }
    void *h = dlopen("/tmp/jiritsu/doom_a7s3_v17.so", RTLD_NOW | RTLD_NODELETE);
    long fd = sc4(56, -100, (long)"/tmp/jiritsu/doom-loader.txt", 1 | 64 | 1024, 0644);
    if (fd >= 0) {
        if (h) {
            write_str(fd, "DOOM_LOADER=OK\n");
        } else {
            write_str(fd, "DOOM_LOADER=ERR: ");
            const char *err = dlerror();
            write_str(fd, err ? err : "unknown");
            write_str(fd, "\n");
        }
        sc4(57, fd, 0, 0, 0);
    }
}
