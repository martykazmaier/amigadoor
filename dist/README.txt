AMIGA door for Windows BBSes (32-bit)
=====================================

An Amiga 500 and an Amiga 1200 as one DOOR32 door. PUAE (the libretro build
of UAE) runs on the BBS machine and the caller plays it in SyncTERM: the
Amiga's screen is sent as JPEG XL pictures, with its sound. Callers load games
from your library or upload their own by ZMODEM (kept for their next visit),
and switch between the A500 and the A1200 from the F12 menu.
Built 2026-11-22. Nothing to compile and nothing else to install, except the
Kickstart ROMs and the games (see THE ROMS and LIBRARY below: they can't be
included).


WHAT CALLERS NEED
-----------------
SyncTERM 1.2 or newer (it draws JPEG XL pictures and plays sound files).
SyncTERM 1.330 or newer reports the mouse to the pixel; older versions to the
text cell, which is coarse but clicking works. Any terminal that can't draw
the pictures gets a page saying so and goes back to the BBS.


REQUIREMENTS
------------
- Windows 10 / 11 / Server 2016 or later, 32-bit or 64-bit (this 32-bit
  build runs on both).
- A BBS that runs DOOR32 doors and passes the caller's TELNET SOCKET in
  DOOR32.SYS (comm type 2), as Mystic BBS for Windows does.
- The door folder must be writable by the account the BBS runs as (the door
  keeps callers' disks and saved games in .\players\ and logs in .\logs\).
- A reasonable CPU: each caller playing takes about half to one core.
- Bandwidth: a busy game sends 0.5 to 1.5 MB a second at full speed. On a
  slower connection the door sends fewer pictures, and switches to slightly
  blurrier (lossy) ones while the link can't keep up, so it still plays.


INSTALL
-------
1. Unzip this whole folder to e.g.  C:\mystic\doors\amiga\
   Keep every file together: the .dll files are only looked for beside
   amigadoor.exe.

2. Put your Kickstart ROMs in system\ (THE ROMS, below).

3. Put some games in library\ and list them in library\library.txt
   (LIBRARY, below). The door works without any, but callers then only get
   the Amiga's insert-disk screen and their own uploads.

4. Add a menu command that runs it as a DOOR32 door, giving it the folder the
   BBS writes DOOR32.SYS into for that node (or the full path of the
   DOOR32.SYS file itself; either works).

   Mystic BBS for Windows:
     Command:  (D3) Exec DOOR32 program
     Data:     C:\mystic\doors\amiga\amigadoor.exe C:\mystic\temp%3

   (%3 is the node number; Mystic writes DOOR32.SYS in temp<node>. Change the
   paths to match where Mystic and the door live.)

   Other DOOR32 BBS software: run amigadoor.exe with the node's drop-file
   folder (or the DOOR32.SYS path) as its only argument.

5. Try it from a test account in SyncTERM.


THE ROMS
--------
The Amiga's Kickstart ROMs are still under copyright, so they are not in this
zip. Copy your own into the system\ folder under these exact names:

  system\kick34005.A500    262,144 bytes  Kickstart 1.3 (34.5), A500
  system\kick40068.A1200   524,288 bytes  Kickstart 3.1 (40.68), A1200
  system\kick40063.A600    524,288 bytes  Kickstart 3.1 (40.63), A500/A600
                                          (recommended, see below)

- The first two are required. If either is missing or the wrong size,
  callers see "This door isn't installed properly" with which one, so a test
  call tells you what's wrong.
- Kickstart 1.3 must be the 256 KB file (262,144 bytes). Some copies are the
  same 256 KB twice over (524,288 bytes); WHDLoad refuses those.
- Use Kickstart 3.1, not 3.2, for the A1200: with 3.2 WHDLoad games stop on
  "Please insert a volume containing LIBS/icon.library".
- kick40063.A600: a WHDLoad game (.lha or .hdf) started on the A500 side needs
  Kickstart 2 or later to boot, so the door runs it on an A600 with this ROM.
  Without it such games run on the A1200 instead.

Where to find them: Amiga Forever (amigaforever.com) includes all of these;
its files are named differently (e.g. amiga-os-310-a1200.rom), so copy them in
under the names above. TOSEC Kickstart sets work too, renamed the same way.
The door never uses a ROM it doesn't find under these names.


LIBRARY
-------
The games callers can load are in library\library.txt, one line a game:

  title | one-line description | file1.adf,file2.adf | machine

For example:

  Lotus Turbo Challenge II | Gremlin racer (1991) | Lotus2.adf | A500
  Super Stardust | Team17 shooter, AGA | SuperStardust.lha | A1200
  Alien Breed 2 | Team17 shooter, AGA | AlienBreed2_AGA.hdf | A1200

- The files go in the library\ folder beside library.txt, and are named in
  the line by file name only (no folders).
- The machine is A500 or A1200. Add NTSC after it for an American game that
  needs an NTSC Amiga (e.g. "A500 NTSC"); otherwise the Amiga is PAL, which
  most Amiga games expect. Left out, an .lha or .hdf gets the A1200 and an
  .adf the A500.
- Callers see only the games for the machine they are on, so get the machine
  right: A1200 for AGA games, A500 for older ones. They switch machine with
  F12, A.
- File formats:
    .LHA  a WHDLoad game (the best kind: the Amiga boots it from a hard
          drive at once, loading is quick and there are no disks to swap).
          The usual WHDLoad installs ("Game_v1.2_AGA_0175.lha") work as they
          are; no WHDLoad setup is needed, the door brings its own.
    .HDF  a hard-drive image with a bootable install (as WHDLoad HDF sets
          are): booted the same way.
    .ADF  an 880 KB floppy. A multi-disk game lists every disk, up to 8, in
          order; callers change disks with F12, D. Floppy games load at
          the speed of the game's own loader, so give them a moment.
  Up to 32 MB a file. Not .IPF, .DMS or .ZIP.
- Lines starting with # are comments. Callers see 18 names a page on a
  lightbar (Up/Down, PgUp/PgDn, Home/End, Enter). S searches.
- Games that boot to a Workbench desktop and want an icon double-clicked
  work (the caller has the mouse) but are awkward; whole-screen games suit
  the door best.
- Callers never change your library files: each visit plays on copies.

The zip comes with an empty library: Amiga games are still owned by their
publishers, so add the ones you have the right to offer.


WHAT CALLERS SEE
----------------
A disclaimer about the ROMs, a short how-to, then an Amiga 1200 waiting for a
disk. Everything else is in the F12 menu (Ctrl-Q opens it too):
  L  load a game from the library (lightbar; S searches)
  U  upload their own .ADF, .LHA or .HDF by ZMODEM (SyncTERM: Alt-U); kept
     for their next visit, up to 20 a caller
  M  boot or delete a kept disk
  D  next disk (multi-disk games)
  A  switch machine, A500 <-> A1200 (starts fresh, the disk out)
  N  PAL or NTSC
  K  arrow keys as joystick or as cursor keys
  S  sound on/off
  R  reset (the disk comes out: back to the insert-disk screen)
  Q  back to the BBS

Controls: arrows (or the keypad) are the joystick, Ctrl or Alt (either side)
fires. The Windows keys (or Page Up/Down) are the Amiga keys, Insert is Help.
The caller's own mouse works over the picture. Holding fire while a WHDLoad
game starts opens WHDLoad's own game chooser, so callers should let go of
fire until the game is up.


FOLDERS THE DOOR MAKES
----------------------
players\<handle>-<user number>\        each caller's kept disks (their uploads,
                                       with their saved games)
