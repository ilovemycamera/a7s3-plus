#include <stdint.h>
#include "cp_og_patches.h"

static char maps_buf[131072];
static long g_log_fd = -1;

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

static void write_str(const char *s) {
    if (g_log_fd < 0) return;
    long len = 0;
    while (s[len]) ++len;
    sc4(64, g_log_fd, (long)s, len, 0);
}

static void write_hex32(uint32_t v) {
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 8; ++i) {
        uint32_t nib = (v >> ((7 - i) * 4)) & 0xf;
        buf[2 + i] = (nib < 10) ? ('0' + nib) : ('a' + nib - 10);
    }
    buf[10] = 0;
    write_str(buf);
}

static void write_hex_bytes(const uint8_t *p, int n) {
    char buf[4];
    for (int i = 0; i < n; ++i) {
        uint8_t b = p[i];
        uint32_t hi = (b >> 4) & 0xf;
        uint32_t lo = b & 0xf;
        buf[0] = (hi < 10) ? ('0' + hi) : ('a' + hi - 10);
        buf[1] = (lo < 10) ? ('0' + lo) : ('a' + lo - 10);
        buf[2] = 0;
        write_str(buf);
    }
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

static uint64_t find_module_base(const char *name) {
    if (read_file("/proc/self/maps", maps_buf, sizeof(maps_buf)) <= 0) return 0;
    char *line = maps_buf;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        if (contains(line, "r-xp") && contains(line, name)) {
            uint64_t addr = hex_address(line);
            *end = saved;
            return addr;
        }
        *end = saved;
        line = saved ? (end + 1) : end;
    }
    return 0;
}

static int mem_eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

typedef int (*fn_u_osal_init)(void);
typedef int (*fn_osal_dynamic_id)(uint32_t cmd, uint32_t type, uint64_t arg);
typedef int (*fn_osal_valloc_msg_wait)(uint32_t dst_id, void **out_msg, uint32_t size, uint32_t wait);
typedef void (*fn_osal_set_msg_src)(void *msg, uint32_t src_id);
typedef int (*fn_osal_snd_msg)(uint32_t dst_id, void *msg);
typedef int (*fn_osal_rcv_msg_tmo)(uint32_t msgq_id, void **out_msg, uint32_t timeout_ms);
typedef int (*fn_osal_snd_sync_msg)(uint32_t dst_id, void *msg);
typedef int (*fn_osal_free_msg)(uint32_t id, void *msg);

static fn_u_osal_init p_u_osal_init;
static fn_osal_dynamic_id p_osal_dynamic_id;
static fn_osal_valloc_msg_wait p_osal_valloc_msg_wait;
static fn_osal_set_msg_src p_osal_set_msg_src;
static fn_osal_snd_msg p_osal_snd_msg;
static fn_osal_rcv_msg_tmo p_osal_rcv_msg_tmo;
static fn_osal_snd_sync_msg p_osal_snd_sync_msg;
static fn_osal_free_msg p_osal_free_msg;

static uint64_t my_dmm_alloc(uint32_t size, uint32_t *out_handle) {
    uint32_t msgq_id = 0;
    if (p_osal_dynamic_id(0x2a, 0x3d, (uint64_t)&msgq_id) != 0) return 0;
    uint8_t *msg = 0;
    if (p_osal_valloc_msg_wait(0x61003c, (void **)&msg, 0x28, 1) != 0) {
        p_osal_dynamic_id(0x4a, 0, msgq_id);
        return 0;
    }
    for (int i = 0; i < 0x28; ++i) msg[i] = 0;
    p_osal_set_msg_src(msg, msgq_id);
    *(uint16_t *)(msg + 0x00) = 0x20;
    *(uint32_t *)(msg + 0x04) = 0x192;
    *(uint32_t *)(msg + 0x0c) = 10000;
    *(uint16_t *)(msg + 0x14) = (uint16_t)size;
    *(uint16_t *)(msg + 0x16) = 1;

    uint64_t phys_addr = 0;
    if (p_osal_snd_msg(0x61003c, msg) == 0) {
        uint8_t *reply = 0;
        if (p_osal_rcv_msg_tmo(msgq_id, (void **)&reply, 10000) == 0 && reply) {
            int16_t err = *(int16_t *)(reply + 2);
            uint32_t got_size = *(uint32_t *)(reply + 0x20);
            if (err == 0 && got_size >= size) {
                *out_handle = *(uint32_t *)(reply + 0x0c);
                phys_addr = *(uint64_t *)(reply + 0x18);
            }
            p_osal_free_msg(msgq_id, reply);
        }
    } else {
        p_osal_free_msg(0x61003c, msg);
    }
    p_osal_dynamic_id(0x4a, 0, msgq_id);
    return phys_addr;
}

