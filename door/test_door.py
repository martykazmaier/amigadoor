#!/usr/bin/env python3
"""
test_door.py -- runs the Amiga door on a pseudo-terminal and pretends to be SyncTERM (taken from the C64 door's).
No BBS and no terminal needed.

It answers the door's questions (CTerm version, JPEG XL, sound files, key reports, screen size), agrees to the
disclaimer, picks a library title, plays it for a while with the joystick (key reports: right held, fire pulses) and
the mouse (SGR reports, in pixels as CTerm 1.330+ sends them), opens the F12 menu and leaves, then quits the door. It
answers every frame's cursor-position request, so the door's pacing runs as on a fast link (--ack-delay slows that).

    python3 test_door.py                    # library title 1, 20 s
    python3 test_door.py --title 3 --seconds 60 --swap-at 30   # disk 2 in at 30 s
    python3 test_door.py --title 2 --seconds 40 --reset-at 20  # F12, R at 20 s: the Amiga resets
    python3 test_door.py --title 2 --machine-at 15             # F12, A at 15 s: the other machine
    python3 test_door.py --kept --seconds 30   # kept disk 1 from players/<player>/ (put an .adf there first)
    python3 test_door.py --empty            # no disk: switch on and wait
    python3 test_door.py --nokeys           # a terminal without key reports (typed keys only)
    python3 test_door.py --cells            # an older CTerm (1.329): mouse reports in text cells
    python3 test_door.py --rows 24          # an 80x24 SyncTERM (640x384)

Run it in the folder the door is installed in (or give --door). Never in the live door folder on the BBS: the door
writes its log under logs/ beside itself.
"""

import argparse
import os
import pty
import re
import select
import sys
import time

p = argparse.ArgumentParser()
p.add_argument("--door", default="./amigadoor")
p.add_argument("--title", type=int, default=1)
p.add_argument("--seconds", type=float, default=20)
p.add_argument("--empty", action="store_true")
p.add_argument("--a500", action="store_true", help="switch to the A500 first (F12, A): --title counts its games")
p.add_argument("--mouse-cal", action="store_true", help="after --seconds of nothing: the mouse 100 Amiga pixels right, "
               "then 100 down, slowly (dump frames to see where the pointer goes)")
p.add_argument("--cells", action="store_true", help="CTerm 1.329: no pixel mouse reports")
p.add_argument("--ack-delay", type=float, default=0, help="seconds before each frame is answered (a slow link)")
p.add_argument("--machine-at", type=float, default=0, help="seconds in: F12, A (the other machine)")
p.add_argument("--nokeys", action="store_true")
p.add_argument("--nosound", action="store_true")
p.add_argument("--swap-at", type=float, default=0, help="seconds in: F12, D (next disk), back to the C64")
p.add_argument("--kept", action="store_true", help="boot kept disk 1 (F12, M, 1, A) instead of a library title")
p.add_argument("--reset-at", type=float, default=0, help="seconds in: F12, R (reset)")
p.add_argument("--rows", type=int, default=25, help="the screen's text rows: 25 is 640x400, 24 is 640x384")
args = p.parse_args()

pid, fd = pty.fork()
if pid == 0:
    os.execv(args.door, [args.door])

out = bytearray()           # everything the door has sent
seen = 0                    # how much of it the responder has looked at
stats = {"frames": 0, "frame_bytes": 0, "sound_pieces": 0, "acks": 0, "asked_size": False}
acks_due = []               # (when, how many) frame answers held back by --ack-delay


def send(data):
    os.write(fd, data if isinstance(data, bytes) else data.encode())


