#include <stdint.h>
#include "cp_og_patches.h"
#include "og_2560_patches.h"

extern "C" {
int pthread_create(uint64_t *thread, const void *attr, void *(*start_routine)(void *), void *arg);
int usleep(unsigned int usec);
}

static const Patch4 kMprTkhd2560Patches[] = {
    { 0x002272b0, 0x53103ef7, 0x52a14017 }, // tkhd.height: lsl w23, w23, #16 (kept STOCK for native 3840x2560)
    { 0x00229a24, 0x53103c84, 0x52a14004 }, // Rotate tkhd.height: lsl w4, w4, #16 (kept STOCK for native 3840x2560)
};

static const Patch4 kAvVuiSar27x32Patches[] = {
    // HEVC VUI SAR (kept STOCK 1:1 for native square-pixel 3840x2560)
    { 0x0021c008, 0x3943b52a, 0x52801fea },
    { 0x0021c010, 0x3943b92a, 0x5280036a },
    { 0x0021c018, 0x3943bd2a, 0x5280040a },
    // H.264 VUI SAR (kept STOCK 1:1 for native square-pixel 3840x2560)
    { 0x0021c388, 0x3943b14b, 0x52801feb },
    { 0x0021c390, 0x3943b54b, 0x5280036b },
    { 0x0021c398, 0x3943b94b, 0x5280040b },
};

// Native 3840x2560 (1:1 SAR, true 3:2) MP + AV + CP VEF patch tables
static const Patch4 kAppFwExtra2560[] = {
    { 0x016ddb58U, 0x52810e00U, 0x52814000U }, // mov w0, #0x870 -> #0xa00
};

static const Patch4 kMprExtra2560[] = {
    { 0x0018629cU, 0x52810e01U, 0x52814001U }, // mov w1, #0x870 -> #0xa00
    { 0x00223880U, 0x7121c01fU, 0x7128001fU }, // H.264 level_idc: cmp w0, #0x870 -> #0xa00
    { 0x002239dcU, 0x7121c01fU, 0x7128001fU }, // HEVC level_idc:  cmp w0, #0x870 -> #0xa00
    { 0x002260a0U, 0x72a10e01U, 0x72a14001U }, // packed res: movk w1, #0x870, lsl #16 -> #0xa00
};

// HEVC Level 6.0 (0xb4 = 180) for >8.91M luma samples (3840x2560 = 9.83M).
// Note: H.264 levels remain stock (5.1/5.2/6.0) so Sony's in-camera AVC playback validator accepts 3840x2560!
static const Patch4 kMprHevcLvl6[] = {
    { 0x00223a78U, 0x528012c0U, 0x52801680U }, // HEVC Level 5.0 (0x96) -> Level 6.0 (0xb4)
    { 0x00223a94U, 0x52801380U, 0x52801680U }, // HEVC Level 5.2 (0x9c) -> Level 6.0 (0xb4)
    { 0x00223a9cU, 0x52801320U, 0x52801680U }, // HEVC Level 5.1 (0x99) -> Level 6.0 (0xb4)
};

static const Patch4 kAvImolaText2560[] = {
    { 0x001a132cU, 0x7121c11fU, 0x7128011fU }, // SFMC is_3840x2160: cmp w8, #0x870 -> #0xa00
    { 0x00217f5cU, 0x91595108U, 0x91600108U }, // ENCHANDLER 4K key=2 pixel ceiling: 8.85M -> 10.60M
    { 0x00217f68U, 0x52a3f489U, 0x52a90009U }, // ENCHANDLER 4K key=2 pixel-rate ceiling -> 1.21G px/s (up to 120p)
    { 0x002765c0U, 0x7121c11fU, 0x7128011fU }, // IMLENC height validator: cmp w8, #0x870 -> #0xa00
    { 0x00354c04U, 0x0f000870U, 0x0f000a00U }, // SFMC enc_config image_size_info
    { 0x00354c08U, 0x00000870U, 0x00000a00U },
    { 0x00354c10U, 0x00000870U, 0x00000a00U },
    { 0x00354c18U, 0x00000870U, 0x00000a00U },
    { 0x0038bc34U, 0x00000870U, 0x00000a00U }, // SFMC enc_timing_normal clip_size QFHD lookup
    // ENCHANDLER unittype DMM table (key=2 @ 0x38d8e8, key=4 @ 0x38d908):
    // Swap unused DRAM CD pool 0x486 (96.12 MB) into LD slot (stock 0x474 = 48.44 MB) so
    // Imola2 createEncPool's 56.28 MB LD requirement for 3840x2560 4:2:2 10-bit fits with 40 MB spare.
    { 0x0038d8e8U, 0x00000486U, 0x00000474U },
    { 0x0038d8ecU, 0x00000474U, 0x00000486U },
    { 0x0038d908U, 0x00000486U, 0x00000474U },
    { 0x0038d90cU, 0x00000474U, 0x00000486U },
};

// Always-on in-camera playback support for BOTH native 3840x2560 (Open Gate) and stock 3840x2160 clips:
// Routes 3840x2560 decode frames to 4K DMM pool 0x369 (12.66 MB, 28 slots) using Sony's native EBC 1.25 Bpp
// (5/4) decode format (0x0d for 10-bit, 0x09 for 8-bit, YC_PIN[0x28]=5 -> 11.72 MB <= 12.66 MB), leaving
// 3840x2160 100% stock (0x0e/0x0a, YC_PIN[0x28]=6). Applied unconditionally so 3:2 clips play even when OG=Off.
static const Patch4 kAvPlayback2560[] = {
    // 1. IMLDEC DMM unittype 4K pixel ceiling: 0x870001 -> 0x99c001 (uses 0x369, 28 slots, max_dmm_cnt=15)
    { 0x00231eacU, 0x115952e9U, 0x115e02e9U }, // add w9, w23, #0x780, lsl #12
    { 0x00231f70U, 0x115952e9U, 0x115e02e9U }, // add w9, w23, #0x780, lsl #12
    { 0x002322f8U, 0x1159514aU, 0x115e014aU }, // add w10, w10, #0x780, lsl #12
    { 0x002357dcU, 0x11595309U, 0x115e0309U }, // add w9, w24, #0x780, lsl #12
    // 2. ImolaCh decode yc_format: 3840x2560 -> 0x09 (8b 5/4) / 0x0d (10b 5/4); else stock 0x0a/0x0e
    { 0x002140b0U, 0xb9404ae8U, 0xb9404aebU }, // ldr w11, [x23, #0x48]
    { 0x002140b4U, 0x34000208U, 0x7100057fU }, // cmp w11, #1
    { 0x002140b8U, 0x7100051fU, 0x54000228U }, // b.hi #0x2140fc
    { 0x002140bcU, 0x54000201U, 0xf9400aa8U }, // ldr x8, [x21, #0x10]
    { 0x002140c0U, 0xf9400aa8U, 0x7940b909U }, // ldrh w9, [x8, #0x5c]
    { 0x002140c4U, 0x7940b909U, 0x7940bd0aU }, // ldrh w10, [x8, #0x5e]
    { 0x002140c8U, 0x32170feaU, 0x52800148U }, // mov w8, #0xa
    { 0x002140ccU, 0x6b0a013fU, 0x0b0b0908U }, // add w8, w8, w11, lsl #2
    { 0x002140d0U, 0x54000060U, 0x713c013fU }, // cmp w9, #0xf00
    { 0x002140d4U, 0x7140093fU, 0x54000321U }, // b.ne #0x214138
    { 0x002140d8U, 0x540002e1U, 0x7128015fU }, // cmp w10, #0xa00
    { 0x002140dcU, 0x7940bd08U, 0x540002e1U }, // b.ne #0x214138
    { 0x002140e0U, 0x52821c09U, 0x51000508U }, // sub w8, w8, #1
    { 0x002140e4U, 0x6b09011fU, 0x14000015U }, // b #0x214138
    // 3. IMLDEC GET_PIC_NOTIFY [x25, #0x28] = 5 (EBC 5/4) for 3840x2560 (8-bit and 10-bit)
    { 0x00237580U, 0x54000341U, 0xd503201fU }, // nop
    { 0x002375bcU, 0x32170fe9U, 0x5281e009U }, // mov w9, #0xf00
    { 0x002375d4U, 0x52821c09U, 0x52814009U }, // mov w9, #0xa00
    // 4. IMLDEC get_aspect_ratio: accept BOTH 2160 (stock @ 0x239900) AND 2560
    { 0x00239908U, 0x52821c08U, 0x52814008U }, // mov w8, #0xa00
    { 0x00239914U, 0x12003e68U, 0x1400001bU }, // b #0x239980
};

static const Patch4 kAvRestore239900[] = {
    { 0x00239900U, 0x7121c2bfU, 0x712802bfU }, // keep stock cmp w21, #0x870 so 3840x2160 works cleanly
};

