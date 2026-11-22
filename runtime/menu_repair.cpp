#include <stdint.h>

static char maps[131072];
static uint8_t uxc_buf[163840];

static long sc5(long n, long a, long b, long c, long d, long e) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory", "cc");
    return x0;
}
static long sc4(long n, long a, long b, long c, long d) {
    return sc5(n, a, b, c, d, 0);
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
static uint64_t hex_address_end(const char *s) {
    while (*s && *s != '-') ++s;
    if (*s != '-') return 0;
    ++s;
    uint64_t value = 0;
    while (*s && *s != ' ') {
        uint32_t d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else return 0;
        value = (value << 4) | d;
        ++s;
    }
    return value;
}
static uint64_t find_mod_base(const char *mod_needle) {
    if (read_file("/proc/self/maps", maps, sizeof(maps)) <= 0) return 0;
    char *line = maps;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') ++end;
        char saved = *end;
        *end = 0;
        int match = contains(line, mod_needle) && contains(line, "r-xp");
        uint64_t base = match ? hex_address(line) : 0;
        *end = saved;
        if (base) return base;
        if (!*end) break;
        line = end + 1;
    }
    return 0;
}
static uint64_t appfw_base() {
    return find_mod_base("/usr/lib/appFw.so");
}

static int stock_fd = -1;
static int restore_bytes(uint64_t base, uint32_t va, uint32_t file_offset, uint32_t size, int prot) {
    uint8_t stock[1024];
    if (size > sizeof(stock) || stock_fd < 0) return -1;
    if (sc4(67, stock_fd, (long)stock, size, file_offset) != size) return -2;
    uint64_t first = base + va;
    uint64_t last = first + size - 1;
    for (uint64_t page = first & ~0xfffUL; page <= (last & ~0xfffUL); page += 0x1000)
        if (sc4(226, page, 0x1000, prot, 0) != 0) return -3;
    for (uint32_t i = 0; i < size; ++i) *reinterpret_cast<uint8_t *>(first + i) = stock[i];
    return 0;
}
static void writable(uint64_t base, uint32_t va, int prot) {
    sc4(226, (base + va) & ~0xfffUL, 0x2000, prot, 0);
}
static void fill_modes(uint64_t base, uint32_t va) {
    for (int i = 0; i < 20; ++i) *reinterpret_cast<uint16_t *>(base + va + i * 2) = 1;
}
static void flush_icache(void *begin, void *end) {
    for (uintptr_t a = (uintptr_t)begin & ~31UL; a < (uintptr_t)end; a += 32)
        __asm__ volatile("dc cvau, %0" :: "r"(a));
    __asm__ volatile("dsb ish" ::: "memory");
    for (uintptr_t a = (uintptr_t)begin & ~31UL; a < (uintptr_t)end; a += 32)
        __asm__ volatile("ic ivau, %0" :: "r"(a));
    __asm__ volatile("dsb ish; isb" ::: "memory");
}
static uint8_t *cache_entry(uint64_t base, uint32_t cache, uint32_t id) {
    uint8_t *entry = reinterpret_cast<uint8_t *>(base + cache + id * 0x18);
    entry[0] = 1;
    entry[1] = 0;
    entry[2] = 1;
    entry[3] = 1;
    entry[0x0c] = 1;
    return entry;
}
static void leaf_cache_entry(uint64_t base, uint32_t id, uint64_t options) {
    uint8_t *entry = cache_entry(base, 0x7597ea8, id);
    entry[4] = 1;
    *reinterpret_cast<uint64_t *>(entry + 0x10) = options;
}
static void unlock_cap_table(uint64_t base, uint32_t cap_va, uint32_t count, uint32_t bss_va) {
    writable(base, cap_va, 3);
    uint64_t target = base + bss_va + count;
    *reinterpret_cast<volatile uint64_t *>(base + cap_va) = target;
    for (uint32_t i = 1; i <= count; ++i) {
        *reinterpret_cast<volatile uint64_t *>(base + cap_va + 8 + i * 8) = target;
    }
    for (uint32_t i = 0; i <= count; ++i) {
        *reinterpret_cast<volatile uint8_t *>(base + bss_va + i) = 1;
    }
}
static void replace_uxc_buf_at(uint8_t *buf, uint32_t off, uint32_t max_len, const char *str, int do_mprotect) {
    if (do_mprotect) {
        uint64_t p1 = ((uint64_t)(buf + off)) & ~0xfffUL;
        uint64_t p2 = ((uint64_t)(buf + off + max_len)) & ~0xfffUL;
        sc4(226, (long)p1, 0x1000, 3, 0);
        if (p2 != p1) sc4(226, (long)p2, 0x1000, 3, 0);
    }
    for (uint32_t i = 0; i < max_len; ++i) buf[off + i] = 0;
    for (uint32_t i = 0; str[i] && i < max_len; ++i) buf[off + i] = (uint8_t)str[i];
}
static void apply_uxc_patches_to_buf(uint8_t *buf, int do_mprotect) {
    if (!buf) return;
    if (buf[0] != 0x75 || buf[1] != 0x78 || buf[2] != 0x63 || buf[3] != 0x07) return;
    if (buf[10] != 0xe3 || buf[11] != 0x00) return;
    uint8_t lang = buf[8];
    if (lang == 0x08 && buf[0x0c2bc] == 0x34 && buf[0x0c2bd] == 0x65) {
        if (do_mprotect) {
            sc4(226, (long)(((uint64_t)(buf + 0x0c100)) & ~0xfffUL), 0x2000, 3, 0);
            sc4(226, (long)(((uint64_t)(buf + 0x20600)) & ~0xfffUL), 0x2000, 3, 0);
        }
        buf[0x0c2bc + 4] = 0xee; buf[0x0c2bc + 5] = 0x99; buf[0x0c2bc + 6] = 0xa0;
        buf[0x0c2d8 + 4] = 0xee; buf[0x0c2d8 + 5] = 0x82; buf[0x0c2d8 + 6] = 0x8b;
        replace_uxc_buf_at(buf, 0x20ff0, 4, "2.0x", do_mprotect);
        replace_uxc_buf_at(buf, 0x2101c, 25, "Open Gate 3:2 Recording", do_mprotect);
        replace_uxc_buf_at(buf, 0x2065c, 139, "Switches between Full-Frame 3:2 Open Gate (4240x2826 -> 3840x2560) and standard 16:9 4K movie recording.", do_mprotect);
        replace_uxc_buf_at(buf, 0x206ec, 105, "Records Full-Frame 3:2 Open Gate (4240x2826 sensor -> 3840x2560 MP4, SAR 27:32).", do_mprotect);
        replace_uxc_buf_at(buf, 0x207d4, 135, "Sets Full-Frame 3:2 Open Gate (4240x2826 -> 3840x2560, SAR 27:32) or standard 16:9 4K movie recording.", do_mprotect);
        replace_uxc_buf_at(buf, 0x2087c, 112, "Records standard 16:9 4K movie format (4240x2385 sensor -> 3840x2160 MP4, SAR 1:1).", do_mprotect);
        replace_uxc_buf_at(buf, 0x0c2c3, 17, "1993 DOOM (E1M1)", do_mprotect);
        replace_uxc_buf_at(buf, 0x0c2df, 17, "1993 DOOM (E1M1)", do_mprotect);
        replace_uxc_buf_at(buf, 0x0c1d8, 82, "Launches classic 1993 DOOM (E1M1: Hangar) with music & sound effects.", do_mprotect);
        replace_uxc_buf_at(buf, 0x0c230, 68, "Exits DOOM overlay and returns to normal camera display.", do_mprotect);
        replace_uxc_buf_at(buf, 0x0c27c, 60, "Launches classic 1993 DOOM (E1M1: Hangar) on camera screen.", do_mprotect);
    } else if (lang == 0x0f && buf[0x15a38] == 0x34 && buf[0x15a39] == 0x65) {
        replace_uxc_buf_at(buf, 0x15a3f, 28, "1993 DOOM (E1M1)", do_mprotect);
        replace_uxc_buf_at(buf, 0x15a63, 28, "1993 DOOM (E1M1)", do_mprotect);
        replace_uxc_buf_at(buf, 0x3b750, 34, "Open Gate 3:2 Recording", do_mprotect);
    }
}
static int g_pfd[2] = {-1, -1};
static int safe_read_mem(uint64_t addr, void *out, uint32_t len) {
    if (addr < 0x10000UL) return 0;
    if (g_pfd[0] < 0) {
        if (sc4(59, (long)g_pfd, 0, 0, 0) != 0) return 0;
    }
    long w = sc4(64, g_pfd[1], (long)addr, len, 0);
    if (w != (long)len) return 0;
    return sc4(63, g_pfd[0], (long)out, len, 0) == (long)len;
}
static void try_patch_uxc_cand(uint64_t cand) {
    uint8_t hdr[16];
    if (safe_read_mem(cand, hdr, 16) &&
        hdr[0] == 0x75 && hdr[1] == 0x78 && hdr[2] == 0x63 && hdr[3] == 0x07 &&
        hdr[10] == 0xe3 && hdr[11] == 0x00 &&
        safe_read_mem(cand + 0x21100, hdr, 16)) {
        apply_uxc_patches_to_buf(reinterpret_cast<uint8_t *>(cand), 1);
    }
}
static void try_patch_bank_obj(uint64_t bank_obj) {
    uint64_t ptr18 = 0;
    if (!safe_read_mem(bank_obj + 0x18, &ptr18, 8) || ptr18 < 0x10008UL) return;
    try_patch_uxc_cand(ptr18 - 8);
    try_patch_uxc_cand(ptr18);
}
static void try_patch_sub_bank(uint64_t sub_bank) {
    if (sub_bank < 0x10000UL) return;
    uint16_t max_banks = 0;
    uint64_t slots = 0, lru = 0, fc = 0;
    if (safe_read_mem(sub_bank + 0x02, &max_banks, 2) &&
        safe_read_mem(sub_bank + 0x10, &slots, 8) && slots > 0x10000UL) {
        uint32_t lim = max_banks < 1024 ? max_banks : 1024;
        for (uint32_t i = 0; i < lim; ++i) {
            uint64_t bank_obj = 0;
            if (safe_read_mem(slots + (uint64_t)i * 16, &bank_obj, 8) && bank_obj > 0x10000UL) {
                try_patch_bank_obj(bank_obj);
            }
        }
    }
    if (safe_read_mem(sub_bank + 0x18, &lru, 8)) {
        uint64_t node = lru;
        for (int step = 0; node > 0x10000UL && step < 256; ++step) {
            try_patch_bank_obj(node);
            uint64_t next = 0;
            if (!safe_read_mem(node, &next, 8)) break;
            node = next;
        }
    }
    if (safe_read_mem(sub_bank + 0x08, &fc, 8) && fc > 0x10000UL) {
        uint32_t cnt = 0;
        if (safe_read_mem(fc + 0x960, &cnt, 4)) {
            uint32_t lim = cnt < 256 ? cnt : 256;
            for (uint32_t i = 0; i < lim; ++i) {
                uint64_t uxc_ptr = 0;
                if (safe_read_mem(fc + 0x968 + (uint64_t)i * 0x18, &uxc_ptr, 8) && uxc_ptr > 0x10000UL) {
                    try_patch_uxc_cand(uxc_ptr);
                }
            }
        }
    }
}
static void patch_uxc_in_ram() {
    uint64_t libobj = find_mod_base("/usr/lib/libObj.so");
    if (!libobj) return;
    uint64_t rm = 0, db = 0;
    if (safe_read_mem(libobj + 0x2d2160, &rm, 8) && rm &&
        safe_read_mem(rm + 0x20, &db, 8) && db) {
        try_patch_sub_bank(db + 0x108);
        for (int lang = 1; lang <= 35; ++lang) {
            uint64_t sub_bank = 0;
            if (safe_read_mem(db + 0x140 + (uint64_t)(lang - 1) * 8, &sub_bank, 8) && sub_bank > 0x10000UL) {
                try_patch_sub_bank(sub_bank);
            }
        }
    }
    if (g_pfd[0] >= 0) {
        sc4(57, g_pfd[0], 0, 0, 0);
        sc4(57, g_pfd[1], 0, 0, 0);
        g_pfd[0] = g_pfd[1] = -1;
    }
}
static void patch_uxc_strings() {
    for (int i = 0; i < 64; ++i) {
        if (sc4(39, (long)"/usr/share/app/string_english_f.uxc", 2 /* MNT_DETACH */, 0, 0) != 0) break;
    }
    long fd = sc4(56, -100, (long)"/usr/share/app/string_english_f.uxc", 0, 0);
    if (fd >= 0) {
        long used = 0;
        while (used < (long)sizeof(uxc_buf)) {
            long got = sc4(63, fd, (long)(uxc_buf + used), sizeof(uxc_buf) - used, 0);
            if (got <= 0) break;
            used += got;
        }
        sc4(57, fd, 0, 0, 0);
        if (used >= 0x21100) {
            apply_uxc_patches_to_buf(uxc_buf, 0);
            long ofd = sc4(56, -100, (long)"/tmp/jiritsu/string_english_f.uxc", 1 | 64 | 512, 0644);
            if (ofd >= 0) {
                sc4(64, ofd, (long)uxc_buf, used, 0);
                sc4(57, ofd, 0, 0, 0);
                sc5(40, (long)"/tmp/jiritsu/string_english_f.uxc", (long)"/usr/share/app/string_english_f.uxc", 0, 4096 /* MS_BIND */, 0);
            }
        }
    }
    patch_uxc_in_ram();
}
static void copy14(uint64_t destination, uint64_t source) {
    for (int i = 0; i < 14; ++i)
        *reinterpret_cast<volatile uint8_t *>(destination + i) = *reinterpret_cast<volatile uint8_t *>(source + i);
}
static void stage(char code) {
    char message[] = "STAGE 0\n";
    message[6] = code;
    long fd = sc4(56, -100, (long)"/tmp/jiritsu/menu-stage.txt", 1 | 64 | 512, 0644);
    if (fd >= 0) { sc4(64, fd, (long)message, sizeof(message) - 1, 0); sc4(57, fd, 0, 0, 0); }
}
static void patch_custom_tree(uint64_t base, uint64_t node, int depth) {
    if (!node || depth > 48 || node < 0x10000UL) return;
    uint32_t key = *reinterpret_cast<volatile uint32_t *>(node + 0x20);
    /* Tree at appFw + 0x75ab168 is keyed by intId:
     *   0x501 = Var. Shutter Select (rawId 0x517)
     *   0x157 = Var. Shutter Set.   (rawId 0x15a)
     *   0x574 = Home / BIG6         (rawId 0x58a)
     * NOTE: Do NOT unlock 0x502 (Anti-flicker Tv Scan); keeping 0x502 zero ensures
     * Var. Shutter Set. opens the manual frequency screen (scr0061_0101). */
    if (key == 0x501 || key == 0x157 || key == 0x574) {
        uint64_t vec_beg = *reinterpret_cast<volatile uint64_t *>(node + 0x28);
        uint64_t vec_end = *reinterpret_cast<volatile uint64_t *>(node + 0x30);
        if (vec_beg > 0x10000UL && vec_end >= vec_beg && (vec_end - vec_beg) == 280UL) {
            copy14(vec_beg + 0x54, key == 0x574 ? (base + 0x3f0ed6c) : (vec_beg + 0x46));
        }
    }
    patch_custom_tree(base, *reinterpret_cast<volatile uint64_t *>(node + 0x10), depth + 1);
    patch_custom_tree(base, *reinterpret_cast<volatile uint64_t *>(node + 0x18), depth + 1);
}

