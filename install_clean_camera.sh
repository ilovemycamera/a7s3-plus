#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
export DYLD_FALLBACK_LIBRARY_PATH="$DIR/vendor:/opt/homebrew/lib:/usr/local/lib:/opt/local/lib:${DYLD_FALLBACK_LIBRARY_PATH:-}"

python_ready() {
    "$1" -c 'import sys; sys.path[:0] = sys.argv[1:3]; import usb.core; from pmca.commands.usb import senserShellCommand' \
        "$DIR/vendor" "$DIR/sony-pmca-re" >/dev/null 2>&1
}

choose_python() {
    if [ -x "$DIR/.venv/bin/python" ] && python_ready "$DIR/.venv/bin/python"; then
        PY="$DIR/.venv/bin/python"
        return
    fi
    if command -v python3 >/dev/null 2>&1 && python_ready "$(command -v python3)"; then
        PY="$(command -v python3)"
        return
    fi
    echo "=== Creating local .venv and installing Python dependencies (pyusb) ==="
    rm -rf "$DIR/.venv"
    python3 -m venv "$DIR/.venv"
    "$DIR/.venv/bin/pip" install --upgrade pip
    "$DIR/.venv/bin/pip" install -r "$DIR/requirements.txt"
    PY="$DIR/.venv/bin/python"
}

# Verify libusb is reachable on macOS before prompting for sudo
if [ "$(uname -s)" = "Darwin" ]; then
    HAS_VENDOR_LIBUSB=0
    if [ "$(uname -m)" = "arm64" ] && [ -f "$DIR/vendor/libusb-1.0.dylib" ]; then
        HAS_VENDOR_LIBUSB=1
    fi
    if [ "$HAS_VENDOR_LIBUSB" -eq 0 ] && [ ! -f "/opt/homebrew/lib/libusb-1.0.dylib" ] && [ ! -f "/usr/local/lib/libusb-1.0.dylib" ] && [ ! -f "/opt/local/lib/libusb-1.0.dylib" ]; then
        echo "ERROR: libusb not found. Install it via Homebrew first:"
        echo "  brew install libusb"
        exit 1
    fi
fi

choose_python

MODE="${1:-doom}"

sudo -v
sudo pkill -f "analysis/auto_agent.py" 2>/dev/null || true
sudo pkill -f "analysis/test_local_ipc.py" 2>/dev/null || true

if [ "$MODE" = "--check" ] || [ "$MODE" = "check" ]; then
    echo "=== Running camera diagnostics (check_coldboot.py) ==="
    exec sudo -E "$PY" "$DIR/analysis/check_coldboot.py"
fi

if [ "$MODE" = "--recover" ] || [ "$MODE" = "recover" ]; then
    echo "=== Running emergency recovery (recovery_restore.py) ==="
    exec sudo -E "$PY" "$DIR/analysis/recovery_restore.py"
fi

if [ "$MODE" = "--persist" ] || [ "$MODE" = "persist" ]; then
    echo "=== Installing Persistence v2 (WBI2 Warm Boot + Cold Boot, Auto-Disarm + SD NO_MOD kill-switch) ==="
    exec sudo -E "$PY" "$DIR/analysis/persist_v2.py" arm
fi

if [ "$MODE" = "--disarm" ] || [ "$MODE" = "disarm" ]; then
    echo "=== Disabling Persistence v2 (restoring 100% stock boot) ==="
    exec sudo -E "$PY" "$DIR/analysis/persist_v2.py" disarm
fi

if [ "$MODE" = "--persist-status" ] || [ "$MODE" = "persist-status" ]; then
    exec sudo -E "$PY" "$DIR/analysis/persist_v2.py" status
fi

echo "=== Loading a7s3-plus on Sony A7S III (FW 5.01) [mode: ${*:-doom}] ==="
sudo -E "$PY" "$DIR/analysis/load_opengate_profile.py" "${@:-doom}"
echo "=== Done! Logs saved to: ==="
echo "  - $DIR/analysis/last_load_output.txt"
echo "  - $DIR/analysis/coldboot_check.log"
