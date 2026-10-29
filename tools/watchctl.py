#!/usr/bin/env python3
"""WatchOS Ultra host tool: talks to the watch's USB-CDC serial console.

  watchctl.py [--port PORT] time                      set RTC to this PC's clock + time zone
  watchctl.py [--port PORT] shot OUT.png [--page P]   screenshot (optionally jump to page P first)
  watchctl.py [--port PORT] tour OUTDIR               screenshot every watch face and page
  watchctl.py [--port PORT] goto P                    navigate to page P (name or id)
  watchctl.py [--port PORT] face F                    select watch face F (name or index)
  watchctl.py [--port PORT] notify TITLE BODY         show a notification
  watchctl.py [--port PORT] bench [--seconds N]       collect [PERF] frame statistics (idle + swipes)
  watchctl.py [--port PORT] dmabench                  time one full-frame DMA push (firmware 'B')
  watchctl.py [--port PORT] gestures                  synthetic swipe/key navigation test
  watchctl.py list-ports                              list serial ports

The port is taken from --port, else the WATCHCTL_PORT environment variable,
else auto-detected (Espressif USB VID 0x303A, or the only port present).
Works on Linux, macOS and Windows (COMx).

Requirements: pyserial, Pillow (for screenshots); optional: tzlocal (zone name).
"""
import argparse
import datetime
import os
import re
import struct
import sys
import time

import serial
import serial.tools.list_ports

BAUD = 115200
ESPRESSIF_VID = 0x303A

# Page ids: must match `enum PageId` in include/ui/Navigator.h (firmware 'G' command).
PAGES = {
    "launcher": 0, "face": 1, "ailimits": 2, "activity": 3, "agenda": 4,
    "position": 5, "lora": 6, "system": 7,
    "control": 8, "notifs": 9,
    "app_calendar": 10, "app_navi": 11, "app_stopwatch": 12, "app_lora": 13,
    "app_connect": 14, "app_settings": 15, "app_faces": 16, "app_power": 17,
    "app_media": 18, "calib": 19, "app_calib": 19,
    "app_games": 20, "games": 20, "game": 21,
    "app_calc": 22, "calc": 22,
}
CAROUSEL = ["launcher", "face", "ailimits", "activity", "agenda", "position", "lora", "system"]
SHEETS = ["control", "notifs"]
APPS = ["app_calendar", "app_navi", "app_stopwatch", "app_lora", "app_connect",
        "app_settings", "app_faces", "app_power", "app_media", "app_calib"]

# Watch faces: must match `enum WatchFaceId` in include/Config.h (firmware 'F' command).
FACES = [
    "modular", "chrono", "minimal", "wayfinder", "tourbillon",
    "casio", "bauhaus", "cyberpunk", "wordclock", "neon_horizon",
    "flieger", "diver", "radar", "lifetime",
]


class WatchError(RuntimeError):
    pass


# ----------------------------------------------------------------------------- port
def list_ports():
    return list(serial.tools.list_ports.comports())


def detect_port():
    ports = list_ports()
    espressif = [p for p in ports if p.vid == ESPRESSIF_VID]
    if len(espressif) == 1:
        return espressif[0].device
    if len(espressif) > 1:
        raise WatchError("several Espressif devices found, use --port: " +
                         ", ".join(p.device for p in espressif))
    if len(ports) == 1:
        return ports[0].device
    if not ports:
        raise WatchError("no serial port found (is the watch connected?)")
    raise WatchError("cannot pick a port, use --port: " + ", ".join(p.device for p in ports))


def open_port(port):
    port = port or os.environ.get("WATCHCTL_PORT") or detect_port()
    if os.name == "posix":
        import termios
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            attrs = termios.tcgetattr(fd)
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(fd, termios.TCSANOW, attrs)
            os.close(fd)
        except Exception:
            pass
    s = serial.Serial()
    s.port = port
    s.baudrate = BAUD
    s.timeout = 2.0
    s.dtr = True
    s.rts = False
    s.open()
    s.rts = False
    s.dtr = True
    time.sleep(0.1)
    s.reset_input_buffer()
    return s


