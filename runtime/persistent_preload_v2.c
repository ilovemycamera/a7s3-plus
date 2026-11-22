/*
 * persistent_preload_v2.c -> /setting/mode/libcinema_preload.so
 *
 * ZERO external symbols (0 DT_NEEDED, 0 UND): uses raw AArch64 Linux syscalls only.
 * Safe to place in /etc/ld.so.preload — cannot break busybox, pam_tally2, or any
 * binary lacking libdl/libpthread.
 *
 * Behavior:
 *  - If /setting/mode/persist_dryrun exists: appends "<pid> <comm>\n" to
 *    /tmp/jiritsu/warm_procs.log (capped at 4 KiB) without launching anything.
 *  - Otherwise:
 *      1) Returns in <2 us for every process except safe boot triggers
 *         (im.elf on cold boot; pam_tally2 / btproperty.elf on WBI2 warm boot).
 *      2) Verifies A7S III FW 5.01 (/version.txt contains "39183_16.2026041504").
 *      3) Atomically claims a volatile RAM-only one-shot lock in /tmp_wbshd/.a7s3_armed
 *         (O_CREAT | O_EXCL) so it fires strictly ONCE per camera power-on.
 *      4) Performs a classic double-fork() + setsid() + closes fd 0..63 -> /dev/null
 *         and execve's /setting/mode/a7s3_wake_inject. The parent process resumes
 *         its 100% stock execution immediately.
 */
#include <stdint.h>

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_EXCL   0x80
#define O_APPEND 0x400
#define SIGCHLD  17

#define SYS_dup3     24
#define SYS_openat   56
#define SYS_close    57
#define SYS_read     63
#define SYS_write    64
#define SYS_readlinkat 78
#define SYS_exit     93
#define SYS_setsid   157
#define SYS_getpid   172
#define SYS_clone    220
#define SYS_execve   221
#define SYS_wait4    260

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

