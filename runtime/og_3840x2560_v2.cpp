#include <stdint.h>
#include "og_2560_patches.h"

// ============================================================================
// Surgical Staged 3840x2560 (True 3:2 1:1 SAR) RAM Patch v2 for Sony A7S III
// FW 5.01 — Zero NAND writes; command/logs in /tmp_wbshd (volatile tmpfs).
//
// Command tokens in /tmp_wbshd/res2560_cmd (or /tmp/jiritsu/res2560_cmd):
//   "status"  : inspect all patch groups without modifying memory
//   "restore" : restore all patched groups back to stock 3840x2160
//   "apply"   : apply selected sub-groups (or default Stage 1: mp + imola + avlog):
//     - "mp"     : appFw.so (14 sites) + libmpr.so (33 sites, incl. level_idc & 0x2260a0)
//     - "lvl6"   : libmpr.so HEVC level_idc = 180 (Level 6.0) for >8.91M luma samples
//     - "imola"  : av-cam.bin Imola text (ENCHANDLER 0x217f5c key=2 fix, 0x239900, 0x2765c0)
//                  + Imola-only .rodata (0x38d968..0x3e9000), leaving VDT 3840x2160 untouched!
//     - "tile1"  : force CMNHANDLER hevc_tile_mode = 1 (0x2116fc, 0x211770)
//     - "dec"    : raise IMLDEC 4K threshold (0x231eac, 0x231f70, 0x2322f8, 0x2357dc)
//     - "vdt"    : Stage 2 VDT 2560 text + 0x320960 RC_RESIZE input table
//     - "vdt488" : keep VDT PCIe RDIMG in unit 0x488 instead of 0x489 (0x1c4bec)
//     - "cpvef"  : Stage 2 surgical CP Movie YC / VEF scaler ONLY (no VOUT/HDMI VSYNC!)
//     - "avlog"  : enable Imola/CMNHANDLER/ENCHANDLER kikilog flags in DDR .bss (0x401ba1890)
// ============================================================================

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

static void flush_icache_user(uint64_t addr) {
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
        sc4(226, (long)page, 8192, 7, 0);
        volatile uint32_t *ptr = (volatile uint32_t *)target_va;
        uint32_t cur = *ptr;
        if (mode == 1 && cur == patches[i].stock) {
            *ptr = patches[i].og;
            flush_icache_user(target_va);
        } else if (mode == 2 && cur == patches[i].og) {
            *ptr = patches[i].stock;
            flush_icache_user(target_va);
        }
        cur = *ptr;
        if (cur == patches[i].og) ++n_og;
        else if (cur == patches[i].stock) ++n_st;
        else {
            ++n_other;
            write_str("  MISMATCH ");
            write_str(label);
            write_str(" @ ");
            write_hex32(patches[i].addr);
            write_str(" = ");
            write_hex32(cur);
            write_str("\n");
        }
    }
    write_str(label);
    write_str(": OG=");
    write_u32(n_og);
    write_str(" STOCK=");
    write_u32(n_st);
    write_str(" OTHER=");
    write_u32(n_other);
    write_str("\n");
}

static void patch_av_words(const char *label, uint8_t *av_ptr, const Patch4 *patches, uint32_t count, int mode) {
    uint32_t n_og = 0, n_st = 0, n_oth = 0;
    for (uint32_t i = 0; i < count; ++i) {
        volatile uint32_t *p = (volatile uint32_t *)(av_ptr + patches[i].addr);
        uint32_t cur = *p;
        if (mode == 1 && cur == patches[i].stock) {
            *p = patches[i].og;
            __asm__ volatile("dsb sy" ::: "memory");
        } else if (mode == 2 && cur == patches[i].og) {
            *p = patches[i].stock;
            __asm__ volatile("dsb sy" ::: "memory");
        }
        cur = *p;
        if (cur == patches[i].og) ++n_og;
        else if (cur == patches[i].stock) ++n_st;
        else {
            ++n_oth;
            write_str("  AV_MISMATCH ");
            write_str(label);
            write_str(" @ ");
            write_hex32(patches[i].addr);
            write_str(" = ");
            write_hex32(cur);
            write_str("\n");
        }
    }
    write_str(label);
    write_str(": OG=");
    write_u32(n_og);
    write_str(" STOCK=");
    write_u32(n_st);
    write_str(" OTHER=");
    write_u32(n_oth);
    write_str("\n");
}

