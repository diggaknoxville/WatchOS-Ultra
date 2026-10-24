#!/usr/bin/env python3
"""KronosOS host tool for the T-Watch Ultra (USB serial).

  watchctl.py time                 set RTC to this PC's clock + timezone
  watchctl.py shot out.png [c r]   screenshot (optionally jump to tile c,r first)
  watchctl.py tour outdir          screenshot every tile
"""
import sys, time, datetime, struct
import serial

PORT = "/dev/ttyACM0"
TILES = [(1, 0, "activity"), (2, 0, "position"), (3, 0, "lora"), (4, 0, "system"), (5, 0, "connect"), (0, 1, "control")]
FACES = ["modular", "chrono", "minimal"]


def open_port():
    s = serial.Serial(PORT, 115200, timeout=0.5)
    time.sleep(0.2)
    s.reset_input_buffer()
    return s


def local_zone():
    try:
        p = __import__("os").path.realpath("/etc/localtime")
        return p.split("zoneinfo/")[1] if "zoneinfo/" in p else ""
    except Exception:
        return ""


def cmd_time(s):
    now = time.time()
    off = int(datetime.datetime.now().astimezone().utcoffset().total_seconds())
    zone = local_zone()
    s.write(f"T{int(round(now))},{off},{zone}\n".encode())
    deadline = time.time() + 3
    while time.time() < deadline:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("TIME_SET") or "Synced" in line:
            print(line)
    print(f"sent epoch={int(now)} offset={off} zone={zone}")


def wait_line(s, prefix, timeout=5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith(prefix):
            return line
    raise TimeoutError(prefix)


def snapshot(s, path):
    from PIL import Image
    s.reset_input_buffer()
    s.write(b"S\n")
    hdr = wait_line(s, "SNAP")
    _, w, h, stride = hdr.split()
    w, h, stride = int(w), int(h), int(stride)
    need = stride * h
    data = bytearray()
    deadline = time.time() + 20
    while len(data) < need and time.time() < deadline:
        data += s.read(need - len(data))
    if len(data) < need:
        raise RuntimeError(f"short read {len(data)}/{need}")
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row = y * stride
        for x in range(w):
            v = data[row + 2 * x] | (data[row + 2 * x + 1] << 8)
            px[x, y] = (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
    img.save(path)
    print(f"saved {path} ({w}x{h})")


def goto(s, p):
    s.write(f"G{p}\n".encode())
    deadline = time.time() + 2
    while time.time() < deadline:
        line = s.readline().decode(errors="replace").strip()
        if line.startswith("PAGE"):
            break
    time.sleep(0.6)


def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    s = open_port()
    op = sys.argv[1]
    if op == "time":
        cmd_time(s)
    elif op == "shot":
        if len(sys.argv) >= 4:
            goto(s, int(sys.argv[3]))
        snapshot(s, sys.argv[2])
    elif op == "tour":
        out = sys.argv[2]
        __import__("os").makedirs(out, exist_ok=True)
        goto(s, 0, 0)
        for i, name in enumerate(FACES):
            s.write(f"F{i}\n".encode())
            wait_line(s, "FACE")
            time.sleep(0.8)
            snapshot(s, f"{out}/face_{name}.png")
        s.write(b"F0\n")
        wait_line(s, "FACE")
        for c, r, name in TILES:
            goto(s, c, r)
            snapshot(s, f"{out}/{name}.png")
        goto(s, 0, 0)
    elif op == "bench":
        # idle watch face, then animated page flips; prints the [PERF] lines
        goto(s, 0, 0)
        s.write(b"P\n")
        def collect(sec, label):
            end = time.time() + sec
            while time.time() < end:
                line = s.readline().decode(errors="replace").strip()
                if line.startswith("[PERF]"):
                    print(f"{label:10s} {line}")
        collect(3.2, "idle-face")
        for col in (1, 2, 1, 0):
            s.write(f"W{col}\n".encode())
            collect(1.1, f"swipe->{col}")
        s.write(b"P\n")
    elif op == "rendercost":
        # full-screen render time of every tile and every watch face
        for i, name in enumerate(FACES):
            s.write(f"F{i}\n".encode()); wait_line(s, "FACE"); time.sleep(0.3)
            s.write(b"R0,0\n"); print(f"face {name:8s}", wait_line(s, "RENDER", 8))
        s.write(b"F0\n"); wait_line(s, "FACE")
        for c, r, name in TILES:
            s.write(f"R{c},{r}\n".encode()); print(f"tile {name:8s}", wait_line(s, "RENDER", 8))
        goto(s, 0, 0)
    elif op == "gestures":
        # end-to-end navigation test with synthetic swipes (prints [NAV] stats)
        steps = [
            ("face -> activity (swipe left)",    "D330,250,80,250,260"),
            ("activity -> face (swipe right)",   "D80,250,330,250,260"),
            ("face -> control (swipe down)",     "D205,120,205,430,300"),
            ("control -> face (swipe up)",       "D205,420,205,90,300"),
            ("face -> notifications (swipe up)", "D205,420,205,90,300"),
            ("notifications -> face (down)",     "D205,160,205,460,300"),
            ("face -> launcher (swipe right)",   "D80,250,330,250,260"),
            ("launcher -> face (side key)",      "K"),
            ("face -> launcher (side key)",      "K"),
            ("launcher -> face (side key)",      "K"),
            ("power menu (long key)",            "L"),
            ("power -> home (bottom edge up)",   "D205,496,205,120,300"),
            ("face: small drag cancels",         "D330,250,290,250,200"),
            ("diagonal swipe ~30deg (->activity)","D330,200,150,310,260"),
            ("fast flick 90 ms / 120 px (->agenda)","D300,250,180,250,90"),
            ("short slow drag 35% (->position)", "D300,250,155,250,600"),
            ("steep diagonal on page (->lora)",  "D300,180,190,330,250"),
            ("flick back to face (side key)",    "K"),
            ("face: diagonal down -> control",   "D180,120,240,330,250"),
            ("control: flick up closes",         "D205,350,205,240,80"),
        ]
        time.sleep(2.5)                               # let neighbour surfaces prerender
        for name, cmd in steps:
            s.reset_input_buffer()
            s.write((cmd + "\n").encode())
            end = time.time() + 3.0
            got = []
            while time.time() < end:
                line = s.readline().decode(errors="replace").strip()
                if line.startswith("[NAV]") or line.startswith("KEY"):
                    got.append(line)
            print(f"{name:36s} {' | '.join(got) if got else '(no transition)'}")
            time.sleep(0.8)
    elif op == "notify":
        s.write(f"N{sys.argv[2]}|{sys.argv[3]}\n".encode())
    elif op == "face":
        s.write(f"F{sys.argv[2]}\n".encode())
        print(wait_line(s, "FACE"))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
