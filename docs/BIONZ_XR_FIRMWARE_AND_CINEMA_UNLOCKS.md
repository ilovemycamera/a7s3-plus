# Bionz XR Firmware Map, Cinema Unlocks & Persistent NVRAM Reference

This document summarizes our static and live reverse-engineering findings across Sony's Bionz XR camera platform, focusing on **Sony A7S III (`ILCE-7SM3`, firmware `5.01`)** compared against **FX3 (`7.02`)**, **A7 IV (`6.02`)**, and **ZV-E1 (`2.02`)**.

---

## 1. Cross-Body Bionz XR Architecture

Unpacking stock `BODYDATA.DAT` containers (`\x89UFU`) reveals that Sony's modern full-frame Bionz XR bodies share a single unified Linux + RTOS platform:

| Camera Body | Shared `.so` Count | `appFw.so` Size | `appFw.so` SHA-256 (prefix) | Product Class / Menu Catalogue |
|---|---:|---:|---|---|
| **A7S III (`5.01`)** | `211` | `97,781,960` B | `eba89299273542e9` | `OT_UUD` (feature index `6`, catalogue `20`) |
| **FX3 (`7.02`)** | `209` | `79,015,712` B | `545c5053a74e04ff` | `ALCIN_UUD` (catalogue `2`) |
| **A7 IV (`6.02`)** | `206` | `98,212,632` B | `1e1b9202eaa69198` | Bionz XR shared tree |
| **ZV-E1 (`2.02`)** | `196` | `99,762,272` B | `88146fa5f5844b45` | Bionz XR shared tree |

### Key Structural Findings
- **Identical Scenario Layer:** All 26 `scenario/*_SCN_*.so` libraries (`AVBB` start/stop video, HDMI, mic, `CAMERA_SCN_EE_*`, `EXTERNAL_RAW_RECORD`, etc.) have identical filenames across all four bodies.
- **Model-Specific Libraries Are Tiny:** Only small hardware/peripheral libraries differ (`libfan.so` on FX3; `libHeapMon.so` & `libInfraStreamingUsb.so` on A7S III; `libAiDictionary.so` on ZV-E1).
- **Feature Gating Lives Inside `appFw.so`:** Rather than compiling completely separate firmware trees per body, Sony gates Cinema Line features (BIG6 Home Screen, Variable Shutter, Cine EI workflows, menu entries) through per-model descriptor tables and feature flags inside `appFw.so`.
- **Flash Partition Map (`ILCE-7SM3` FW `5.01`):**
  - `nflasha3` (`32 MB`): early boot `sabin` + `initrd`
  - `nflasha5` (`210 MB`): `WBI2` Linux hibernation snapshot (see [`SAFETY_AND_POSTMORTEM.md`](SAFETY_AND_POSTMORTEM.md))
  - `nflasha7` (`5.7 MB`): secondary `initrd` (`wbshd` / Bluetooth)
  - `nflasha15` (`403 MB`): main root filesystem (`lib/` 162 MB, `share/app/` 92 MB of `master_*.uxc` / `string_*.uxc` / `image_*.uxb`, `codec/` 24 MB, `kmod/` 13 MB)
  - `0800_appli/setting/`: DTS (`cxd90057.dtb`), `DmmCfg.bin` memory pools, and runtime configuration

---

## 2. Variable Shutter & Dual-Renderer Live-View Pipeline

FX3's **Variable Shutter** (fine anti-flicker decimal shutter control in Movie mode) is physically present inside A7S III firmware `5.01`, gated behind model visibility tables and a dual-renderer OSD overlay:

### Native Event & Property Plumbing (`appFw.so`)
- **Event Dispatcher:** `appFw + 0x1bd4fd4(event_id)` looks up the event in a runtime `std::map` red-black tree rooted at `base + 0x5969e08` (node layout `{+0x20 event_id, +0x24 handler_id}`) and invokes `vtable + 0x1d8`.
  - `0x021a000b / 0x021a000c` -> `ANTI_FLICKER_SHOOT` ON / OFF
  - `0x021a000d / 0x021a000e` -> `HIGH_RES_SS` (Variable Shutter) ON / OFF -> handlers `0x501 / 0x508`
