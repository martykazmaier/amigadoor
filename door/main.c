/*
 * main.c -- an Amiga as a BBS door: an Amiga 500 or an Amiga 1200, whichever the game wants.
 *
 * The Amiga is PUAE (a libretro core beside the door, core.c), the machine chosen at each start: an A500 with
 * Kickstart 1.3 or an A1200 with Kickstart 3.1, PAL unless a title asks for NTSC. It switches on as soon as the caller
 * has agreed to the ROM disclaimer; everything else is in its F12 menu (play.c): a game from the sysop's library,
 * their own .ADF or WHDLoad .LHA sent up by ZMODEM, the next disk, the machine, and the way back to the BBS. Choosing
 * a game or a machine ends that Amiga session with PLAY_RELOAD and this door starts the next one at once.
 * Written by JSONBourne. Originally published at https://github.com/omniphil/AmigaDoor
 * under the GNU GPL, version 2 or (at your option) any later version (../LICENSE).
 * Built the same way as the C64 door (../C64Door), from which most of this file comes.
 *
 * It plays on terminals that draw JPEG XL through the CTerm APC commands (SyncTERM 1.2+): the Amiga runs here and its
 * picture and sound go out (play.c).
 *
 * Each session gets its own folder in /tmp: the disks it uses are copies, so a game that writes to its disk never
 * changes the library, and the folder is deleted when the door ends. Uploads are kept in players/<handle>-<number>/
 * beside the door instead, used in place, so they and the games saved on them are there next visit; WHDLoad's saved
 * games go to WHDSaves/ in that same folder. The Amiga itself runs in a child process, inside bubblewrap when the box
 * has it: it sees the door's folder read-only, its session folder, and nothing of /home, /tmp or the network.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include "win32.h"
#include "zmodem.h"
#define CORE_FILE "puae_libretro.dll"
#else
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/wait.h>
#define CORE_FILE "puae_libretro.so"
#endif

#include "apc.h"
#include "door.h"
#include "jxl_enc.h"
#include "play.h"
#include "session.h"

#define CSI "\033["

/* ---- where things are ---- */

static char g_door_dir[PATH_MAX];      /* the folder the door binary is in */
static char g_session[PATH_MAX];       /* this caller's own folder in /tmp */
static char g_player[96];              /* their handle and user number, for the log's name */
static char g_mine[PATH_MAX];          /* their kept disks, players/<player>; "" if it couldn't be made */

static void find_door_dir(void)
{
#ifdef _WIN32
    {
        char library[PATH_MAX];
        win_exe_dir(g_door_dir, sizeof(g_door_dir));
        /* The .exe may sit in the project folder, with the library one level down in door/. */
        snprintf(library, sizeof(library), "%s/library/library.txt", g_door_dir);
        if (access(library, R_OK) != 0)
        {
            snprintf(library, sizeof(library), "%s/door/library/library.txt", g_door_dir);
            if (access(library, R_OK) == 0)
            {
                size_t n = strlen(g_door_dir);
                if (n + 5 < sizeof(g_door_dir))
                    memcpy(g_door_dir + n, "/door", 6);
            }
        }
    }
#else
    ssize_t n = readlink("/proc/self/exe", g_door_dir, sizeof(g_door_dir) - 1);
    char *slash;
    if (n <= 0)
    {
        strcpy(g_door_dir, ".");
        return;
    }
    g_door_dir[n] = '\0';
    if ((slash = strrchr(g_door_dir, '/')) != NULL)
        *slash = '\0';
#endif
}

static void door_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, "%s/%s", g_door_dir, name);
}

/* The Kickstart ROMs aren't shipped with the door (they're still under copyright), so the sysop supplies them in
 * system/, under the names PUAE looks for. The core would otherwise fall back to its built-in AROS ROM, which runs
 * almost no games, so the door looks before it starts: what's wrong with the first one missing, or NULL.
 * Kickstart 1.3 has to be the 256 KB image: WHDLoad refuses the 512 KB doubled dumps (spike, 2026-10-28). */
