#!/usr/bin/env python3
import os
import sys
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOG_PATH = ROOT / 'analysis' / 'last_load_output.txt'


class Tee:
    def __init__(self, stream, log_file):
        self.stream = stream
        self.log_file = log_file

    def write(self, data):
        self.stream.write(data)
        self.stream.flush()
        self.log_file.write(data)
        self.log_file.flush()

    def flush(self):
        self.stream.flush()
        self.log_file.flush()


log_fh = open(LOG_PATH, 'w', encoding='utf-8')
try:
    os.chmod(LOG_PATH, 0o666)
except Exception:
    pass
sys.stdout = Tee(sys.__stdout__, log_fh)
sys.stderr = Tee(sys.__stderr__, log_fh)

for sp in (ROOT / '.venv/lib').glob('python*/site-packages'):
    sys.path.insert(0, str(sp))
if (ROOT / 'vendor').exists():
    sys.path.insert(0, str(ROOT / 'vendor'))
sys.path.insert(0, str(ROOT / 'analysis'))
sys.path.insert(0, str(ROOT / 'sony-pmca-re'))

from stand import complete, read_file, write_file, unmount_pmhome, run_script, LIBJIT
from pmca.commands.usb import senserShellCommand

ENABLE_PERSISTENT = '--persistent' in sys.argv
pos_args = [a for a in sys.argv[1:] if not a.startswith('--')]
ARG = pos_args[0] if pos_args else 'doom'
if ARG == 'all':
    ARG = 'doom'
if ARG in ('1', '6', 'fhd1', 'noaction'):
    sys.exit(f'Refusing profile {ARG}: unsafe on A7S III (use doom, og_menu, dci4k, fhd6, 2, 3, or base).')


def ensure_doom_wad_gz():
    """Locate or compress user-supplied DOOM1.WAD / freedoom1.wad into runtime/doom1.wad.gz."""
    import gzip
    import struct
    gz_path = ROOT / 'runtime' / 'doom1.wad.gz'
    candidates = [
        ROOT / 'DOOM1.WAD', ROOT / 'doom1.wad', ROOT / 'freedoom1.wad', ROOT / 'DOOM.WAD', ROOT / 'doom.wad',
        ROOT / 'runtime' / 'DOOM1.WAD', ROOT / 'runtime' / 'doom1.wad', ROOT / 'runtime' / 'freedoom1.wad',
    ]
    for cand in candidates:
        if cand.is_file() and cand.stat().st_size > 100_000:
            raw = cand.read_bytes()
            if not gz_path.exists() or cand.stat().st_mtime > gz_path.stat().st_mtime:
                print(f'[WAD] Compressing {cand.name} ({len(raw)} bytes) -> runtime/doom1.wad.gz ...')
                gz_path.write_bytes(gzip.compress(raw, compresslevel=9))
            return gz_path, len(raw)
    if gz_path.is_file() and gz_path.stat().st_size > 50_000:
        raw_sz = struct.unpack('<I', gz_path.read_bytes()[-4:])[0]
        return gz_path, raw_sz
    return None, 0


if ARG == 'doom':
    _wad_gz, _wad_sz = ensure_doom_wad_gz()
    if _wad_gz is None:
        print(
            '[NOTE] No DOOM1.WAD / freedoom1.wad found in repository root or runtime/.\n'
            '       Falling back to "og_menu" (Open Gate 3:2 + Native Menu Toggle + Cinema Unlocks).\n'
            '       To enable 1993 DOOM (E1M1), drop DOOM1.WAD or freedoom1.wad into this folder and re-run!'
        )
        ARG = 'og_menu'

Z256 = ' '.join(['00'] * 256)
P_E011 = ' '.join(['11' if i == 0x8a else ('e0' if i == 0x8b else ('01' if i == 0x93 else '00')) for i in range(256)])
P_5020 = ' '.join(['20' if i == 0x8a else ('50' if i == 0x8b else ('01' if i == 0x93 else '00')) for i in range(256)])
RESET_BK_SH = f"#!/bin/sh\n/tmp/jiritsu/bk.elf wc 00030072 {Z256} >/dev/null 2>&1\n"