players\<handle>-<user number>\amiga\  the Amiga's own files for that caller:
                                       WHDLoad's saved games (WHDSaves\)
logs\<handle>-<user number>.log        the last session's numbers, for
                                       debugging
To start a caller over, delete their players\ folder.


TROUBLESHOOTING
---------------
- "This door isn't installed properly": a Kickstart is missing or the wrong
  size (the page says which), or a .dll is missing from beside the .exe.
- A WHDLoad game shows "requires at least a MC68020" or "needs AGA": it is an
  A1200 game listed as A500. Fix its machine in library.txt.
- A floppy game sits at the Kickstart 1.3 insert-disk hand: the disk isn't
  bootable on 1.3 (some later A500 games need Kickstart 2 or more); try it as
  A1200.
- logs\<caller>.log has, every 5 seconds, pictures sent and skipped, KB/s,
  the link speed measured and how long each step took.


CREDITS AND LICENCES
--------------------
Amiga door was written by JSONBourne and originally published at
https://github.com/omniphil/AmigaDoor
The door is free software under the GNU General Public License, version 2 or
(at your option) any later version: licenses\GPL.TXT. Its source is in
source\ and at https://github.com/martykazmaier/amigadoor
This Windows build keeps that licence and that attribution.
PUAE (libretro-uae, from WinUAE and E-UAE) is GPL-2:
https://github.com/libretro/libretro-uae. No Kickstart is included in it (its
built-in fallback, AROS, is a free replacement the door doesn't use). Note
that the WHDLoad helper PUAE builds into the core includes WHDLoad and a few
AmigaOS 3.1 Workbench commands and libraries, as upstream libretro-uae ships
them.
libjxl, brotli and highway: BSD/MIT-style licences, in licenses\libjxl\.
The Microsoft C++ runtime DLLs (msvcp140.dll, vcruntime140.dll) are
redistributed as libjxl's Windows release ships them.