static void my_dmm_free(uint32_t handle) {
    uint32_t msgq_id = 0;
    if (p_osal_dynamic_id(0x2a, 0x3d, (uint64_t)&msgq_id) != 0) return;
    uint8_t *msg = 0;
    if (p_osal_valloc_msg_wait(0x61003c, (void **)&msg, 0x28, 1) == 0) {
        for (int i = 0; i < 0x28; ++i) msg[i] = 0;
        p_osal_set_msg_src(msg, msgq_id);
        *(uint16_t *)(msg + 0x00) = 0x23;
        *(uint32_t *)(msg + 0x08) = handle;
        *(uint16_t *)(msg + 0x16) = 2;
        if (p_osal_snd_msg(0x61003c, msg) == 0) {
            uint8_t *reply = 0;
            if (p_osal_rcv_msg_tmo(msgq_id, (void **)&reply, 10000) == 0 && reply) {
                p_osal_free_msg(msgq_id, reply);
            }
        } else {
            p_osal_free_msg(0x61003c, msg);
        }
    }
    p_osal_dynamic_id(0x4a, 0, msgq_id);
}

static int my_sen_control_cache_cp(int dir, uint64_t cp_addr, uint32_t size) {
    uint8_t *msg = 0;
    if (p_osal_valloc_msg_wait(0x928a0770, (void **)&msg, 0x150, 1) != 0) return -1;
    *(uint16_t *)(msg + 0x26) = (uint16_t)(dir != 0);
    *(uint64_t *)(msg + 0x18) = cp_addr;
    *(uint32_t *)(msg + 0x20) = size;
    int rc = p_osal_snd_sync_msg(0x928a0770, msg);
    int ret = (rc == 0 && msg[1] == 1) ? 0 : -1;
    p_osal_free_msg(0x928a0770, msg);
    return ret;
}

static int my_sen_dma_transfer(uint32_t dir, uint64_t src_addr, uint64_t dst_addr, uint32_t size) {
    uint64_t cp_addr = (dir == 0) ? src_addr : dst_addr;
    if (my_sen_control_cache_cp((int)dir, cp_addr, size) != 0) return -1;
    long fd = sc4(56, -100, (long)"/dev/pcidma", 2, 0);
    if (fd < 0) return -2;
    struct {
        uint8_t b0;
        uint8_t b1;
        uint8_t dir;
        uint8_t b3;
        uint32_t size;
        uint64_t src;
        uint64_t dst;
    } req;
    req.b0 = 0x00;
    req.b1 = 0x05;
    req.dir = (uint8_t)dir;
    req.b3 = 0x00;
    req.size = size;
    req.src = src_addr;
    req.dst = dst_addr;

    long rc = sc4(29, fd, 0x40185001UL, (long)&req, 0);
    sc4(57, fd, 0, 0, 0);
    if (rc != 0) return -3;
    if (dir == 1) {
        my_sen_control_cache_cp(1, cp_addr, size);
    }
    return 0;
}

