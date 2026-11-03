/*
 * session.h -- a caller's disks and the sysop's library, shared by the door (outside the sandbox) and the Amiga session
 * (inside it). Everything lives as files in the session folder, so both sides see the same thing:
 *
 *   drive.txt    what the Amiga boots with: "model A500|A1200", "standard PAL|NTSC", then one "path|name" a line, the
 *                first in DF0: (or a WHDLoad .lha or a hard drive .hdf, which the core boots as a hard drive)
 *   ledger.txt   every disk ever added this visit: "path|name|md5 as added", for the "download the changed disk?"
 *                offer on the way out (a disk can leave the drive and still have a saved game on it)
 *
 * The caller's kept disks are in their own folder beside the door, players/<handle>-<user number>/, bound writable into
 * the sandbox. Uploads go there and the drive uses them in place, so a game's saves are kept too; so do WHDLoad's saved
 * games, in its WHDSaves/ folder there.
 *
 * See session.c.
 */

#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>
#include <stddef.h>

#define SESSION_MAX_DISKS 8
#define IMAGE_MAX         (32 * 1024 * 1024)    /* the biggest image taken: WHDLoad .lha files run to several MB */
#define MINE_MAX          20    /* kept disks a caller may have */

typedef struct
{
    char path[512];             /* the copy in the session folder */
    char name[64];              /* what the caller sees */
    char md5[33];               /* as it went in */
} disk_t;

typedef struct
{
    bool   a1200;               /* the machine: an A1200 (Kickstart 3.1), otherwise an A500 (Kickstart 1.3) */
    bool   ntsc;                /* an NTSC machine, otherwise PAL (most Amiga games are European) */
    int    count;
    disk_t disk[SESSION_MAX_DISKS];
} drive_t;

typedef struct
{
    char title[48];
    char about[72];
    char files[256];            /* comma-separated, in library/ */
    char machine[32];           /* "A500", "A1200", either with " NTSC"; "" goes by the first file (.lha, .hdf: A1200) */
} title_t;

/* The session folder and the door's own folder, for every call below */
void session_set_dirs(const char *session_dir, const char *door_dir);

/* What the Amiga boots with */
void drive_load(drive_t *d);
bool drive_save(const drive_t *d);

/* The caller's kept disks: their folder, "" for none (then uploads last this visit only) */
void session_set_mine(const char *dir);
bool mine_available(void);

/* The kept disks' file names, sorted. Returns how many (up to max). */
int mine_list(char names[][64], int max);

/* An upload into the kept disks (replacing one of the same name), and into the drive list as it is. False with a
 * reason, "full" included. */
bool mine_add(drive_t *d, const char *source, const char *display, const char **why);

/* Kept disk number index into DF0:, the ones after it next in the list (for D). False if it's gone. */
bool mine_use(int index, drive_t *d);

/* Deletes a kept disk by its file name */
bool mine_delete(const char *name);

/* Adds a disk image to the drive list as a copy in the session folder, and to the ledger. False with a reason. */
bool disk_add(drive_t *d, const char *source, const char *display, const char **why);

/* Every disk added this visit, for the way out. Returns how many (up to max). */
int ledger_load(disk_t *out, int max);

/* What the core starts with: the one image itself, or an .m3u of the drive list. False if the list is empty. */
bool drive_image(const drive_t *d, char *path, size_t size);

/* Whether a path is a hard drive for the core (a WHDLoad .lha or an .hdf) rather than a floppy */
bool path_is_hd(const char *path);

/* Whether a library title is for the A1200 (otherwise the A500): its machine field, or by its first file */
bool library_a1200(const title_t *t);

/* The machine a disk suits when nothing says otherwise: A1200 for a hard drive (.lha, .hdf), A500 for a floppy */
void drive_guess_machine(drive_t *d);

/* The library: library/library.txt beside the door, "title | description | disk1.adf,disk2.adf | A500" a line
 * (the machine optional, "A1200 NTSC" for an American title) */
int library_load(title_t *titles, int max);

/* A library title's disks and machine, replacing the drive list. False with a reason. */
bool library_use(const title_t *t, drive_t *d, const char **why);

/* MD5 of a file, "" if unreadable */
void file_md5(const char *path, char md5[33]);

#endif