CAMERA_RECORD_CMD = (
    "L=/tmp/jiritsu/auto-cycle.log; F=/tmp/jiritsu/failsafe.pid; "
    "K='/tmp/jiritsu/inject-key 0x807a0020 0x92840747'; "
    "rm -f $L $F; "
    "(sleep 35; echo 'FAILSAFE REBOOT' >> $L; /bin/sh /tmp/jiritsu/reset_bk.sh; /bin/busybox sync; /usr/bin/ud_send_lsi.elf 0; sleep 1; /bin/busybox reboot -f) </dev/null >/dev/null 2>&1 & "
    "echo $! > $F; "
    "("
    "echo '1: sleep 3 in Senser Movie Live View' > $L; "
    "sleep 3; "
    "echo '2: inject MOVIE REC (0x01f0) START tap' >> $L; "
    "$K 0x01f0 0 1 0 1 >> $L 2>&1; $K 0x01f0 0 0 0 1 >> $L 2>&1; "
    "sleep 5; "
    "echo '3: inject MOVIE REC (0x01f0) STOP tap' >> $L; "
    "$K 0x01f0 0 1 0 1 >> $L 2>&1; $K 0x01f0 0 0 0 1 >> $L 2>&1; "
    "sleep 4; "
    "/bin/sh /tmp/jiritsu/reset_bk.sh; "
    "/bin/busybox sync; "
    "echo '4: recording + sync complete, rebooting back to USB MSC' >> $L; "
    "/usr/bin/ud_send_lsi.elf 0 >> $L 2>&1; sleep 1; /bin/busybox sync; /bin/busybox reboot -f"
    ") </dev/null >/dev/null 2>&1 &"
)

ran_ok = False


def load_one_so(camera, so_path: Path, wait_s: float = 2.5):
    payload = so_path.read_bytes()
    print(f'[STEP] Loading {so_path.name} ({len(payload)} bytes) via LIBJIT...')
    res = write_file(camera, LIBJIT, payload)
    if res != b'\x00\x00\x00\x00':
        raise RuntimeError(f'write LIBJIT ({so_path.name}) failed: {res.hex()}')
    time.sleep(wait_s)
    print(f'[STEP] Finished {so_path.name} (waited {wait_s}s)')


def ensure_base_loaded(camera):
    ram_chk = run_script(
        camera,
        'grep -q og_menu_daemon /proc/*/maps 2>/dev/null && cat /tmp_wbshd/menu_repair_ram.ok 2>/dev/null || true',
        wait=0.8,
    ).strip()
    if ram_chk == '1' and ARG not in ('base', 'og_menu', 'dci4k', 'dci3_rec', 'modee011_rec', 'mode5020_rec', 'sethd', 'fhd6', 'fhd6_rec'):
        print('Base modules already armed in RAM (/proc/*/maps + /tmp_wbshd/menu_repair_ram.ok), skipping reload')
        return
    if ARG in ('dci4k', 'dci3_rec', 'modee011_rec', 'mode5020_rec'):
        write_file(camera, '/tmp/jiritsu/set-dci4k.flag', b'1\n')
    elif ARG in ('sethd', 'fhd6', 'fhd6_rec'):
        write_file(camera, '/tmp/jiritsu/set-hd.flag', b'1\n')
    write_file(camera, '/tmp/jiritsu/menu-stage.txt', b'PENDING\n')
    write_file(camera, '/tmp/jiritsu/menu-repair.txt', b'PENDING\n')
    write_file(camera, '/tmp/jiritsu/high-res-display.txt', b'PENDING\n')
    for name in ('menu_repair.so', 'high_res_display_v10.so', 'big6_key_hook.so'):
        so_path = ROOT / 'runtime' / name
        if not so_path.exists():
            continue
        load_one_so(camera, so_path, 2.5)
        print(f'Loaded {name} ({so_path.stat().st_size} bytes)')
    for tag_file in ('menu-stage.txt', 'menu-repair.txt', 'high-res-display.txt'):
        val = (read_file(camera, f'/tmp/jiritsu/{tag_file}') or b'').decode('ascii', 'replace').strip()
        print(f'{tag_file}: {val}')
    run_script(camera, 'echo 1 > /tmp_wbshd/menu_repair_ram.ok', wait=0.5)


