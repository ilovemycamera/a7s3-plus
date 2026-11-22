#!/usr/bin/env python3
"""
persist_v2.py — Safe staging, testing, and control tool for a7s3-plus Persistence v2.

Commands:
  status         Read-only check of camera state, im.elf maps, preload, and logs.
  test-manual    Stage Persistence v2 binaries to /setting/mode/ (NO /etc/ld.so.preload)
                 and run /setting/mode/a7s3_wake_inject --manual once to prove local IPC.
  arm-dryrun     Enable dry-run process logger (/etc/ld.so.preload + persist_dryrun)
                 to verify which processes spawn on WBI2 warm boot (does NOT inject).
  arm            Enable full Persistence v2 (Auto-Disarm boot_tries<=2 + SD NO_MOD kill-switch).
  test-coldboot  Arm Persistence v2, trigger automated cold reboot, wait for USB reconnect,
                 and verify all modules auto-loaded inside im.elf with boot_tries reset to 0.
  disarm         Remove /etc/ld.so.preload and reset boot_tries=0 (100% stock boot).
"""
import argparse
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for sp in (ROOT / '.venv/lib').glob('python*/site-packages'):
    sys.path.insert(0, str(sp))
if (ROOT / 'vendor').exists():
    sys.path.insert(0, str(ROOT / 'vendor'))
parent_venv = ROOT.parent / '.venv/lib'
if parent_venv.exists():
    for sp in parent_venv.glob('python*/site-packages'):
        sys.path.insert(0, str(sp))
sys.path.insert(0, str(ROOT / 'analysis'))
sys.path.insert(0, str(ROOT / 'sony-pmca-re'))

from stand import complete, unmount_pmhome, run_script, write_file, read_file
from pmca.commands.usb import senserShellCommand


STAGE_FILES = (
    ('libcinema_preload.so', 'persistent_preload_v2.so'),
    ('a7s3_wake_inject', 'a7s3_wake_inject'),
    ('libcinema_worker.so', 'libcinema_worker.so'),
    ('libjit_warm.so', 'libjit_warm.so'),
    ('menu_repair.so', 'menu_repair.so'),
    ('high_res_display.so', 'high_res_display_v10.so'),
    ('og_menu_daemon.so', 'og_menu_daemon_v15.so'),
    ('big6_key_hook.so', 'big6_key_hook.so'),
)


def stage_all(camera):
    for dst_name, src_name in STAGE_FILES:
        src = ROOT / 'runtime' / src_name
        if not src.is_file():
            raise FileNotFoundError(f'Missing built artifact: {src}')
        data = src.read_bytes()
        res = write_file(camera, f'/tmp/jiritsu/st2_{dst_name}', data)
        if res != b'\x00\x00\x00\x00':
            raise RuntimeError(f'Failed staging {dst_name}: {res.hex()}')
        print(f'Staged {src_name} -> /tmp/jiritsu/st2_{dst_name} ({len(data)} bytes)')

    install_cmd = (
        'mkdir -p /setting/mode; '
        'for f in libcinema_preload.so a7s3_wake_inject libcinema_worker.so libjit_warm.so '
        'menu_repair.so high_res_display.so og_menu_daemon.so big6_key_hook.so; do '
        'rm -f /setting/mode/$f; '
        'cp /tmp/jiritsu/st2_$f /setting/mode/$f; '
        'chmod 755 /setting/mode/$f; '
        'rm -f /tmp/jiritsu/st2_$f; '
        'done; '
        'printf "3\\n" > /setting/mode/dmode; '
        ': > /setting/mode/preload; '
        'chmod 644 /setting/mode/dmode /setting/mode/preload; '
        '/bin/busybox sync; '
        'ls -la /setting/mode/'
    )
    out = run_script(camera, install_cmd, wait=3.0)
    print('=== STAGED /setting/mode ===\n' + out)


