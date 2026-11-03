/*
 * zmodem.h -- ZMODEM uploads and downloads, for the Windows door (on Linux the door runs lrzsz's rz and sz instead,
 * which Windows doesn't have). See zmodem.c.
 */

#ifndef ZMODEM_H
#define ZMODEM_H

#include <stdbool.h>
#include <stddef.h>

/* The caller's connection: the door's own reading and writing (telnet already taken care of) */
typedef struct
{
    int  (*read)(unsigned char *buf, int max, int timeout_ms);     /* >0 bytes, 0 nothing in time, -1 hung up */
    bool (*write)(const void *data, size_t len);                    /* false: hung up */
} zm_io_t;

#define ZM_HUNG_UP (-1)

/* Receives files into dir, as `rz -b -E -R` does: plain names only (no folders), a name already there gets a number,
 * nothing bigger than max_size and no more than max_files (the rest are skipped). Waits up to a couple of minutes for
 * the caller to start. Returns how many files arrived whole (a broken one is deleted), or ZM_HUNG_UP. */
int zm_receive(const zm_io_t *io, const char *dir, long max_size, int max_files);

/* Sends one file under its own name (the part of path after the last / or \), as `sz -b` does. Returns 1 if it
 * arrived, 0 if not (refused, cancelled, timed out), or ZM_HUNG_UP. */
int zm_send(const zm_io_t *io, const char *path);

#endif
