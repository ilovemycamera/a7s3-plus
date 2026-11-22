/*
 * a7s3_wake_inject.c -> /setting/mode/a7s3_wake_inject
 *
 * Standalone AArch64 helper binary linked against Sony's stock libtestcmd.so,
 * libosal_uipc.so, and libc.so.6 (no LD_PRELOAD hijacking of adjstctl.elf).
 *
 * Safety architecture:
 *   1. Auto-Disarm Dead-Man's Switch:
 *      Tracks consecutive boot attempts in /setting/mode/boot_tries.
 *      If tries >= 2 (meaning 2 consecutive boots failed to reach stable 8s
 *      operation), immediately unlinks /etc/ld.so.preload, resets boot_tries=0,
 *      syncs flash, and exits -> permanent bootloop is impossible.
 *   2. Physical SD-Card Kill-Switch:
 *      Scans /proc/mounts for removable media (/dev/mm*, /dev/ms*, /mnt, /nmnt).
 *      If NO_MOD or a7s3_disable.txt exists in the root of any card, immediately
 *      unlinks /etc/ld.so.preload, resets boot_tries=0, syncs, and exits.
 *   3. USB Coexistence & Idempotency:
 *      Waits if /tmp/jbusy exists (USB senser active), checks /proc/<im_pid>/maps
 *      for appFw.so + libJiritsu.so, skips injection if og_menu_daemon is already
 *      loaded, then injects /setting/mode/libjit_warm.so via local OSAL UIPC
 *      (0x016f0480 + DMM 0x8061003c lock/unlock).
 *   4. Post-Load Stability Gate:
 *      Waits until og_menu_daemon appears in im.elf maps, then sleeps 8s
 *      (past the 6.6s hardware watchdog window) before clearing boot_tries=0.
 */
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* Stock Sony library symbols */
extern int __libc_start_main(int (*main_fn)(int, char **, char **),
                             int argc, char **argv,
                             void (*init_fn)(void), void (*fini_fn)(void),
                             void (*rtld_fini)(void), void *stack_end);
extern int testcmd_init(void *ctx);
extern int testcmd_sndmsg(u32 dest_id, void *ctx, u32 flags);
extern int osal_dynamic_id(int op, u32 param, u32 *out_id);
extern int osal_valloc_msg_wait(u32 dest, void **msg, u32 size, u32 flag);
extern int osal_set_msg_src(void *msg, u32 src);
extern int osal_snd_msg(u32 dest, void *msg);
extern int osal_rcv_msg_tmo(u32 id, void **msg, int tmo);
extern int osal_free_msg(u32 id, void *msg);

/* Raw AArch64 Linux syscalls (kernel 4.9.337) */
#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_SYNC_RDWR 0x101002

#define SYS_mkdirat    34
#define SYS_unlinkat   35
#define SYS_mount      40
#define SYS_openat     56
#define SYS_close      57
#define SYS_getdents64 61
#define SYS_read       63
#define SYS_write      64
#define SYS_pread64    67
#define SYS_pwrite64   68
#define SYS_sync       81
#define SYS_nanosleep  101
#define SYS_munmap     215
#define SYS_mmap       222

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

/* Minimal freestanding CRT0 invoking glibc __libc_start_main */
__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "  mov x29, #0\n"
    "  mov x30, #0\n"
    "  mov x5, x0\n"
    "  ldr x1, [sp]\n"
    "  add x2, sp, #8\n"
    "  mov x6, sp\n"
    "  adrp x0, :got:main\n"
    "  ldr x0, [x0, :got_lo12:main]\n"
    "  mov x3, #0\n"
    "  mov x4, #0\n"
    "  bl __libc_start_main\n"
    "  brk #0\n"
);

static void my_memset(void *dst, int val, unsigned long n) {
    u8 *d = (u8 *)dst;
    while (n--) *d++ = (u8)val;
}

static void my_memcpy(void *dst, const void *src, unsigned long n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    while (n--) *d++ = *s++;
}

static long my_strlen(const char *s) {
    long n = 0;
    while (s[n]) ++n;
    return n;
}

static int my_contains(const char *hay, const char *needle) {
    while (*hay) {
        const char *a = hay, *b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return 1;
        ++hay;
    }
    return 0;
}

