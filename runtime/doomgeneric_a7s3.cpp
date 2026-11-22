#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>

#define PROT_READ  1
#define PROT_WRITE 2
extern "C" int mprotect(void *addr, size_t len, int prot);

extern "C" {
#include "doomgeneric/doomgeneric/doomgeneric.h"
#include "doomgeneric/doomgeneric/doomkeys.h"
extern int key_menu_confirm;
extern int key_menu_abort;
extern int key_prevweapon;
extern int key_nextweapon;
}

typedef uint64_t (*KeyCallbackFn)(uint64_t x0, void *event, uint64_t x2, uint64_t x3,
                                  uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7);
typedef void *(*GetSingletonFn)(void);
typedef void (*FbrmLockFn)(void *fbrm);
typedef void (*FbrmSetBoolFn)(void *fbrm, int flag);
typedef void (*FbrmSwapFn)(void *fbrm, int flag, void *rect);

static uint64_t g_libobj_base = 0;
static GetSingletonFn g_fn_get_fbrm = nullptr;
static FbrmLockFn g_fn_fbrm_lock = nullptr;
static FbrmLockFn g_fn_fbrm_unlock = nullptr;
static FbrmSetBoolFn g_fn_fbrm_set_freeze = nullptr;
static FbrmSetBoolFn g_fn_fbrm_begin_update = nullptr;
static FbrmSwapFn g_fn_fbrm_swap = nullptr;

static KeyCallbackFn g_orig_key_cb = nullptr;
static void **g_key_cb_slot = nullptr;

// Start hidden in camera menu (MENU -> Setup -> Display Option -> De-Squeeze Display -> DOOM, or C3)
static volatile int g_doom_visible = 0;
static volatile int g_draw_mode = 2; // 2 = Sony PourRaw (lock + setFreeze(1) + BeginUpdate(1) + Swap(1, nullptr))
static volatile uint32_t g_last_ev_code = 0;
static volatile int32_t g_last_ev_status = 0;
static volatile uint32_t g_frame_count = 0;
static volatile uint32_t g_key_log_count = 0;

struct KeyEvent {
    unsigned char key;
    int pressed;
};

#define KEY_QUEUE_SIZE 128
static KeyEvent g_key_queue[KEY_QUEUE_SIZE];
static volatile uint32_t g_key_head = 0;
static volatile uint32_t g_key_tail = 0;
static uint32_t g_dial_release_left_ms = 0;
static uint32_t g_dial_release_right_ms = 0;

static uint16_t g_osd_frame[640 * 480];
static volatile uint32_t g_startup_logs = 0;

static void doom_log_raw(const char *msg, size_t len) {
    if (!msg || len == 0) return;
    int fd = open("/tmp/jiritsu/doom.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        write(fd, msg, len);
        close(fd);
    }
}

static void doom_log(const char *msg) {
    if (!msg) return;
    doom_log_raw(msg, strlen(msg));
}

extern "C" {
int a7s3_fflush(FILE *stream) {
    (void)stream;
    return 0;
}

int a7s3_putchar(int c) {
    char ch = (char)c;
    doom_log_raw(&ch, 1);
    return c;
}

int a7s3_puts(const char *s) {
    if (s) {
        doom_log_raw(s, strlen(s));
    }
    doom_log_raw("\n", 1);
    return 0;
}

int a7s3_vfprintf(FILE *stream, const char *format, va_list ap) {
    if ((uintptr_t)stream > 0x10000UL) {
        char buf[1024];
        int len = vsnprintf(buf, sizeof(buf), format, ap);
        if (len > 0) {
            size_t to_write = (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1;
            fwrite(buf, 1, to_write, stream);
        }
        return len;
    }
    char buf[512];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    if (len > 0) {
        size_t to_write = (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1;
        doom_log_raw(buf, to_write);
    }
    return len;
}

int a7s3_fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = a7s3_vfprintf(stream, format, ap);
    va_end(ap);
    return r;
}

int a7s3_printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int r = a7s3_vfprintf(nullptr, format, ap);
    va_end(ap);
    return r;
}
}

static uint64_t find_module_base(const char *name) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;
    char line[512];
    uint64_t base = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, name) && strstr(line, "r-xp")) {
            base = strtoull(line, nullptr, 16);
            break;
        }
    }
    fclose(fp);
    return base;
}