// Surgical CP Movie YC / VEF scaler patches (excludes CAMLV 0x40afae, JPEG 0xb76e20, and VOUT/HDMI 0xd2d8dc!)
static const Patch4 kCpSurgicalVef2560[] = {
    { 0x013ec2dcU, 0x08700f00U, 0x0a000f00U },
    { 0x013ec2e4U, 0x08700f00U, 0x0a000f00U },
    { 0x001c4898U, 0x6207f44fU, 0x6220f44fU },
    { 0x001c48b0U, 0x6207f44fU, 0x6220f44fU },
    { 0x00631c08U, 0x6007f44fU, 0x6020f44fU },
    { 0x00631c30U, 0x6007f44fU, 0x6020f44fU },
    { 0x00b9f8e8U, 0x08700f00U, 0x0a000f00U },
    { 0x00b9f9c0U, 0x08700f00U, 0x0a000f00U },
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
    { 0x00ff20d4U, 0x008700f0U, 0x00a000f0U },
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

static uint8_t g_saved_hevc_lvl[320];
static int g_saved_hevc_lvl_valid = 0;
static uint16_t g_saved_ddx_bidx[2][2] = {{9, 10}, {9, 10}};
static int g_saved_ddx_valid[2][2] = {{0, 0}, {0, 0}};

static char maps_buf[131072];
static long g_log_fd = -1;
static uint64_t g_appfw_base = 0;
static uint64_t g_mpr_base = 0;
static uint64_t g_uipc_base = 0;
static volatile uint32_t g_last_prio = 0xffffffffU;
static volatile uint32_t g_switch_count = 0;
static volatile uint32_t g_restart_count = 0;
static volatile uint32_t g_force_refresh = 0;
static volatile uint32_t g_pending_sensor_restart = 0;
static volatile int g_last_mode = 1;
static volatile int g_applied_hw_mode = 0;
static volatile uint32_t g_was_usb = 0;
static int g_worker_started = 0;
static const uint32_t kDaemonTokenV5 = 0x4f474d1aU;
extern "C" {
__attribute__((visibility("hidden"))) uint64_t g_orig_3519da4 = 0;
__attribute__((visibility("hidden"))) int call_orig_3519d94(uint32_t dst_id, void *msg, uint32_t size, void *reply, uint32_t reply_size);
__attribute__((visibility("hidden"))) uint64_t g_orig_26a6f5c = 0;
__attribute__((visibility("hidden"))) void call_orig_26a6f4c(void *ctx, int delta);
}

__asm__(
    ".text\n"
    ".align 4\n"
    ".global call_orig_3519d94\n"
    ".hidden call_orig_3519d94\n"
    ".hidden g_orig_3519da4\n"
    "call_orig_3519d94:\n"
    "stp x29, x30, [sp, #-0x60]!\n"
    "mov x29, sp\n"
    "stp x21, x22, [sp, #0x20]\n"
    "add x22, x29, #0x50\n"
    "adrp x16, g_orig_3519da4\n"
    "ldr x16, [x16, :lo12:g_orig_3519da4]\n"
    "br x16\n"
    ".align 4\n"
    ".global call_orig_26a6f4c\n"
    ".hidden call_orig_26a6f4c\n"
    ".hidden g_orig_26a6f5c\n"
    "call_orig_26a6f4c:\n"
    "stp x29, x30, [sp, #-0x60]!\n"
    "mov x2, #1\n"
    "mov x29, sp\n"
    "str x21, [sp, #0x20]\n"
    "adrp x16, g_orig_26a6f5c\n"
    "ldr x16, [x16, :lo12:g_orig_26a6f5c]\n"
    "br x16\n"
);

static inline int is_movie_or_sq_exp(uint32_t exp) {
    if (exp > 28U) return 0;
    const uint32_t mask = 0x0801f000U | 0x103e0000U; // Movie (12..16, 27) | S&Q (17..21, 28)
    return ((mask >> exp) & 1U) != 0U;
}

typedef uint64_t (*MenuLeafCbFn)(void *menu_arg);
typedef uint32_t (*MenuGetSelFn)(void *menu_arg);

static uint64_t hooked_opengate_menu_cb(void *menu_arg) {
    if (!g_appfw_base) return 1;
    MenuGetSelFn get_sel = reinterpret_cast<MenuGetSelFn>(g_appfw_base + 0x1bd7148);
    uint32_t val = get_sel(menu_arg);
    uint32_t norm_val = (val == 0) ? 0U : 1U;

    MenuLeafCbFn orig_cb = reinterpret_cast<MenuLeafCbFn>(g_appfw_base + 0x1c91d74);
    uint64_t ret = orig_cb(menu_arg);

    *(volatile uint32_t *)(g_appfw_base + 0x7f83490) = norm_val;
    *(volatile uint32_t *)(g_appfw_base + 0x7f83498) = norm_val;
    *(volatile uint32_t *)(g_appfw_base + 0x7f834a8) = norm_val;
    *(volatile uint32_t *)(g_appfw_base + 0x8476378) = norm_val;
    uint64_t ptr2 = *(volatile uint64_t *)(g_appfw_base + 0x7f834a0);
    if (ptr2 > 0x10000ULL) {
        *(volatile uint32_t *)ptr2 = norm_val;
    }
    g_force_refresh = 1;
    return ret;
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

static long sc4(long n, long a, long b, long c, long d) {
    return sc6(n, a, b, c, d, 0, 0);
}

static uint64_t hooked_doom_menu_cb(void *menu_arg) {
    if (!g_appfw_base) return 1;
    MenuGetSelFn get_sel = reinterpret_cast<MenuGetSelFn>(g_appfw_base + 0x1bd7148);
    uint32_t val = get_sel(menu_arg);
    uint32_t norm_val = (val == 0U) ? 0U : 1U;
    *(volatile uint32_t *)(g_appfw_base + 0x7f88718) = norm_val;
    *(volatile uint32_t *)(g_appfw_base + 0x7f88720) = norm_val;
    const char *cmd = (val == 0U) ? "v\n" : "h\n";
    long fd = sc4(56, -100, (long)"/tmp/jiritsu/doom_v14_mode", 1 | 64 | 512, 0644);
    if (fd >= 0) {
        sc4(64, fd, (long)cmd, 2, 0);
        sc4(57, fd, 0, 0, 0);
    }
    return 1;
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

static void write_fd_str(long fd, const char *s) {
    if (fd < 0) return;
    long len = 0;
    while (s[len]) ++len;
    sc4(64, fd, (long)s, len, 0);
}

static void write_fd_u32(long fd, uint32_t v) {
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
    write_fd_str(fd, buf + pos);
}

static void write_fd_hex(long fd, uint64_t v, int digits) {
    char buf[20];
    for (int i = 0; i < digits; ++i) {
        buf[i] = "0123456789abcdef"[(v >> ((digits - 1 - i) * 4)) & 0xf];
    }
    buf[digits] = 0;
    write_fd_str(fd, buf);
}

extern "C" int hook_3519d94(uint32_t dst_id, uint8_t *msg, uint32_t size, void *reply, uint32_t reply_size) {
    uint64_t lr = (uint64_t)__builtin_return_address(0);
    if (dst_id == 0x92840747U && msg && g_appfw_base) {
        uint16_t cmd = *(uint16_t *)(msg + 2);
        uint32_t cur_sys = *(volatile uint32_t *)(g_appfw_base + 0x7f721f8);
        int og_on = (g_last_mode == 1);
        int fixed = 0;
        uint16_t orig_dy = 0, orig_dw = 0, orig_dh = 0;
        uint8_t orig_b8 = 0, orig_b11 = 0;
        if (cmd == 0x16e && size == 0x0c) {
            orig_b8 = msg[8];
            orig_b11 = msg[11];
            if (msg[8] != 0 || msg[11] == 0) {
                msg[8] = 0;
                msg[9] = 1;
                msg[10] = 1;
                msg[11] = 1;
                fixed = 1;
            }
        } else if (cmd == 0x14c && size == 0x28) {
            uint8_t out_id = msg[8];
            uint8_t fit_mode = msg[9];
            uint16_t sw = *(uint16_t *)(msg + 0x10);
            uint16_t sh = *(uint16_t *)(msg + 0x12);
            uint16_t dx = *(uint16_t *)(msg + 0x14);
            uint16_t dy = *(uint16_t *)(msg + 0x16);
            uint16_t dw = *(uint16_t *)(msg + 0x18);
            uint16_t dh = *(uint16_t *)(msg + 0x1a);
            orig_dy = dy;
            orig_dw = dw;
            orig_dh = dh;
            if (og_on && cur_sys != 2U) {
                if ((out_id == 1U || out_id == 2U) && dw == 640 && dh > 0 && dh <= 360) {
                    // Explicit manual 3:2 dst rect; ALWAYS preserve Focus Magnifier src crop when fit_mode == 0!
                    if (fit_mode != 0 || sw == 0 || sh == 0) {
                        msg[9] = 0;
                        *(uint16_t *)(msg + 0x0c) = 0;
                        *(uint16_t *)(msg + 0x0e) = 0;
                        *(uint16_t *)(msg + 0x10) = 640;
                        *(uint16_t *)(msg + 0x12) = 480;
                        msg[0x1e] = 0;
                        msg[0x1f] = 0;
                    }
                    // IMPORTANT: Preserve msg[0x1c] (Dev Aspect = 1) and msg[0x20] (M20 = 3)!
                    // CP VDF 0x001e9204 drops EVERY live frame ("VDF : PIN Aspect %d, Dev Aspect %d")
                    // whenever B0A==0 (FocusMag active) && fit_mode==0 && PIN Aspect (1) != msg[0x1c]!
                    *(uint16_t *)(msg + 0x14) = 0;
                    *(uint16_t *)(msg + 0x16) = 26;
                    *(uint16_t *)(msg + 0x18) = 640;
                    *(uint16_t *)(msg + 0x1a) = 428;
                    uint64_t cache_off = (out_id == 1U) ? 0x8a005f8ULL : 0x8a005c8ULL;
                    volatile uint16_t *disp_cache = reinterpret_cast<volatile uint16_t *>(g_appfw_base + cache_off);
                    disp_cache[0] = 0;
                    disp_cache[1] = 26;
                    disp_cache[2] = 640;
                    disp_cache[3] = 428;
                    fixed = 1;
                } else if (out_id == 4U && fit_mode == 0 && dx == 0 && dy == 0 && dw == 640 && dh == 480) {
                    // HDMI Focus Magnifier / manual rect: preserve src crop, msg[0x1c] (Dev Aspect=1), and msg[0x20];
                    // pillarbox dst to true 3:2 (50,0,540,480)
                    if (sw == 0 || sh == 0) {
                        *(uint16_t *)(msg + 0x0c) = 0;
                        *(uint16_t *)(msg + 0x0e) = 0;
                        *(uint16_t *)(msg + 0x10) = 640;
                        *(uint16_t *)(msg + 0x12) = 480;
                    }
                    *(uint16_t *)(msg + 0x14) = 50;
                    *(uint16_t *)(msg + 0x16) = 0;
                    *(uint16_t *)(msg + 0x18) = 540;
                    *(uint16_t *)(msg + 0x1a) = 480;
                    fixed = 1;
                }
            } else if ((out_id == 1U || out_id == 2U) && dw == 640 && dh > 0 && dh < 360) {
                if (fit_mode != 0) {
                    msg[9] = 1;
                }
                *(uint16_t *)(msg + 0x14) = 0;
                *(uint16_t *)(msg + 0x16) = 60;
                *(uint16_t *)(msg + 0x18) = 640;
                *(uint16_t *)(msg + 0x1a) = 360;
                uint64_t cache_off = (out_id == 1U) ? 0x8a005f8ULL : 0x8a005c8ULL;
                volatile uint16_t *disp_cache = reinterpret_cast<volatile uint16_t *>(g_appfw_base + cache_off);
                disp_cache[0] = 0;
                disp_cache[1] = 60;
                disp_cache[2] = 640;
                disp_cache[3] = 360;
                fixed = 1;
            }
            (void)dx;
        } else if (cmd == 0x147 && size == 0x308) {
            // VdfInputCmdSetYuvLayoutOption (24 x 32-byte entries).
            // Critical CP VDF behavior (cpapp-b.bin 0x060fcc8):
            //   ent[1] == 1 -> AUTO-FIT (0x061017e): IGNORES ent[0x0c..0x13] and letterboxes using CP src_aspect (16:9)!
            //   ent[1] == 0 -> MANUAL RECT (0x01e5068): maps full src (0,0,640,480) directly onto dst (dx,dy,dw,dh)!
            // SYSTEM_MODE enum (0x7f721f8): 2=REC_STILL, 3=REC_MOVIE (LiveView+Rec), 4=PLAY_STILL, 5=PLAY_MOVIE_STREAM.
            for (int k = 0; k < 24; ++k) {
                uint8_t *ent = msg + 0x08 + k * 0x20;
                uint8_t out_id = ent[0];
                uint8_t aspect = ent[3];
                uint16_t dw = *(uint16_t *)(ent + 0x10);
                uint16_t dh = *(uint16_t *)(ent + 0x12);
                int is_movie_aspect = (aspect == 1U || aspect >= 5U || ((out_id == 1U || out_id == 2U) && dw == 640 && dh > 0 && dh < 360));
                if (!is_movie_aspect) continue;
                if (og_on && cur_sys != 2U) {
                    if (out_id == 1U || out_id == 2U) {
                        // Rear LCD (800x600 4:3) & EVF (2048x1536 4:3): true unsquished 3:2 (640x428 -> 800x536 / 2048x1368)
                        ent[1] = 0;
                        *(uint16_t *)(ent + 0x04) = 0;
                        *(uint16_t *)(ent + 0x06) = 0;
                        *(uint16_t *)(ent + 0x08) = 640;
                        *(uint16_t *)(ent + 0x0a) = 480;
                        *(uint16_t *)(ent + 0x0c) = 0;
                        *(uint16_t *)(ent + 0x0e) = 26;
                        *(uint16_t *)(ent + 0x10) = 640;
                        *(uint16_t *)(ent + 0x12) = 428;
                        ent[0x14] = 2; // dst_aspect = 3:2
                        ent[0x16] = 0;
                        ent[0x17] = 0;
                        fixed = 1;
                    } else if (out_id == 4U) {
                        // HDMI (16:9 external monitor): true unsquished 3:2 pillarboxed (540/640 * 16/9 = 1.500)
                        ent[1] = 0;
                        *(uint16_t *)(ent + 0x04) = 0;
                        *(uint16_t *)(ent + 0x06) = 0;
                        *(uint16_t *)(ent + 0x08) = 640;
                        *(uint16_t *)(ent + 0x0a) = 480;
                        *(uint16_t *)(ent + 0x0c) = 50;
                        *(uint16_t *)(ent + 0x0e) = 0;
                        *(uint16_t *)(ent + 0x10) = 540;
                        *(uint16_t *)(ent + 0x12) = 480;
                        ent[0x14] = 2; // dst_aspect = 3:2
                        ent[0x16] = 0;
                        ent[0x17] = 0;
                        fixed = 1;
                    }
                } else {
                    if (out_id == 1U || out_id == 2U) {
                        if (ent[1] != 1 || dh != 360) {
                            ent[1] = 1;
                            *(uint16_t *)(ent + 0x0c) = 0;
                            *(uint16_t *)(ent + 0x0e) = 60;
                            *(uint16_t *)(ent + 0x10) = 640;
                            *(uint16_t *)(ent + 0x12) = 360;
                            ent[0x14] = aspect;
                            fixed = 1;
                        }
                    } else if (out_id == 4U && aspect == 1U) {
                        if (ent[1] != 1 || dw != 640 || dh != 480) {
                            ent[1] = 1;
                            *(uint16_t *)(ent + 0x0c) = 0;
                            *(uint16_t *)(ent + 0x0e) = 0;
                            *(uint16_t *)(ent + 0x10) = 640;
                            *(uint16_t *)(ent + 0x12) = 480;
                            ent[0x14] = aspect;
                            fixed = 1;
                        }
                    }
                }
            }
        }
        int call_rc = call_orig_3519d94(dst_id, msg, size, reply, reply_size);
        if (cmd == 0x134 || cmd == 0x15f || cmd == 0x147 || cmd == 0x152 || (cmd == 0x16e && fixed) || (cmd == 0x14c && (msg[8] != 7 || fixed))) {
            long vfd = sc4(56, -100, (long)"/tmp/jiritsu/vdf-log.txt", 1 | 64 | 1024, 0644);
            if (vfd >= 0) {
                write_fd_str(vfd, "SYS=");
                write_fd_u32(vfd, cur_sys);
                write_fd_str(vfd, " M=");
                write_fd_u32(vfd, (uint32_t)g_last_mode);
                write_fd_str(vfd, " CMD=0x");
                write_fd_hex(vfd, cmd, 4);
                write_fd_str(vfd, " LR=0x");
                write_fd_hex(vfd, lr - g_appfw_base, 7);
                if (cmd == 0x134 && size >= 13) {
                    write_fd_str(vfd, " EI_GAIN=");
                    write_fd_u32(vfd, *(uint32_t *)(msg + 8));
                    write_fd_str(vfd, " ON=");
                    write_fd_u32(vfd, (uint32_t)msg[12]);
                } else if (cmd == 0x15f && size >= 12) {
                    write_fd_str(vfd, " ISO_15F=");
                    write_fd_u32(vfd, *(uint32_t *)(msg + 8));
                } else if (cmd == 0x16e && size == 0x0c) {
                    write_fd_str(vfd, " DESQ=(");
                    write_fd_u32(vfd, orig_b8);
                    write_fd_str(vfd, ",");
                    write_fd_u32(vfd, orig_b11);
                    write_fd_str(vfd, ") FIXED_DESQ_OFF");
                } else if (cmd == 0x152 && size == 0x10) {
                    write_fd_str(vfd, " PK=(");
                    write_fd_u32(vfd, msg[8]);
                    write_fd_str(vfd, ",");
                    write_fd_u32(vfd, msg[9]);
                    write_fd_str(vfd, ",");
                    write_fd_u32(vfd, msg[10]);
                    write_fd_str(vfd, ",");
                    write_fd_u32(vfd, msg[11]);
                    write_fd_str(vfd, ")");
                } else if (cmd == 0x14c && size == 0x28) {
                    write_fd_str(vfd, " OUT=");
                    write_fd_u32(vfd, msg[8]);
                    write_fd_str(vfd, " F=");
                    write_fd_u32(vfd, msg[9]);
                    write_fd_str(vfd, " B0A=");
                    write_fd_u32(vfd, msg[0x0a]);
                    write_fd_str(vfd, " A=");
                    write_fd_u32(vfd, msg[0x1c]);
                    write_fd_str(vfd, " M20=");
                    write_fd_u32(vfd, msg[0x20]);
                    write_fd_str(vfd, " SRC=(");
                    write_fd_u32(vfd, *(uint16_t *)(msg + 0x0c));
                    write_fd_str(vfd, ",");
                    write_fd_u32(vfd, *(uint16_t *)(msg + 0x0e));
                    write_fd_str(vfd, ",");
                    write_fd_u32(vfd, *(uint16_t *)(msg + 0x10));
                    write_fd_str(vfd, ",");
                    write_fd_u32(vfd, *(uint16_t *)(msg + 0x12));
                    write_fd_str(vfd, ") DST=(");
                    write_fd_u32(vfd, *(uint16_t *)(msg + 0x14));
                    write_fd_str(vfd, ",");
                    write_fd_u32(vfd, *(uint16_t *)(msg + 0x16));
                    write_fd_str(vfd, ",");
                    write_fd_u32(vfd, *(uint16_t *)(msg + 0x18));
                    write_fd_str(vfd, ",");
                    write_fd_u32(vfd, *(uint16_t *)(msg + 0x1a));
                    write_fd_str(vfd, ")");
                    if (fixed) {
                        write_fd_str(vfd, " FIXED_FROM=(");
                        write_fd_u32(vfd, orig_dy);
                        write_fd_str(vfd, ",");
                        write_fd_u32(vfd, orig_dw);
                        write_fd_str(vfd, ",");
                        write_fd_u32(vfd, orig_dh);
                        write_fd_str(vfd, ")");
                    }
                } else if (cmd == 0x147 && size == 0x308) {
                    uint8_t *e1 = msg + 0x08 + 1 * 0x20; // OUT=1 aspect=1
                    write_fd_str(vfd, " L1=(F=");
                    write_fd_u32(vfd, e1[1]);
                    write_fd_str(vfd, ",A=");
                    write_fd_u32(vfd, e1[0x14]);
                    write_fd_str(vfd, ",Y=");
                    write_fd_u32(vfd, *(uint16_t *)(e1 + 0x0e));
                    write_fd_str(vfd, ",H=");
                    write_fd_u32(vfd, *(uint16_t *)(e1 + 0x12));
                    write_fd_str(vfd, ")");
                    if (fixed) write_fd_str(vfd, " FIXED_147");
                }
                write_fd_str(vfd, " RC=");
                write_fd_hex(vfd, (uint32_t)call_rc, 8);
                if (reply && reply_size >= 12) {
                    write_fd_str(vfd, " ST=");
                    write_fd_hex(vfd, *(uint32_t *)((uint8_t *)reply + 8), 8);
                }
                write_fd_str(vfd, "\n");
                sc4(57, vfd, 0, 0, 0);
            }
        }
        return call_rc;
    }
    return call_orig_3519d94(dst_id, msg, size, reply, reply_size);
}

// Hardware Shutter Angle CP IPC encoder (PID 0x401c, exact port of FX3 7.02 @ 0x2e1cad8).
// A7S III cpapp-b.bin @ 0x007a6c3e already implements the full mode/cmd/numer/denom/frame
// handler, while appFw.so @ 0x355bda0 stripped the ShutterMode==1 (Angle) branch.
// Note: A7S III sequence_set_shut_drive_for_shutter_angle::run (0x34a46fc) packs
// ShutterMode (u16) at in+0x0c and Angle (u16) at in+0x0e.
extern "C" void hook_355bda0(const uint8_t *in_b, uint32_t *out) {
    if (!g_appfw_base || !in_b) {
        if (out) *out = 0U;
        return;
    }
    uint8_t msg[0x18];
    for (int i = 0; i < 0x18; ++i) msg[i] = 0;
    msg[0] = 5;
    *(uint16_t *)(msg + 2) = 0x401cU;
    *(uint32_t *)(msg + 4) = *(const uint32_t *)(in_b + 0x00);

    uint16_t shut_mode = *(const uint16_t *)(in_b + 0x0c); // 0 = Speed, 1 = Angle
    uint16_t angle     = *(const uint16_t *)(in_b + 0x0e); // 0..9 = 64F..2F, 10..28 = 360.0..5.6
    uint32_t cur_exp   = *(const uint32_t *)(in_b + 0x10);
    uint32_t file_fmt  = *(const uint32_t *)(in_b + 0x14);
    uint32_t fps_idx   = *(const uint32_t *)(in_b + 0x18);
    uint32_t sq_rec    = *(const uint32_t *)(in_b + 0x1c);
    uint32_t sq_nrm    = *(const uint32_t *)(in_b + 0x20);
    uint32_t sq_dci    = *(const uint32_t *)(in_b + 0x24);

    if (shut_mode == 0U) {
        uint32_t drv = *(const uint32_t *)(in_b + 0x04);
        uint32_t typ = *(const uint32_t *)(in_b + 0x08);
        msg[8] = (uint8_t)(drv & 0xffU);
        msg[9] = (typ == 1U) ? 1U : 0U;
    } else if (shut_mode == 1U) {
        if (angle <= 9U) {
            static const uint8_t kFrames[10] = {64, 32, 16, 8, 7, 6, 5, 4, 3, 2};
            msg[10] = 1; // mode = Angle
            msg[11] = 2; // cmd = frame-accumulation (64F..2F)
            *(uint32_t *)(msg + 20) = (uint32_t)kFrames[angle];
        } else if (angle <= 28U) {
            static const uint32_t kAngleX1000[19] = {
                360000, 300000, 270000, 240000, 216000, 210000, 180000, 172800,
                150000, 144000, 120000,  90000,  86400,  72000,  45000,  30000,
                 22500,  11250,   5600
            };
            static const uint16_t kFpsX100[8] = {
                5994, 5000, 2997, 2500, 2398, 11988, 10000, 2400
            };
            static const uint8_t kSqDci[7]  = {48, 24, 12, 6, 3, 2, 1};
            static const uint8_t kSqNrm[16] = {240, 200, 120, 100, 60, 50, 30, 25, 15, 12, 8, 6, 4, 3, 2, 1};

            uint32_t ang_x1000 = kAngleX1000[angle - 10U];
            int is_sq = (cur_exp <= 28U) && (((0x103e0000U >> cur_exp) & 1U) != 0U);
            msg[10] = 1; // mode = Angle
            msg[11] = 1; // cmd = absolute numer/denom
            if (!is_sq) {
                uint64_t fps_x100 = (fps_idx <= 7U) ? (uint64_t)kFpsX100[fps_idx] : 2500ULL;
                uint64_t denom = (fps_x100 * 360000ULL) / (uint64_t)ang_x1000;
                if (denom > 0xffffffffULL) denom = 0xffffffffULL;
                *(uint32_t *)(msg + 12) = 100U;
                *(uint32_t *)(msg + 16) = (uint32_t)denom;
            } else {
                uint32_t sq_fps = 1U;
                if (file_fmt == 0x0bU && sq_rec == 7U) {
                    sq_fps = (sq_dci <= 6U) ? (uint32_t)kSqDci[sq_dci] : 1U;
                } else {
                    sq_fps = (sq_nrm <= 15U) ? (uint32_t)kSqNrm[sq_nrm] : 1U;
                }
                if (sq_fps == 0U) sq_fps = 1U;
                *(uint32_t *)(msg + 12) = ang_x1000;
                *(uint32_t *)(msg + 16) = sq_fps * 360000U;
            }
        }
    }

    reinterpret_cast<void (*)(uint16_t, const void *, uint64_t)>(g_appfw_base + 0x3691938)(
        0x401cU, msg + 8, 0x10ULL);
    reinterpret_cast<void (*)(uint32_t, const void *, uint64_t, uint32_t, uint64_t, uint64_t)>(g_appfw_base + 0x355277c)(
        0x90030001U, msg, 0x18ULL, 0U, 0ULL, 0ULL);
    if (out) *out = 0U;

    long vfd = sc4(56, -100, (long)"/tmp/jiritsu/vdf-log.txt", 1 | 64 | 1024, 0644);
    if (vfd >= 0) {
        write_fd_str(vfd, "SHUT_401C MODE=");
        write_fd_u32(vfd, (uint32_t)shut_mode);
        write_fd_str(vfd, " ANG=");
        write_fd_u32(vfd, (uint32_t)angle);
        write_fd_str(vfd, " FPS=");
        write_fd_u32(vfd, fps_idx);
        write_fd_str(vfd, " CMD=");
        write_fd_u32(vfd, (uint32_t)msg[11]);
        write_fd_str(vfd, " ABS=");
        write_fd_u32(vfd, *(uint32_t *)(msg + 12));
        write_fd_str(vfd, "/");
        write_fd_u32(vfd, *(uint32_t *)(msg + 16));
        write_fd_str(vfd, " F=");
        write_fd_u32(vfd, *(uint32_t *)(msg + 20));
        write_fd_str(vfd, "\n");
        sc4(57, vfd, 0, 0, 0);
    }
}

// Shutter Angle dial stepper hook (appFw + 0x26a6f4c):
// When delta == 0 (apply from menu or FPS sync), runs original 0x26a6f4c -> sequence 0x63 -> hook_355bda0.
// When delta != 0 (physical shutter wheel), steps Angle cleanly via native Angle setter 0x26a70ac.
extern "C" void hook_26a6f4c(void *ctx, int delta) {
    if (!g_appfw_base || delta == 0) {
        call_orig_26a6f4c(ctx, delta);
        if (g_appfw_base) {
            reinterpret_cast<void (*)(void)>(g_appfw_base + 0x24ef294)();
        }
        return;
    }
    uint32_t cur = *(volatile uint32_t *)(g_appfw_base + 0x7f84f98);
    if (cur > 28U) cur = 16U;
    int lo = (cur >= 10U) ? 10 : 0;
    int next = (int)cur + delta;
    if (next < lo) next = lo;
    if (next > 28) next = 28;
    if ((uint32_t)next != cur) {
        reinterpret_cast<void (*)(void *, uint32_t)>(g_appfw_base + 0x26a70ac)(ctx, (uint32_t)next);
    }
}

static void trigger_shutter_angle_apply(void) {
    if (!g_appfw_base) return;
    uint64_t ctx = 0;
    reinterpret_cast<void (*)(uint64_t *)>(g_appfw_base + 0x2540a08)(&ctx);
    call_orig_26a6f4c(&ctx, 0);
    reinterpret_cast<void (*)(void)>(g_appfw_base + 0x24ef294)();
    reinterpret_cast<void (*)(uint64_t *)>(g_appfw_base + 0x2540a18)(&ctx);
}

// Exposure Index (Cine EI / Cine EI Quick) physical ISO wheel steppers.
// Stock A7S III strips dataflow step nodes 0x4046..0x404b, while the native EI setters
// (0x26cb460, 0x26c972c, 0x26c9aa0) and VDF EI gain pipeline (CMD 0x0134/0x015f) are intact.
static void step_ei_value(void *ctx, int delta, uint32_t val_off, uint32_t setter_off, int lo, int hi) {
    if (!g_appfw_base || delta == 0) return;
    int cur = (int)*(volatile uint32_t *)(g_appfw_base + val_off);
    if (cur < lo) cur = lo;
    if (cur > hi) cur = hi;
    int next = cur + delta;
    if (next < lo) next = lo;
    if (next > hi) next = hi;
    if (next != cur) {
        reinterpret_cast<void (*)(void *, uint32_t)>(g_appfw_base + setter_off)(ctx, (uint32_t)next);
        reinterpret_cast<void (*)(void)>(g_appfw_base + 0x24ef294)();
    }
}

extern "C" void hook_ei_auto_step(void *ctx, int delta) {
    step_ei_value(ctx, delta, 0x7f85160U, 0x26cb460U, 1, 25); // Cine EI Quick: 200..51200 EI
}

extern "C" void hook_ei_low_step(void *ctx, int delta) {
    step_ei_value(ctx, delta, 0x7f85198U, 0x26c972cU, 1, 13); // Base ISO 800: 200..3200 EI
}

extern "C" void hook_ei_high_step(void *ctx, int delta) {
    step_ei_value(ctx, delta, 0x7f851d0U, 0x26c9aa0U, 13, 25); // Base ISO 12800: 3200..51200 EI
}

static void install_hook16(uint64_t target_va, void *hook_fn) {
    sc4(226, (long)(target_va & ~0xfffULL), 4096, 7, 0);
    *(volatile uint32_t *)(target_va + 0) = 0x58000050U; // ldr x16, #8
    *(volatile uint32_t *)(target_va + 4) = 0xd61f0200U; // br x16
    *(volatile uint64_t *)(target_va + 8) = (uint64_t)hook_fn;
    for (int i = 0; i < 16; i += 4) {
        flush_icache_addr(target_va + i);
    }
}

static void restore_stock_4b72818(void) {
    if (!g_appfw_base) return;
    uint8_t stock[0x4e8];
    uint8_t stock_fn[0x6c];
    long fd = sc4(56, -100, (long)"/usr/lib/appFw.so", 0, 0);
    if (fd >= 0) {
        long got = sc4(67, fd, (long)stock, 0x4e8, 0x4b72818);
        long got_fn = sc4(67, fd, (long)stock_fn, 0x6c, 0x34f991c);
        sc4(57, fd, 0, 0, 0);
        if (got == 0x4e8) {
            uint64_t tbl_va = g_appfw_base + 0x4b72818;
            sc4(226, (long)(tbl_va & ~0xfffULL), 8192, 3, 0);
            for (uint32_t i = 0; i < 0x4e8; ++i) {
                *(volatile uint8_t *)(tbl_va + i) = stock[i];
            }
        }
        if (got_fn == 0x6c) {
            uint64_t fn_va = g_appfw_base + 0x34f991c;
            sc4(226, (long)(fn_va & ~0xfffULL), 4096, 7, 0);
            for (uint32_t i = 0; i < 0x6c; i += 4) {
                *(volatile uint32_t *)(fn_va + i) = *(uint32_t *)(stock_fn + i);
                flush_icache_addr(fn_va + i);
            }
        }
    }

    uint64_t p1 = g_appfw_base + 0x595e2e8;
    uint64_t p2 = g_appfw_base + 0x597a6a0;
    sc4(226, (long)(p1 & ~0xfffULL), 4096, 3, 0);
    sc4(226, (long)(p2 & ~0xfffULL), 4096, 3, 0);
    *(volatile uint64_t *)p1 = g_appfw_base + 0x4b72818;
    *(volatile uint64_t *)p2 = g_appfw_base + 0x4b72818;

    // Install 16-byte hook on 0x3519d94 (VDF IPC sender)
    g_orig_3519da4 = g_appfw_base + 0x3519da4;
    install_hook16(g_appfw_base + 0x3519d94, (void *)&hook_3519d94);

    // Install 16-byte hook on 0x355bda0 (PID 0x401c PID_SET_SHUT_DRIVE for hardware Shutter Angle)
    install_hook16(g_appfw_base + 0x355bda0, (void *)&hook_355bda0);

    // Install 16-byte hook on 0x26a6f4c (Shutter Angle wheel stepper -> native 0x26a70ac)
    g_orig_26a6f5c = g_appfw_base + 0x26a6f5c;
    install_hook16(g_appfw_base + 0x26a6f4c, (void *)&hook_26a6f4c);

    // Install 16-byte hooks on the 6 Cine EI / Cine EI Quick ISO-wheel steppers (replacing stripped 0x4046..0x404b)
    install_hook16(g_appfw_base + 0x26cb554, (void *)&hook_ei_auto_step);
    install_hook16(g_appfw_base + 0x26cb644, (void *)&hook_ei_auto_step);
    install_hook16(g_appfw_base + 0x26c9820, (void *)&hook_ei_low_step);
    install_hook16(g_appfw_base + 0x26c9910, (void *)&hook_ei_low_step);
    install_hook16(g_appfw_base + 0x26c9b94, (void *)&hook_ei_high_step);
    install_hook16(g_appfw_base + 0x26c9c84, (void *)&hook_ei_high_step);
}

static void patch_viewport_table(int mode) {
    if (!g_appfw_base) return;
    uint64_t tbl_va = g_appfw_base + 0x4b72818;
    sc4(226, (long)(tbl_va & ~0xfffULL), 8192, 3, 0);
    uint8_t vp_slot = (mode == 1) ? 0x19 : 0x1a;
    // Patch Row 2 (Movie / S&Q Live View) so appFw OSD/AF/grid coordinates match 3:2 (0,26,640,428) / HDMI (50,0,540,480).
    volatile uint8_t *row2 = reinterpret_cast<volatile uint8_t *>(tbl_va + 2 * 44);
    row2[0x05] = vp_slot;
    row2[0x09] = vp_slot;
    row2[0x0b] = vp_slot;
    row2[0x0c] = vp_slot;
    row2[0x0d] = vp_slot;
    row2[0x0e] = vp_slot;
    row2[0x0f] = vp_slot;
    row2[0x10] = (mode == 1) ? 0x19 : 0x36;
    row2[0x13] = vp_slot;
    row2[0x17] = vp_slot;
    row2[0x19] = vp_slot;
    row2[0x1a] = vp_slot;
    row2[0x1b] = vp_slot;
    row2[0x1c] = vp_slot;
    row2[0x1d] = vp_slot;
    row2[0x1e] = (mode == 1) ? 0x19 : 0x36;
    row2[0x21] = (mode == 1) ? 0x1b : 0x16; // HDMI OUT=4 aspect=1: 0x1b=(50,0,540,480 3:2) vs 0x16=(0,0,640,480 16:9)

    // Patch Row 7 (Focus Magnifier 1.0x box & 4.0x zoom) so OSD/minimap/YUV rects match 3:2 (0x19 LCD/EVF, 0x1b HDMI).
    volatile uint8_t *row7 = reinterpret_cast<volatile uint8_t *>(tbl_va + 7 * 44);
    row7[0x05] = vp_slot;
    row7[0x09] = (mode == 1) ? 0x19 : 0x22;
    row7[0x0c] = (mode == 1) ? 0x19 : 0x2d;
    row7[0x0d] = (mode == 1) ? 0x19 : 0x2d;
    row7[0x13] = vp_slot;
    row7[0x17] = vp_slot;
    row7[0x1a] = (mode == 1) ? 0x19 : 0x2d;
    row7[0x1b] = (mode == 1) ? 0x19 : 0x2d;
    row7[0x21] = (mode == 1) ? 0x1b : 0x16;
    row7[0x25] = (mode == 1) ? 0x1b : 0x16;
    row7[0x28] = (mode == 1) ? 0x1b : 0x2f;
    row7[0x29] = (mode == 1) ? 0x1b : 0x16;
}

static void refresh_yuv_viewport(void) {
    if (!g_appfw_base) return;
    uint32_t cur_sys = *(volatile uint32_t *)(g_appfw_base + 0x7f721f8);
    uint32_t cur_opt = *(volatile uint32_t *)(g_appfw_base + 0x7f72268);
    // Push updated CMD=0x0147 table to CP VDF whenever not in Still Photo (2) or USB Mass Storage (4, opt=2).
    // CP DevCmdSetYuvLayoutOption sets dirty flag (0x617912) so VDF applies it on the very next VSync.
    if (cur_sys == 2U || (cur_sys == 4U && cur_opt == 2U)) {
        return;
    }
    struct {
        uint32_t mode_row;
        uint32_t sub_idx;
        uint32_t aspect_idx;
        uint32_t pad0;
        uint64_t rect_tbl;
        uint64_t layout_tbl;
    } in_arg;
    in_arg.mode_row = (cur_sys == 3U) ? 2U : 0U;
    in_arg.sub_idx = 0U;
    in_arg.aspect_idx = 1U;
    in_arg.pad0 = 0U;
    in_arg.rect_tbl = *(volatile uint64_t *)(g_appfw_base + 0x59863c8);
    in_arg.layout_tbl = g_appfw_base + 0x4b72818;
    uint64_t out_arg = 0;
    reinterpret_cast<void (*)(void *, void *)>(g_appfw_base + 0x34fa610)(&in_arg, &out_arg);
}

static void patch_mp_group(const char *label, uint64_t base, const Patch4 *patches, uint32_t count, int enable_og) {
    if (!base) return;
    uint32_t n_og = 0, n_st = 0, n_oth = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t target_va = base + patches[i].addr;
        uint64_t page = target_va & ~0xfffULL;
        sc4(226, (long)page, 8192, 7, 0);
        volatile uint32_t *ptr = (volatile uint32_t *)target_va;
        uint32_t cur = *ptr;
        if (enable_og && cur == patches[i].stock) {
            *ptr = patches[i].og;
            flush_icache_addr(target_va);
        } else if (!enable_og && cur == patches[i].og) {
            *ptr = patches[i].stock;
            flush_icache_addr(target_va);
        }
        cur = *ptr;
        if (cur == patches[i].og) ++n_og;
        else if (cur == patches[i].stock) ++n_st;
        else ++n_oth;
    }
    write_str(label);
    write_str(": OG=");
    write_u32(n_og);
    write_str(" STOCK=");
    write_u32(n_st);
    write_str(" OTH=");
    write_u32(n_oth);
    write_str("\n");
}

static void patch_av_group(const char *label, uint8_t *av_ptr, const Patch4 *patches, uint32_t count, int enable_og) {
    uint32_t n_og = 0, n_st = 0, n_oth = 0;
    for (uint32_t i = 0; i < count; ++i) {
        volatile uint32_t *p = (volatile uint32_t *)(av_ptr + patches[i].addr);
        uint32_t cur = *p;
        if (enable_og && cur == patches[i].stock) {
            *p = patches[i].og;
            __asm__ volatile("dsb sy" ::: "memory");
        } else if (!enable_og && cur == patches[i].og) {
            *p = patches[i].stock;
            __asm__ volatile("dsb sy" ::: "memory");
        }
        cur = *p;
        if (cur == patches[i].og) ++n_og;
        else if (cur == patches[i].stock) ++n_st;
        else ++n_oth;
    }
    write_str(label);
    write_str(": OG=");
    write_u32(n_og);
    write_str(" STOCK=");
    write_u32(n_st);
    write_str(" OTH=");
    write_u32(n_oth);
    write_str("\n");
}

static void apply_all_og_patches(int mode, const char *reason, int do_refresh) {
    int en = (mode == 1) ? 1 : 0;
    g_log_fd = sc4(56, -100, (long)"/tmp/jiritsu/og-menu-daemon.txt", 1 | 64 | 1024, 0644);
    write_str("REASON=");
    write_str(reason);
    write_str(" MODE=");
    write_str(en ? "OG_3840x2560" : "STOCK_16x9");
    write_str(" SWITCH_COUNT=");
    write_u32(g_switch_count);
    write_str("\n");

    // 0. Update 0x4b72818 viewport layout table (0x19 = 640x428 3:2 vs 0x1a = 640x360 16:9)
    patch_viewport_table(mode);

    // 1. Patch MP (appFw.so + libmpr.so) for native 3840x2560 (1:1 SAR, HEVC Level 6.0)
    patch_mp_group("MP_appFw", g_appfw_base, kAppFw2560Patches, sizeof(kAppFw2560Patches) / sizeof(Patch4), en);
    patch_mp_group("MP_appFw_ex", g_appfw_base, kAppFwExtra2560, sizeof(kAppFwExtra2560) / sizeof(Patch4), en);
    // Keep tkhd stock (lsl #16) so MP4 uses native coded height (2560 when OG, 2160 when Stock)
    patch_mp_group("MP_tkhd", g_mpr_base, kMprTkhd2560Patches, sizeof(kMprTkhd2560Patches) / sizeof(Patch4), 0);
    patch_mp_group("MP_mpr", g_mpr_base, kMpr2560Patches, sizeof(kMpr2560Patches) / sizeof(Patch4), en);
    patch_mp_group("MP_mpr_ex", g_mpr_base, kMprExtra2560, sizeof(kMprExtra2560) / sizeof(Patch4), en);
    patch_mp_group("MP_hevc_lvl6", g_mpr_base, kMprHevcLvl6, sizeof(kMprHevcLvl6) / sizeof(Patch4), en);

    // 2. Patch AV (av-cam.bin Imola + VDT) and CP RAM (3:2 sensor + surgical Movie YC/VEF 2560)
    long mem_fd = sc4(56, -100, (long)"/dev/mem", 0x101002, 0);
    if (mem_fd >= 0) {
        const uint64_t av_phys_base = 0x401000000ULL;
        const uint32_t av_map_size = 0x00400000U;
        long av_map = sc6(222, 0, av_map_size, 3, 1, mem_fd, av_phys_base);
        if (av_map > 0 && (uint64_t)av_map < 0xfffffffffffff000ULL) {
            uint8_t *av_ptr = (uint8_t *)av_map;
            uint32_t vdt_w = *(volatile uint32_t *)(av_ptr + 0x320960);
            uint32_t vdt_h = *(volatile uint32_t *)(av_ptr + 0x320964);
            if (vdt_w == 3840U && (vdt_h == 2160U || vdt_h == 2560U)) {
                // Keep VUI SAR 1:1 stock (native square pixels!)
                patch_av_group("AV_VUI_1x1", av_ptr, kAvVuiSar27x32Patches, sizeof(kAvVuiSar27x32Patches) / sizeof(Patch4), 0);
                patch_av_group("AV_IMOLA_TXT", av_ptr, kAvImolaText2560, sizeof(kAvImolaText2560) / sizeof(Patch4), en);
                // Always keep 0x239900 stock (accepts 2160) and enable 23-word EBC 1.25 Bpp 3840x2560 playback patch
                patch_av_group("AV_PB_2160", av_ptr, kAvRestore239900, sizeof(kAvRestore239900) / sizeof(Patch4), 0);
                patch_av_group("AV_PB_2560", av_ptr, kAvPlayback2560, sizeof(kAvPlayback2560) / sizeof(Patch4), 1);
                patch_av_group("AV_VDT_TXT", av_ptr, kAvText2560Patches, sizeof(kAvText2560Patches) / sizeof(Patch4), en);

                // Imola CreateCh/EncStart .rodata 3840x2160 <-> 3840x2560 pairs
                uint32_t ro32_og = 0, ro16_og = 0;
                for (uint32_t off = 0x38d968U; off <= 0x3e9000U - 8U; off += 4U) {
                    volatile uint32_t *wp = (volatile uint32_t *)(av_ptr + off);
                    volatile uint32_t *hp = (volatile uint32_t *)(av_ptr + off + 4U);
                    if (*wp == 3840U) {
                        uint32_t h = *hp;
                        if (en && h == 2160U) *hp = 2560U;
                        else if (!en && h == 2560U) *hp = 2160U;
                        if (*hp == 2560U) ++ro32_og;
                    }
                }
                for (uint32_t off = 0x38d968U; off <= 0x3e9000U - 4U; off += 2U) {
                    volatile uint16_t *wp = (volatile uint16_t *)(av_ptr + off);
                    volatile uint16_t *hp = (volatile uint16_t *)(av_ptr + off + 2U);
                    if (*wp == 3840U) {
                        uint16_t h = *hp;
                        if (en && h == 2160U) *hp = 2560U;
                        else if (!en && h == 2560U) *hp = 2160U;
                        if (*hp == 2560U) ++ro16_og;
                    }
                }

                // HEVC EncStart Vsize_Core0 (1088 <-> 1280) and Level (-> 0xb4 Level 6.0, saved for clean restore)
                uint32_t hevc_v0_og = 0, hevc_lvl_og = 0, idx = 0;
                for (uint32_t off = 0x3bd2f8U; off < 0x3d0ef8U && idx < 320U; off += 0x100U, ++idx) {
                    if (!g_saved_hevc_lvl_valid) {
                        g_saved_hevc_lvl[idx] = *(volatile uint8_t *)(av_ptr + off + 0x9eU);
                    }
                    uint16_t ew = *(volatile uint16_t *)(av_ptr + off + 4U);
                    uint16_t eh = *(volatile uint16_t *)(av_ptr + off + 6U);
                    if (ew == 3840U && (eh == 2160U || eh == 2560U)) {
                        volatile uint16_t *v0p = (volatile uint16_t *)(av_ptr + off + 0x56U);
                        if (en && *v0p == 1088U) *v0p = 1280U;
                        else if (!en && *v0p == 1280U) *v0p = 1088U;
                        if (*v0p == 1280U) ++hevc_v0_og;

                        volatile uint8_t *lvp = (volatile uint8_t *)(av_ptr + off + 0x9eU);
                        uint8_t orig_lv = g_saved_hevc_lvl[idx];
                        if (orig_lv == 0x96U || orig_lv == 0x99U || orig_lv == 0x9cU) {
                            *lvp = en ? 0xb4U : orig_lv;
                        }
                        if (*lvp == 0xb4U) ++hevc_lvl_og;
                    }
                }
                g_saved_hevc_lvl_valid = 1;

                // AVC EncStart Vsize_Core0 (1072 <-> 1280); AVC levels remain stock for in-camera playback
                uint32_t avc_v0_og = 0;
                for (uint32_t off = 0x3a5cf8U; off < 0x3bd1f8U; off += 0x100U) {
                    uint16_t ew = *(volatile uint16_t *)(av_ptr + off + 4U);
                    uint16_t eh = *(volatile uint16_t *)(av_ptr + off + 6U);
                    if (ew == 3840U && (eh == 2160U || eh == 2560U)) {
                        volatile uint16_t *v0p = (volatile uint16_t *)(av_ptr + off + 0x56U);
                        if (en && *v0p == 1072U) *v0p = 1280U;
                        else if (!en && *v0p == 1280U) *v0p = 1072U;
                        if (*v0p == 1280U) ++avc_v0_og;
                    }
                }

                // VDT RC_RESIZE input height table
                volatile uint32_t *vdt_ro_h = (volatile uint32_t *)(av_ptr + 0x320964U);
                if (en && *vdt_ro_h == 2160U) *vdt_ro_h = 2560U;
                else if (!en && *vdt_ro_h == 2560U) *vdt_ro_h = 2160U;
                __asm__ volatile("dsb sy" ::: "memory");

                write_str("AV_RO: 32=");
                write_u32(ro32_og);
                write_str(" 16=");
                write_u32(ro16_og);
                write_str(" HV0=");
                write_u32(hevc_v0_og);
                write_str(" HLV=");
                write_u32(hevc_lvl_og);
                write_str(" AV0=");
                write_u32(avc_v0_og);
                write_str(" VDT=");
                write_u32(*vdt_ro_h);
                write_str("\n");
            }
            sc4(215, av_map, av_map_size, 0, 0);
        }

        if (g_uipc_base) {
            if (*(volatile uint64_t *)(g_uipc_base + 0x1b560 + 8) == 0) {
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
                    uint32_t num_patches = sizeof(kCpPatches) / sizeof(kCpPatches[0]);
                    uint32_t cp_og = 0, cp_st = 0, cp_oth = 0;

                    for (uint32_t idx = 0; idx < num_patches; ++idx) {
                        const CpPatch &p = kCpPatches[idx];
                        uint32_t aligned_start = p.addr & ~0x3fU;
                        uint32_t aligned_end = (p.addr + p.len + 0x3fU) & ~0x3fU;
                        uint32_t aligned_size = aligned_end - aligned_start;
                        uint32_t offset = p.addr - aligned_start;

                        for (uint32_t i = 0; i < aligned_size; ++i) buf[i] = 0;
                        if (my_sen_dma_transfer(0, aligned_start, dmm_phys, aligned_size) != 0) {
                            ++cp_oth;
                            continue;
                        }
                        const uint8_t *target = en ? p.og : p.stock;
                        for (uint32_t i = 0; i < p.len; ++i) {
                            buf[offset + i] = target[i];
                        }
                        __asm__ volatile("dsb sy" ::: "memory");
                        if (my_sen_dma_transfer(1, dmm_phys, aligned_start, aligned_size) == 0) {
                            if (mem_eq(buf + offset, p.og, p.len)) ++cp_og;
                            else if (mem_eq(buf + offset, p.stock, p.len)) ++cp_st;
                            else ++cp_oth;
                        } else {
                            ++cp_oth;
                        }
                    }

                    uint32_t num_vef = sizeof(kCpSurgicalVef2560) / sizeof(Patch4);
                    uint32_t vef_og = 0, vef_st = 0, vef_oth = 0;
                    for (uint32_t idx = 0; idx < num_vef; ++idx) {
                        const Patch4 &p = kCpSurgicalVef2560[idx];
                        uint32_t aligned_start = p.addr & ~0x3fU;
                        uint32_t aligned_end = (p.addr + 4U + 0x3fU) & ~0x3fU;
                        uint32_t aligned_size = aligned_end - aligned_start;
                        uint32_t offset = p.addr - aligned_start;

                        for (uint32_t i = 0; i < aligned_size; ++i) buf[i] = 0;
                        if (my_sen_dma_transfer(0, aligned_start, dmm_phys, aligned_size) != 0) {
                            ++vef_oth;
                            continue;
                        }
                        uint32_t cur = *(uint32_t *)(buf + offset);
                        if ((en && cur == p.stock) || (!en && cur == p.og)) {
                            *(uint32_t *)(buf + offset) = en ? p.og : p.stock;
                            __asm__ volatile("dsb sy" ::: "memory");
                            my_sen_dma_transfer(1, dmm_phys, aligned_start, aligned_size);
                            for (uint32_t i = 0; i < aligned_size; ++i) buf[i] = 0;
                            __asm__ volatile("dsb sy" ::: "memory");
                            my_sen_dma_transfer(0, aligned_start, dmm_phys, aligned_size);
                        }
                        cur = *(uint32_t *)(buf + offset);
                        if (cur == p.og) ++vef_og;
                        else if (cur == p.stock) ++vef_st;
                        else ++vef_oth;
                    }

                    // Patch CP ZIT/DDX defect pixel correction dictionaries (0x01260fc8 static factory + 0x012630b8 dynamic Pixel Mapping):
                    // At cold boot CP cropped mode 0x0820 keys (side=1/2) to stock 16:9 (y_min=220, h=2408, shifted by -220).
                    // Redirecting mode 0x0820 keys to B[0] (f0=0x02, rect=(0,0,0,0), full-sensor uncropped 4264x2848 DDX map)
                    // makes program_ddx_hw (0x007ab542) program exact HW offset (live_y=0, live_x>>2) and filter all 2848 rows cleanly.
                    static const uint32_t kDdxTblBases[2] = { 0x01260fc8U, 0x012630b8U };
                    uint32_t ddx_og = 0, ddx_st = 0, ddx_oth = 0;
                    for (int t = 0; t < 2; ++t) {
                        uint32_t tbl_base = kDdxTblBases[t];
                        uint32_t attr_addr = tbl_base + 0x12d8U;
                        uint32_t attr_al = attr_addr & ~0x3fU;
                        uint32_t attr_off = attr_addr - attr_al;
                        for (uint32_t i = 0; i < 0x80U; ++i) buf[i] = 0;
                        if (my_sen_dma_transfer(0, attr_al, dmm_phys, 0x80U) != 0) {
                            ddx_oth += 2U;
                            continue;
                        }
                        uint8_t b0_f0 = buf[attr_off + 0];
                        uint16_t b0_x = *(uint16_t *)(buf + attr_off + 2);
                        uint16_t b0_y = *(uint16_t *)(buf + attr_off + 4);
                        uint16_t b0_w = *(uint16_t *)(buf + attr_off + 6);
                        uint16_t b0_h = *(uint16_t *)(buf + attr_off + 8);
                        if (b0_f0 != 0x02U || b0_x != 0U || b0_y != 0U || b0_w != 0U || b0_h != 0U) {
                            ddx_oth += 2U;
                            continue;
                        }
                        uint32_t key_al = tbl_base & ~0x3fU;
                        uint32_t key_off = tbl_base - key_al;
                        const uint32_t key_sz = 0x0a00U;
                        for (uint32_t i = 0; i < key_sz; ++i) buf[i] = 0;
                        if (my_sen_dma_transfer(0, key_al, dmm_phys, key_sz) != 0) {
                            ddx_oth += 2U;
                            continue;
                        }
                        uint32_t num_buckets = *(uint32_t *)(buf + key_off + 0x10U);
                        uint32_t num_keys = *(uint32_t *)(buf + key_off + 0x14U);
                        if (num_buckets < 11U || num_buckets > 300U || num_keys < 15U || num_keys > 300U) {
                            ddx_oth += 2U;
                            continue;
                        }
                        int dirty = 0;
                        for (uint32_t k = 0; k < num_keys; ++k) {
                            uint8_t *kp = buf + key_off + 0x18U + k * 8U;
                            uint32_t dm = *(uint32_t *)(kp + 0);
                            uint8_t flg = kp[4];
                            if (dm == 0x00000820U && (flg == 1U || flg == 2U)) {
                                int slot = (flg == 1U) ? 0 : 1;
                                uint16_t *bp = (uint16_t *)(kp + 6);
                                if (!g_saved_ddx_valid[t][slot]) {
                                    g_saved_ddx_bidx[t][slot] = (*bp != 0U) ? *bp : (uint16_t)((flg == 1U) ? 9U : 10U);
                                    g_saved_ddx_valid[t][slot] = 1;
                                }
                                uint16_t want = en ? 0U : g_saved_ddx_bidx[t][slot];
                                if (*bp != want) {
                                    *bp = want;
                                    dirty = 1;
                                }
                            }
                        }
                        if (dirty) {
                            __asm__ volatile("dsb sy" ::: "memory");
                            my_sen_dma_transfer(1, dmm_phys, key_al, key_sz);
                            for (uint32_t i = 0; i < key_sz; ++i) buf[i] = 0;
                            __asm__ volatile("dsb sy" ::: "memory");
                            my_sen_dma_transfer(0, key_al, dmm_phys, key_sz);
                        }
                        for (uint32_t k = 0; k < num_keys; ++k) {
                            uint8_t *kp = buf + key_off + 0x18U + k * 8U;
                            uint32_t dm = *(uint32_t *)(kp + 0);
                            uint8_t flg = kp[4];
                            if (dm == 0x00000820U && (flg == 1U || flg == 2U)) {
                                int slot = (flg == 1U) ? 0 : 1;
                                uint16_t cur_b = *(uint16_t *)(kp + 6);
                                if (cur_b == 0U) ++ddx_og;
                                else if (cur_b == g_saved_ddx_bidx[t][slot]) ++ddx_st;
                                else ++ddx_oth;
                            }
                        }
                    }

                    write_str("CP_RAM: OG=");
                    write_u32(cp_og);
                    write_str(" STOCK=");
                    write_u32(cp_st);
                    write_str(" OTH=");
                    write_u32(cp_oth);
                    write_str(" | CP_VEF: OG=");
                    write_u32(vef_og);
                    write_str(" STOCK=");
                    write_u32(vef_st);
                    write_str(" OTH=");
                    write_u32(vef_oth);
                    write_str(" | CP_DDX: OG=");
                    write_u32(ddx_og);
                    write_str(" STOCK=");
                    write_u32(ddx_st);
                    write_str(" OTH=");
                    write_u32(ddx_oth);
                    write_str("\n");
                    sc4(215, map_res, 4096 + page_off, 0, 0);
                }
                my_dmm_free(handle);
            }
        }
        sc4(57, mem_fd, 0, 0, 0);
    }

    if (do_refresh) {
        uint32_t cur_sys = g_appfw_base ? *(volatile uint32_t *)(g_appfw_base + 0x7f721f8) : 999U;
        uint32_t cur_opt = g_appfw_base ? *(volatile uint32_t *)(g_appfw_base + 0x7f72268) : 999U;
        uint32_t cur_exp = g_appfw_base ? *(volatile uint32_t *)(g_appfw_base + 0x7f99b78) : 999U;
        uint32_t busy340 = g_appfw_base ? *(volatile uint32_t *)(g_appfw_base + 0x7f72340) : 999U;
        uint32_t ee_b10  = g_appfw_base ? *(volatile uint32_t *)(g_appfw_base + 0x7f72b10) : 999U;
        uint32_t rec360  = g_appfw_base ? *(volatile uint32_t *)(g_appfw_base + 0x7f72360) : 999U;
        // Avoid premature 3:2 stretch over live 16:9 CP frames: let execute_sensor_mode_bounce()
        // apply CMD=0x0147 atomically inside sequence_set_movie_format when in Movie Live View.
        if (!(cur_sys == 3U && cur_opt == 0U && is_movie_or_sq_exp(cur_exp))) {
            refresh_yuv_viewport();
        }
        g_pending_sensor_restart = 1;
        write_str("RESTART=QUEUED SYS=");
        write_u32(cur_sys);
        write_str(" OPT=");
        write_u32(cur_opt);
        write_str(" EXP=");
        write_u32(cur_exp);
        write_str(" B340=");
        write_u32(busy340);
        write_str(" EEB10=");
        write_u32(ee_b10);
        write_str(" REC360=");
        write_u32(rec360);
        write_str("\n");
    }

    if (g_log_fd >= 0) {
        sc4(57, g_log_fd, 0, 0, 0);
        g_log_fd = -1;
    }
}

static void execute_sensor_mode_bounce(void) {
    if (!g_appfw_base) return;
    uint32_t pre_sys = *(volatile uint32_t *)(g_appfw_base + 0x7f721f8);
    uint32_t pre_opt = *(volatile uint32_t *)(g_appfw_base + 0x7f72268);
    uint32_t pre_exp = *(volatile uint32_t *)(g_appfw_base + 0x7f99b78);
    uint32_t busy340 = *(volatile uint32_t *)(g_appfw_base + 0x7f72340);
    uint32_t ee_b10  = *(volatile uint32_t *)(g_appfw_base + 0x7f72b10);
    uint32_t rec360  = *(volatile uint32_t *)(g_appfw_base + 0x7f72360);

    // Matches Sony's 0x26eceac guards: SYSTEM_MODE_REC_MOVIE (3), OPT=0, B340=0 (ready), EEB10!=0 (EE running), REC360=0 (not rec)
    if (pre_sys != 3U || pre_opt != 0U || busy340 != 0U || ee_b10 == 0U || rec360 != 0U || !is_movie_or_sq_exp(pre_exp)) {
        return;
    }

    g_log_fd = sc4(56, -100, (long)"/tmp/jiritsu/og-menu-daemon.txt", 1 | 64 | 1024, 0644);
    write_str("REASON=RECONFIG_MOVIE_FMT_EE_START MODE=");
    write_str(g_last_mode == 1 ? "OG_3x2" : "STOCK_16x9");
    write_str(" PRE_SYS=");
    write_u32(pre_sys);
    write_str(" PRE_OPT=");
    write_u32(pre_opt);
    write_str(" PRE_EXP=");
    write_u32(pre_exp);
    write_str(" B340=");
    write_u32(busy340);
    write_str(" EEB10=");
    write_u32(ee_b10);
    write_str(" REC360=");
    write_u32(rec360);
    write_str("\n");
    if (g_log_fd >= 0) {
        sc4(57, g_log_fd, 0, 0, 0);
        g_log_fd = -1;
    }

    // Enqueue Sony's non-recording Movie Format + EE Restart + HDMI Update atomically in one FIFO flush
    // under model recursive lock (0x2e7849c/0x2e78178), avoiding a double blackout/glitch:
    //   1) cmd 0x0f3 (0x24fabfc): model::sequence_set_movie_format (mpr_SetMovieRecParam + VDF 0x0111/0x0147)
    //   2) cmd 0x2bf (0x250e83c): model::sequence_restart_ee (reloads IMX510 SPI, 0820_CLV, CP_VEF, DDX SRAM)
    //   3) cmd 0x0ee (0x24fa88c): model::sequence_change_hdmi_resolution
    typedef void (*LockFn)(uint64_t *);
    typedef void (*EnqueueSeqFn)(const void *, uint8_t, uint64_t);
    typedef void (*FlushSeqFn)(void);

    LockFn model_lock_enter = reinterpret_cast<LockFn>(g_appfw_base + 0x2e7849c);
    LockFn model_lock_leave = reinterpret_cast<LockFn>(g_appfw_base + 0x2e78178);
    EnqueueSeqFn enqueue_set_movie_format = reinterpret_cast<EnqueueSeqFn>(g_appfw_base + 0x24fabfc);
    EnqueueSeqFn enqueue_restart_ee = reinterpret_cast<EnqueueSeqFn>(g_appfw_base + 0x250e83c);
    EnqueueSeqFn enqueue_hdmi_res = reinterpret_cast<EnqueueSeqFn>(g_appfw_base + 0x24fa88c);
    FlushSeqFn flush_seq_queue = reinterpret_cast<FlushSeqFn>(g_appfw_base + 0x24ef294);

    uint64_t lock = 0;
    uint8_t dummy = 0;
    model_lock_enter(&lock);
    enqueue_set_movie_format(&dummy, 1, 0);
    enqueue_restart_ee(&dummy, 1, 0);
    enqueue_hdmi_res(&dummy, 1, 0);
    flush_seq_queue();
    model_lock_leave(&lock);

    usleep(250000);
    for (int i = 0; i < 30; ++i) {
        uint32_t s = *(volatile uint32_t *)(g_appfw_base + 0x7f721f8);
        uint32_t b = *(volatile uint32_t *)(g_appfw_base + 0x7f72340);
        uint32_t e = *(volatile uint32_t *)(g_appfw_base + 0x7f72b10);
        if (s == 3U && b == 0U && e != 0U) {
            break;
        }
        usleep(50000);
    }
    patch_viewport_table(g_last_mode);
    refresh_yuv_viewport();

    uint32_t end_sys = *(volatile uint32_t *)(g_appfw_base + 0x7f721f8);
    uint32_t end_opt = *(volatile uint32_t *)(g_appfw_base + 0x7f72268);
    uint32_t end_b340 = *(volatile uint32_t *)(g_appfw_base + 0x7f72340);
    uint32_t end_b10 = *(volatile uint32_t *)(g_appfw_base + 0x7f72b10);
    ++g_restart_count;

    g_log_fd = sc4(56, -100, (long)"/tmp/jiritsu/og-menu-daemon.txt", 1 | 64 | 1024, 0644);
    write_str("REASON=RECONFIG_MOVIE_FMT_EE_DONE MODE=");
    write_str(g_last_mode == 1 ? "OG_3x2" : "STOCK_16x9");
    write_str(" SWITCH_COUNT=");
    write_u32(g_switch_count);
    write_str(" RESTART_COUNT=");
    write_u32(g_restart_count);
    write_str(" END_SYS=");
    write_u32(end_sys);
    write_str(" END_OPT=");
    write_u32(end_opt);
    write_str(" B340=");
    write_u32(end_b340);
    write_str(" EEB10=");
    write_u32(end_b10);
    write_str("\n");
    if (g_log_fd >= 0) {
        sc4(57, g_log_fd, 0, 0, 0);
        g_log_fd = -1;
    }
}

static void pwrite_uxc_str(long fd, uint32_t off, uint32_t max_len, const char *str) {
    char tmp[160];
    if (max_len > sizeof(tmp)) max_len = sizeof(tmp);
    for (uint32_t i = 0; i < max_len; ++i) tmp[i] = 0;
    for (uint32_t i = 0; str[i] && i < max_len; ++i) tmp[i] = str[i];
    sc4(68, fd, (long)tmp, max_len, off);
}

static void replace_ram_uxc_str(uint8_t *buf, uint32_t off, uint32_t max_len, const char *str) {
    uint64_t p1 = ((uint64_t)(buf + off)) & ~0xfffULL;
    uint64_t p2 = ((uint64_t)(buf + off + max_len)) & ~0xfffULL;
    sc4(226, (long)p1, 0x1000, 3, 0);
    if (p2 != p1) sc4(226, (long)p2, 0x1000, 3, 0);
    for (uint32_t i = 0; i < max_len; ++i) buf[off + i] = 0;
    for (uint32_t i = 0; str[i] && i < max_len; ++i) buf[off + i] = (uint8_t)str[i];
}

static int g_pfd[2] = {-1, -1};
static int safe_read_mem(uint64_t addr, void *out, uint32_t len) {
    if (addr < 0x10000ULL) return 0;
    if (g_pfd[0] < 0) {
        if (sc4(59, (long)g_pfd, 0, 0, 0) != 0) return 0;
    }
    long w = sc4(64, g_pfd[1], (long)addr, len, 0);
    if (w != (long)len) return 0;
    return sc4(63, g_pfd[0], (long)out, len, 0) == (long)len;
}

static void try_patch_ram_uxc_cand(uint64_t cand) {
    uint8_t tmp[16];
    if (!safe_read_mem(cand, tmp, 16) || !safe_read_mem(cand + 0x21100, tmp, 16)) return;
    uint8_t *ram_uxc = reinterpret_cast<uint8_t *>(cand);
    if (ram_uxc[0] == 0x75 && ram_uxc[1] == 0x78 && ram_uxc[2] == 0x63 && ram_uxc[3] == 0x07 &&
        ram_uxc[10] == 0xe3 && ram_uxc[11] == 0x00) {
        if (ram_uxc[8] == 0x08 && ram_uxc[0x0c2bc] == 0x34 && ram_uxc[0x0c2bd] == 0x65) {
            replace_ram_uxc_str(ram_uxc, 0x0c2bc + 4, 3, "\xee\x99\xa0");
            replace_ram_uxc_str(ram_uxc, 0x0c2d8 + 4, 3, "\xee\x82\x8b");
            replace_ram_uxc_str(ram_uxc, 0x20ff0, 4, "2.0x");
            replace_ram_uxc_str(ram_uxc, 0x2101c, 25, "Open Gate 3:2 3840x2560");
            replace_ram_uxc_str(ram_uxc, 0x0c2c3, 17, "1993 DOOM (E1M1)");
            replace_ram_uxc_str(ram_uxc, 0x0c2df, 17, "1993 DOOM (E1M1)");
            replace_ram_uxc_str(ram_uxc, 0x0c1d8, 82, "Launches classic 1993 DOOM (E1M1: Hangar) with music & sound effects.");
            replace_ram_uxc_str(ram_uxc, 0x0c230, 68, "Exits DOOM overlay and returns to normal camera display.");
            replace_ram_uxc_str(ram_uxc, 0x0c27c, 60, "Launches classic 1993 DOOM (E1M1: Hangar) on camera screen.");
        }
    }
}

static void try_patch_sub_bank(uint64_t sub_bank) {
    if (sub_bank < 0x10000ULL) return;
    uint16_t max_banks = 0;
    uint64_t slots = 0, lru = 0, fc = 0;
    if (safe_read_mem(sub_bank + 0x02, &max_banks, 2) &&
        safe_read_mem(sub_bank + 0x10, &slots, 8) && slots > 0x10000ULL) {
        uint32_t lim = max_banks < 1024 ? max_banks : 1024;
        for (uint32_t i = 0; i < lim; ++i) {
            uint64_t bank_obj = 0, ptr18 = 0;
            if (safe_read_mem(slots + (uint64_t)i * 16, &bank_obj, 8) && bank_obj > 0x10000ULL &&
                safe_read_mem(bank_obj + 0x18, &ptr18, 8) && ptr18 > 0x10008ULL) {
                try_patch_ram_uxc_cand(ptr18 - 8);
                try_patch_ram_uxc_cand(ptr18);
            }
        }
    }
    if (safe_read_mem(sub_bank + 0x18, &lru, 8)) {
        uint64_t node = lru;
        for (int step = 0; node > 0x10000ULL && step < 256; ++step) {
            uint64_t ptr18 = 0, next = 0;
            if (safe_read_mem(node + 0x18, &ptr18, 8) && ptr18 > 0x10008ULL) {
                try_patch_ram_uxc_cand(ptr18 - 8);
                try_patch_ram_uxc_cand(ptr18);
            }
            if (!safe_read_mem(node, &next, 8)) break;
            node = next;
        }
    }
    if (safe_read_mem(sub_bank + 0x08, &fc, 8) && fc > 0x10000ULL) {
        uint32_t cnt = 0;
        if (safe_read_mem(fc + 0x960, &cnt, 4)) {
            uint32_t lim = cnt < 256 ? cnt : 256;
            for (uint32_t i = 0; i < lim; ++i) {
                uint64_t uxc_ptr = 0;
                if (safe_read_mem(fc + 0x968 + (uint64_t)i * 0x18, &uxc_ptr, 8) && uxc_ptr > 0x10000ULL) {
                    try_patch_ram_uxc_cand(uxc_ptr);
                }
            }
        }
    }
}

static uint64_t g_libobj_base = 0;

static void patch_uxc_ram_only(void) {
    if (!g_libobj_base) g_libobj_base = find_module_base("libObj.so");
    if (g_libobj_base) {
        uint64_t rm = 0, db = 0;
        if (safe_read_mem(g_libobj_base + 0x2d2160, &rm, 8) && rm &&
            safe_read_mem(rm + 0x20, &db, 8) && db) {
            try_patch_sub_bank(db + 0x108);
            for (int lang = 1; lang <= 35; ++lang) {
                uint64_t sub_bank = 0;
                if (safe_read_mem(db + 0x140 + (uint64_t)(lang - 1) * 8, &sub_bank, 8) && sub_bank > 0x10000ULL) {
                    try_patch_sub_bank(sub_bank);
                }
            }
        }
    }
}

static void patch_uxc_doom_strings(void) {
    long fd = sc4(56, -100, (long)"/tmp/jiritsu/string_english_f.uxc", 1, 0);
    if (fd >= 0) {
        static const uint8_t hdr32[4] = {0x32, 0x65, 0x1c, 0x1e};
        static const uint8_t hdr33[4] = {0x33, 0x65, 0x1c, 0x1e};
        static const uint8_t hdr34[7] = {0x34, 0x65, 0x1c, 0x1e, 0xee, 0x99, 0xa0};
        static const uint8_t hdr35[7] = {0x35, 0x65, 0x1c, 0x1e, 0xee, 0x82, 0x8b};
        static const uint8_t hdr36[4] = {0x36, 0x65, 0x1c, 0x1e};
        sc4(68, fd, (long)hdr32, 4, 0x0c22c);
        sc4(68, fd, (long)hdr33, 4, 0x0c278);
        sc4(68, fd, (long)hdr34, 7, 0x0c2bc);
        sc4(68, fd, (long)hdr35, 7, 0x0c2d8);
        sc4(68, fd, (long)hdr36, 4, 0x0c2f4);
        pwrite_uxc_str(fd, 0x20ff0, 4, "2.0x");
        pwrite_uxc_str(fd, 0x2101c, 25, "Open Gate 3:2 3840x2560");
        pwrite_uxc_str(fd, 0x0c2c3, 17, "1993 DOOM (E1M1)");
        pwrite_uxc_str(fd, 0x0c2df, 17, "1993 DOOM (E1M1)");
        pwrite_uxc_str(fd, 0x0c1d8, 82, "Launches classic 1993 DOOM (E1M1: Hangar) with music & sound effects.");
        pwrite_uxc_str(fd, 0x0c230, 68, "Exits DOOM overlay and returns to normal camera display.");
        pwrite_uxc_str(fd, 0x0c27c, 60, "Launches classic 1993 DOOM (E1M1: Hangar) on camera screen.");
        sc4(57, fd, 0, 0, 0);
    }
    patch_uxc_ram_only();
}

static void *og_worker(void *) {
    usleep(120000);
    uint32_t prev_sys = 0xffffffffU;
    if (g_appfw_base) {
        uint32_t prio = *(volatile uint32_t *)(g_appfw_base + 0x7f83490);
        if (prio > 1) {
            prio = 0;
            *(volatile uint32_t *)(g_appfw_base + 0x7f83490) = 0;
            *(volatile uint32_t *)(g_appfw_base + 0x7f83498) = 0;
        }
        g_last_prio = prio;
        g_last_mode = (prio == 0) ? 1 : 2;
        g_applied_hw_mode = g_last_mode;
        apply_all_og_patches(g_last_mode, "INIT_V26_CINEMA_LINE", 1);
        uint32_t cur_sys = *(volatile uint32_t *)(g_appfw_base + 0x7f721f8);
        uint32_t cur_opt = *(volatile uint32_t *)(g_appfw_base + 0x7f72268);
        uint32_t cur_exp = *(volatile uint32_t *)(g_appfw_base + 0x7f99b78);
        uint32_t busy340 = *(volatile uint32_t *)(g_appfw_base + 0x7f72340);
        uint32_t ee_b10  = *(volatile uint32_t *)(g_appfw_base + 0x7f72b10);
        uint32_t rec360  = *(volatile uint32_t *)(g_appfw_base + 0x7f72360);
        prev_sys = cur_sys;
        if (cur_sys == 4U && cur_opt == 2U) {
            g_was_usb = 1;
        } else if (cur_sys == 3U && cur_opt == 0U && busy340 == 0U && ee_b10 != 0U && rec360 == 0U && is_movie_or_sq_exp(cur_exp)) {
            // Immediately reconfigure Movie Format + EE right after patching CP RAM so CP does not stream stale 16:9 frames
            g_pending_sensor_restart = 0;
            execute_sensor_mode_bounce();
        }
    }
    uint32_t uxc_tick = 0;
    uint32_t last_shut_mode = 0xffffffffU;
    uint32_t last_angle_val = 0xffffffffU;
    uint32_t last_fps_hash  = 0xffffffffU;
    uint32_t last_angle_sys = 0xffffffffU;
    uint32_t angle_sync_delay = 0;
    for (;;) {
        usleep(60000);
        if (!g_appfw_base) continue;
        if (*(volatile uint32_t *)(g_appfw_base + 0x7f83494) != kDaemonTokenV5) {
            break; // Superseded by a newer daemon
        }
        // Keep menu callback hooks in place even if menu tables are refreshed
        *(void **)(g_appfw_base + 0x5875d40) = (void *)hooked_opengate_menu_cb;
        *(void **)(g_appfw_base + 0x5860040) = (void *)(g_appfw_base + 0x1c94cf4);
        *(void **)(g_appfw_base + 0x587e798) = (void *)hooked_doom_menu_cb;
        // Keep Shutter Angle dial, +/- step locks, and Shutter Mode caps open
        *(volatile uint8_t *)(g_appfw_base + 0x8455f39) = 0;
        *(volatile uint8_t *)(g_appfw_base + 0x8455f3c) = 0;
        *(volatile uint8_t *)(g_appfw_base + 0x8455f3d) = 0;
        *(volatile uint8_t *)(g_appfw_base + 0x7f84f2c) = 0;
        *(volatile uint8_t *)(g_appfw_base + 0x7f84f2d) = 0;
        *(volatile uint8_t *)(g_appfw_base + 0x7f84f2e) = 0;

        uint32_t cur_sys = *(volatile uint32_t *)(g_appfw_base + 0x7f721f8);
        uint32_t cur_opt = *(volatile uint32_t *)(g_appfw_base + 0x7f72268);
        uint32_t cur_exp = *(volatile uint32_t *)(g_appfw_base + 0x7f99b78);
        uint32_t busy340 = *(volatile uint32_t *)(g_appfw_base + 0x7f72340);
        uint32_t ee_b10  = *(volatile uint32_t *)(g_appfw_base + 0x7f72b10);
        uint32_t rec360  = *(volatile uint32_t *)(g_appfw_base + 0x7f72360);
        uint32_t prio    = *(volatile uint32_t *)(g_appfw_base + 0x7f83490);
        if (prio > 1U) {
            prio = (g_last_mode == 1) ? 0U : 1U;
            *(volatile uint32_t *)(g_appfw_base + 0x7f83490) = prio;
            *(volatile uint32_t *)(g_appfw_base + 0x7f83498) = prio;
        }
        uint32_t forced = g_force_refresh;
        int want_mode = (prio == 0U) ? 1 : 2;

        // Hardware Shutter Angle auto-sync across Shutter Mode switches, Rec FPS changes, S&Q FPS changes,
        // and Playback/USB returns in Movie/S&Q Live View.
        {
            uint32_t sm  = *(volatile uint32_t *)(g_appfw_base + 0x7f84f10);
            uint32_t ang = *(volatile uint32_t *)(g_appfw_base + 0x7f84f98);
            uint32_t fh = *(volatile uint32_t *)(g_appfw_base + 0x7f81c10) * 31U;
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f81c18) * 17U;
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f81c60) * 19U;
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f81c88) * 23U;
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f81cb0) * 29U;
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f81cd8) * 37U;
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f81d00) * 41U;
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f81d28) * 43U;
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f81d50) * 47U;
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f82608) * 53U; // S&Q Rec FPS
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f82630) * 59U; // S&Q Frame Rate (Normal)
            fh ^= *(volatile uint32_t *)(g_appfw_base + 0x7f82660) * 61U; // S&Q Frame Rate (DCI)
            fh ^= (cur_exp << 16);
            int in_movie_lv = (cur_sys == 3U && cur_opt == 0U && busy340 == 0U && ee_b10 != 0U && rec360 == 0U && is_movie_or_sq_exp(cur_exp));
            if (in_movie_lv) {
                if (sm != last_shut_mode || (sm == 1U && (ang != last_angle_val || fh != last_fps_hash || cur_sys != last_angle_sys))) {
                    int mode_off_transition = (last_shut_mode == 1U && sm == 0U);
                    last_shut_mode = sm;
                    last_angle_val = ang;
                    last_fps_hash  = fh;
                    last_angle_sys = cur_sys;
                    if (sm == 1U || mode_off_transition) {
                        angle_sync_delay = 3U; // ~180ms settle for format/mode transitions
                    }
                } else if (angle_sync_delay > 0U) {
                    if (--angle_sync_delay == 0U) {
                        trigger_shutter_angle_apply();
                    }
                }
            } else {
                last_angle_sys = cur_sys;
                angle_sync_delay = 0U;
            }
        }

        // SYSTEM_MODE_PLAY_STILL (4) + SYSTEM_MODE_OPTION_MASS_STORAGE_MTP (2) == USB Mass Storage
        if (cur_sys == 4U && cur_opt == 2U) {
            g_was_usb = 1;
        }

        if (prio != g_last_prio || forced || g_applied_hw_mode != want_mode) {
            g_last_prio = prio;
            g_force_refresh = 0;
            g_last_mode = want_mode;
            g_applied_hw_mode = want_mode;
            ++g_switch_count;
            usleep(60000);
            apply_all_og_patches(
                want_mode,
                (want_mode == 1) ? "MENU_ON_3x2" : "MENU_OFF_16x9",
                1);
        }

        // Keep RAM viewport table synced on mode change without firing an async CMD=0x0147 IPC
        // mid-transition (hook_3519d94 already rewrites appFw's synchronized CMD=0x0147 cleanly).
        if (cur_sys != prev_sys) {
            prev_sys = cur_sys;
            if (cur_sys == 3U || cur_sys == 4U || cur_sys == 5U) {
                patch_viewport_table(g_last_mode);
            }
        }

        // Refresh RAM UXC menu strings periodically while not actively recording
        if (rec360 == 0U && (++uxc_tick % 16U) == 0U) {
            patch_uxc_ram_only();
        }

        // SYSTEM_MODE_REC_MOVIE == 3U: reconfigure CP sensor+VEF+DDX once Movie Live View is idle (B340=0, EEB10!=0, REC360=0)
        if (g_pending_sensor_restart || (g_was_usb && cur_sys == 3U && cur_opt == 0U && busy340 == 0U && ee_b10 != 0U && rec360 == 0U)) {
            if (cur_sys == 3U && cur_opt == 0U && busy340 == 0U && ee_b10 != 0U && rec360 == 0U && is_movie_or_sq_exp(cur_exp)) {
                usleep(80000);
                uint32_t s2 = *(volatile uint32_t *)(g_appfw_base + 0x7f721f8);
                uint32_t o2 = *(volatile uint32_t *)(g_appfw_base + 0x7f72268);
                uint32_t x2 = *(volatile uint32_t *)(g_appfw_base + 0x7f99b78);
                uint32_t b2 = *(volatile uint32_t *)(g_appfw_base + 0x7f72340);
                uint32_t e2 = *(volatile uint32_t *)(g_appfw_base + 0x7f72b10);
                uint32_t r2 = *(volatile uint32_t *)(g_appfw_base + 0x7f72360);
                if (s2 == 3U && o2 == 0U && b2 == 0U && e2 != 0U && r2 == 0U && is_movie_or_sq_exp(x2)) {
                    if (g_was_usb) {
                        g_was_usb = 0;
                        patch_uxc_ram_only();
                        apply_all_og_patches(g_last_mode, "USB_EXIT_RESTORE", 0);
                    }
                    g_pending_sensor_restart = 0;
                    execute_sensor_mode_bounce();
                }
            }
        }
    }
    return 0;
}