static int my_startswith(const char *s, const char *pfx) {
    while (*pfx) {
        if (*s++ != *pfx++) return 0;
    }
    return 1;
}

static void sleep_ms(long ms) {
    struct { long sec; long nsec; } ts;
    ts.sec = ms / 1000;
    ts.nsec = (ms % 1000) * 1000000L;
    sc4(SYS_nanosleep, (long)&ts, 0, 0, 0);
}

static int g_manual = 0;

static long read_file_buf(const char *path, char *buf, long cap) {
    long fd = sc4(SYS_openat, AT_FDCWD, (long)path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long total = 0;
    while (total < cap - 1) {
        long n = sc4(SYS_read, fd, (long)(buf + total), cap - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    sc4(SYS_close, fd, 0, 0, 0);
    buf[total] = 0;
    return total;
}

static int write_file_bytes(const char *path, const u8 *buf, long len) {
    long fd = sc4(SYS_openat, AT_FDCWD, (long)path,
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    long off = 0;
    while (off < len) {
        long n = sc4(SYS_write, fd, (long)(buf + off), len - off, 0);
        if (n <= 0) {
            sc4(SYS_close, fd, 0, 0, 0);
            return -1;
        }
        off += n;
    }
    sc4(SYS_close, fd, 0, 0, 0);
    return 0;
}

static void log_msg(const char *msg) {
    char line[256];
    int p = 0;
    char up[32];
    long un = read_file_buf("/proc/uptime", up, sizeof(up));
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
    if (g_manual) {
        sc4(SYS_write, 1, (long)line, p, 0);
    }
    long fd = sc4(SYS_openat, AT_FDCWD, (long)"/tmp_wbshd/persist_v2.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        sc4(SYS_write, fd, (long)line, p, 0);
        sc4(SYS_close, fd, 0, 0, 0);
    }
}

static void log_flash(const char *msg) {
    log_msg(msg);
    long fd = sc4(SYS_openat, AT_FDCWD, (long)"/tmp/jiritsu/persist_v2.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        sc4(SYS_write, fd, (long)msg, my_strlen(msg), 0);
        sc4(SYS_close, fd, 0, 0, 0);
    }
}

static void log_hex(const char *prefix, u64 val) {
    char buf[96];
    int p = 0;
    while (prefix[p] && p < 64) { buf[p] = prefix[p]; ++p; }
    buf[p++] = '0'; buf[p++] = 'x';
    static const char hex[] = "0123456789abcdef";
    int started = 0;
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (int)((val >> sh) & 0xf);
        if (d || started || sh == 0) {
            buf[p++] = hex[d];
            started = 1;
        }
    }
    buf[p++] = '\n';
    buf[p] = 0;
    log_msg(buf);
}

static int file_exists(const char *path) {
    long fd = sc4(SYS_openat, AT_FDCWD, (long)path, O_RDONLY, 0);
    if (fd < 0) return 0;
    sc4(SYS_close, fd, 0, 0, 0);
    return 1;
}

static int read_boot_tries(void) {
    char b[16];
    long n = read_file_buf("/setting/mode/boot_tries", b, sizeof(b));
    if (n <= 0) return 0;
    if (b[0] >= '0' && b[0] <= '9') return b[0] - '0';
    return 0;
}

static void write_boot_tries(int v) {
    if (v < 0) v = 0;
    if (v > 9) v = 9;
    char b[2] = {(char)('0' + v), '\n'};
    long fd = sc4(SYS_openat, AT_FDCWD, (long)"/setting/mode/boot_tries",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        sc4(SYS_write, fd, (long)b, 2, 0);
        sc4(SYS_close, fd, 0, 0, 0);
    }
    sc4(SYS_sync, 0, 0, 0, 0);
}

static void disarm_persistence(const char *reason) {
    sc4(SYS_unlinkat, AT_FDCWD, (long)"/etc/ld.so.preload", 0, 0);
    write_boot_tries(0);
    sc4(SYS_sync, 0, 0, 0, 0);
    log_flash(reason);
}

static int check_sd_killswitch(void) {
    static char mounts[16384];
    long n = read_file_buf("/proc/mounts", mounts, sizeof(mounts));
    if (n <= 0) return 0;

    char *p = mounts;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') ++p;
        if (*p == '\n') *p++ = 0;

        /* Parse "<dev> <mnt> <fstype> ..." */
        char *sp1 = line;
        while (*sp1 && *sp1 != ' ') ++sp1;
        if (!*sp1) continue;
        *sp1 = 0;
        char *dev = line;
        char *mnt = sp1 + 1;
        char *sp2 = mnt;
        while (*sp2 && *sp2 != ' ') ++sp2;
        if (!*sp2) continue;
        *sp2 = 0;

        int is_card = my_startswith(dev, "/dev/mm") ||
                      my_startswith(dev, "/dev/ms") ||
                      my_startswith(mnt, "/mnt/") ||
                      my_startswith(mnt, "/nmnt/");
        if (!is_card) continue;

        static const char *names[] = {
            "/NO_MOD", "/no_mod", "/NO_MOD.TXT", "/a7s3_disable.txt", 0
        };
        for (int i = 0; names[i]; ++i) {
            char path[256];
            int k = 0;
            for (int j = 0; mnt[j] && k < 200; ++j) path[k++] = mnt[j];
            for (int j = 0; names[i][j] && k < 250; ++j) path[k++] = names[i][j];
            path[k] = 0;
            if (file_exists(path)) {
                log_flash("[KILLSWITCH] Detected SD card disarm file: ");
                log_flash(path);
                log_flash("\n");
                return 1;
            }
        }
    }
    return 0;
}

struct linux_dirent64 {
    u64 d_ino;
    int64_t d_off;
    u16 d_reclen;
    u8 d_type;
    char d_name[];
};

static int find_im_pid(void) {
    long fd = sc4(SYS_openat, AT_FDCWD, (long)"/proc", O_RDONLY, 0);
    if (fd < 0) return -1;
    static char dbuf[16384];
    int found = -1;
    for (;;) {
        long n = sc4(SYS_getdents64, fd, (long)dbuf, sizeof(dbuf), 0);
        if (n <= 0) break;
        long pos = 0;
        while (pos < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(dbuf + pos);
            pos += d->d_reclen;
            if (d->d_name[0] < '0' || d->d_name[0] > '9') continue;
            int pid = 0;
            int all_num = 1;
            for (int i = 0; d->d_name[i]; ++i) {
                if (d->d_name[i] < '0' || d->d_name[i] > '9') { all_num = 0; break; }
                pid = pid * 10 + (d->d_name[i] - '0');
            }
            if (!all_num || pid <= 1) continue;
            char cpath[64];
            int k = 0;
            const char *pfx = "/proc/";
            while (*pfx) cpath[k++] = *pfx++;
            for (int i = 0; d->d_name[i] && k < 48; ++i) cpath[k++] = d->d_name[i];
            const char *sfx = "/comm";
            while (*sfx) cpath[k++] = *sfx++;
            cpath[k] = 0;
            char comm[64];
            if (read_file_buf(cpath, comm, sizeof(comm)) > 0 && my_contains(comm, "im.elf")) {
                found = pid;
                break;
            }
        }
        if (found > 0) break;
    }
    sc4(SYS_close, fd, 0, 0, 0);
    return found;
}

static char g_maps[524288];

static long read_pid_maps(int pid) {
    char mpath[64];
    int k = 0;
    const char *pfx = "/proc/";
    while (*pfx) mpath[k++] = *pfx++;
    char digits[16];
    int dn = 0, v = pid;
    while (v > 0 && dn < 12) { digits[dn++] = (char)('0' + (v % 10)); v /= 10; }
    while (dn > 0) mpath[k++] = digits[--dn];
    const char *sfx = "/maps";
    while (*sfx) mpath[k++] = *sfx++;
    mpath[k] = 0;
    return read_file_buf(mpath, g_maps, sizeof(g_maps));
}

static u64 parse_hex_addr(const char *s) {
    u64 v = 0;
    while (*s && *s != '-') {
        u32 d;
        if (*s >= '0' && *s <= '9') d = (u32)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (u32)(*s - 'a' + 10);
        else return 0;
        v = (v << 4) | d;
        ++s;
    }
    return v;
}

static u64 find_mod_base_in_maps(const char *needle) {
    char *line = g_maps;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        int match = my_contains(line, needle) && my_contains(line, "r-xp");
        u64 base = match ? parse_hex_addr(line) : 0;
        *end = saved;
        if (base) return base;
        if (!*end) break;
        line = end + 1;
    }
    return 0;
}

/*
 * On stock WBI2 hibernation snapshots, libJiritsu's g_pImJInst (jbase + 0x47bd8)
 * is NULL because /tmp/jmode did not exist at factory cold boot. Seeding a zeroed
 * 0x68-byte CImJiritsu struct (state = 0x0a) in the unused tail of libJiritsu's
 * rw-p BSS page (jbase + 0x47c00, right past .bss end 0x47bf8) lets JIRITSU_START
 * (0x0004) load /tmp/jiritsu/libJit.so with ZERO DMM allocation — working reliably
 * even in Live View when USB is unplugged! cinema_worker later calls CImJiritsu::exit()
 * and restores g_pImJInst = NULL so im.elf returns to 100% stock state.
 */
static int seed_jiritsu_instance(int pid) {
    if (read_pid_maps(pid) <= 0) return -1;
    u64 jbase = find_mod_base_in_maps("/usr/lib/libJiritsu.so");
    if (!jbase) return -2;

    char mempath[64];
    int k = 0;
    const char *pfx = "/proc/";
    while (*pfx) mempath[k++] = *pfx++;
    char digits[16];
    int dn = 0, v = pid;
    while (v > 0 && dn < 12) { digits[dn++] = (char)('0' + (v % 10)); v /= 10; }
    while (dn > 0) mempath[k++] = digits[--dn];
    const char *sfx = "/mem";
    while (*sfx) mempath[k++] = *sfx++;
    mempath[k] = 0;

    long fd = sc4(SYS_openat, AT_FDCWD, (long)mempath, O_RDWR, 0);
    if (fd < 0) return -3;

    u64 cur = 0;
    if (sc4(SYS_pread64, fd, (long)&cur, 8, (long)(jbase + 0x47bd8UL)) != 8) {
        sc4(SYS_close, fd, 0, 0, 0);
        return -4;
    }
    if (cur == 0) {
        u8 inst[0x68];
        my_memset(inst, 0, sizeof(inst));
        *(u32 *)(inst + 0x08) = 0x0a;
        u64 slot = jbase + 0x47c00UL;
        if (sc4(SYS_pwrite64, fd, (long)inst, sizeof(inst), (long)slot) != (long)sizeof(inst) ||
            sc4(SYS_pwrite64, fd, (long)&slot, 8, (long)(jbase + 0x47bd8UL)) != 8) {
            sc4(SYS_close, fd, 0, 0, 0);
            return -5;
        }
        log_hex("[wake_inject] seeded g_pImJInst=", slot);
    }
    sc4(SYS_close, fd, 0, 0, 0);
    return 0;
}

/* ---- Local OSAL UIPC 2-phase Jiritsu injector (proven protocol) ---- */

struct testcmd_ctx {
    u64 msg_size;
    long (*send_cb)(u8 *msg, u32 size, void *user);
    long (*recv_cb)(u8 *msg, u32 size, void *user);
    u32 flags;
    u8 pad[32];
};

static u8 g_reply[0x150];
static u8 g_mode;
static u16 g_cmd;
static const u8 *g_payload;
static u32 g_plen;

static long send_cb(u8 *msg, u32 size, void *user) {
    (void)user;
    my_memset(msg, 0, size);
    if (g_mode == 0) {
        /* 1-phase direct command (e.g. JIRITSU_START 0x0004): zero DMM! */
        msg[0x00] = 0;
        msg[0x01] = 0;
    } else if (g_mode == 1) {
        msg[0x00] = 1;
        msg[0x01] = 1;
        *(u16 *)(msg + 0x02) = 1;
        *(u32 *)(msg + 0x04) = g_plen + 4;
        *(u16 *)(msg + 0x08) = 0x40;
    } else {
        msg[0x00] = 4;
        msg[0x01] = 1;
        msg[0x0c] = g_reply[0x04];
        *(u32 *)(msg + 0x10) = *(u32 *)(g_reply + 0x08);
        *(u64 *)(msg + 0x18) = *(u64 *)(g_reply + 0x10);
        *(u32 *)(msg + 0x20) = g_plen;
    }
    *(u16 *)(msg + 0x24) = 0x1001;
    *(u16 *)(msg + 0x26) = g_cmd;
    u32 copy_len = g_plen < 0x124 ? g_plen : 0x124;
    if (copy_len > 0 && g_payload) {
        my_memcpy(msg + 0x28, g_payload, copy_len);
    }
    return (long)size;
}

static long recv_cb(u8 *msg, u32 size, void *user) {
    (void)user;
    my_memcpy(g_reply, msg, size < 0x150 ? size : 0x150);
    return 0;
}

static int run_cmd_start(struct testcmd_ctx *ctx, u8 subcmd) {
    my_memset(g_reply, 0, sizeof(g_reply));
    g_mode = 0;
    g_cmd = 0x0004;
    g_payload = &subcmd;
    g_plen = 1;
    int rc = testcmd_sndmsg(0x016f0480, ctx, 0x3d);
    log_hex("  START rc=", (u64)(unsigned int)rc);
    log_hex("  START status=", *(u32 *)(g_reply + 0x1c));
    return rc;
}

static int run_2phase(struct testcmd_ctx *ctx, u16 cmd, const u8 *payload, u32 plen) {
    my_memset(g_reply, 0, sizeof(g_reply));
    g_mode = 1;
    g_cmd = cmd;
    g_payload = payload;
    g_plen = plen;
    int rc = testcmd_sndmsg(0x016f0480, ctx, 0x3d);
    log_hex("  P1 rc=", (u64)(unsigned int)rc);
    log_hex("  P1 mode_res=", ((u64)g_reply[0] << 8) | g_reply[1]);
    if (g_reply[0] != 2 || g_reply[1] != 1) return -1;

    u32 dyn_id = 0;
    osal_dynamic_id(0x2a, 0x3d, &dyn_id);

    /* Lock DMM buffer (0x21) to get physical address */
    void *dmsg = 0;
    osal_valloc_msg_wait(0x8061003c, &dmsg, 0x28, 1);
    osal_set_msg_src(dmsg, dyn_id);
    my_memset(dmsg, 0, 0x28);
    *(u16 *)((u8 *)dmsg + 0x00) = 0x21;
    *(u32 *)((u8 *)dmsg + 0x04) = *(u32 *)(g_reply + 0x10);
    *(u32 *)((u8 *)dmsg + 0x08) = *(u32 *)(g_reply + 0x08);
    *(u16 *)((u8 *)dmsg + 0x14) = 0;
    *(u16 *)((u8 *)dmsg + 0x16) = 1;
    osal_snd_msg(0x8061003c, dmsg);
    void *drep = 0;
    osal_rcv_msg_tmo(dyn_id, &drep, -1);
    u64 phys_addr = 0;
    if (drep) {
        phys_addr = *(u64 *)((u8 *)drep + 0x18);
        osal_free_msg(dyn_id, drep);
    }

    if (phys_addr) {
        long fd = sc4(SYS_openat, AT_FDCWD, (long)"/dev/mem", O_SYNC_RDWR, 0);
        unsigned long map_sz = (plen + 0xfffUL) & ~0xfffUL;
        long p = (fd >= 0) ? sc6(SYS_mmap, 0, (long)map_sz, 3, 1, fd, (long)phys_addr) : -1;
        log_hex("  DMM phys=", phys_addr);
        if (p != -1 && (unsigned long)p < 0xfffffffffffff000UL) {
            my_memcpy((void *)p, payload, plen);
            __asm__ volatile("dsb sy" ::: "memory");
            sc4(SYS_munmap, p, (long)map_sz, 0, 0);
        }
        if (fd >= 0) sc4(SYS_close, fd, 0, 0, 0);
    }

    /* Unlock DMM buffer (0x23, op=2) */
    dmsg = 0;
    if (osal_valloc_msg_wait(0x8061003c, &dmsg, 0x28, 1) == 0 && dmsg) {
        osal_set_msg_src(dmsg, dyn_id);
        my_memset(dmsg, 0, 0x28);
        *(u16 *)((u8 *)dmsg + 0x00) = 0x23;
        *(u32 *)((u8 *)dmsg + 0x08) = *(u32 *)(g_reply + 0x08);
        *(u16 *)((u8 *)dmsg + 0x14) = 0;
        *(u16 *)((u8 *)dmsg + 0x16) = 2;
        osal_snd_msg(0x8061003c, dmsg);
        drep = 0;
        if (osal_rcv_msg_tmo(dyn_id, &drep, -1) == 0 && drep) {
            osal_free_msg(dyn_id, drep);
        }
    }
    osal_dynamic_id(0x4a, 0, (u32 *)(unsigned long)dyn_id);

    g_mode = 4;
    rc = testcmd_sndmsg(0x016f0480, ctx, 0x3d);
    log_hex("  P2 rc=", (u64)(unsigned int)rc);
    log_hex("  P2 status=", *(u32 *)(g_reply + 0x1c));
    return 0;
}

static u8 g_file_buf[65536];

int main(int argc, char **argv) {
    int is_cold = 0;
    for (int i = 1; i < argc; ++i) {
        if (my_contains(argv[i], "manual")) g_manual = 1;
        if (my_contains(argv[i], "cold")) is_cold = 1;
    }

    log_msg("[wake_inject] started\n");

    /* Ensure /tmp/jiritsu is mounted (ignore EBUSY if already mounted) */
    sc4(SYS_mkdirat, AT_FDCWD, (long)"/tmp/jiritsu", 0755, 0);
    sc6(SYS_mount, (long)"/dev/nflasha6", (long)"/tmp/jiritsu", (long)"vfat", 0, 0, 0);

    if (!g_manual) {
        /* Safety #1: Auto-Disarm Dead-Man's Switch */
        int tries = read_boot_tries();
        log_hex("[wake_inject] boot_tries=", (u64)tries);
        if (tries >= 2) {
            disarm_persistence("[AUTO-DISARM] boot_tries>=2 -> deleted /etc/ld.so.preload, reverted to stock!\n");
            return 0;
        }
        write_boot_tries(tries + 1);

        /* Settle delay so im.elf and SD cards finish waking up */
        sleep_ms(is_cold ? 3500 : 1200);

        /* Safety #2: Physical SD-Card Kill-Switch */
        if (check_sd_killswitch()) {
            disarm_persistence("[KILLSWITCH] Disabled persistence via SD card NO_MOD file!\n");
            return 0;
        }
    }

    /* Wait for im.elf + appFw.so + libJiritsu.so (up to 25s) */
    int im_pid = -1;
    for (int i = 0; i < 50; ++i) {
        im_pid = find_im_pid();
        if (im_pid > 0 && read_pid_maps(im_pid) > 0) {
            if (my_contains(g_maps, "og_menu_daemon")) {
                log_msg("[wake_inject] og_menu_daemon already loaded in im.elf!\n");
                if (!g_manual) write_boot_tries(0);
                return 0;
            }
            if (my_contains(g_maps, "/usr/lib/appFw.so") &&
                my_contains(g_maps, "/usr/lib/libJiritsu.so")) {
                break;
            }
        }
        sleep_ms(500);
    }

    if (im_pid <= 0) {
        log_msg("[wake_inject] ERROR: im.elf not found\n");
        return 1;
    }
    log_hex("[wake_inject] im_pid=", (u64)im_pid);

    /* Coexist cleanly with USB senser (/tmp/jbusy) */
    for (int i = 0; i < 20; ++i) {
        if (!file_exists("/tmp/jbusy")) break;
        log_msg("[wake_inject] waiting for /tmp/jbusy...\n");
        sleep_ms(500);
    }

    /* Read /setting/mode/libjit_warm.so payload */
    long fd = sc4(SYS_openat, AT_FDCWD, (long)"/setting/mode/libjit_warm.so", O_RDONLY, 0);
    if (fd < 0) {
        log_msg("[wake_inject] ERROR: missing /setting/mode/libjit_warm.so\n");
        return 1;
    }
    g_file_buf[0] = 0x00;
    g_file_buf[1] = 0xff;
    g_file_buf[2] = 0x00;
    long flen = sc4(SYS_read, fd, (long)(g_file_buf + 3), sizeof(g_file_buf) - 3, 0);
    sc4(SYS_close, fd, 0, 0, 0);
    if (flen <= 0) {
        log_msg("[wake_inject] ERROR: empty libjit_warm.so\n");
        return 1;
    }
    log_hex("[wake_inject] libjit_warm.so bytes=", (u64)flen);

    struct testcmd_ctx ctx;
    my_memset(&ctx, 0, sizeof(ctx));
    testcmd_init(&ctx);
    ctx.msg_size = 0x150;
    ctx.send_cb = send_cb;
    ctx.recv_cb = recv_cb;
    ctx.flags = 1;

    /*
     * PRIMARY PATH: Zero-DMM JIRITSU_START (0x0004).
     * Unlike SET_PATH/SET_FILE2 (which allocate 5 MiB DMM unit 0x304 and block
     * for 15s while Live View is active off-USB), cmd_start loads /tmp/jiritsu/libJit.so
     * directly from filesystem via jit_thread with ZERO DMM allocation!
     */
    int worker_seen = 0;
    sc4(SYS_unlinkat, AT_FDCWD, (long)"/tmp/jiritsu/jinit", 0, 0);
    int seed_rc = seed_jiritsu_instance(im_pid);
    if (seed_rc == 0 &&
        write_file_bytes("/tmp/jiritsu/libJit.so", g_file_buf + 3, flen) == 0 &&
        write_file_bytes("/tmp/jmode", (const u8 *)"  ", 2) == 0) {
        log_msg("[wake_inject] sending zero-DMM JIRITSU_START(0)...\n");
        run_cmd_start(&ctx, 0);
        for (int i = 0; i < 8; ++i) {
            sleep_ms(200);
            if (read_pid_maps(im_pid) > 0 &&
                (my_contains(g_maps, "libcinema_worker") || my_contains(g_maps, "og_menu_daemon"))) {
                worker_seen = 1;
                break;
            }
        }
        if (!worker_seen) {
            log_msg("[wake_inject] sending zero-DMM JIRITSU_START(1)...\n");
            run_cmd_start(&ctx, 1);
            for (int i = 0; i < 8; ++i) {
                sleep_ms(200);
                if (read_pid_maps(im_pid) > 0 &&
                    (my_contains(g_maps, "libcinema_worker") || my_contains(g_maps, "og_menu_daemon"))) {
                    worker_seen = 1;
                    break;
                }
            }
        }
        sc4(SYS_unlinkat, AT_FDCWD, (long)"/tmp/jmode", 0, 0);
        sc4(SYS_unlinkat, AT_FDCWD, (long)"/tmp/jiritsu/libJit.so", 0, 0);
    } else if (seed_rc != 0) {
        log_hex("[wake_inject] seed_jiritsu_instance err=", (u64)(unsigned int)seed_rc);
    }

    if (!worker_seen) {
        /* Fallback: 2-phase OSAL UIPC SET_PATH + SET_FILE2 to Jiritsu (0x016f0480) */
        static const u8 path_pkt[] = "\x00/tmp/jiritsu/libJit.so";
        log_msg("[wake_inject] fallback SET_PATH (0x000f)...\n");
        if (run_2phase(&ctx, 0x000f, path_pkt, sizeof(path_pkt)) != 0) {
            log_msg("[wake_inject] ERROR: SET_PATH failed\n");
            return 1;
        }
        log_msg("[wake_inject] fallback SET_FILE2 (0x0010)...\n");
        if (run_2phase(&ctx, 0x0010, g_file_buf, (u32)(flen + 3)) != 0) {
            log_msg("[wake_inject] ERROR: SET_FILE2 failed\n");
            return 1;
        }
    } else {
        log_msg("[wake_inject] zero-DMM JIRITSU_START loaded libcinema_worker!\n");
    }

    /* Verify modules appeared inside im.elf (wait up to 15s) */
    int loaded = 0;
    for (int i = 0; i < 30; ++i) {
        if (read_pid_maps(im_pid) > 0 && my_contains(g_maps, "og_menu_daemon")) {
            loaded = 1;
            break;
        }
        sleep_ms(500);
    }

    if (!loaded) {
        log_msg("[wake_inject] WARN: og_menu_daemon not seen in im.elf maps within 20s\n");
        return 1;
    }

    log_msg("[wake_inject] SUCCESS: og_menu_daemon confirmed in im.elf maps!\n");

    if (!g_manual) {
        /* Stability gate: survive 8s (> 6.6s hardware watchdog) before clearing boot_tries */
        sleep_ms(8000);
        write_boot_tries(0);
        log_msg("[wake_inject] STABLE: boot_tries cleared to 0!\n");
    }
    return 0;
}