# ----------------------------------------------------------------------------- helpers
def read_line(s):
    return s.readline().decode(errors="replace").strip()


def wait_line(s, prefix, timeout=5.0):
    """Return the first line starting with `prefix` within timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = read_line(s)
        if line.startswith(prefix):
            return line
    raise WatchError(f"timeout waiting for '{prefix}'")


def page_id(p):
    if isinstance(p, int):
        return p
    p = str(p).strip().lower()
    if p.isdigit():
        return int(p)
    if p in PAGES:
        return PAGES[p]
    raise WatchError(f"unknown page '{p}', known: {', '.join(PAGES)}")


def face_index(f):
    f = str(f).strip().lower()
    if f.isdigit():
        i = int(f)
    elif f in FACES:
        i = FACES.index(f)
    else:
        raise WatchError(f"unknown face '{f}', known: {', '.join(FACES)}")
    if not 0 <= i < len(FACES):
        raise WatchError(f"face index out of range: {i}")
    return i


def goto(s, page, settle=0.6):
    pid = page_id(page)
    s.write(f"G{pid}\n".encode())
    line = wait_line(s, "PAGE", 3.0)
    time.sleep(settle)                       # let the transition finish
    return line


def set_face(s, face, settle=0.8):
    idx = face_index(face)
    s.write(f"F{idx}\n".encode())
    line = wait_line(s, "FACE", 3.0)
    time.sleep(settle)
    return line


def local_zone():
    """IANA zone name of this PC, '' if unknown (the watch then uses the offset)."""
    try:
        import tzlocal  # optional dependency
        name = tzlocal.get_localzone_name()
        if name:
            return name
    except Exception:
        pass
    tz = datetime.datetime.now().astimezone().tzinfo
    key = getattr(tz, "key", None)            # zoneinfo.ZoneInfo
    if key:
        return key
    if os.name == "posix":
        env = os.environ.get("TZ", "")
        if "/" in env:
            return env.lstrip(":")
        try:
            p = os.path.realpath("/etc/localtime")
            if "zoneinfo/" in p:
                return p.split("zoneinfo/")[-1]
        except OSError:
            pass
    return ""


# ----------------------------------------------------------------------------- commands
def cmd_time(s, args):
    now = time.time()
    off = int(datetime.datetime.now().astimezone().utcoffset().total_seconds())
    zone = local_zone()
    s.reset_input_buffer()
    s.write(f"T{int(round(now))},{off},{zone}\n".encode())
    line = wait_line(s, "TIME_SET", 5.0)
    print(line)
    print(f"sent epoch={int(now)} offset={off} zone={zone or '(offset only)'}")


SNAP_RE = re.compile(r"^SNAP (\d+) (\d+) (\d+)$")


def _read_exact(s, need, timeout):
    data = bytearray()
    deadline = time.time() + timeout
    while len(data) < need and time.time() < deadline:
        want = min(need - len(data), 8192)
        try:
            chunk = s.read(want)
            if chunk:
                data += chunk
            else:
                time.sleep(0.005)
        except Exception:
            time.sleep(0.01)
    return bytes(data)


def _snapshot_once(s, path):
    from PIL import Image
    time.sleep(0.3)
    s.reset_input_buffer()
    s.write(b"S\n")
    # exact header line; other log lines before it are skipped
    deadline = time.time() + 5.0
    m = None
    while time.time() < deadline:
        line = read_line(s)
        if line == "SNAP_FAIL":
            raise WatchError("firmware could not take a snapshot (SNAP_FAIL)")
        m = SNAP_RE.match(line)
        if m:
            break
    if not m:
        raise WatchError("no SNAP header")
    w, h, stride = (int(x) for x in m.groups())
    need = stride * h
    data = _read_exact(s, need, timeout=120.0)
    if len(data) != need:
        raise WatchError(f"short read {len(data)}/{need}")
    # trailer: "\nEND\n" must follow the pixel data immediately
    trailer = _read_exact(s, 5, timeout=5.0)
    if trailer != b"\nEND\n":
        raise WatchError(f"stream corrupted (trailer {trailer!r}); background log interleaved?")
    # RGB565 little endian -> RGB888
    px = bytearray(w * h * 3)
    row_fmt = "<%dH" % w
    o = 0
    for y in range(h):
        row = struct.unpack_from(row_fmt, data, y * stride)
        for v in row:
            px[o] = ((v >> 11) & 31) * 255 // 31
            px[o + 1] = ((v >> 5) & 63) * 255 // 63
            px[o + 2] = (v & 31) * 255 // 31
            o += 3
    Image.frombytes("RGB", (w, h), bytes(px)).save(path)
    print(f"saved {path} ({w}x{h})")


def snapshot(s, path, retries=3):
    last = None
    for attempt in range(1, retries + 1):
        try:
            return _snapshot_once(s, path)
        except Exception as e:
            last = e
            print(f"  snapshot attempt {attempt} failed: {e}")
            time.sleep(1.0)
            try:
                s.reset_input_buffer()
            except Exception:
                pass
    raise WatchError(f"screenshot failed after {retries} attempts: {last}")


def cmd_shot(s, args):
    if args.page is not None:
        goto(s, args.page)
    snapshot(s, args.out)


def cmd_tour(s, args):
    os.makedirs(args.outdir, exist_ok=True)
    goto(s, "face")
    for i, name in enumerate(FACES):
        set_face(s, i)
        snapshot(s, os.path.join(args.outdir, f"face_{i:02d}_{name}.png"))
    set_face(s, 0)
    for name in CAROUSEL + SHEETS + APPS:
        try:
            goto(s, name)
        except WatchError as e:
            print(f"  skipping {name}: {e}")
            continue
        snapshot(s, os.path.join(args.outdir, f"{name}.png"))
    goto(s, "face")


def cmd_goto(s, args):
    print(goto(s, args.page))


def cmd_face(s, args):
    print(set_face(s, args.face))


def cmd_aisync(s, args):
    payload = args.json if args.json else '{"claude_5h":85,"claude_wk":42,"ag_gemini_5h":90,"ag_gemini_wk":35,"ag_claude_5h":65,"ag_claude_wk":28,"gc_5h":78,"gc_weekly":92,"gc_daily":45,"argon_weekly":80}'
    s.write(f"A{payload}\n".encode())
    line = wait_line(s, "AI_LIMITS_SET", 3.0)
    print(line or "sent")


def cmd_notify(s, args):
    title = args.title.replace("|", "/")
    s.write(f"N{title}|{args.body}\n".encode())
    print("sent")


def _perf(s, on):
    """Set the firmware's [PERF] logging (the 'P' command toggles, so check the answer)."""
    for _ in range(2):
        s.write(b"P\n")
        line = wait_line(s, "PERF", 3.0)
        if line.endswith("on") == on:
            return