// --- Extra V2 patches discovered by static disassembly ---

static const Patch4 kAppFwExtraPatches[] = {
    { 0x016ddb58U, 0x52810e00U, 0x52814000U }, // mov w0, #0x870 -> #0xa00 (packed 3840<<16 | h)
};

static const Patch4 kMprExtraPatches[] = {
    { 0x0018629cU, 0x52810e01U, 0x52814001U }, // mov w1, #0x870 -> #0xa00
    { 0x00223880U, 0x7121c01fU, 0x7128001fU }, // H.264 level_idc: cmp w0, #0x870 -> #0xa00
    { 0x002239dcU, 0x7121c01fU, 0x7128001fU }, // HEVC level_idc:  cmp w0, #0x870 -> #0xa00
    { 0x002260a0U, 0x72a10e01U, 0x72a14001U }, // packed res: movk w1, #0x870, lsl #16 -> #0xa00
};

// HEVC Level 6.0 (0xb4) is required for >8.91M luma samples (3840x2560 = 9.83M px).
// H.264 levels (5.1/5.2) MUST remain stock: Imola2 accepts 3840x2560 at 5.1/5.2,
// whereas forcing H.264 level 6.0 trips libmpr.so's AVC Long-GOP playback validator ("Movie Error").
static const Patch4 kMprHevcLvl6Patches[] = {
    { 0x00223a78U, 0x528012c0U, 0x52801680U }, // HEVC Level 5.0 (0x96) -> Level 6.0 (0xb4)
    { 0x00223a94U, 0x52801380U, 0x52801680U }, // HEVC Level 5.2 (0x9c) -> Level 6.0 (0xb4)
    { 0x00223a9cU, 0x52801320U, 0x52801680U }, // HEVC Level 5.1 (0x99) -> Level 6.0 (0xb4)
};

static const Patch4 kAvImolaTextPatches[] = {
    // SFMC enc_util::is_3840x2160 exact equality check (used by sfmc_enc_timing yc_pin_rate!)
    { 0x001a132cU, 0x7121c11fU, 0x7128011fU }, // cmp w8, #0x870 -> #0xa00
    // ENCHANDLER 4K pixel-count & pixel-rate ceilings:
    //   0x217f5c: 0x21c001 + 0x654000 (8,847,361) -> +0x800000 (10,600,449) for 3840x2560 (9.83M px)
    //   0x217f68: 0x1fa40000 (530.84M px/s) -> 0x48000000 (1,207.96M px/s) so up to 120p stays in key=2
    { 0x00217f5cU, 0x91595108U, 0x91600108U },
    { 0x00217f68U, 0x52a3f489U, 0x52a90009U },
    // IMLENC aspect / height validators:
    { 0x00239900U, 0x7121c2bfU, 0x712802bfU }, // cmp w21, #0x870 -> #0xa00
    { 0x002765c0U, 0x7121c11fU, 0x7128011fU }, // cmp w8, #0x870  -> #0xa00
    // SFMC enc_config image_size_info (0x354bd0 entry for 3840x2160 -> 3840x2560)
    { 0x00354c04U, 0x0f000870U, 0x0f000a00U },
    { 0x00354c08U, 0x00000870U, 0x00000a00U },
    { 0x00354c10U, 0x00000870U, 0x00000a00U },
    { 0x00354c18U, 0x00000870U, 0x00000a00U },
    // SFMC enc_timing_normal convert_ext_isp_clip_size (3840x2160 -> QFHD 0x17)
    { 0x0038bc34U, 0x00000870U, 0x00000a00U },
    // ENCHANDLER unittype DMM table (key=2 @ 0x38d8e8, key=4 @ 0x38d908):
    // Swap unused DRAM CD unit 0x486 (96.12 MB) into LD slot (stock 0x474 = 48.44 MB)!
    // Imola2 createEncPool needs 56.28 MB LD for 3840x2560 4:2:2 10-bit (ESRAM CD stays 2.38 MB).
    { 0x0038d8e8U, 0x00000486U, 0x00000474U },
    { 0x0038d8ecU, 0x00000474U, 0x00000486U },
    { 0x0038d908U, 0x00000486U, 0x00000474U },
    { 0x0038d90cU, 0x00000474U, 0x00000486U },
};

