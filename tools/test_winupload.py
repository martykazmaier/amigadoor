#!/usr/bin/env python3
"""test_winupload.py -- a ZMODEM upload into the Windows door (amigadoor.exe), from WSL (the C64 door's, without its
download half: the Amiga door offers no downloads).

Plays a SyncTERM without key reports through tools/winlaunch.exe (as test_windoor.py), opens the menu (Ctrl-Q), U, and
when the door is ready hands the connection to a real `sz` (lrzsz: a local one, or SZ="ssh host sz" for one on another
machine), which sends the disk. Then checks the door kept it byte for byte in players/player-1/, and leaves.

    python3 tools/test_winupload.py <folder with amigadoor.exe> <disk.adf or game.lha>
"""
import hashlib
import os
import re
import select
import shlex
import socket
import subprocess
import sys
import threading
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
FOLDER = os.path.abspath(sys.argv[1])
DISK = sys.argv[2]
SZ = shlex.split(os.environ.get('SZ', 'sz'))

ours, theirs = socket.socketpair()
exe = subprocess.check_output(['wslpath', '-w', os.path.join(FOLDER, 'amigadoor.exe')], text=True).strip()
door = subprocess.Popen([os.path.join(TOOLS, 'winlaunch.exe'), exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        cwd=FOLDER)


def up():
    while (data := theirs.recv(65536)):
        door.stdin.write(data)
        door.stdin.flush()


def down():
    while (data := door.stdout.read1(65536)):
        theirs.sendall(data)
    theirs.shutdown(socket.SHUT_WR)


threading.Thread(target=up, daemon=True).start()
threading.Thread(target=down, daemon=True).start()
out = bytearray()
seen = 0                                                # how much of out has been answered
sizes_asked = False


def pump(timeout=0.05):
    """Reads what the door sent and answers its questions, in order. False when the door has gone."""
    global seen, sizes_asked
    if select.select([ours], [], [], timeout)[0]:
        data = ours.recv(65536)
        if not data:
            return False
        out.extend(data)
    for m in re.finditer(rb'\x1b\[c|\x1b\[5n|\x1b\[6n', bytes(out[max(0, seen - 8):])):
        if max(0, seen - 8) + m.end() <= seen:
            continue
        if m.group(0) == b'\x1b[c':
            ours.sendall(b'\x1b[=67;84;101;114;109;1;332c')
        elif m.group(0) == b'\x1b[5n':
            ours.sendall(b'\x1b[=1;1-n\x1b[?2;0;640;400S\x1b[0n')
            sizes_asked = True
        else:
            ours.sendall(b'\x1b[1;1R' if sizes_asked else b'\x1b[25;80R')
    seen = len(out)
    return True


def wait_for(text, timeout=30):
    mark = len(out)
    end = time.time() + timeout
    while time.time() < end:
        if text.encode() in out[mark:]:
            return
        if not pump():
            break
    sys.exit(f'FAIL: never saw {text!r}: {bytes(out[-1500:])!r}')


wait_for('Press Y to agree')
time.sleep(0.5)
ours.sendall(b'y')
wait_for('Press any key to switch on')
ours.sendall(b' ')
for _ in range(40):
    pump()
ours.sendall(b'\x11')                                   # Ctrl-Q: the menu
wait_for('Amiga paused')
ours.sendall(b'u')
wait_for('Start a ZMODEM upload')
time.sleep(0.5)
while pump(0.2):                                        # what's left before the transfer starts
    if out.endswith(b'\x11') or b'\x18B01' in out[-40:]:
        break

# The connection to sz until it's done
sz = subprocess.Popen(SZ + ['-b', os.path.basename(DISK)], cwd=os.path.dirname(os.path.abspath(DISK)) or '.',
                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
mark = len(out)


def to_sz():
    try:
        while sz.poll() is None:
            if select.select([ours], [], [], 0.1)[0]:
                data = ours.recv(65536)
                if not data:
                    break
                sz.stdin.write(data)
                sz.stdin.flush()
    except (OSError, ValueError):
        pass


t = threading.Thread(target=to_sz, daemon=True)
t.start()
start = time.time()
while (data := sz.stdout.read1(65536)):
    ours.sendall(data)
sz.wait()
t.join()
print(f'sz exited with {sz.returncode} after {time.time() - start:.1f} s')

wait_for('keeps what\'s in')
kept = os.path.join(FOLDER, 'players', 'player-1', os.path.basename(DISK).lower())
candidates = [os.path.join(os.path.dirname(kept), f) for f in os.listdir(os.path.dirname(kept))
              if os.path.isfile(os.path.join(os.path.dirname(kept), f))]       # not amiga\, the core's own files
print('arrived:', bytes(out[mark:]).decode('cp437', 'replace').split('Arrived')[1][:60].strip() if b'Arrived' in out[mark:]
      else 'nothing said')
want = hashlib.md5(open(DISK, 'rb').read()).hexdigest()
got = {os.path.basename(c): hashlib.md5(open(c, 'rb').read()).hexdigest() for c in candidates}
print('kept:', got)
ok = want in got.values()
ours.sendall(b'x')                                      # keep what's in the Amiga: back to the menu
wait_for('Amiga paused', 10)
ours.sendall(b'q')
wait_for('Thanks for visiting')
ours.sendall(b' ')
print('door exited with', door.wait(timeout=30))
print('PASS: the disk arrived whole' if ok else 'FAIL: the kept disk differs')
sys.exit(0 if ok else 1)
