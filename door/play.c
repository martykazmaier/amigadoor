/*
 * play.c -- an Amiga session on a CTerm terminal (SyncTERM): the Amiga runs here, the caller sees and hears it.
 *
 * The C64 door's play.c (../C64Door/door), with the Amiga's own needs added. Measured first in ../spike (2026-10-28):
 *
 *   Pictures   Only what changed is sent: 32x8 tiles, the whole picture when more than half changed, otherwise each
 *              run of changed tiles merged downwards into rectangles. LOSSLESS while the link keeps up; when it doesn't
 *              (frames are being skipped) LOSSY, JPEG XL distance 3, which on AGA art is half the size (spike: Super
 *              Stardust 248 KB/s lossless at 50 fps, 110 at distance 3). A blurred picture that has gone still is sent
 *              again lossless, and lossy is tried away from again after a while.
 *              The frame is whatever size the core sends (360x287 on PAL, borders in); a new size is laid out afresh.
 *              It is shown as large as the screen above the controls row allows: the terminal scales it by a whole
 *              number when 2x or more fits, otherwise it is stretched here, keeping the Amiga's shape.
 *   Pacing     As the C64 door: a cursor-position request after each frame, as many frames on their way as the link's
 *              speed times its round trip allows. A frame that finds no room is skipped, never queued; the Amiga itself
 *              always runs at its own 50 (PAL) or 60 (NTSC) frames a second.
 *   Sound      Paula's four channels as the core mixes them, in 200 ms pieces of 8-bit mono WAV at 22050 Hz, each
 *              cached, loaded into a slot and queued on one channel behind the last (as the C64 door).
 *   Keys       Real presses and releases (CSI = 1 h) where the terminal reports them, mapped to the Amiga's keyboard
 *              (the Windows keys and Page Up/Down are the Amiga keys, Insert is Help), with the arrows and fire keys as
 *              the joystick while joystick mode is on. Otherwise typed characters are turned into key taps.
 *   Mouse      xterm reports (CSI ? 1003 h, any movement; SGR form, CSI ? 1006 h), in pixels where the terminal can
 *              (CSI ? 1016 h, CTerm 1.330+), otherwise in text cells. The Amiga's mouse only moves, it has no
 *              position, so the caller's movement over the picture becomes the Amiga's in its own pixels. When the
 *              caller's pointer goes past the picture's edge the Amiga's is pushed against that edge too, which brings
 *              the two back together after any drift.
 *   F12        (or Ctrl-Q, for keyboards whose F12 needs Fn) pauses the Amiga and shows the session menu: load a
 *              title from the library (a lightbar; S searches the names), upload one by ZMODEM (kept for the caller's next visit),
 *              their kept disks, next disk, the machine (A500 or A1200) and PAL or NTSC, sound, reset, back to the BBS.
 *              Loading a title or changing the machine ends this session with PLAY_RELOAD, and the door starts the
 *              Amiga again.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include "win32.h"
#include "zmodem.h"
#else
#include <sys/select.h>
#endif

#include "apc.h"
#include "core.h"
#include "jxl_enc.h"
#include "libretro.h"
#include "play.h"
#include "session.h"

#include <dirent.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#define TW 32
#define TH 8
#define MAX_TX ((CORE_MAX_W + TW - 1) / TW)
#define MAX_TY ((CORE_MAX_H + TH - 1) / TH)

#define MAX_IN_FLIGHT    16
#define MIN_WINDOW       4
#define QUEUE_ALLOWED_MS 50
#define STALL_MS         3000
#define RATE_MS          250
#define FULL_FRACTION    0.5

#define SND_RATE         22050
#define SND_PIECE        (SND_RATE / 5)         /* 200 ms */
#define SND_PREBUFFER    2                      /* pieces held back before the first is queued */
#define SND_CH           2
#define SND_SLOT         250
#define SND_FILE         "amigadoor/snd.wav"

#define TAP_DOWN_FRAMES  3                      /* a typed key is held this long, then let go this long */
#define TAP_UP_FRAMES    2
#define TYPED_HOLD_MS    180                    /* a typed arrow holds the joystick this long after its last repeat */

#define LOSSY_SKIP       0.3                    /* lossy when more than this share of frames was skipped two seconds
                                                 * running (one alone is often just a scene changing) */
#define LOSSY_HOLD_MS    10000                  /* then lossy at least this long, doubling each time it comes back */
#define LOSSY_HOLD_MAX   80000
#define STILL_FRAMES     25                     /* a blurred picture unchanged this many frames goes again, lossless */

static const play_caps_t *g_caps;
static const char *g_session_dir;      /* the caller's session folder: drive.txt, uploads */

#ifdef _WIN32

static long now_ms(void)
{
    return win_now_ms();
}

static bool write_all(const char *data, size_t len)
{
    return conn_write(data, len);
}

/* What the caller sent, waiting up to timeout_ms: >0 bytes, 0 nothing, -1 hung up */
static int read_input(unsigned char *buf, int max, int timeout_ms)
{
    return conn_read(buf, max, timeout_ms);
}

#else

/* The caller's connection for writing. Not stdout: PUAE prints its own chatter there (retro_vfs_file_open_impl ...),
 * which reached the caller's screen and broke pictures and sound apart (sysop, 2026-10-28), so play_main moves the
 * connection to another descriptor and points stdout and stderr at /dev/null. */
static int g_conn = STDOUT_FILENO;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static bool write_all(const char *data, size_t len)
{
    while (len > 0)
    {
        ssize_t n = write(g_conn, data, len);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN)
            {
                struct timeval tv = { 0, 10000 };
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(g_conn, &fds);
                select(g_conn + 1, NULL, &fds, NULL, &tv);
                continue;
            }
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

static int read_input(unsigned char *buf, int max, int timeout_ms)
{
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    fd_set fds;
    ssize_t n;

    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
        return 0;
    n = read(STDIN_FILENO, buf, (size_t)max);
    if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN))
        return -1;
    return n < 0 ? 0 : (int)n;
}

#endif

/* ---- what's on its way (as pix_play.c) ---- */

static struct
{
    long   sent[MAX_IN_FLIGHT];
    size_t bytes[MAX_IN_FLIGHT];
    size_t total;
    int    head, count;
} g_fly;

static long   g_rtt_sum, g_rtt_n;
static long   g_rtt_floor[10];
static long   g_rtt_floor_sec = -1;
static size_t g_acked;
static double g_rate;                   /* the link's speed as measured, bytes a millisecond */
static double g_frame_avg = 4000;
static int    g_orphans;
static long   g_pushes, g_acks, g_resets; /* for the log */

static void fly_push(long now, size_t bytes)
{
    int i = (g_fly.head + g_fly.count) % MAX_IN_FLIGHT;
    g_fly.sent[i] = now;
    g_fly.bytes[i] = bytes;
    g_fly.total += bytes;
    g_fly.count++;
    g_pushes++;
}

static void fly_reset(void)
{
    g_orphans += g_fly.count;
    g_resets++;
    g_fly.count = 0;
    g_fly.total = 0;
}

static long rtt_min(void)
{
    long m = 0;
    for (int i = 0; i < 10; i++)
        if (g_rtt_floor[i] > 0 && (m == 0 || g_rtt_floor[i] < m))
            m = g_rtt_floor[i];
    return m;
}

static void fly_ack(long now)
{
    long rtt, sec = now / 1000;
    g_acks++;
    if (g_orphans > 0)
    {
        g_orphans--;
        return;
    }
    if (g_fly.count == 0)
        return;
    rtt = now - g_fly.sent[g_fly.head];
    g_acked += g_fly.bytes[g_fly.head];
    g_fly.total -= g_fly.bytes[g_fly.head];
    g_fly.head = (g_fly.head + 1) % MAX_IN_FLIGHT;
    g_fly.count--;
    g_rtt_sum += rtt;
    g_rtt_n++;
    if (sec != g_rtt_floor_sec)
    {
        g_rtt_floor_sec = sec;
        g_rtt_floor[sec % 10] = rtt;
    }
    else if (rtt < g_rtt_floor[sec % 10])
        g_rtt_floor[sec % 10] = rtt;
}

static bool window_open(void)
{
    double window = g_rate * (double)(rtt_min() + QUEUE_ALLOWED_MS);
    if (window < MIN_WINDOW * g_frame_avg)
        window = MIN_WINDOW * g_frame_avg;
    return g_fly.count < MAX_IN_FLIGHT - 1 && (double)g_fly.total < window;
}

/* ---- the picture ---- */

static int     g_vw, g_vh;              /* the Amiga's frame as shown: its whole size, borders and all */
static int     g_tx, g_ty;              /* its tiles; the last column and row may be part tiles */
static uint8_t g_view[CORE_MAX_H * CORE_MAX_W * 3];
static uint8_t g_ref[CORE_MAX_H * CORE_MAX_W * 3];
static bool    g_have_ref;
static uint8_t g_scaled[CORE_MAX_W * 2 * CORE_MAX_H * 2 * 3];
static int     g_zoom, g_ox, g_oy;
/* Stretched here, by any ratio under 2x, when a whole-number zoom would leave it small (SyncTERM's usual 80x25 is
 * 640x400: 360x287 fits only at 1x). g_dw x g_dh is its size on screen. */
static bool    g_fill;
static int     g_dw, g_dh;
static int     g_bar_row, g_cell_h; /* the controls' text row (1-based) and a row's height in pixels */
static int     g_cell_w;
static bool    g_ntsc;
static bool    g_small;                 /* 1x, never stretched: smaller on screen, far less to send */
static bool    g_relayout;
static long    g_enc_ms;                /* time in the encoder, for the log */

/* Lossy pictures, while the link can't keep up */
static float   g_distance;              /* 0 lossless, otherwise the lossy distance in use now */
static float   g_lossy_distance = 3.0f; /* AMIGADOOR_LOSSY_DISTANCE; 0 keeps every picture lossless */
static long    g_lossy_until, g_lossy_hold = LOSSY_HOLD_MS;
static bool    g_blurred;               /* something lossy is on the caller's screen */
static int     g_still;                 /* frames the picture hasn't changed */

/* How many of a frame's pixels make one low-resolution pixel's width: 1, 2 (high resolution) or 4 (super-high) */
static int lores_per(int w)
{
    return w > 800 ? 4 : w > 400 ? 2 : 1;
}

