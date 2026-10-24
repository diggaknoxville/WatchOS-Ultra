#!/usr/bin/env bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo "======================================================="
echo "   WatchOS Ultra // LilyGO T-Watch Ultra Build Tool   "
echo "======================================================="

ACTION="${1:-build}"

case "$ACTION" in
    build|compile)
        echo "[BUILD] Kompiliere WatchOS Ultra Firmware mit PlatformIO..."
        pio run
        ;;
    upload|flash)
        echo "[FLASH] Flashe WatchOS Ultra auf verbundene Smartwatch..."
        pio run -t upload
        ;;
    clean)
        echo "[CLEAN] Bereinige Build-Dateien..."
        pio run -t clean
        ;;
    monitor)
        echo "[MONITOR] Starte Seriellen Monitor (115200 Baud)..."
        pio device monitor -b 115200
        ;;
    tour)
        echo "[TOUR] Erstelle Screenshots aller Kacheln via watchctl.py..."
        mkdir -p screenshots
        python3 tools/watchctl.py tour screenshots
        ;;
    sync-time)
        echo "[TIME] Synchronisiere Uhrzeit & Zeitzone mit Host-PC..."
        python3 tools/watchctl.py time
        ;;
    *)
        echo "Verwendung: $0 [build | upload | clean | monitor | tour | sync-time]"
        exit 1
        ;;
esac