def _collect_perf(s, seconds, label):
    end = time.time() + seconds
    n = 0
    while time.time() < end:
        line = read_line(s)
        if line.startswith("[PERF]"):
            print(f"{label:14s} {line}")
            n += 1
    if n == 0:
        print(f"{label:14s} (no [PERF] lines)")


def cmd_bench(s, args):
    # Honest benchmark: the firmware prints one [PERF] line per second while
    # perf logging is on. Idle watch face first, then synthetic swipes
    # (face -> right neighbour -> back) injected with the 'D' command.
    goto(s, "face")
    _perf(s, True)
    try:
        _collect_perf(s, args.seconds, "idle-face")
        for name, cmd in (("swipe-left", "D330,250,80,250,260"),
                          ("swipe-right", "D80,250,330,250,260")):
            s.write((cmd + "\n").encode())
            _collect_perf(s, 2.2, name)
    finally:
        _perf(s, False)
        goto(s, "face")


def cmd_dmabench(s, args):
    s.reset_input_buffer()
    s.write(b"B\n")
    print(wait_line(s, "BENCH", 10.0))


def cmd_gestures(s, args):
    # end-to-end navigation test with synthetic swipes (prints [NAV] stats)
    steps = [
        ("face -> ailimits (swipe left)",     "D330,250,80,250,260"),
        ("ailimits -> face (swipe right)",    "D80,250,330,250,260"),
        ("face -> control (swipe down)",      "D205,120,205,430,300"),
        ("control -> face (swipe up)",        "D205,420,205,90,300"),
        ("face -> notifications (swipe up)",  "D205,420,205,90,300"),
        ("notifications -> face (down)",      "D205,160,205,460,300"),
        ("face -> launcher (swipe right)",    "D80,250,330,250,260"),
        ("launcher -> face (side key)",       "K"),
        ("face -> launcher (side key)",       "K"),
        ("launcher -> face (side key)",       "K"),
        ("power menu (long key)",             "L"),
        ("power -> home (bottom edge up)",    "D205,496,205,120,300"),
        ("face: small drag cancels",          "D330,250,290,250,200"),
        ("diagonal swipe ~30deg (->ailimits)", "D330,200,150,310,260"),
        ("fast flick 90 ms / 120 px",         "D300,250,180,250,90"),
        ("short slow drag 35%",               "D300,250,155,250,600"),
        ("steep diagonal on page",            "D300,180,190,330,250"),
        ("flick back to face (side key)",     "K"),
        ("face: diagonal down -> control",    "D180,120,240,330,250"),
        ("control: flick up closes",          "D205,350,205,240,80"),
    ]
    goto(s, "face")
    time.sleep(2.5)                               # let neighbour surfaces prerender
    for name, cmd in steps:
        s.reset_input_buffer()
        s.write((cmd + "\n").encode())
        end = time.time() + 3.0
        got = []
        while time.time() < end:
            line = read_line(s)
            if line.startswith("[NAV]") or line.startswith("KEY"):
                got.append(line)
        print(f"{name:38s} {' | '.join(got) if got else '(no transition)'}")
        time.sleep(0.8)
    goto(s, "face")


