/*
 * play.h -- an Amiga session on a CTerm terminal (SyncTERM): the Amiga runs here and its picture goes out as JPEG XL,
 * its sound as short WAV pieces. See play.c.
 */

#ifndef PLAY_H
#define PLAY_H

#include <stdbool.h>
#include <time.h>

typedef struct
{
    bool blob;              /* pictures can travel inside the command (CTerm 1.329+), not only through the cache */
    bool zoom;              /* the terminal scales pictures up itself (ZX/ZY, CTerm 1.332+) */
    bool keys;              /* the terminal reports key presses and releases (CSI = 1 h) */
    bool sound;             /* it plays WAV files */
    bool mouse_px;          /* it reports the mouse in pixels (CSI ? 1016 h, CTerm 1.330+), not only in text cells */
    int  px_w, px_h;        /* the screen in pixels */
    int  rows;              /* the screen in text rows (0: not known); the last one holds the controls */
} play_caps_t;

typedef struct
{
    const char *core;       /* puae_libretro.so */
    const char *system;     /* the core's system folder: the Kickstart ROMs */
    const char *image;      /* the session's disk (or an .m3u of them, the first in DF0:); NULL boots with none */
    bool a1200, ntsc;       /* the machine */
    const char *session;    /* the caller's session folder (drive.txt, uploads) */
    const char *log;        /* where the session's numbers go, or NULL */
    time_t deadline;        /* when the caller's time on the BBS runs out */
    int link_kbps, link_ping;   /* a modelled slower link for testing (0: none) */
    int conn_fd;            /* the caller's connection for writing (0: stdout) */
} play_setup_t;

typedef enum
{
    PLAY_QUIT,              /* the caller chose to leave the Amiga */
    PLAY_HANGUP,            /* the connection went */
    PLAY_TIMEOUT,           /* their time ran out */
    PLAY_FAILED,            /* the Amiga couldn't start */
    PLAY_RELOAD             /* new disks or another machine were chosen: start the Amiga again with drive.txt */
} play_result_t;

play_result_t play(const play_caps_t *caps, const play_setup_t *setup);

#endif