static void choose_view(int w, int h)
{
    int zf, aw, ah;

    /* The last text row holds the controls; the picture gets the rest */
    g_bar_row = g_caps->rows > 0 && g_caps->px_h / g_caps->rows >= 6 && g_caps->px_h / g_caps->rows <= 40
                ? g_caps->rows : g_caps->px_h / 16;     /* a row count that makes no sense: 16-pixel rows */
    g_cell_h = g_bar_row > 0 ? g_caps->px_h / g_bar_row : 16;
    g_cell_w = g_caps->px_w / 80 > 0 ? g_caps->px_w / 80 : 8;
    aw = g_caps->px_w;
    ah = g_caps->px_h - g_cell_h;

    g_vw = w;
    g_vh = h;
    g_tx = (w + TW - 1) / TW;
    g_ty = (h + TH - 1) / TH;
    zf = aw / w < ah / h ? aw / w : ah / h;
    g_fill = (zf < 2 && !g_small) || w > 400;          /* a high-resolution frame always: its pixels are narrower */
    if (zf < 1)
        zf = 1;
    if (g_fill)
    {
        /* As high as it goes, as wide as the Amiga's shape makes it: PAL low-resolution pixels are a little wider
         * than tall (16:15), NTSC ones a little narrower (8:9 or so); high-resolution ones half as wide again,
         * super-high-resolution ones a quarter */
        double par = (g_ntsc ? 0.9 : 16.0 / 15.0) / lores_per(w);
        g_zoom = 1;
        g_dh = ah < h * 2 ? ah : h * 2;
        g_dw = (int)(w * par * g_dh / h + 0.5);
        if (g_dw > aw || g_dw > w * 2)
        {
            g_dw = aw < w * 2 ? aw : w * 2;
            g_dh = (int)(h * g_dw / (w * par) + 0.5);
        }
    }
    else
    {
        g_zoom = zf;
        if (g_zoom > 2 && !g_caps->zoom)
            g_zoom = 2;                 /* sending them pre-scaled beyond 2x costs too much */
        g_dw = w * g_zoom;
        g_dh = h * g_zoom;
    }
    g_ox = (g_caps->px_w - g_dw) / 2;
    g_oy = (ah - g_dh) / 2;
    if (g_ox < 0) g_ox = 0;
    if (g_oy < 0) g_oy = 0;
    g_have_ref = false;
    g_relayout = true;                  /* the mouse starts afresh: its last place was on the old layout */
}

/* The controls, on the screen's last row under the picture: drawn after every clear */
static void draw_bar(outbuf_t *o)
{
    char line[256];
    const char *fire = g_caps->keys ? "Ctrl/Alt" : "Space";     /* the line stays within 74 columns */
    int n;

    if (g_bar_row < 1)
        return;
    n = snprintf(line, sizeof(line),
                 "\033[%d;1H\033[0;44m \033[1;37mF12\033[0;36;44m/\033[1;37mCtrl-Q\033[0;36;44m Menu & leave "
                 "\xb3 \033[1;37mArrows\033[0;36;44m Joystick \xb3 \033[1;37m%s\033[0;36;44m Fire "
                 "\xb3 \033[1;37mMouse\033[0;36;44m Mouse\033[K\033[0m",
                 g_bar_row, fire);
    if (n > 0)
        out_bytes(o, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}

static long g_led_until;                /* the drive light shows until then (ms): it flickers, the picture shouldn't */

static void fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
        {
            uint8_t *p = g_view + (yy * g_vw + xx) * 3;
            p[0] = r, p[1] = g, p[2] = b;
        }
}

/* While a drive is busy, the Amiga's lights in the bottom right corner, as on its case: the power light and the
 * drive light, so a caller watching a quiet screen knows a game is loading */
static void draw_drive_light(void)
{
    int w = 22, h = 7, x = g_vw - w - 3, y = g_vh - h - 3;
    fill(x, y, w, h, 0x80, 0x7c, 0x70);                 /* the Amiga's warm grey, its dark edge */
    fill(x + 1, y + 1, w - 2, h - 2, 0xd0, 0xcc, 0xbc);
    fill(x + 3, y + 3, 6, 2, 0x20, 0xc0, 0x40);         /* power */
    fill(x + w - 9, y + 3, 6, 2, 0xff, 0x90, 0x10);     /* drive */
}

/* For the first seconds of every Amiga start, how to get to the menu, in the bottom left corner. A 5x7 font of just
 * the letters it needs. */
static long g_hint_until;

static const uint8_t *hint_glyph(char c)
{
    static const struct { char c; uint8_t rows[7]; } font[] = {
        { 'C', { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e } },
        { 'T', { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
        { 'R', { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 } },
        { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f } },
        { 'Q', { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d } },
        { 'M', { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 } },
        { 'e', { 0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e } },
        { 'n', { 0x00, 0x00, 0x16, 0x19, 0x11, 0x11, 0x11 } },
        { 'u', { 0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0d } },
    };
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].c == c)
            return font[i].rows;
    return NULL;                                        /* a space */
}

static void draw_hint(void)
{
    static const char text[] = "CTRL Q Menu";
    int n = (int)sizeof(text) - 1, w = n * 6 + 5, h = 11, x = 3, y = g_vh - h - 3;
    fill(x, y, w, h, 0xff, 0x88, 0x00);                 /* Workbench 1.3's orange edge, its blue inside */
    fill(x + 1, y + 1, w - 2, h - 2, 0x00, 0x55, 0xaa);
    for (int i = 0; i < n; i++)
    {
        const uint8_t *g = hint_glyph(text[i]);
        for (int r = 0; g != NULL && r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (g[r] & (0x10 >> b))
                    fill(x + 3 + i * 6 + b, y + 2 + r, 1, 1, 0xff, 0xff, 0xff);
    }
}

static void grab_view(long now)
{
    const uint8_t *rgb = core_rgb();
    for (int y = 0; y < g_vh; y++)
        memcpy(g_view + y * g_vw * 3, rgb + y * CORE_MAX_W * 3, (size_t)g_vw * 3);
    if (now < g_led_until)
        draw_drive_light();
    if (now < g_hint_until)
        draw_hint();
}

static bool tile_changed(int tx, int ty)
{
    int tw = (tx + 1) * TW <= g_vw ? TW : g_vw - tx * TW;
    for (int y = ty * TH; y < (ty + 1) * TH && y < g_vh; y++)
        if (memcmp(g_view + (y * g_vw + tx * TW) * 3, g_ref + (y * g_vw + tx * TW) * 3, (size_t)tw * 3) != 0)
            return true;
    return false;
}

static void draw_rect(outbuf_t *o, int x, int y, int w, int h)
{
    const uint8_t *src = g_view + (y * g_vw + x) * 3;
    int stride = g_vw * 3, zoom = g_zoom;
    const uint8_t *jxl;
    size_t n;
    char head[96];

    if (g_fill)
    {
        /* Stretched: screen column d shows the Amiga's column d * vw / dw, so the tile's own columns are
         * ceil(x * dw / vw) up to ceil((x + w) * dw / vw), and tiles meet with no gap or overlap. Shrunk (a
         * high-resolution screen), a screen column is the average of the Amiga columns it covers, so thin text
         * strokes stay, greyed, rather than vanishing with a dropped column. */
        int d0 = (x * g_dw + g_vw - 1) / g_vw, d1 = ((x + w) * g_dw + g_vw - 1) / g_vw;
        int e0 = (y * g_dh + g_vh - 1) / g_vh, e1 = ((y + h) * g_dh + g_vh - 1) / g_vh;
        int sw = d1 - d0, sh = e1 - e0;
        if (sw <= 0 || sh <= 0)
            return;
        for (int yy = 0; yy < sh; yy++)
        {
            int sy = (e0 + yy) * g_vh / g_dh - y;
            for (int xx = 0; xx < sw; xx++)
            {
                int c0 = (d0 + xx) * g_vw / g_dw, c1 = (d0 + xx + 1) * g_vw / g_dw;
                const uint8_t *row = src + sy * stride - x * 3;     /* the view's own row: columns outside the tile
                                                                     * are still the picture's */
                uint8_t *d = g_scaled + (yy * sw + xx) * 3;
                if (c1 > g_vw)
                    c1 = g_vw;
                if (c1 - c0 <= 1)
                    memcpy(d, row + c0 * 3, 3);
                else
                {
                    int r = 0, g = 0, b = 0, n = c1 - c0;
                    for (int c = c0; c < c1; c++)
                        r += row[c * 3], g += row[c * 3 + 1], b += row[c * 3 + 2];
                    d[0] = (uint8_t)(r / n), d[1] = (uint8_t)(g / n), d[2] = (uint8_t)(b / n);
                }
            }
        }
        src = g_scaled;
        stride = sw * 3;
        w = sw, h = sh, x = d0, y = e0;
        zoom = 1;
    }
    /* A terminal that can't scale pictures itself gets them at the size they're shown */
    else if (!g_caps->zoom && g_zoom > 1)
    {
        int sw = w * g_zoom;
        for (int yy = 0; yy < h * g_zoom; yy++)
            for (int xx = 0; xx < sw; xx++)
                memcpy(g_scaled + (yy * sw + xx) * 3, src + (yy / g_zoom) * stride + (xx / g_zoom) * 3, 3);
        src = g_scaled;
        stride = sw * 3;
        w *= g_zoom;
        h *= g_zoom;
        x *= g_zoom;
        y *= g_zoom;
        zoom = 1;
    }
    else
    {
        x *= g_zoom;
        y *= g_zoom;
    }

    {
        long t0 = now_ms();
        n = jxl_enc_rgb(src, stride, w, h, g_distance, &jxl);
        g_enc_ms += now_ms() - t0;
    }
    if (n == 0)
        return;
    if (zoom > 1)
        snprintf(head, sizeof(head), "DX=%d;DY=%d;ZX=%d;ZY=%d;", g_ox + x, g_oy + y, zoom, zoom);
    else
        snprintf(head, sizeof(head), "DX=%d;DY=%d;", g_ox + x, g_oy + y);

    if (g_caps->blob)
    {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "C;DrawJXLBlob;%s", head);
        apc_blob(o, cmd, jxl, n);
    }
    else
    {
        apc_blob(o, "C;S;amigadoor/frame.jxl;", jxl, n);
        apc_cmd(o, "C;DrawJXL;%samigadoor/frame.jxl", head);
    }
}

