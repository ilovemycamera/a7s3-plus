#!/usr/bin/env python3
"""A7S III emergency restore: disables persistent preload via the USB Senser window."""
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for sp in (ROOT / '.venv/lib').glob('python*/site-packages'):
    sys.path.insert(0, str(sp))
if (ROOT / 'vendor').exists():
    sys.path.insert(0, str(ROOT / 'vendor'))
sys.path.insert(0, str(ROOT / 'sony-pmca-re'))
sys.path.insert(0, str(ROOT / 'analysis'))

import usb.core  # noqa: E402
from stand import complete, unmount_pmhome, run_script  # noqa: E402
from pmca.commands.usb import senserShellCommand  # noqa: E402

done = False


def cb(camera):
    global done
    complete(camera)
    out = run_script(
        camera,
        'rm -f /etc/ld.so.preload; '
        'printf "3\\n" > /setting/mode/dmode; '
        ': > /setting/mode/preload; '
        '/bin/busybox sync; '
        'echo RESTORE_MARKER; ls -la /etc/ld.so.preload 2>&1; '
        'echo dmode=$(cat /setting/mode/dmode); echo preload=[$(cat /setting/mode/preload)]',
        wait=1.5,
    )
    print('=== RESTORE OUTPUT ===')
    print(out)
    if 'RESTORE_MARKER' in out:
        done = True


def main():
    unmount_pmhome()
    print('Waiting for Sony USB device (0x054c)... (Press Ctrl+C to exit)')
    attempt = 0
    while not done:
        attempt += 1
        devs = list(usb.core.find(idVendor=0x054c, find_all=True))
        if not devs:
            time.sleep(0.3)
            continue
        pids = ', '.join(f'0x{d.idProduct:04x}' for d in devs)
        print(f'[attempt {attempt}] detected {pids} — opening Senser session...', flush=True)
        try:
            senserShellCommand(complete=cb)
        except Exception as e:
            print(f'  session failed: {e!r}')
        if not done:
            print('  missed window, waiting for next boot cycle...')
            time.sleep(0.5)
    print()
    print('DONE: persistent preload removed. Turn off the camera, remove the battery for 15 seconds,')
    print('and power it back on.')


if __name__ == '__main__':
    main()