extern "C" __attribute__((visibility("default"))) void _init(void) {
    uint64_t base = appfw_base();
    int result = 0;
    if (!base) result = -10;
    if (!result) {
        stock_fd = sc4(56, -100, (long)"/usr/lib/appFw.so", 0, 0);
        if (stock_fd < 0) result = -11;
    }
    if (!result) {
        result = restore_bytes(base, 0x1bd8b4c, 0x1bd8b4c, 36, 7);
        if (!result) result = restore_bytes(base, 0x1bddb2c, 0x1bddb2c, 48, 7);
        if (!result) result = restore_bytes(base, 0x1bc6a20, 0x1bc6a20, 4, 7);
        if (!result) result = restore_bytes(base, 0x355ee60, 0x355ee60, 4, 7);
        if (!result) result = restore_bytes(base, 0x355eed0, 0x355eed0, 4, 7);
        if (!result) result = restore_bytes(base, 0x355ef84, 0x355ef84, 12, 7);
        if (!result) result = restore_bytes(base, 0x355f078, 0x355f078, 4, 7);
        if (!result) result = restore_bytes(base, 0x355f304, 0x355f304, 4, 7);
        if (!result) result = restore_bytes(base, 0x351b1a4, 0x351b1a4, 4, 7);
        flush_icache(reinterpret_cast<void *>(base + 0x1bc6a20),
                     reinterpret_cast<void *>(base + 0x1bde000));
        flush_icache(reinterpret_cast<void *>(base + 0x351b1a4),
                     reinterpret_cast<void *>(base + 0x351b1a8));
        flush_icache(reinterpret_cast<void *>(base + 0x355ee60),
                     reinterpret_cast<void *>(base + 0x355f308));
        const uint32_t records[] = {0x5892ef8, 0x58a4cb0};
        for (uint32_t record : records) {
            result = restore_bytes(base, record + 0x0e, record - 0x10000 + 0x0e, 30, 3);
            if (result) break;
            result = restore_bytes(base, record + 0x50, record - 0x10000 + 0x50, 8, 3);
            if (result) break;
            writable(base, record, 3);
            *reinterpret_cast<volatile uint32_t *>(base + record + 0x28) = 0x00010101;
            fill_modes(base, record + 0x2c);
            *reinterpret_cast<uint64_t *>(base + record + 0x70) = base + 0x1c5dd98;
            *reinterpret_cast<uint64_t *>(base + record + 0xa8) = base + 0x1bdf304;
        }
        const uint32_t options[] = {
            0x3fb0338, 0x3fb03b8,
            0x3fa8838, 0x3fa88b8,
            0x3f66038, 0x3f660b8, 0x3f66138,
            0x3f83ab8,
            0x3fe5138, 0x3fe51b8, 0x3fe5238, 0x3fe52b8, 0x3fe5338,
            0x3fe53b8, 0x3fe5438, 0x3fe54b8, 0x3fe5538, 0x3fe55b8,
            0x3fec438, 0x3fec4b8, 0x3fec338, 0x3fec3b8, 0x3fec238, 0x3fec2b8,
            0x3fec138, 0x3fec1b8, 0x3fec038, 0x3fec0b8, 0x3ff0c38, 0x3ff0cb8,
            // Log Shooting (Off, Flexible ISO, Cine EI Quick, Cine EI)
            0x3fc7438, 0x3fc74b8, 0x3fc7538, 0x3fc75b8, 0x3fc7638,
            // Base ISO (ISO 800 & ISO 12800) + Base ISO Switch EI
            0x3f4bc38, 0x3f4bdb8,
            0x3f85538, 0x3f855b8, 0x3f85638, 0x3f856b8, 0x3f85738, 0x3f857b8, 0x3f85838,
            // Still Log Shooting & Color Gamut
            0x3fe6bb8, 0x3fe6c38, 0x3fd4d38, 0x3fd4db8, 0x3fd4e38,
            // HDMI RAW Output Setting (all frame rates including 24.00p)
            0x3fd3238, 0x3fd32b8, 0x3fd3338, 0x3fd33b8, 0x3fd3438,
            0x3fd34b8, 0x3fd3538, 0x3fd35b8, 0x3fd3638,
            // Embed LUT File (On / Off)
            0x3fcde38, 0x3fcdeb8, 0x3fcdf38,
            // Video Light Mode (Power Link / REC Link / REC Link & STBY / Auto)
            0x3f753b8, 0x3f75438, 0x3f754b8, 0x3f75538,
            // 4ch Audio Monitoring (CH1/CH2, CH3/CH4, CH1+3/CH2+4, Mono)
            0x3f91138, 0x3f911b8, 0x3f91238, 0x3f912b8, 0x3f91338,
            // 4ch Audio Output (HDMI CH1/CH2, CH3/CH4)
            0x3fcee38, 0x3fceeb8,
            // Display File Name (On / Off)
            0x3fa1138, 0x3fa11b8,
            // Shutter When Pwr OFF & Curtain when Pwr OFF
            0x3fd2338, 0x3fd23b8, 0x3fdaeb8, 0x3fdaf38,
            0x3fd8b38, 0x3fd8bb8, 0x3fe0e38, 0x3fe0eb8,
            // Auto Pixel Mapping (On / Off)
            0x3f85438, 0x3f854b8, 0x3fd4c38, 0x3fd4cb8,
            // Live View Brightness (Standard / Bright Monitoring)
            0x3fcfc38, 0x3fcfcb8,
            // Double Tap During AF (On / Off)
            0x3fcfa38, 0x3fcfab8
        };
        if (!result) for (uint32_t option : options) {
            result = restore_bytes(base, option + 0x0e, option + 0x0e, 30, 7);
            if (result) break;
            result = restore_bytes(base, option + 0x50, option + 0x50, 8, 7);
            if (result) break;
            writable(base, option, 7);
            fill_modes(base, option + 0x44);
        }
        if (!result) {
            writable(base, 0x3f83ab8, 7);
            *reinterpret_cast<volatile uint16_t *>(base + 0x3f83ab8 + 0x30) = 0;
        }
        const uint32_t dci_leaves[] = {
            0x58c7a50,
            0x58cb930, 0x58cb870, 0x58cb7b0, 0x58cb6f0, 0x58cb630, 0x58ce490
        };
        if (!result) for (uint32_t lva : dci_leaves) {
            result = restore_bytes(base, lva + 0x0e, lva - 0x10000 + 0x0e, 30, 3);
            if (result) break;
            result = restore_bytes(base, lva + 0x50, lva - 0x10000 + 0x50, 8, 3);
            if (result) break;
            writable(base, lva, 3);
            fill_modes(base, lva + 0x2c);
            *reinterpret_cast<uint64_t *>(base + lva + 0xa8) = base + 0x1bdf304;
        }
        if (!result) {
            writable(base, 0x5079000, 3);
            *reinterpret_cast<uint64_t *>(base + 0x5079928) = base + 0x1bdf304;
            for (uint32_t off = 0x5079a68; off <= 0x5079a98; off += 8)
                *reinterpret_cast<uint64_t *>(base + off) = base + 0x1bdf304;
            for (uint32_t off = 0x5079ce8; off <= 0x5079d40; off += 8)
                *reinterpret_cast<uint64_t *>(base + off) = base + 0x1bdf304;

            writable(base, 0x54f5d80, 3);
            *reinterpret_cast<uint64_t *>(base + 0x54f5d80) = base + 0x7f81c3a;
            *reinterpret_cast<volatile uint8_t *>(base + 0x7f81c38) = 1;

            const uint32_t dci_cap_ptrs[] = {0x552a8e8, 0x552a8f8, 0x552a900, 0x552a908};
            for (uint32_t ptr : dci_cap_ptrs) {
                writable(base, ptr, 3);
                *reinterpret_cast<uint64_t *>(base + ptr) = base + 0x8010ba8;
            }
            *reinterpret_cast<volatile uint32_t *>(base + 0x8010b88) = 1;
            *reinterpret_cast<volatile uint32_t *>(base + 0x8010b90) = 1;
            *reinterpret_cast<volatile uint32_t *>(base + 0x8010ba0) = 1;
            *reinterpret_cast<volatile uint32_t *>(base + 0x8010ba4) = 0x01010101;

            writable(base, 0x555c4a0, 3);
            *reinterpret_cast<uint64_t *>(base + 0x555c4a0) = base + 0x7f81d76;
            for (uint32_t off = 0x555c4b0; off <= 0x555c4f0; off += 8)
                *reinterpret_cast<uint64_t *>(base + off) = base + 0x7f81d76;
            for (uint32_t off = 0x7f81d6c; off <= 0x7f81d75; ++off)
                *reinterpret_cast<volatile uint8_t *>(base + off) = 1;
        }
        if (!result) {
            result = restore_bytes(base, 0x585e848 + 0x0e, 0x585e848 - 0x10000 + 0x0e, 42, 3);
            if (!result) {
                writable(base, 0x585e848, 3);
                fill_modes(base, 0x585e848 + 0x0e);
                *reinterpret_cast<uint64_t *>(base + 0x585e848 + 0x48) = base + 0x1c5dd98;
                *reinterpret_cast<uint64_t *>(base + 0x585e848 + 0x50) = base + 0x1bdf304;
            }
        }
        if (!result) {
            writable(base, 0x507b620, 3);
            *reinterpret_cast<uint64_t *>(base + 0x507b620) = base + 0x1bdf304;
            *reinterpret_cast<uint64_t *>(base + 0x507b628) = base + 0x1bdf304;
            writable(base, 0x588a0fe, 3);
            *reinterpret_cast<uint16_t *>(base + 0x588a0fe) = 1;
            const uint32_t pointers[] = {0x54f5e40, 0x54f5e50, 0x54f5e58, 0x54fd200, 0x54fd210};
            for (uint32_t pointer : pointers) {
                writable(base, pointer, 3);
                *reinterpret_cast<uint64_t *>(base + pointer) = base + 0x7f844f7;
            }
            *reinterpret_cast<uint32_t *>(base + 0x7f844f4) = 0x01010101;
            *reinterpret_cast<uint16_t *>(base + 0x7f84514) = 0x0101;

            stage('1');
            writable(base, 0x3ea5c38, 3);
            copy14(base + 0x3ea5c38, base + 0x3ea5c2a);
            copy14(base + 0x3ea5d54, base + 0x3ea5d46);
            sc4(226, (base + 0x3ea5c38) & ~0xfffUL, 0x1000, 1, 0);
            writable(base, 0x3f0ee88, 3);
            copy14(base + 0x3f0ee88, base + 0x3f0ed6c);
            sc4(226, (base + 0x3f0ee88) & ~0xfffUL, 0x1000, 1, 0);
            stage('2');
            /* Patch runtime multimap tree at appFw + 0x75ab168 (root ptr at +0x75ab178) */
            patch_custom_tree(base, *reinterpret_cast<volatile uint64_t *>(base + 0x75ab178), 0);
            stage('3');
            writable(base, 0x58b5a40, 3);
            fill_modes(base, 0x58b5a40 + 0x0e);
            writable(base, 0x58b2570, 3);
            fill_modes(base, 0x58b2570 + 0x0e);

            stage('4');
            writable(base, 0x25556cc, 7);
            volatile uint32_t *toggle_fn = reinterpret_cast<volatile uint32_t *>(base + 0x25556cc);
            toggle_fn[0] = 0xf002d162;
            toggle_fn[1] = 0xb944d841;
            toggle_fn[2] = 0x7100003f;
            toggle_fn[3] = 0x1a9f17e1;
            toggle_fn[4] = 0x1400e0db;
            sc4(226, (base + 0x25556cc) & ~0xfffUL, 0x2000, 5, 0);
            flush_icache(reinterpret_cast<void *>(base + 0x25556cc), reinterpret_cast<void *>(base + 0x25556cc + 20));
            volatile uint8_t *select_gate = *reinterpret_cast<volatile uint8_t **>(base + 0x541a230);
            if (select_gate) { select_gate[0] = 0; select_gate[1] = 0; }

            stage('5');
            // Do not push modified key codes into RTOS; that path stalled the camera.

            // UI only: restore records before every rebuild, then unlock them.
            result = restore_bytes(base, 0x5875cb8 + 0x0e, 0x5875cb8 - 0x10000 + 0x0e, 30, 3);
            if (!result) result = restore_bytes(base, 0x5875cb8 + 0x50, 0x5875cb8 - 0x10000 + 0x50, 8, 3);
            if (!result) result = restore_bytes(base, 0x585ffb8 + 0x0e, 0x585ffb8 - 0x10000 + 0x0e, 30, 3);
            if (!result) result = restore_bytes(base, 0x585ffb8 + 0x50, 0x585ffb8 - 0x10000 + 0x50, 8, 3);
            writable(base, 0x5875cb8, 3);
            *reinterpret_cast<volatile uint32_t *>(base + 0x5875cb8 + 0x28) = 0x00010101;
            fill_modes(base, 0x5875cb8 + 0x2c);
            *reinterpret_cast<volatile uint32_t *>(base + 0x5875cb8 + 0x54) = 2;
            *reinterpret_cast<volatile uint64_t *>(base + 0x5875cb8 + 0x70) = base + 0x1c5dd98;
            *reinterpret_cast<volatile uint64_t *>(base + 0x5875cb8 + 0xa8) = base + 0x1bdf304;
            writable(base, 0x507a618, 3);
            *reinterpret_cast<volatile uint64_t *>(base + 0x507a618) = base + 0x1bdf304;
            *reinterpret_cast<volatile uint64_t *>(base + 0x507a620) = base + 0x1bdf304;
            if (*reinterpret_cast<volatile uint32_t *>(base + 0x7f83490) > 1)
                *reinterpret_cast<volatile uint32_t *>(base + 0x7f83490) = 0;
            const uint32_t view_pointers[] = {0x55207f8, 0x5520808, 0x5520810, 0x5520818};
            for (uint32_t pointer : view_pointers) {
                writable(base, pointer, 3);
                *reinterpret_cast<volatile uint64_t *>(base + pointer) = base + 0x7f834af;
            }
            *reinterpret_cast<volatile uint32_t *>(base + 0x7f834ac) = 0x01010101;
            stage('6');

            // UI only: De-Squeeze Display (0x43d). The record crosses 0x5860000.
            writable(base, 0x585ffb8, 3);
            writable(base, 0x5860000, 3);
            stage('d');
            *reinterpret_cast<volatile uint32_t *>(base + 0x585ffb8 + 0x28) = 0x00010101;
            stage('e');
            fill_modes(base, 0x585ffb8 + 0x2c);
            stage('f');
            *reinterpret_cast<volatile uint64_t *>(base + 0x585ffb8 + 0x70) = base + 0x1c5dd98;
            *reinterpret_cast<volatile uint64_t *>(base + 0x585ffb8 + 0xa8) = base + 0x1bdf304;
            stage('b');
            writable(base, 0x507ebb8, 3);
            *reinterpret_cast<volatile uint64_t *>(base + 0x507ebb8) = base + 0x1bdf304;
            *reinterpret_cast<volatile uint64_t *>(base + 0x507ebc0) = base + 0x1bdf304;
            *reinterpret_cast<volatile uint64_t *>(base + 0x507ebc8) = base + 0x1bdf304;
            const uint32_t squeeze_pointers[] = {0x5501f58, 0x5501f68, 0x5501f70, 0x5501f78};
            for (uint32_t pointer : squeeze_pointers) {
                writable(base, pointer, 3);
                *reinterpret_cast<volatile uint64_t *>(base + pointer) = base + 0x7f88687;
            }
            stage('c');
            *reinterpret_cast<volatile uint32_t *>(base + 0x7f88684) = 0x01010101;
            // Restore 0x4b72818 and 0x34f991c from stock /usr/lib/appFw.so, and point both
            // 0x595e2e8 and 0x597a6a0 to 0x4b72818 so 0x34f991c never returns the zero-filled 0x4b72b98 table.
            restore_bytes(base, 0x4b72818, 0x4b72818, 0x4e8, 3);
            restore_bytes(base, 0x34f991c, 0x34f991c, 0x6c, 7);
            flush_icache(reinterpret_cast<void *>(base + 0x34f991c), reinterpret_cast<void *>(base + 0x34f9988));
            writable(base, 0x595e2e8, 3);
            *reinterpret_cast<volatile uint64_t *>(base + 0x595e2e8) = base + 0x4b72818;
            writable(base, 0x597a6a0, 3);
            *reinterpret_cast<volatile uint64_t *>(base + 0x597a6a0) = base + 0x4b72818;
            writable(base, 0x4b72818, 3);
            uint32_t og_val = *reinterpret_cast<volatile uint32_t *>(base + 0x7f83490);
            uint8_t vp_slot = (og_val == 0U) ? 0x19 : 0x1a;
            // Only patch Row 2 (Movie Live View); keep Row 1 (Playback) 100% stock!
            volatile uint8_t *row2 = reinterpret_cast<volatile uint8_t *>(base + 0x4b72818 + 2 * 44);
            row2[0x05] = vp_slot;
            row2[0x13] = vp_slot;

            // Unlock SteadyShot menu item during Movie Recording (prm_id=106, 0x17f8c90 -> return 0 like FX3)
            writable(base, 0x17f8c90, 7);
            *reinterpret_cast<volatile uint32_t *>(base + 0x17f8c90) = 0x52800000; // mov w0, #0
            *reinterpret_cast<volatile uint32_t *>(base + 0x17f8c94) = 0xd65f03c0; // ret
            flush_icache(reinterpret_cast<void *>(base + 0x17f8c90), reinterpret_cast<void *>(base + 0x17f8c98));

            // Unlock Cine EI / Cine EI Quick (0x5856ad8), Base ISO 800/12800 (0x584ea00), Base ISO Switch EI (0x586bc28), Still Log (0x586cfa8, 0x5855ac0)
            // + Embed LUT File (0x585c9e8, 0x5862ac8), Video Light Mode (0x58610f8), 4ch Audio Mon/Out (0x586f688, 0x58cb3f0),
            // + Display File Name (0x587e710), Shutter/Curtain When Pwr OFF (0x58bb5d0, 0x58c0540, 0x58c15a0, 0x58c48f0),
            // + Auto Pixel Mapping (0x586bb68, 0x58ba6b0), Live View Brightness (0x5892fb8), Double Tap During AF (0x58bc110)
            const uint32_t cine_leaves[] = {
                0x5856ad8, 0x584ea00, 0x586bc28, 0x586cfa8, 0x5855ac0,
                0x585c9e8, 0x5862ac8, 0x58610f8, 0x586f688, 0x58cb3f0,
                0x587e710, 0x58bb5d0, 0x58c0540, 0x58c15a0, 0x58c48f0,
                0x586bb68, 0x58ba6b0, 0x5892fb8, 0x58bc110
            };
            for (uint32_t lva : cine_leaves) {
                result = restore_bytes(base, lva + 0x0e, lva - 0x10000 + 0x0e, 30, 3);
                if (result) break;
                result = restore_bytes(base, lva + 0x50, lva - 0x10000 + 0x50, 8, 3);
                if (result) break;
                writable(base, lva, 3);
                *reinterpret_cast<volatile uint32_t *>(base + lva + 0x28) = 0x00010101;
                fill_modes(base, lva + 0x2c);
                *reinterpret_cast<volatile uint64_t *>(base + lva + 0x70) = base + 0x1c5dd98;
                *reinterpret_cast<volatile uint64_t *>(base + lva + 0xa8) = base + 0x1bdf304;
            }
            writable(base, 0x507a480, 3);
            for (uint32_t off = 0x507a480; off <= 0x507a498; off += 8)
                *reinterpret_cast<volatile uint64_t *>(base + off) = base + 0x1bdf304;
            for (uint32_t off = 0x507a4d0; off <= 0x507a4f0; off += 8)
                *reinterpret_cast<volatile uint64_t *>(base + off) = base + 0x1bdf304;
            for (uint32_t off = 0x507a668; off <= 0x507a678; off += 8)
                *reinterpret_cast<volatile uint64_t *>(base + off) = base + 0x1bdf304;
            writable(base, 0x507b558, 3);
            for (uint32_t off = 0x507b558; off <= 0x507b570; off += 8)
                *reinterpret_cast<volatile uint64_t *>(base + off) = base + 0x1bdf304;
            writable(base, 0x507c598, 3);
            for (uint32_t off = 0x507c598; off <= 0x507c5e0; off += 8)
                *reinterpret_cast<volatile uint64_t *>(base + off) = base + 0x1bdf304;
            writable(base, 0x507e7b0, 3);
            *reinterpret_cast<volatile uint64_t *>(base + 0x507e7b0) = base + 0x1bdf304;
            *reinterpret_cast<volatile uint64_t *>(base + 0x507e7b8) = base + 0x1bdf304;
            *reinterpret_cast<volatile uint64_t *>(base + 0x507ec50) = base + 0x1bdf304;
            *reinterpret_cast<volatile uint64_t *>(base + 0x507ec58) = base + 0x1bdf304;
            for (uint32_t off = 0x507ee48; off <= 0x507ee68; off += 8)
                *reinterpret_cast<volatile uint64_t *>(base + off) = base + 0x1bdf304;
            writable(base, 0x507ef18, 3);
            for (uint32_t off = 0x507ef18; off <= 0x507ef48; off += 8)
                *reinterpret_cast<volatile uint64_t *>(base + off) = base + 0x1bdf304;
            *reinterpret_cast<volatile uint64_t *>(base + 0x507f150) = base + 0x1bdf304;
            *reinterpret_cast<volatile uint64_t *>(base + 0x507f158) = base + 0x1bdf304;
            for (uint32_t off = 0x507f1e8; off <= 0x507f200; off += 8)
                *reinterpret_cast<volatile uint64_t *>(base + off) = base + 0x1bdf304;
            for (uint32_t off = 0x507f238; off <= 0x507f250; off += 8)
                *reinterpret_cast<volatile uint64_t *>(base + off) = base + 0x1bdf304;
            writable(base, 0x50819b0, 3);
            *reinterpret_cast<volatile uint64_t *>(base + 0x50819b0) = base + 0x1bdf304;
            *reinterpret_cast<volatile uint64_t *>(base + 0x50819b8) = base + 0x1bdf304;

            const uint32_t log_pointers[] = {0x54f8a68, 0x54f8a78, 0x54f8a80, 0x54f8a88, 0x54f8a90};
            for (uint32_t ptr : log_pointers) {
                writable(base, ptr, 3);
                *reinterpret_cast<volatile uint64_t *>(base + ptr) = base + 0x7f834cf;
            }
            *reinterpret_cast<volatile uint32_t *>(base + 0x7f834cc) = 0x01010101;
            *reinterpret_cast<volatile uint16_t *>(base + 0x7f834d0) = 0x0101;

            const uint32_t base_iso_pointers[] = {0x54ef398, 0x54ef3a8, 0x54ef3b0, 0x54ef3b8, 0x54ef3c0, 0x54ef3c8};
            for (uint32_t ptr : base_iso_pointers) {
                writable(base, ptr, 3);
                *reinterpret_cast<volatile uint64_t *>(base + ptr) = base + 0x7f8511f;
            }
            *reinterpret_cast<volatile uint32_t *>(base + 0x7f8511c) = 0x01010101;
            *reinterpret_cast<volatile uint16_t *>(base + 0x7f85120) = 0x0101;

            for (uint32_t ptr = 0x550dfc8; ptr <= 0x550e068; ptr += 8) {
                writable(base, ptr, 3);
                *reinterpret_cast<volatile uint64_t *>(base + ptr) = base + 0x7f85147;
            }
            *reinterpret_cast<volatile uint32_t *>(base + 0x7f85144) = 0x01010101;

            const uint32_t still_log_pointers[] = {0x550f058, 0x550f068, 0x550f070, 0x550f078, 0x550f080};
            for (uint32_t ptr : still_log_pointers) {
                writable(base, ptr, 3);
                *reinterpret_cast<volatile uint64_t *>(base + ptr) = base + 0x7f83537;
            }
            *reinterpret_cast<volatile uint32_t *>(base + 0x7f83534) = 0x01010101;

            // Capability tables for newly unlocked Cinema Line / Alpha 1 / FX3 features
            unlock_cap_table(base, 0x54fdf28, 3, 0x7f83514); // Embed LUT File
            unlock_cap_table(base, 0x5502e18, 4, 0x7f88c74); // Video Light Mode
            unlock_cap_table(base, 0x55144c8, 7, 0x7f888b4); // 4ch Audio Monitoring
            unlock_cap_table(base, 0x54fab30, 2, 0x7f88bd4); // 4ch Audio Output
            unlock_cap_table(base, 0x552bed0, 2, 0x7f88734); // Display File Name
            unlock_cap_table(base, 0x551b760, 2, 0x7f88d0c); // Shutter/Curtain When Pwr OFF (1)
            unlock_cap_table(base, 0x5567e50, 2, 0x7f843ac); // Shutter/Curtain When Pwr OFF (2)
            unlock_cap_table(base, 0x550df18, 2, 0x7f88d2c); // Auto Pixel Mapping (1)
            unlock_cap_table(base, 0x5564c78, 2, 0x7f843cc); // Auto Pixel Mapping (2)
            unlock_cap_table(base, 0x5540d30, 2, 0x7f8a16c); // Live View Brightness
            unlock_cap_table(base, 0x555ed40, 2, 0x7f8816c); // Double Tap During AF
            stage('7');

            // Keep the user's On/Off choice; stock recalculation writes 2 ('-') or 1 ('Off') on mode dial changes.
            writable(base, 0x2f06458, 7);
            *reinterpret_cast<volatile uint32_t *>(base + 0x2f06458) = 0xd65f03c0; // ret
            flush_icache(reinterpret_cast<void *>(base + 0x2f06458), reinterpret_cast<void *>(base + 0x2f0645c));
            writable(base, 0x2f12884, 7);
            *reinterpret_cast<volatile uint32_t *>(base + 0x2f12884) = 0xd65f03c0; // ret (prevent resetting Display File Name 0x7f88718 to 2)
            flush_icache(reinterpret_cast<void *>(base + 0x2f12884), reinterpret_cast<void *>(base + 0x2f12888));
            if (*reinterpret_cast<volatile uint32_t *>(base + 0x7f88718) > 1)
                *reinterpret_cast<volatile uint32_t *>(base + 0x7f88718) = 1;
            if (*reinterpret_cast<volatile uint32_t *>(base + 0x7f88720) > 1)
                *reinterpret_cast<volatile uint32_t *>(base + 0x7f88720) = 1;
            writable(base, 0x27ad10c, 7);
            *reinterpret_cast<volatile uint32_t *>(base + 0x27ad10c) = 0xd65f03c0; // ret (prevent mode dial from resetting 0x7f83490)
            *reinterpret_cast<volatile uint32_t *>(base + 0x27ad124) = 0xd65f03c0; // ret (prevent mode dial from resetting 0x7f83498)
            flush_icache(reinterpret_cast<void *>(base + 0x27ad10c), reinterpret_cast<void *>(base + 0x27ad130));

            // Bypass PAL/24.00p confirmation modal abort in File Format (0x269228c) and DCI 4K FPS (0x26082dc) setters.
            writable(base, 0x26922d8, 7);
            *reinterpret_cast<volatile uint32_t *>(base + 0x26922d8) = 0x140000a7;
            flush_icache(reinterpret_cast<void *>(base + 0x26922d8), reinterpret_cast<void *>(base + 0x26922dc));
            writable(base, 0x26085c4, 7);
            *reinterpret_cast<volatile uint32_t *>(base + 0x26085c4) = 0x17ffffc1;
            flush_icache(reinterpret_cast<void *>(base + 0x26085c4), reinterpret_cast<void *>(base + 0x26085c8));
            stage('8');
            *reinterpret_cast<volatile uint8_t *>(base + 0x75aadd8) = 0;
            reinterpret_cast<void (*)()>(base + 0x240ed34)();
            stage('9');
            /* Var. Shutter menu items: entry[3]=0 + entry[8]=0 means ACTIVE (not greyed out) */
            uint8_t *e158 = cache_entry(base, 0x7597ea8, 0x158);
            e158[3] = 0; e158[8] = 0;
            leaf_cache_entry(base, 0x159, base + 0x75564e0 + 0x470 * 0x18);
            reinterpret_cast<uint8_t *>(base + 0x7597ea8 + 0x159 * 0x18)[3] = 0;
            reinterpret_cast<uint8_t *>(base + 0x7597ea8 + 0x159 * 0x18)[8] = 0;
            leaf_cache_entry(base, 0x15a, 0);
            reinterpret_cast<uint8_t *>(base + 0x7597ea8 + 0x15a * 0x18)[3] = 0;
            leaf_cache_entry(base, 0x517, 0);
            reinterpret_cast<uint8_t *>(base + 0x7597ea8 + 0x517 * 0x18)[3] = 0;
            leaf_cache_entry(base, 0x58a, 0);
            reinterpret_cast<uint8_t *>(base + 0x7597ea8 + 0x58a * 0x18)[3] = 0;
            leaf_cache_entry(base, 0x09a, base + 0x75564e0 + 0x241 * 0x18);
            leaf_cache_entry(base, 0x09c, base + 0x75564e0 + 0x244 * 0x18);
            leaf_cache_entry(base, 0x09e, base + 0x75564e0 + 0x24c * 0x18);
            leaf_cache_entry(base, 0x09f, base + 0x75564e0 + 0x24c * 0x18);
            leaf_cache_entry(base, 0x0a4, base + 0x75564e0 + 0x256 * 0x18);
            leaf_cache_entry(base, 0x0a5, base + 0x75564e0 + 0x258 * 0x18);
            leaf_cache_entry(base, 0x14a, base + 0x75564e0 + 0x455 * 0x18);
            leaf_cache_entry(base, 0x14b, base + 0x75564e0 + 0x457 * 0x18);
            leaf_cache_entry(base, 0x14c, base + 0x75564e0 + 0x459 * 0x18);
            leaf_cache_entry(base, 0x1c4, base + 0x75564e0 + 0x71b * 0x18);
            leaf_cache_entry(base, 0x1c5, base + 0x75564e0 + 0x720 * 0x18);
            leaf_cache_entry(base, 0x404, base + 0x75564e0 + 0xc43 * 0x18);
            leaf_cache_entry(base, 0x43d, base + 0x75564e0 + 0xcc9 * 0x18);
            leaf_cache_entry(base, 0x442, base + 0x75564e0 + 0xcdc * 0x18);
            leaf_cache_entry(base, 0x454, base + 0x75564e0 + 0xd40 * 0x18);
            leaf_cache_entry(base, 0x46e, base + 0x75564e0 + 0xdb3 * 0x18);
            leaf_cache_entry(base, 0x476, base + 0x75564e0 + 0xdc0 * 0x18);
            leaf_cache_entry(base, 0x47c, base + 0x75564e0 + 0xdd0 * 0x18);
            leaf_cache_entry(base, 0x47d, base + 0x75564e0 + 0xdd2 * 0x18);
            leaf_cache_entry(base, 0x47e, base + 0x75564e0 + 0xdd4 * 0x18);
            leaf_cache_entry(base, 0x62f, base + 0x75564e0 + 0x15cf * 0x18);
            cache_entry(base, 0x75564e0, 0x241);
            cache_entry(base, 0x75564e0, 0x242);
            for (uint32_t oid = 0x244; oid <= 0x248; ++oid)
                cache_entry(base, 0x75564e0, oid);
            for (uint32_t oid = 0x24c; oid <= 0x24e; ++oid)
                cache_entry(base, 0x75564e0, oid);
            for (uint32_t oid = 0x256; oid <= 0x25a; ++oid)
                cache_entry(base, 0x75564e0, oid);
            for (uint32_t oid = 0x455; oid <= 0x45a; ++oid)
                cache_entry(base, 0x75564e0, oid);
            for (uint32_t oid = 0x71b; oid <= 0x726; ++oid)
                cache_entry(base, 0x75564e0, oid);
            cache_entry(base, 0x75564e0, 0xc43);
            cache_entry(base, 0x75564e0, 0xc44);
            cache_entry(base, 0x75564e0, 0xcdc);
            cache_entry(base, 0x75564e0, 0xcdd);
            for (uint32_t oid = 0xd40; oid <= 0xd44; ++oid)
                cache_entry(base, 0x75564e0, oid);
            for (uint32_t oid = 0xd9f; oid <= 0xda7; ++oid)
                cache_entry(base, 0x75564e0, oid);
            cache_entry(base, 0x75564e0, 0xdb3);
            cache_entry(base, 0x75564e0, 0xdb4);
            for (uint32_t oid = 0xdc0; oid <= 0xdc3; ++oid)
                cache_entry(base, 0x75564e0, oid);
            for (uint32_t oid = 0xdd0; oid <= 0xdd5; ++oid)
                cache_entry(base, 0x75564e0, oid);
            cache_entry(base, 0x75564e0, 0x15cf);
            cache_entry(base, 0x75564e0, 0x15d0);
            cache_entry(base, 0x75564e0, 0xcc9);
            cache_entry(base, 0x75564e0, 0xcca);
            cache_entry(base, 0x75564e0, 0xccb);
            cache_entry(base, 0x75564e0, 0x470)[3] = 0;
            cache_entry(base, 0x75564e0, 0x471)[3] = 0;
            cache_entry(base, 0x75564e0, 0x8d);
            for (uint32_t oid = 0xc8; oid <= 0xd1; ++oid)
                cache_entry(base, 0x75564e0, oid);
            for (uint32_t oid = 0x11b; oid <= 0x126; ++oid)
                cache_entry(base, 0x75564e0, oid);
            cache_entry(base, 0x7597ea8, 0x2a)[4] = 1;
            for (uint32_t lid = 0x42; lid <= 0x47; ++lid)
                cache_entry(base, 0x7597ea8, lid)[4] = 1;
            for (uint32_t mva = 0x7f82340; mva <= 0x7f82480; mva += 0x40)
                for (uint32_t i = 0x14; i <= 0x34; ++i)
                    *reinterpret_cast<volatile uint8_t *>(base + mva + i) = 1;
            *reinterpret_cast<uint32_t *>(base + 0x75971d8 + 0x157 * 4) = 0x01000101;
            patch_uxc_strings();
            long dci_fd = sc4(56, -100, (long)"/tmp/jiritsu/set-dci4k.flag", 0, 0);
            if (dci_fd >= 0) {
                sc4(57, dci_fd, 0, 0, 0);
                sc4(35, -100, (long)"/tmp/jiritsu/set-dci4k.flag", 0, 0);
                *reinterpret_cast<volatile uint32_t *>(base + 0x7f81d50) = 3;
                uint64_t ctx = 0;
                reinterpret_cast<void (*)(uint64_t *)>(base + 0x2540a08)(&ctx);
                reinterpret_cast<void (*)(uint64_t *, uint32_t)>(base + 0x26925c0)(&ctx, 0x0b);
                reinterpret_cast<void (*)(uint64_t *, uint32_t)>(base + 0x260c4c8)(&ctx, 3);
                reinterpret_cast<void (*)(uint64_t *)>(base + 0x2540a18)(&ctx);
            }
            long hd_fd = sc4(56, -100, (long)"/tmp/jiritsu/set-hd.flag", 0, 0);
            if (hd_fd >= 0) {
                sc4(57, hd_fd, 0, 0, 0);
                sc4(35, -100, (long)"/tmp/jiritsu/set-hd.flag", 0, 0);
                uint32_t cur = *reinterpret_cast<volatile uint32_t *>(base + 0x7f81c10);
                uint32_t target = (cur == 8) ? 10 : 8;
                uint64_t ctx = 0;
                reinterpret_cast<void (*)(uint64_t *)>(base + 0x2540a08)(&ctx);
                reinterpret_cast<void (*)(uint64_t *, uint32_t)>(base + 0x26925c0)(&ctx, target);
                reinterpret_cast<void (*)(uint64_t *)>(base + 0x2540a18)(&ctx);
            }
            // Keep the menu stable while the Open Gate hook is being validated.
        }
        sc4(57, stock_fd, 0, 0, 0);
    }
    char message[] = "MENU_REPAIR result=00000000 base=0000000000000000\n";
    uint32_t code = (uint32_t)result;
    for (int i = 0; i < 8; ++i) message[19 + i] = "0123456789abcdef"[(code >> ((7 - i) * 4)) & 15];
    for (int i = 0; i < 16; ++i) message[33 + i] = "0123456789abcdef"[(base >> ((15 - i) * 4)) & 15];
    long fd = sc4(56, -100, (long)"/tmp/jiritsu/menu-repair.txt", 1 | 64 | 512, 0644);
    if (fd >= 0) { sc4(64, fd, (long)message, sizeof(message) - 1, 0); sc4(57, fd, 0, 0, 0); }
}