static const Patch4 kAvImolaTile1Patches[] = {
    { 0x002116fcU, 0x1a9f17eaU, 0x5280002aU }, // CMNHANDLER open: cset w10, eq -> mov w10, #1
    { 0x00211770U, 0x1a9f17e8U, 0x52800028U }, // CMNHANDLER log:  cset w8, eq  -> mov w8, #1
};

static const Patch4 kAvImolaDecPatches[] = {
    { 0x00231eacU, 0x115952e9U, 0x116002e9U },
    { 0x00231f70U, 0x115952e9U, 0x116002e9U },
    { 0x002322f8U, 0x1159514aU, 0x1160014aU },
    { 0x002357dcU, 0x11595309U, 0x11600309U },
};

static const Patch4 kAvVdt488Patches[] = {
    { 0x001c4becU, 0x11595149U, 0x11600149U }, // VDT PCIe RDIMG threshold: 0x870000 -> 0xa1c000
};

// Surgical CP Movie YC / VEF scaler patches (excludes 0x0040afae CAMLV, 0x00b76e20 JPEG, and 0x00d2d8dc VOUT/HDMI!)
static const Patch4 kCpSurgicalVefPatches[] = {
    { 0x013ec2dcU, 0x08700f00U, 0x0a000f00U },
    { 0x013ec2e4U, 0x08700f00U, 0x0a000f00U },
    { 0x001c4898U, 0x6207f44fU, 0x6220f44fU }, // mm open yc
    { 0x001c48b0U, 0x6207f44fU, 0x6220f44fU }, // mm open yc
    { 0x00631c08U, 0x6007f44fU, 0x6020f44fU }, // VEF scaler
    { 0x00631c30U, 0x6007f44fU, 0x6020f44fU }, // VEF scaler
    { 0x00b9f8e8U, 0x08700f00U, 0x0a000f00U }, // mm open yc table
    { 0x00b9f9c0U, 0x08700f00U, 0x0a000f00U }, // mm open yc table
    { 0x00bbbd10U, 0x08700f00U, 0x0a000f00U },
    { 0x00bbbd3cU, 0x08700f00U, 0x0a000f00U },
    { 0x00bbbd68U, 0x08700f00U, 0x0a000f00U },
    { 0x00bbbd94U, 0x08700f00U, 0x0a000f00U },
    { 0x00bbbdc0U, 0x08700f00U, 0x0a000f00U },
    { 0x00bbbdecU, 0x08700f00U, 0x0a000f00U },
    { 0x00bbbe18U, 0x08700f00U, 0x0a000f00U },
    { 0x00bbbe44U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff20c0U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff20c4U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff20d4U, 0x008700f0U, 0x00a000f0U }, // MB grid 240x135 -> 240x160
    { 0x00ff23c0U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff23c4U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff23d4U, 0x008700f0U, 0x00a000f0U },
    { 0x00ff26c0U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff26c4U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff26d4U, 0x008700f0U, 0x00a000f0U },
    { 0x00ff29c0U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff29c4U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff29d4U, 0x008700f0U, 0x00a000f0U },
    { 0x00ff2cc0U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff2cc4U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff2cd4U, 0x008700f0U, 0x00a000f0U },
    { 0x00ff2fc0U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff2fc4U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff2fd4U, 0x008700f0U, 0x00a000f0U },
    { 0x00ff32c0U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff32c4U, 0x08700f00U, 0x0a000f00U },
    { 0x00ff32d4U, 0x008700f0U, 0x00a000f0U },
};

// OSAL + CP DMA helpers (used only if cpvef is requested or inspected)
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
        uint8_t b0, b1, dir, b3;
        uint32_t size;
        uint64_t src, dst;
    } req = { 0x00, 0x05, (uint8_t)dir, 0x00, size, src_addr, dst_addr };
    long rc = sc4(29, fd, 0x40185001UL, (long)&req, 0);
    sc4(57, fd, 0, 0, 0);
    if (rc != 0) return -3;
    if (dir == 1) my_sen_control_cache_cp(1, cp_addr, size);
    return 0;
}

