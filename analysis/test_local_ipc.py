#!/usr/bin/env python3
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
for sp in (ROOT / ".venv/lib").glob("python*/site-packages"):
    sys.path.insert(0, str(sp))
sys.path.insert(0, str(ROOT / "analysis"))
sys.path.insert(0, str(ROOT / "sony-pmca-re"))
from stand import complete, unmount_pmhome, run_script, write_file
from pmca.commands.usb import senserShellCommand

CLANG = "/opt/homebrew/opt/llvm/bin/clang"
if not Path(CLANG).exists():
    CLANG = "clang"
ENV = dict(os.environ)
ENV["PATH"] = "/opt/homebrew/opt/llvm/bin:/opt/homebrew/bin:" + ENV.get("PATH", "")

def build_so(src: Path, out: Path, extra_flags=()):
    cmd = [
        CLANG,
        "-target", "aarch64-linux-gnu",
        "-fuse-ld=lld",
        "-nostdlib",
        "-shared",
        "-fPIC",
        "-O2",
        "-Wl,--hash-style=both",
        "-Wl,--allow-shlib-undefined",
        *extra_flags,
        str(src),
        "-o", str(out),
    ]
    subprocess.run(cmd, check=True, env=ENV)

build_so(
    ROOT / "runtime/persistent_preload.c",
    ROOT / "runtime/persistent_preload.so",
    ["-Wl,-soname,libcinema.so", "-Wl,-z,nodelete", "-Wl,-init,_init"],
)
build_so(
    ROOT / "runtime/libjit_warm.c",
    ROOT / "runtime/libjit_warm.so",
    ["-Wl,-z,nodelete", "-Wl,-init,_init"],
)

LOG_FILE = ROOT / "analysis/usb_trace.log"

def log(msg: str):
    ts = time.strftime("%H:%M:%S") + f".{int((time.time() % 1) * 1000):03d}"
    line = f"[{ts}] {msg}\n"
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(line)
    try:
        os.chmod(LOG_FILE, 0o666)
    except Exception:
        pass
    print(line, end="", flush=True)

done = False

def cb(camera):
    global done
    log("cb() entered: staging libjit_warm.so, libcinema.so into /tmp/jiritsu/stage_*...")
    r1 = write_file(camera, "/tmp/jiritsu/stage_libjit_warm.so", (ROOT / "runtime/libjit_warm.so").read_bytes())
    r2 = write_file(camera, "/tmp/jiritsu/stage_libcinema.so", (ROOT / "runtime/persistent_preload.so").read_bytes())
    done = True
    log(f"STAGED r1={r1.hex()} r2={r2.hex()}")
    complete(camera)
    out = run_script(
        camera,
        "mkdir -p /setting/mode; "
        "rm -f /tmp/jiritsu/setting/mode/* 2>/dev/null; "
        "rmdir /tmp/jiritsu/setting/mode /tmp/jiritsu/setting /tmp/jiritsu/etc 2>/dev/null; "
        "for f in libcinema.so libjit_warm.so; do "
        "rm -f /setting/mode/$f; cp /tmp/jiritsu/stage_$f /setting/mode/$f; chmod 755 /setting/mode/$f; rm -f /tmp/jiritsu/stage_$f; "
        "done; "
        "cp /setting/mode/libcinema.so /setting/libcinema.so 2>/dev/null; "
        "/bin/busybox sync; "
        "ls -la /setting/mode; "
        "cat /tmp/persistent-preload.log 2>&1; echo '===MAPS==='; "
        "PID=$(pidof im.elf); grep -E 'menu_repair|high_res|og_menu|doom|libJit|libcinema' /proc/$PID/maps 2>&1",
        wait=2.5,
    )
    log("POST_PATCH_CHECK:\n" + out)

if __name__ == "__main__":
    if "--stop-watcher" in sys.argv:
        import signal
        me = os.getpid()
        res = subprocess.run(["pgrep", "-f", "test_local_ipc.py"], capture_output=True, text=True)
        killed = []
        for line in res.stdout.splitlines():
            pid = int(line.strip())
            if pid != me:
                try:
                    os.kill(pid, signal.SIGTERM)
                    killed.append(pid)
                except Exception:
                    pass
        print("STOPPED_WATCHERS:", killed)
        sys.exit(0)
    import usb.core
    LOG_FILE.write_text("", encoding="utf-8")
    os.chmod(LOG_FILE, 0o666)
    log("Started USB watcher (safe backoff=3.0s, no 0x0d68 filter)...")
    last_state = None
    for attempt in range(1800):
        all_sony = list(usb.core.find(idVendor=0x054c, find_all=True))
        state = tuple(sorted((hex(d.idProduct), d.bDeviceClass) for d in all_sony))
        if state != last_state:
            log(f"USB 0x054c state changed: {state}")
            last_state = state
        cam_devs = list(all_sony)
        if cam_devs:
            log(f"Detected camera device {hex(cam_devs[0].idProduct)}, calling senserShellCommand...")
            try:
                senserShellCommand(complete=cb)
            except Exception as e:
                log(f"senserShellCommand exception: {e!r}; pausing 3.0s before retry...")
                time.sleep(3.0)
                unmount_pmhome()
            if done:
                log("Patch completed successfully!")
                break
        time.sleep(0.5)