def cmd_list_ports(args):
    ports = list_ports()
    if not ports:
        print("no serial ports found")
    for p in ports:
        vid = f"{p.vid:04X}" if p.vid is not None else "----"
        pid = f"{p.pid:04X}" if p.pid is not None else "----"
        mark = "  <- Espressif" if p.vid == ESPRESSIF_VID else ""
        print(f"{p.device:12s} {vid}:{pid}  {p.description}{mark}")


# ----------------------------------------------------------------------------- main
def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: $WATCHCTL_PORT or auto-detect)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("time", help="sync RTC + time zone from this PC").set_defaults(fn=cmd_time)

    p = sub.add_parser("shot", help="screenshot to PNG")
    p.add_argument("out")
    p.add_argument("--page", help="navigate to this page first (name or id)")
    p.set_defaults(fn=cmd_shot)

    p = sub.add_parser("tour", help="screenshot all faces and pages")
    p.add_argument("outdir")
    p.set_defaults(fn=cmd_tour)

    p = sub.add_parser("goto", help="navigate to a page")
    p.add_argument("page")
    p.set_defaults(fn=cmd_goto)

    p = sub.add_parser("face", help="select a watch face")
    p.add_argument("face")
    p.set_defaults(fn=cmd_face)

    p = sub.add_parser("aisync", help="push AI limits JSON over USB")
    p.add_argument("--json", default=None, help="custom JSON payload")
    p.set_defaults(fn=cmd_aisync)

    p = sub.add_parser("notify", help="show a notification")
    p.add_argument("title")
    p.add_argument("body")
    p.set_defaults(fn=cmd_notify)

    p = sub.add_parser("bench", help="collect [PERF] frame statistics")
    p.add_argument("--seconds", type=float, default=3.2, help="idle collection time")
    p.set_defaults(fn=cmd_bench)

    sub.add_parser("dmabench", help="time one full-frame DMA push").set_defaults(fn=cmd_dmabench)
    sub.add_parser("gestures", help="synthetic navigation test").set_defaults(fn=cmd_gestures)
    sub.add_parser("list-ports", help="list serial ports").set_defaults(fn=None)
    return ap


def main():
    args = build_parser().parse_args()
    try:
        if args.cmd == "list-ports":
            cmd_list_ports(args)
            return 0
        s = open_port(args.port)
        try:
            args.fn(s, args)
        finally:
            s.close()
    except WatchError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except serial.SerialException as e:
        print(f"serial error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
