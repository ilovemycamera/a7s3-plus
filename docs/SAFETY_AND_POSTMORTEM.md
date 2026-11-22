# Flash Layout, Safety Design & Bootloop Post-Mortem

Camera firmware hacking carries real risk if you misunderstand what is volatile RAM and what is backed by persistent NAND flash. This document shares everything we learned about the Sony A7S III (`ILCE-7SM3`) storage layout — including an honest post-mortem of how an early persistent-boot experiment caused a 6.6-second USB bootloop, how we recovered it without opening the camera, and why **`a7s3-plus` runs 100% in volatile RAM by default**.

---

## 1. The Surprise in Sony's `fstab`: `/tmp` and `/etc` Are Persistent Flash!

On a typical embedded Linux device, `/tmp` and `/var` are `tmpfs` (RAM) and `/etc` is read-only squashfs. **Not on Sony Bionz XR!**

Inspecting `sbin/initrc/fstab` from `nflasha7` revealed:

| Mount Point | Partition | Filesystem | Persistence Across Reboot | Notes |
|---|---|---|---|---|
| `/usr` | `nflasha15` | `ext2` (`ro`) | Read-Only Flash | Contains `im.elf`, `appFw.so`, `libmpr.so`, `/usr/kmod/*.ko`. |
| `/setting` | `nflasha2` | `vfat` (`rw`) | **Persistent Flash!** | Factory & runtime config (`DmmCfg.bin`, `/setting/mode/*`). |
| `/etc`, `/tmp`, `/var` | `nflasha10` | `vfat` (`rw`) | **Persistent Flash!** | All three share a single writable FAT partition on NAND! |
| `/tmp/jiritsu` | `nflasha6` | `vfat` (`rw`) | **Persistent Flash!** | Service/diagnostic workspace used by `senser`. |
| `/tmp_wbshd` | `tmpfs` (`rw`) | **True Volatile RAM** | **Wiped on Reboot** | Actual RAM-backed `tmpfs`! |

> [!IMPORTANT]
> Any file written to `/etc/ld.so.preload` or `/tmp/*` **survives power-cycling** because `nflasha10` is persistent NAND flash! Only `/tmp_wbshd` and process memory (`im.elf` pages, `CP` RAM, `AV` DDR `0x401000000`) are truly volatile.

---

## 2. Post-Mortem: The Two Bugs Behind Our Early Persistent-Boot Incident

During early testing of an optional cold-boot auto-loader (`/etc/ld.so.preload` → `/setting/mode/libcinema.so`), the camera entered a reboot loop whenever a USB cable was plugged in (`~6.6s` watchdog cycle, enumerating briefly as `054c:096f` HID before resetting), while booting normally without USB.

### Root Cause 1: Hooking USB Service Helper Processes (`pam_tally2` / `adjstctl.elf`)
- Our early `persistent_preload.c` had three `argv[0]` branches:
  1. `bootin.elf` (early UXC bind-mount) — **safe**.
  2. `im.elf` (spawns deferred worker thread for `appFw.so` patches) — **safe**.
  3. `pam_tally2` → `execve("/usr/bin/adjstctl.elf")` with `LD_PRELOAD=jit_inject.so` — **UNSAFE!**
- When USB was connected, Sony's USB/service stack invoked `pam_tally2` / `adjstctl.elf`. Re-execing `adjstctl.elf` under `jit_inject.so` crashed a critical service process, triggering the hardware watchdog every `6.6s`.
- Meanwhile, our host polling script (`test_local_ipc.py`) was polling `senser` every `20 ms` with zero backoff and filtering out PID `0x0d68`, starving USB enumeration!

### Root Cause 2: The `SET_FILE2` Relative-Path Trap in `stand.write_file()`
- Sony's `senser` `SET_FILE2` file-write command chroots/anchors relative paths inside `/tmp/jiritsu/` (`nflasha6`)!
- Passing `"/setting/mode/foo"` without a `../../` traversal prefix silently wrote to **`/tmp/jiritsu/setting/mode/foo`** instead of the real `/setting/mode/foo` (`nflasha2`) — and reading back via the same `GET_FILE` API read back the shadowed copy, masking the bug!

### How We Recovered & Hardened Everything
1. Caught the ~2-second `senser` window between watchdog reboots (`analysis/recovery_restore.py`), deleted `/etc/ld.so.preload`, reset `/setting/mode/dmode` to `3`, emptied `/setting/mode/preload`, and ran `sync`.
2. **Removed `/etc/ld.so.preload` persistence from default operation:** `load_opengate_profile.py` explicitly runs `rm -f /etc/ld.so.preload` on every run so your camera always boots 100% stock.
3. **Stripped `pam_tally2` / `jit_inject` completely** from [`runtime/persistent_preload.c`](../runtime/persistent_preload.c) and moved runtime flags/logs to `/tmp_wbshd` (real `tmpfs`).
4. **Added path-traversal normalization & guards** in [`analysis/stand.py`](../analysis/stand.py) so writes outside `/tmp/jiritsu/` cannot silently land in shadow directories.
5. **Mandatory `check_coldboot.py` logging:** Every run logs `dmode`, `preload_status`, and `/etc` state to `analysis/coldboot_check.log` and `analysis/coldboot_history.log`.