/* What changed since the caller's copy. False if nothing did. */
static bool draw_changes(outbuf_t *o)
{
    static bool tiles[MAX_TY][MAX_TX];
    int count = 0;

    for (int ty = 0; ty < g_ty; ty++)
        for (int tx = 0; tx < g_tx; tx++)
        {
            tiles[ty][tx] = !g_have_ref || tile_changed(tx, ty);
            count += tiles[ty][tx];
        }
    memcpy(g_ref, g_view, (size_t)g_vw * g_vh * 3);
    g_have_ref = true;
    if (count == 0)
        return false;
    if (g_distance > 0)
        g_blurred = true;

    if (count > FULL_FRACTION * g_tx * g_ty)
    {
        draw_rect(o, 0, 0, g_vw, g_vh);
        return true;
    }
    for (int ty = 0; ty < g_ty; ty++)
        for (int tx = 0; tx < g_tx; )
        {
            int x0, x1, ty1;
            if (!tiles[ty][tx])
            {
                tx++;
                continue;
            }
            x0 = tx;
            while (tx < g_tx && tiles[ty][tx])
                tx++;
            x1 = tx;
            for (ty1 = ty + 1; ty1 < g_ty; ty1++)
            {
                bool same = (x0 == 0 || !tiles[ty1][x0 - 1]) && (x1 == g_tx || !tiles[ty1][x1]);
                for (int k = x0; k < x1 && same; k++)
                    same = tiles[ty1][k];
                if (!same)
                    break;
                for (int k = x0; k < x1; k++)
                    tiles[ty1][k] = false;
            }
            {
                int px = x0 * TW, py = ty * TH;
                draw_rect(o, px, py, (x1 * TW <= g_vw ? x1 * TW : g_vw) - px, (ty1 * TH <= g_vh ? ty1 * TH : g_vh) - py);
            }
        }
    return true;
}

/* ---- sound ---- */

static bool    g_sound_on;
static double  g_snd_pos, g_snd_step;   /* resampling: where the next output sample falls between the last input
                                         * sample (0) and the newest (1), and how far each output sample moves on */
static int16_t g_snd_last;
static uint8_t g_piece[SND_PREBUFFER + 1][44 + SND_PIECE];
static int     g_piece_fill, g_pieces_held;
static bool    g_snd_started;
static long    g_snd_pieces;

static void put_le(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void wav_header(uint8_t *h)
{
    memcpy(h, "RIFF", 4);
    put_le(h + 4, 36 + SND_PIECE, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2);                   /* PCM */
    put_le(h + 22, 1, 2);                   /* mono */
    put_le(h + 24, SND_RATE, 4);
    put_le(h + 28, SND_RATE, 4);            /* bytes a second */
    put_le(h + 32, 1, 2);
    put_le(h + 34, 8, 2);
    memcpy(h + 36, "data", 4);
    put_le(h + 40, SND_PIECE, 4);
}

static void sound_send(outbuf_t *o, const uint8_t *wav)
{
    apc_blob(o, "C;S;" SND_FILE ";", wav, 44 + SND_PIECE);
    apc_cmd(o, "A;Load;S=%d;" SND_FILE, SND_SLOT);
    apc_cmd(o, "A;Queue;C=%d;S=%d", SND_CH, SND_SLOT);
    g_snd_pieces++;
}

/* The core's newest sound into pieces; a finished piece goes out (after the first few are held back as a cushion).
 * Returns the bytes added, 0 if none. */
static size_t sound_update(outbuf_t *o)
{
    int16_t buf[2048 * 2];
    int n;
    size_t before = o->len;

    while ((n = core_audio(buf, 2048)) > 0)
    {
        if (!g_sound_on)
            continue;
        for (int i = 0; i < n; i++)
        {
            int16_t mono = (int16_t)((buf[i * 2] + buf[i * 2 + 1]) / 2);
            /* the input moves on one sample; every output sample that falls between the last and this one is made */
            while (g_snd_pos <= 1.0)
            {
                double s = g_snd_last + (mono - g_snd_last) * g_snd_pos;
                uint8_t *piece = g_piece[g_pieces_held];
                piece[44 + g_piece_fill++] = (uint8_t)((int)lrint(s) / 256 + 128);
                g_snd_pos += g_snd_step;
                if (g_piece_fill == SND_PIECE)
                {
                    wav_header(piece);
                    g_piece_fill = 0;
                    if (!g_snd_started && ++g_pieces_held < SND_PREBUFFER + 1)
                        continue;
                    if (!g_snd_started)
                    {
                        for (int k = 0; k <= SND_PREBUFFER; k++)
                            sound_send(o, g_piece[k]);
                        g_pieces_held = 0;
                        g_snd_started = true;
                    }
                    else
                        sound_send(o, piece);
                }
            }
            g_snd_pos -= 1.0;
            g_snd_last = mono;
        }
    }
    return o->len - before;
}

static void sound_restart(outbuf_t *o)
{
    apc_cmd(o, "A;Flush;C=%d", SND_CH);
    g_piece_fill = g_pieces_held = 0;
    g_snd_started = false;
    g_snd_pos = 0;
}

/* ---- keys ---- */

static bool g_held[256];                /* by Linux key code, as CSI = 1 h reports them */
static bool g_joy_mode = true;          /* the arrows and fire keys are the joystick */
static bool g_menu_asked, g_quit_asked;
static unsigned g_typed_joy;            /* the joystick as typed keys hold it, without key reports */
static void mouse_release(void);
static long g_typed_joy_until;

/* A Linux key code as a libretro key */
static unsigned evdev_to_retrok(int code)
{
    static const unsigned main_block[89] = {
        0, RETROK_ESCAPE, RETROK_1, RETROK_2, RETROK_3, RETROK_4, RETROK_5, RETROK_6, RETROK_7, RETROK_8, RETROK_9,
        RETROK_0, RETROK_MINUS, RETROK_EQUALS, RETROK_BACKSPACE, RETROK_TAB, RETROK_q, RETROK_w, RETROK_e, RETROK_r,
        RETROK_t, RETROK_y, RETROK_u, RETROK_i, RETROK_o, RETROK_p, RETROK_LEFTBRACKET, RETROK_RIGHTBRACKET,
        RETROK_RETURN, RETROK_LCTRL, RETROK_a, RETROK_s, RETROK_d, RETROK_f, RETROK_g, RETROK_h, RETROK_j, RETROK_k,
        RETROK_l, RETROK_SEMICOLON, RETROK_QUOTE, RETROK_BACKQUOTE, RETROK_LSHIFT, RETROK_BACKSLASH, RETROK_z,
        RETROK_x, RETROK_c, RETROK_v, RETROK_b, RETROK_n, RETROK_m, RETROK_COMMA, RETROK_PERIOD, RETROK_SLASH,
        RETROK_RSHIFT, RETROK_KP_MULTIPLY, RETROK_LALT, RETROK_SPACE, RETROK_CAPSLOCK, RETROK_F1, RETROK_F2,
        RETROK_F3, RETROK_F4, RETROK_F5, RETROK_F6, RETROK_F7, RETROK_F8, RETROK_F9, RETROK_F10, RETROK_NUMLOCK,
        RETROK_SCROLLOCK, RETROK_KP7, RETROK_KP8, RETROK_KP9, RETROK_KP_MINUS, RETROK_KP4, RETROK_KP5, RETROK_KP6,
        RETROK_KP_PLUS, RETROK_KP1, RETROK_KP2, RETROK_KP3, RETROK_KP0, RETROK_KP_PERIOD, 0, 0, RETROK_LESS,
        RETROK_F11, RETROK_F12,
    };
    if (code >= 0 && code <= 88)
        return main_block[code];
    switch (code)
    {
    case 96:  return RETROK_KP_ENTER;
    case 97:  return RETROK_RCTRL;
    case 98:  return RETROK_KP_DIVIDE;
    case 100: return RETROK_RALT;
    case 102: return RETROK_HOME;
    case 103: return RETROK_UP;
    case 104: return RETROK_PAGEUP;
    case 105: return RETROK_LEFT;
    case 106: return RETROK_RIGHT;
    case 107: return RETROK_END;
    case 108: return RETROK_DOWN;
    case 109: return RETROK_PAGEDOWN;
    case 110: return RETROK_INSERT;             /* PUAE: Help */
    case 111: return RETROK_DELETE;
    case 125: return RETROK_LSUPER;             /* the Windows keys: PUAE makes them the left and right Amiga keys */
    case 126: return RETROK_RSUPER;
    case 127: return RETROK_RSUPER;             /* the menu key */
    default:  return 0;
    }
}

/* Which joystick bits a Linux key code drives in joystick mode (0: none) */
static unsigned joy_bits(int code)
{
    switch (code)
    {
    case 103: case 72: return JOY_UP;               /* Up, keypad 8 */
    case 108: case 80: return JOY_DOWN;
    case 105: case 75: return JOY_LEFT;
    case 106: case 77: return JOY_RIGHT;
    case 71:  return JOY_UP | JOY_LEFT;             /* keypad 7, 9, 1, 3 */
    case 73:  return JOY_UP | JOY_RIGHT;
    case 79:  return JOY_DOWN | JOY_LEFT;
    case 81:  return JOY_DOWN | JOY_RIGHT;
    case 97: case 100: case 29: case 56: case 82: return JOY_FIRE;  /* either Ctrl or Alt (the right ones alone were
                                                                      * cramped, sysop 2026-10-28), keypad 0 */
    default:  return 0;
    }
}

static void sync_joystick(long now)
{
    unsigned bits = 0;
    if (g_joy_mode)
    {
        for (int c = 0; c < 256; c++)
            if (g_held[c])
                bits |= joy_bits(c);
        if (now < g_typed_joy_until)
            bits |= g_typed_joy;
    }
    core_joystick(bits);
}

static void key_report(const char *params, bool down, long now)
{
    /* CSI = a;b;c K (down) or k (up): usually one code, several only when the terminal lets go of everything */
    for (const char *p = params; *p != '\0'; )
    {
        int code = atoi(p);
        if (code > 0 && code < 256 && g_held[code] != down)
        {
            g_held[code] = down;
            if (code == 88 || (code == 16 && (g_held[29] || g_held[97])))
            {
                /* F12, or Ctrl-Q for keyboards whose F12 needs Fn: the session menu, never the Amiga's */
                if (down)
                    g_menu_asked = true;
            }
            else if (g_joy_mode && joy_bits(code) != 0)
                sync_joystick(now);
            else if (evdev_to_retrok(code) != 0)
                core_key(evdev_to_retrok(code), down);
        }
        p = strchr(p, ';');
        if (p == NULL)
            break;
        p++;
    }
}

static void release_all(long now)
{
    for (int c = 1; c < 256; c++)
        if (g_held[c])
        {
            g_held[c] = false;
            if (evdev_to_retrok(c) != 0)
                core_key(evdev_to_retrok(c), false);
        }
    g_typed_joy_until = 0;
    sync_joystick(now);
    mouse_release();
}

/* Without key reports: typed characters become key taps, one at a time */
#define TAPS 256
static struct { unsigned key; bool shift; } g_taps[TAPS];
static int g_tap_head, g_tap_count, g_tap_phase;

static void tap(unsigned key, bool shift)
{
    if (g_tap_count < TAPS)
    {
        int i = (g_tap_head + g_tap_count++) % TAPS;
        g_taps[i].key = key;
        g_taps[i].shift = shift;
    }
}

/* A typed character as the US-layout key (and Shift) that makes it */
static void tap_char(unsigned char c)
{
    static const char shifted[] = "~!@#$%^&*()_+{}|:\"<>?";
    static const char base[]    = "`1234567890-=[]\\;',./";
    const char *s;

    if (c >= 'a' && c <= 'z')
        tap(RETROK_a + (c - 'a'), false);
    else if (c >= 'A' && c <= 'Z')
        tap(RETROK_a + (c - 'A'), true);
    else if (c >= '0' && c <= '9')
        tap(RETROK_0 + (c - '0'), false);
    else if (c == ' ')
        tap(RETROK_SPACE, false);
    else if (c == '\r' || c == '\n')
        tap(RETROK_RETURN, false);
    else if (c == 8 || c == 127)
        tap(RETROK_BACKSPACE, false);
    else if (c != 0 && (s = strchr(shifted, c)) != NULL)
        tap((unsigned)base[s - shifted], true);
    else if (c != 0 && strchr(base, c) != NULL)
        tap(c, false);                                  /* RETROK_ for these is the character itself */
}

/* One step a frame: press, hold, let go, pause */
static void tap_step(void)
{
    if (g_tap_count == 0)
        return;
    if (g_tap_phase == 0)
    {
        if (g_taps[g_tap_head].shift)
            core_key(RETROK_LSHIFT, true);
        core_key(g_taps[g_tap_head].key, true);
    }
    else if (g_tap_phase == TAP_DOWN_FRAMES)
    {
        core_key(g_taps[g_tap_head].key, false);
        if (g_taps[g_tap_head].shift)
            core_key(RETROK_LSHIFT, false);
    }
    if (++g_tap_phase == TAP_DOWN_FRAMES + TAP_UP_FRAMES)
    {
        g_tap_phase = 0;
        g_tap_head = (g_tap_head + 1) % TAPS;
        g_tap_count--;
    }
}

/* A typed escape sequence (no key reports): arrows, F1-F8, F12 */
static void typed_sequence(const char *params, unsigned char final, long now)
{
    unsigned dir = final == 'A' ? JOY_UP : final == 'B' ? JOY_DOWN : final == 'C' ? JOY_RIGHT : final == 'D' ? JOY_LEFT : 0;

    if (dir != 0 && params[0] == '\0')
    {
        if (g_joy_mode)
        {
            g_typed_joy = (now < g_typed_joy_until ? g_typed_joy : 0) | dir;
            g_typed_joy_until = now + TYPED_HOLD_MS;
            sync_joystick(now);
        }
        else
            tap(final == 'A' ? RETROK_UP : final == 'B' ? RETROK_DOWN : final == 'C' ? RETROK_RIGHT : RETROK_LEFT,
                false);
        return;
    }
    if (final == '~')
    {
        static const struct { int n; unsigned key; } f[] = { { 11, RETROK_F1 }, { 12, RETROK_F2 }, { 13, RETROK_F3 },
            { 14, RETROK_F4 }, { 15, RETROK_F5 }, { 17, RETROK_F6 }, { 18, RETROK_F7 }, { 19, RETROK_F8 },
            { 20, RETROK_F9 }, { 21, RETROK_F10 }, { 2, RETROK_INSERT }, { 3, RETROK_DELETE } };
        int n = atoi(params);
        if (n == 24)
        {
            g_menu_asked = true;
            return;
        }
        for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++)
            if (f[i].n == n)
                tap(f[i].key, false);
    }
}