def upload_chunked_gz(camera, gz_path: Path, remote_target: str, expected_size: int = 0):
    if expected_size > 0:
        chk = run_script(camera, f"wc -c < {remote_target} 2>/dev/null || echo 0", wait=1.5).strip()
        if chk.endswith(str(expected_size)):
            print(f'{remote_target} already present ({expected_size} bytes), skipping upload')
            return
    data = gz_path.read_bytes()
    chunk_sz = 65536
    chunks = [data[i:i + chunk_sz] for i in range(0, len(data), chunk_sz)]
    print(f'Uploading {gz_path.name} ({len(data)} bytes in {len(chunks)} chunks of 64KB)...')
    for idx, ch in enumerate(chunks):
        res = write_file(camera, f'/tmp/jiritsu/gzpart_{idx:03d}', ch)
        if res != b'\x00\x00\x00\x00':
            raise RuntimeError(f'chunk {idx} write failed: {res.hex()}')
    out = run_script(
        camera,
        f"rm -f {remote_target}; cat /tmp/jiritsu/gzpart_* | /bin/busybox gunzip -c > {remote_target} 2>&1; "
        f"rm -f /tmp/jiritsu/gzpart_*; chmod 755 {remote_target}; ls -l {remote_target}",
        wait=3.0,
    ).strip()
    print(f'Unpacked -> {out}')


