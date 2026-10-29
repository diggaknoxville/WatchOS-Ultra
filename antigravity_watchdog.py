#!/usr/bin/env python3
"""
Antigravity Watchdog for WatchOS Ultra development environment.
Monitors real log files (Antigravity CLI / VS Code logs) for API rate limits and quota exhaustion.
Automatically pauses for 5 hours upon encountering rate limits and resumes monitoring.
"""

import os
import sys
import time
import re
import glob
from pathlib import Path

# Paths to relevant log directories in this environment
ANTIGRAVITY_LOG_DIR = Path.home() / ".gemini" / "antigravity" / "log"
ANTIGRAVITY_CLI_LOG = Path.home() / ".gemini" / "antigravity" / "cli.log"
VSCODE_LOG_DIR = Path.home() / ".config" / "Code" / "logs"

LIMIT_5H_REGEX = re.compile(
    r"(?i)(5-hour limit|rate limit reached|wait \d+ minutes|too many requests|resource_exhausted.*429|code 429|individual quota reached)"
)
WEEKLY_LIMIT_REGEX = re.compile(
    r"(?i)(weekly limit reached|quota exhausted for this week|weekly budget exceeded)"
)

CHECK_INTERVAL_SECONDS = 10
PAUSE_5H_SECONDS = 18000  # 5 hours

PID_FILE = Path("/tmp/antigravity_watchdog.pid")


def is_already_running():
    if PID_FILE.exists():
        try:
            pid = int(PID_FILE.read_text().strip())
            # Check if process with this PID is still active and is python
            if os.path.exists(f"/proc/{pid}"):
                with open(f"/proc/{pid}/cmdline", "rb") as f:
                    cmdline = f.read().decode(errors="ignore")
                    if "antigravity_watchdog" in cmdline:
                        return True
        except Exception:
            pass
    return False


def get_monitored_log_files():
    files = set()
    if ANTIGRAVITY_CLI_LOG.exists():
        files.add(ANTIGRAVITY_CLI_LOG.resolve())
    if ANTIGRAVITY_LOG_DIR.exists():
        for p in ANTIGRAVITY_LOG_DIR.glob("*.log"):
            files.add(p.resolve())
    if VSCODE_LOG_DIR.exists():
        for p in VSCODE_LOG_DIR.glob("**/*.log"):
            files.add(p.resolve())
    return list(files)


def monitor():
    PID_FILE.write_text(str(os.getpid()))
    print(f"[WATCHDOG] Gestartet (PID {os.getpid()}). Überwache Logs auf Ratenlimits...")

    file_positions = {}

    # Initialize current positions to end of existing files to only catch new log lines
    for fpath in get_monitored_log_files():
        try:
            file_positions[fpath] = fpath.stat().st_size
        except Exception:
            pass

    while True:
        try:
            log_files = get_monitored_log_files()
            for fpath in log_files:
                try:
                    current_size = fpath.stat().st_size
                    last_pos = file_positions.get(fpath, current_size)

                    if current_size > last_pos:
                        with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                            f.seek(last_pos)
                            new_lines = f.readlines()
                            file_positions[fpath] = f.tell()

                        for line in new_lines:
                            # Skip editor RPC notifications where the file content itself is echoed
                            if "textDocument/didOpen" in line or "textDocument/didChange" in line or "textDocument/didSave" in line:
                                continue
                            if "antigravity_watchdog" in line:
                                continue

                            if WEEKLY_LIMIT_REGEX.search(line):
                                print(f"[WATCHDOG] [{fpath.name}] Wochenlimit erkannt: {line.strip()}")
                                print("[WATCHDOG] Wochenlimit erreicht. Beende Watchdog dauerhaft.")
                                if PID_FILE.exists():
                                    PID_FILE.unlink(missing_ok=True)
                                sys.exit(0)

                            if LIMIT_5H_REGEX.search(line):
                                print(f"[WATCHDOG] [{fpath.name}] Ratenlimit erkannt: {line.strip()}")
                                print(f"[WATCHDOG] Pausiere für 5 Stunden ({PAUSE_5H_SECONDS}s)...")
                                time.sleep(PAUSE_5H_SECONDS)
                                print("[WATCHDOG] 5-Stunden-Pause beendet. Setze Arbeitszyklus und Überwachung fort...")
                                # Reset positions after pause
                                for p in get_monitored_log_files():
                                    try:
                                        file_positions[p] = p.stat().st_size
                                    except Exception:
                                        pass
                                break
                    elif current_size < last_pos:
                        # Log rotated / truncated
                        file_positions[fpath] = current_size
                except Exception:
                    pass

            time.sleep(CHECK_INTERVAL_SECONDS)
        except KeyboardInterrupt:
            print("\n[WATCHDOG] Beendet durch Benutzer.")
            break
        except Exception as e:
            time.sleep(CHECK_INTERVAL_SECONDS)

    if PID_FILE.exists():
        PID_FILE.unlink(missing_ok=True)


if __name__ == "__main__":
    if is_already_running():
        print("[WATCHDOG] Läuft bereits als Hintergrundprozess.")
        sys.exit(0)
    monitor()