static int is_interesting_ulog_line(const char *s) {
    return contains(s, "[CMNHANDLER]") || contains(s, "[ENCHANDLER]") ||
           contains(s, "[IMLENC") || contains(s, "[IMLDEC") ||
           contains(s, "[STATE") || contains(s, "[TIMING") ||
           contains(s, "[ENC_UTIL") || contains(s, "[VDT]") ||
           contains(s, "BlackBelt") || contains(s, "Imola") ||
           contains(s, "bso_done") || contains(s, "file:") ||
           contains(s, "2560");
}

static void dump_ulog_ring(long mem_fd) {
    // Physical ulogio LOGB (0x405000000, 0x778000) + PRTB (0x405778000, 0x8000)
    const uint64_t ulog_phys = 0x405000000ULL;
    const uint32_t ulog_size = 0x00780000U;
    long umap = sc6(222, 0, ulog_size, 1, 1, mem_fd, ulog_phys); // PROT_READ, MAP_SHARED
    if (umap <= 0 || (uint64_t)umap >= 0xfffffffffffff000ULL) {
        write_str("ULOG_SCAN: mmap 0x405000000 failed\n");
        return;
    }
    long out_fd = sc4(56, -100, (long)"/tmp_wbshd/og-2560-ulog.txt", 1 | 64 | 512, 0644);
    const uint8_t *base = (const uint8_t *)umap;
    char line[320];
    uint32_t n_lines = 0;
    uint32_t n_bytes = 0;
    const uint32_t max_bytes = 131072U;
    uint32_t i = 0;
    while (i < ulog_size && n_bytes < max_bytes) {
        // Fast skip 8-byte zero runs
        if ((i & 7U) == 0 && i + 8 <= ulog_size && *(const uint64_t *)(base + i) == 0) {
            i += 8;
            continue;
        }
        uint8_t c = base[i];
        if (c >= 0x20 && c < 0x7f) {
            uint32_t len = 0;
            while (i < ulog_size && len < 300) {
                uint8_t ch = base[i];
                if (ch >= 0x20 && ch < 0x7f) {
                    line[len++] = (char)ch;
                    ++i;
                } else {
                    break;
                }
            }
            line[len] = 0;
            if (len >= 12 && is_interesting_ulog_line(line)) {
                line[len] = '\n';
                if (out_fd >= 0) sc4(64, out_fd, (long)line, (long)(len + 1), 0);
                ++n_lines;
                n_bytes += len + 1;
            }
        } else {
            ++i;
        }
    }
    if (out_fd >= 0) sc4(57, out_fd, 0, 0, 0);
    sc4(215, umap, ulog_size, 0, 0);
    write_str("ULOG_SCAN: lines=");
    write_u32(n_lines);
    write_str(" bytes=");
    write_u32(n_bytes);
    write_str("\n");
}