---

## 3. Fail-Safe Persistence v2 (`--persist` / `--disarm`)

After dissecting `bootin.elf`, `im.elf`, `libInfraSSHManager.so`, and Sony's `WBI2` (`nflasha5`) fast-boot hibernation flow, we designed **Persistence v2** to survive power-off and battery pulls **without** disabling Sony's `0.5s` WBI2 boot and **without** breaking USB:

1. **Zero-Import Preload Shim ([`runtime/persistent_preload_v2.c`](../runtime/persistent_preload_v2.c) → `/setting/mode/libcinema_preload.so`):**
   - Compiled with `-nostdlib -ffreestanding -fno-builtin`: **0 `DT_NEEDED`, 0 undefined symbols**. It cannot cause linker failures in `busybox`, `fserr.sh`, `pam_tally2`, or any minimal binary.
   - Claims an atomic one-shot lock in **volatile RAM `tmpfs`** (`/tmp_wbshd/.a7s3_armed`, `O_CREAT | O_EXCL`), so it triggers **strictly once** per power-on (`fserr.sh` / `ssh-keygen` / `pam_tally2` / `btproperty` on WBI2 warm boot; `im.elf` on cold boot).
   - Performs a clean **`double-fork()` + `setsid()` + closes `fd 0..63`**, letting the parent process immediately resume 100% stock execution.
2. **Standalone Zero-DMM OSAL UIPC Injector ([`runtime/a7s3_wake_inject.c`](../runtime/a7s3_wake_inject.c) → `/setting/mode/a7s3_wake_inject`):**
   - Linked directly against stock `libtestcmd.so` + `libosal_uipc.so` + `libc.so.6` (no `LD_PRELOAD` hijacking of `adjstctl.elf`).
   - **Zero-DMM Off-USB Live-View Compatibility (`JIRITSU_START 0x0004`):** Sony's 2-phase `SET_PATH`/`SET_FILE2` commands allocate a 5 MiB DMM buffer (unit `0x304`, `15000 ms` timeout) from the shared imaging pool (`0x430400000`), which blocks while Live View is active off-USB. Instead, `a7s3_wake_inject` seeds a zeroed `0x68`-byte `CImJiritsu` struct (`state = 0x0a`) into the unused tail of `libJiritsu.so`'s `rw-p` BSS page (`jbase + 0x47c00`), writes `/setting/mode/libjit_warm.so` directly to `/tmp/jiritsu/libJit.so`, and fires a 1-phase **`JIRITSU_START(0)` (`0x0004`)** message — injecting `libcinema_worker.so` inside `im.elf` in ~20 ms with **zero DMM allocation** even in full Live View (all 6 modules armed ~3 s after power-on).
   - **Auto-Disarm Dead-Man's Switch:** Increments `/setting/mode/boot_tries` before injecting, and only resets it to `0` after `og_menu_daemon.so` is confirmed inside `im.elf` **plus 8 seconds of stable uptime** (past the 6.6s watchdog). If `boot_tries >= 2`, it automatically deletes `/etc/ld.so.preload`, syncs flash, and boots 100% stock.
   - **Physical SD-Card Kill-Switch:** Scans `/proc/mounts` for mounted media cards; if an empty `NO_MOD` or `a7s3_disable.txt` file exists on any card, it immediately deletes `/etc/ld.so.preload` and restores stock boot **without needing USB**.
   - **Clean Cleanup & USB Coexistence:** `libjit_warm.so` exports `CreateScriptI`/`LoadScriptI`/`DestroyScriptI` (without `RTLD_NODELETE`). Once modules are armed, `cinema_worker.c` calls `CImJiritsu::exit()` (`jbase + 0xb68c`) and resets `g_pImJInst = NULL`, cleanly `dlclose`ing `libjit_warm.so`, unmounting `ramdsk_libJit`, joining `jit_thread`, and keeping all future USB `senser` / `libJit.so` sessions 100% operational.
3. **In-Process Worker ([`runtime/cinema_worker.c`](../runtime/cinema_worker.c) → `/setting/mode/libcinema_worker.so`):**
   - Loaded strictly inside `im.elf` with `RTLD_NODELETE`, sequentially arms `menu_repair.so` → `high_res_display.so` → `og_menu_daemon.so` → `big6_key_hook.so` → `doom_a7s3_v17.so` (if WAD is staged), then tears down the temporary `jit_thread` & ramdisk.
