#include <stdint.h>
#include "og_2560_patches.h"

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

static void write_u32(uint32_t v) {
    char buf[16];
    int pos = 15;
    buf[pos] = 0;
    if (v == 0) {
        buf[--pos] = '0';
    } else {
        while (v > 0 && pos > 0) {
            buf[--pos] = '0' + (v % 10);
            v /= 10;
        }
    }
    write_str(buf + pos);
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

static void flush_icache_addr(uint64_t addr) {
    __asm__ volatile(
        "dc cvau, %0\n\t"
        "dsb ish\n\t"
        "ic ivau, %0\n\t"
        "dsb ish\n\t"
        "isb\n\t"
        : : "r"(addr) : "memory"
    );
}

static void patch_mp_module(const char *label, uint64_t base, const Patch4 *patches, uint32_t count, int mode) {
    if (!base) {
        write_str(label);
        write_str(": base not found\n");
        return;
    }
    uint32_t n_og = 0, n_st = 0, n_other = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t target_va = base + patches[i].addr;
        uint64_t page = target_va & ~0xfffULL;
        sc4(226, (long)page, 8192, 7, 0); // mprotect RWX
        volatile uint32_t *ptr = (volatile uint32_t *)target_va;
        if (mode == 1) {
            *ptr = patches[i].og;
            flush_icache_addr(target_va);
        } else if (mode == 2) {
            *ptr = patches[i].stock;
            flush_icache_addr(target_va);
        }
        uint32_t cur = *ptr;
        if (cur == patches[i].og) ++n_og;
        else if (cur == patches[i].stock) ++n_st;
        else {
            ++n_other;
            write_str("  OTHER ");
            write_str(label);
            write_str(" @ ");
            write_hex32(patches[i].addr);
            write_str(" = ");
            write_hex32(cur);
            write_str("\n");
        }
    }
    write_str(label);
    write_str(": OG_2560=");
    write_u32(n_og);
    write_str(" STOCK_2160=");
    write_u32(n_st);
    write_str(" OTHER=");
    write_u32(n_other);
    write_str("\n");
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

static const Patch4 kMprTkhd2560Patches[] = {
    { 0x002272b0, 0x53103ef7, 0x52a14017 }, // tkhd.height: lsl w23, w23, #16 -> movz w23, #0x0a00, lsl #16 (2560.0)
    { 0x00229a24, 0x53103c84, 0x52a14004 }, // Rotate tkhd.height: lsl w4, w4, #16 -> movz w4, #0x0a00, lsl #16 (2560.0)
};

static const Patch4 kAvVuiSar27x32Patches[] = {
    // HEVC VUI aspect_ratio_idc = 255 (Extended_SAR), sar_width = 27, sar_height = 32
    { 0x0021c008, 0x3943b52a, 0x52801fea }, // ldrb w10, [x9, #0xed] -> movz w10, #0xff
    { 0x0021c010, 0x3943b92a, 0x5280036a }, // ldrb w10, [x9, #0xee] -> movz w10, #0x1b (27)
    { 0x0021c018, 0x3943bd2a, 0x5280040a }, // ldrb w10, [x9, #0xef] -> movz w10, #0x20 (32)
    // H.264 VUI aspect_ratio_idc = 255 (Extended_SAR), sar_width = 27, sar_height = 32
    { 0x0021c388, 0x3943b14b, 0x52801feb }, // ldrb w11, [x10, #0xec] -> movz w11, #0xff
    { 0x0021c390, 0x3943b54b, 0x5280036b }, // ldrb w11, [x10, #0xed] -> movz w11, #0x1b (27)
    { 0x0021c398, 0x3943b94b, 0x5280040b }, // ldrb w11, [x10, #0xee] -> movz w11, #0x20 (32)
};

extern "C" __attribute__((visibility("default"))) void _init(void) {
    g_log_fd = sc4(56, -100, (long)"/tmp/jiritsu/og-3840x2560-patch.txt", 1 | 64 | 512, 0644);

    char cmd_buf[32];
    for (int i = 0; i < 32; ++i) cmd_buf[i] = 0;
    read_file("/tmp/jiritsu/res2560_cmd", cmd_buf, sizeof(cmd_buf));
    int mode = 0; // 0 = status, 1 = apply, 2 = restore
    if (contains(cmd_buf, "apply")) mode = 1;
    else if (contains(cmd_buf, "restore")) mode = 2;

    int has_sub = contains(cmd_buf, "mp") || contains(cmd_buf, "avt") || contains(cmd_buf, "avro") || contains(cmd_buf, "cp") || contains(cmd_buf, "tkhd");
    int mode_mp = (mode == 1 && has_sub && !contains(cmd_buf, "mp")) ? 0 : mode;
    int mode_tkhd = (mode == 1 && has_sub && !contains(cmd_buf, "tkhd")) ? 0 : mode;
    int mode_avt = (mode == 1 && has_sub && !contains(cmd_buf, "avt")) ? 0 : mode;
    int mode_avro = (mode == 1 && has_sub && !contains(cmd_buf, "avro")) ? 0 : mode;
    int mode_cp = (mode == 1 && has_sub && !contains(cmd_buf, "cp")) ? 0 : mode;

    write_str(mode == 1 ? "=== 3840x2560 APPLY ===\n" : (mode == 2 ? "=== 3840x2560 RESTORE ===\n" : "=== 3840x2560 STATUS ===\n"));

    // 1. Patch MP modules (appFw.so and libmpr.so)
    uint64_t appfw_base = find_module_base("appFw.so");
    patch_mp_module("MP_appFw", appfw_base, kAppFw2560Patches, sizeof(kAppFw2560Patches) / sizeof(kAppFw2560Patches[0]), mode_mp);

    uint64_t mpr_base = find_module_base("libmpr.so");
    patch_mp_module("MP_libmpr", mpr_base, kMpr2560Patches, sizeof(kMpr2560Patches) / sizeof(kMpr2560Patches[0]), mode_mp);
    patch_mp_module("MP_tkhd", mpr_base, kMprTkhd2560Patches, sizeof(kMprTkhd2560Patches) / sizeof(kMprTkhd2560Patches[0]), mode_tkhd);

    // 2. Patch BB/AV (av-cam.bin at physical DDR0 0x401000000) and CP RAM
    long mem_fd = sc4(56, -100, (long)"/dev/mem", 0x101002, 0);
    if (mem_fd >= 0) {
        const uint64_t av_phys_base = 0x401000000ULL;
        const uint32_t av_map_size = 0x00400000U; // 4 MB covers .text + .rodata (0x0 .. 0x3eb000)
        long av_map = sc6(222, 0, av_map_size, 3, 1, mem_fd, av_phys_base);
        if (av_map > 0 && (uint64_t)av_map < 0xfffffffffffff000ULL) {
            uint8_t *av_ptr = (uint8_t *)av_map;
            uint32_t vdt_w = *(volatile uint32_t *)(av_ptr + 0x320960);
            uint32_t vdt_h = *(volatile uint32_t *)(av_ptr + 0x320964);
            if (vdt_w == 3840 && (vdt_h == 2160 || vdt_h == 2560)) {
                uint32_t t_og = 0, t_st = 0, t_oth = 0;
                uint32_t n_av_text = sizeof(kAvText2560Patches) / sizeof(kAvText2560Patches[0]);
                for (uint32_t i = 0; i < n_av_text; ++i) {
                    volatile uint32_t *p = (volatile uint32_t *)(av_ptr + kAvText2560Patches[i].addr);
                    if (mode_avt == 1) {
                        *p = kAvText2560Patches[i].og;
                        flush_icache_addr((uint64_t)p);
                    } else if (mode_avt == 2) {
                        *p = kAvText2560Patches[i].stock;
                        flush_icache_addr((uint64_t)p);
                    }
                    uint32_t cur = *p;
                    if (cur == kAvText2560Patches[i].og) ++t_og;
                    else if (cur == kAvText2560Patches[i].stock) ++t_st;
                    else ++t_oth;
                }
                write_str("AV_TEXT: OG_2560=");
                write_u32(t_og);
                write_str(" STOCK_2160=");
                write_u32(t_st);
                write_str(" OTHER=");
                write_u32(t_oth);
                write_str("\n");

                uint32_t vui_og = 0, vui_st = 0, vui_oth = 0;
                uint32_t n_av_vui = sizeof(kAvVuiSar27x32Patches) / sizeof(kAvVuiSar27x32Patches[0]);
                for (uint32_t i = 0; i < n_av_vui; ++i) {
                    volatile uint32_t *p = (volatile uint32_t *)(av_ptr + kAvVuiSar27x32Patches[i].addr);
                    if (mode_tkhd == 1) {
                        *p = kAvVuiSar27x32Patches[i].og;
                        flush_icache_addr((uint64_t)p);
                    } else if (mode_tkhd == 2) {
                        *p = kAvVuiSar27x32Patches[i].stock;
                        flush_icache_addr((uint64_t)p);
                    }
                    uint32_t cur = *p;
                    if (cur == kAvVuiSar27x32Patches[i].og) ++vui_og;
                    else if (cur == kAvVuiSar27x32Patches[i].stock) ++vui_st;
                    else ++vui_oth;
                }
                write_str("AV_VUI_SAR: OG_27x32=");
                write_u32(vui_og);
                write_str(" STOCK_1x1=");
                write_u32(vui_st);
                write_str(" OTHER=");
                write_u32(vui_oth);
                write_str("\n");

                // Scan .rodata (0x320000 .. 0x3eb000) for u32(3840, 2160/2560) and u16(3840, 2160/2560)
                uint32_t ro32_og = 0, ro32_st = 0;
                for (uint32_t off = 0x320000; off <= 0x3eb000 - 8; off += 4) {
                    volatile uint32_t *w_ptr = (volatile uint32_t *)(av_ptr + off);
                    volatile uint32_t *h_ptr = (volatile uint32_t *)(av_ptr + off + 4);
                    if (*w_ptr == 3840) {
                        uint32_t h = *h_ptr;
                        if (h == 2160 || h == 2560) {
                            if (mode_avro == 1 && h == 2160) {
                                *h_ptr = 2560;
                                flush_icache_addr((uint64_t)h_ptr);
                            } else if (mode_avro == 2 && h == 2560) {
                                *h_ptr = 2160;
                                flush_icache_addr((uint64_t)h_ptr);
                            }
                            if (*h_ptr == 2560) ++ro32_og;
                            else if (*h_ptr == 2160) ++ro32_st;
                        }
                    }
                }
                uint32_t ro16_og = 0, ro16_st = 0;
                for (uint32_t off = 0x320000; off <= 0x3eb000 - 4; off += 2) {
                    volatile uint16_t *w_ptr = (volatile uint16_t *)(av_ptr + off);
                    volatile uint16_t *h_ptr = (volatile uint16_t *)(av_ptr + off + 2);
                    if (*w_ptr == 3840) {
                        uint16_t h = *h_ptr;
                        if (h == 2160 || h == 2560) {
                            if (mode_avro == 1 && h == 2160) {
                                *h_ptr = 2560;
                                flush_icache_addr((uint64_t)h_ptr);
                            } else if (mode_avro == 2 && h == 2560) {
                                *h_ptr = 2160;
                                flush_icache_addr((uint64_t)h_ptr);
                            }
                            if (*h_ptr == 2560) ++ro16_og;
                            else if (*h_ptr == 2160) ++ro16_st;
                        }
                    }
                }
                write_str("AV_RO32: OG_2560=");
                write_u32(ro32_og);
                write_str(" STOCK_2160=");
                write_u32(ro32_st);
                write_str("\nAV_RO16: OG_2560=");
                write_u32(ro16_og);
                write_str(" STOCK_2160=");
                write_u32(ro16_st);
                write_str("\n");
            } else {
                write_str("AV_ERR: unexpected magic at 0x401320960: ");
                write_hex32(vdt_w);
                write_str(" x ");
                write_hex32(vdt_h);
                write_str("\n");
            }
            sc4(215, av_map, av_map_size, 0, 0);
        } else {
            write_str("AV_ERR: mmap 0x401000000 failed\n");
        }

        // 3. Patch CP RAM
        uint64_t uipc_base = find_module_base("libosal_uipc.so");
        if (uipc_base) {
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
                uint64_t page_off = dmm_phys & 0xfffULL;
                uint64_t page_base = dmm_phys - page_off;
                long map_res = sc6(222, 0, 4096 + page_off, 3, 1, mem_fd, page_base);
                if (map_res > 0 && (uint64_t)map_res < 0xfffffffffffff000ULL) {
                    uint8_t *buf = (uint8_t *)(map_res + page_off);
                    uint32_t num_cp = sizeof(kCp2560Patches) / sizeof(kCp2560Patches[0]);
                    uint32_t cp_og = 0, cp_st = 0, cp_oth = 0;

                    for (uint32_t idx = 0; idx < num_cp; ++idx) {
                        const Patch4 &p = kCp2560Patches[idx];
                        uint32_t aligned_start = p.addr & ~0x3fU;
                        uint32_t aligned_end = (p.addr + 4 + 0x3fU) & ~0x3fU;
                        uint32_t aligned_size = aligned_end - aligned_start;
                        uint32_t offset = p.addr - aligned_start;

                        for (uint32_t i = 0; i < aligned_size; ++i) buf[i] = 0;
                        if (my_sen_dma_transfer(0, aligned_start, dmm_phys, aligned_size) != 0) {
                            ++cp_oth;
                            continue;
                        }
                        if (mode_cp == 1 || mode_cp == 2) {
                            *(uint32_t *)(buf + offset) = (mode_cp == 1) ? p.og : p.stock;
                            __asm__ volatile("dsb sy" ::: "memory");
                            my_sen_dma_transfer(1, dmm_phys, aligned_start, aligned_size);
                            for (uint32_t i = 0; i < aligned_size; ++i) buf[i] = 0;
                            __asm__ volatile("dsb sy" ::: "memory");
                            my_sen_dma_transfer(0, aligned_start, dmm_phys, aligned_size);
                        }
                        uint32_t cur = *(uint32_t *)(buf + offset);
                        if (cur == p.og) ++cp_og;
                        else if (cur == p.stock) ++cp_st;
                        else {
                            ++cp_oth;
                            write_str("  CP_OTHER @ ");
                            write_hex32(p.addr);
                            write_str(" = ");
                            write_hex32(cur);
                            write_str("\n");
                        }
                    }
                    write_str("CP_RAM: OG_2560=");
                    write_u32(cp_og);
                    write_str(" STOCK_2160=");
                    write_u32(cp_st);
                    write_str(" OTHER=");
                    write_u32(cp_oth);
                    write_str("\n");
                    sc4(215, map_res, 4096 + page_off, 0, 0);
                }
                my_dmm_free(handle);
            }
        }
        sc4(57, mem_fd, 0, 0, 0);
    }
    if (g_log_fd >= 0) sc4(57, g_log_fd, 0, 0, 0);
}
