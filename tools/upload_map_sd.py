#!/usr/bin/env python3
"""Stream a compiled KNAV Germany map file over USB-Serial to /kronos/germany.knav on the watch's microSD card."""
import os
import sys
import time
import select

def read_line(fd, timeout=5.0):
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < timeout:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                b = os.read(fd, 1)
                if not b:
                    continue
                if b == b"\n":
                    return buf.decode("utf-8", errors="ignore").strip()
                elif b != b"\r":
                    buf.extend(b)
            except BlockingIOError:
                pass
    return buf.decode("utf-8", errors="ignore").strip()

def main():
    port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"
    map_path = sys.argv[1] if len(sys.argv) > 1 else "/home/michael/kronos-mapdata/germany_sd.knav"
    if not os.path.exists(map_path):
        print(f"Map file not found: {map_path}")
        return 1

    with open(map_path, "rb") as f:
        data = f.read()

    total = len(data)
    print(f"Uploading {map_path} ({total / 1048576.0:.2f} MB) to {port} -> /kronos/germany.knav ...")

    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)

    # Drain any pending input
    while True:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            break
        try:
            if not os.read(fd, 4096):
                break
        except BlockingIOError:
            break

    ready = False
    t0 = time.time()
    last_send = 0.0
    while time.time() - t0 < 15.0:
        if time.time() - last_send >= 2.0:
            os.write(fd, f"U{total}\n".encode("ascii"))
            last_send = time.time()
        line = read_line(fd, timeout=0.5)
        if line:
            print("  <", line)
        if "UP_READY" in line:
            ready = True
            break
        if "UP_ERR" in line:
            print("Watch reported error opening SD file!")
            os.close(fd)
            return 1

    if not ready:
        print("Timed out waiting for UP_READY")
        os.close(fd)
        return 1

    sent = 0
    chunk_size = 4096
    start_t = time.time()
    last_print = start_t
    while sent < total:
        chunk = data[sent:sent + chunk_size]
        # Write 4096 bytes
        off = 0
        while off < len(chunk):
            _, w, _ = select.select([], [fd], [], 1.0)
            if w:
                n = os.write(fd, chunk[off:])
                off += n

        got_ack = False
        ack_t0 = time.time()
        while time.time() - ack_t0 < 6.0:
            ack = read_line(fd, timeout=1.0)
            if ack == "K" or ack.endswith("K"):
                got_ack = True
                break
            elif "UP_DONE" in ack:
                print("\n  <", ack)
                got_ack = True
                break
            elif ack:
                print("\n  [log]", ack)
        if not got_ack:
            print(f"\nMissing ACK at {sent}/{total}")
            break

        sent += len(chunk)
        now = time.time()
        if now - last_print >= 1.0 or sent == total:
            elapsed = max(0.001, now - start_t)
            rate = (sent / 1024.0) / elapsed
            pct = sent * 100.0 / total
            print(f"\r  {pct:5.1f}% ({sent // 1024} / {total // 1024} KB) @ {rate:.1f} KB/s", end="", flush=True)
            last_print = now

    print()
    for _ in range(4):
        done_line = read_line(fd, timeout=1.5)
        if done_line:
            print("  <", done_line)
        if "UP_DONE" in done_line:
            break
    os.close(fd)
    return 0

if __name__ == "__main__":
    sys.exit(main())

