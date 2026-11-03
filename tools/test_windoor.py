#!/usr/bin/env python3
"""test_windoor.py -- door/test_door.py, run against the Windows door (amigadoor.exe), from WSL.

tools/winlaunch.exe plays Mystic for Windows: a real loopback TCP connection, DOOR32.SYS with comm type 2 and the socket
handle, telnet commands up front, 0xFF doubled both ways. Its stdin/stdout are the caller's side, which this relays to
test_door.py's fake SyncTERM in place of the pseudo-terminal it uses on Linux. (As the Duke Nukem 3D door's.)

    python3 tools/test_windoor.py <folder with amigadoor.exe> [test_door.py's options...]

The folder needs the core, libjxl's DLLs, the Kickstart ROMs and the library, as on a BBS. Never the live door folder: the door
writes logs/ and players/ beside itself.
"""
import os
import pty
import socket
import subprocess
import sys
import threading

TOOLS = os.path.dirname(os.path.abspath(__file__))
LAUNCHER = os.path.join(TOOLS, 'winlaunch.exe')
FOLDER = os.path.abspath(sys.argv[1])
EXE = os.path.join(FOLDER, 'amigadoor.exe')
launched = {}


def winpath(p):
    return subprocess.check_output(['wslpath', '-w', os.path.abspath(p)], text=True).strip()


def fake_fork():
    """pty.fork() for test_door.py: the parent's side only, with the door behind winlaunch"""
    ours, theirs = socket.socketpair()
    proc = subprocess.Popen([LAUNCHER, winpath(EXE)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, cwd=FOLDER)
    launched['proc'] = proc

    def up():       # fake terminal -> door
        try:
            while True:
                data = theirs.recv(65536)
                if not data:
                    break
                proc.stdin.write(data)
                proc.stdin.flush()
        except OSError:
            pass

    def down():     # door -> fake terminal
        while True:
            data = proc.stdout.read1(65536)
            if not data:
                break
            theirs.sendall(data)
        theirs.shutdown(socket.SHUT_WR)

    threading.Thread(target=up, daemon=True).start()
    threading.Thread(target=down, daemon=True).start()
    return proc.pid, ours.detach()


pty.fork = fake_fork                    # test_door.py's os.waitpid then waits for winlaunch, which ends with the door
script = os.path.join(TOOLS, '..', 'door', 'test_door.py')
sys.argv = [script] + sys.argv[2:]
exec(compile(open(script).read(), script, 'exec'))