static const char *missing_rom(void)
{
    static const struct { const char *name; long size; const char *problem; } roms[] = {
        { "kick34005.A500", 262144, "system/kick34005.A500 (Kickstart 1.3, 256 KB) is missing or the wrong size" },
        { "kick40068.A1200", 524288, "system/kick40068.A1200 (Kickstart 3.1 A1200, 512 KB) is missing or the wrong size" },
    };
    char path[PATH_MAX];
    struct stat st;

    for (size_t i = 0; i < sizeof(roms) / sizeof(roms[0]); i++)
    {
        snprintf(path, sizeof(path), "%s/system/%s", g_door_dir, roms[i].name);
        if (stat(path, &st) != 0 || st.st_size != roms[i].size)
            return roms[i].problem;
    }
    return NULL;
}

/* The handle cut down to plain characters, then the BBS user number, as the Doom door keeps its saves */
static void make_player_name(void)
{
    size_t n = 0;
    for (const char *p = door_info.handle; *p != '\0' && n < 40; p++)
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')
            g_player[n++] = *p;
    g_player[n] = '\0';
    if (n == 0)
        strcpy(g_player, "player");
    snprintf(g_player + strlen(g_player), sizeof(g_player) - strlen(g_player), "-%d", door_info.user_record);
}

/* ---- the session folder ---- */

#ifdef _WIN32

/* On Windows: under %TEMP%, deleted on the way out (Mystic ends a door that's left running when the caller hangs up,
 * so a folder can be left behind then; it's only the copies of that visit's disks) */
static void session_cleanup(void)
{
    if (g_session[0] != '\0')
        win_remove_tree(g_session);
}

static bool session_create(void)
{
    if (!win_make_session_dir(g_session, sizeof(g_session)))
    {
        g_session[0] = '\0';
        return false;
    }
    atexit(session_cleanup);
    return true;
}

#else

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)flag, (void)ftw;
    return remove(path);
}

