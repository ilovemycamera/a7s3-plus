# Native Square-Pixel `3840 × 2560` (`1:1` SAR) Recording — Technical Breakthrough & Implementation

> [!TIP]
> **Status: SOLVED & INTEGRATED into `og_menu_daemon.so` (`a7s3-plus`)!**
> Sony A7S III (`ILCE-7SM3`, FW `5.01`) now records **true native coded `3840 × 2560` (`1:1` square-pixel SAR, `3:2` DAR)** directly from the full `4240 × 2826` sensor with zero black bars, zero anamorphic squeeze, 10-bit 4:2:2 / 4:2:0 encoding (`XAVC HS 4K` HEVC Level 6.0 up to 200 Mbps 50p, `XAVC S 4K` H.264 25p/50p, `XAVC S-I 4K`), intact Sony `rtmd` gyro metadata, and **native in-camera playback**.

---

## 1. The Complete Root-Cause Chain (How We Unlocked `3840 × 2560`)

Earlier naive `2160 → 2560` patches failed at multiple layers across all three Bionz XR domains (`MP` Linux, `AV` Imola2 RTOS, `CP` ISP RTOS). Live physical `ulogio` ring-buffer capture (`0x405000000`, `0x780000` bytes) + MIPS32LE disassembly of `vcd_firmware_imola2.bin` isolated every single blocker:

### A. `Imola2` Codec Firmware (`vcd_firmware_imola2.bin`) — Exact `createEncPool` Math
Disassembling `imola_api_parameter_check.c` and `dyn_pool_mng::createEncPool` (`0x80063ddc` → `0x800663f4` → `0x80067174/a4/d8`) proved that **Imola2 hardware natively accepts up to `8640 × 5760`** with zero hard-coded `2160` encoder cap. The sole reason `ImolaCreateCh` rejected `3840 × 2560` 4:2:2 10-bit (`iml_ch = 255`) was a single DMM buffer-size check:

| Buffer | `3840 × 2160` 4:2:2 10-bit | `3840 × 2560` 4:2:2 10-bit | Stock 4K DMM (`key=2`) | Fix in `a7s3-plus` |
|---|---|---|---|---|
| **FrmBuf CD (ESRAM)** | `0x0260000` (`2.38 MB`) | `0x0260000` (`2.38 MB`) | `0x470` (`2.38 MB`) / `0x46f` (`4.75 MB`) | Fits identically! |
| **FrmBuf LD (Ref Frames)** | `0x2f78000` (`47.47 MB`) | **`0x3848000` (`56.28 MB`)** | `0x474` (`48.44 MB`) → **OVERFLOW!** | **Swap with unused `0x486` (`96.12 MB`)!** |
| **MultiBuf** | `0x0918000` (`9.09 MB`) | `0x0ab0000` (`10.69 MB`) | `0x477` (`13.56 MB`) | Fits with `2.87 MB` spare! |
| **EsBuf** | `0x8449c00` (`132.29 MB`) | `0x8449c00` (`132.29 MB`) | `0x47a` (`132.29 MB`) | Fits identically! |

- **The Zero-DMM-Mod Breakthrough:** Inside parent DMM pool `0x668` (`290.62 MB`), Sony pre-allocates unit **`0x486` (`96.12 MB`)** exclusively for DRAM `FrmBuf CD` — which is completely unused in ESRAM Long-GOP modes (`XAVC HS 4K` / `XAVC S 4K`). Swapping `0x486` ↔ `0x474` in `ENCHANDLER`'s `unittype` table (`av-cam.bin @ 0x38d8e8/ec` for `key=2`, `0x38d908/0c` for `key=4`) gives `FrmBuf LD` **`96.12 MB`** (40 MB spare for 4:2:2 10-bit `3840 × 2560`) while keeping `48.44 MB` in `CD_DRAM` (sufficient for All-Intra `XAVC S-I 4K`, which needs `42.22 MB` CD and `0 MB` LD!).
- Across all **212** 4K entries in `av-cam.bin`'s `CreateCh` table, **211/212** modes (`XAVC HS 4K`, `XAVC S 4K`, `XAVC S-I 4K`) pass `createEncPool` cleanly with this single swap!

### B. `AV` Core (`av-cam.bin`) Patches
1. **`ENCHANDLER` 4K `key=2` Pixel & Pixel-Rate Ceilings (`0x217f5c`, `0x217f68`):**
   - `0x217f5c`: `0x870001` (`8,847,361` px) → `0xa1c001` (`10,600,449` px) so `3840 × 2560` (`9,830,400` px) stays in 4K `key=2` instead of jumping into 8K `key=3` (whose DMM units are empty on A7S III).
   - `0x217f68`: `0x1fa40000` (`530.84M` px/s) → `0x48000000` (`1.21G` px/s) so high frame rates up to 120p stay in `key=2`.
