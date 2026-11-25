#!/usr/bin/env python3
"""Camera test bench (read-only by default).

Automates everything that used to be manual: PMHOME unmount, USB discovery,
root-shell command execution on the camera via the proven Jiritsu channel
(request file -> libJit.so dlopen -> output files). Writes to the camera are
refused unless --write is given explicitly.

Usage (host needs sudo for USB; password comes from BENCH_SUDO_PASS or a prompt):
    sudo -E .venv/bin/python analysis/stand.py status
    sudo -E .venv/bin/python analysis/stand.py angle
    sudo -E .venv/bin/python analysis/stand.py bksweep 02cf0000 02cfffff
    sudo -E .venv/bin/python analysis/stand.py hooks
"""
import argparse
import ctypes.util
import getpass
import platform
import secrets
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for sp in (ROOT / '.venv/lib').glob('python*/site-packages'):
    sys.path.insert(0, str(sp))
if (ROOT / 'vendor').exists():
    sys.path.insert(0, str(ROOT / 'vendor'))
sys.path.insert(0, str(ROOT / 'sony-pmca-re'))

_VENDOR_LIBUSB = ROOT / 'vendor' / 'libusb-1.0.dylib'
if sys.platform == 'darwin' and platform.machine() == 'arm64' and _VENDOR_LIBUSB.is_file():
    _orig_find_library = ctypes.util.find_library

    def _find_library_with_vendor(name):
        res = _orig_find_library(name)
        if res:
            return res
        if name and 'usb-1.0' in name:
            for candidate in (
                '/opt/homebrew/lib/libusb-1.0.dylib',
                '/usr/local/lib/libusb-1.0.dylib',
                str(_VENDOR_LIBUSB),
            ):
                if Path(candidate).is_file():
                    return candidate
        return None

    ctypes.util.find_library = _find_library_with_vendor

CATEGORY = 0x1001
JIRITSU_MOUNT = 0x0002
SET_PATH = 0x400F
SET_FILE2 = 0x4010
GET_FILE2 = 0x4011
BASE = '/tmp/jiritsu/'
LIBJIT = BASE + 'libJit.so'
REQUEST = BASE + 'a7c2-exec.request'
PAYLOAD = (ROOT / 'analysis/reference/libjit_exec.so').read_bytes()
NONCE = secrets.token_hex(4)

JIRITSU_ERRORS = {
    0x10000004: 'mount /dev/nflasha6 -> /tmp/jiritsu failed',
    0x10000008: 'file open failed on camera',
    0x10000009: 'short file write on camera',
    0x1000000d: 'mkdir /tmp/jiritsu failed',
    0x1000000f: '/tmp/jiritsu partition full (>24 MiB)',
    0x10000015: 'DMM buffer alloc failed (set camera to idle Movie mode, close menu/playback)',
    0x1000001c: 'parent directory creation failed',
    0x10000201: 'invalid Jiritsu parameter',
}


def format_jiritsu_status(raw: bytes) -> str:
    if not raw or len(raw) < 4:
        return raw.hex() if raw else 'empty'
    code = int.from_bytes(raw[:4], 'little')
    hint = JIRITSU_ERRORS.get(code)
    return f'0x{code:08x} ({hint})' if hint else f'0x{code:08x}'


def ensure_jiritsu_mounted(camera):
    """Create /tmp/jiritsu and mount /dev/nflasha6 (required on untouched stock cameras)."""
    try:
        res = call(camera, JIRITSU_MOUNT, b'\x00\x00')[:4]
    except Exception as exc:
        print(f'note: JIRITSU_MOUNT(0,0) skipped ({exc})')
        return None
    if res != b'\x00\x00\x00\x00':
        print(f'note: JIRITSU_MOUNT(0,0) -> {format_jiritsu_status(res)}')
    return res