def pump(timeout=0.05):
    """Reads what the door sent and answers its questions. False when the door has gone."""
    global seen
    r, _, _ = select.select([fd], [], [], timeout)
    if r:
        try:
            data = os.read(fd, 65536)
        except OSError:
            return False
        if not data:
            return False
        out.extend(data)
    new = bytes(out[max(0, seen - 16):])
    start = max(0, seen - 16)
    # (no ESC inside a picture's header: the C64 door's [^;]* could run on through the picture into the next frame's
    # CSI 6n when no ZX/ZY followed, and that frame then went unanswered)
    for m in re.finditer(rb"\x1b\[c|\x1b\[5n|\x1b\[6n|C;DrawJXLBlob;[^;\x1b]*;[^;\x1b]*;(?:[^;\x1b]*;[^;\x1b]*;)?|A;Queue;",
                         new):
        if start + m.end() <= seen:
            continue
        tok = m.group(0)
        if tok == b"\x1b[c":
            send("\x1b[=67;84;101;114;109;1;329c" if args.cells else "\x1b[=67;84;101;114;109;1;332c")
        elif tok == b"\x1b[5n":
            answers = "\x1b[=1;1-n"
            if not args.nosound:
                answers += "\x1b[=7;100;1n\x1b[=7;101;1;5;1n"
            if not args.nokeys:
                answers += "\x1b[<0;8c"
            answers += f"\x1b[?2;0;640;{args.rows * 16}S\x1b[0n"
            send(answers)
            stats["asked_size"] = True
        elif tok == b"\x1b[6n" and not stats["asked_size"]:
            send(f"\x1b[{args.rows};80R")               # the screen-size question: the cursor stopped at the corner
        elif tok == b"\x1b[6n":
            if args.ack_delay > 0:
                acks_due.append(time.time() + args.ack_delay)
            else:
                send("\x1b[1;1R")
            stats["acks"] += 1
        elif tok.startswith(b"C;DrawJXLBlob"):
            stats["frames"] += 1
        elif tok == b"A;Queue;":
            stats["sound_pieces"] += 1
    seen = len(out)
    while acks_due and acks_due[0] <= time.time():
        acks_due.pop(0)
        send("\x1b[1;1R")
    return True


def wait_for(text, timeout=15):
    mark = len(out)
    end = time.time() + timeout
    while time.time() < end:
        if not pump():
            return False
        if text.encode() in out[max(0, mark - 200):]:
            return True
    print(f"FAIL: never saw {text!r}")
    print(bytes(out[-600:]))
    sys.exit(1)


def mouse(x, y, button=None, press=True):
    """An SGR mouse report: motion, or a left-button press or release, at a screen pixel (or its text cell)"""
    if args.cells:
        x, y = x // 8 + 1, y // 16 + 1
    else:
        x, y = x + 1, y + 1
    b = 35 if button is None else 0
    send(f"\x1b[<{b};{x};{y}{'M' if press else 'm'}")


def play_for(seconds, quiet=0):
    """Joystick and mouse for a while; nothing pressed in the first quiet seconds (WHDLoad's boot opens its game
    chooser when fire is held as it starts)"""
    start = time.time()
    t = quiet
    while time.time() - start < seconds:
        pump(0.02)
        now = time.time() - start
        if now > t:
            k = int(now * 2)
            mouse(200 + (k % 20) * 10, 150 + (k % 10) * 8)     # the mouse wanders over the picture...
            if k % 6 == 0:
                mouse(320, 200, 0, True)                        # ...and clicks
            elif k % 6 == 1:
                mouse(320, 200, 0, False)
            t += 0.5
            if args.nokeys:
                send("\x1b[C")                       # a typed arrow
                if int(now * 2) % 4 == 0:
                    send(" ")                        # fire, typed
            else:
                send("\x1b[=106K")                   # right held
                if int(now * 2) % 4 == 0:
                    send("\x1b[=97K")                # fire (right Ctrl) down...
                elif int(now * 2) % 4 == 1:
                    send("\x1b[=97k")                # ...and up


def open_menu(ctrl_q=False):
    if args.nokeys:
        send("\x11" if ctrl_q else "\x1b[24~")
    elif ctrl_q:
        send("\x1b[=106k\x1b[=29K\x1b[=16K\x1b[=16k\x1b[=29k")     # let go of right; Ctrl-Q
    else:
        send("\x1b[=106k\x1b[=88K\x1b[=88k")                       # let go of right; F12
    wait_for("Amiga paused")