static long read_small(const char *path, char *buf, long cap) {
    long fd = sc4(SYS_openat, AT_FDCWD, (long)path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long n = sc4(SYS_read, fd, (long)buf, cap - 1, 0);
    sc4(SYS_close, fd, 0, 0, 0);
    if (n < 0) return -1;
    buf[n] = 0;
    return n;
}

static int file_exists(const char *path) {
    long fd = sc4(SYS_openat, AT_FDCWD, (long)path, O_RDONLY, 0);
    if (fd < 0) return 0;
    sc4(SYS_close, fd, 0, 0, 0);
    return 1;
}

static int contains(const char *hay, const char *needle) {
    while (*hay) {
        const char *a = hay, *b = needle;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return 1;
        ++hay;
    }
    return 0;
}

static long str_len(const char *s) {
    long n = 0;
    while (s[n]) ++n;
    return n;
}

static void log_dryrun(const char *comm) {
    /* Only log while file is < 4096 bytes so flash never fills */
    char probe[4096];
    long cur = read_small("/tmp/jiritsu/warm_procs.log", probe, sizeof(probe));
    if (cur >= 3900) return;

    long pid = sc4(SYS_getpid, 0, 0, 0, 0);
    char line[96];
    int pos = 0;
    char digits[24];
    int dn = 0;
    long v = pid;
    if (v <= 0) {
        digits[dn++] = '0';
    } else {
        while (v > 0 && dn < 20) {
            digits[dn++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (dn > 0) line[pos++] = digits[--dn];
    line[pos++] = ' ';
    for (int i = 0; comm[i] && comm[i] != '\n' && pos < 90; ++i) {
        line[pos++] = comm[i];
    }
    line[pos++] = '\n';

    long fd = sc4(SYS_openat, AT_FDCWD, (long)"/tmp/jiritsu/warm_procs.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        sc4(SYS_write, fd, (long)line, pos, 0);
        sc4(SYS_close, fd, 0, 0, 0);
    }
}

static void log_ram_hit(const char *comm) {
    char probe[1024];
    long cur = read_small("/tmp_wbshd/preload_hits.log", probe, sizeof(probe));
    if (cur >= 960) return;

    long pid = sc4(SYS_getpid, 0, 0, 0, 0);
    char line[96];
    int pos = 0;
    char digits[24];
    int dn = 0;
    long v = pid;
    if (v <= 0) {
        digits[dn++] = '0';
    } else {
        while (v > 0 && dn < 20) {
            digits[dn++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (dn > 0) line[pos++] = digits[--dn];
    line[pos++] = ' ';
    for (int i = 0; comm[i] && comm[i] != '\n' && pos < 90; ++i) {
        line[pos++] = comm[i];
    }
    line[pos++] = '\n';

    long fd = sc4(SYS_openat, AT_FDCWD, (long)"/tmp_wbshd/preload_hits.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        sc4(SYS_write, fd, (long)line, pos, 0);
        sc4(SYS_close, fd, 0, 0, 0);
    }
}

__attribute__((visibility("default")))
void _init(void) {
    char comm[64];
    if (read_small("/proc/self/comm", comm, sizeof(comm)) <= 0) return;

    /* Never recurse into our own helper */
    if (contains(comm, "a7s3_wake")) return;

    /* Record early process spawns to volatile RAM tmpfs for diagnostics (capped at 1 KiB) */
    log_ram_hit(comm);

    /* Optional safe dry-run probe mode to record which processes launch on warm boot */
    if (file_exists("/setting/mode/persist_dryrun")) {
        log_dryrun(comm);
        return;
    }

    int is_im = contains(comm, "im.elf");
    int is_trigger = is_im ||
                     contains(comm, "fserr.sh") ||
                     contains(comm, "ssh-keygen") ||
                     contains(comm, "pam_tally2") ||
                     contains(comm, "btproperty");

    if (!is_trigger) return;

    /* Strict firmware check: Sony A7S III FW 5.01 ONLY */
    char ver[64];
    if (read_small("/version.txt", ver, sizeof(ver)) <= 0) return;
    if (!contains(ver, "39183_16.2026041504")) return;

    /* Verify standalone injector exists before claiming the one-shot RAM lock */
    if (!file_exists("/setting/mode/a7s3_wake_inject")) return;

    /* Atomic RAM-only one-shot lock (/tmp_wbshd is true volatile tmpfs) */
    long lfd = sc4(SYS_openat, AT_FDCWD, (long)"/tmp_wbshd/.a7s3_armed",
                   O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (lfd < 0) return;
    sc4(SYS_write, lfd, (long)comm, str_len(comm), 0);
    sc4(SYS_close, lfd, 0, 0, 0);

    /* Classic double-fork so parent continues 100% stock and never waits on grandchild */
    long p1 = sc6(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
    if (p1 < 0) return;
    if (p1 > 0) {
        int status = 0;
        sc4(SYS_wait4, p1, (long)&status, 0, 0);
        return;
    }

    /* Child 1: fork grandchild and exit immediately */
    long p2 = sc6(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
    if (p2 != 0) {
        sc4(SYS_exit, 0, 0, 0, 0);
    }

    /* Grandchild daemon: detach session & close all inherited fds so bootin/popen never hangs */
    sc4(SYS_setsid, 0, 0, 0, 0);
    long devnull = sc4(SYS_openat, AT_FDCWD, (long)"/dev/null", O_RDWR, 0);
    if (devnull >= 0) {
        sc4(SYS_dup3, devnull, 0, 0, 0);
        sc4(SYS_dup3, devnull, 1, 0, 0);
        sc4(SYS_dup3, devnull, 2, 0, 0);
        if (devnull > 2) sc4(SYS_close, devnull, 0, 0, 0);
    }
    for (int fd = 3; fd < 64; ++fd) {
        sc4(SYS_close, fd, 0, 0, 0);
    }

    char *argv[3];
    argv[0] = (char *)"/setting/mode/a7s3_wake_inject";
    argv[1] = (char *)(is_im ? "cold" : "warm");
    argv[2] = 0;

    char *envp[3];
    envp[0] = (char *)"PATH=/sbin:/bin:/usr/sbin:/usr/bin";
    envp[1] = (char *)"LD_LIBRARY_PATH=/usr/lib:/lib";
    envp[2] = 0;

    sc4(SYS_execve, (long)argv[0], (long)argv, (long)envp, 0);
    sc4(SYS_exit, 1, 0, 0, 0);
}