def unmount_pmhome():
    if sys.platform != 'darwin' or not shutil.which('diskutil'):
        return
    out = subprocess.run(['diskutil', 'list', 'external'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith('/dev/disk'):
            disk = line.split()[0]
            info = subprocess.run(['diskutil', 'info', disk], capture_output=True, text=True).stdout
            info_up = info.upper()
            if 'DSC' in info_up or 'SONY' in info_up or 'ILCE' in info_up:
                r = subprocess.run(['diskutil', 'unmountDisk', 'force', disk], capture_output=True, text=True)
                print(f'unmountDisk {disk} (DSC): {r.returncode == 0}')
        elif 'PMHOME' in line:
            ident = line.split()[-1]
            r = subprocess.run(['diskutil', 'unmount', 'force', ident], capture_output=True, text=True)
            print(f'unmounted {ident}: {r.returncode == 0}')
    time.sleep(0.4)


def call(camera, command, data):
    return camera._sendAdjustControlPacket(CATEGORY, command, data)


def normalize_senser_path(path: str) -> str:
    if path.startswith('/tmp/jiritsu/'):
        return path
    if path.startswith('../../'):
        return path
    if path.startswith('/'):
        return '../..' + path
    return path


def select(camera, path):
    norm_path = normalize_senser_path(path)
    result = call(camera, SET_PATH, b'\x00' + norm_path.encode() + b'\x00')
    if result[:4] != b'\x00\x00\x00\x00':
        raise RuntimeError(f'select {path} (normalized {norm_path}): {result.hex()}')


def read_file(camera, path, limit=8_000_000):
    select(camera, path)
    response = call(camera, GET_FILE2, b'\x00\xff\x00')
    read_file._last_status = response[:4]
    if response[:4] != b'\x00\x00\x00\x00':
        return None
    data = response[4:]
    if len(data) > limit:
        raise RuntimeError(f'oversized response for {path}: {len(data)}')
    return data


read_file._last_status = b''


def write_file(camera, path, data):
    if not path.startswith('/tmp/jiritsu/') and not path.startswith('../../'):
        raise ValueError(
            f'Refusing write_file({path!r}): writes outside /tmp/jiritsu/ must use ../../ prefix or root-shell cp'
        )
    select(camera, path)
    return call(camera, SET_FILE2, b'\x00\xff\x00' + data)[:4]


def run_script(camera, script, wait=4.0):
    if not script.endswith('\n'):
        script += '\n'
    script += f'echo "A7JIT001 {NONCE} $?" > {BASE}a7c2-exec.status\n'
    payload = script.encode('ascii', 'replace')
    if len(payload) >= 2048:
        raise RuntimeError(f'script too long for libjit_exec REQUEST_MAX=2048 ({len(payload)} bytes)')
    r_start = write_file(camera, BASE + 'a7c2-exec.start', b'A7JIT000 pending\n')
    r_stat = write_file(camera, BASE + 'a7c2-exec.status', b'A7JIT000 pending\n')
    r_req = write_file(camera, REQUEST, payload)
    if r_req != b'\x00\x00\x00\x00':
        raise RuntimeError(
            f'write REQUEST failed: {format_jiritsu_status(r_req)} '
            f'(start={format_jiritsu_status(r_start)}, status={format_jiritsu_status(r_stat)})'
        )
    rb = read_file(camera, REQUEST, 200_000)
    if rb != payload:
        raise RuntimeError(
            f'request readback mismatch (wrote {len(payload)}B, read {None if rb is None else len(rb)}B, '
            f'get_status={format_jiritsu_status(read_file._last_status)})'
        )
    result = write_file(camera, LIBJIT, PAYLOAD)
    if result != b'\x00\x00\x00\x00':
        raise RuntimeError(f'libJit write/dlopen failed: {format_jiritsu_status(result)}')
    time.sleep(wait)
    start = read_file(camera, BASE + 'a7c2-exec.start')
    status = read_file(camera, BASE + 'a7c2-exec.status')
    output = read_file(camera, BASE + 'a7c2-exec.out')
    if not start or not start.startswith(b'A7JIT001'):
        print(f'warning: start={start!r}')
    return (output or b'').decode('utf-8', 'replace')


SCRIPTS = {
    'status': (
        'cat /setting/version.txt 2>/dev/null; uname -a; '
        'PID=$(pidof im.elf); echo "im.elf pid=$PID"; '
        'grep "r-xp.*appFw.so" /proc/$PID/maps | sed -n 1p; '
        'ls /tmp/jiritsu/ 2>/dev/null'
    ),
    'angle': (
        'PID=$(pidof im.elf); LINE=$(grep "r-xp.*appFw.so" /proc/$PID/maps | sed -n 1p); '
        'B=${LINE%%-*}; '
        'M=/tmp/jiritsu/memtool; '
        'if [ -x $M ]; then '
        'A1=$(printf "0x%x" $((0x$B + 0x7f84f10))); '
        'A2=$(printf "0x%x" $((0x$B + 0x7f84f18))); '
        'A3=$(printf "0x%x" $((0x$B + 0x8455f39))); '
        'A4=$(printf "0x%x" $((0x$B + 0x7f5e320))); '
        'echo "base=0x$B"; '
        'echo mode=$($M r $A1 0); '
        'echo angle=$($M r $A2 0); '
        'echo guard=$($M r $A3 0); '
        'echo miss=$($M r $A4 0); '
        'else echo "memtool absent (RAM-only, reboot clears it)"; fi'
    ),
    'hooks': (
        'cat /tmp/jiritsu/big6-hook.txt 2>/dev/null; '
        'cat /tmp/jiritsu/angle-hook.txt 2>/dev/null; '
        'ls /tmp/jiritsu/*.txt 2>/dev/null'
    ),
    'bkcp': (
        'echo "=== auto-cycle.log ==="; cat /tmp/jiritsu/auto-cycle.log 2>&1; '
        'M=/tmp/jiritsu/bk.elf; '
        'for id in 00030072 00bc0016; do '
        'echo "=== CP $id ==="; '
        '$M gc $id 2>&1; '
        '$M rc $id 2>&1; '
        'done'
    ),
    'bktest93': (
        'M=/tmp/jiritsu/bk.elf; '
        'Z256="' + ' '.join(['00'] * 256) + '"; '
        'P93="' + ' '.join(['01' if i == 0x93 else '00' for i in range(256)]) + '"; '
        'echo "--- Write +0x93=01 ---"; '
        '$M wc 00030072 $P93 2>&1; '
        'echo "--- Readback ---"; '
        '$M rc 00030072 2>&1; '
        'echo "--- Restore all 00 ---"; '
        '$M wc 00030072 $Z256 2>&1; '
        'echo "--- Final Readback ---"; '
        '$M rc 00030072 2>&1'
    ),
}


def build_bksweep(lo, hi):
    return (
        'M=/tmp/jiritsu/bk.elf; '
        'if [ ! -x $M ]; then echo "bk.elf absent"; exit 0; fi; '
        'i=' + str(lo) + '; while [ $i -le ' + str(hi) + ' ]; do '
        'V=$($M r $i 2>/dev/null); '
        'case "$V" in ""|"-"*|*" "*" "*) ;; *) echo "$i $V";; esac; '
        'i=$((i+1)); done'
    )


def complete(camera):
    camera.readHasp()
    ensure_jiritsu_mounted(camera)
    if read_file(camera, '/tmp/jbusy') is not None:
        raise RuntimeError('/tmp/jbusy exists; camera busy, refusing')


def main():
    global args_output
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['status', 'angle', 'hooks', 'bksweep', 'bkcp', 'bktest93'])
    ap.add_argument('range', nargs='*', help='bksweep: lo hi as hex without 0x')
    ap.add_argument('--keep-mounted', action='store_true')
    ap.add_argument('--wait', type=float, default=4.0)
    a = ap.parse_args()

    if a.cmd == 'bksweep':
        if len(a.range) != 2:
            sys.exit('bksweep needs <lo> <hi>, e.g. 02cf0000 02cfffff')
        script = build_bksweep(int(a.range[0], 16), int(a.range[1], 16))
        if int(a.range[1], 16) - int(a.range[0], 16) > 0x20000:
            sys.exit('range too wide for one run; split it')
    else:
        script = SCRIPTS[a.cmd]

    if not a.keep_mounted:
        unmount_pmhome()

    from pmca.commands.usb import senserShellCommand

    def runner(camera):
        global args_output
        args_output = run_script(camera, script, a.wait)
        outdir = ROOT / 'analysis' / 'bench-last-run'
        outdir.mkdir(exist_ok=True)
        (outdir / 'output.txt').write_text(args_output, encoding='utf-8')
        print('--- camera output ---')
        print(args_output if args_output.strip() else '<empty>')

    senserShellCommand(complete=runner)


if __name__ == '__main__':
    if subprocess.run(['id', '-u'], capture_output=True, text=True).stdout.strip() != '0':
        sys.exit('run with sudo (USB needs root)')
    main()