- **Live Properties:**
  - Property **`0x612`** (`base + 0x7f844d8`, getter `0x28aa8e0`): Variable Shutter On/Off state (`0` = ON, `1` = OFF)
  - Property **`0x611`** (`0x28aa8b8`): current high-resolution shutter value (tenths, e.g. `600` = `60.0`)
  - Property **`0x399`** (`0x28a5b18`, 36 bytes from RTOS IPC): live shutter numerator, denominator, and decimal-format flag at `+0x08` (`1` -> `"60.0"` template at `0x200a425`, `0` -> `"1/60"` at `0x2006026`)

### Why Two Renderers Fight Over the Shutter Plate
On A7S III Movie Live View, **two separate renderers** draw the shutter speed readout:
1. **Linux UX Widget (`appFw.so`):** `No382_AutoIconforShutterspeed` (updater `0x23fc5dc`), which knows how to render decimal shutter values (`60.0`, `48.2`, etc.).
2. **RTOS Hardware OSD Overlay (MP GUI):** Continuously paints stock `1/60` directly over the Linux UX plate unless suppressed.

[`runtime/high_res_display_v10.cpp`](../runtime/high_res_display_v10.cpp) solves both cleanly:
1. Opens the Linux UX `view_string` gate at `0x23fc620`.
2. Drives the decimal-format flag dynamically from live property `0x612` (`[x20 + 0x220]`) at `0x23fc680..0x23fc688` — leaving neighboring exposure plates and stock `1/60` behavior untouched when Variable Shutter is OFF.
3. Suppresses **only** the duplicate RTOS shutter OSD widget (`w = 5`) by zeroing its visibility halfword `*(u16*)(x0 + 0x20e)` and rect via its virtual getters (`0x18789f8` / `0x1878a28`).
4. Unlocks the Movie control dial by clearing the dial-lock byte at `base + 0x8455f38` and patching `0x1b6e1e8` (`NOP`), allowing tenths stepping directly from the camera dial.

### Menu Visibility Repair (`runtime/menu_repair.cpp`)
Unlocks gated menu items without switching the camera's global product class (which causes boot loops):
- Visibility records: `0x588a0fe`, `0x5892f30`, `0x58a4ce8`, option tables `0x3fb0388`, `0x3fb0408`
- Mode tables (`20 × u16`): leaf records `0x5892ef8` / `0x58a4cb0`, parent `0x585e848`
- Activity flags & `.bss` cache invalidation: `0x7597ea8`, `0x75564e0`, Fn cache `0x75971d8`

---

## 3. BIG6 Cinema Home Screen & Persistent Custom Key NVRAM IDs

The FX3/FX30 **BIG6 Cinema Home Screen** ships dormant inside A7S III `5.01` and can be bound natively in the camera's persistent settings memory (Backup / NVRAM):

- Backup <-> Internal Custom-Key lookup table in `appFw.so`: `.rodata 0x4aa1080..0x4aa5138`
- Verified A7S III `5.01` Custom-Key enum IDs:

| Function | Backup NVRAM Enum | Internal ID | Notes |
|---|---:|---:|---|
| **Home / BIG6 Screen** | `0x0b70` | `0x574` | Once enabled in NVRAM, swipe-down BIG6 persists across cold reboots without RAM patches |
| **Var. Shutter Select** | `0x0307` | `0x501` | Toggles Variable Shutter ON/OFF with native Sony toast |
| **Var. Shutter Set.** | `0x00c4` | `0x157` | Opens native Variable Shutter fine-adjustment screen |

> **Important:** FX3's `0x035c` value maps to *"Not set"* on A7S III — never blindly copy Backup enum IDs across Sony bodies without verifying `.rodata 0x4aa1080..0x4aa5138`.

---

## 4. Hardware Shutter Angle, Cine EI / EI Wheel, Embed LUT & Aspect Markers (`v26`)

