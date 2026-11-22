#include <stdint.h>

// Standalone zero-dependency ARM64 watcher for Sony A7S III ulogio ring buffer (0x405000000).
// Lives in volatile /tmp_wbshd/ulog_spy and snapshots AV-core logs to /tmp/jiritsu/last_2560_ulog.txt
// with fsync() so logs survive a hard REC freeze / battery pull.

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

static long sc4(long n, long a, long b, long c, long d) {
    return sc6(n, a, b, c, d, 0, 0);
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

static int is_interesting(const char *s) {
    if (contains(s, "[DBUG]:[VDT][RDSTP")) return 0;
    if (contains(s, "[DBUG]:[VDT][RDIMG")) return 0;
    return contains(s, "[CMNHANDLER]") || contains(s, "[ENCHANDLER]") ||
           contains(s, "[IMLENC") || contains(s, "[IMLDEC") ||
           contains(s, "[ENCSTATE") || contains(s, "[CMNSTATE") ||
           contains(s, "[STATE") || contains(s, "[TIMING") ||
           contains(s, "[ENC CONFIG") || contains(s, "[CODEC V") ||
           contains(s, "[ENC_UTIL") || contains(s, "[VDT]") ||
           contains(s, "BlackBelt") || contains(s, "Imola") ||
           contains(s, "bso_done") || contains(s, "[file:") ||
           contains(s, "2560") || contains(s, "can't ") ||
           contains(s, "cannot ") || contains(s, "NG ") ||
           contains(s, "fail ") || contains(s, "ERR");
}

static char out_buf[98304];

static uint32_t scan_ring(const uint8_t *base, uint32_t size, uint32_t *out_hash) {
    uint32_t pos = 0;
    uint32_t hash = 2166136261U;
    char line[320];
    uint32_t i = 0;
    while (i < size && pos + 324 < sizeof(out_buf)) {
        if ((i & 7U) == 0 && i + 8 <= size && *(const uint64_t *)(base + i) == 0) {
            i += 8;
            continue;
        }
        uint8_t c = base[i];
        if (c >= 0x20 && c < 0x7f) {
            uint32_t len = 0;
            while (i < size && len < 300) {
                uint8_t ch = base[i];
                if (ch >= 0x20 && ch < 0x7f) {
                    line[len++] = (char)ch;
                    ++i;
                } else {
                    break;
                }
            }
            line[len] = 0;
            if (len >= 12 && is_interesting(line)) {
                for (uint32_t k = 0; k < len; ++k) {
                    out_buf[pos++] = line[k];
                    hash ^= (uint8_t)line[k];
                    hash *= 16777619U;
                }
                out_buf[pos++] = '\n';
            }
        } else {
            ++i;
        }
    }
    *out_hash = hash;
    return pos;
}

static void save_snapshot(const char *path, uint32_t len) {
    long fd = sc4(56, -100, (long)path, 1 | 64 | 512, 0644); // O_WRONLY|O_CREAT|O_TRUNC
    if (fd < 0) return;
    uint32_t off = 0;
    while (off < len) {
        long w = sc4(64, fd, (long)(out_buf + off), (long)(len - off), 0);
        if (w <= 0) break;
        off += (uint32_t)w;
    }
    sc4(82, fd, 0, 0, 0); // fsync
    sc4(57, fd, 0, 0, 0); // close
}

extern "C" void _start(void) {
    sc4(157, 0, 0, 0, 0); // setsid()

    long mem_fd = sc4(56, -100, (long)"/dev/mem", 0, 0); // O_RDONLY
    if (mem_fd < 0) sc4(94, 1, 0, 0, 0);

    const uint64_t ulog_phys = 0x405000000ULL;
    const uint32_t ulog_size = 0x00780000U;
    long umap = sc6(222, 0, ulog_size, 1, 1, mem_fd, ulog_phys);
    if (umap <= 0 || (uint64_t)umap >= 0xfffffffffffff000ULL) {
        sc4(57, mem_fd, 0, 0, 0);
        sc4(94, 2, 0, 0, 0);
    }

    const uint8_t *base = (const uint8_t *)umap;
    uint32_t last_hash = 0;
    uint32_t last_len = 0;
    struct { long tv_sec; long tv_nsec; } ts = { 1, 500000000L };

    // Poll for 120 seconds (80 x 1.5s) and flush immediately whenever AV log changes
    for (int iter = 0; iter < 80; ++iter) {
        uint32_t h = 0;
        uint32_t n = scan_ring(base, ulog_size, &h);
        if (n > 0 && (n != last_len || h != last_hash)) {
            save_snapshot("/tmp/jiritsu/last_2560_ulog.txt", n);
            last_len = n;
            last_hash = h;
        }
        sc4(101, (long)&ts, 0, 0, 0); // nanosleep
    }

    sc4(215, umap, ulog_size, 0, 0);
    sc4(57, mem_fd, 0, 0, 0);
    sc4(94, 0, 0, 0, 0); // exit_group
}