2. **`SFMC` & `IMLENC` Validators & Tables:**
   - `0x1a132c`: `sfmc_enc_util::is_3840x2160` (`cmp w8, #0x870 → #0xa00`).
   - `0x239900`, `0x2765c0`: `IMLENC` height checks (`0x870 → 0xa00`).
   - `0x354c04..0x354c18`: `sfmc_enc_config` `image_size_info` (`3840 × 2160 → 3840 × 2560`).
   - `0x38bc34`: `sfmc_enc_timing_normal::convert_ext_isp_clip_size` (`3840 × 2160 → QFHD 0x17`).
   - `0x38d968..0x3e9000`: Imola `CreateCh` / `EncStart` `.rodata` resolution pairs (`225` u32 + `641` u16 pairs).
   - Dual-core `EncStart` `Vsize_Core0` (`+0x56`): HEVC `1088 → 1280` (`63` entries), AVC `1072 → 1280` (`52` entries).
   - **HEVC `EncStart` Level (`+0x9e`):** `0x96 / 0x99 / 0x9c → 0xb4` (**HEVC Level 6.0**, `115` entries — required because `9,830,400 > 8,912,896` Level 5.1/5.2 `MaxLumaPS`, and natively supported by Sony's in-camera HEVC decoder!). Original stock bytes are saved in RAM for exact restoration when toggled Off.
3. **`VDT` (`FLOW_READ_STP_THRHOUGH_IMAGE`):**
   - `kAvText2560Patches` (12 sites) + `0x320964` (`2160 → 2560`).
   - **Bonus:** At `9.83M px > 8.85M px`, `vdt_flow_pcie_read_image` (`0x1c4bf8`) automatically selects DMM unit **`0x489` (`29.40 MB`)** instead of `0x488` (`12.66 MB`), which accommodates full `3840 × 2560` 10-bit 4:2:2 (`25.00 MB`) with zero DMM resizing!
4. **Always-On In-Camera Playback (`kAvPlayback2560`, 23 words in `av-cam.bin`):**
   - Stock `IMLDEC` routes `3840 × 2160` decode frames into DMM pool `0x369` (`12.66 MB`, 28 slots), whereas uncompressed `3840 × 2560` exceeds `12.66 MB` per slot and `IMLDEC` validators reject height `2560`.
   - `kAvPlayback2560` keeps `0x239900` stock (preserving `3840 × 2160` acceptance), extends `IMLDEC` validators (`0x227834..0x2765c0`) to accept both `2160` and `2560`, and routes `3840 × 2560` decode buffers into `DMM 0x369` using Sony's native **EBC `1.25 Bpp` (`5/4` compression, `GET_PIC_NOTIFY [x25, #0x28] = 5`)** (`12.50 MB ≤ 12.66 MB` per slot!). This patch stays permanently enabled so both Open Gate `3840 × 2560` and stock `3840 × 2160` clips play back seamlessly on camera regardless of the menu toggle state.

### C. `MP` Linux (`appFw.so` + `libmpr.so`) Patches
- `appFw.so` (14 sites) + `libmpr.so` (33 sites): updates resolution lookups, muxer `stsd`/`tkhd`, and playback resolution checks (`0x2469b4`, `0x246a9c`, `0x246c80`) from `2160` to `2560`.
- `libmpr.so` HEVC `level_idc` (`0x223a78`, `0x223a94`, `0x223a9c`): emits **Level 6.0 (`180`)** in `hvcC`.
- **Important H.264 Playback Discovery:** `libmpr.so`'s AVC Long-GOP playback validator (`0x246c58 → 0x3be370`) matches exact `(width, height, chroma, bitdepth, fps, bitrate, level)` keys and expects stock H.264 Level `5.1` (`25p`) / `5.2` (`50p`) rather than `6.0`. Keeping H.264 `level_idc` stock (while elevating HEVC to `6.0`) preserves valid recording AND prevents in-camera `Movie Error` on H.264 playback.
- `MP_tkhd` and `AV_VUI_SAR` remain **100% stock (`1:1` SAR)** — no anamorphic flags needed.
- **Full-Screen Unsquished `3:2` Display & Focus Magnifier (`hook_3519d94` on CP VDF IPC `0x3519d94`):**
  - Disassembling CP's `VdfInputCmdSetYuvLayoutOption` (`CMD=0x0147`, `cpapp-b.bin @ 0x060fcc8`) revealed why earlier layout-table edits still letterboxed Movie Live View and Playback: when `ent[1] == 1` (`auto-fit`, `0x061017e`), CP ignores `ent[0x0c..0x13]` and recalculates a `16:9` letterbox from its internal source aspect; when **`ent[1] == 0` (`manual rect`, `0x01e5068`)**, CP maps the full source `(0, 0, 640, 480)` directly onto the specified destination rectangle.
  - `hook_3519d94` intercepts `CMD=0x0147` (`size == 0x308`) and `CMD=0x014c` (`size == 0x28`) whenever Open Gate is active and `SYSTEM_MODE != 2` (Still Photo), setting `ent[1] = 0` with `dst = (0, 26, 640, 428)` on Rear LCD (`OUT=1`) & EVF (`OUT=2`) and `dst = (50, 0, 540, 480)` on 16:9 HDMI (`OUT=4`), while patching `0x4b72818` Row 2 (`0x19` / `0x1b`) and Row 7 (Focus Magnifier 1.0× box & 4.0× zoom) so OSD/AF/minimap overlays align with `3:2`.
  - **Focus Magnifier Live-Frame Gate (`cpapp-b.bin @ 0x001e9204`):** When Focus Magnifier enters active manual-rect mode (`CMD=0x014c`, `msg[0x0a] = 0`, `fit_mode = 0`), CP VDF enables a strict per-frame check (`"VDF : PIN Aspect %d, Dev Aspect %d, Dev %d"`) that drops every incoming live YUV frame whenever `PIN Aspect` (from CP VEF, `1`) differs from `msg[0x1c]` (`Dev Aspect`). Preserving Focus Magnifier's `SRC` crop rectangle along with stock `msg[0x1c] = 1` and `msg[0x20] = 3` (and leaving exit `F=1, msg[0x0a]=1` untouched so CP reloads `CMD=0x0147`) keeps 1.0× box, 4.0× zoom, joystick panning, and exit running live at full frame rate with zero freezes.
- **Glitch-Free Atomic Cold-Boot & Mode Transitions:**
  - On cold boot (after `persistent_preload_v2` injects `og_menu_daemon.so` into `im.elf`), once Movie Live View is idle (`SYS=3, OPT=0, B340=0, EEB10!=0, REC360=0`), `execute_sensor_mode_bounce()` enqueues Sony's native **`model::sequence_set_movie_format`** (`cmd 0x0f3`), **`model::sequence_restart_ee`** (`cmd 0x2bf`), and **`model::sequence_change_hdmi_resolution`** (`cmd 0x0ee`) atomically in a single FIFO flush under recursive mutex `0x2e7849c`/`0x2e78178`, waits for EE completion (`SYS=3, B340=0, EEB10!=0`), and issues one synchronized post-EE `refresh_yuv_viewport()` — booting directly into clean unsquished `3:2` Live View without starting recording, touching SD media, or racing `sequence_mode_transition` on Playback → Movie return.

### D. `CP` Real-Time ISP (`cpapp-b.bin`) Patches
- **3:2 Sensor Readout (`kCpPatches`, 25 patches):** configures IMX510 SPI readout (`4240 × 2826`) and dual-stripe `0820_L_CLV` / `0820_R_CLV` scalers.
- **Surgical Movie YC / VEF Scaler (`kCpSurgicalVef2560`, 37 patches):** updates `mm open yc` (`0x1c4898/b0`), VEF scaler (`0x631c08/30`), and Movie YC / MB grid tables (`0xb9f8e8..0xff32d4`, `240×135 → 240×160`) while strictly excluding `CAMLV` (`0x40afae`), `JPEG` (`0xb76e20`), and `VOUT/HDMI` CEA-861 timing tables (`0xd2d8dc..0xd2daf8`, which hang VSYNC if modified).
- **Full-Sensor Hardware Defect Pixel Correction (`CP_DDX`, 4 keys):** at cold boot (`t ≈ 1 s`, before `og_menu_daemon.so` loads), CP's `ZIT`/`DDX` initializer (`0x007ae0d8 → 0x003063a8`) builds mode `0x0820` defect buckets in `0x01260fc8` (factory) and `0x012630b8` (dynamic Pixel Mapping) cropped to stock `16:9` (`y_min = 220, h = 2408`, shifted by `-220`). Redirecting the `0x00000820` (`flg = 1, 2`) bucket indices in both dictionaries to `B[0]` (`f0 = 0x02, rect = (0, 0, 0, 0)`, the full-sensor uncropped `4264 × 2848` DDX map) causes `program_ddx_hw` (`0x007ab542`) to program exact `(live_y = 0, live_x >> 2)` hardware offsets and eliminate hot pixels across all 2,826 rows (restoring saved stock bucket indices when toggled Off).

---

## 2. Verified Live Recording Metrics (`ILCE-7SM3`, FW `5.01`)

| Format | Coded Resolution | SAR / DAR | Profile / Level | Bitrate | Frame Integrity (`ffmpeg`) |
|---|---|---|---|---|---|
| **XAVC HS 4K 50p 4:2:2 10-bit** | **`3840 × 2560`** | **`1:1` / `3:2`** | `hevc` (`hvc1` Rext `yuv422p10le`, **L6.0 / `180`**) | `200.5 Mbps` | 432/432 frames, **0 errors**, 0 black bars, **plays on camera!** |
| **XAVC S 4K 25p 4:2:0 8-bit** | **`3840 × 2560`** | **`1:1` / `3:2`** | `h264` (`avc1` High `yuv420p`) | `97.4 Mbps` | 96/96 frames, **0 errors**, 0 black bars |
| **XAVC S 4K 50p 4:2:0 8-bit** | **`3840 × 2560`** | **`1:1` / `3:2`** | `h264` (`avc1` High `yuv420p`) | `151.3 Mbps` | 216/216 frames, **0 errors**, 0 black bars |

All clips preserve 48 kHz 16-bit LPCM audio and full Sony `rtmd` gyro/lens metadata (Catalyst Browse / Gyroflow compatible).

---

## 3. Standalone Research Harness (Optional)

The staged volatile-RAM test harness (`runtime/og_3840x2560_v2.cpp`, `runtime/ulog_spy.cpp`, `analysis/run_2560_experiment.py`) remains in the repository for researchers exploring additional modes (e.g., `HEVC 25p/30p`, `XAVC S-I 4K`, `100p/120p`):

```bash
# Inspect live RAM patch status
sudo .venv/bin/python analysis/run_2560_experiment.py status

# Dump live AV core ulogio ring buffer (0x405000000)
sudo .venv/bin/python analysis/run_2560_experiment.py dumplog

# Restore 100% stock 3840x2160 in RAM
sudo .venv/bin/python analysis/run_2560_experiment.py restore
```

---

## 4. Resolved Hardware Bugs (v20–v23)

1. **Hot Pixels in Open Gate Recordings (`cpapp-b.bin` `ZIT`/`DDX` Defect Pixel Correction) — RESOLVED:**
   - At cold boot (`t ≈ 1 s`, before `og_menu_daemon.so` loads), CP's defect initialization (`0x007ae0d8 → 0x007ae158 → 0x00307830 → 0x0012f820 → cutOutDefectPos 0x003063a8`) builds the in-RAM defect dictionaries at `0x01260fc8` (factory) and `0x012630b8` (user Pixel Mapping) using stock `16:9` tables (`t14_10 @ 0x00c7078c`: `y_min = 220`, `h = 2408`), discarding defects outside `[220, 2628)` and shifting coordinates by `-220`.
   - Fixed by redirecting the `0x00000820` (`flg = 1, 2`) `uint16_t bucket_idx` entries in both dictionaries to `B[0]` (the full-sensor uncropped `4264 × 2848` DDX map) when Open Gate is enabled, and restoring the saved stock bucket indices when toggled Off.
2. **Cold-Boot & Playback → Movie Visual Glitches — RESOLVED:**
   - Removed the asynchronous `cur_sys != prev_sys` `refresh_yuv_viewport()` race (letting `hook_3519d94` rewrite Sony's own synchronized `CMD=0x0147` cleanly) and combined `sequence_set_movie_format` (`cmd 0x0f3`), `sequence_restart_ee` (`cmd 0x2bf`), and `sequence_change_hdmi_resolution` (`cmd 0x0ee`) into a single atomic FIFO flush followed by one synchronized post-EE viewport push, booting straight into clean unsquished `3:2` Live View.
3. **Focus Magnifier Freezing Live View in Open Gate Movie Mode — RESOLVED:**
   - Patched `0x4b72818` Row 7 (Focus Magnifier 1.0×/4.0× layout slots) to `0x19` (LCD/EVF `3:2`) and `0x1b` (HDMI `3:2`), preserved Focus Magnifier `SRC` crop coordinates in `hook_3519d94`, and kept stock `msg[0x1c] = 1` (`Dev Aspect`) + `msg[0x20] = 3` so CP VDF's `0x001e9204` (`"VDF : PIN Aspect %d, Dev Aspect %d"`) live-frame check passes on every frame.