static long sys_sc6(long n, long a, long b, long c, long d, long e, long f) {
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

static long sys_sc5(long n, long a, long b, long c, long d, long e) {
    return sys_sc6(n, a, b, c, d, e, 0);
}

static long sys_sc4(long n, long a, long b, long c, long d) {
    return sys_sc6(n, a, b, c, d, 0, 0);
}

static void clean_dcache_range(const void *addr, size_t len) {
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)63u;
    uintptr_t end = (uintptr_t)addr + len;
    for (uintptr_t p = start; p < end; p += 64u) {
        __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool safe_read_mem(void *dst, const void *src, size_t len) {
    uintptr_t addr = (uintptr_t)src;
    if (addr < 0x10000UL || addr >= 0x0001000000000000UL) return false;
    int pfd[2] = {-1, -1};
    if (sys_sc4(59, (long)pfd, 0, 0, 0) != 0) return false;
    long w = sys_sc4(64, pfd[1], (long)src, (long)len, 0);
    bool ok = false;
    if (w == (long)len) {
        long r = sys_sc4(63, pfd[0], (long)dst, (long)len, 0);
        ok = (r == (long)len);
    }
    sys_sc4(57, pfd[0], 0, 0, 0);
    sys_sc4(57, pfd[1], 0, 0, 0);
    return ok;
}

static void restore_fbrm_swap_suppress() {
    if (!g_fn_get_fbrm) return;
    void *fbrm = g_fn_get_fbrm();
    if (!fbrm) return;
    if (g_fn_fbrm_lock && g_fn_fbrm_set_freeze && g_fn_fbrm_begin_update && g_fn_fbrm_swap) {
        g_fn_fbrm_lock(fbrm);
        *((volatile uint8_t *)fbrm + 0x60) = 0;
        g_fn_fbrm_set_freeze(fbrm, 0);
        g_fn_fbrm_begin_update(fbrm, 1);
        g_fn_fbrm_swap(fbrm, 1, nullptr); // also unlocks fbrm at 0x0f3eec
    } else {
        *((volatile uint8_t *)fbrm + 0x60) = 0;
        void *screen = *(void **)((char *)fbrm + 0x08);
        if (screen) {
            *((volatile uint8_t *)screen + 0x78) = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
}

// ============================================================================
// Sony A7S III Native Speaker Audio + Real 1993 DOOM WAD Sound Loader
// ============================================================================

struct AdfSysReq {
    uint32_t sound_id; // +0x00: KindID (1..0x1d)
    uint32_t param4;   // +0x04: 0
    uint8_t  byte8;    // +0x08: 1
    uint8_t  is_sync;  // +0x09: 0 = non-blocking async fire-and-forget
    uint16_t pad;      // +0x0a: 0
};

struct AdfBeepTableReq {
    uint8_t  enable;          // +0x00: 0 = unregister, 1 = register
    uint8_t  pad1[3];         // +0x01..0x03: 0
    uint32_t kind_id;         // +0x04: KindID (entry[1])
    uint64_t dsp_phys_addr;   // +0x08: physical address in Audio DSP RAM
    uint64_t size;            // +0x10: byte size (multiple of 64)
    uint32_t priority;        // +0x18: Priority (entry[0], 1..7)
    uint32_t rsrc_acs_type;   // +0x1c: entry[3] (0)
    uint32_t is_continuous;   // +0x20: (cont_length != 0) ? 1 : 0
    uint32_t cont_length;     // +0x24: *(uint32_t *)(entry + 0x08) (0 for one-shot)
    uint32_t cont_first_len;  // +0x28: *(uint32_t *)(entry + 0x04) (0 for one-shot)
    uint32_t pad2[3];         // +0x2c..0x37: 0 (total size = 0x38)
};

struct AdfKindVolReq {
    uint32_t kind_id;     // +0x00: KindID (msg[9])
    uint32_t enable;      // +0x04: 1 = unmute/on, 0 = mute/off (msg[8])
    uint32_t volume;      // +0x08: 2
};

struct AdfAllocReq {
    uint32_t pool_id;     // +0x00: 0x00070817
    uint32_t mem_type;    // +0x04: 0 = Pool 0 (CPU staging), 1 = Pool 1 (Audio DSP)
};

struct AdfAllocResp {
    uint32_t pad0;        // +0x00
    uint32_t hnd;         // +0x04
    uint64_t phys_addr;   // +0x08
    uint32_t size;        // +0x10 (0x7000 = 28672)
    uint32_t offset;      // +0x14 (0)
    uint32_t err;         // +0x18 (0 = OK)
    uint32_t pad1c;       // +0x1c
};

struct AdfMmapReq {
    const char *dev_path; // +0x00: "/dev/mem"
    uint64_t phys_addr;   // +0x08: Pool 0 phys_addr
    uint32_t size;        // +0x10: 28672
    uint32_t flags;       // +0x14: 0 (uses O_RDWR | O_SYNC)
};

struct AdfMmapResp {
    void    *virt_addr;   // +0x00: CPU virtual pointer to Pool 0 buffer
    uint32_t size;        // +0x08
    int32_t  fd;          // +0x0c
    void    *map_base;    // +0x10
    uint32_t map_size;    // +0x18
    uint32_t pad1c;       // +0x1c
    uint32_t err;         // +0x20 (0 = OK)
    uint32_t pad24;       // +0x24
};

struct AdfDmaCopyReq {
    uint32_t src_hnd;     // +0x00: Pool 0 hnd
    uint32_t src_off;     // +0x04: 0
    uint32_t dst_hnd;     // +0x08: Pool 1 hnd
    uint32_t dst_off;     // +0x0c: 0
    uint32_t size;        // +0x10: 28672
};

typedef void *(*GetAdfTableBaseFn)(void);
typedef uint32_t (*LoadAdfEntryFn)(void *fake_this, int idx, uint64_t base_phys_addr);
typedef void (*AdfSetOutputPathFn)(const uint32_t *req3, uint32_t *resp);
typedef void (*AdfSetVolumeFn)(const uint32_t *vol_req, uint32_t *resp);
typedef void (*AdfSetBeepTableFn)(const AdfBeepTableReq *req, uint32_t *resp);
typedef void (*AdfSetKindVolFn)(const AdfKindVolReq *req, uint32_t *resp);
typedef void (*AdfSysRequestFn)(const AdfSysReq *req, uint32_t *resp);
typedef void (*AdfAllocMemFn)(const AdfAllocReq *req, AdfAllocResp *resp);
typedef void (*AdfFreeMemFn)(const uint32_t *hnd_req, uint32_t *resp);
typedef void (*AdfMmapFn)(const AdfMmapReq *req, AdfMmapResp *resp);
typedef void (*AdfDmaCopyFn)(const AdfDmaCopyReq *req, uint32_t *resp);

static uint64_t g_appfw_base = 0;
static GetAdfTableBaseFn  g_fn_get_adf_table = nullptr;
static LoadAdfEntryFn     g_fn_load_adf_entry = nullptr;
static AdfSetOutputPathFn g_fn_adf_set_output = nullptr;
static AdfSetVolumeFn     g_fn_adf_set_volume = nullptr;
static AdfSetBeepTableFn  g_fn_adf_set_beep_table = nullptr;
static AdfSetKindVolFn    g_fn_adf_set_kind_vol = nullptr;
static AdfSysRequestFn    g_fn_adf_request = nullptr;
static AdfAllocMemFn      g_fn_adf_alloc_mem = nullptr;
static AdfFreeMemFn       g_fn_adf_free_mem = nullptr;
static AdfMmapFn          g_fn_adf_mmap = nullptr;
static AdfDmaCopyFn       g_fn_adf_dma_copy = nullptr;

static uint8_t g_orig_adf_table[21][0xb0];
static bool g_orig_adf_saved = false;
static uint64_t g_pack_phys[6] = {0, 0, 0, 0, 0, 0};
static uint32_t g_pack_hnd[6]  = {0, 0, 0, 0, 0, 0};
static uint32_t g_stage_hnd  = 0;
static uint64_t g_stage_phys = 0;
static uint32_t g_stage_size = 28672;
static uint32_t g_stage_off  = 0;
static uint8_t *g_stage_virt = nullptr;
static bool g_doom_dsp_loaded = false;
static volatile int g_quit_requested = 0;

// 6 Pool 1 streaming ring slots (28,672 bytes = 896 blocks = 27,776 samples = 1,157.3 ms each)
static const uint32_t kStreamKindIds[6] = { 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u };
static int g_stream_ring_slot = 0;
static uint64_t g_last_stream_us = 0;
static uint64_t g_last_dma_us = 2500ULL;
static uint32_t g_music_sample_cursor = 0;
static volatile int g_force_mix_now = 0;
static volatile uint32_t g_pending_sfx_mask = 0;
static uint32_t g_stream_push_count = 0;

// Continuous 4-bar (8.721 s = 209,312 samples at 24,000 Hz) full-band loop of D_E1M1 ("At Doom's Gate")
#define E1M1_TOTAL_SAMPLES (16u * 13082u)
static int16_t *g_e1m1_pcm = nullptr;

// 12 full-length 24,000 Hz int16_t PCM DOOM sound effects in CPU RAM for real-time polyphonic mixing
#define NUM_DOOM_SFX 12
static const char *const kDoomSfxLumpNames[NUM_DOOM_SFX] = {
    "DSPISTOL", // 0: Pistol shot
    "DSSHOTGN", // 1: Shotgun blast
    "DSSGCOCK", // 2: Shotgun cock
    "DSITEMUP", // 3: Item/weapon pickup
    "DSDOROPN", // 4: Door open / teleport
    "DSDORCLS", // 5: Door close
    "DSPLPAIN", // 6: Doomguy pain / death
    "DSSWTCHN", // 7: Switch activation
    "DSOOF",    // 8: Doomguy wall "oof!"
    "DSPOSIT1", // 9: Enemy alert
    "DSPODTH1", // 10: Enemy death scream
    "DSBAREXP"  // 11: Barrel / rocket explosion
};
static int16_t *g_sfx_pcm[NUM_DOOM_SFX] = {nullptr};
static uint32_t g_sfx_len[NUM_DOOM_SFX] = {0};

struct ActiveSfxVoice {
    bool active;
    int sfx_idx;
    uint32_t pos;
};
#define MAX_ACTIVE_SFX 8
static ActiveSfxVoice g_active_sfx[MAX_ACTIVE_SFX];

static uint64_t get_now_us() {
    struct timeval tp;
    gettimeofday(&tp, nullptr);
    return (uint64_t)tp.tv_sec * 1000000ULL + (uint64_t)tp.tv_usec;
}

static bool register_dsp_kind(uint32_t kind_id, uint64_t phys_addr, uint64_t size,
                              uint32_t priority, uint32_t rsrc_acs,
                              uint32_t cont_len, uint32_t cont_first_len) {
    if (!g_fn_adf_set_beep_table || !phys_addr || !size) return false;
    AdfBeepTableReq req;
    memset(&req, 0, sizeof(req));
    req.enable         = 0;
    req.kind_id        = kind_id;
    req.dsp_phys_addr  = phys_addr;
    req.size           = size;
    req.priority       = priority;
    req.is_continuous  = (cont_len != 0) ? 1u : 0u;
    req.cont_length    = cont_len;
    req.rsrc_acs_type  = rsrc_acs;
    req.cont_first_len = cont_first_len;

    uint32_t resp = 0;
    g_fn_adf_set_beep_table(&req, &resp); // unregister existing

    req.enable = 1;
    resp = 0;
    g_fn_adf_set_beep_table(&req, &resp); // register new

    uint32_t vresp = 0;
    if (g_fn_adf_set_kind_vol) {
        // vreq[1] (+0x04) is the per-KindID unmute flag (msg[8] = 1 -> ON/unmuted, 0 -> OFF/muted)!
        AdfKindVolReq vreq = { kind_id, 1, 2 };
        g_fn_adf_set_kind_vol(&vreq, &vresp);
    }
    if (kind_id == 0x01 || resp != 0 || vresp != 0) {
        char b[128];
        snprintf(b, sizeof(b), "[SFX] reg kind=0x%02x phys=0x%lx sz=%lu prio=%u resp=0x%x vresp=0x%x\n",
                 kind_id, (unsigned long)phys_addr, (unsigned long)size, priority, resp, vresp);
        doom_log(b);
    }
    return (resp == 0);
}

static void apply_doom_dsp_sounds() {
    if (!g_doom_dsp_loaded || !g_fn_adf_set_beep_table) return;
    if (g_fn_adf_set_output && g_fn_adf_set_volume) {
        uint32_t resp = 0;
        uint32_t out_req[3] = {1, 1, 1};
        g_fn_adf_set_output(out_req, &resp);
        uint32_t vol_req = 2;
        g_fn_adf_set_volume(&vol_req, &resp);
    }
    int ok_cnt = 0;
    for (int p = 0; p < 6; ++p) {
        if (g_pack_phys[p]) {
            if (register_dsp_kind(kStreamKindIds[p], g_pack_phys[p], 28672u, 6u, 0u, 0u, 0u)) {
                ++ok_cnt;
            }
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "[SFX] Activated %d/6 Pool 1 streaming mixer slots (28672B each) in Sony Audio DSP\n",
             ok_cnt);
    doom_log(buf);
}

static void restore_sony_dsp_sounds() {
    if (!g_orig_adf_saved || !g_fn_adf_set_beep_table) return;
    int restored = 0;
    for (int idx = 0; idx < 21; ++idx) {
        const uint8_t *e = g_orig_adf_table[idx];
        uint32_t prio       = e[0];
        uint32_t kind_id    = e[1];
        uint32_t rsrc_acs   = e[3];
        uint32_t cont_first = *(const uint32_t *)(e + 0x04);
        uint32_t cont_len   = *(const uint32_t *)(e + 0x08);
        uint64_t size       = *(const uint64_t *)(e + 0x10);
        uint64_t phys       = *(const uint64_t *)(e + 0x20);
        if (kind_id != 0 && phys != 0 && size != 0) {
            if (register_dsp_kind(kind_id, phys, size, prio, rsrc_acs, cont_len, cont_first)) {
                ++restored;
            }
        }
    }
    char buf[128];
    snprintf(buf, sizeof(buf), "[SFX] Restored %d/21 stock Sony camera sounds in Audio DSP\n", restored);
    doom_log(buf);
}

// Decode a DMX sound lump (11025/22050 Hz 8-bit unsigned PCM) into 24000 Hz 16-bit signed PCM in CPU RAM
static int16_t *decode_dmx_to_pcm16(const uint8_t *wad, size_t wad_size,
                                    const char *lump_name, uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!wad || wad_size < 12 || !lump_name) return nullptr;
    uint32_t numlumps = *(const uint32_t *)(wad + 4);
    uint32_t infotableofs = *(const uint32_t *)(wad + 8);
    if ((size_t)infotableofs + (size_t)numlumps * 16u > wad_size) return nullptr;

    uint32_t lump_pos = 0, lump_size = 0;
    for (uint32_t i = 0; i < numlumps; ++i) {
        const uint8_t *ent = wad + infotableofs + i * 16u;
        char name[9];
        memcpy(name, ent + 8, 8);
        name[8] = '\0';
        if (!strncmp(name, lump_name, 8)) {
            lump_pos  = *(const uint32_t *)(ent + 0);
            lump_size = *(const uint32_t *)(ent + 4);
            break;
        }
    }
    if (!lump_pos || lump_size <= 24 || (size_t)lump_pos + lump_size > wad_size) return nullptr;

    const uint8_t *lump = wad + lump_pos;
    uint16_t fmt  = *(const uint16_t *)(lump + 0);
    uint16_t rate = *(const uint16_t *)(lump + 2);
    uint32_t nsamp = *(const uint32_t *)(lump + 4);
    if (fmt != 3 || rate == 0 || nsamp <= 32) return nullptr;
    if (8u + nsamp > lump_size) nsamp = lump_size - 8u;

    // Skip 16-byte DMX lead-in/lead-out padding
    const uint8_t *src = lump + 8 + 16;
    uint32_t src_len = nsamp - 32;

    uint32_t out_samples_total = (uint32_t)(((uint64_t)src_len * 24000ULL) / rate);
    if (out_samples_total < 64u) return nullptr;
    if (out_samples_total > 48000u) out_samples_total = 48000u; // cap at 2.0s

    int16_t *pcm = (int16_t *)malloc(out_samples_total * sizeof(int16_t));
    if (!pcm) return nullptr;

    for (uint32_t out_idx = 0; out_idx < out_samples_total; ++out_idx) {
        uint64_t num = (uint64_t)out_idx * (uint64_t)rate;
        uint32_t s_idx = (uint32_t)(num / 24000ULL);
        uint32_t frac  = (uint32_t)(num % 24000ULL);
        if (s_idx >= src_len) s_idx = src_len - 1u;
        uint32_t s_next = (s_idx + 1u < src_len) ? (s_idx + 1u) : s_idx;
        int32_t u0 = (int32_t)src[s_idx] - 128;
        int32_t u1 = (int32_t)src[s_next] - 128;
        int32_t interp = u0 * 250 + ((u1 - u0) * (int32_t)frac * 250) / 24000;
        if (interp > 32512) interp = 32512;
        else if (interp < -32512) interp = -32512;
        if (out_idx < 48u) {
            interp = (interp * (int32_t)out_idx) / 48;
        } else if (out_idx + 160u > out_samples_total) {
            uint32_t rem = out_samples_total - out_idx;
            interp = (interp * (int32_t)rem) / 160;
        }
        pcm[out_idx] = (int16_t)interp;
    }

    if (out_len) *out_len = out_samples_total;
    return pcm;
}

static uint32_t midi_note_inc(uint8_t note) {
    // 24,000 Hz 32-bit phase increments for octave 8 (notes 96..107, C7..B7)
    static const uint32_t kOct8Inc[12] = {
        374557489u, 396829839u, 420426546u, 445426410u,
        471912841u, 499974236u, 529704248u, 561202096u,
        594572894u, 629927851u, 667385284u, 707070062u
    };
    if (note > 107u) note = 107u;
    int oct = (int)(note / 12u);
    int semi = (int)(note % 12u);
    int rshift = 8 - oct;
    return (rshift > 0) ? (kOct8Inc[semi] >> rshift) : kOct8Inc[semi];
}

static void encode_pcm16_to_sony_blocks(const int16_t *samples, uint32_t nblocks, uint8_t *out_buf) {
    for (uint32_t b = 0; b < nblocks; ++b) {
        int32_t max_abs = 0;
        for (uint32_t j = 0; j < 31u; ++j) {
            int32_t v = samples[b * 31u + j];
            int32_t a = v < 0 ? -v : v;
            if (a > max_abs) max_abs = a;
        }
        uint8_t shift = 0;
        while (shift < 8u && (max_abs >> shift) > 127) {
            ++shift;
        }
        uint8_t *blk = out_buf + b * 32u;
        blk[0] = shift;
        for (uint32_t j = 0; j < 31u; ++j) {
            int32_t q = ((int32_t)samples[b * 31u + j]) >> shift;
            if (q > 127) q = 127;
            else if (q < -127) q = -127;
            blk[1u + j] = (uint8_t)(int8_t)q;
        }
    }
}

// Parse the DMX MUS lump "D_E1M1" ("At Doom's Gate" by Bobby Prince) from doom1.wad
// and synthesize the full 8.721-second (209,312 samples at 24,000 Hz) continuous 4-bar loop
// (Bars 9..12, ticks 2443..3665: Lead Guitar + Overdriven Rhythm Guitar + Picked Bass + Full Thrash Drum Kit)
// directly into g_e1m1_pcm in CPU RAM for real-time polyphonic mixing with DOOM SFX!
static bool synth_e1m1_from_wad(const uint8_t *wad, size_t wad_size) {
    if (!wad || wad_size < 12) return false;
    uint32_t numlumps = *(const uint32_t *)(wad + 4);
    uint32_t infotableofs = *(const uint32_t *)(wad + 8);
    if ((size_t)infotableofs + (size_t)numlumps * 16u > wad_size) return false;

    uint32_t lump_pos = 0, lump_size = 0;
    for (uint32_t i = 0; i < numlumps; ++i) {
        const uint8_t *ent = wad + infotableofs + i * 16u;
        if (!memcmp(ent + 8, "D_E1M1\0\0", 8)) {
            lump_pos  = *(const uint32_t *)(ent + 0);
            lump_size = *(const uint32_t *)(ent + 4);
            break;
        }
    }
    if (!lump_pos || lump_size < 32 || (size_t)lump_pos + lump_size > wad_size) return false;

    const uint8_t *lump = wad + lump_pos;
    if (memcmp(lump, "MUS\x1a", 4) != 0) return false;
    uint16_t score_start = *(const uint16_t *)(lump + 6);
    if (score_start >= lump_size) return false;

    if (!g_e1m1_pcm) {
        g_e1m1_pcm = (int16_t *)malloc(E1M1_TOTAL_SAMPLES * sizeof(int16_t));
    }
    if (!g_e1m1_pcm) return false;

    struct MusVoice {
        bool active;
        uint8_t ch;
        uint8_t note;
        uint8_t vol;
        uint32_t age;
        uint32_t p1;
        uint32_t p2;
        uint32_t p3;
    };
    MusVoice voices[8];
    memset(voices, 0, sizeof(voices));
    uint8_t ch_vol[16];
    for (int i = 0; i < 16; ++i) ch_vol[i] = 100;

    uint32_t kick_age = 0, kick_phase = 0;
    uint32_t snare_age = 0, snare_phase = 0;
    uint32_t hat_age = 0, hat_len = 1600u;
    uint32_t tom_age = 0, tom_phase = 0, tom_inc = 0;
    uint32_t lfsr = 0xACE1u;
    int32_t prev_noise = 0;

    uint32_t pos = score_start;
    uint32_t cur_tick = 0;

    auto step_mus_to_tick = [&](uint32_t target_tick) {
        while (pos < lump_size && cur_tick <= target_tick) {
            uint8_t desc = lump[pos++];
            uint8_t ch   = desc & 0x0Fu;
            uint8_t ev   = (desc >> 4) & 0x07u;
            bool last    = (desc & 0x80u) != 0;

            if (ev == 0) { // Release note
                if (pos >= lump_size) break;
                uint8_t note = lump[pos++] & 0x7Fu;
                for (int v = 0; v < 8; ++v) {
                    if (voices[v].active && voices[v].ch == ch && voices[v].note == note) {
                        voices[v].active = false;
                    }
                }
            } else if (ev == 1) { // Play note
                if (pos >= lump_size) break;
                uint8_t b1 = lump[pos++];
                uint8_t note = b1 & 0x7Fu;
                if (b1 & 0x80u) {
                    if (pos < lump_size) ch_vol[ch] = lump[pos++] & 0x7Fu;
                }
                if (ch == 15) {
                    if (note == 35 || note == 36) {
                        kick_age = 1;
                        kick_phase = 0;
                    } else if (note == 38 || note == 40) {
                        snare_age = 1;
                        snare_phase = 0;
                    } else if (note == 41 || note == 43 || note == 45 || note == 47 || note == 48 || note == 50) {
                        tom_age = 1;
                        tom_phase = 0;
                        tom_inc = midi_note_inc(note + 12);
                    } else {
                        hat_age = 1;
                        hat_len = (note == 42 || note == 44) ? 1100u : 2600u;
                    }
                } else if (ch <= 2) {
                    int slot = -1;
                    if (ch == 0 || ch == 2) {
                        // Monophonic lead guitar (ch 0) & picked bass (ch 2)
                        for (int v = 0; v < 8; ++v) {
                            if (voices[v].active && voices[v].ch == ch) {
                                slot = v;
                                break;
                            }
                        }
                    } else {
                        // Rhythm guitar (ch 1): reuse same note or keep up to 2 notes for dyads
                        int ch1_cnt = 0;
                        int oldest_v = -1;
                        uint32_t oldest_age = 0;
                        for (int v = 0; v < 8; ++v) {
                            if (voices[v].active && voices[v].ch == 1) {
                                if (voices[v].note == note) {
                                    slot = v;
                                    break;
                                }
                                ++ch1_cnt;
                                if (voices[v].age >= oldest_age) {
                                    oldest_age = voices[v].age;
                                    oldest_v = v;
                                }
                            }
                        }
                        if (slot < 0 && ch1_cnt >= 2) slot = oldest_v;
                    }
                    if (slot < 0) {
                        for (int v = 0; v < 8; ++v) {
                            if (!voices[v].active) {
                                slot = v;
                                break;
                            }
                        }
                    }
                    if (slot < 0) slot = 0;
                    voices[slot].active = true;
                    voices[slot].ch     = ch;
                    voices[slot].note   = note;
                    voices[slot].vol    = ch_vol[ch] ? ch_vol[ch] : 100;
                    voices[slot].age    = 0;
                }
            } else if (ev == 2) { // Pitch bend
                if (pos < lump_size) ++pos;
            } else if (ev == 3) { // System event
                if (pos < lump_size) {
                    uint8_t sys = lump[pos++] & 0x7Fu;
                    if (sys >= 10 && sys <= 14) {
                        for (int v = 0; v < 8; ++v) {
                            if (voices[v].ch == ch) voices[v].active = false;
                        }
                    }
                }
            } else if (ev == 4) { // Controller
                if (pos + 1 < lump_size) {
                    uint8_t ctrl = lump[pos++] & 0x7Fu;
                    uint8_t val  = lump[pos++] & 0x7Fu;
                    if (ctrl == 3) ch_vol[ch] = val;
                } else {
                    pos = lump_size;
                }
            } else if (ev == 6) { // End of score
                break;
            }

            if (last) {
                uint32_t delay = 0;
                while (pos < lump_size) {
                    uint8_t db = lump[pos++];
                    delay = (delay << 7) | (uint32_t)(db & 0x7Fu);
                    if (!(db & 0x80u)) break;
                }
                cur_tick += delay;
            }
        }
    };

    // Fast-forward MUS state from tick 0 to start of Bar 9 (tick 2443)
    step_mus_to_tick(2442);

    // Synthesize all 209,312 samples across Bars 9..12 (ticks 2443..3665) in one continuous pass
    for (uint32_t g_s = 0; g_s < E1M1_TOTAL_SAMPLES; ++g_s) {
        uint32_t q = g_s / 13082u;
        uint32_t s = g_s % 13082u;
        uint32_t t0_num = 2443u * 16u + q * 1222u;
        uint32_t tick_now = (t0_num + (s * 1222u) / 13082u) >> 4;
        step_mus_to_tick(tick_now);

        int32_t mix = 0;
        for (int v = 0; v < 8; ++v) {
            if (!voices[v].active) continue;
            uint8_t note = voices[v].note;
            // Shift sub-bass notes up an octave so the camera's micro-speaker projects E2/D2/C2 loudly
            if (voices[v].ch == 2 && note < 48u) note += 12u;
            uint32_t inc = midi_note_inc(note);
            voices[v].p1 += inc;
            voices[v].p2 += inc << 1;
            voices[v].p3 += (inc * 3u) >> 1;
            uint32_t age = voices[v].age++;

            int32_t saw1 = (int32_t)(voices[v].p1 >> 24) - 128;
            int32_t sq2  = (voices[v].p2 & 0x80000000u) ? 96 : -96;
            int32_t saw3 = (int32_t)(voices[v].p3 >> 24) - 128;

            if (voices[v].ch == 0 || voices[v].ch == 1) {
                // Heavy-metal overdriven guitar (fundamental + octave + 5th power harmonic + pick attack)
                int32_t raw = saw1 * 4 + sq2 * 3 + saw3 * 3;
                if (age < 380u) {
                    raw += (voices[v].p2 & 0x40000000u) ? 260 : -260;
                }
                if (raw > 400) raw = 400 + ((raw - 400) >> 3);
                else if (raw < -400) raw = -400 - ((-400 - raw) >> 3);
                int32_t env = (age < 11000u) ? (256 - (int32_t)(age / 95u)) : 140;
                mix += (raw * env * (int32_t)voices[v].vol) >> 9;
            } else {
                // Picked bass guitar
                int32_t raw = saw1 * 3 + sq2 * 3;
                int32_t env = (age < 6500u) ? (256 - (int32_t)(age / 40u)) : 96;
                mix += (raw * env * (int32_t)voices[v].vol) >> 10;
            }
        }

        // LFSR white noise for snare/hi-hat/crash
        lfsr = (lfsr >> 1) ^ (-(int32_t)(lfsr & 1u) & 0xB400u);
        int32_t noise = (int32_t)(lfsr & 0xFFu) - 128;
        int32_t hp_noise = noise - prev_noise;
        prev_noise = noise;

        if (kick_age > 0 && kick_age < 2400u) {
            uint32_t k_inc = 36000000u - (kick_age * 10500u);
            kick_phase += k_inc;
            int32_t tri = (int32_t)((kick_phase >> 23) & 0x1FFu);
            if (tri >= 256) tri = 511 - tri;
            tri -= 128;
            int32_t k_env = 256 - (int32_t)((kick_age * 256u) / 2400u);
            mix += (tri * k_env) >> 1;
            if (kick_age < 180u) mix += (hp_noise * 45);
            ++kick_age;
        }
        if (snare_age > 0 && snare_age < 3200u) {
            snare_phase += 39370000u;
            int32_t body = (snare_phase & 0x80000000u) ? 90 : -90;
            int32_t s_env = 256 - (int32_t)((snare_age * 256u) / 3200u);
            mix += ((body + noise * 3) * s_env) >> 2;
            ++snare_age;
        }
        if (tom_age > 0 && tom_age < 2600u) {
            uint32_t t_inc = tom_inc - (tom_age * (tom_inc >> 13));
            tom_phase += t_inc;
            int32_t tbody = (tom_phase & 0x80000000u) ? 120 : -120;
            int32_t t_env = 256 - (int32_t)((tom_age * 256u) / 2600u);
            mix += (tbody * t_env) >> 2;
            ++tom_age;
        }
        if (hat_age > 0 && hat_age < hat_len) {
            int32_t h_env = 256 - (int32_t)((hat_age * 256u) / hat_len);
            mix += (hp_noise * h_env) >> 3;
            ++hat_age;
        }

        // Tiny 48-sample (2.0 ms) de-click only at the outer 8.721-second loop boundary
        if (g_s < 48u) {
            mix = (mix * (int32_t)g_s) / 48;
        } else if (g_s + 48u > E1M1_TOTAL_SAMPLES) {
            mix = (mix * (int32_t)(E1M1_TOTAL_SAMPLES - g_s)) / 48;
        }

        if (mix > 30000) mix = 30000;
        else if (mix < -30000) mix = -30000;
        g_e1m1_pcm[g_s] = (int16_t)mix;
    }

    return true;
}

struct DspSavedStateV2 {
    uint64_t appfw_base;
    uint64_t pack_phys[6];
    uint32_t pack_hnd[6];
    uint32_t magic;      // 0x444d5632u ('DMV2')
    uint32_t stage_hnd;  // Pool 0 staging handle
    uint64_t stage_phys; // Pool 0 staging physical address
    uint32_t stage_size; // 28672
    uint32_t stage_off;  // 0
};

static bool alloc_or_repair_dsp_buffers(bool force_realloc) {
    if (!g_fn_adf_alloc_mem || !g_fn_adf_mmap || !g_fn_adf_dma_copy) return false;

    DspSavedStateV2 saved_state;
    memset(&saved_state, 0, sizeof(saved_state));
    bool can_reuse = false;

    if (!force_realloc) {
        int sfd = open("/tmp/jiritsu/doom_dsp_state.bin", O_RDONLY);
        if (sfd >= 0) {
            ssize_t nread = read(sfd, &saved_state, sizeof(saved_state));
            close(sfd);
            if (nread == (ssize_t)sizeof(DspSavedStateV2) &&
                saved_state.appfw_base == g_appfw_base &&
                saved_state.magic == 0x444d5633u &&
                saved_state.pack_phys[0] != 0 &&
                saved_state.pack_hnd[0] != 0 &&
                saved_state.stage_phys != 0 &&
                saved_state.stage_hnd != 0) {
                can_reuse = true;
            }
        }
    }

    if (can_reuse) {
        for (int p = 0; p < 6; ++p) {
            g_pack_phys[p] = saved_state.pack_phys[p];
            g_pack_hnd[p]  = saved_state.pack_hnd[p];
        }
        g_stage_hnd  = saved_state.stage_hnd;
        g_stage_phys = saved_state.stage_phys;
        g_stage_size = saved_state.stage_size ? saved_state.stage_size : 28672u;
        g_stage_off  = saved_state.stage_off;

        if (!g_stage_virt) {
            AdfMmapReq mreq = { "/dev/mem", g_stage_phys, g_stage_size, 0u };
            AdfMmapResp mresp;
            memset(&mresp, 0, sizeof(mresp));
            g_fn_adf_mmap(&mreq, &mresp);
            if (mresp.err == 0 && mresp.virt_addr != nullptr) {
                g_stage_virt = (uint8_t *)mresp.virt_addr;
            }
        }

        // Probe all 6 Pool 1 handles with a 32-byte DMA copy to verify none were freed by Sony mode changes
        bool dma_ok = (g_stage_virt != nullptr);
        uint32_t bad_dresp = 0;
        if (dma_ok) {
            memset(g_stage_virt, 0, 32);
            for (int p = 0; p < 6; ++p) {
                AdfDmaCopyReq dreq = { g_stage_hnd, g_stage_off, g_pack_hnd[p], 0u, 32u };
                uint32_t dresp = 0;
                g_fn_adf_dma_copy(&dreq, &dresp);
                if (dresp != 0) {
                    dma_ok = false;
                    bad_dresp = dresp;
                    break;
                }
            }
        }
        if (dma_ok) {
            char logbuf[160];
            snprintf(logbuf, sizeof(logbuf),
                     "[SFX] Verified & reused 6 Pool 1 slots (hnd[0]=0x%x..hnd[5]=0x%x) + Pool 0 stage (hnd=0x%x)\n",
                     g_pack_hnd[0], g_pack_hnd[5], g_stage_hnd);
            doom_log(logbuf);
            return true;
        }
        char logbuf[160];
        snprintf(logbuf, sizeof(logbuf),
                 "[SFX] Cached DSP handles invalid (dresp=0x%x), reallocating fresh Pool 0 + Pool 1 buffers\n",
                 bad_dresp);
        doom_log(logbuf);
        g_stage_virt = nullptr;
    }

    // 1. Allocate fresh Pool 0 (CPU staging DRAM) buffer directly via g_fn_adf_alloc_mem (mem_type = 0)
    {
        AdfAllocReq areq = { 0x00070817u, 0u };
        AdfAllocResp aresp;
        memset(&aresp, 0, sizeof(aresp));
        g_fn_adf_alloc_mem(&areq, &aresp);
        if (aresp.err == 0 && aresp.phys_addr != 0) {
            g_stage_hnd  = aresp.hnd;
            g_stage_phys = aresp.phys_addr;
            g_stage_size = aresp.size ? aresp.size : 28672u;
            g_stage_off  = aresp.offset;
        }
        char logbuf[160];
        snprintf(logbuf, sizeof(logbuf),
                 "[SFX] Allocated fresh Pool 0 staging buffer -> err=0x%x phys=0x%lx hnd=0x%x sz=%u off=%u\n",
                 aresp.err, (unsigned long)g_stage_phys, g_stage_hnd, g_stage_size, g_stage_off);
        doom_log(logbuf);
    }

    if (g_stage_phys != 0) {
        AdfMmapReq mreq = { "/dev/mem", g_stage_phys, g_stage_size, 0u };
        AdfMmapResp mresp;
        memset(&mresp, 0, sizeof(mresp));
        g_fn_adf_mmap(&mreq, &mresp);
        if (mresp.err == 0 && mresp.virt_addr != nullptr) {
            g_stage_virt = (uint8_t *)mresp.virt_addr;
            memset(g_stage_virt, 0, 28672);
        }
        char logbuf[160];
        snprintf(logbuf, sizeof(logbuf),
                 "[SFX] Mapped Pool 0 staging buffer -> err=0x%x virt=%p fd=%d\n",
                 mresp.err, mresp.virt_addr, mresp.fd);
        doom_log(logbuf);
    }

    // 2. Allocate 6 fresh Pool 1 (Audio DSP RAM) ring slots directly via g_fn_adf_alloc_mem (mem_type = 1)
    // without touching Sony's 21-entry ADF table so mode changes never free our handles!
    for (int p = 0; p < 6; ++p) {
        AdfAllocReq areq = { 0x00070817u, 1u };
        AdfAllocResp aresp;
        memset(&aresp, 0, sizeof(aresp));
        g_fn_adf_alloc_mem(&areq, &aresp);
        if (aresp.err == 0 && aresp.phys_addr != 0) {
            g_pack_hnd[p]  = aresp.hnd;
            g_pack_phys[p] = aresp.phys_addr;
        }
        uint32_t dresp = 0;
        if (g_stage_hnd != 0 && g_pack_hnd[p] != 0) {
            AdfDmaCopyReq dreq = { g_stage_hnd, g_stage_off, g_pack_hnd[p], 0u, 28672u };
            g_fn_adf_dma_copy(&dreq, &dresp);
        }
        char logbuf[160];
        snprintf(logbuf, sizeof(logbuf),
                 "[SFX] Allocated fresh Pool 1 ring slot %d -> err=0x%x phys=0x%lx hnd=0x%x zero_dresp=0x%x\n",
                 p, aresp.err, (unsigned long)g_pack_phys[p], g_pack_hnd[p], dresp);
        doom_log(logbuf);
    }

    if (g_pack_phys[0] != 0 && g_stage_phys != 0) {
        memset(&saved_state, 0, sizeof(saved_state));
        saved_state.appfw_base = g_appfw_base;
        for (int p = 0; p < 6; ++p) {
            saved_state.pack_phys[p] = g_pack_phys[p];
            saved_state.pack_hnd[p]  = g_pack_hnd[p];
        }
        saved_state.magic      = 0x444d5633u; // 'DMV3'
        saved_state.stage_hnd  = g_stage_hnd;
        saved_state.stage_phys = g_stage_phys;
        saved_state.stage_size = g_stage_size;
        saved_state.stage_off  = g_stage_off;
        int ofd = open("/tmp/jiritsu/doom_dsp_state.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ofd >= 0) {
            write(ofd, &saved_state, sizeof(saved_state));
            close(ofd);
        }
        return true;
    }
    return false;
}

static void build_and_upload_doom_dsp_packs(void *table_base) {
    if (!table_base || g_doom_dsp_loaded) return;

    int fd = open("/tmp/jiritsu/doom1.wad", O_RDONLY);
    if (fd < 0) {
        doom_log("[SFX] Cannot open /tmp/jiritsu/doom1.wad for SFX extraction\n");
        return;
    }
    off_t fsz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (fsz <= 0 || fsz > 16 * 1024 * 1024) {
        close(fd);
        return;
    }
    uint8_t *wad = (uint8_t *)malloc((size_t)fsz);
    if (!wad) {
        close(fd);
        return;
    }
    size_t rd = 0;
    while (rd < (size_t)fsz) {
        ssize_t n = read(fd, wad + rd, (size_t)fsz - rd);
        if (n <= 0) break;
        rd += (size_t)n;
    }
    close(fd);

    bool have_e1m1 = synth_e1m1_from_wad(wad, rd);
    doom_log(have_e1m1 ? "[MUS] Synthesized continuous 8.72s D_E1M1 (At Doom's Gate, 209312 samples) in CPU RAM\n"
                       : "[MUS] Failed to synthesize D_E1M1 from doom1.wad\n");

    int decoded_sfx = 0;
    for (int i = 0; i < NUM_DOOM_SFX; ++i) {
        g_sfx_pcm[i] = decode_dmx_to_pcm16(wad, rd, kDoomSfxLumpNames[i], &g_sfx_len[i]);
        if (g_sfx_pcm[i] && g_sfx_len[i] > 0) {
            ++decoded_sfx;
        }
    }
    {
        char sbuf[128];
        snprintf(sbuf, sizeof(sbuf), "[SFX] Decoded %d/%d DOOM sound effects into 24kHz PCM16 in CPU RAM\n",
                 decoded_sfx, NUM_DOOM_SFX);
        doom_log(sbuf);
    }
    free(wad);

    alloc_or_repair_dsp_buffers(false);
    g_doom_dsp_loaded = true;
}

// 28,672 bytes = 896 blocks * 31 samples/block = 27,776 samples (1,157.33 ms at 24,000 Hz)
#define STREAM_BLOCKS  896u
#define STREAM_SAMPLES (STREAM_BLOCKS * 31u)
static int16_t g_mix_pcm[STREAM_SAMPLES];

static void mix_and_push_stream_buffer(bool is_fresh_start) {
    if (!g_doom_visible || !g_stage_virt || !g_fn_adf_dma_copy || !g_fn_adf_request) return;

    uint64_t now_us = get_now_us();
    uint32_t advance_samples = 0;
    if (is_fresh_start || g_last_stream_us == 0) {
        advance_samples = 0;
    } else {
        uint64_t elapsed_us = (now_us > g_last_stream_us) ? (now_us - g_last_stream_us + g_last_dma_us) : g_last_dma_us;
        advance_samples = (uint32_t)((elapsed_us * 24ULL) / 1000ULL);
        if (advance_samples > STREAM_SAMPLES) advance_samples = STREAM_SAMPLES;
    }

    // 1. Advance continuous D_E1M1 music cursor by exact elapsed samples so the music never resets or stops!
    g_music_sample_cursor = (g_music_sample_cursor + advance_samples) % E1M1_TOTAL_SAMPLES;

    // 2. Advance all currently active SFX voices by elapsed samples (keeping ongoing sounds alive)
    for (int v = 0; v < MAX_ACTIVE_SFX; ++v) {
        if (g_active_sfx[v].active) {
            g_active_sfx[v].pos += advance_samples;
            int idx = g_active_sfx[v].sfx_idx;
            if (idx < 0 || idx >= NUM_DOOM_SFX || g_active_sfx[v].pos >= g_sfx_len[idx]) {
                g_active_sfx[v].active = false;
            }
        }
    }

    // 3. Atomically consume newly triggered SFX bits and spawn/restart voices at pos = 0
    uint32_t new_mask = __sync_fetch_and_and(&g_pending_sfx_mask, 0u);
    if (new_mask != 0) {
        for (int idx = 0; idx < NUM_DOOM_SFX; ++idx) {
            if ((new_mask & (1u << idx)) && g_sfx_pcm[idx] && g_sfx_len[idx] > 0) {
                int target_v = -1;
                // If the same SFX is already playing, retrigger its voice from sample 0
                for (int v = 0; v < MAX_ACTIVE_SFX; ++v) {
                    if (g_active_sfx[v].active && g_active_sfx[v].sfx_idx == idx) {
                        target_v = v;
                        break;
                    }
                }
                if (target_v < 0) {
                    for (int v = 0; v < MAX_ACTIVE_SFX; ++v) {
                        if (!g_active_sfx[v].active) {
                            target_v = v;
                            break;
                        }
                    }
                }
                if (target_v < 0) {
                    uint32_t oldest_pos = 0;
                    target_v = 0;
                    for (int v = 0; v < MAX_ACTIVE_SFX; ++v) {
                        if (g_active_sfx[v].pos >= oldest_pos) {
                            oldest_pos = g_active_sfx[v].pos;
                            target_v = v;
                        }
                    }
                }
                g_active_sfx[target_v].active  = true;
                g_active_sfx[target_v].sfx_idx = idx;
                g_active_sfx[target_v].pos     = 0;
            }
        }
    }

    // 4. Mix 27,776 samples (1.157 s) of D_E1M1 music + all active DOOM SFX voices
    uint32_t mus_cur = g_music_sample_cursor;
    for (uint32_t s = 0; s < STREAM_SAMPLES; ++s) {
        int32_t sfx_sum = 0;
        int active_cnt = 0;
        for (int v = 0; v < MAX_ACTIVE_SFX; ++v) {
            if (!g_active_sfx[v].active) continue;
            int idx = g_active_sfx[v].sfx_idx;
            uint32_t p = g_active_sfx[v].pos + s;
            if (p < g_sfx_len[idx]) {
                sfx_sum += (int32_t)g_sfx_pcm[idx][p];
                ++active_cnt;
            }
        }

        int32_t mus_val = 0;
        if (g_e1m1_pcm) {
            mus_val = (int32_t)g_e1m1_pcm[mus_cur];
            if (++mus_cur >= E1M1_TOTAL_SAMPLES) mus_cur = 0;
        }

        // Smooth automatic ducking: keep music at 75% normally, and 55% while gunshots/explosions
        // are actively sounding so both the SFX punch and the D_E1M1 riff are crystal clear!
        int32_t mus_gain = (active_cnt > 0) ? 140 : 192;
        int32_t mix = ((mus_val * mus_gain) >> 8) + ((sfx_sum * 224) >> 8);

        // Soft knee limiter to prevent hard digital clipping when multiple SFX + music coincide
        if (mix > 24000) mix = 24000 + ((mix - 24000) >> 2);
        else if (mix < -24000) mix = -24000 - ((-24000 - mix) >> 2);
        if (mix > 32512) mix = 32512;
        else if (mix < -32512) mix = -32512;

        // 24-sample (1.0 ms) micro-ramp at buffer edges
        if (s < 24u) {
            mix = (mix * (int32_t)s) / 24;
        } else if (s + 24u > STREAM_SAMPLES) {
            mix = (mix * (int32_t)(STREAM_SAMPLES - s)) / 24;
        }
        g_mix_pcm[s] = (int16_t)mix;
    }

    // 5. Encode 896 Sony DSP 32-byte blocks (28,672 bytes) into Pool 0 staging buffer
    encode_pcm16_to_sony_blocks(g_mix_pcm, STREAM_BLOCKS, g_stage_virt);

    // 6. DMA transfer 28,672 bytes from Pool 0 (g_stage_hnd) into next Pool 1 ring slot (g_pack_hnd[slot])
    int slot = g_stream_ring_slot;
    g_stream_ring_slot = (slot + 1) % 6;

    uint64_t t_dma0 = get_now_us();
    AdfDmaCopyReq dreq = { g_stage_hnd, g_stage_off, g_pack_hnd[slot], 0u, 28672u };
    uint32_t dresp = 0;
    g_fn_adf_dma_copy(&dreq, &dresp);
    if (dresp != 0) {
        char rbuf[128];
        snprintf(rbuf, sizeof(rbuf), "[MIX] DMA error dresp=0x%x on slot %d -> self-healing DSP buffers!\n", dresp, slot);
        doom_log_raw(rbuf, strlen(rbuf));
        if (alloc_or_repair_dsp_buffers(true) && g_stage_virt) {
            apply_doom_dsp_sounds();
            encode_pcm16_to_sony_blocks(g_mix_pcm, STREAM_BLOCKS, g_stage_virt);
            dreq.src_hnd = g_stage_hnd;
            dreq.src_off = g_stage_off;
            dreq.dst_hnd = g_pack_hnd[slot];
            dresp = 0;
            g_fn_adf_dma_copy(&dreq, &dresp);
        }
    }
    uint64_t t_dma1 = get_now_us();
    if (t_dma1 > t_dma0 && (t_dma1 - t_dma0) < 50000ULL) {
        g_last_dma_us = t_dma1 - t_dma0;
    }
    g_last_stream_us = t_dma1;

    // 7. Trigger playback of the updated Pool 1 ring slot
    AdfSysReq sreq;
    sreq.sound_id = kStreamKindIds[slot];
    sreq.param4   = 0;
    sreq.byte8    = 1;
    sreq.is_sync  = 0;
    sreq.pad      = 0;
    uint32_t sresp = 0;
    g_fn_adf_request(&sreq, &sresp);

    uint32_t pcnt = __sync_fetch_and_add(&g_stream_push_count, 1);
    if (pcnt < 16 || (pcnt % 50 == 0)) {
        char buf[140];
        snprintf(buf, sizeof(buf),
                 "[MIX] #%u slot=%d kind=0x%02x mus_cur=%u new_sfx=0x%x dma_us=%lu dresp=0x%x sresp=0x%x\n",
                 pcnt, slot, (unsigned)kStreamKindIds[slot], g_music_sample_cursor,
                 new_mask, (unsigned long)g_last_dma_us, dresp, sresp);
        doom_log_raw(buf, strlen(buf));
    }
}

static void *audio_mixer_thread_main(void *arg) {
    (void)arg;
    doom_log("[MIX] Real-time polyphonic audio mixer thread started\n");
    while (!g_quit_requested) {
        if (!g_doom_visible || !g_doom_dsp_loaded || !g_stage_virt) {
            usleep(15000);
            continue;
        }
        uint64_t now_us = get_now_us();
        uint64_t elapsed_us = (now_us >= g_last_stream_us) ? (now_us - g_last_stream_us) : 0ULL;

        if (__sync_lock_test_and_set(&g_force_mix_now, 0) != 0) {
            mix_and_push_stream_buffer(true);
        } else if (g_pending_sfx_mask != 0 && elapsed_us >= 35000ULL) {
            mix_and_push_stream_buffer(false);
        } else if (elapsed_us >= 960000ULL) {
            mix_and_push_stream_buffer(false);
        }
        usleep(8000);
    }
    return nullptr;
}

static void ensure_input_hook_active();

static void set_doom_visible(int vis) {
    int old_vis = g_doom_visible;
    if (vis == old_vis) return;
    unlink("/tmp/jiritsu/doom_v14_mode");
    unlink("/tmp/jiritsu/doom_v15_mode");
    unlink("/tmp/jiritsu/doom_v16_mode");
    if (vis) {
        g_key_head = g_key_tail = 0;
        g_key_log_count = 0;
        ensure_input_hook_active();
        apply_doom_dsp_sounds();
        memset(g_active_sfx, 0, sizeof(g_active_sfx));
        g_music_sample_cursor = 0;
        g_last_stream_us = 0;
        // Queue DSITEMUP (idx 3) on launch so it plays mixed together with D_E1M1!
        __sync_fetch_and_or(&g_pending_sfx_mask, (1u << 3));
        g_doom_visible = 1;
        if (g_appfw_base) {
            *(volatile uint32_t *)(g_appfw_base + 0x7f88718) = 0u;
            *(volatile uint32_t *)(g_appfw_base + 0x7f88720) = 0u;
        }
        __sync_lock_test_and_set(&g_force_mix_now, 1);
    } else {
        g_doom_visible = 0;
        if (g_appfw_base) {
            *(volatile uint32_t *)(g_appfw_base + 0x7f88718) = 1u;
            *(volatile uint32_t *)(g_appfw_base + 0x7f88720) = 1u;
        }
        g_pending_sfx_mask = 0;
        memset(g_active_sfx, 0, sizeof(g_active_sfx));
        g_key_head = g_key_tail = 0;
        restore_fbrm_swap_suppress();
        restore_sony_dsp_sounds();
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "[VIS] DOOM visibility %d -> %d\n", old_vis, vis);
    doom_log(buf);
}

// ============================================================================
// Sony Camera Menu Integration:
// MENU -> Setup -> Display Option -> De-Squeeze Display -> DOOM
// ============================================================================

typedef uint64_t (*MenuLeafCbFn)(void *menu_arg);
typedef uint32_t (*MenuGetSelFn)(void *menu_arg);

static void **g_desqueeze_cb_slot = nullptr;
static MenuLeafCbFn g_orig_desqueeze_cb = nullptr;
static MenuGetSelFn g_fn_menu_get_sel = nullptr;

static uint64_t hooked_desqueeze_menu_cb(void *menu_arg) {
    uint32_t val = g_fn_menu_get_sel ? g_fn_menu_get_sel(menu_arg) : 0;
    char buf[96];
    snprintf(buf, sizeof(buf), "[MENU] 1993 DOOM (Display File Name stub) selected val=%u\n", val);
    doom_log_raw(buf, strlen(buf));

    if (val == 0u) {
        set_doom_visible(1);
    } else {
        set_doom_visible(0);
    }
    return 1;
}

static int g_pfd[2] = {-1, -1};
static int safe_read_mem(uintptr_t addr, void *out, size_t len) {
    if (addr < 0x10000UL) return 0;
    if (g_pfd[0] < 0) {
        if (sys_sc4(59, (long)g_pfd, 0, 0, 0) != 0) return 0;
    }
    ssize_t w = write(g_pfd[1], (const void *)addr, len);
    if (w != (ssize_t)len) return 0;
    return read(g_pfd[0], out, len) == (ssize_t)len;
}

static void replace_uxc_str(uint8_t *buf, size_t sz, size_t off, size_t max_len, const char *s) {
    if (off + max_len > sz) return;
    memset(buf + off, 0, max_len);
    size_t slen = strlen(s);
    if (slen > max_len) slen = max_len;
    memcpy(buf + off, s, slen);
}

static void try_patch_doom_ram_uxc_cand(uintptr_t cand) {
    uint8_t tmp[16];
    if (!safe_read_mem(cand, tmp, 16) || !safe_read_mem(cand + 0x21100, tmp, 16)) return;
    uint8_t *ram_uxc = (uint8_t *)cand;
    if (ram_uxc[0] == 0x75 && ram_uxc[1] == 0x78 && ram_uxc[2] == 0x63 && ram_uxc[3] == 0x07 &&
        ram_uxc[10] == 0xe3 && ram_uxc[11] == 0x00) {
        mprotect((void *)(cand & ~0xfffUL), 0x40000, PROT_READ | PROT_WRITE);
        if (ram_uxc[8] == 0x08 && ram_uxc[0x0c2bc] == 0x34 && ram_uxc[0x0c2bd] == 0x65) {
            ram_uxc[0x0c2bc + 4] = 0xee; ram_uxc[0x0c2bc + 5] = 0x99; ram_uxc[0x0c2bc + 6] = 0xa0;
            ram_uxc[0x0c2d8 + 4] = 0xee; ram_uxc[0x0c2d8 + 5] = 0x82; ram_uxc[0x0c2d8 + 6] = 0x8b;
            replace_uxc_str(ram_uxc, 0x25000, 0x20ff0, 4, "2.0x");
            replace_uxc_str(ram_uxc, 0x25000, 0x2101c, 25, "Open Gate 3:2 Recording");
            replace_uxc_str(ram_uxc, 0x25000, 0x0c2c3, 17, "1993 DOOM (E1M1)");
            replace_uxc_str(ram_uxc, 0x25000, 0x0c2df, 17, "1993 DOOM (E1M1)");
            replace_uxc_str(ram_uxc, 0x25000, 0x0c1d8, 82, "Launches classic 1993 DOOM (E1M1: Hangar) with music & sound effects.");
            replace_uxc_str(ram_uxc, 0x25000, 0x0c230, 68, "Exits DOOM overlay and returns to normal camera display.");
            replace_uxc_str(ram_uxc, 0x25000, 0x0c27c, 60, "Launches classic 1993 DOOM (E1M1: Hangar) on camera screen.");
            char msg[128];
            snprintf(msg, sizeof(msg), "[MENU] Patched in-RAM string_english_f.uxc (ram_uxc=0x%lx) -> 1993 DOOM (E1M1)\n", (unsigned long)cand);
            doom_log(msg);
        }
    }
}

static void try_patch_doom_sub_bank(uintptr_t sub_bank) {
    if (sub_bank < 0x10000UL) return;
    uint16_t max_banks = 0;
    uintptr_t slots = 0, lru = 0, fc = 0;
    if (safe_read_mem(sub_bank + 0x02, &max_banks, 2) &&
        safe_read_mem(sub_bank + 0x10, &slots, 8) && slots > 0x10000UL) {
        uint32_t lim = max_banks < 1024 ? max_banks : 1024;
        for (uint32_t i = 0; i < lim; ++i) {
            uintptr_t bank_obj = 0, ptr18 = 0;
            if (safe_read_mem(slots + (uintptr_t)i * 16, &bank_obj, 8) && bank_obj > 0x10000UL &&
                safe_read_mem(bank_obj + 0x18, &ptr18, 8) && ptr18 > 0x10008UL) {
                try_patch_doom_ram_uxc_cand(ptr18 - 8);
                try_patch_doom_ram_uxc_cand(ptr18);
            }
        }
    }
    if (safe_read_mem(sub_bank + 0x18, &lru, 8)) {
        uintptr_t node = lru;
        for (int step = 0; node > 0x10000UL && step < 256; ++step) {
            uintptr_t ptr18 = 0, next = 0;
            if (safe_read_mem(node + 0x18, &ptr18, 8) && ptr18 > 0x10008UL) {
                try_patch_doom_ram_uxc_cand(ptr18 - 8);
                try_patch_doom_ram_uxc_cand(ptr18);
            }
            if (!safe_read_mem(node, &next, 8)) break;
            node = next;
        }
    }
    if (safe_read_mem(sub_bank + 0x08, &fc, 8) && fc > 0x10000UL) {
        uint32_t cnt = 0;
        if (safe_read_mem(fc + 0x960, &cnt, 4)) {
            uint32_t lim = cnt < 256 ? cnt : 256;
            for (uint32_t i = 0; i < lim; ++i) {
                uintptr_t uxc_ptr = 0;
                if (safe_read_mem(fc + 0x968 + (uintptr_t)i * 0x18, &uxc_ptr, 8) && uxc_ptr > 0x10000UL) {
                    try_patch_doom_ram_uxc_cand(uxc_ptr);
                }
            }
        }
    }
}

static void patch_desqueeze_uxc_strings() {
    // Unmount all stacked bind-mounts first so we read the clean stock UXC file
    for (int i = 0; i < 64; ++i) {
        if (sys_sc4(39, (long)"/usr/share/app/string_english_f.uxc", 2 /* MNT_DETACH */, 0, 0) != 0) break;
    }

    int fd = open("/usr/share/app/string_english_f.uxc", O_RDONLY);
    if (fd >= 0) {
        off_t sz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        if (sz > 0x21100 && sz < 1024 * 1024) {
            uint8_t *buf = (uint8_t *)malloc((size_t)sz);
            if (buf) {
                ssize_t n = read(fd, buf, (size_t)sz);
                close(fd);
                fd = -1;
                if (n == sz) {
                    replace_uxc_str(buf, (size_t)sz, 0x20ff0, 4, "2.0x");
                    // Rename "4K angle of view Priority" -> "Open Gate 3:2 Recording" & Help Guides
                    replace_uxc_str(buf, (size_t)sz, 0x2101c, 25, "Open Gate 3:2 Recording");
                    replace_uxc_str(buf, (size_t)sz, 0x2065c, 139, "Switches between Full-Frame 3:2 Open Gate (4240x2826 -> 3840x2560) and standard 16:9 4K movie recording.");
                    replace_uxc_str(buf, (size_t)sz, 0x206ec, 105, "Records Full-Frame 3:2 Open Gate (4240x2826 sensor -> 3840x2560 MP4, SAR 27:32).");
                    replace_uxc_str(buf, (size_t)sz, 0x207d4, 135, "Sets Full-Frame 3:2 Open Gate (4240x2826 -> 3840x2560, SAR 27:32) or standard 16:9 4K movie recording.");
                    replace_uxc_str(buf, (size_t)sz, 0x2087c, 112, "Records standard 16:9 4K movie format (4240x2385 sensor -> 3840x2160 MP4, SAR 1:1).");

                    // Rename "Display File Name" (unused FX3 stub at 0x587e710) -> "1993 DOOM (E1M1)" & Help Guides
                    replace_uxc_str(buf, (size_t)sz, 0x0c2c3, 17, "1993 DOOM (E1M1)");
                    replace_uxc_str(buf, (size_t)sz, 0x0c2df, 17, "1993 DOOM (E1M1)");
                    replace_uxc_str(buf, (size_t)sz, 0x0c1d8, 82, "Launches classic 1993 DOOM (E1M1: Hangar) with music & sound effects.");
                    replace_uxc_str(buf, (size_t)sz, 0x0c230, 68, "Exits DOOM overlay and returns to normal camera display.");
                    replace_uxc_str(buf, (size_t)sz, 0x0c27c, 60, "Launches classic 1993 DOOM (E1M1: Hangar) on camera screen.");

                    int ofd = open("/tmp/jiritsu/string_english_f.uxc", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (ofd >= 0) {
                        write(ofd, buf, (size_t)sz);
                        close(ofd);
                        long mrc = sys_sc5(40,
                                           (long)"/tmp/jiritsu/string_english_f.uxc",
                                           (long)"/usr/share/app/string_english_f.uxc",
                                           0, 4096 /* MS_BIND */, 0);
                        char msg[128];
                        snprintf(msg, sizeof(msg),
                                 "[MENU] Patched string_english_f.uxc (Open Gate 3:2 + DOOM), bind-mount rc=%ld\n",
                                 mrc);
                        doom_log(msg);
                    }
                }
                free(buf);
            }
        }
        if (fd >= 0) close(fd);
    }

    // Also patch the already-loaded UXC bank 0xe3 in mpTextHeap in RAM via libObj.so
    if (!g_libobj_base) g_libobj_base = find_module_base("libObj.so");
    if (g_libobj_base) {
        uintptr_t rm = 0, db = 0;
        if (safe_read_mem(g_libobj_base + 0x2d2160, &rm, 8) && rm &&
            safe_read_mem(rm + 0x20, &db, 8) && db) {
            try_patch_doom_sub_bank(db + 0x108);
            for (int lang = 1; lang <= 35; ++lang) {
                uintptr_t sub_bank = 0;
                if (safe_read_mem(db + 0x140 + (uintptr_t)(lang - 1) * 8, &sub_bank, 8) && sub_bank > 0x10000UL) {
                    try_patch_doom_sub_bank(sub_bank);
                }
            }
        }
    }
    if (g_pfd[0] >= 0) {
        close(g_pfd[0]);
        close(g_pfd[1]);
        g_pfd[0] = g_pfd[1] = -1;
    }
}

static void install_menu_hook() {
    if (!g_appfw_base) {
        g_appfw_base = find_module_base("appFw.so");
    }
    if (!g_appfw_base) return;

    g_fn_menu_get_sel = (MenuGetSelFn)(g_appfw_base + 0x1bd7148);
    // Hook unused FX3 "Display File Name" leaf (0x587e710 + 0x88 = 0x587e798) instead of De-Squeeze Display
    g_desqueeze_cb_slot = (void **)(g_appfw_base + 0x587e798);

    uintptr_t page = ((uintptr_t)g_desqueeze_cb_slot) & ~(uintptr_t)0xfffUL;
    mprotect((void *)page, 4096, PROT_READ | PROT_WRITE);

    g_orig_desqueeze_cb = (MenuLeafCbFn)(g_appfw_base + 0x1c94e34);
    *g_desqueeze_cb_slot = (void *)hooked_desqueeze_menu_cb;

    // Also restore stock De-Squeeze Display callback at 0x5860040 in case an older version hooked it
    void **old_desq_slot = (void **)(g_appfw_base + 0x5860040);
    mprotect((void *)(((uintptr_t)old_desq_slot) & ~(uintptr_t)0xfffUL), 4096, PROT_READ | PROT_WRITE);
    *old_desq_slot = (void *)(g_appfw_base + 0x1c94cf4);

    // Ensure Display File Name (0x442) value is valid (0=On, 1=Off) and its recalculation (0x2f12884) is stubbed
    uintptr_t recalc_va = (uintptr_t)(g_appfw_base + 0x2f12884);
    mprotect((void *)(recalc_va & ~(uintptr_t)0xfffUL), 4096, 7);
    *(volatile uint32_t *)recalc_va = 0xd65f03c0; // ret
    clean_dcache_range((void *)recalc_va, 4);
    __asm__ volatile("ic ivau, %0; dsb ish; isb" :: "r"(recalc_va & ~(uintptr_t)31UL) : "memory");
    if (!g_doom_visible) {
        *(volatile uint32_t *)(g_appfw_base + 0x7f88718) = 1u;
        *(volatile uint32_t *)(g_appfw_base + 0x7f88720) = 1u;
    }
    *(volatile uint8_t *)(g_appfw_base + 0x7597ea8 + 0x442 * 0x18 + 3) = 1;
    *(volatile uint8_t *)(g_appfw_base + 0x75564e0 + 0xcdc * 0x18 + 3) = 1;
    *(volatile uint8_t *)(g_appfw_base + 0x75564e0 + 0xcdd * 0x18 + 3) = 1;

    patch_desqueeze_uxc_strings();

    char buf[128];
    snprintf(buf, sizeof(buf), "[MENU] Hooked 1993 DOOM (0x442) cb slot=%p orig=%p\n",
             (void *)g_desqueeze_cb_slot, (void *)g_orig_desqueeze_cb);
    doom_log(buf);
}

static void neutralize_old_doom_instances() {
    uint64_t v6_base = find_module_base("doom_a7s3_v6.so");
    if (v6_base) *(volatile int *)(v6_base + 0x0c9cd0) = 0;
    uint64_t v7_base = find_module_base("doom_a7s3_v7.so");
    if (v7_base) *(volatile int *)(v7_base + 0x0c9e24) = 0;
    uint64_t v8_base = find_module_base("doom_a7s3_v8.so");
    if (v8_base) *(volatile int *)(v8_base + 0x0c945c) = 0;
    uint64_t v9_base = find_module_base("doom_a7s3_v9.so");
    if (v9_base) *(volatile int *)(v9_base + 0x0c945c) = 0;
    uint64_t v10_base = find_module_base("doom_a7s3_v10.so");
    if (v10_base) *(volatile int *)(v10_base + 0x0c931c) = 0;
    uint64_t v11_base = find_module_base("doom_a7s3_v11.so");
    if (v11_base) *(volatile int *)(v11_base + 0x0caafc) = 0;
    uint64_t v12_base = find_module_base("doom_a7s3_v12.so");
    if (v12_base) *(volatile int *)(v12_base + 0x0cafd4) = 0;
    uint64_t v14_base = find_module_base("doom_a7s3_v14.so");
    if (v14_base) {
        *(void **)(v14_base + 0x161b40) = nullptr;      // clear v14 g_key_cb_slot
        *(volatile int *)(v14_base + 0x0cb5b4) = 0;     // v14 g_doom_visible = 0
        *(volatile int *)(v14_base + 0x161b38) = 1;     // v14 g_quit_requested = 1
        doom_log("[Init] Neutralized lingering doom_a7s3_v14.so in RAM\n");
    }
    uint64_t v15_base = find_module_base("doom_a7s3_v15.so");
    if (v15_base) {
        *(void **)(v15_base + 0x161ea0) = nullptr;      // clear v15 g_key_cb_slot
        *(volatile int *)(v15_base + 0x0cb914) = 0;     // v15 g_doom_visible = 0
        *(volatile int *)(v15_base + 0x161e98) = 1;     // v15 g_quit_requested = 1
        doom_log("[Init] Neutralized lingering doom_a7s3_v15.so in RAM\n");
    }
    uint64_t v16_base = find_module_base("doom_a7s3_v16.so");
    if (v16_base) {
        *(void **)(v16_base + 0x161ea0) = nullptr;      // clear v16 g_key_cb_slot
        *(volatile int *)(v16_base + 0x0cb914) = 0;     // v16 g_doom_visible = 0
        *(volatile int *)(v16_base + 0x161e98) = 1;     // v16 g_quit_requested = 1
        doom_log("[Init] Neutralized lingering doom_a7s3_v16.so in RAM\n");
    }
}

extern "C" void a7s3_sound_init(void) {
    g_appfw_base = find_module_base("appFw.so");
    if (!g_appfw_base) {
        doom_log("[SFX] appFw.so not found in /proc/self/maps\n");
        return;
    }

    neutralize_old_doom_instances();

    g_fn_get_adf_table      = (GetAdfTableBaseFn)(g_appfw_base + 0x30d4488);
    g_fn_load_adf_entry     = (LoadAdfEntryFn)(g_appfw_base + 0x30dbf78);
    g_fn_adf_set_output     = (AdfSetOutputPathFn)(g_appfw_base + 0x34fbb50);
    g_fn_adf_set_volume     = (AdfSetVolumeFn)(g_appfw_base + 0x34fbb90);
    g_fn_adf_set_beep_table = (AdfSetBeepTableFn)(g_appfw_base + 0x34fbbd0);
    g_fn_adf_set_kind_vol   = (AdfSetKindVolFn)(g_appfw_base + 0x34fbc50);
    g_fn_adf_request        = (AdfSysRequestFn)(g_appfw_base + 0x34fc510);
    g_fn_adf_alloc_mem      = (AdfAllocMemFn)(g_appfw_base + 0x3508020);
    g_fn_adf_free_mem       = (AdfFreeMemFn)(g_appfw_base + 0x3508120);
    g_fn_adf_mmap           = (AdfMmapFn)(g_appfw_base + 0x3508220);
    g_fn_adf_dma_copy       = (AdfDmaCopyFn)(g_appfw_base + 0x34fff60);

    void *table_base = g_fn_get_adf_table ? g_fn_get_adf_table() : nullptr;
    if (table_base && !g_orig_adf_saved) {
        memcpy(g_orig_adf_table, table_base, sizeof(g_orig_adf_table));
        g_orig_adf_saved = true;
    }

    restore_sony_dsp_sounds();

    char buf[128];
    snprintf(buf, sizeof(buf), "[SFX] Initialized appFw.so=0x%lx table=%p\n",
             (unsigned long)g_appfw_base, table_base);
    doom_log(buf);

    if (table_base) {
        build_and_upload_doom_dsp_packs(table_base);
        if (g_doom_visible) {
            apply_doom_dsp_sounds();
            __sync_fetch_and_or(&g_pending_sfx_mask, (1u << 3));
            __sync_lock_test_and_set(&g_force_mix_now, 1);
        } else {
            restore_sony_dsp_sounds();
        }
    }

    install_menu_hook();
}

extern "C" void a7s3_sound_start(const char *name, int vol, int sep, int channel) {
    (void)sep;
    (void)channel;
    if (!name || vol <= 4 || !g_fn_adf_request || !g_doom_visible) return;

    int sfx_idx = -1;

    // Player weapons
    if (!strcmp(name, "pistol") || !strcmp(name, "sawup") || !strcmp(name, "sawful") || !strcmp(name, "sawhit")) {
        sfx_idx = 0; // DSPISTOL
    } else if (!strcmp(name, "shotgn") || !strcmp(name, "dshtgn") || !strcmp(name, "rlaunc") || !strcmp(name, "punch") || !strcmp(name, "claw")) {
        sfx_idx = 1; // DSSHOTGN
    } else if (!strcmp(name, "sgcock") || !strcmp(name, "dbopn") || !strcmp(name, "dbcls") || !strcmp(name, "dbload")) {
        sfx_idx = 2; // DSSGCOCK
    } else if (!strcmp(name, "itemup") || !strcmp(name, "wpnup") || !strcmp(name, "getpow")) {
        sfx_idx = 3; // DSITEMUP
    } else if (!strcmp(name, "doropn") || !strcmp(name, "bdopn") || !strcmp(name, "telept") || !strcmp(name, "pstart")) {
        sfx_idx = 4; // DSDOROPN
    } else if (!strcmp(name, "dorcls") || !strcmp(name, "bdcls") || !strcmp(name, "pstop")) {
        sfx_idx = 5; // DSDORCLS
    } else if (!strcmp(name, "plpain") || !strcmp(name, "pldeth") || !strcmp(name, "pdiehi") || !strcmp(name, "slop")) {
        sfx_idx = 6; // DSPLPAIN
    } else if (!strcmp(name, "swtchn") || !strcmp(name, "swtchx")) {
        sfx_idx = 7; // DSSWTCHN
    } else if (!strcmp(name, "oof") || !strcmp(name, "noway")) {
        sfx_idx = 8; // DSOOF
    } else if (!strcmp(name, "barexp") || !strcmp(name, "rxplod") || !strcmp(name, "plasma") || !strcmp(name, "bfg") || !strcmp(name, "firxpl")) {
        sfx_idx = 11; // DSBAREXP
    } else if (vol >= 20) {
        if (!strncmp(name, "podth", 5) || !strncmp(name, "bgdth", 5) ||
            !strcmp(name, "sgtdth") || !strcmp(name, "brsdth") ||
            !strcmp(name, "popain") || !strcmp(name, "dmpain")) {
            sfx_idx = 10; // DSPODTH1
        } else if (!strncmp(name, "posit", 5) || !strncmp(name, "bgsit", 5) ||
                   !strcmp(name, "sgtsit") || !strcmp(name, "brssit") ||
                   !strcmp(name, "sgtatk") || !strcmp(name, "firsht")) {
            sfx_idx = 9; // DSPOSIT1
        }
    }

    if (sfx_idx >= 0 && sfx_idx < NUM_DOOM_SFX) {
        __sync_fetch_and_or(&g_pending_sfx_mask, (1u << sfx_idx));
    }
}

static uint64_t hooked_key_cb(uint64_t x0, void *event, uint64_t x2, uint64_t x3,
                              uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7);

static void poll_draw_mode() {
    ensure_input_hook_active();

    const char *mode_path = "/tmp/jiritsu/doom_v17_mode";
    int fd = open(mode_path, O_RDONLY);
    if (fd < 0) {
        mode_path = "/tmp/jiritsu/doom_v16_mode";
        fd = open(mode_path, O_RDONLY);
    }
    if (fd < 0) {
        mode_path = "/tmp/jiritsu/doom_v15_mode";
        fd = open(mode_path, O_RDONLY);
    }
    if (fd < 0) {
        mode_path = "/tmp/jiritsu/doom_v14_mode";
        fd = open(mode_path, O_RDONLY);
    }
    if (fd >= 0) {
        char buf[16] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0 && buf[0] == 'q') {
            unlink(mode_path);
            g_quit_requested = 1;
            if (g_libobj_base && g_orig_key_cb) {
                GetSingletonFn get_input = (GetSingletonFn)(g_libobj_base + 0x0c9ac0);
                void *inst = get_input();
                uint8_t *node = nullptr;
                if (inst && safe_read_mem(&node, (const uint8_t *)inst + 0xf0, sizeof(node))) {
                    int scanned = 0;
                    while (node && scanned < 64) {
                        uint8_t *next_node = nullptr;
                        void *cb = nullptr;
                        if (!safe_read_mem(&next_node, node, sizeof(next_node)) ||
                            !safe_read_mem(&cb, node + 0x30, sizeof(cb))) break;
                        if (cb == (void *)hooked_key_cb) {
                            *(void **)(node + 0x30) = (void *)g_orig_key_cb;
                        }
                        node = next_node;
                        ++scanned;
                    }
                }
            }
            if (g_appfw_base && g_orig_key_cb) {
                uint8_t *input_mgr = nullptr;
                if (safe_read_mem(&input_mgr, (const void *)(g_appfw_base + 0x5d50300), sizeof(input_mgr)) && input_mgr) {
                    void **mgr_cb_slot = (void **)(input_mgr + 0x18);
                    if (*mgr_cb_slot == (void *)hooked_key_cb) {
                        *mgr_cb_slot = (void *)g_orig_key_cb;
                    }
                }
            }
            if (g_desqueeze_cb_slot && *g_desqueeze_cb_slot == (void *)hooked_desqueeze_menu_cb && g_orig_desqueeze_cb) {
                *g_desqueeze_cb_slot = (void *)g_orig_desqueeze_cb;
            }
            set_doom_visible(0);
            doom_log("[Mode] Received 'q' -> unhooked & exiting thread\n");
            pthread_exit(nullptr);
        }
        if (n > 0 && buf[0] == 'v') {
            unlink(mode_path);
            set_doom_visible(1);
            return;
        }
        if (n > 0 && buf[0] == 'h') {
            unlink(mode_path);
            set_doom_visible(0);
            return;
        }
        if (n > 0 && buf[0] >= '0' && buf[0] <= '9') {
            unlink(mode_path);
            int new_mode = buf[0] - '0';
            if (new_mode != g_draw_mode) {
                int old_mode = g_draw_mode;
                g_draw_mode = new_mode;
                if (old_mode >= 1 && new_mode == 0) {
                    restore_fbrm_swap_suppress();
                }
                char msg[96];
                snprintf(msg, sizeof(msg), "[Mode] Switched draw_mode %d -> %d\n", old_mode, new_mode);
                doom_log(msg);
            }
        }
    }

    // Direct RAM check on 0x442 ("1993 DOOM (E1M1)") menu state so menu launch never depends on file I/O
    if (!g_doom_visible && g_appfw_base) {
        if (*(volatile uint32_t *)(g_appfw_base + 0x7f88718) == 0u) {
            doom_log("[Mode] Detected 0x442 menu state == 0 (On) in RAM -> launching DOOM\n");
            set_doom_visible(1);
        }
    }
}

static void enqueue_key(unsigned char key, int pressed) {
    uint32_t next = (g_key_head + 1) % KEY_QUEUE_SIZE;
    if (next != g_key_tail) {
        g_key_queue[g_key_head].key = key;
        g_key_queue[g_key_head].pressed = pressed;
        __sync_synchronize();
        g_key_head = next;
    }
}

static uint64_t hooked_key_cb(uint64_t x0, void *event, uint64_t x2, uint64_t x3,
                              uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7) {
    if (!event || !g_doom_visible || g_draw_mode == 0) {
        return g_orig_key_cb ? g_orig_key_cb(x0, event, x2, x3, x4, x5, x6, x7) : 0;
    }
    uint32_t ev_code = *(const uint32_t *)event;
    int32_t status = *(const int32_t *)((const char *)event + 8);

    g_last_ev_code = ev_code;
    g_last_ev_status = status;

    uint32_t log_idx = __sync_fetch_and_add(&g_key_log_count, 1);
    if (log_idx < 32) {
        char buf[128];
        int len = snprintf(buf, sizeof(buf), "[KEY] code=0x%08x status=%d vis=%d mode=%d\n",
                           ev_code, status, g_doom_visible, g_draw_mode);
        if (len > 0) doom_log_raw(buf, (size_t)len);
    }

    // While inside DOOM, pressing C3 / Delete ((ev_code & 0xfff) == 0x114) exits DOOM back to camera
    if ((ev_code & 0xfffu) == 0x114u) {
        if (status == 1) {
            set_doom_visible(0);
        }
        return 0;
    }

    // Dial / wheel rotation events
    if ((ev_code & 0xffff0000u) == 0x10000000u) {
        uint32_t now = DG_GetTicksMs();
        if (status < 0) {
            if (!g_dial_release_left_ms) {
                enqueue_key(KEY_LEFTARROW, 1);
                enqueue_key('[', 1);
            }
            g_dial_release_left_ms = now + 70;
        } else if (status > 0) {
            if (!g_dial_release_right_ms) {
                enqueue_key(KEY_RIGHTARROW, 1);
                enqueue_key(']', 1);
            }
            g_dial_release_right_ms = now + 70;
        }
        return 0;
    }

    int pressed = (status == 1 || status == 2) ? 1 : 0;
    uint32_t low = ev_code & 0xfffu;

    if (ev_code == 0x00000003u || low == 0x003u) {
        enqueue_key(KEY_UPARROW, pressed);
        return 0;
    } else if (ev_code == 0x00000030u || low == 0x030u) {
        enqueue_key(KEY_DOWNARROW, pressed);
        return 0;
    } else if (ev_code == 0x00000015u || low == 0x015u) {
        enqueue_key(KEY_LEFTARROW, pressed);
        return 0;
    } else if (ev_code == 0x000001afu || low == 0x1afu) {
        enqueue_key(KEY_RIGHTARROW, pressed);
        return 0;
    } else if (ev_code == 0x000001acu || low == 0x1acu) {
        enqueue_key(KEY_ENTER, pressed);
        enqueue_key(KEY_USE, pressed);
        return 0;
    } else if (ev_code == 0x000001abu || low == 0x1abu) {
        enqueue_key(KEY_ESCAPE, pressed);
        return 0;
    } else if (low == 0x110u || low == 0x10eu) {
        enqueue_key(KEY_TAB, pressed);
        return 0;
    } else if (low == 0x1f8u || low == 0x001u || low == 0x002u ||
               (low >= 0x1f0u && low <= 0x1ffu) ||
               low == 0x112u || low == 0x10cu || low == 0x10du) {
        enqueue_key(KEY_FIRE, pressed);
        return 0;
    }

    // Swallow all other camera events while DOOM is visible so nothing leaks to the Sony Menu behind DOOM!
    return 0;
}

static void install_input_hook() {
    if (!g_libobj_base) return;
    if (!g_appfw_base) {
        g_appfw_base = find_module_base("appFw.so");
    }
    if (g_appfw_base) {
        g_orig_key_cb = (KeyCallbackFn)(g_appfw_base + 0x17f5c38);
        uint8_t *input_mgr = nullptr;
        if (safe_read_mem(&input_mgr, (const void *)(g_appfw_base + 0x5d50300), sizeof(input_mgr)) && input_mgr) {
            void **mgr_cb_slot = (void **)(input_mgr + 0x18);
            if (*mgr_cb_slot != (void *)hooked_key_cb) {
                *mgr_cb_slot = (void *)hooked_key_cb;
            }
        }
    }
    GetSingletonFn get_input = (GetSingletonFn)(g_libobj_base + 0x0c9ac0);
    void *inst = get_input();
    if (!inst) {
        doom_log("[ERR] InfraInput::GetInstance() returned null\n");
        return;
    }
    uint8_t *node = nullptr;
    if (!safe_read_mem(&node, (const uint8_t *)inst + 0xf0, sizeof(node)) || !node) {
        doom_log("[ERR] InfraInput node head (+0xf0) is null or unreadable\n");
        return;
    }
    int scanned = 0;
    int hooked_cnt = 0;
    while (node && scanned < 64) {
        uint32_t type = 0;
        void *cb = nullptr;
        uint8_t *next_node = nullptr;
        if (!safe_read_mem(&next_node, node, sizeof(next_node)) ||
            !safe_read_mem(&type, node + 0x24, sizeof(type)) ||
            !safe_read_mem(&cb, node + 0x30, sizeof(cb))) {
            doom_log("[ERR] InfraInput node unreadable during walk\n");
            break;
        }
        if (cb != nullptr && (!g_appfw_base || cb != (void *)(g_appfw_base + 0x3566e7c))) {
            g_key_cb_slot = (void **)(node + 0x30);
            if (!g_orig_key_cb && cb != (void *)hooked_key_cb) {
                g_orig_key_cb = (KeyCallbackFn)cb;
            }
            if (*g_key_cb_slot != (void *)hooked_key_cb) {
                *g_key_cb_slot = (void *)hooked_key_cb;
                char buf[128];
                snprintf(buf, sizeof(buf), "[OK] Hooked InfraInput node=%p (id=0x%x) prev_cb=%p orig_cb=%p\n",
                         (void *)node, type, cb, (void *)g_orig_key_cb);
                doom_log(buf);
            }
            ++hooked_cnt;
        }
        node = next_node;
        ++scanned;
    }
    if (hooked_cnt == 0) {
        doom_log("[ERR] Could not find callback node in InfraInput\n");
    }
}

static void ensure_input_hook_active() {
    if (!g_libobj_base) return;
    if (g_appfw_base) {
        uint8_t *input_mgr = nullptr;
        if (safe_read_mem(&input_mgr, (const void *)(g_appfw_base + 0x5d50300), sizeof(input_mgr)) && input_mgr) {
            void **mgr_cb_slot = (void **)(input_mgr + 0x18);
            if (*mgr_cb_slot != (void *)hooked_key_cb) {
                *mgr_cb_slot = (void *)hooked_key_cb;
            }
        }
    }
    GetSingletonFn get_input = (GetSingletonFn)(g_libobj_base + 0x0c9ac0);
    void *inst = get_input();
    if (!inst) return;
    uint8_t *node = nullptr;
    if (!safe_read_mem(&node, (const uint8_t *)inst + 0xf0, sizeof(node)) || !node) return;
    int scanned = 0;
    while (node && scanned < 64) {
        uint32_t type = 0;
        void *cb = nullptr;
        uint8_t *next_node = nullptr;
        if (!safe_read_mem(&next_node, node, sizeof(next_node)) ||
            !safe_read_mem(&type, node + 0x24, sizeof(type)) ||
            !safe_read_mem(&cb, node + 0x30, sizeof(cb))) {
            break;
        }
        if (cb != nullptr && cb != (void *)hooked_key_cb &&
            (!g_appfw_base || cb != (void *)(g_appfw_base + 0x3566e7c))) {
            *(void **)(node + 0x30) = (void *)hooked_key_cb;
            g_key_cb_slot = (void **)(node + 0x30);
            char buf[128];
            snprintf(buf, sizeof(buf), "[KEY] Re-hooked InfraInput node=%p (id=0x%x) prev_cb=%p\n",
                     (void *)node, type, cb);
            doom_log(buf);
        }
        node = next_node;
        ++scanned;
    }
}

extern "C" void DG_Init() {
    doom_log("[DG_Init] Classic 1993 DOOM v12 (Menu + C3 + Real-Time Polyphonic D_E1M1 Music + SFX Mixer)\n");
    key_menu_confirm = KEY_ENTER;
    key_menu_abort = KEY_ESCAPE;
    key_prevweapon = '[';
    key_nextweapon = ']';

    poll_draw_mode();

    g_libobj_base = find_module_base("libObj.so");
    if (g_libobj_base) {
        g_fn_get_fbrm          = (GetSingletonFn)(g_libobj_base + 0x0f38b4);
        g_fn_fbrm_lock         = (FbrmLockFn)(g_libobj_base + 0x0f3ba8);
        g_fn_fbrm_unlock       = (FbrmLockFn)(g_libobj_base + 0x0f3bb4);
        g_fn_fbrm_set_freeze   = (FbrmSetBoolFn)(g_libobj_base + 0x0f3c74);
        g_fn_fbrm_begin_update = (FbrmSetBoolFn)(g_libobj_base + 0x0f3cb0);
        g_fn_fbrm_swap         = (FbrmSwapFn)(g_libobj_base + 0x0f3e14);

        char buf[128];
        snprintf(buf, sizeof(buf), "[DG_Init] libObj.so base=0x%lx mode=%d vis=%d\n",
                 (unsigned long)g_libobj_base, g_draw_mode, g_doom_visible);
        doom_log(buf);
        install_input_hook();
    } else {
        doom_log("[ERR] Failed to locate libObj.so in /proc/self/maps\n");
    }
    install_menu_hook();
}

extern "C" void DG_DrawFrame() {
    uint32_t fc = __sync_fetch_and_add(&g_frame_count, 1);
    if ((fc & 15u) == 0) {
        poll_draw_mode();
    }

    if (!g_doom_visible || !DG_ScreenBuffer) return;

    static const uint8_t kBayer2x2[2][2] = {
        { 0, 8 },
        { 12, 4 }
    };

    const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;
    for (int y = 0; y < 480; ++y) {
        int sy = (y * 400) / 480;
        const uint32_t *src_row = src + sy * 640;
        uint16_t *dst_row = g_osd_frame + y * 640;
        const uint8_t *bayer_row = kBayer2x2[y & 1];
        for (int x = 0; x < 640; ++x) {
            uint32_t p = src_row[x];
            uint32_t d = bayer_row[x & 1];
            uint32_t r = ((p >> 16) & 0xFFu) + d;
            uint32_t g = ((p >> 8) & 0xFFu) + d;
            uint32_t b = (p & 0xFFu) + d;
            if (r > 255u) r = 255u;
            if (g > 255u) g = 255u;
            if (b > 255u) b = 255u;
            dst_row[x] = (uint16_t)(((r & 0xF0u) << 8) |
                                    ((g & 0xF0u) << 4) |
                                    (b & 0xF0u) |
                                    0x000Fu);
        }
    }

    int mode = g_draw_mode;
    if (mode >= 1 && g_fn_get_fbrm && g_fn_fbrm_lock && g_fn_fbrm_unlock && g_fn_fbrm_set_freeze) {
        void *fbrm = g_fn_get_fbrm();
        if (fbrm) {
            g_fn_fbrm_lock(fbrm);
            *((volatile uint8_t *)fbrm + 0x60) = 0;
            g_fn_fbrm_set_freeze(fbrm, 1);

            void *screen = *(void **)((char *)fbrm + 0x08);
            if (screen) {
                void *entry_on = *(void **)((char *)screen + 0x68);
                void *on_bmp = entry_on ? *(void **)entry_on : nullptr;
                void *on_pix = on_bmp ? *(void **)((char *)on_bmp + 0x10) : nullptr;
                if (on_pix) {
                    memcpy(on_pix, g_osd_frame, sizeof(g_osd_frame));
                    clean_dcache_range(on_pix, sizeof(g_osd_frame));
                }
            }

            if (mode >= 2 && g_fn_fbrm_begin_update && g_fn_fbrm_swap) {
                g_fn_fbrm_begin_update(fbrm, 1);
                g_fn_fbrm_swap(fbrm, 1, nullptr);
            } else {
                g_fn_fbrm_unlock(fbrm);
            }
        }
    }

    if (fc == 0 || fc == 35 || (fc % 350 == 0)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "[DG_DrawFrame] frame=%u mode=%d vis=%d\n", fc, mode, g_doom_visible);
        doom_log(buf);
    }
}

extern "C" void DG_SleepMs(uint32_t ms) {
    usleep(ms * 1000u);
}

extern "C" uint32_t DG_GetTicksMs() {
    struct timeval tp;
    gettimeofday(&tp, nullptr);
    static uint64_t s_base_ms = 0;
    uint64_t now_ms = (uint64_t)tp.tv_sec * 1000ULL + (uint64_t)(tp.tv_usec / 1000);
    if (!s_base_ms) s_base_ms = now_ms;
    return (uint32_t)(now_ms - s_base_ms);
}

extern "C" int DG_GetKey(int *pressed, unsigned char *doomKey) {
    uint32_t now = DG_GetTicksMs();
    if (g_dial_release_left_ms && now >= g_dial_release_left_ms) {
        g_dial_release_left_ms = 0;
        enqueue_key(KEY_LEFTARROW, 0);
        enqueue_key('[', 0);
    }
    if (g_dial_release_right_ms && now >= g_dial_release_right_ms) {
        g_dial_release_right_ms = 0;
        enqueue_key(KEY_RIGHTARROW, 0);
        enqueue_key(']', 0);
    }

    if (g_key_tail == g_key_head) {
        return 0;
    }
    *doomKey = g_key_queue[g_key_tail].key;
    *pressed = g_key_queue[g_key_tail].pressed;
    __sync_synchronize();
    g_key_tail = (g_key_tail + 1) % KEY_QUEUE_SIZE;
    return 1;
}

extern "C" void DG_SetWindowTitle(const char *title) {
    if (title) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[Title] %s\n", title);
        doom_log(buf);
    }
}

extern "C" void a7s3_doom_hide_overlay(void) {
    doom_log("[I_Quit] Hiding DOOM & restoring Sony camera sounds (launch via Menu -> De-Squeeze Display -> DOOM or C3)\n");
    set_doom_visible(0);
}

extern "C" void a7s3_doom_exit(int code) {
    char buf[96];
    snprintf(buf, sizeof(buf), "[a7s3_doom_exit] code=%d -> hiding overlay & stopping thread\n", code);
    doom_log(buf);
    set_doom_visible(0);
    pthread_exit(nullptr);
}

static void *doom_thread_main(void *arg) {
    (void)arg;
    doom_log("[Thread] Starting DOOM v12 worker thread in 0.2s...\n");
    usleep(200 * 1000);
    doom_log("[Thread] Calling doomgeneric_Create(4, argv)...\n");

    static char arg0[] = "doom";
    static char arg1[] = "-iwad";
    static char arg2[] = "/tmp/jiritsu/doom1.wad";
    static char arg3[] = "-nogui";
    static char *argv[] = { arg0, arg1, arg2, arg3, nullptr };

    doomgeneric_Create(4, argv);
    // Run 4 initial ticks so E1M1: Hangar is set up and ready to display immediately when launched
    for (int i = 0; i < 4; ++i) {
        doomgeneric_Tick();
    }
    doom_log("[Standby] DOOM v17 ready! Launch from MENU -> Setup -> Display Option -> 1993 DOOM (E1M1)\n");

    while (!g_quit_requested) {
        while (!g_doom_visible && !g_quit_requested) {
            poll_draw_mode();
            usleep(40 * 1000);
        }
        if (g_quit_requested) break;
        doomgeneric_Tick();
    }
    return nullptr;
}

static int g_started = 0;

extern "C" __attribute__((visibility("default"))) void _init(void) {
    if (__sync_lock_test_and_set(&g_started, 1)) return;
    doom_log("[_init] doom_a7s3_v17.so loaded into dicam\n");
    pthread_t tid, mix_tid;
    pthread_create(&tid, nullptr, doom_thread_main, nullptr);
    pthread_detach(tid);
    pthread_create(&mix_tid, nullptr, audio_mixer_thread_main, nullptr);
    pthread_detach(mix_tid);
}
