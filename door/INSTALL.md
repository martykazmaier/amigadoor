# Installing the Amiga door on the BBS

## What the door folder holds

| File | What | Where it comes from |
| --- | --- | --- |
| `amigadoor` | the door | `make` here, on the BBS box |
| `puae_libretro.so` | the Amiga (PUAE, GPL-2), with no Kickstart inside | `sh build-core.sh` here (clones libretro/libretro-uae at a pinned commit and builds it) |
| `system/kick34005.A500`, `system/kick40068.A1200` | the Kickstart ROMs | **the sysop's own** (below); never in the repo |
| `library/library.txt` + disk images | the library | the sysop: one line a title, `title \| description \| disk1.adf,disk2.adf \| A500` |
| `logs/` | one log per player, rewritten each session | made by the door |
| `players/<handle>-<number>/` | each caller's kept disks (their uploads, saves and all; up to 20) and `WHDSaves/`, WHDLoad's saved games | made by the door |
| `test_door.py` | pretends to be SyncTERM | here |

## The ROMs

The Amiga's Kickstart ROMs are still under copyright, so they aren't in this repo. Put your own in `system/` under
the names PUAE uses:

```
system/kick34005.A500     256 KB  Kickstart 1.3 (34.5), for the Amiga 500
system/kick40068.A1200    512 KB  Kickstart 3.1 (40.68), for the Amiga 1200
```

- **Kickstart 1.3 must be the 256 KB image.** Some dumps are the same 256 KB twice (512 KB); WHDLoad refuses those.
  The door checks both sizes and won't start without them (it names what's wrong).
- **Not Kickstart 3.2** for the A1200: with it every WHDLoad game stopped on "Please insert a volume containing
  LIBS/icon.library". 3.1 boots them straight away.
- `kick40063.A600` (Kickstart 3.1 for the A500/A600/A2000) should go there too, as `system/kick40063.A600` (512 KB).
  A WHDLoad game or `.hdf` started on the A500 side runs on an A600 with it: WHDLoad's boot needs Kickstart 2 or later, and on
  1.3 stops at "Unknown command FAILAT". Without it such a game runs on the A1200 instead.
- PUAE also has a free built-in replacement, AROS, which runs almost no games; the door never uses it.

Amiga Forever's and TOSEC's sets have these under other names: copy them in under the names above.

The box needs **libjxl 0.11 or newer**, **lrzsz** (`rz`/`sz`, for uploads and downloads) and, for the sandbox,
**bubblewrap** (`bwrap`). Building needs **git, gcc and make** (on Ubuntu:
`sudo apt install git build-essential libjxl0.11 lrzsz bubblewrap`). libjxl's headers come with the door
(`jxl_include/`) and the library is loaded at run time, so no -dev package is needed.

## Steps

1. Copy the door folder to the box and build there: `make clean && make`, then `sh build-core.sh` for the Amiga.
   Put your ROMs in `system/` (above).
2. Put games in `library/` and list them in `library/library.txt`. WHDLoad `.lha` files are best: PUAE boots them
   straight from the archive as a hard drive, with no disks to swap and loading in moments. `.hdf` hard-drive images
   (a bootable WHDLoad install, as the "amigaromset" ones are) boot the same way. `.adf` floppies work too, several to
   a title for a multi-disk game (the caller swaps with F12, D). The last field says the machine: `A500` or `A1200`,
   `NTSC` after it for an American title; left out, an `.lha` or `.hdf` gets an A1200 and an `.adf` an A500. Callers
   see only the titles for the machine they're on (F12, A switches), so get the field right.
3. Test it in a **staging copy**, never in the live folder (the test writes `logs/`):
   `python3 test_door.py` (library title 1, 20 s), `python3 test_door.py --title 3 --seconds 60 --swap-at 30`
   (a multi-disk title, disk 2 in through F12 at 30 s), `python3 test_door.py --machine-at 15` (switching machine).
4. Add a door entry to your BBS that writes a **door32.sys** drop file and runs `amigadoor` with the node's drop file
   folder (or the door32.sys file itself) as its one argument. The door talks to the caller on stdin/stdout, so the
   BBS must hand it the connection that way (Mystic does on Linux).
   On Mystic that is a menu command **D3** with the data `<door folder>/amigadoor <mystic>/temp%3`.

## Raspberry Pi and other ARM boxes (untested)

Nothing in the door or the build is x86-specific (no assembly, no SIMD, no `-march`), and PUAE builds on ARM, so the
steps above should work as they are on a Pi running 64-bit Linux: `make`, then `sh build-core.sh`. Not tried yet;
two things to watch:

- **libjxl 0.11 or newer.** Raspberry Pi OS based on Debian Bookworm ships libjxl 0.7, which is too old: use the
  Trixie-based release (it has 0.11) or build libjxl from source (https://github.com/libjxl/libjxl).
- **Speed.** Every caller runs a whole Amiga at full speed plus a JPEG XL encoder. A Pi 5 should manage a caller or
  two; a Pi 4 may struggle with A1200 (AGA) games. Watch `logs/` for skipped frames and limit the door's nodes to suit.

## What callers see

The disclaimer, a short how-to, then an Amiga 1200 waiting for a disk. Everything else is in the **F12** menu
(**Ctrl-Q** opens it too, for keyboards whose F12 needs Fn): load a library title (a CP437 lightbar, 18 names a page, **S** searches;
the Amiga restarts with it, on the machine the title asks for), upload their own `.adf` or WHDLoad `.lha` by ZMODEM
(kept for their next visit), boot or
delete a kept disk (**M**), next disk, **A** the other machine (A500 or A1200; it starts fresh, the disk out), **N** PAL or NTSC
(restarts it), arrows as joystick or cursor keys, sound, reset (the disk comes out: back to the Kickstart's
insert-disk screen on the same machine), and **Q** back to the BBS. Nothing is offered for download on the
way out (sysop's call, 2026-10-28): a library floppy a game wrote to is a copy for that visit only. WHDLoad games keep
their saves in the caller's `WHDSaves/`, and the caller's own uploaded disks are kept, saves and all.

Keys: arrows (or the keypad) are the joystick, Ctrl or Alt (left or right) fires. The Windows keys (or Page Up/Down) are the
left and right Amiga keys, Insert is Help. The mouse is the caller's own, over the picture: SyncTERM 1.330 and newer
report it to the pixel, older ones to the text cell (coarse, but clicking works). Holding fire while a WHDLoad game
boots opens WHDLoad's own chooser (PUAE's boot script does that on purpose), so callers should let go until the game
starts.

## Pictures and the link

The Amiga always runs its own 50 frames a second (60 on NTSC); the pictures follow the link, as on the C64 door. When
frames are being skipped two seconds running, the door sends them **lossy** (JPEG XL distance 3, about half the
bytes on AGA art) for 10 s, then tries lossless again (staying lossy longer each time it has to come back). A still
picture that went out lossy is sent once more, sharp. `AMIGADOOR_LOSSY_DISTANCE=0` keeps every picture lossless;
another number sets the distance.

The frame is 360x287 for a low-resolution screen and 720x287 for a high-resolution one (Workbench, the Kickstart
screen, WHDLoad's messages, and some games: Super Stardust's play screen is one), borders in. It is stretched or
shrunk to fill the screen above the controls row, keeping the Amiga's shape: 514x384 on SyncTERM's usual 80x25. A
high-resolution screen shrunk is averaged, so its text stays readable. `AMIGADOOR_SMALL=1` sends low-resolution
screens at 1x instead (smaller on screen, cheaper). High-resolution screens cost more: Super Stardust's play went
from 500-770 KB/s to 700-1150.

Making a big picture can take longer than a frame lasts (up to 50 ms against 20). When the Amiga is behind, the door
leaves that frame's picture out rather than let the Amiga (and its sound) fall behind; the log counts these.

Measured with `test_door.py` on the box, Super Stardust (AGA, busy):

| Link (modelled) | Pictures a second | KB/s sent |
| --- | --- | --- |
| no limit | 47-50, lossless | 500-770 |
| 3000 KB/s, 80 ms ping | 47, lossless | 440-670 |
| 300 KB/s, 80 ms ping | 31-41, lossy | 200-280 |
| 150 KB/s, 120 ms ping | 9-14, lossy | 115-155 |

On the slowest link the door still rates the link too high (it measured 200-300 KB/s on the 150 KB/s one) and its
round trip creeps up to a quarter of a second; worth a look before callers on slow links use it.

## Per-player link test

`logs/<player>.linktest` holding `kbps=300 ping=80` makes that player's sessions go through a modelled link of that
speed (KB/s) and round trip. Delete the file to go back.

## Debugging

- `logs/<player>.log`: what the terminal reported, then every 5 s the frames run and sent (and how many lossy), KB/s,
  the link, sound pieces.
- `AMIGADOOR_DUMP=<dir>`: a snapshot of the Amiga's screen every 5 s, as PPM.
- `AMIGADOOR_CORE_LOG=<file>`: the core's own warnings.
- `AMIGADOOR_MOUSE_SCALE=<n>`: Amiga mouse counts a picture pixel. 2 (the default) makes the Amiga's pointer keep
  pace with the caller's, as measured with an Intuition pointer; a game that reads the mouse itself may want another.
- `AMIGADOOR_DUMP_EVERY=<frames>`: how often `AMIGADOOR_DUMP` takes a snapshot (250, every 5 s).