static void session_cleanup(void)
{
    if (g_session[0] != '\0')
        nftw(g_session, remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

static void on_signal(int sig)
{
    session_cleanup();
    signal(sig, SIG_DFL);
    raise(sig);
}

static bool session_create(void)
{
    strcpy(g_session, "/tmp/amigadoor-XXXXXX");
    if (mkdtemp(g_session) == NULL)
    {
        g_session[0] = '\0';
        return false;
    }
    atexit(session_cleanup);
    signal(SIGHUP, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    return true;
}

#endif

/* ---- screens ---- */

static void cls(void)
{
    door_write(CSI "0m" CSI "2J" CSI "H");
}

static void title(void)
{
    cls();
    door_write(CSI "1;34m"
               "  ===============================================\r\n"
               CSI "1;37m"
               "                  A M I G A\r\n"
               CSI "1;34m"
               "  ===============================================\r\n" CSI "0m");
    door_write(CSI "1;34m" "              BBS door by JSONBourne\r\n" CSI "0m" "\r\n");
}

static int press_any_key(void)
{
    door_write(CSI "0;37m\r\n  Press any key..." CSI "0m");
    return door_read_char();
}

/* The caller has to agree before the Amiga is switched on: the ROMs are still under copyright */
static bool disclaimer(void)
{
    int c;

    title();
    door_write(CSI "1;33m  Before you switch it on\r\n\r\n" CSI "0;37m");
    door_write("  This door emulates a Commodore Amiga 500 and 1200. It is for use only\r\n"
               "  by people who own an Amiga or a licensed copy of its Kickstart ROMs.\r\n\r\n"
               "  Upload only disk images you have the right to use. The disks you\r\n"
               "  upload are kept for your next visit until you delete them (F12, M).\r\n\r\n");
    door_write(CSI "1;37m  Press Y to agree, or any other key to go back. " CSI "0m");
    c = door_read_char();
    return c == 'y' || c == 'Y';
}

/* ---- the terminal (as the Doom door's main.c) ---- */

typedef struct
{
    bool cterm;
    int  cterm_major, cterm_minor;
    bool jxl, sound, keys, mouse_px;
    int  px_w, px_h;
    int  rows;          /* text rows, from where the cursor stops at 999;999 (0: no answer) */
} terminal_t;

static void detect_cterm(terminal_t *term)
{
    char reply[64];
    int n = 0, c;
    const char *p;

    door_write(CSI "c");
    c = door_read_char_timeout(500);
    while (c >= 0 && n < (int)sizeof(reply) - 1)
    {
        reply[n++] = (char)c;
        if (c == 'c' && n > 2)
            break;
        c = door_read_char_timeout(200);
    }
    reply[n] = '\0';
    p = strstr(reply, "[=67;84;101;114;109;");
    if (p != NULL && sscanf(p + strlen("[=67;84;101;114;109;"), "%d;%d", &term->cterm_major, &term->cterm_minor) == 2)
        term->cterm = true;
}

static bool cterm_at_least(const terminal_t *term, int major, int minor)
{
    return term->cterm && (term->cterm_major > major || (term->cterm_major == major && term->cterm_minor >= minor));
}

/*
 *   Q;JXL                          ESC [ = 1 ; 1 - n       JPEG XL pictures
 *   Q;libsndfile                   ESC [ = 7 ; 100 ; 1 n   sound files
 *   Q;libsndfileFormat;1;5         ESC [ = 7 ; 101 ; 1 ; 5 ; 1 n      WAV, 8-bit
 *   CSI < 0 c                      ESC [ < 0 ; ... c       its features: 8 = key presses and releases
 *   CSI ? 2 ; 1 S                  ESC [ ? 2 ; 0 ; w ; h S the screen in pixels
 */
static void detect_pixels(terminal_t *term)
{
    char reply[512];
    int n = 0, c;
    const char *p;

    if (!term->cterm)
        return;
    door_write(APC_PREFIX "Q;JXL" APC_END APC_PREFIX "Q;libsndfile" APC_END
               APC_PREFIX "Q;libsndfileFormat;1;5" APC_END CSI "<0c" CSI "?2;1S"
               CSI "s" CSI "999;999H" CSI "6n" CSI "u" CSI "5n");
    c = door_read_char_timeout(1500);
    while (c >= 0 && n < (int)sizeof(reply) - 1)
    {
        reply[n++] = (char)c;
        reply[n] = '\0';
        if (strstr(reply, "\033[0n") != NULL)
            break;
        c = door_read_char_timeout(500);
    }
    reply[n] = '\0';

    term->jxl = strstr(reply, "\033[=1;1-n") != NULL;
    term->sound = strstr(reply, "\033[=7;100;1n") != NULL && strstr(reply, "\033[=7;101;1;5;1n") != NULL;
    p = strstr(reply, "\033[<0");
    if (p != NULL)
        for (p += 4; *p == ';'; )
        {
            if (atoi(++p) == 8)
                term->keys = true;
            while (*p >= '0' && *p <= '9')
                p++;
        }
    term->mouse_px = cterm_at_least(term, 1, 330);    /* SGR pixel mouse reports, CSI ? 1016 h (cterm.adoc) */
    p = strstr(reply, "\033[?2;0;");
    if (p == NULL || sscanf(p + 7, "%d;%d", &term->px_w, &term->px_h) != 2)
        term->px_w = 640, term->px_h = 400;
    /* The cursor position report from the bottom-right corner: the screen's rows */
    for (p = reply; (p = strstr(p, "\033[")) != NULL; p += 2)
    {
        int r, c;
        char end;
        if (sscanf(p, "\033[%d;%d%c", &r, &c, &end) == 3 && end == 'R' && r > 0 && r < 300)
        {
            term->rows = r;
            break;
        }
    }
}

/* ---- the Amiga session, in a child process ---- */

#ifndef _WIN32
static bool bwrap_works(void)
{
    static int known = -1;
    if (known < 0)
        known = system("bwrap --ro-bind / / --unshare-all --die-with-parent true >/dev/null 2>&1") == 0;
    return known == 1;
}
#endif

static play_result_t run_amiga(const terminal_t *term)
{
    char self[PATH_MAX], core[PATH_MAX], system_dir[PATH_MAX], log[PATH_MAX], logs[PATH_MAX];
    char caps[64], deadline[32], link[32];
    char *argv[64];
    int argc = 0, kbps = 0, ping = 0;
    FILE *f;

    snprintf(self, sizeof(self), "%s/amigadoor", g_door_dir);
    door_path(core, sizeof(core), CORE_FILE);
    door_path(system_dir, sizeof(system_dir), "system");
    door_path(logs, sizeof(logs), "logs");
    mkdir(logs, 0755);
    snprintf(log, sizeof(log), "%s/%s.log", logs, g_player);

    /* A modelled slower link for one player: logs/<player>.linktest holding "kbps=300 ping=80" */
    {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.linktest", logs, g_player);
        if ((f = fopen(path, "r")) != NULL)
        {
            if (fscanf(f, "kbps=%d ping=%d", &kbps, &ping) < 1)
                kbps = 0;
            fclose(f);
        }
    }

    snprintf(caps, sizeof(caps), "%d,%d,%d,%d,%d,%d,%d,%d", cterm_at_least(term, 1, 329), cterm_at_least(term, 1, 332),
             term->keys, term->sound, term->px_w, term->px_h, term->rows, term->mouse_px);
    snprintf(deadline, sizeof(deadline), "%ld", (long)(time(NULL) + door_time_remaining() * 60L));
    snprintf(link, sizeof(link), "%d,%d", kbps, ping);
    setenv("AMIGADOOR_SAVE_DIR", g_session, 1);         /* the core's own scratch space: writable, and private */
#ifdef _WIN32
    {
        /* No symlinks for the WHDSaves link on Windows (main() makes it on Linux): the core's save folder is the
         * caller's own instead, players/<player>/amiga, so WHDLoad's saves (its WHDSaves/ there) last. The core
         * unpacks its WHDLoad helper there too, once. */
        static char saves[PATH_MAX];
        if (g_mine[0] != '\0')
        {
            snprintf(saves, sizeof(saves), "%s/amiga", g_mine);
            mkdir(saves, 0700);
            setenv("AMIGADOOR_SAVE_DIR", saves, 1);
        }
    }
#endif
    setenv("AMIGADOOR_MINE", g_mine, 1);

#ifdef _WIN32
    /* The Amiga side is this .exe again; it inherits the caller's socket and is told which one it is */
    {
        char conn[64];
        int code;
        conn_describe(conn, sizeof(conn));
        setenv("AMIGADOOR_CONN", conn, 1);
        argv[argc++] = "--play";
        argv[argc++] = core;
        argv[argc++] = system_dir;
        argv[argc++] = g_session;
        argv[argc++] = caps;
        argv[argc++] = deadline;
        argv[argc++] = link;
        argv[argc++] = log;
        argv[argc] = NULL;
        (void)self;
        code = win_run_self(argv);
        if (code >= 100 && code <= 100 + PLAY_RELOAD)
            return (play_result_t)(code - 100);
        return PLAY_FAILED;                             /* it couldn't start, or crashed (the Amiga side put the
                                                         * terminal back first) */
    }
#else
    if (bwrap_works())
    {
        const char *box[] = { "bwrap", "--ro-bind", "/", "/", "--tmpfs", "/home", "--tmpfs", "/tmp", "--tmpfs", "/var/tmp",
                              "--tmpfs", "/run", "--dev", "/dev", "--ro-bind", g_door_dir, g_door_dir, "--bind", logs, logs,
                              "--bind", g_session, g_session, "--proc", "/proc", "--unshare-all", "--die-with-parent",
                              "--cap-drop", "ALL", "--" };
        for (size_t i = 0; i < sizeof(box) / sizeof(box[0]) - 1; i++)
            argv[argc++] = (char *)box[i];
        if (g_mine[0] != '\0')
        {
            argv[argc++] = "--bind";                    /* the kept disks: writable, inside the read-only door folder */
            argv[argc++] = g_mine;
            argv[argc++] = g_mine;
        }
        argv[argc++] = "--";
    }
    argv[argc++] = self;
    argv[argc++] = "--play";
    argv[argc++] = core;
    argv[argc++] = system_dir;
    argv[argc++] = g_session;
    argv[argc++] = caps;
    argv[argc++] = deadline;
    argv[argc++] = link;
    argv[argc++] = log;
    argv[argc] = NULL;

    {
        pid_t pid;
        int status;
        if ((pid = fork()) == 0)
        {
            execvp(argv[0], argv);
            _exit(100 + PLAY_FAILED);
        }
        if (pid < 0 || waitpid(pid, &status, 0) < 0)
            return PLAY_FAILED;
        if (WIFEXITED(status) && WEXITSTATUS(status) >= 100 && WEXITSTATUS(status) <= 100 + PLAY_RELOAD)
            return (play_result_t)(WEXITSTATUS(status) - 100);
        /* Killed by a signal (a crash: the Amiga side put the terminal back first) */
        return PLAY_FAILED;
    }
#endif
}

/* The child: amigadoor --play core system session blob,zoom,keys,sound,w,h,rows,mousepx deadline kbps,ping log */
static int play_main(int argc, char **argv)
{
    play_caps_t caps = { 0 };
    play_setup_t setup = { 0 };
    drive_t d;
    char image[600], door_dir[PATH_MAX], *slash;
    int b, z, k, s, mp = 0;

    if (argc < 9 || sscanf(argv[5], "%d,%d,%d,%d,%d,%d,%d,%d", &b, &z, &k, &s, &caps.px_w, &caps.px_h, &caps.rows,
                           &mp) < 6)
        return 100 + PLAY_FAILED;
    caps.blob = b, caps.zoom = z, caps.keys = k, caps.sound = s, caps.mouse_px = mp;
#ifdef _WIN32
    {
        int type = 0;
        long long handle = 0;
        if (getenv("AMIGADOOR_CONN") != NULL)
            sscanf(getenv("AMIGADOOR_CONN"), "%d,%lld", &type, &handle);
        if (!conn_open(type, handle))
            return 100 + PLAY_FAILED;
    }
#endif
    snprintf(door_dir, sizeof(door_dir), "%s", argv[2]);
    if ((slash = strrchr(door_dir, '/')) != NULL)
        *slash = '\0';
    session_set_dirs(argv[4], door_dir);
    session_set_mine(getenv("AMIGADOOR_MINE"));

    /* What to boot with: drive.txt, which the session menu rewrites when a disk is chosen */
    drive_load(&d);
    setup.core = argv[2];
    setup.system = argv[3];
    setup.session = argv[4];
    setup.image = drive_image(&d, image, sizeof(image)) ? image : NULL;
    setup.a1200 = d.a1200;
    setup.ntsc = d.ntsc;
    setup.deadline = (time_t)atol(argv[6]);
    sscanf(argv[7], "%d,%d", &setup.link_kbps, &setup.link_ping);
    setup.log = argv[8];
#ifdef _WIN32
    /* The core's chatter: nowhere (the caller's connection is the socket, never stdout, but a console could show it) */
    freopen("NUL", "w", stdout);
    freopen("NUL", "w", stderr);
#else
    {
        /* The core writes chatter to stdout and stderr, which are the caller's connection: the connection moves to
         * a descriptor of its own, and stdout and stderr go nowhere */
        char spill[PATH_MAX];
        int nul = open("/dev/null", O_WRONLY);
        if (nul < 0)
        {
            snprintf(spill, sizeof(spill), "%s/core-output.txt", argv[4]);     /* deleted with the session */
            nul = open(spill, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        }
        fflush(stdout);
        setup.conn_fd = dup(STDOUT_FILENO);
        if (setup.conn_fd < 0 || nul < 0)
            return 100 + PLAY_FAILED;
        dup2(nul, STDOUT_FILENO);
        dup2(nul, STDERR_FILENO);
        close(nul);
    }
#endif
    return 100 + (int)play(&caps, &setup);
}

/* ---- the door ---- */

static void how_to_play(const terminal_t *term)
{
    title();
    door_write(CSI "1;37m  How it works\r\n\r\n" CSI "0;37m");
    door_write("  The Amiga 1200 switches on waiting for a disk. Press " CSI "1;37mF12" CSI "0;37m (or " CSI "1;37mCtrl-Q"
               CSI "0;37m)\r\n  any time for the menu: load a game from the library, upload your own\r\n"
               "  (kept for next time), change disks, switch between the A500 and A1200,\r\n  or leave.\r\n\r\n");
    if (term->keys)
        door_write("  Joystick: arrows (or the keypad) move, Ctrl or Alt (either side) fires.\r\n"
                   "  The Windows keys (or Page Up/Down) are the Amiga keys, Insert is Help.\r\n");
    else
        door_write("  Joystick: arrows move, Space fires (your terminal can't report held\r\n"
                   "  keys, so fast games will be hard).\r\n");
    door_write("  Mouse: your own, over the picture; the left and right buttons click.\r\n");
    if (!term->sound)
        door_write(CSI "1;33m  Your terminal doesn't play sound files, so the Amiga is silent.\r\n" CSI "0;37m");
    door_write(CSI "1;37m\r\n  Press any key to switch on. " CSI "0m");
}

static void goodbye(void)
{
    title();
    door_write(CSI "0;37m  Thanks for visiting. Your uploaded disks and WHDLoad saves are kept\r\n"
               "  for next time.\r\n\r\n");
    door_write("  The Amiga here is PUAE (libretro-uae, from WinUAE and E-UAE), free\r\n"
               "  software under the GNU GPL v2: https://github.com/libretro/libretro-uae\r\n");
    press_any_key();
}

int main(int argc, char **argv)
{
    terminal_t term = { 0 };
    char core[PATH_MAX];
    drive_t empty = { .a1200 = true };
    play_result_t r;

    if (argc > 1 && strcmp(argv[1], "--play") == 0)
        return play_main(argc, argv);

    find_door_dir();
    door_init(argc > 1 ? argv[1] : NULL);
    make_player_name();

    title();
    door_path(core, sizeof(core), CORE_FILE);
    if (access(core, R_OK) != 0 || !jxl_enc_available() || !session_create())
    {
        door_write(CSI "1;33m  This door isn't installed properly. Please tell the sysop.\r\n" CSI "0m");
        press_any_key();
        door_cleanup();
        return 1;
    }
    if (missing_rom() != NULL)
    {
        char line[200];
        snprintf(line, sizeof(line), CSI "1;33m  This door isn't installed properly. Please tell the sysop.\r\n\r\n"
                 CSI "0;37m  (%s)\r\n" CSI "0m", missing_rom());
        door_write(line);
        press_any_key();
        door_cleanup();
        return 1;
    }
    session_set_dirs(g_session, g_door_dir);
    drive_save(&empty);
    {
        char players[PATH_MAX];
        door_path(players, sizeof(players), "players");
        mkdir(players, 0700);
        snprintf(g_mine, sizeof(g_mine), "%s/%s", players, g_player);
        if (mkdir(g_mine, 0700) != 0 && errno != EEXIST)
            g_mine[0] = '\0';                           /* uploads then last this visit only */
#ifndef _WIN32
        else
        {
            /* WHDLoad saves its games in WHDSaves/ in the core's save folder, which is the session's: linked to the
             * caller's own folder, they last. Inside the sandbox both paths are the same, so the link holds there. */
            char saves[PATH_MAX], link_path[PATH_MAX];
            snprintf(saves, sizeof(saves), "%s/WHDSaves", g_mine);
            snprintf(link_path, sizeof(link_path), "%s/WHDSaves", g_session);
            mkdir(saves, 0700);
            if (symlink(saves, link_path) != 0)
                mkdir(link_path, 0700);
        }
#endif
    }

    detect_cterm(&term);
    detect_pixels(&term);
    if (!term.jxl)
    {
        title();
        door_write(CSI "1;37m  This door needs a terminal that draws pictures\r\n\r\n" CSI "0;37m");
        door_write("  The Amiga's screen is sent as JPEG XL pictures, which SyncTERM 1.2 or\r\n"
                   "  newer draws. Call back with SyncTERM to play.\r\n");
        press_any_key();
        door_cleanup();
        return 0;
    }
    if (!disclaimer())
    {
        door_cleanup();
        return 0;
    }
    how_to_play(&term);
    if (door_read_char() < 0)
    {
        door_cleanup();
        return 0;
    }

    /* The Amiga, again each time the caller picks new disks or another machine, until they leave */
    while ((r = run_amiga(&term)) == PLAY_RELOAD)
        ;
    if (r == PLAY_HANGUP)
    {
        door_cleanup();
        return 0;
    }
    if (r == PLAY_FAILED)
    {
        title();
        door_write(CSI "1;33m  The Amiga couldn't start, or it crashed on that disk.\r\n" CSI "0;37m"
                   "  If it keeps happening, please tell the sysop.\r\n");
        press_any_key();
    }
    if (r == PLAY_TIMEOUT)
    {
        title();
        door_write(CSI "1;33m  Your time on the BBS is up.\r\n" CSI "0m");
        press_any_key();
    }
    else
    {
        goodbye();
    }
    door_cleanup();
    return 0;
}
