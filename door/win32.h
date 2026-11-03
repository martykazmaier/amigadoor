/*
 * win32.h -- what the door does differently on Windows (Mystic BBS for Windows), in one place. See win32.c.
 *
 * Mystic for Windows starts a DOOR32 door with DOOR32.SYS saying comm type 2 and, on line 2, the caller's telnet
 * socket, inherited by the door. So the door is the telnet end itself: it talks on that socket with send/recv and does
 * the telnet layer (telnet.c, from the Duke Nukem 3D door). The Amiga runs in a second copy of the door
 * (amigadoor.exe --play), which inherits the same socket and is told its number in AMIGADOOR_CONN. No stdin, no console,
 * no bubblewrap: on Windows the Amiga isn't boxed in.
 *
 * Without a drop file (or with comm type 0) it uses the console instead, to try it out locally.
 *
 * Only included on Windows; also gives the few POSIX calls the shared files use (mkdir with a mode, setenv).
 */

#ifndef AMIGA_WIN32_H
#define AMIGA_WIN32_H

#ifdef _WIN32

#include <direct.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define mkdir(path, mode) _mkdir(path)
#define setenv(name, value, overwrite) win_setenv(name, value)

/* For this process's getenv and for the --play copy it starts */
int win_setenv(const char *name, const char *value);

/* The connection: comm type and handle from DOOR32.SYS (0, 0: the console). Also called by the --play copy, which
 * takes them from AMIGADOOR_CONN. */
bool conn_open(int comm_type, long long handle);
void conn_close(void);

/* Reads what the caller sent, waiting up to timeout_ms (-1: forever). >0 bytes, 0 nothing in time, -1 hung up. */
int conn_read(unsigned char *buf, int max, int timeout_ms);

/* Writes all of it. False when the caller has hung up. */
bool conn_write(const void *data, size_t len);

/* "type,handle" for the --play copy's AMIGADOOR_CONN */
void conn_describe(char *out, size_t size);

/* Milliseconds from a fixed point, to the millisecond (QueryPerformanceCounter, not the 15.6 ms tick) */
long win_now_ms(void);

/* The folder the door's .exe is in, with no trailing separator */
void win_exe_dir(char *out, size_t size);

/* A DLL from the door's own folder only (and the DLLs it needs from there too); NULL if it won't load */
void *win_lib_open(const char *path);
void *win_lib_sym(void *lib, const char *name);

/* Runs the door's own .exe with these arguments (quoted here) and waits. Returns its exit code, or -1 if it couldn't
 * start; a crash comes back as the exception's code, which is never 100-104. */
int win_run_self(char *const argv[]);

/* A fresh private folder under %TEMP% (amigadoor-...) in out; false if none could be made */
bool win_make_session_dir(char *out, size_t size);

/* Deletes a folder and everything in it */
void win_remove_tree(const char *path);

/* If the Amiga side crashes: these bytes go to the caller first (put the terminal back), then it ends. NULL: off. */
void win_crash_guard(const char *reset, size_t len);

#endif /* _WIN32 */
#endif