def runner(camera):
    global ran_ok
    complete(camera)
    write_file(camera, '/tmp/jiritsu/a7c2-exec.out', b'ok\n')
    pp_log = (read_file(camera, '/tmp/jiritsu/persistent-preload.log') or b'').decode('utf-8', 'replace').strip()
    cb_once = (read_file(camera, '/tmp/jiritsu/cb_once') or b'').decode('utf-8', 'replace').strip()
    bt_log = (read_file(camera, '/proc/snsc_boot_time') or b'').decode('utf-8', 'replace').strip()
    print(f'=== BOOT DIAG: pp_log={pp_log!r} cb_once={cb_once!r} ===\n{bt_log[:600]}\n=== END BOOT DIAG ===')
    write_file(camera, '/tmp/jiritsu/reset_bk.sh', RESET_BK_SH.encode('ascii'))
    if ARG in ('doom', 'doom_log', 'doom_show', 'doom_hide', 'doom_mode0', 'doom_mode1', 'doom_mode2', 'doom_mode3'):
        if ARG in ('doom_show', 'doom_hide'):
            cmd_ch = 'v' if ARG == 'doom_show' else 'h'
            write_file(camera, '/tmp/jiritsu/doom_v14_mode', f'{cmd_ch}\n'.encode('ascii'))
            time.sleep(1.2)
            out = (read_file(camera, '/tmp/jiritsu/doom.log') or b'').decode('utf-8', 'replace')
            print(f'=== Sent {ARG} ({cmd_ch}) ===\n{out}')
            ran_ok = True
            return
        if ARG.startswith('doom_mode'):
            m = ARG[-1]
            write_file(camera, '/tmp/jiritsu/doom_v14_mode', f'{m}\n'.encode('ascii'))
            time.sleep(1.5)
            out = (read_file(camera, '/tmp/jiritsu/doom.log') or b'').decode('utf-8', 'replace')
            print(f'=== Switched doom_v14_mode={m} ===\n{out}')
            ran_ok = True
            return
        if ARG == 'doom_log':
            out = (read_file(camera, '/tmp/jiritsu/doom.log') or b'').decode('utf-8', 'replace')
            ldr = (read_file(camera, '/tmp/jiritsu/doom-loader.txt') or b'').decode('utf-8', 'replace')
            dmn = (read_file(camera, '/tmp/jiritsu/og-menu-daemon.txt') or b'').decode('utf-8', 'replace')
            vdf = (read_file(camera, '/tmp/jiritsu/vdf-log.txt') or b'').decode('utf-8', 'replace')
            print(f'=== doom-loader.txt ===\n{ldr}\n=== og-menu-daemon.txt ===\n{dmn}\n=== vdf-log.txt ===\n{vdf}\n=== doom.log ===\n{out}')
            ran_ok = True
            return
        v17_loaded = run_script(
            camera,
            'grep -q doom_a7s3_v17.so /proc/*/maps 2>/dev/null && echo LOADED || echo NOT_LOADED',
            wait=1.0,
        ).strip()
        print(f'doom_a7s3_v17.so status in /proc/*/maps: {v17_loaded}')
        prev_log = (read_file(camera, '/tmp/jiritsu/doom.log') or b'').decode('utf-8', 'replace')
        print(f'=== PREVIOUS doom.log BEFORE v17 ===\n{prev_log}')
        if 'NOT_LOADED' in v17_loaded:
            write_file(camera, '/tmp/jiritsu/doom_mode', b'0\n')
            write_file(camera, '/tmp/jiritsu/doom_v2_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v3_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v4_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v5_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v6_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v7_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v8_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v10_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v11_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v12_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v13_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v14_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v15_mode', b'q\n')
            write_file(camera, '/tmp/jiritsu/doom_v16_mode', b'q\n')
            time.sleep(0.8)
            run_script(camera, 'rm -f /tmp/jiritsu/doom_a7s3_v*.so /tmp/jiritsu/doom_v3_mode /tmp/jiritsu/doom_v4_mode /tmp/jiritsu/doom_v5_mode /tmp/jiritsu/doom_v6_mode /tmp/jiritsu/doom_v7_mode /tmp/jiritsu/doom_v11_mode /tmp/jiritsu/doom_v12_mode /tmp/jiritsu/doom_v13_mode /tmp/jiritsu/doom_v14_mode /tmp/jiritsu/doom_v15_mode /tmp/jiritsu/doom_v16_mode /tmp/jiritsu/doom_v17_mode /tmp/menu_repair_ram.ok /tmp_wbshd/menu_repair_ram.ok', wait=0.5)
        else:
            run_script(camera, 'rm -f /tmp/menu_repair_ram.ok /tmp_wbshd/menu_repair_ram.ok', wait=0.5)
        ensure_base_loaded(camera)
        og_loaded = run_script(
            camera,
            'grep -q og_menu_daemon_v15.so /proc/*/maps 2>/dev/null && echo LOADED || echo NOT_LOADED',
            wait=1.0,
        ).strip()
        daemon_path = ROOT / 'runtime' / 'og_menu_daemon_v15.so'
        loader_path = ROOT / 'runtime' / 'og_menu_loader.so'
        if 'NOT_LOADED' in og_loaded and daemon_path.exists() and loader_path.exists():
            write_file(camera, '/tmp/jiritsu/og_menu_daemon_v15.so', daemon_path.read_bytes())
            write_file(camera, '/tmp/jiritsu/og-menu-loader.txt', b'PENDING\n')
            write_file(camera, '/tmp/jiritsu/og-menu-daemon.txt', b'PENDING\n')
            load_one_so(camera, loader_path, 3.5)
            dmn_out = (read_file(camera, '/tmp/jiritsu/og-menu-daemon.txt') or b'').decode('ascii', 'replace').strip()
            print(f'=== OG MENU DAEMON ===\n{dmn_out}')
        else:
            print('og_menu_daemon_v15.so already running in RAM, skipping reload')
        def install_persistent_mode():
            from check_coldboot import COLDBOOT_DIAG_SCRIPT, save_coldboot_report
            for dst_name, src_name in (
                ('libcinema.so', 'persistent_preload.so'),
                ('libjit_warm.so', 'libjit_warm.so'),
                ('menu_repair.so', 'menu_repair.so'),
                ('high_res_display.so', 'high_res_display_v10.so'),
                ('og_menu_daemon.so', 'og_menu_daemon_v15.so'),
            ):
                p_bytes = (ROOT / 'runtime' / src_name).read_bytes()
                write_file(camera, '/tmp/jiritsu/stage_' + dst_name, p_bytes)
                print(f'Staged /tmp/jiritsu/stage_{dst_name} ({len(p_bytes)} bytes)')
            preload_cmd = (
                'if ! grep -qa pam_tally /setting/mode/libcinema.so 2>/dev/null; then '
                'printf "/setting/mode/libcinema.so\\n" > /etc/ld.so.preload && chmod 644 /etc/ld.so.preload; '
                'else rm -f /etc/ld.so.preload; fi; '
                if ENABLE_PERSISTENT
                else 'rm -f /etc/ld.so.preload; '
            )
            df_out = run_script(
                camera,
                'mkdir -p /setting/mode; '
                'rm -f /etc/ld.so.preload 2>/dev/null; '
                'rm -f /tmp/adj.elf /tmp/cinema-preload.active /tmp/inj.txt /tmp/ipc.txt /tmp/ipc0f.txt /tmp/jdata.bin /tmp/jiritsumode /tmp/jit_inject.so /tmp/jpath.bin /tmp/libcinema.so /tmp/menu_repair_ram.ok /tmp/out.txt /tmp/persistent-preload.log /tmp/start_out.txt /tmp/uipc_stat_kernel.txt 2>/dev/null; '
                'rmdir /tmp/a6_mnt /tmp/lib /tmp/log 2>/dev/null; '
                'chmod 777 /setting/mode 2>/dev/null; '
                'chmod 666 /setting/mode/* 2>/dev/null; '
                'rm -f /setting/mode/c3_high_res_hook.so /setting/mode/jit_inject.so /tmp/jiritsu/setting/mode/* 2>/dev/null; '
                'rmdir /tmp/jiritsu/setting/mode /tmp/jiritsu/setting /tmp/jiritsu/etc 2>/dev/null; '
                'for f in libcinema.so libjit_warm.so menu_repair.so high_res_display.so og_menu_daemon.so; do '
                'rm -f /setting/mode/$f; '
                'cp /tmp/jiritsu/stage_$f /setting/mode/$f; '
                'chmod 755 /setting/mode/$f; '
                'rm -f /tmp/jiritsu/stage_$f; '
                'done; '
                'cp /setting/mode/libcinema.so /setting/libcinema.so 2>/dev/null; '
                + preload_cmd +
                ': > /setting/mode/preload; '
                'printf "3\\n" > /setting/mode/dmode; '
                'chmod 644 /setting/mode/preload /setting/mode/dmode; '
                '/bin/busybox sync; '
                'ls -la /setting/mode/ /tmp/jiritsu/doom_a7s3_v17.so /tmp/jiritsu/doom1.wad 2>&1; '
                'cat /setting/mode/dmode /setting/mode/preload /etc/ld.so.preload 2>&1',
                wait=2.5,
            ).strip()
            print(f'=== PERSISTENT PRELOAD & STORAGE ===\n{df_out}')
            cb_diag = run_script(camera, COLDBOOT_DIAG_SCRIPT, wait=2.5)
            save_coldboot_report(cb_diag)

        if 'LOADED' in v17_loaded and 'NOT_LOADED' not in v17_loaded:
            install_persistent_mode()
            cur = (read_file(camera, '/tmp/jiritsu/doom.log') or b'').decode('utf-8', 'replace')
            print(f'=== doom_a7s3_v17.so already running in RAM; menu_repair + persistent_preload installed! ===\n{cur}')
            ran_ok = True
            return
        wad_gz, wad_expected_sz = ensure_doom_wad_gz()
        if wad_gz is None:
            raise RuntimeError('Missing DOOM1.WAD / runtime/doom1.wad.gz')
        upload_chunked_gz(camera, wad_gz, '/tmp/jiritsu/doom1.wad', expected_size=wad_expected_sz)
        upload_chunked_gz(camera, ROOT / 'runtime' / 'doom_a7s3.so.gz', '/tmp/jiritsu/doom_a7s3_v17.so')
        install_persistent_mode()
        write_file(camera, '/tmp/jiritsu/doom-loader.txt', b'PENDING\n')
        write_file(camera, '/tmp/jiritsu/doom.log', b'')
        load_one_so(camera, ROOT / 'runtime' / 'doom_loader.so', 0.4)
        ldr_out = (read_file(camera, '/tmp/jiritsu/doom-loader.txt') or b'').decode('utf-8', 'replace').strip()
        print(f'=== doom-loader.txt ===\n{ldr_out}')
        print('=== Streaming /tmp/jiritsu/doom.log (every 0.25s) ===')
        printed_len = 0
        for _ in range(90):
            cur = (read_file(camera, '/tmp/jiritsu/doom.log') or b'').decode('utf-8', 'replace')
            if len(cur) > printed_len:
                sys.stdout.write(cur[printed_len:])
                sys.stdout.flush()
                printed_len = len(cur)
            if '[Standby] DOOM v17 ready!' in cur:
                print('\n>>> [OK] DOOM v17 is ready & hidden in MENU -> Setup -> Display Option -> 1993 DOOM (E1M1)! <<<')
                break
            time.sleep(0.25)
        ran_ok = True
        return
    if ARG == 'reset_bk':
        out = run_script(
            camera,
            "M=/tmp/jiritsu/bk.elf; echo '=== AUTO-CYCLE LOG ==='; cat /tmp/jiritsu/auto-cycle.log 2>&1; "
            "echo '=== 00030072 BEFORE ==='; $M rc 00030072 2>&1; "
            "echo '=== RESETTING 00030072 ==='; /bin/sh /tmp/jiritsu/reset_bk.sh 2>&1; $M s 2>&1; "
            "echo '=== 00030072 AFTER ==='; $M rc 00030072 2>&1; "
            "echo '=== 00bc0016 BASELINE ==='; $M rc 00bc0016 2>&1",
            wait=3.0,
        )
        print(out)
        ran_ok = True
        return
    if ARG in ('og', 'tkhd2560', 'og_menu'):
        prev_dmn = (read_file(camera, '/tmp/jiritsu/og-menu-daemon.txt') or b'').decode('ascii', 'replace').strip()
        prev_vdf = (read_file(camera, '/tmp/jiritsu/vdf-log.txt') or b'').decode('ascii', 'replace').strip()
        print(f'=== PREV OG MENU DAEMON LOG ===\n{prev_dmn}')
        print(f'=== PREV VDF LOG ===\n{prev_vdf}')
        run_script(camera, 'rm -f /tmp/menu_repair_ram.ok /tmp_wbshd/menu_repair_ram.ok', wait=0.5)
        ensure_base_loaded(camera)
        daemon_path = ROOT / 'runtime' / 'og_menu_daemon_v15.so'
        loader_path = ROOT / 'runtime' / 'og_menu_loader.so'
        res_d = write_file(camera, '/tmp/jiritsu/og_menu_daemon_v15.so', daemon_path.read_bytes())
        print(f'Uploaded og_menu_daemon_v15.so ({daemon_path.stat().st_size} bytes) -> {res_d.hex()}')
        write_file(camera, '/tmp/jiritsu/og-menu-loader.txt', b'PENDING\n')
        write_file(camera, '/tmp/jiritsu/og-menu-daemon.txt', b'PENDING\n')
        load_one_so(camera, loader_path, 4.5)
        ldr_out = (read_file(camera, '/tmp/jiritsu/og-menu-loader.txt') or b'').decode('ascii', 'replace').strip()
        dmn_out = (read_file(camera, '/tmp/jiritsu/og-menu-daemon.txt') or b'').decode('ascii', 'replace').strip()
        vdf_out = (read_file(camera, '/tmp/jiritsu/vdf-log.txt') or b'').decode('ascii', 'replace').strip()
        print(f'og-menu-loader.txt: {ldr_out}')
        print(f'=== OG MENU DAEMON ===\n{dmn_out}')
        print(f'=== VDF LOG ===\n{vdf_out}')
        ran_ok = True
        return
    if ARG in ('cp_status', 'cp_apply', 'cp_restore'):
        cmd_mode = ARG.split('_')[1]
        write_file(camera, '/tmp/jiritsu/cp_patch_cmd', f'{cmd_mode}\n'.encode('ascii'))
        write_file(camera, '/tmp/jiritsu/cp-opengate-patch.txt', b'PENDING\n')
        so_path = ROOT / 'runtime' / 'cp_opengate_patch.so'
        load_one_so(camera, so_path, 3.0)
        out = (read_file(camera, '/tmp/jiritsu/cp-opengate-patch.txt') or b'').decode('ascii', 'replace').strip()
        print(f'=== {ARG.upper()} ===\n{out}')
        ran_ok = True
        return
    if ARG in ('res2560_status', 'res2560_apply', 'res2560_restore', 'og2560', 'og2560_rec'):
        if ARG in ('og2560', 'og2560_rec'):
            ensure_base_loaded(camera)
            write_file(camera, '/tmp/jiritsu/cp_patch_cmd', b'apply\n')
            write_file(camera, '/tmp/jiritsu/cp-opengate-patch.txt', b'PENDING\n')
            load_one_so(camera, ROOT / 'runtime' / 'cp_opengate_patch.so', 3.0)
            cp_out = (read_file(camera, '/tmp/jiritsu/cp-opengate-patch.txt') or b'').decode('ascii', 'replace').strip()
            print(f'=== CP OG 3:2 ===\n{cp_out}')
            cmd_mode = 'apply'
        else:
            cmd_mode = ARG.split('_')[1]
        write_file(camera, '/tmp/jiritsu/res2560_cmd', f'{cmd_mode}\n'.encode('ascii'))
        write_file(camera, '/tmp/jiritsu/og-3840x2560-patch.txt', b'PENDING\n')
        so_path = ROOT / 'runtime' / 'og_3840x2560_patch.so'
        load_one_so(camera, so_path, 3.0)
        out = (read_file(camera, '/tmp/jiritsu/og-3840x2560-patch.txt') or b'').decode('ascii', 'replace').strip()
        print(f'=== {ARG.upper()} ===\n{out}')
        if ARG == 'og2560_rec':
            print('Waiting 2.5s after 3840x2560 patch, then recording 5s clip...')
            time.sleep(2.5)
            run_script(camera, CAMERA_RECORD_CMD, wait=1.5)
            ran_ok = True
            print('Armed 5s recording + sync + reboot in Senser Movie Live View; waiting 22s...')
            time.sleep(22.0)
            return
        ran_ok = True
        return
    if ARG == 'record':
        print('Entered Senser (Movie Live View); waiting 2s for mode transition...')
        time.sleep(2.0)
        run_script(camera, CAMERA_RECORD_CMD, wait=1.5)
        ran_ok = True
        print('Armed 5s recording + sync + reboot in Senser Movie Live View; waiting 22s...')
        time.sleep(22.0)
        return

    prof_arg = 'fhd6' if ARG == 'fhd6_rec' else ('dci3' if ARG in ('dci3_rec', 'modee011_rec', 'mode5020_rec') else ARG)
    if ARG in ('modee011_rec', 'mode5020_rec'):
        patch_str = P_5020 if ARG == 'mode5020_rec' else P_E011
        out_bk = run_script(
            camera,
            f"M=/tmp/jiritsu/bk.elf; $M wc 00030072 {patch_str} 2>&1; $M rc 00030072 2>&1",
            wait=2.5,
        )
        print(f'Set 00030072 ({ARG}):', out_bk)
    ensure_base_loaded(camera)
    if prof_arg not in ('base', 'dci4k', 'sethd'):
        so_path = ROOT / 'runtime' / f'opengate_profile_{prof_arg}.so'
        if not so_path.exists():
            sys.exit(f'Missing {so_path}')
        write_file(camera, '/tmp/jiritsu/opengate-profile-patch.txt', b'PENDING\n')
        load_one_so(camera, so_path, 2.5)
        out = (read_file(camera, '/tmp/jiritsu/opengate-profile-patch.txt') or b'').decode('ascii', 'replace').strip()
        print(f'Loaded {so_path.name} ({so_path.stat().st_size} bytes) -> {out}')
    if ARG in ('fhd6_rec', 'dci3_rec', 'modee011_rec', 'mode5020_rec'):
        print('Waiting 2.5s after format trigger, then recording 5s clip...')
        time.sleep(2.5)
        run_script(camera, CAMERA_RECORD_CMD, wait=1.5)
        ran_ok = True
        print('Armed 5s recording + sync + reboot in Senser Movie Live View; waiting 22s...')
        time.sleep(22.0)
        return
    ran_ok = True


