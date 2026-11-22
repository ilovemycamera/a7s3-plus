#!/usr/bin/env python3
"""Staged volatile-RAM runner for 3840x2560 v2 experiments on ILCE-7SM3 (FW 5.01).

Usage:
  .venv/bin/python analysis/run_2560_experiment.py status
  .venv/bin/python analysis/run_2560_experiment.py stage1          # apply mp + imola + avlog (VDT & CP stay 3840x2160)
  .venv/bin/python analysis/run_2560_experiment.py stage1_lvl6     # apply mp + lvl6 + imola + avlog
  .venv/bin/python analysis/run_2560_experiment.py stage1_tile1    # apply mp + lvl6 + imola + tile1 + avlog
  .venv/bin/python analysis/run_2560_experiment.py logs            # dump /tmp_wbshd/og-2560-v2.log + kikilog strings
  .venv/bin/python analysis/run_2560_experiment.py restore         # restore all RAM patches to stock 3840x2160
"""
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for sp in (ROOT / '.venv/lib').glob('python*/site-packages'):
    sys.path.insert(0, str(sp))
parent_venv = ROOT.parent / '.venv/lib'
if parent_venv.exists():
    for sp in parent_venv.glob('python*/site-packages'):
        sys.path.insert(0, str(sp))
if (ROOT / 'vendor').exists():
    sys.path.insert(0, str(ROOT / 'vendor'))
sys.path.insert(0, str(ROOT / 'analysis'))
sys.path.insert(0, str(ROOT / 'sony-pmca-re'))

from stand import complete, read_file, write_file, unmount_pmhome, run_script, LIBJIT
from check_coldboot import COLDBOOT_DIAG_SCRIPT, save_coldboot_report
from pmca.commands.usb import senserShellCommand

MODE_MAP = {
    'status': 'status',
    'dumplog': 'dumplog',
    'restore': 'restore',
    'stage1': 'apply mp imola avlog',
    'stage1_lvl6': 'apply mp lvl6 imola avlog',
    'stage1_tile1': 'apply mp lvl6 imola tile1 avlog',
    'stage2_vdt': 'apply mp lvl6 imola vdt avlog',
    'stage2_full': 'apply mp lvl6 imola vdt cpvef avlog',
}

ACTION = sys.argv[1] if len(sys.argv) > 1 else 'status'
SO_PATH = ROOT / 'runtime' / 'og_3840x2560_v2.so'
SPY_PATH = ROOT / 'runtime' / 'ulog_spy'
LOG_DIR = ROOT / 'analysis' / 'logs_2560'
LOG_DIR.mkdir(parents=True, exist_ok=True)


def dump_kikilog_tail(camera) -> str:
    # Pull /tmp/kikilog.dat or grep printable strings on host (camera busybox lacks strings/tail)
    raw = read_file(camera, '../../tmp/kikilog.dat')
    if raw:
        import re
        lines = [m.decode('ascii', 'ignore') for m in re.findall(rb'[\x20-\x7e]{6,}', raw)]
        pat = re.compile(r'IMLENC|IMLDEC|CMNHANDLER|ENCHANDLER|VDT|MPR|Rec|Belt|tile|unit_type|DMM|ERROR|ERR|FAIL|2560|2160', re.I)
        hits = [ln for ln in lines if pat.search(ln)]
        return '\n'.join(hits[-120:])
    return run_script(
        camera,
        "ls -la /tmp/kikilog* /var/log/* /log/* 2>&1",
        wait=1.5,
    ).strip()