/* ---- the mouse ---- */

static bool     g_mouse_px;             /* reports in pixels (CSI ? 1016 h); otherwise in text cells */
static bool     g_mouse_seen;           /* a report has come: g_mx, g_my hold where */
static double   g_mx, g_my;             /* the caller's pointer, in the Amiga's pixels over the picture */
static double   g_mfx, g_my_frac;       /* movement smaller than a pixel, carried to the next report */
static unsigned g_mbuttons;
static int      g_mouse_scale = 2;      /* Amiga mouse counts a low-resolution pixel (AMIGADOOR_MOUSE_SCALE): two, as
                                         * Intuition moves its pointer a high-resolution pixel a count (measured in
                                         * Sergoth: 100 pixels of the caller's moved it 50 at 1) */

#define EDGE_PUSH 64                    /* past the picture's edge, the Amiga's pointer is pushed this far that way */

static void mouse_at(int px, int py, bool left_edge, bool top_edge, unsigned buttons)
{
    /* Where on the picture, in the Amiga's low-resolution pixels (it may be off it): a high-resolution frame's
     * pixels are narrower, and the mouse moves the same on any */
    double lw = (double)g_vw / lores_per(g_vw);
    double x = (double)(px - g_ox) * lw / g_dw, y = (double)(py - g_oy) * g_vh / g_dh;
    double dx = 0, dy = 0;
    int ix, iy;

    if (g_relayout)
    {
        g_mouse_seen = false;
        g_relayout = false;
    }
    if (g_mouse_seen)
    {
        dx = x - g_mx + g_mfx;
        dy = y - g_my + g_my_frac;
    }
    g_mouse_seen = true;
    g_mx = x;
    g_my = y;
    ix = (int)dx;
    iy = (int)dy;
    g_mfx = dx - ix;
    g_my_frac = dy - iy;
    ix *= g_mouse_scale;
    iy *= g_mouse_scale;
    if (x < 0 || left_edge) ix -= EDGE_PUSH;
    if (x >= lw) ix += EDGE_PUSH;
    if (y < 0 || top_edge) iy -= EDGE_PUSH;   /* the screen's own top: the picture usually starts there */
    if (y >= g_vh) iy += EDGE_PUSH;
    g_mbuttons = buttons;
    core_mouse(ix, iy, buttons);
}

/* CSI < b ; x ; y M (pressed, or moved) or m (let go) */
static void sgr_mouse(const char *params, bool press)
{
    int b, col, row, base, px, py;
    unsigned buttons = g_mbuttons, bit;

    if (sscanf(params, "%d;%d;%d", &b, &col, &row) != 3)
        return;
    base = b & ~(4 | 8 | 16 | 32);
    if (base >= 64)
        return;                                         /* the wheel: the Amiga's mouse has none */
    bit = base == 0 ? MOUSE_LEFT : base == 2 ? MOUSE_RIGHT : 0;
    if (!(b & 32) && bit != 0)
        buttons = press ? (buttons | bit) : (buttons & ~bit);
    if (g_mouse_px)
        px = col - 1, py = row - 1;
    else
        px = (col - 1) * g_cell_w + g_cell_w / 2, py = (row - 1) * g_cell_h + g_cell_h / 2;
    mouse_at(px, py, g_mouse_px ? px == 0 : col == 1, g_mouse_px ? py == 0 : row == 1, buttons);
}

static void mouse_release(void)
{
    g_mbuttons = 0;
    g_mouse_seen = false;
    core_mouse(0, 0, 0);
}

/*
 * The caller's bytes: answers to the door's own questions (cursor position, key reports, sound notices) are taken
 * out here; anything else is typing. An escape sequence can be split across reads, so it is gathered until complete.
 */
static unsigned char g_seq[64];
static int  g_seq_len;
static long g_seq_at;

static void finish_sequence(long now)
{
    unsigned char final = g_seq[g_seq_len - 1];
    char params[64];
    int n = g_seq_len - 3;
    bool ss3 = g_seq[1] == 'O';

    memcpy(params, g_seq + 2, (size_t)n);
    params[n] = '\0';
    g_seq_len = 0;

    if (ss3)
    {
        /* ESC O P..S: F1-F4 */
        if (!g_caps->keys && final >= 'P' && final <= 'S')
            tap(RETROK_F1 + (final - 'P'), false);
        return;
    }
    if (final == 'R' && params[0] >= '0' && params[0] <= '9')
        fly_ack(now);                                   /* ESC [ row ; col R: a frame has arrived */
    else if (params[0] == '<' && (final == 'M' || final == 'm'))
        sgr_mouse(params + 1, final == 'M');
    else if (params[0] == '=' && (final == 'K' || final == 'k'))
        key_report(params + 1, final == 'K', now);
    else if (params[0] == '=' || params[0] == '<' || params[0] == '?')
        ;                                               /* sound notices and other answers */
    else if (!g_caps->keys)
        typed_sequence(params, final, now);
}

static void take_input(const unsigned char *data, int len, long now)
{
    for (int i = 0; i < len; i++)
    {
        unsigned char c = data[i];

        if (g_seq_len == 1)
        {
            if (c == '[' || c == 'O')
            {
                g_seq[g_seq_len++] = c;
                continue;
            }
            if (!g_caps->keys)
                tap(RETROK_ESCAPE, false);              /* ESC then something else: Esc, then that */
            g_seq_len = 0;
        }
        else if (g_seq_len >= 2)
        {
            if (g_seq_len < (int)sizeof(g_seq))
                g_seq[g_seq_len++] = c;
            if (c >= 0x40 && c <= 0x7E)
                finish_sequence(now);
            else if (g_seq_len == (int)sizeof(g_seq))
                g_seq_len = 0;
            continue;
        }

        if (c == 27)
        {
            g_seq[0] = c;
            g_seq_len = 1;
            g_seq_at = now;
            continue;
        }
        if (g_caps->keys)
            continue;                                   /* keys come as reports; this is just the typed copy */
        if (c == 17)
            g_menu_asked = true;                        /* Ctrl-Q: the session menu, as F12 */
        else if (c == ' ' && g_joy_mode)
        {
            g_typed_joy = (now < g_typed_joy_until ? g_typed_joy : 0) | JOY_FIRE;
            g_typed_joy_until = now + TYPED_HOLD_MS;
        }
        else
            tap_char(c);
    }
    if (g_seq_len == 1 && now - g_seq_at > 50)
    {
        if (!g_caps->keys)
            tap(RETROK_ESCAPE, false);                  /* a lone Escape */
        g_seq_len = 0;
    }
}

