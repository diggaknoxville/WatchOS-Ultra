#!/usr/bin/env python3
"""Stream a compiled KNAV map file over USB serial to /kronos/germany.knav on the
watch's microSD card.

Protocol (serial console of the watch, 115200 baud):
    host  -> watch   "U<total_bytes>\\n"
    watch -> host    "UP_READY"            (or "UP_ERR_OPEN")
    host  -> watch   4096-byte chunks (last one shorter)
    watch -> host    "K"                   exactly, one line per chunk
    watch -> host    "UP_DONE ok=1 recvd=<n> map=<0|1>"

Works on Windows, macOS and Linux (pyserial, no select() on file descriptors).

Usage:
    upload_map_sd.py <map.knav> [--port COM5|/dev/ttyACM0] [--chunk 4096]
    Port default: $WATCHCTL_PORT, else the first port pyserial lists.
"""
import argparse
import os
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("pyserial is required: pip install pyserial  (see tools/requirements.txt)")
    sys.exit(2)

MAGICS = (b"KNAVDE02", b"KNAVDE03")


def pick_port(explicit):
    if explicit:
        return explicit
    env = os.environ.get("WATCHCTL_PORT")
    if env:
        return env
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        return None
    # prefer something that looks like a USB CDC device
    for p in ports:
        desc = (p.description or "").lower()
        if "usb" in desc or "cdc" in desc or "jtag" in desc or "espressif" in desc:
            return p.device
    return ports[0].device


class LineReader:
    """Line reader on top of pyserial with a timeout and a leftover buffer."""

    def __init__(self, ser):
        self.ser = ser
        self.buf = bytearray()

    def read_line(self, timeout):
        deadline = time.monotonic() + timeout
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = bytes(self.buf[:nl])
                del self.buf[:nl + 1]
                return line.decode("utf-8", errors="replace").strip("\r\n")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.ser.timeout = min(0.05, remaining)
            chunk = self.ser.read(256)
            if chunk:
                self.buf.extend(chunk)

    def drain(self, seconds=0.3):
        end = time.monotonic() + seconds
        self.ser.timeout = 0.05
        while time.monotonic() < end:
            if not self.ser.read(4096):
                break
        self.buf.clear()


def main():
    ap = argparse.ArgumentParser(description="Upload a KNAV map to the watch SD card over serial.")
    ap.add_argument("map", help="path to the .knav file")
    ap.add_argument("--port", help="serial port (default: $WATCHCTL_PORT or first available)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--chunk", type=int, default=4096, help="chunk size, must match the firmware (4096)")
    ap.add_argument("--ack-timeout", type=float, default=8.0, help="seconds to wait for each K")
    args = ap.parse_args()

    if not os.path.isfile(args.map):
        print(f"Map file not found: {args.map}")
        return 1
    with open(args.map, "rb") as f:
        data = f.read()
    total = len(data)
    if total < 52 or data[:8] not in MAGICS:
        print("This does not look like a KNAV map file (bad magic).")
        return 1

    port = pick_port(args.port)
    if not port:
        print("No serial port found. Use --port or set WATCHCTL_PORT.")
        return 1

    print(f"Uploading {args.map} ({total / 1048576.0:.2f} MB) via {port} -> /kronos/germany.knav")
    try:
        ser = serial.Serial(port, args.baud, timeout=0.05, write_timeout=5.0)
    except serial.SerialException as e:
        print(f"Cannot open {port}: {e}")
        return 1

    rd = LineReader(ser)
    try:
        rd.drain(0.5)

        # --- handshake ---
        ready = False
        t0 = time.monotonic()
        last_send = 0.0
        while time.monotonic() - t0 < 20.0:
            if time.monotonic() - last_send >= 2.0:
                ser.write(f"U{total}\n".encode("ascii"))
                ser.flush()
                last_send = time.monotonic()
            line = rd.read_line(0.5)
            if line is None:
                continue
            if line:
                print("  <", line)
            if line.startswith("UP_READY"):
                ready = True
                break
            if line.startswith("UP_ERR"):
                print("Watch could not open the SD file for writing.")
                return 1
        if not ready:
            print("Timed out waiting for UP_READY (is the watch connected and the console idle?)")
            return 1

        # --- data ---
        sent = 0
        start_t = time.monotonic()
        last_print = start_t
        chunk_size = args.chunk
        while sent < total:
            chunk = data[sent:sent + chunk_size]
            ser.write(chunk)
            ser.flush()

            got_ack = False
            done_line = None
            ack_deadline = time.monotonic() + args.ack_timeout
            while time.monotonic() < ack_deadline:
                line = rd.read_line(ack_deadline - time.monotonic())
                if line is None:
                    break
                if line == "K":              # ACK must be exactly "K"
                    got_ack = True
                    break
                if line.startswith("UP_DONE"):
                    done_line = line
                    break
                if line:
                    print("\n  [log]", line)
            if done_line:
                print("\n  <", done_line)
                print("Watch finished early.")
                return 1
            if not got_ack:
                print(f"\nMissing ACK at {sent}/{total} bytes")
                return 1

            sent += len(chunk)
            now = time.monotonic()
            if now - last_print >= 0.5 or sent == total:
                elapsed = max(0.001, now - start_t)
                rate = (sent / 1024.0) / elapsed
                pct = sent * 100.0 / total
                eta = (total - sent) / 1024.0 / max(rate, 0.001)
                print(f"\r  {pct:5.1f}%  {sent // 1024:7d} / {total // 1024} KB  {rate:6.1f} KB/s  ETA {eta:5.0f} s",
                      end="", flush=True)
                last_print = now
        print()

        # --- completion ---
        done = None
        deadline = time.monotonic() + 30.0   # reloadMap + write test may take a moment
        while time.monotonic() < deadline:
            line = rd.read_line(deadline - time.monotonic())
            if line is None:
                break
            if line:
                print("  <", line)
            if line.startswith("UP_DONE"):
                done = line
                break
        if not done:
            print("No UP_DONE from the watch.")
            return 1
        fields = dict(kv.split("=", 1) for kv in done.split()[1:] if "=" in kv)
        if fields.get("ok") != "1":
            print(f"Upload FAILED: {done}")
            return 1
        if fields.get("recvd") not in (None, str(total)):
            print(f"Upload size mismatch: watch received {fields.get('recvd')} of {total} bytes")
            return 1
        print(f"Upload OK ({total} bytes). Map loaded on watch: {'yes' if fields.get('map') == '1' else 'NO'}")
        return 0 if fields.get("map") == "1" else 3
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
