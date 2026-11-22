#!/usr/bin/env python3
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for sp in (ROOT / '.venv/lib').glob('python*/site-packages'):
    sys.path.insert(0, str(sp))
if (ROOT / 'vendor').exists():
    sys.path.insert(0, str(ROOT / 'vendor'))
sys.path.insert(0, str(ROOT / 'analysis'))
sys.path.insert(0, str(ROOT / 'sony-pmca-re'))

from stand import complete, unmount_pmhome, run_script
from pmca.commands.usb import senserShellCommand

LATEST_LOG = ROOT / 'analysis' / 'coldboot_check.log'
HISTORY_LOG = ROOT / 'analysis' / 'coldboot_history.log'

COLDBOOT_DIAG_SCRIPT = (
    'echo "=== MOUNTS ==="; cat /proc/mounts; '
    'echo "=== /etc ==="; ls -la /etc /etc/ld.so.preload /tmp/ld.so.preload /tmp_wbshd/persistent-preload.log /tmp/jiritsu/persistent-preload.log 2>&1; '
    'echo "=== persistent-preload.log (/tmp_wbshd & /tmp/jiritsu) ==="; cat /tmp_wbshd/persistent-preload.log /tmp/jiritsu/persistent-preload.log 2>&1; '
    'echo "=== /setting/mode ==="; ls -la /setting/mode 2>&1; '
    'cat /setting/mode/dmode /setting/mode/preload /etc/ld.so.preload 2>&1; '
    'echo "=== stray /tmp/jiritsu/setting or /etc ==="; ls -la /tmp/jiritsu/setting /tmp/jiritsu/etc 2>&1; '
    'echo "=== im.elf env & maps ==="; PID=$(pidof im.elf); echo "im.elf pid=$PID"; '
    '/bin/busybox tr "\\0" "\\n" < /proc/$PID/environ 2>&1; '
    'grep -E "cinema|jemalloc|HeapMon|menu_repair|high_res|og_menu|doom" /proc/$PID/maps 2>&1'
)


def save_coldboot_report(out: str):
    ts = time.strftime('%Y-%m-%d %H:%M:%S %z')
    block = f'=== COLDBOOT CHECK [{ts}] ===\n{out.strip()}\n=== END COLDBOOT CHECK ===\n'
    LATEST_LOG.write_text(block, encoding='utf-8')
    with open(HISTORY_LOG, 'a', encoding='utf-8') as f:
        f.write(block + '\n')
    for p in (LATEST_LOG, HISTORY_LOG):
        try:
            os.chmod(p, 0o666)
        except Exception:
            pass
    print(block)


def cb(camera):
    complete(camera)
    out = run_script(camera, COLDBOOT_DIAG_SCRIPT, wait=2.5)
    save_coldboot_report(out)


if __name__ == '__main__':
    unmount_pmhome()
    senserShellCommand(complete=cb)