extern "C" __attribute__((visibility("default"))) void _init(void) {
    g_appfw_base = find_module_base("appFw.so");
    g_mpr_base = find_module_base("libmpr.so");
    g_uipc_base = find_module_base("libosal_uipc.so");
    if (!g_appfw_base || !g_mpr_base || !g_uipc_base) return;

    p_u_osal_init = (fn_u_osal_init)(g_uipc_base + 0x28b8);
    p_osal_snd_msg = (fn_osal_snd_msg)(g_uipc_base + 0x5e60);
    p_osal_snd_sync_msg = (fn_osal_snd_sync_msg)(g_uipc_base + 0x6458);
    p_osal_rcv_msg_tmo = (fn_osal_rcv_msg_tmo)(g_uipc_base + 0x6468);
    p_osal_valloc_msg_wait = (fn_osal_valloc_msg_wait)(g_uipc_base + 0x67b0);
    p_osal_free_msg = (fn_osal_free_msg)(g_uipc_base + 0x6970);
    p_osal_dynamic_id = (fn_osal_dynamic_id)(g_uipc_base + 0x75b8);
    p_osal_set_msg_src = (fn_osal_set_msg_src)(g_uipc_base + 0x7e20);

    // Restore complete 640x480 viewport layout table (0x4b72818) from stock appFw.so and point
    // both 0x595e2e8 and 0x597a6a0 to it, then patch only Row 2 (Movie Live View) to 3:2 (0x19).
    restore_stock_4b72818();
    patch_viewport_table(1);

    // Hook Open Gate 3:2 Recording menu leaf callback slot (0x5875cb8 + 0x88 = 0x5875d40)
    uint64_t cb_slot_va = g_appfw_base + 0x5875d40;
    sc4(226, (long)(cb_slot_va & ~0xfffULL), 4096, 3, 0);
    *(void **)cb_slot_va = (void *)hooked_opengate_menu_cb;

    // Custom Key C3 kept 100% stock (no 0x25556cc override)

    // Prevent physical Mode Dial rotation (0x2f0584c -> 0x27ad10c) and stock recalculation (0x2f06458)
    // from overwriting the user's Open Gate 3:2 Recording choice (0x7f83490 / 0x7f83498).
    uint64_t recalc_va = g_appfw_base + 0x2f06458;
    sc4(226, (long)(recalc_va & ~0xfffULL), 4096, 7, 0);
    *(volatile uint32_t *)recalc_va = 0xd65f03c0; // ret
    flush_icache_addr(recalc_va);

    uint64_t dial_va = g_appfw_base + 0x27ad10c;
    sc4(226, (long)(dial_va & ~0xfffULL), 4096, 7, 0);
    *(volatile uint32_t *)(g_appfw_base + 0x27ad10c) = 0xd65f03c0; // ret
    *(volatile uint32_t *)(g_appfw_base + 0x27ad124) = 0xd65f03c0; // ret
    flush_icache_addr(g_appfw_base + 0x27ad10c);
    flush_icache_addr(g_appfw_base + 0x27ad124);

    uint64_t snap_va = g_appfw_base + 0x2ad8174;
    sc4(226, (long)(snap_va & ~0xfffULL), 4096, 7, 0);
    *(volatile uint32_t *)snap_va = 0xd65f03c0; // ret
    flush_icache_addr(snap_va);

    // Restore De-Squeeze Display (0x5860040) to 100% stock and move 1993 DOOM (E1M1) to
    // the unused FX3 stub leaf Display File Name (0x587e710, callback slot 0x587e798).
    uint64_t desq_slot = g_appfw_base + 0x5860040;
    sc4(226, (long)(desq_slot & ~0xfffULL), 4096, 3, 0);
    *(void **)desq_slot = (void *)(g_appfw_base + 0x1c94cf4);

    uint64_t doom_slot = g_appfw_base + 0x587e798;
    sc4(226, (long)(doom_slot & ~0xfffULL), 4096, 3, 0);
    *(void **)doom_slot = (void *)hooked_doom_menu_cb;

    uint64_t dfn_recalc = g_appfw_base + 0x2f12884;
    sc4(226, (long)(dfn_recalc & ~0xfffULL), 4096, 7, 0);
    *(volatile uint32_t *)dfn_recalc = 0xd65f03c0; // ret
    flush_icache_addr(dfn_recalc);
    if (*(volatile uint32_t *)(g_appfw_base + 0x7f88718) > 1U) {
        *(volatile uint32_t *)(g_appfw_base + 0x7f88718) = 1U;
    }
    if (*(volatile uint32_t *)(g_appfw_base + 0x7f88720) > 1U) {
        *(volatile uint32_t *)(g_appfw_base + 0x7f88720) = 1U;
    }
    *(volatile uint8_t *)(g_appfw_base + 0x7597ea8 + 0x442 * 0x18 + 3) = 1;
    *(volatile uint8_t *)(g_appfw_base + 0x75564e0 + 0xcdc * 0x18 + 3) = 1;
    *(volatile uint8_t *)(g_appfw_base + 0x75564e0 + 0xcdd * 0x18 + 3) = 1;

    patch_uxc_doom_strings();

    // Default to On (0 = 3:2 Open Gate) on fresh daemon load so Live View immediately switches to 3:2
    *(volatile uint32_t *)(g_appfw_base + 0x7f83490) = 0U;
    *(volatile uint32_t *)(g_appfw_base + 0x7f83498) = 0U;
    *(volatile uint32_t *)(g_appfw_base + 0x7f834a8) = 0U;
    *(volatile uint32_t *)(g_appfw_base + 0x8476378) = 0U;
    uint64_t ptr2 = *(volatile uint64_t *)(g_appfw_base + 0x7f834a0);
    if (ptr2 > 0x10000ULL) {
        *(volatile uint32_t *)ptr2 = 0U;
    }

    volatile uint32_t *guard_ptr = (volatile uint32_t *)(g_appfw_base + 0x7f83494);
    *guard_ptr = kDaemonTokenV5;
    long tfd1 = sc4(56, -100, (long)"/tmp/jiritsu/og-menu-daemon.txt", 1 | 64 | 512, 0644);
    if (tfd1 >= 0) sc4(57, tfd1, 0, 0, 0);
    long tfd2 = sc4(56, -100, (long)"/tmp/jiritsu/vdf-log.txt", 1 | 64 | 512, 0644);
    if (tfd2 >= 0) sc4(57, tfd2, 0, 0, 0);
    if (!g_worker_started) {
        g_worker_started = 1;
        uint64_t tid = 0;
        pthread_create(&tid, 0, og_worker, 0);
    }
}