if __name__ == '__main__':
    try:
        import usb.core, usb.util
        unmount_pmhome()
        max_attempts = 10
        for attempt in range(1, max_attempts + 1):
            print(f'=== Attempt {attempt}/{max_attempts} ===')
            for wait_tick in range(30):
                if usb.core.find(idVendor=0x054c) is not None:
                    break
                if wait_tick == 0:
                    print('Waiting for Sony USB device (0x054c) to appear...')
                time.sleep(1.0)
            try:
                unmount_pmhome()
                senserShellCommand(complete=runner)
            except Exception as e:
                if ran_ok:
                    print(f'Note: camera already rebooting during dev.stop ({e})')
                else:
                    print(f'Attempt {attempt} exception: {e}')
                    try:
                        d = usb.core.find(idVendor=0x054c)
                        if d:
                            try:
                                d.ctrl_transfer(usb.util.CTRL_OUT | usb.util.CTRL_TYPE_VENDOR | usb.util.CTRL_RECIPIENT_OTHER, 1, 0xc800, 0x2855, b'')
                            except Exception:
                                pass
                            try:
                                d.reset()
                            except Exception:
                                pass
                    except Exception:
                        pass
                    print('Waiting 4.0s before next attempt...')
                    time.sleep(4.0)
                    unmount_pmhome()
                    if attempt == max_attempts:
                        raise
            if ran_ok:
                print('=== ALL DONE ===')
                break
            time.sleep(4.0)
    except Exception:
        traceback.print_exc()
        sys.exit(1)