### Sony `appFw.so` Capability Polarity & Leaf Mode Slots
- **Capability Polarity (`0 = ENABLED`, `1 = DISABLED`):** Across `appFw.so` capability structs (`+0x14` overall leaf flag, `+0x15 + ev` per-option flag) and menu cache entries (`0x7597ea8 + lid * 0x18`, byte `[3]`), **`0` means active/enabled** and **`1` means disabled/greyed out**. `menu_repair.cpp` allocates a permanent 4 KB anonymous `mmap` sink page (`cap_sink`) and redirects capability writer pointers into it via `unlock_cap_zero()` while writing the desired `0/1` enable mask into the live `.bss` capability bytes.
- **20-Slot Leaf Visibility Array:** Every menu leaf record holds `20 × u16` mode-visibility entries at `lva + 0x2c..+0x53` (indices `18–19` at `+0x50..+0x53` cover Movie mode — updating only 18 entries hides the item in Movie mode after a cold reboot).

### Hardware Shutter Angle (`PID 0x401c`)
Cross-body analysis of `cpapp-b.bin` shows that **A7S III `5.01` already contains the identical real-time CP `PID 0x401c` Shutter Angle handler** as FX3 `7.02` (`FW: SET_SHUT_DRIVE[drv=%d:sum=%d(up=%d):typ=%d:m=%d:c=%d:abs=%lu/%lu:f=%lu]`), and `No382_AutoIconforShutterspeed` already renders `180.0°` cleanly. Sony only stripped the `ShutterMode == 1` encoder branch out of `appFw.so @ 0x355bda0` and omitted the `0x3e03/0x3e04` wheel dataflow step nodes:
1. **`hook_355bda0` (`runtime/og_menu_daemon.cpp`):** Exact port of FX3 `7.02 @ 0x2e1cad8` adapted for A7S III `sequence_set_shut_drive` (`0x34a46fc`), which packs `u16 shut_mode` at `in+0x0c` and `u16 angle` at `in+0x0e`. Encodes `msg[10] = 1` (Angle), `msg[11] = 1` (absolute `100 / ((fps_x100 * 360000) / angle_x1000)`) or `msg[11] = 2` (frame accumulation `64F..2F`), plus full S&Q (`cur_exp` mask `0x103e0000`) and clean `ShutterMode == 0` (Speed) pass-through.
2. **`hook_26a6f4c` (`appFw + 0x26a6f4c`):** Intercepts physical shutter wheel rotation (`delta != 0`) and steps `Angle` (`0x7f84f98`) smoothly via Sony's native Angle setter `0x26a70ac`; when `delta == 0`, calls the original trampoline `call_orig_26a6f4c` followed by explicit `flush_seq_queue()` (`appFw + 0x24ef294`) so sequence `0x63` dispatches immediately.
3. **Automatic FPS / S&Q / Mode Sync (`og_worker`):** Watches `ShutterMode` (`0x7f84f10`), `Angle` (`0x7f84f98`), `FileFormat` (`0x7f81c10`), all 8 Rec FPS words (`0x7f81c18..0x7f81d50`), S&Q FPS words (`0x7f82608/630/660`), `cur_sys`, and `cur_exp`, firing `trigger_shutter_angle_apply()` after a ~180 ms settle delay (hardware-verified in `/tmp/jiritsu/vdf-log.txt`: e.g. `90.0° @ 50p -> 100/20000` vs `90.0° @ 25p -> 100/10000`).