def status_cmd_script() -> str:
    return (
        'echo "=== UPTIME & BOOT ==="; cat /proc/uptime; '
        'echo "=== PRELOAD & TRIES ==="; '
        'ls -la /etc/ld.so.preload /setting/mode/persist_dryrun /setting/mode/boot_tries /tmp_wbshd/.a7s3_armed 2>&1; '
        'echo -n "ld.so.preload="; cat /etc/ld.so.preload 2>&1; '
        'echo -n "boot_tries="; cat /setting/mode/boot_tries 2>&1; '
        'echo "=== /tmp_wbshd/persist_v2.log ==="; cat /tmp_wbshd/persist_v2.log 2>&1; '
        'echo "=== /tmp/jiritsu/persist_v2.log ==="; cat /tmp/jiritsu/persist_v2.log 2>&1; '
        'echo "=== /tmp/jiritsu/warm_procs.log ==="; cat /tmp/jiritsu/warm_procs.log 2>&1; '
        'echo "=== IM.ELF MAPS ==="; '
        'PID=$(pidof im.elf); echo "im_pid=$PID"; '
        'grep -E "cinema|menu_repair|high_res|og_menu|big6|doom" /proc/$PID/maps 2>&1 || true; '
        'echo "=== OG DAEMON & BIG6 LOGS ==="; cat /tmp/jiritsu/og-menu-daemon.txt /tmp/jiritsu/big6-hook.txt 2>&1'
    )


def wait_for_usb_disconnect_then_reconnect(timeout_s=60):
    import usb.core
    print('Waiting for camera USB to disconnect during reboot...')
    t0 = time.time()
    while time.time() - t0 < 20:
        if usb.core.find(idVendor=0x054c) is None:
            print('Camera USB disconnected.')
            break
        time.sleep(0.5)
    print('Waiting for camera USB to reconnect...')
    t1 = time.time()
    while time.time() - t1 < timeout_s:
        if usb.core.find(idVendor=0x054c) is not None:
            print('Camera USB reconnected! Settling 12s for boot + stability gate...')
            time.sleep(12.0)
            return True
        time.sleep(0.5)
    return False


def run_once(fn):
    import usb.core, usb.util
    unmount_pmhome()
    for attempt in range(1, 6):
        try:
            unmount_pmhome()
            senserShellCommand(complete=fn)
            return
        except Exception as e:
            print(f'Attempt {attempt} failed: {e}')
            d = usb.core.find(idVendor=0x054c)
            if d:
                try:
                    d.reset()
                except Exception:
                    pass
            time.sleep(3.0)
            if attempt == 5:
                raise


