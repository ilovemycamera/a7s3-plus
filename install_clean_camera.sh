#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
export DYLD_FALLBACK_LIBRARY_PATH="$DIR/vendor:/opt/homebrew/lib:/usr/local/lib:${DYLD_FALLBACK_LIBRARY_PATH:-}"

choose_python() {
    if [ -x "$DIR/.venv/bin/python" ] && "$DIR/.venv/bin/python" -c "import sys; sys.path.insert(0, '$DIR/vendor'); import usb.core" >/dev/null 2>&1; then
        PY="$DIR/.venv/bin/python"
        return
    fi
    if command -v python3 >/dev/null 2>&1 && python3 -c "import sys; sys.path.insert(0, '$DIR/vendor'); import usb.core" >/dev/null 2>&1; then
        PY="$(command -v python3)"
        return
    fi
    echo "=== Creating local .venv and installing pyusb ==="
    rm -rf "$DIR/.venv"
    python3 -m venv "$DIR/.venv"
    "$DIR/.venv/bin/pip" install --upgrade pip pyusb
    PY="$DIR/.venv/bin/python"
}

# Verify libusb is reachable on macOS before prompting for sudo
if [ "$(uname -s)" = "Darwin" ]; then
    if [ ! -f "$DIR/vendor/libusb-1.0.dylib" ] && [ ! -f "/opt/homebrew/lib/libusb-1.0.dylib" ] && [ ! -f "/usr/local/lib/libusb-1.0.dylib" ]; then
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