extern "C" __attribute__((visibility("default"))) void _init(void) {
    g_log_fd = sc4(56, -100, (long)"/tmp_wbshd/og-2560-v2.log", 1 | 64 | 512, 0644);
    if (g_log_fd < 0) {
        g_log_fd = sc4(56, -100, (long)"/tmp/jiritsu/og-2560-v2.log", 1 | 64 | 512, 0644);
    }

    char cmd_buf[128];
    for (int i = 0; i < 128; ++i) cmd_buf[i] = 0;
    if (read_file("/tmp_wbshd/res2560_cmd", cmd_buf, sizeof(cmd_buf)) <= 0) {
        read_file("/tmp/jiritsu/res2560_cmd", cmd_buf, sizeof(cmd_buf));
    }

    int mode = 0; // 0 = status, 1 = apply, 2 = restore
    if (contains(cmd_buf, "apply")) mode = 1;
    else if (contains(cmd_buf, "restore")) mode = 2;

    int any_sub = contains(cmd_buf, "mp") || contains(cmd_buf, "lvl6") ||
                  contains(cmd_buf, "imola") || contains(cmd_buf, "tile1") ||
                  contains(cmd_buf, "dec") || contains(cmd_buf, "vdt") ||
                  contains(cmd_buf, "cpvef") || contains(cmd_buf, "avlog");

    // Default "apply" with no sub-tokens runs Safe Stage 1: mp + imola + avlog!
    int m_mp     = (mode == 2) ? 2 : ((mode == 1 && (!any_sub || contains(cmd_buf, "mp"))) ? 1 : 0);
    int m_lvl6   = (mode == 2) ? 2 : ((mode == 1 && contains(cmd_buf, "lvl6")) ? 1 : 0);
    int m_imola  = (mode == 2) ? 2 : ((mode == 1 && (!any_sub || contains(cmd_buf, "imola"))) ? 1 : 0);
    int m_tile1  = (mode == 2) ? 2 : ((mode == 1 && contains(cmd_buf, "tile1")) ? 1 : 0);
    int m_dec    = (mode == 2) ? 2 : ((mode == 1 && contains(cmd_buf, "dec")) ? 1 : 0);
    int m_vdt    = (mode == 2) ? 2 : ((mode == 1 && contains(cmd_buf, "vdt")) ? 1 : 0);
    int m_vdt488 = (mode == 2) ? 2 : ((mode == 1 && contains(cmd_buf, "vdt488")) ? 1 : 0);
    int m_cpvef  = (mode == 2) ? 2 : ((mode == 1 && contains(cmd_buf, "cpvef")) ? 1 : 0);
    int m_avlog  = (mode == 2) ? 2 : ((mode == 1 && (!any_sub || contains(cmd_buf, "avlog"))) ? 1 : 0);

    write_str(mode == 1 ? "=== OG 3840x2560 V2 APPLY ===\nCMD: " :
             (mode == 2 ? "=== OG 3840x2560 V2 RESTORE ===\nCMD: " :
                          "=== OG 3840x2560 V2 STATUS ===\nCMD: "));
    write_str(cmd_buf);
    write_str("\n");

    // Note: if anamorphic tkhd=2560 patch was active from Open Gate 2160 profile,
    // restore MP_tkhd back to stock (lsl #16) so 3840x2560 uses native width/height cleanly,
    // and pause og_menu_daemon's USB-exit loop so it does not overwrite AV_VUI_SAR=1:1.
    uint64_t appfw_base = find_module_base("appFw.so");
    if (mode == 1 && appfw_base) {
        *(volatile uint32_t *)(appfw_base + 0x7f83494) = 0x32353630U; // pause og_worker during 2560 test
    }
    patch_mp_module("MP_appFw_base", appfw_base, kAppFw2560Patches, sizeof(kAppFw2560Patches) / sizeof(Patch4), m_mp);
    patch_mp_module("MP_appFw_v2", appfw_base, kAppFwExtraPatches, sizeof(kAppFwExtraPatches) / sizeof(Patch4), m_mp);

    uint64_t mpr_base = find_module_base("libmpr.so");
    if (m_mp == 1 && mpr_base) {
        static const Patch4 kMprTkhdStockReset[] = {
            { 0x002272b0U, 0x53103ef7U, 0x52a14017U },
            { 0x00229a24U, 0x53103c84U, 0x52a14004U },
        };
        patch_mp_module("MP_tkhd_reset", mpr_base, kMprTkhdStockReset, 2, 2);
    }
    patch_mp_module("MP_mpr_base", mpr_base, kMpr2560Patches, sizeof(kMpr2560Patches) / sizeof(Patch4), m_mp);
    patch_mp_module("MP_mpr_v2", mpr_base, kMprExtraPatches, sizeof(kMprExtraPatches) / sizeof(Patch4), m_mp);
    patch_mp_module("MP_mpr_lvl6", mpr_base, kMprHevcLvl6Patches, sizeof(kMprHevcLvl6Patches) / sizeof(Patch4), m_lvl6);

    long mem_fd = sc4(56, -100, (long)"/dev/mem", 0x101002, 0); // O_RDWR | O_SYNC
    if (mem_fd >= 0) {
        const uint64_t av_phys_base = 0x401000000ULL;
        const uint32_t av_map_size  = 0x00c00000U; // 12 MB covers .text, .rodata AND .bss (0xba008c)!
        long av_map = sc6(222, 0, av_map_size, 3, 1, mem_fd, av_phys_base);
        if (av_map > 0 && (uint64_t)av_map < 0xfffffffffffff000ULL) {
            uint8_t *av_ptr = (uint8_t *)av_map;
            uint32_t vdt_w = *(volatile uint32_t *)(av_ptr + 0x320960);
            uint32_t vdt_h = *(volatile uint32_t *)(av_ptr + 0x320964);
            if (vdt_w == 3840 && (vdt_h == 2160 || vdt_h == 2560)) {
                // 1) Restore VUI SAR 27:32 -> 1:1 if m_imola == 1 (native square-pixel 3840x2560!)
                if (m_imola == 1) {
                    static const Patch4 kAvVuiSarReset[] = {
                        { 0x0021c008, 0x3943b52a, 0x52801fea },
                        { 0x0021c010, 0x3943b92a, 0x5280036a },
                        { 0x0021c018, 0x3943bd2a, 0x5280040a },
                        { 0x0021c388, 0x3943b14b, 0x52801feb },
                        { 0x0021c390, 0x3943b54b, 0x5280036b },
                        { 0x0021c398, 0x3943b94b, 0x5280040b },
                    };
                    patch_av_words("AV_VUI_1x1_RESET", av_ptr, kAvVuiSarReset, 6, 2);
                }

                // 2) Imola text + Imola-only .rodata (0x38d968..0x3e9000)
                patch_av_words("AV_IMOLA_TEXT", av_ptr, kAvImolaTextPatches, sizeof(kAvImolaTextPatches) / sizeof(Patch4), m_imola);
                patch_av_words("AV_IMOLA_TILE1", av_ptr, kAvImolaTile1Patches, sizeof(kAvImolaTile1Patches) / sizeof(Patch4), m_tile1);
                patch_av_words("AV_IMOLA_DEC", av_ptr, kAvImolaDecPatches, sizeof(kAvImolaDecPatches) / sizeof(Patch4), m_dec);

                uint32_t im_ro32_og = 0, im_ro32_st = 0;
                for (uint32_t off = 0x38d968; off <= 0x3e9000 - 8; off += 4) {
                    volatile uint32_t *wp = (volatile uint32_t *)(av_ptr + off);
                    volatile uint32_t *hp = (volatile uint32_t *)(av_ptr + off + 4);
                    if (*wp == 3840) {
                        uint32_t h = *hp;
                        if (h == 2160 || h == 2560) {
                            if (m_imola == 1 && h == 2160) *hp = 2560;
                            else if (m_imola == 2 && h == 2560) *hp = 2160;
                            if (*hp == 2560) ++im_ro32_og;
                            else if (*hp == 2160) ++im_ro32_st;
                        }
                    }
                }
                uint32_t im_ro16_og = 0, im_ro16_st = 0;
                for (uint32_t off = 0x38d968; off <= 0x3e9000 - 4; off += 2) {
                    volatile uint16_t *wp = (volatile uint16_t *)(av_ptr + off);
                    volatile uint16_t *hp = (volatile uint16_t *)(av_ptr + off + 2);
                    if (*wp == 3840) {
                        uint16_t h = *hp;
                        if (h == 2160 || h == 2560) {
                            if (m_imola == 1 && h == 2160) *hp = 2560;
                            else if (m_imola == 2 && h == 2560) *hp = 2160;
                            if (*hp == 2560) ++im_ro16_og;
                            else if (*hp == 2160) ++im_ro16_st;
                        }
                    }
                }
                __asm__ volatile("dsb sy" ::: "memory");
                write_str("AV_IMOLA_RO32: OG=");
                write_u32(im_ro32_og);
                write_str(" STOCK=");
                write_u32(im_ro32_st);
                write_str("\nAV_IMOLA_RO16: OG=");
                write_u32(im_ro16_og);
                write_str(" STOCK=");
                write_u32(im_ro16_st);
                write_str("\n");

                // 2b) Imola EncStart tables (HEVC 0x3bd2f8..0x3d0ef8, AVC 0x3a5cf8..0x3bd1f8):
                //     Patch Vsize_Core0 (1088/1072 -> 1280) under m_imola and Level (-> 6.0) under m_lvl6!
                uint32_t hevc_v0_og = 0, hevc_v0_st = 0, hevc_lvl_og = 0, hevc_lvl_st = 0;
                for (uint32_t off = 0x3bd2f8U; off < 0x3d0ef8U; off += 0x100U) {
                    uint16_t ew = *(volatile uint16_t *)(av_ptr + off + 4);
                    uint16_t eh = *(volatile uint16_t *)(av_ptr + off + 6);
                    if (ew == 3840 && (eh == 2160 || eh == 2560)) {
                        volatile uint16_t *v0p = (volatile uint16_t *)(av_ptr + off + 0x56);
                        uint16_t v0 = *v0p;
                        if (m_imola == 1 && v0 == 1088) *v0p = 1280;
                        else if (m_imola == 2 && v0 == 1280) *v0p = 1088;
                        v0 = *v0p;
                        if (v0 == 1280) ++hevc_v0_og;
                        else if (v0 == 1088) ++hevc_v0_st;

                        volatile uint8_t *lvp = (volatile uint8_t *)(av_ptr + off + 0x9e);
                        uint8_t lv = *lvp;
                        if (m_lvl6 == 1 && (lv == 0x96 || lv == 0x99 || lv == 0x9c)) *lvp = 0xb4;
                        lv = *lvp;
                        if (lv == 0xb4) ++hevc_lvl_og;
                        else ++hevc_lvl_st;
                    }
                }
                uint32_t avc_v0_og = 0, avc_v0_st = 0;
                for (uint32_t off = 0x3a5cf8U; off < 0x3bd1f8U; off += 0x100U) {
                    uint16_t ew = *(volatile uint16_t *)(av_ptr + off + 4);
                    uint16_t eh = *(volatile uint16_t *)(av_ptr + off + 6);
                    if (ew == 3840 && (eh == 2160 || eh == 2560)) {
                        volatile uint16_t *v0p = (volatile uint16_t *)(av_ptr + off + 0x56);
                        uint16_t v0 = *v0p;
                        if (m_imola == 1 && v0 == 1072) *v0p = 1280;
                        else if (m_imola == 2 && v0 == 1280) *v0p = 1072;
                        v0 = *v0p;
                        if (v0 == 1280) ++avc_v0_og;
                        else if (v0 == 1072) ++avc_v0_st;
                        // Keep AVC level_idc (off + 0xa7) stock (5.1/5.2) for in-camera playback compatibility.
                    }
                }
                __asm__ volatile("dsb sy" ::: "memory");
                write_str("AV_HEVC_VSIZE0: OG=");
                write_u32(hevc_v0_og);
                write_str(" STOCK=");
                write_u32(hevc_v0_st);
                write_str("\nAV_HEVC_LVL6: OG=");
                write_u32(hevc_lvl_og);
                write_str(" STOCK=");
                write_u32(hevc_lvl_st);
                write_str("\nAV_AVC_VSIZE0: OG=");
                write_u32(avc_v0_og);
                write_str(" STOCK=");
                write_u32(avc_v0_st);
                write_str("\n");

                // 3) Stage 2 VDT text + 0x320960 RC_RESIZE input table (kept STOCK in Stage 1!)
                patch_av_words("AV_VDT_TEXT", av_ptr, kAvText2560Patches, sizeof(kAvText2560Patches) / sizeof(Patch4), m_vdt);
                patch_av_words("AV_VDT_488", av_ptr, kAvVdt488Patches, sizeof(kAvVdt488Patches) / sizeof(Patch4), m_vdt488);
                volatile uint32_t *vdt_ro_h = (volatile uint32_t *)(av_ptr + 0x320964);
                if (m_vdt == 1 && *vdt_ro_h == 2160) *vdt_ro_h = 2560;
                else if (m_vdt == 2 && *vdt_ro_h == 2560) *vdt_ro_h = 2160;
                __asm__ volatile("dsb sy" ::: "memory");
                write_str("AV_VDT_320964: ");
                write_u32(*vdt_ro_h);
                write_str("\n");

                // 4) Enable Imola/SFMC ulogio levels in .bss (0xba008c + 0x1804[13] & 0x186c[20])
                uint8_t *log_ctl = av_ptr + 0xba008c;
                if (m_avlog == 1) {
                    *(volatile uint8_t *)(log_ctl + 0x1908) = 0; // unmute
                    for (uint32_t lvl = 0; lvl < 13; ++lvl) {
                        *(volatile uint8_t *)(log_ctl + 0x1804 + lvl * 8) = 1;
                    }
                    for (uint32_t lvl = 0; lvl < 20; ++lvl) {
                        *(volatile uint8_t *)(log_ctl + 0x186c + lvl * 8) = 1;
                    }
                    // Refill error-log byte quotas for subsys 0x57 (SFMC/VDT) and 0x68 (Imola)
                    *(volatile uint32_t *)(log_ctl + 0x57U * 0x18U + 0x14U) = 0x10000U;
                    *(volatile uint32_t *)(log_ctl + 0x68U * 0x18U + 0x14U) = 0x10000U;
                    __asm__ volatile("dsb sy" ::: "memory");
                }
                write_str("AV_LOG_CTL[0..3]: ");
                write_u32(*(volatile uint8_t *)(log_ctl + 0x1804));
                write_str(",");
                write_u32(*(volatile uint8_t *)(log_ctl + 0x180c));
                write_str(",");
                write_u32(*(volatile uint8_t *)(log_ctl + 0x1814));
                write_str(",");
                write_u32(*(volatile uint8_t *)(log_ctl + 0x181c));
                write_str(" mute=");
                write_u32(*(volatile uint8_t *)(log_ctl + 0x1908));
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

        // 4b) Optional live ulogio ring-buffer scan (0x405000000..0x405780000)
        if (contains(cmd_buf, "dumplog")) {
            dump_ulog_ring(mem_fd);
        }

        // 5) Surgical CP VEF Movie YC patches (ONLY touched if cpvef or restore or status!)
        if (m_cpvef != 0 || contains(cmd_buf, "cpvef")) {
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
                if (*(volatile uint64_t *)(uipc_base + 0x1b560 + 8) == 0) p_u_osal_init();

                uint32_t handle = 0;
                uint64_t dmm_phys = my_dmm_alloc(4096, &handle);
                if (dmm_phys != 0) {
                    uint64_t page_off = dmm_phys & 0xfffULL;
                    uint64_t page_base = dmm_phys - page_off;
                    long map_res = sc6(222, 0, 4096 + page_off, 3, 1, mem_fd, page_base);
                    if (map_res > 0 && (uint64_t)map_res < 0xfffffffffffff000ULL) {
                        uint8_t *buf = (uint8_t *)(map_res + page_off);
                        uint32_t num_cp = sizeof(kCpSurgicalVefPatches) / sizeof(Patch4);
                        uint32_t cp_og = 0, cp_st = 0, cp_oth = 0;
                        for (uint32_t idx = 0; idx < num_cp; ++idx) {
                            const Patch4 &p = kCpSurgicalVefPatches[idx];
                            uint32_t aligned_start = p.addr & ~0x3fU;
                            uint32_t aligned_end = (p.addr + 4 + 0x3fU) & ~0x3fU;
                            uint32_t aligned_size = aligned_end - aligned_start;
                            uint32_t offset = p.addr - aligned_start;
                            for (uint32_t i = 0; i < aligned_size; ++i) buf[i] = 0;
                            if (my_sen_dma_transfer(0, aligned_start, dmm_phys, aligned_size) != 0) {
                                ++cp_oth;
                                continue;
                            }
                            uint32_t cur = *(uint32_t *)(buf + offset);
                            if ((m_cpvef == 1 && cur == p.stock) || (m_cpvef == 2 && cur == p.og)) {
                                *(uint32_t *)(buf + offset) = (m_cpvef == 1) ? p.og : p.stock;
                                __asm__ volatile("dsb sy" ::: "memory");
                                my_sen_dma_transfer(1, dmm_phys, aligned_start, aligned_size);
                                for (uint32_t i = 0; i < aligned_size; ++i) buf[i] = 0;
                                __asm__ volatile("dsb sy" ::: "memory");
                                my_sen_dma_transfer(0, aligned_start, dmm_phys, aligned_size);
                            }
                            cur = *(uint32_t *)(buf + offset);
                            if (cur == p.og) ++cp_og;
                            else if (cur == p.stock) ++cp_st;
                            else ++cp_oth;
                        }
                        write_str("CP_VEF: OG=");
                        write_u32(cp_og);
                        write_str(" STOCK=");
                        write_u32(cp_st);
                        write_str(" OTHER=");
                        write_u32(cp_oth);
                        write_str("\n");
                        sc4(215, map_res, 4096 + page_off, 0, 0);
                    }
                    my_dmm_free(handle);
                }
            }
        }
        sc4(57, mem_fd, 0, 0, 0);
    }
    if (g_log_fd >= 0) sc4(57, g_log_fd, 0, 0, 0);
}