/* ---- a slower link, for testing (as pix_play.c) ---- */

#define SHAPE_CHUNKS 4096
#define SHAPE_IN     256

static bool     g_shape;
static double   g_shape_rate, g_shape_free;
static long     g_shape_half_ping;
static outbuf_t g_shape_out;
static size_t   g_shape_sent;
static struct { long due; size_t end; } g_shape_chunks[SHAPE_CHUNKS];
static int      g_shape_head, g_shape_count;
static struct { long due; int len; unsigned char data[512]; } g_shape_in[SHAPE_IN];
static int      g_in_head, g_in_count;

static void shape_out(const char *data, size_t len, long now)
{
    int i;
    if (g_shape_count == SHAPE_CHUNKS)
        return;
    if (g_shape_free < now)
        g_shape_free = now;
    g_shape_free += (double)len / g_shape_rate;
    out_bytes(&g_shape_out, data, len);
    i = (g_shape_head + g_shape_count) % SHAPE_CHUNKS;
    g_shape_chunks[i].due = (long)g_shape_free + g_shape_half_ping;
    g_shape_chunks[i].end = g_shape_out.len;
    g_shape_count++;
}

static bool shape_flush(long now, bool all)
{
    while (g_shape_count > 0 && (all || g_shape_chunks[g_shape_head].due <= now))
    {
        size_t end = g_shape_chunks[g_shape_head].end;
        if (!write_all(g_shape_out.data + g_shape_sent, end - g_shape_sent))
            return false;
        g_shape_sent = end;
        g_shape_head = (g_shape_head + 1) % SHAPE_CHUNKS;
        g_shape_count--;
    }
    if (g_shape_count == 0)
        g_shape_out.len = g_shape_sent = 0;
    return true;
}

static void shape_in(const unsigned char *data, int len, long now)
{
    int i;
    if (g_in_count == SHAPE_IN)
        return;
    i = (g_in_head + g_in_count) % SHAPE_IN;
    g_shape_in[i].due = now + g_shape_half_ping;
    g_shape_in[i].len = len > 512 ? 512 : len;
    memcpy(g_shape_in[i].data, data, (size_t)g_shape_in[i].len);
    g_in_count++;
}

static void shape_in_release(long now)
{
    while (g_in_count > 0 && g_shape_in[g_in_head].due <= now)
    {
        take_input(g_shape_in[g_in_head].data, g_shape_in[g_in_head].len, now);
        g_in_head = (g_in_head + 1) % SHAPE_IN;
        g_in_count--;
    }
}

static bool send_out(const outbuf_t *o, long now)
{
    if (o->len == 0)
        return true;
    if (g_shape)
    {
        shape_out(o->data, o->len, now);
        return true;
    }
    return write_all(o->data, o->len);
}

/* ---- if the door itself falls over: put the terminal back first (as pix_play.c) ---- */

#define MOUSE_ON  "\033[?1000h\033[?1003h\033[?1006h"
#define MOUSE_OFF "\033[?1016l\033[?1006l\033[?1003l\033[?1000l"

static const char CRASH_RESET[] = MOUSE_OFF "\033[=2l\033[=1l" APC_PREFIX "A;Flush;C=2" APC_END "\033[0m\033[2J\033[H\033[?25h";

#ifdef _WIN32

static void crash_guard(bool on)
{
    win_crash_guard(on ? CRASH_RESET : NULL, sizeof(CRASH_RESET) - 1);
}

#else

static const int CRASH_SIGNALS[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };

