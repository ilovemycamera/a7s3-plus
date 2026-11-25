#!/usr/bin/env python3
"""Run an arbitrary root shell command inside im.elf over Sony Jiritsu USB (MSC mode)."""
import secrets
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

from pmca.commands.usb import senserShellCommand
from stand import ensure_jiritsu_mounted, format_jiritsu_status, unmount_pmhome

CATEGORY = 0x1001
SET_PATH = 0x400F
SET_FILE2 = 0x4010
GET_FILE2 = 0x4011
BASE = '/tmp/jiritsu/'
LIBJIT = BASE + 'libJit.so'
REQUEST = BASE + 'a7c2-exec.request'
START = BASE + 'a7c2-exec.start'
STATUS = BASE + 'a7c2-exec.status'
OUTPUT = BASE + 'a7c2-exec.out'
PAYLOAD = (ROOT / 'analysis/reference/libjit_exec.so').read_bytes()
COMMAND = ' '.join(sys.argv[1:]) or 'uname -a; cat /proc/$$/status; cat /proc/mounts'
NONCE = secrets.token_hex(4)


def call(camera, command, data):
    return camera._sendAdjustControlPacket(CATEGORY, command, data)


def select(camera, path):
    result = call(camera, SET_PATH, b'\x00' + path.encode() + b'\x00')
    if result[:4] != b'\x00\x00\x00\x00':
        raise RuntimeError(f'select {path}: {result.hex()}')


def read_file(camera, path, limit=262144):
    select(camera, path)
    response = call(camera, GET_FILE2, b'\x00\xff\x00')
    if response[:4] != b'\x00\x00\x00\x00':
        return None
    data = response[4:]
    if len(data) > limit:
        raise RuntimeError(f'oversized response for {path}: {len(data)}')
    return data


def write_file(camera, path, data):
    select(camera, path)
    result = call(camera, SET_FILE2, b'\x00\xff\x00' + data)
    print(f'write {path}: {result[:4].hex()} ({len(data)} bytes)', flush=True)
    return result[:4]


def complete(camera):
    camera.readHasp()
    ensure_jiritsu_mounted(camera)
    if read_file(camera, '/tmp/jbusy') is not None:
        raise RuntimeError('/tmp/jbusy exists; refusing to write')
    script = COMMAND
    if not script.endswith('\n'):
        script += '\n'
    script += f'echo "A7JIT001 {NONCE} $?" > {STATUS}\n'
    script_bytes = script.encode('ascii')
    if len(script_bytes) > 1800:
        raise RuntimeError('command is too long (max 1800 bytes)')
    write_file(camera, START, b'A7JITE00 pending\n')
    write_file(camera, STATUS, b'A7JIT000 pending\n')
    r_req = write_file(camera, REQUEST, script_bytes)
    if r_req != b'\x00\x00\x00\x00':
        raise RuntimeError(f'write REQUEST failed: {format_jiritsu_status(r_req)}')
    if read_file(camera, REQUEST) != script_bytes:
        raise RuntimeError('request readback mismatch')
    result = write_file(camera, LIBJIT, PAYLOAD)
    time.sleep(3)
    start = read_file(camera, START)
    status = read_file(camera, STATUS)
    output = read_file(camera, OUTPUT)
    print(f'libjit_remaining={read_file(camera, LIBJIT, 6000) is not None}')
    print(f'start={start!r}')
    print(f'status={status!r}')
    print('--- output ---')
    print(output.decode('utf-8', 'replace') if output else '<absent>')
    evidence = ROOT / 'analysis' / 'jit-last-run'
    evidence.mkdir(exist_ok=True)
    for name, data in [('start', start), ('status', status), ('output', output)]:
        if data is not None:
            (evidence / (name + '.bin')).write_bytes(data)
    (evidence / 'command.txt').write_text(COMMAND + '\n')
    if result != b'\x00\x00\x00\x00':
        raise RuntimeError(f'libJit write/dlopen result: {result.hex()}')
    if not start or not start.startswith(b'A7JITE01'):
        raise RuntimeError('module did not produce a start record')


if __name__ == '__main__':
    unmount_pmhome()
    senserShellCommand(complete=complete)