extern "C" __attribute__((visibility("default"))) void _init(void) {
    g_log_fd = sc4(56, -100, (long)"/tmp/jiritsu/cp-opengate-patch.txt", 1 | 64 | 512, 0644);

    char cmd_buf[32];
    for (int i = 0; i < 32; ++i) cmd_buf[i] = 0;
    read_file("/tmp/jiritsu/cp_patch_cmd", cmd_buf, sizeof(cmd_buf));
    int mode = 0; // 0 = status, 1 = apply, 2 = restore
    if (contains(cmd_buf, "apply")) mode = 1;
    else if (contains(cmd_buf, "restore")) mode = 2;

    write_str(mode == 1 ? "MODE=APPLY\n" : (mode == 2 ? "MODE=RESTORE\n" : "MODE=STATUS\n"));

    uint64_t uipc_base = find_module_base("libosal_uipc.so");
    if (!uipc_base) {
        write_str("ERR: libosal_uipc.so not found\n");
        if (g_log_fd >= 0) sc4(57, g_log_fd, 0, 0, 0);
        return;
    }
    p_u_osal_init = (fn_u_osal_init)(uipc_base + 0x28b8);
    p_osal_snd_msg = (fn_osal_snd_msg)(uipc_base + 0x5e60);
    p_osal_snd_sync_msg = (fn_osal_snd_sync_msg)(uipc_base + 0x6458);
    p_osal_rcv_msg_tmo = (fn_osal_rcv_msg_tmo)(uipc_base + 0x6468);
    p_osal_valloc_msg_wait = (fn_osal_valloc_msg_wait)(uipc_base + 0x67b0);
    p_osal_free_msg = (fn_osal_free_msg)(uipc_base + 0x6970);
    p_osal_dynamic_id = (fn_osal_dynamic_id)(uipc_base + 0x75b8);
    p_osal_set_msg_src = (fn_osal_set_msg_src)(uipc_base + 0x7e20);

    if (*(volatile uint64_t *)(uipc_base + 0x1b560 + 8) == 0) {
        p_u_osal_init();
    }

    uint32_t handle = 0;
    uint64_t dmm_phys = my_dmm_alloc(4096, &handle);
    if (dmm_phys != 0) {
        long mem_fd = sc4(56, -100, (long)"/dev/mem", 0x101002, 0);
        if (mem_fd >= 0) {
            uint64_t page_off = dmm_phys & 0xfffULL;
            uint64_t page_base = dmm_phys - page_off;
            long map_res = sc6(222, 0, 4096 + page_off, 3, 1, mem_fd, page_base);
            if (map_res > 0 && (uint64_t)map_res < 0xfffffffffffff000ULL) {
                uint8_t *buf = (uint8_t *)(map_res + page_off);
                uint32_t num_patches = sizeof(kCpPatches) / sizeof(kCpPatches[0]);

                for (uint32_t idx = 0; idx < num_patches; ++idx) {
                    const CpPatch &p = kCpPatches[idx];
                    uint32_t aligned_start = p.addr & ~0x3fU;
                    uint32_t aligned_end = (p.addr + p.len + 0x3fU) & ~0x3fU;
                    uint32_t aligned_size = aligned_end - aligned_start;
                    uint32_t offset = p.addr - aligned_start;

                    for (uint32_t i = 0; i < aligned_size; ++i) buf[i] = 0;
                    int rc = my_sen_dma_transfer(0, aligned_start, dmm_phys, aligned_size);
                    if (rc != 0) {
                        write_str("ERR read ");
                        write_str(p.name);
                        write_str("\n");
                        continue;
                    }

                    if (mode == 1 || mode == 2) {
                        const uint8_t *target = (mode == 1) ? p.og : p.stock;
                        for (uint32_t i = 0; i < p.len; ++i) {
                            buf[offset + i] = target[i];
                        }
                        __asm__ volatile("dsb sy" ::: "memory");
                        int wrc = my_sen_dma_transfer(1, dmm_phys, aligned_start, aligned_size);
                        if (wrc != 0) {
                            write_str("ERR write ");
                            write_str(p.name);
                            write_str("\n");
                            continue;
                        }
                        for (uint32_t i = 0; i < aligned_size; ++i) buf[i] = 0;
                        __asm__ volatile("dsb sy" ::: "memory");
                        my_sen_dma_transfer(0, aligned_start, dmm_phys, aligned_size);
                    }

                    int is_og = mem_eq(buf + offset, p.og, p.len);
                    int is_st = mem_eq(buf + offset, p.stock, p.len);
                    write_str("[");
                    write_str(p.name);
                    write_str(" @ ");
                    write_hex32(p.addr);
                    write_str("]: ");
                    if (is_og) write_str("OG_3x2");
                    else if (is_st) write_str("STOCK_16x9");
                    else {
                        write_str("OTHER(");
                        write_hex_bytes(buf + offset, (p.len < 8) ? (int)p.len : 8);
                        write_str(")");
                    }
                    write_str("\n");
                }

                sc4(215, map_res, 4096 + page_off, 0, 0);
            }
            sc4(57, mem_fd, 0, 0, 0);
        }
        my_dmm_free(handle);
    } else {
        write_str("ERR: dmm_alloc failed\n");
    }
    if (g_log_fd >= 0) sc4(57, g_log_fd, 0, 0, 0);
}