static void crash_reset(int sig)
{
    ssize_t n = write(g_conn, CRASH_RESET, sizeof(CRASH_RESET) - 1);
    (void)n;
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_guard(bool on)
{
    for (size_t i = 0; i < sizeof(CRASH_SIGNALS) / sizeof(CRASH_SIGNALS[0]); i++)
        signal(CRASH_SIGNALS[i], on ? crash_reset : SIG_DFL);
}

#endif

/* ---- the session menu (F12) ---- */

/* One typed key, waiting up to timeout_ms; -1 if none, -2 if the caller has gone */
static int read_key(int timeout_ms)
{
    unsigned char c;
    int n = read_input(&c, 1, timeout_ms);
    return n > 0 ? c : n == 0 ? -1 : -2;
}

/* Drops whatever the terminal still has to say (answers to frames in flight, key reports) */
static void drain_input(int quiet_ms)
{
    while (read_key(quiet_ms) >= 0)
        ;
}

static void menu_line(outbuf_t *o, const char *key, const char *text, const char *state)
{
    out_fmt(o, "\033[1;37m  [%s] \033[0;37m%-34s\033[1;36m%s\033[0m\r\n", key, text, state ? state : "");
}

static void menu_title(outbuf_t *o, const char *text)
{
    out_str(o, "\033[0m\033[2J\033[H\033[?25l");
    out_fmt(o, "\033[1;34m  ===============================================\r\n"
               "\033[1;37m    %s\r\n"
               "\033[1;34m  ===============================================\r\n\r\n", text);
}

static void show(outbuf_t *o)
{
    write_all(o->data, o->len);
    o->len = 0;
}

typedef enum { MENU_RESUME, MENU_QUIT, MENU_RELOAD, MENU_HANGUP } menu_result_t;

#define LIBRARY_PAGE 18             /* one name a line, so twice the old 9 (those used a second line for the blurb) */
#define LIBRARY_FIND 40

static char fold_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/* Whether the game's name contains the search, ignoring the case of ASCII letters. An empty search matches all. */
static bool name_has(const char *name, const char *query)
{
    size_t qlen = strlen(query);
    if (qlen == 0)
        return true;
    for (; *name != '\0'; name++)
    {
        size_t i;
        for (i = 0; i < qlen; i++)
        {
            if (name[i] == '\0' || fold_ascii(name[i]) != fold_ascii(query[i]))
                break;
        }
        if (i == qlen)
            return true;
    }
    return false;
}

typedef enum
{
    MKEY_NONE, MKEY_UP, MKEY_DOWN, MKEY_PGUP, MKEY_PGDN, MKEY_HOME, MKEY_END,
    MKEY_ENTER, MKEY_ESC, MKEY_CHAR, MKEY_HANGUP
} menu_key_t;

/* One menu key. Arrows, Page Up/Down, Home and End arrive as the sequences SyncTERM and a VT send
 * (CSI A/B, CSI V/U and CSI 5~/6~, CSI H/K and CSI 1~/4~). A leaked key report (CSI = code K) counts too. */
static menu_key_t read_menu_key(int *ch)
{
    char params[40];
    int c = read_key(600000), n = 0, final;

    *ch = 0;
    if (c == -2)
        return MKEY_HANGUP;
    if (c == -1)
        return MKEY_NONE;
    if (c == '\r' || c == '\n')
        return MKEY_ENTER;
    if (c != 27)
    {
        *ch = c;
        return MKEY_CHAR;
    }
    c = read_key(50);
    if (c == -2)
        return MKEY_HANGUP;
    if (c != '[' && c != 'O')
        return MKEY_ESC;
    for (;;)
    {
        c = read_key(50);
        if (c == -2)
            return MKEY_HANGUP;
        if (c < 0)
            return MKEY_ESC;
        if (c >= 0x40 && c <= 0x7E)
            break;
        if (n + 1 < (int)sizeof(params))
            params[n++] = (char)c;
    }
    params[n] = '\0';
    final = c;
    if (params[0] == '=' && (final == 'K' || final == 'k'))
    {
        int code;
        if (final == 'k')
            return MKEY_NONE;                       /* a key going up: the press already moved the bar */
        code = atoi(params + 1);
        if (code == 103 || code == 72)
            return MKEY_UP;
        if (code == 108 || code == 80)
            return MKEY_DOWN;
        if (code == 104 || code == 73)
            return MKEY_PGUP;
        if (code == 109 || code == 81)
            return MKEY_PGDN;
        if (code == 102 || code == 71)
            return MKEY_HOME;
        if (code == 107 || code == 79)
            return MKEY_END;
        return MKEY_NONE;
    }
    if (final == 'A')
        return MKEY_UP;
    if (final == 'B')
        return MKEY_DOWN;
    if (final == 'V' || (final == '~' && params[0] == '5'))
        return MKEY_PGUP;
    if (final == 'U' || (final == '~' && params[0] == '6'))
        return MKEY_PGDN;
    if (final == 'H' || (final == '~' && (params[0] == '1' || params[0] == '7')))
        return MKEY_HOME;
    if (final == 'F' || final == 'K' || (final == '~' && (params[0] == '4' || params[0] == '8')))
        return MKEY_END;
    return MKEY_NONE;
}

/* One library row. The bar is inverse; the marker is CP437 0x10 (a triangle), one byte, not a UTF-8 arrow. */
static void library_row(outbuf_t *o, const char *name, bool on)
{
    char shown[80];

    snprintf(shown, sizeof(shown), " %c %-74.74s", on ? '\x10' : ' ', name);
    if (on)
        out_fmt(o, "\033[1;37;44m%s\033[0m\r\n", shown);
    else
        out_fmt(o, "\033[0;36m%s\033[0m\r\n", shown);
}

/* 1: query replaced, 0: left as it was, -1: the caller has gone. A blank line clears the search. */
static int ask_search(outbuf_t *o, const char *side, char *query)
{
    char edit[LIBRARY_FIND + 1];
    int n = (int)strlen(query);

    snprintf(edit, sizeof(edit), "%s", query);
    menu_title(o, "Search the library");
    out_fmt(o, "\033[0;37m  Part of the game's name. A blank line shows every %s game.\r\n\r\n"
               "\033[1;37m  Search: \033[1;36m", side);
    out_str(o, edit);
    out_str(o, "\033[?25h");
    show(o);
    for (;;)
    {
        int c = read_key(600000);
        if (c == -2)
            return -1;
        if (c == '\r' || c == '\n')
        {
            snprintf(query, LIBRARY_FIND + 1, "%s", edit);
            out_str(o, "\033[?25l");
            show(o);
            return 1;
        }
        if (c == 27)
        {
            drain_input(50);
            out_str(o, "\033[?25l");
            show(o);
            return 0;
        }
        if ((c == 8 || c == 127) && n > 0)
        {
            edit[--n] = '\0';
            out_str(o, "\b \b");
            show(o);
            continue;
        }
        if (c >= 32 && c < 127 && n + 1 < (int)sizeof(edit))
        {
            edit[n++] = (char)c;
            edit[n] = '\0';
            out_fmt(o, "%c", c);
            show(o);
        }
    }
}

/* The library, a page at a time. MENU_RELOAD when a title was picked (drive.txt now holds it). */
static menu_result_t menu_library(outbuf_t *o)
{
    title_t *titles = NULL;
    int *pick = NULL;
    int all = library_load(&titles), n = 0, at = 0;
    char query[LIBRARY_FIND + 1] = "";
    drive_t now;
    const char *side;
    menu_result_t result = MENU_RESUME;

    /* Only the games for the machine the caller is on (sysop, 2026-10-28): the other machine's are a switch away */
    drive_load(&now);
    side = now.a1200 ? "Amiga 1200" : "Amiga 500";
    for (int i = 0; i < all; i++)
        if (library_a1200(&titles[i]) == now.a1200)
            titles[n++] = titles[i];
    if (n == 0)
    {
        char heading[64];
        snprintf(heading, sizeof(heading), "The library: %s games", side);
        menu_title(o, heading);
        out_fmt(o, "\033[1;33m  The sysop hasn't put any %s games in the library yet.\r\n\r\n"
                   "\033[0;37m  (F12, A switches machine.) Any key goes back.\033[0m", side);
        show(o);
        result = read_key(600000) == -2 ? MENU_HANGUP : MENU_RESUME;
        goto done;
    }
    if ((pick = malloc((size_t)n * sizeof(*pick))) == NULL)
    {
        menu_title(o, "The library");
        out_str(o, "\033[1;33m  The library couldn't be loaded.\r\n\r\n\033[0;37m  Any key goes back.\033[0m");
        show(o);
        result = read_key(600000) == -2 ? MENU_HANGUP : MENU_RESUME;
        goto done;
    }

    for (;;)
    {
        int ch = 0, matches = 0, top;
        char heading[80];
        menu_key_t key;

        for (int i = 0; i < n; i++)
            if (name_has(titles[i].title, query))
                pick[matches++] = i;
        if (matches == 0)
            at = 0;
        else if (at >= matches)
            at = matches - 1;
        top = matches == 0 ? 0 : (at / LIBRARY_PAGE) * LIBRARY_PAGE;
        if (query[0] == '\0')
            snprintf(heading, sizeof(heading), "The library: %s games", side);
        else
            snprintf(heading, sizeof(heading), "%s games: \"%s\"", side, query);
        menu_title(o, heading);
        if (matches == 0)
            out_fmt(o, "\033[1;33m  Nothing matches \"%s\".\r\n\r\n"
                       "\033[0;37m  S searches again (a blank search shows every game)\r\n"
                       "  Esc goes back.\033[0m", query);
        else
        {
            for (int i = 0; i < LIBRARY_PAGE && top + i < matches; i++)
                library_row(o, titles[pick[top + i]].title, top + i == at);
            out_fmt(o, "\r\n\033[0;37m  Up/Down  PgUp/PgDn  Home/End   Enter loads it   S searches\r\n"
                       "  Esc goes back. The %s games are under F12, A.\033[0m",
                    now.a1200 ? "Amiga 500" : "Amiga 1200");
        }
        show(o);
        key = read_menu_key(&ch);
        if (key == MKEY_HANGUP)
        {
            result = MENU_HANGUP;
            break;
        }
        if (key == MKEY_UP && at > 0)
            at--;
        else if (key == MKEY_DOWN && at + 1 < matches)
            at++;
        else if (key == MKEY_PGUP)
            at = at > LIBRARY_PAGE ? at - LIBRARY_PAGE : 0;
        else if (key == MKEY_PGDN && matches > 0)
            at = at + LIBRARY_PAGE < matches ? at + LIBRARY_PAGE : matches - 1;
        else if (key == MKEY_HOME)
            at = 0;
        else if (key == MKEY_END && matches > 0)
            at = matches - 1;
        else if (key == MKEY_CHAR && (ch == 's' || ch == 'S' || ch == '/'))
        {
            int found = ask_search(o, side, query);
            if (found < 0)
            {
                result = MENU_HANGUP;
                break;
            }
            if (found > 0)
                at = 0;
        }
        else if (key == MKEY_ENTER && matches > 0)
        {
            drive_t d;
            const char *why = NULL;
            if (library_use(&titles[pick[at]], &d, &why) && drive_save(&d))
            {
                result = MENU_RELOAD;
                break;
            }
            menu_title(o, "The library");
            out_fmt(o, "\033[1;33m  That title couldn't be loaded: %s\r\n\r\n\033[0;37m  Any key goes back.\033[0m",
                    why ? why : "the BBS couldn't store it");
            show(o);
            result = read_key(600000) == -2 ? MENU_HANGUP : MENU_RESUME;
            break;
        }
        else if (key == MKEY_ESC || key == MKEY_CHAR)
            break;
    }
done:
    free(pick);
    free(titles);
    return result;
}

static menu_result_t ask_start(outbuf_t *o, drive_t *dp);

/* The caller's own disks by ZMODEM. MENU_RELOAD when some arrived and they chose to boot them. */
#ifdef _WIN32
static int zm_read(unsigned char *buf, int max, int timeout_ms)
{
    return conn_read(buf, max, timeout_ms);
}
#endif

static menu_result_t menu_upload(outbuf_t *o)
{
    char incoming[600];
    drive_t d = { 0 };
    DIR *dir;
    struct dirent *e;

    snprintf(incoming, sizeof(incoming), "%s/incoming", g_session_dir);
    mkdir(incoming, 0700);
    menu_title(o, "Send your disk");
    out_str(o, "\033[0;37m  Start a ZMODEM upload in your terminal now (in SyncTERM: Alt-U).\r\n"
               "  .ADF floppies work, several at once for a multi-disk game, and so do\r\n"
               "  WHDLoad .LHA games and .HDF hard drives (up to 32 MB).\r\n"
               "  Cancel the transfer (Ctrl-X a few times) to come back without one.\r\n\r\n\033[0m");
    show(o);
#ifdef _WIN32
    {
        /* the door's own ZMODEM: no rz on Windows */
        static const zm_io_t io = { zm_read, conn_write };
        if (zm_receive(&io, incoming, IMAGE_MAX, 16) == ZM_HUNG_UP)
            return MENU_HANGUP;
    }
#else
    {
        char *const argv[] = { "rz", "-b", "-E", "-R", NULL };
        pid_t pid;
        if ((pid = fork()) == 0)
        {
            if (chdir(incoming) != 0 || dup2(g_conn, STDOUT_FILENO) < 0)    /* rz talks on stdin and stdout */
                _exit(127);
            execvp(argv[0], argv);
            _exit(127);
        }
        if (pid > 0)
            waitpid(pid, NULL, 0);
    }
#endif
    drain_input(300);                                   /* the transfer's last packets aren't menu choices */

    menu_title(o, "Send your disk");
    if ((dir = opendir(incoming)) != NULL)
    {
        while ((e = readdir(dir)) != NULL)
        {
            char path[900];
            const char *why = NULL, *not_kept;
            if (e->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "%s/%s", incoming, e->d_name);
            if (mine_add(&d, path, e->d_name, &why))
                out_fmt(o, "\033[1;32m  Arrived and kept: %.50s\r\n", d.disk[d.count - 1].name);
            else if (strncmp(why, "it ", 3) != 0 && (not_kept = why, disk_add(&d, path, e->d_name, &why)))
                out_fmt(o, "\033[1;33m  Arrived, for this visit only: %.40s\r\n\033[0;37m    (not kept: %s)\r\n",
                        e->d_name, not_kept);           /* the image is fine, keeping it isn't: a disk for now */
            else
                out_fmt(o, "\033[1;33m  Not used: %.40s (%s)\r\n", e->d_name, why);
            remove(path);
        }
        closedir(dir);
    }
    if (d.count == 0)
    {
        out_str(o, "\033[0;37m  No disk arrived.\r\n\r\n  Any key goes back.\033[0m");
        show(o);
        return read_key(600000) == -2 ? MENU_HANGUP : MENU_RESUME;
    }
    return ask_start(o, &d);
}

/* After disks were chosen: the Amiga restarts with them, on the machine they suit or the other. MENU_RELOAD if so. */
static menu_result_t ask_start(outbuf_t *o, drive_t *dp)
{
    drive_t d = *dp;
    int c;

    drive_guess_machine(&d);
    out_fmt(o, "\r\n\033[0;37m  The Amiga restarts with it in %s.\r\n\r\n",
            path_is_hd(d.disk[0].path) ? "its hard drive" : "DF0:");
    menu_line(o, "A", "Start it", d.a1200 ? "on an A1200" : "on an A500");
    menu_line(o, "B", "Start it on the other machine", d.a1200 ? "an A500" : "an A1200");
    out_str(o, "\r\n\033[0;37m  Any other key keeps what's in the Amiga now.\033[0m");
    show(o);
    c = read_key(600000);
    if (c == -2)
        return MENU_HANGUP;
    if (c != 'a' && c != 'A' && c != 'b' && c != 'B')
        return MENU_RESUME;
    if (c == 'b' || c == 'B')
        d.a1200 = !d.a1200;
    return drive_save(&d) ? MENU_RELOAD : MENU_RESUME;
}

/* The caller's kept disks, a page at a time: boot one, or delete one. MENU_RELOAD when one was booted. */
static menu_result_t menu_mine(outbuf_t *o)
{
    static char names[MINE_MAX][64];
    int page = 0;
    bool deleting = false;

    for (;;)
    {
        int n = mine_list(names, MINE_MAX), pages = (n + 8) / 9, c;
        if (page >= pages && page > 0)
            page = pages - 1;
        menu_title(o, deleting ? "Delete a kept disk" : "Your kept disks");
        if (n == 0)
        {
            out_str(o, "\033[0;37m  You haven't kept any disks yet. The ones you upload (F12, U) are kept\r\n"
                       "  here for your next visit, saved games and all.\r\n\r\n  Any key goes back.\033[0m");
            show(o);
            return read_key(600000) == -2 ? MENU_HANGUP : MENU_RESUME;
        }
        for (int i = 0; i < 9 && page * 9 + i < n; i++)
            out_fmt(o, "\033[1;37m  [%d] \033[1;36m%s\r\n", i + 1, names[page * 9 + i]);
        if (deleting)
            out_str(o, "\r\n\033[1;33m  1-9 = delete that disk for good\033[0;37m   any other key = don't\033[0m");
        else
            out_fmt(o, "\r\n\033[0;37m  %d of %d kept.  1-9 = put it in DF0: (the ones after it follow, for D)\r\n"
                       "  X = delete one%s   any other key = back\033[0m",
                    n, MINE_MAX, pages > 1 ? "   N/P = next/previous page" : "");
        show(o);
        c = read_key(600000);
        if (c == -2)
            return MENU_HANGUP;
        if (c >= '1' && c <= '9' && page * 9 + (c - '1') < n)
        {
            int index = page * 9 + (c - '1');
            drive_t d;
            if (deleting)
            {
                mine_delete(names[index]);
                deleting = false;
                continue;
            }
            if (!mine_use(index, &d))
                continue;
            menu_title(o, "Your kept disks");
            out_fmt(o, "\033[1;36m  %s\r\n", d.disk[0].name);
            return ask_start(o, &d);
        }
        if (deleting)
            deleting = false;
        else if (c == 'x' || c == 'X')
            deleting = true;
        else if ((c == 'n' || c == 'N') && page + 1 < pages)
            page++;
        else if ((c == 'p' || c == 'P') && page > 0)
            page--;
        else
        {
            if (c == 27)
                drain_input(50);
            return MENU_RESUME;
        }
    }
}

static menu_result_t session_menu(void)
{
    outbuf_t o = { 0 };
    menu_result_t result = MENU_RESUME;
    long now = now_ms();

    release_all(now);
    out_str(&o, MOUSE_OFF);
    if (g_caps->keys)
        out_str(&o, "\033[=2l\033[=1l");             /* plain typing while the menu is up */
    show(&o);
    drain_input(150);
    /* The answers for frames in flight were just thrown away with the rest, so none are owed any more */
    g_fly.count = 0;
    g_fly.total = 0;
    g_orphans = 0;

    for (bool done = false; !done; )
    {
        int disks = core_disk_count(), in = core_disk_index(), c;
        drive_t d;
        char state[96];

        drive_load(&d);
        menu_title(&o, "Amiga paused");
        out_fmt(&o, "\033[0;37m  The machine: \033[1;36m%s\033[0;37m, %s\r\n", d.a1200 ? "Amiga 1200 (Kickstart 3.1)"
                : d.count > 0 && path_is_hd(d.disk[0].path) ? "Amiga 500 class (A600, Kickstart 3.1 for WHDLoad)"
                : "Amiga 500 (Kickstart 1.3)", d.ntsc ? "NTSC" : "PAL");
        if (d.count == 0)
            out_str(&o, "\033[0;37m  DF0: \033[1;36mempty\r\n\r\n");
        else
            out_fmt(&o, "\033[0;37m  %s \033[1;36m%s\033[0;37m%s\r\n\r\n",
                    path_is_hd(d.disk[0].path) && in == 0 ? "DH0:" : "DF0:",
                    d.disk[in < d.count ? in : 0].name, disks > 1 ? " (F12, D for the next disk)" : "");
        menu_line(&o, "L", "Load a game from the library", NULL);
        menu_line(&o, "U", "Upload your own disk (ZMODEM)", mine_available() ? "kept for next time" : NULL);
        if (mine_available())
        {
            char kept[MINE_MAX][64];
            snprintf(state, sizeof(state), "%d of %d", mine_list(kept, MINE_MAX), MINE_MAX);
            menu_line(&o, "M", "Your kept disks", state);
        }
        if (disks > 1)
        {
            snprintf(state, sizeof(state), "disk %d of %d", in + 1, disks);
            menu_line(&o, "D", "Next disk into DF0:", state);
        }
        menu_line(&o, "A", "Switch machine (disk out)", d.a1200 ? "A1200 -> A500" : "A500 -> A1200");
        menu_line(&o, "N", "PAL or NTSC (restarts it)", d.ntsc ? "NTSC -> PAL" : "PAL -> NTSC");
        menu_line(&o, "K", "Arrow keys + Ctrl/Alt", g_joy_mode ? "joystick" : "Amiga cursor keys");
        if (g_caps->sound)
            menu_line(&o, "S", "Sound", g_sound_on ? "on" : "off");
        menu_line(&o, "R", "Reset the Amiga", d.count > 0 ? "takes the disk out" : NULL);
        menu_line(&o, "Q", "Leave (back to the BBS)", NULL);
        out_str(&o, "\r\n\033[0;37m  Any other key goes back to the Amiga.\033[0m");
        show(&o);

        c = read_key(600000);
        if (c == -2)
        {
            result = MENU_HANGUP;
            break;
        }
        switch (c)
        {
        case 'l': case 'L':
            result = menu_library(&o);
            done = result != MENU_RESUME;
            break;
        case 'u': case 'U':
            result = menu_upload(&o);
            done = result != MENU_RESUME;
            break;
        case 'm': case 'M':
            if (mine_available())
            {
                result = menu_mine(&o);
                done = result != MENU_RESUME;
            }
            break;
        case 'd': case 'D':
            if (disks > 1)
                core_disk_insert((in + 1) % disks);
            break;
        case 'a': case 'A':
        case 'n': case 'N':
            if (c == 'a' || c == 'A')
            {
                d.a1200 = !d.a1200;
                d.count = 0;                            /* the other machine starts fresh, the disk out (sysop,
                                                         * 2026-10-28): its games are in its own library list */
            }
            else
                d.ntsc = !d.ntsc;
            if (drive_save(&d))
            {
                result = MENU_RELOAD;
                done = true;
            }
            break;
        case 'k': case 'K':
            g_joy_mode = !g_joy_mode;
            break;
        case 's': case 'S':
            if (g_caps->sound)
                g_sound_on = !g_sound_on;
            break;
        case 'r': case 'R':
            /* Back to the Kickstart's insert-disk screen, on the same machine: the disks come out (sysop,
             * 2026-10-28). The Amiga restarts with an empty drive; the game is still in the library or kept disks. */
            d.count = 0;
            if (drive_save(&d))
                result = MENU_RELOAD;
            else
                core_reset();
            done = true;
            break;
        case 'q': case 'Q':
            result = MENU_QUIT;
            done = true;
            break;
        case 27:
            drain_input(50);                            /* an arrow or F-key's sequence: just go back */
            done = true;
            break;
        default:
            done = true;
            break;
        }
    }

    o.len = 0;
    out_str(&o, "\033[0m\033[2J\033[H\033[?25l");
    draw_bar(&o);
    if (g_caps->keys && result == MENU_RESUME)
        out_str(&o, "\033[=1h\033[=2h");
    if (result == MENU_RESUME)
        out_str(&o, g_mouse_px ? MOUSE_ON "\033[?1016h" : MOUSE_ON);
    if (g_caps->sound)
        sound_restart(&o);
    show(&o);
    free(o.data);
    g_have_ref = false;                                 /* the whole picture again */
    g_menu_asked = false;
    return result;
}

/* ---- the session ---- */

/* Lossy while the link can't keep up: decided once a second from the share of frames skipped */
static void choose_quality(long now, long run, long skipped)
{
    static bool behind_before;
    bool behind = run > 0 && (double)skipped / (double)run > LOSSY_SKIP;

    if (g_lossy_distance <= 0 || run == 0)
        return;
    if (g_distance == 0 && behind && !behind_before)
        behind_before = true;
    else if (g_distance == 0 && behind)
    {
        behind_before = false;
        g_distance = g_lossy_distance;
        g_lossy_until = now + g_lossy_hold;
        if (g_lossy_hold < LOSSY_HOLD_MAX)
            g_lossy_hold *= 2;                          /* back again soon after: stay lossy longer next time */
    }
    else if (!behind)
        behind_before = false;
    if (g_distance > 0 && now >= g_lossy_until)
    {
        g_distance = 0;                                 /* try lossless again; a blurred screen is redrawn sharp */
        if (g_blurred)
            g_have_ref = false;
        g_blurred = false;
    }
}

play_result_t play(const play_caps_t *caps, const play_setup_t *setup)
{
    play_result_t result = PLAY_QUIT;
    outbuf_t out = { 0 };
    FILE *log = setup->log != NULL ? fopen(setup->log, "w") : NULL;
    const char *why = NULL;
    double frame_ms;
    long next_emu, stats_at, control_at, quality_at;
    long frames_sent = 0, frames_skipped = 0, bytes = 0, emu_frames = 0, emu_late = 0, drive_frames = 0;
    long lossy_frames = 0, q_run = 0, q_skipped = 0;
    long worst_emu = 0, worst_pic = 0, worst_send = 0;  /* the longest each step took in the last 5 s (ms) */
    long behind_skipped = 0, worst_pic_enc = 0;                            /* pictures not made because the Amiga was behind */
    bool frame_waiting = false;
    const char *dump_dir = getenv("AMIGADOOR_DUMP");
    long dump_n = 0, dump_every = getenv("AMIGADOOR_DUMP_EVERY") != NULL && atol(getenv("AMIGADOOR_DUMP_EVERY")) > 0
                                  ? atol(getenv("AMIGADOOR_DUMP_EVERY")) : 250;
    core_machine_t machine = { setup->a1200, setup->ntsc };

    g_caps = caps;
    g_session_dir = setup->session;
#ifndef _WIN32
    if (setup->conn_fd > 0)
        g_conn = setup->conn_fd;
#endif
    g_sound_on = caps->sound;
    g_ntsc = setup->ntsc;
    g_mouse_px = caps->mouse_px;
    g_small = getenv("AMIGADOOR_SMALL") != NULL;
    if (getenv("AMIGADOOR_LOSSY_DISTANCE") != NULL)
        g_lossy_distance = (float)atof(getenv("AMIGADOOR_LOSSY_DISTANCE"));
    if (getenv("AMIGADOOR_MOUSE_SCALE") != NULL && atoi(getenv("AMIGADOOR_MOUSE_SCALE")) > 0)
        g_mouse_scale = atoi(getenv("AMIGADOOR_MOUSE_SCALE"));
    choose_view(360, setup->ntsc ? 242 : 287);
    if (setup->link_kbps > 0)
    {
        g_shape = true;
        g_shape_rate = setup->link_kbps * 1024.0 / 1000.0;
        g_shape_half_ping = setup->link_ping / 2;
    }
    if (log != NULL)
        fprintf(log, "amiga: %s %s; screen %dx%d, %d rows; showing %dx%d as %dx%d (%s), key reports %s, mouse in %s, "
                "sound %s, pictures %s, lossy distance %.1f when behind%s\n", setup->a1200 ? "A1200" : "A500",
                setup->ntsc ? "NTSC" : "PAL", caps->px_w, caps->px_h, g_bar_row, g_vw, g_vh, g_dw, g_dh,
                g_fill ? "stretched here" : caps->zoom ? "scaled by the terminal" : "pre-scaled here",
                caps->keys ? "on" : "off", caps->mouse_px ? "pixels" : "cells", caps->sound ? "on" : "off",
                caps->blob ? "in the command" : "through the cache", g_lossy_distance, g_shape ? ", LINK TEST" : "");

    if (!core_start(setup->core, setup->system, setup->image, &machine, &why))
    {
        if (log != NULL)
            fprintf(log, "amiga: %s\n", why);
        if (log != NULL)
            fclose(log);
        return PLAY_FAILED;
    }
    frame_ms = 1000.0 / core_fps();
    g_snd_step = core_sample_rate() / SND_RATE;         /* input samples per output sample */
    if (log != NULL)
        fprintf(log, "amiga: core %.3f fps, sound %.0f Hz\n", core_fps(), core_sample_rate());

    /* Colours reset, screen cleared, cursor hidden; key reports on (and the typed copies of keys off), the mouse on */
    out_str(&out, "\033[0m\033[2J\033[H\033[?25l");
    draw_bar(&out);
    if (caps->keys)
        out_str(&out, "\033[=1h\033[=2h");
    out_str(&out, caps->mouse_px ? MOUSE_ON "\033[?1016h" : MOUSE_ON);
    if (caps->sound)
        sound_restart(&out);
    write_all(out.data, out.len);
    crash_guard(true);

    next_emu = stats_at = control_at = quality_at = now_ms();
    if (g_bar_row < 1)
        g_hint_until = next_emu + 5000;   /* the controls row says it; without one, a few seconds on the picture */
    for (;;)
    {
        long now = now_ms();
        long wait = next_emu - now;

        /* Input until the Amiga's next frame is due */
        {
            unsigned char buf[512];
            int n = read_input(buf, sizeof(buf), wait > 0 ? (int)wait : 0);
            if (n < 0)
            {
                result = PLAY_HANGUP;
                break;
            }
            if (n > 0 && g_shape)
                shape_in(buf, n, now_ms());
            else if (n > 0)
                take_input(buf, n, now_ms());
            else
                take_input(NULL, 0, now_ms());
        }
        now = now_ms();
        if (g_shape)
        {
            shape_in_release(now);
            if (!shape_flush(now, false))
            {
                result = PLAY_HANGUP;
                break;
            }
        }
        if (g_quit_asked)
            break;
        if (time(NULL) >= setup->deadline)
        {
            result = PLAY_TIMEOUT;
            break;
        }
        if (g_menu_asked)
        {
            menu_result_t m = session_menu();
            if (m == MENU_QUIT)
                break;
            if (m == MENU_RELOAD)
            {
                result = PLAY_RELOAD;                   /* the door starts the Amiga again with drive.txt */
                break;
            }
            if (m == MENU_HANGUP)
            {
                result = PLAY_HANGUP;
                break;
            }
            next_emu = now_ms();
            continue;
        }

        if (g_fly.count > 0 && now - g_fly.sent[g_fly.head] > STALL_MS)
            fly_reset();

        if (now < next_emu)
            continue;

        /* One frame of Amiga time; a door that fell far behind catches up by skipping time, never by racing */
        if (now - next_emu > 200)
        {
            next_emu = now;
            emu_late++;
        }
        next_emu += (long)lround(frame_ms);
        if (!caps->keys)
        {
            tap_step();
            if (g_typed_joy_until != 0 && now >= g_typed_joy_until)
            {
                g_typed_joy_until = 0;
                sync_joystick(now);
            }
        }
        {
            long t0 = now_ms();
            core_run_frame();
            if (now_ms() - t0 > worst_emu)
                worst_emu = now_ms() - t0;
        }
        emu_frames++;
        q_run++;
        if (core_drive_led())
        {
            g_led_until = now + 600;
            drive_frames++;
        }
        if (dump_dir != NULL && ++dump_n % dump_every == 0)
        {
            /* AMIGADOOR_DUMP=<dir>: a snapshot every 5 s (AMIGADOOR_DUMP_EVERY frames), for debugging */
            char path[600];
            FILE *f;
            snprintf(path, sizeof(path), "%s/f%05ld.ppm", dump_dir, dump_n);
            if ((f = fopen(path, "wb")) != NULL)
            {
                fprintf(f, "P6\n%d %d\n255\n", core_w(), core_h());
                for (int y = 0; y < core_h(); y++)
                    fwrite(core_rgb() + y * CORE_MAX_W * 3, 3, (size_t)core_w(), f);
                fclose(f);
            }
        }
        frame_waiting = true;

        out.len = 0;
        if (core_w() != g_vw || core_h() != g_vh)
        {
            /* The game changed screen mode: laid out again, on a clean screen */
            choose_view(core_w(), core_h());
            out_str(&out, "\033[0m\033[2J\033[H\033[?25l");
            draw_bar(&out);
            if (log != NULL)
                fprintf(log, "amiga: the frame is now %dx%d, shown as %dx%d\n", g_vw, g_vh, g_dw, g_dh);
        }
        if (caps->sound)
        {
            size_t n = sound_update(&out);
            if (n > 0)
            {
                out_str(&out, "\033[6n");
                fly_push(now, out.len);
            }
        }
        if (frame_waiting && now_ms() >= next_emu)
        {
            /* Behind: the next frame is due already. No picture this time (what changed goes with the next one), so
             * the Amiga and its sound keep their time. A big picture can take longer to make than a frame lasts
             * (50 ms seen, against 20), and falling behind lost sound (sysop, 2026-10-28). */
            behind_skipped++;
            frame_waiting = false;
        }
        else if (frame_waiting && window_open())
        {
            size_t before = out.len;
            long t0 = now_ms();
            bool drew;
            g_enc_ms = 0;
            grab_view(now);
            drew = draw_changes(&out);
            if (now_ms() - t0 > worst_pic)
            {
                worst_pic = now_ms() - t0;
                worst_pic_enc = g_enc_ms;
            }
            if (drew)
            {
                out_str(&out, "\033[6n");
                g_frame_avg = g_frame_avg * 0.9 + (double)(out.len - before) * 0.1;
                fly_push(now, out.len - before);
                frames_sent++;
                lossy_frames += g_distance > 0;
                g_still = 0;
            }
            else if (g_blurred && g_distance > 0 && ++g_still >= STILL_FRAMES)
            {
                /* Still, and blurred: the whole picture once more, sharp */
                float d = g_distance;
                g_distance = 0;
                g_have_ref = false;
                g_blurred = false;
                draw_changes(&out);
                out_str(&out, "\033[6n");
                fly_push(now, out.len - before);
                frames_sent++;
                g_distance = d;
                g_still = 0;
            }
            frame_waiting = false;
        }
        else if (frame_waiting)
        {
            frames_skipped++;
            q_skipped++;
        }
        {
            long t0 = now_ms();
            if (!send_out(&out, now))
            {
                result = PLAY_HANGUP;
                break;
            }
            if (now_ms() - t0 > worst_send)
                worst_send = now_ms() - t0;
        }
        bytes += (long)out.len;

        /* The link's speed: what was answered for, trusted fully when more than before, slowly when less */
        if (now - control_at >= RATE_MS)
        {
            double sample = (double)g_acked / (double)(now - control_at);
            g_rate = sample > g_rate ? sample : g_rate * 0.95 + sample * 0.05;
            g_acked = 0;
            g_rtt_sum = g_rtt_n = 0;
            control_at = now;
        }
        if (now - quality_at >= 1000)
        {
            choose_quality(now, q_run, q_skipped);
            q_run = q_skipped = 0;
            quality_at = now;
        }

        if (log != NULL && now - stats_at >= 5000)
        {
            long ms = now - stats_at;
            fprintf(log, "amiga: %ld frames run, %ld sent (%ld fps, %ld lossy), %ld skipped, %ld KB/s, rtt floor %ld ms, "
                    "link %.0f KB/s, sound pieces %ld, late %ld, drive busy %ld frames%s\n", emu_frames, frames_sent,
                    frames_sent * 1000 / ms, lossy_frames, frames_skipped, (long)(bytes / 1.024 / ms), rtt_min(),
                    g_rate * 1000 / 1024, g_snd_pieces, emu_late, drive_frames, "");
            if (g_shape)
                fprintf(log, "amiga: link test: %ld ms queued to go out, %d chunks, %d inputs held; in flight %d (%zu bytes); "
                        "asked %ld, answered %ld, orphans %d, stalls %ld\n", (long)(g_shape_free - now), g_shape_count,
                        g_in_count, g_fly.count, g_fly.total, g_pushes, g_acks, g_orphans, g_resets);
            fprintf(log, "amiga: longest emulating %ld ms, making a picture %ld ms (%ld of it encoding), sending %ld ms; "
                    "%ld pictures left out to keep time\n", worst_emu, worst_pic, worst_pic_enc, worst_send, behind_skipped);
            worst_emu = worst_pic = worst_send = behind_skipped = 0;
            fflush(log);
            frames_sent = frames_skipped = bytes = emu_frames = emu_late = drive_frames = lossy_frames = 0;
            g_snd_pieces = 0;
            stats_at = now;
        }
    }

    release_all(now_ms());
    if (g_shape)
        shape_flush(now_ms(), true);
    out.len = 0;
    if (caps->sound)
        apc_cmd(&out, "A;Flush;C=%d", SND_CH);
    out_str(&out, MOUSE_OFF);
    if (caps->keys)
        out_str(&out, "\033[=2l\033[=1l");
    out_str(&out, "\033[0m\033[2J\033[H\033[?25h");
    write_all(out.data, out.len);
    core_stop();
    crash_guard(false);
    free(out.data);
    drain_input(200);
    if (log != NULL)
        fclose(log);
    return result;
}
