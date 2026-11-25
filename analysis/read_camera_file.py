#!/usr/bin/env python3
"""Read one or more files from the camera filesystem over Sony Jiritsu USB (MSC mode)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for sp in (ROOT / '.venv/lib').glob('python*/site-packages'):
    sys.path.insert(0, str(sp))
if (ROOT / 'vendor').exists():
    sys.path.insert(0, str(ROOT / 'vendor'))
sys.path.insert(0, str(ROOT / 'sony-pmca-re'))
sys.path.insert(0, str(ROOT / 'analysis'))

from pmca.commands.usb import senserShellCommand
from stand import ensure_jiritsu_mounted, unmount_pmhome

CATEGORY = 0x1001
SET_PATH = 0x400F
GET_FILE2 = 0x4011


def complete(camera):
    camera.readHasp()
    ensure_jiritsu_mounted(camera)
    for remote in sys.argv[1:]:
        camera._sendAdjustControlPacket(CATEGORY, SET_PATH, b'\x00' + remote.encode() + b'\x00')
        response = camera._sendAdjustControlPacket(CATEGORY, GET_FILE2, b'\x00\xff\x00')
        print(f'--- {remote} ---')
        print(f'status={response[:4].hex()} length={len(response)}')
        if response[:4] == b'\x00\x00\x00\x00':
            print(response[4:].decode('utf-8', 'replace'))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit('Usage: read_camera_file.py <remote_path> [remote_path2 ...]')
    unmount_pmhome()
    senserShellCommand(complete=complete)
