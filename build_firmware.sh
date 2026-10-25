#!/usr/bin/env bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo "======================================================="
echo "   WatchOS Ultra // LilyGO T-Watch Ultra Build Tool   "
echo "======================================================="

ACTION="${1:-build}"
if [ $# -gt 0 ]; then shift; fi

PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --port)   PORT="$2"; shift 2 ;;
        --port=*) PORT="${1#--port=}"; shift ;;
        *) echo "Unbekannte Option: $1"; exit 1 ;;
    esac
done

# Python-Interpreter (python3 auf Linux/macOS, meist "python" unter Windows/Git-Bash)
PYTHON="${PYTHON:-}"
if [ -z "$PYTHON" ]; then
    if command -v python3 >/dev/null 2>&1; then PYTHON=python3; else PYTHON=python; fi
fi

PIO_UPLOAD_ARGS=()
PIO_MONITOR_ARGS=()
WATCHCTL=("$PYTHON" tools/watchctl.py)
if [ -n "$PORT" ]; then
    PIO_UPLOAD_ARGS=(--upload-port "$PORT")
    PIO_MONITOR_ARGS=(--port "$PORT")
    WATCHCTL+=(--port "$PORT")
fi

case "$ACTION" in
    build|compile)
        echo "[BUILD] Kompiliere WatchOS Ultra Firmware mit PlatformIO..."
        pio run
        ;;
    upload|flash)
        echo "[FLASH] Flashe WatchOS Ultra auf die verbundene Uhr..."
        pio run -t upload "${PIO_UPLOAD_ARGS[@]}"
        ;;
    clean)
        echo "[CLEAN] Bereinige Build-Dateien..."
        pio run -t clean
        ;;
    monitor)
        echo "[MONITOR] Starte seriellen Monitor (115200 Baud)..."
        pio device monitor -b 115200 "${PIO_MONITOR_ARGS[@]}"
        ;;
    tour)
        echo "[TOUR] Erstelle Screenshots aller Zifferblätter und Seiten via watchctl.py..."
        mkdir -p screenshots
        "${WATCHCTL[@]}" tour screenshots
        ;;
    shot)
        echo "[SHOT] Erstelle Screenshot der aktuellen Anzeige..."
        mkdir -p screenshots
        "${WATCHCTL[@]}" shot "screenshots/shot_$(date +%Y%m%d_%H%M%S).png"
        ;;
    sync-time)
        echo "[TIME] Synchronisiere Uhrzeit & Zeitzone mit dem Host-PC..."
        "${WATCHCTL[@]}" time
        ;;
    *)
        echo "Verwendung: $0 [build | upload | clean | monitor | tour | shot | sync-time] [--port PORT]"
        echo "  --port PORT   serieller Port (z. B. /dev/ttyACM0 oder COM5); sonst WATCHCTL_PORT bzw. Auto-Erkennung"
        exit 1
        ;;
esac