def main():
    parser = argparse.ArgumentParser(description='a7s3-plus Persistence v2 controller & tester')
    parser.add_argument('action', choices=['status', 'test-manual', 'arm-dryrun', 'arm', 'test-coldboot', 'disarm'])
    args = parser.parse_args()

    if args.action == 'status':
        def cb(cam):
            complete(cam)
            print(run_script(cam, status_cmd_script(), wait=2.5))
        run_once(cb)
        return

    if args.action == 'disarm':
        def cb(cam):
            complete(cam)
            out = run_script(
                cam,
                'rm -f /etc/ld.so.preload /setting/mode/persist_dryrun /tmp_wbshd/.a7s3_armed; '
                'printf "0\\n" > /setting/mode/boot_tries; '
                '/bin/busybox sync; '
                'ls -la /etc/ld.so.preload /setting/mode/boot_tries 2>&1',
                wait=2.0,
            )
            print('=== DISARMED ===\n' + out)
        run_once(cb)
        return

    if args.action == 'test-manual':
        def cb(cam):
            complete(cam)
            stage_all(cam)
            # Keep /etc/ld.so.preload REMOVED for 100% zero-risk manual smoke test
            out = run_script(
                cam,
                'rm -f /etc/ld.so.preload /setting/mode/persist_dryrun /tmp_wbshd/persist_v2.log /tmp_wbshd/.a7s3_armed; '
                'printf "0\\n" > /setting/mode/boot_tries; '
                '/bin/busybox sync; '
                '(/bin/busybox sleep 1; /setting/mode/a7s3_wake_inject --manual > /tmp_wbshd/manual_wake.out 2>&1) </dev/null >/dev/null 2>&1 & '
                'echo "LAUNCHED_MANUAL_INJECTOR"',
                wait=1.5,
            )
            print(out)
            print('Waiting 12s for manual wake_inject + worker to load all modules into im.elf...')
            time.sleep(12.0)
            chk = run_script(
                cam,
                'echo "=== manual_wake.out ==="; cat /tmp_wbshd/manual_wake.out 2>&1; '
                + status_cmd_script(),
                wait=2.5,
            )
            print(chk)
        run_once(cb)
        return

    if args.action == 'arm-dryrun':
        def cb(cam):
            complete(cam)
            stage_all(cam)
            out = run_script(
                cam,
                'rm -f /tmp/jiritsu/warm_procs.log /tmp_wbshd/.a7s3_armed; '
                'printf "1\\n" > /setting/mode/persist_dryrun; '
                'printf "0\\n" > /setting/mode/boot_tries; '
                'printf "/setting/mode/libcinema_preload.so\\n" > /etc/ld.so.preload; '
                'chmod 644 /etc/ld.so.preload /setting/mode/persist_dryrun /setting/mode/boot_tries; '
                '/bin/busybox sync; '
                'echo "DRY-RUN ARMED:"; cat /etc/ld.so.preload',
                wait=2.0,
            )
            print(out)
        run_once(cb)
        print('\n>>> Dry-run armed! Unplug USB -> Power OFF -> Power ON -> wait 5s -> plug USB -> run `status`! <<<')
        return

    if args.action == 'arm':
        def cb(cam):
            complete(cam)
            stage_all(cam)
            out = run_script(
                cam,
                'rm -f /setting/mode/persist_dryrun /tmp/jiritsu/persist_v2.log /tmp_wbshd/.a7s3_armed; '
                'printf "0\\n" > /setting/mode/boot_tries; '
                'printf "/setting/mode/libcinema_preload.so\\n" > /etc/ld.so.preload; '
                'chmod 644 /etc/ld.so.preload /setting/mode/boot_tries; '
                '/bin/busybox sync; '
                'echo "PERSISTENCE V2 ARMED:"; ls -la /etc/ld.so.preload /setting/mode/libcinema_preload.so /setting/mode/a7s3_wake_inject',
                wait=2.0,
            )
            print(out)
        run_once(cb)
        return

    if args.action == 'test-coldboot':
        def cb_arm_and_reboot(cam):
            complete(cam)
            stage_all(cam)
            out = run_script(
                cam,
                'rm -f /setting/mode/persist_dryrun /tmp/jiritsu/persist_v2.log /tmp_wbshd/.a7s3_armed; '
                'printf "0\\n" > /setting/mode/boot_tries; '
                'printf "/setting/mode/libcinema_preload.so\\n" > /etc/ld.so.preload; '
                'chmod 644 /etc/ld.so.preload /setting/mode/boot_tries; '
                '/bin/busybox sync; '
                'echo "ARMED_REBOOTING"; '
                '(/bin/busybox sleep 2; /usr/bin/ud_send_lsi.elf 0; /bin/busybox sleep 1; /bin/busybox sync; /bin/busybox reboot -f) </dev/null >/dev/null 2>&1 &',
                wait=1.5,
            )
            print(out)
        try:
            run_once(cb_arm_and_reboot)
        except Exception as e:
            print(f'Note during reboot disconnect: {e}')

        if not wait_for_usb_disconnect_then_reconnect(timeout_s=75):
            sys.exit('ERROR: camera did not reconnect on USB within 75s')

        def cb_verify(cam):
            complete(cam)
            print(run_script(cam, status_cmd_script(), wait=2.5))
        run_once(cb_verify)


if __name__ == '__main__':
    main()