def runner(camera):
    complete(camera)

    # 1. Always verify and log coldboot safety state first
    diag = run_script(camera, COLDBOOT_DIAG_SCRIPT, wait=2.0)
    save_coldboot_report(diag)
    preload_ok = (
        ('No such file or directory' in diag)
        or ('/setting/mode/libcinema_preload.so' in diag)
    )
    if not preload_ok:
        raise RuntimeError(f'Unsafe boot config detected, aborting:\n{diag}')

    if ACTION == 'lastlog':
        raw_last = read_file(camera, '../../tmp/jiritsu/last_2560_ulog.txt') or b''
        txt = raw_last.decode('utf-8', 'replace')
        stamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        out_file = LOG_DIR / f'{stamp}_lastlog.txt'
        out_file.write_text(txt, encoding='utf-8')
        print(f'=== /tmp/jiritsu/last_2560_ulog.txt ({len(raw_last)} bytes) ===')
        print(txt[:10000])
        if len(txt) > 10000:
            print(f'... (truncated {len(txt) - 10000} bytes, see {out_file})')
        print(f'[SAVED] {out_file}')
        return

    if ACTION == 'logs':
        v2_log = (read_file(camera, '../../tmp_wbshd/og-2560-v2.log') or b'').decode('utf-8', 'replace')
        kiki = dump_kikilog_tail(camera)
        stamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        out_file = LOG_DIR / f'{stamp}_logs.txt'
        report = f'=== og-2560-v2.log ===\n{v2_log}\n=== kikilog tail ===\n{kiki}\n'
        out_file.write_text(report, encoding='utf-8')
        print(report)
        print(f'[SAVED] {out_file}')
        return

    cmd_str = MODE_MAP.get(ACTION, ACTION)
    print(f'[STEP] Setting command in /tmp_wbshd/res2560_cmd -> {cmd_str!r}')
    run_script(
        camera,
        f"echo '{cmd_str}' > /tmp_wbshd/res2560_cmd; rm -f /tmp_wbshd/og-2560-v2.log",
        wait=0.8,
    )

    payload = SO_PATH.read_bytes()
    print(f'[STEP] Injecting {SO_PATH.name} ({len(payload)} bytes) into im.elf RAM...')
    res = write_file(camera, LIBJIT, payload)
    if res != b'\x00\x00\x00\x00':
        raise RuntimeError(f'write LIBJIT failed: {res.hex()}')
    time.sleep(2.5)

    v2_log = run_script(camera, 'cat /tmp_wbshd/og-2560-v2.log 2>/dev/null || cat /tmp/jiritsu/og-2560-v2.log 2>/dev/null', wait=1.0)
    if ACTION.startswith('stage') and SPY_PATH.exists():
        spy_bytes = SPY_PATH.read_bytes()
        print(f'[STEP] Deploying volatile ulog_spy ({len(spy_bytes)} bytes) to /tmp_wbshd/ulog_spy...')
        write_file(camera, '../../tmp_wbshd/ulog_spy', spy_bytes)
        spy_out = run_script(
            camera,
            'rm -f /tmp/jiritsu/last_2560_ulog.txt; chmod 755 /tmp_wbshd/ulog_spy; /tmp_wbshd/ulog_spy </dev/null >/dev/null 2>&1 & echo SPY_PID=$!',
            wait=0.8,
        )
        print(f'[STEP] {spy_out.strip()}')
    ulog_txt = ''
    if 'dumplog' in cmd_str:
        raw_u = read_file(camera, '../../tmp_wbshd/og-2560-ulog.txt')
        if raw_u:
            ulog_txt = raw_u.decode('utf-8', 'replace')
    kiki = dump_kikilog_tail(camera) if 'dumplog' not in cmd_str else ''
    stamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    out_file = LOG_DIR / f'{stamp}_{ACTION}.txt'
    report = f'=== ACTION: {ACTION} ({cmd_str}) ===\n{v2_log}\n'
    if ulog_txt:
        report += f'=== ULOG RING (0x405000000) ===\n{ulog_txt}\n'
    if kiki:
        report += f'=== KIKILOG TAIL ===\n{kiki}\n'
    out_file.write_text(report, encoding='utf-8')
    print(report[:8000])
    if len(report) > 8000:
        print(f'... (truncated {len(report) - 8000} bytes, see saved file)')
    print(f'[SAVED] {out_file}')


if __name__ == '__main__':
    unmount_pmhome()
    senserShellCommand(complete=runner)