wait_for("Press Y to agree")
send("y")
wait_for("Press any key to switch on")
send(" ")
play_for(3)                                          # the Amiga waiting for a disk
frames_before = stats["frames"]
t0 = time.time()
if args.empty:
    play_for(args.seconds)
else:
    if args.a500:
        open_menu()
        send("a")                                    # the A500: its own library list
        play_for(3, 3)
    open_menu(ctrl_q=True)                           # Ctrl-Q opens the same menu as F12
    boot = 8
    if args.kept:
        send("m")
        wait_for("kept.")
        send("1")
        wait_for("keeps what's in")
        send("a")                                    # the Amiga restarts with kept disk 1
    else:
        send("l")
        wait_for("Enter loads it")
        for _ in range(args.title - 1):
            send("\x1b[B")                           # lightbar: down to that title
        send("\r")                                   # Enter loads it
    if args.swap_at > 0:
        play_for(args.swap_at, boot)
        open_menu()
        send("d")
        wait_for("disk 2 of")
        send(" ")
        play_for(args.seconds - args.swap_at)
    elif args.reset_at > 0:
        play_for(args.reset_at, boot)
        open_menu()
        send("r")                                    # the Amiga resets, the disk still in DF0:
        play_for(args.seconds - args.reset_at, boot)
    elif args.machine_at > 0:
        play_for(args.machine_at, boot)
        open_menu()
        send("a")                                    # the other machine: the Amiga restarts
        play_for(args.seconds - args.machine_at, boot)
    elif args.mouse_cal:
        play_for(args.seconds, args.seconds)         # nothing pressed while it loads
        x0, y0 = 300, 200                             # 514x384 picture: 1.428 screen pixels an Amiga one across,
        mouse(x0, y0)                                 # 1.338 down
        play_for(1, 1)
        for i in range(1, 21):
            mouse(x0 + round(i * 142.8 / 20), y0)
            play_for(0.1, 1)
        play_for(2, 2)
        for i in range(1, 21):
            mouse(x0 + 143, y0 + round(i * 133.8 / 20))
            play_for(0.1, 1)
        play_for(2, 2)
    else:
        play_for(args.seconds, boot)
elapsed = time.time() - t0
frames = stats["frames"] - frames_before
mb = len(out) / 1048576

open_menu()
send("q")                                            # Q: back to the BBS
end = time.time() + 15
while time.time() < end:
    pump()
    tail = bytes(out[-3000:])
    if b"Download the changed disk" in tail:
        send("n")
        out.extend(b"[answered]")
    if b"Thanks for visiting" in tail:
        break
wait_for("Press any key")
send(" ")
for _ in range(100):
    if not pump():
        break

print(f"played {elapsed:.1f} s: {frames} pictures ({frames / elapsed:.1f} a second), {mb:.2f} MB sent in all "
      f"({len(out) / elapsed / 1024:.0f} KB/s), {stats['sound_pieces']} sound pieces "
      f"({stats['sound_pieces'] / elapsed:.1f} a second), {stats['acks']} frame answers")
print("key reports were switched on" if b"\x1b[=1h" in out else "key reports were NOT switched on")
print("mouse reports were switched on" + (" (pixels)" if b"\x1b[?1016h" in out else " (cells)")
      if b"\x1b[?1003h" in out else "mouse reports were NOT switched on")
_, status = os.waitpid(pid, 0)
code = os.waitstatus_to_exitcode(status)
print(f"door exited with {code}")
bar = f"\x1b[{args.rows};1H\x1b[0;44m".encode() in out
print(f"controls row drawn on row {args.rows}" if bar else f"controls row NOT drawn on row {args.rows}")
stray = out.count(b"retro_")                         # the core's own chatter must never reach the caller
print(f"core chatter on the caller's screen: {stray} times" if stray else "no core chatter on the caller's screen")
ok = frames > 0 and (args.nosound or stats["sound_pieces"] > 0) and bar and stray == 0 and code == 0
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