### Cine EI / Cine EI Quick / Base ISO `800 & 12800` / Exposure Index & ISO Wheel
- `sequence_set_iso_speed_rate` (`0x34872a8`), `PID 0x4029` (`0x355c404`), `VDF CMD 0x015f` (`0x35224bc`), and `vdf_set_ei_gain` (`VDF CMD 0x0134` @ `0x3522314`) are **100% native** in A7S III `5.01` once `Log Shooting` (`lid 0x09c`, `0x5856ad8`) passes `FeatureManager(0x9c)` (`0x2e76c78 -> 0x1bdece8`).
- `menu_repair.cpp` unlocks `Log Shooting` (`Off / Flexible ISO / Cine EI Quick / Cine EI`), `Base ISO` (`ISO 800 & ISO 12800` only, keeping FX30 `2500` & FX6 `4000` hidden), `Base ISO Switch EI` (`1000–4000 EI`, `ev 8..14`), and exact FX3 `±2 EV` Exposure Index ranges (`EI_auto` `200–51200 EI`, `EI_low` `200–3200 EI`, `EI_high` `3200–51200 EI`), plus `Embed LUT File` (`0x54fdf28` / `0x7f83514`).
- **Physical ISO/EI Wheel Fix (`og_menu_daemon.cpp`):** Just like Shutter Angle (`0x3e03/0x3e04`), A7S III stripped dataflow step nodes `0x4046..0x404b` for the 6 ISO-wheel EI steppers (`0x26cb554/644`, `0x26c9820/910`, `0x26c9b94/c84`). `og_menu_daemon.cpp` hooks all 6 directly onto Sony's intact menu setters (`0x26cb460`, `0x26c972c`, `0x26c9aa0`) + `flush_seq_queue()` (`0x24ef294`).

### Full 16-Ratio Aspect Markers (Single Menu Entry)
- Stock A7S III `5.01` already exposes `Aspect Marker` leaf `lid 0x1a4` (`0x584d0f8`) sharing the 16-entry option table `0x3f8d038` (`oid 0x5c7..0x5d6`) with gated Cinema Line leaf `lid 0x1a5` (`0x584e880`). Unlocking only the 7 gated ratios in `0x3f8d038` + capability table `0x55223a8` (`0x7f84cd4`) while keeping `lid 0x1a5` hidden enables all 16 aspect markers (`9:16`, `4:5`, `1:1`, `17:9`, `1.91:1`, `2:1`, `2.39:1`, …) with zero duplicate menu items.

---

## 5. `BKID_*` Persistent Settings Catalog & USB Research Bench

Sony stores persistent camera parameters in Backup NVRAM indexed by 32-bit `BKID` identifiers (`0x02cf0000 + id_low16` in the app settings zone). Static extraction of `{u64 name_ptr, u32 id_low16, u32 len}` records from `appFw.so` is included in [`analysis/bkid_catalog.csv`](../analysis/bkid_catalog.csv) (`116` verified `VA <-> id_low16 <-> BKID_APP_*` entries out of `~6,625` `BKID_*` strings in `appFw.so`).

### Read-Only Research Bench Commands
Developers exploring A7S III `5.01` over USB Mass Storage can inspect camera state safely without modifying flash:

```bash
# Inspect kernel, im.elf PID, appFw.so base, and runtime maps
sudo -E .venv/bin/python analysis/stand.py status

# Run an arbitrary read-only root shell command inside im.elf
sudo -E .venv/bin/python analysis/run_jit_command.py "uname -a; cat /proc/$(pidof im.elf)/maps | grep appFw.so"

# Pull any file from camera RAM/rootfs to stdout
sudo -E .venv/bin/python analysis/read_camera_file.py /proc/snsc_boot_time
```

### Hard-Won Safety Rules for Sony Bionz XR Payload Authors
1. **Never keep callbacks inside a transiently `dlopen`'d `.so`:** `/tmp/jiritsu/libJit.so` is `dlclose`'d immediately after `_init`. Either allocate a private `mmap` RWX trampoline page (syscall `222`) or `dlopen(..., RTLD_NODELETE)` a uniquely named resident `.so`.
2. **Always flush instruction caches after patching AArch64 code:** `dc cvau` -> `dsb ish` -> `ic ivau` -> `dsb ish; isb`.
3. **Never change global `OT_UUD` product model class:** Patch individual feature/menu gates instead; switching the global model class triggers boot loops.
4. **Never guess-write Backup NVRAM (`bk.elf w`):** Unverified encodings (especially shutter angle or unknown enums) can trigger immediate reboots or broken camera state.
5. **Never run `EL1`-only cache instructions (`dc civac`) from `im.elf` (`EL0`):** Use `/dev/mem` with `O_SYNC` (`0x101002`) + `dsb sy` for uncached/device MMIO/DMA accesses.
