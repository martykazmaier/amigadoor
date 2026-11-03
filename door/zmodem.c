/*
 * zmodem.c -- ZMODEM, the part of it the door needs: receive the caller's disks, send back a changed one.
 *
 * On Linux the door hands the connection to lrzsz's rz and sz; Windows has neither, and the door is the telnet end
 * itself there (win32.c), so this speaks ZMODEM through the door's own reading and writing.
 *
 *   Headers    hex (ZRINIT, ZRPOS, ZACK, ZFIN... from the receiving side, as rz sends them) or binary with CRC-16 or
 *              CRC-32 (ZFILE, ZDATA, ZEOF from the sending side: CRC-32 when the receiver says it can).
 *   Data       subpackets of up to 1024 bytes, ZDLE-escaped, each ended by ZCRCE/ZCRCG/ZCRCQ/ZCRCW and its CRC.
 *              Sending streams them (ZCRCG) and watches for a ZRPOS coming back between them.
 *   Escaping   EVERY control character, both ways (the receiver asks the sender for it with ESCCTL). A raw CR on a
 *              telnet link can come back with a NUL after it (CR NUL is how telnet sends a bare CR outside binary
 *              mode), and the telnet layer drops that NUL; with no raw CR ever sent, that can't eat a byte of a disk.
 *              Costs a disk image about a third more on the wire.
 *   Cancel     five CANs (Ctrl-X) in a row, from either side, end it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "zmodem.h"

#define ZPAD   '*'
#define ZDLE   0x18
#define ZBIN   'A'
#define ZHEX   'B'
#define ZBIN32 'C'

enum { ZRQINIT, ZRINIT, ZSINIT, ZACK, ZFILE, ZSKIP, ZNAK, ZABORT, ZFIN, ZRPOS, ZDATA, ZEOF, ZFERR, ZCRC, ZCHALLENGE,
       ZCOMPL, ZCAN, ZFREECNT, ZCOMMAND };

#define ZCRCE 'h'                       /* end of frame, header follows, no answer wanted */
#define ZCRCG 'i'                       /* more data follows, no answer */
#define ZCRCQ 'j'                       /* more data follows, ZACK wanted */
#define ZCRCW 'k'                       /* end of frame, ZACK wanted */
#define ZRUB0 'l'                       /* 0x7f */
#define ZRUB1 'm'                       /* 0xff */

/* ZRINIT's flags, in ZF0 (the header's fourth byte) */
#define CANFDX  0x01
#define CANOVIO 0x02
#define CANFC32 0x20
#define ESCCTL  0x40

/* What the reading functions return besides a byte or a header type */
#define R_TIMEOUT  (-1)
#define R_GONE     (-2)
#define R_CANCEL   (-3)
#define R_BADCRC   (-4)
#define R_GARBAGE  (-5)
#define FRAME_END  0x100                /* zdl_get: ZDLE then a frame end, as FRAME_END | ZCRCx */

#define BLOCK      1024
#define WAIT_MS    10000                /* for each header */
#define START_MS   120000               /* for the caller to pick a file and start */

static const zm_io_t *g_io;
static unsigned char  g_in[4096];
static int            g_in_len, g_in_pos;
static bool           g_gone;

/* ---- CRCs ---- */

static unsigned short crc16_byte(unsigned short crc, unsigned char c)
{
    crc ^= (unsigned short)(c << 8);
    for (int i = 0; i < 8; i++)
        crc = (unsigned short)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    return crc;
}

static unsigned long crc32_byte(unsigned long crc, unsigned char c)
{
    crc ^= c;
    for (int i = 0; i < 8; i++)
        crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
    return crc;
}

/* ---- the connection ---- */

static int get_byte(int timeout_ms)
{
    if (g_in_pos < g_in_len)
        return g_in[g_in_pos++];
    if (g_gone)
        return R_GONE;
    g_in_len = g_io->read(g_in, sizeof(g_in), timeout_ms);
    g_in_pos = 0;
    if (g_in_len < 0)
    {
        g_in_len = 0;
        g_gone = true;
        return R_GONE;
    }
    if (g_in_len == 0)
        return R_TIMEOUT;
    return g_in[g_in_pos++];
}

/* Whether the other side has said anything, without waiting */
static bool input_waiting(void)
{
    if (g_in_pos < g_in_len)
        return true;
    if (g_gone)
        return false;
    g_in_len = g_io->read(g_in, sizeof(g_in), 0);
    g_in_pos = 0;
    if (g_in_len < 0)
    {
        g_in_len = 0;
        g_gone = true;
    }
    return g_in_len > 0;
}

static unsigned char g_out[2 * BLOCK + 64];
static size_t        g_out_len;

static bool flush_out(void)
{
    bool ok = g_out_len == 0 || g_io->write(g_out, g_out_len);
    g_out_len = 0;
    if (!ok)
        g_gone = true;
    return ok;
}

static void put_raw(unsigned char c)
{
    if (g_out_len == sizeof(g_out))
        flush_out();
    g_out[g_out_len++] = c;
}

/* One byte, escaped: every control character (0x00-0x1f, 0x80-0x9f), 0x7f and 0xff */
static void put_esc(unsigned char c)
{
    if ((c & 0x60) == 0)
    {
        put_raw(ZDLE);
        put_raw(c ^ 0x40);
    }
    else if (c == 0x7f)
    {
        put_raw(ZDLE);
        put_raw(ZRUB0);
    }
    else if (c == 0xff)
    {
        put_raw(ZDLE);
        put_raw(ZRUB1);
    }
    else
        put_raw(c);
}

/* ---- headers ---- */

static void pos_to_hdr(unsigned char hdr[4], long pos)
{
    hdr[0] = (unsigned char)(pos & 0xff);
    hdr[1] = (unsigned char)((pos >> 8) & 0xff);
    hdr[2] = (unsigned char)((pos >> 16) & 0xff);
    hdr[3] = (unsigned char)((pos >> 24) & 0xff);
}

static long hdr_to_pos(const unsigned char hdr[4])
{
    return (long)hdr[0] | (long)hdr[1] << 8 | (long)hdr[2] << 16 | (long)hdr[3] << 24;
}

static bool send_hex_header(int type, const unsigned char hdr[4])
{
    static const char HEX[] = "0123456789abcdef";
    unsigned short crc = crc16_byte(0, (unsigned char)type);
    unsigned char bytes[7];

    bytes[0] = (unsigned char)type;
    memcpy(bytes + 1, hdr, 4);
    for (int i = 0; i < 4; i++)
        crc = crc16_byte(crc, hdr[i]);
    bytes[5] = (unsigned char)(crc >> 8);
    bytes[6] = (unsigned char)(crc & 0xff);
    put_raw(ZPAD);
    put_raw(ZPAD);
    put_raw(ZDLE);
    put_raw(ZHEX);
    for (int i = 0; i < 7; i++)
    {
        put_raw((unsigned char)HEX[bytes[i] >> 4]);
        put_raw((unsigned char)HEX[bytes[i] & 15]);
    }
    put_raw('\r');
    put_raw(0x8a);                      /* LF with the top bit set, as rz sends it */
    if (type != ZFIN && type != ZACK)
        put_raw(0x11);                  /* XON, in case anything on the way stopped for an XOFF */
    return flush_out();
}

static bool send_hex_pos(int type, long pos)
{
    unsigned char hdr[4];
    pos_to_hdr(hdr, pos);
    return send_hex_header(type, hdr);
}

static bool send_bin_header(int type, const unsigned char hdr[4], bool crc32)
{
    put_raw(ZPAD);
    put_raw(ZDLE);
    put_raw(crc32 ? ZBIN32 : ZBIN);
    put_esc((unsigned char)type);
    for (int i = 0; i < 4; i++)
        put_esc(hdr[i]);
    if (crc32)
    {
        unsigned long crc = crc32_byte(0xFFFFFFFFUL, (unsigned char)type);
        for (int i = 0; i < 4; i++)
            crc = crc32_byte(crc, hdr[i]);
        crc = ~crc;
        for (int i = 0; i < 4; i++)
            put_esc((unsigned char)((crc >> (8 * i)) & 0xff));
    }
    else
    {
        unsigned short crc = crc16_byte(0, (unsigned char)type);
        for (int i = 0; i < 4; i++)
            crc = crc16_byte(crc, hdr[i]);
        put_esc((unsigned char)(crc >> 8));
        put_esc((unsigned char)(crc & 0xff));
    }
    return flush_out();
}

static bool send_bin_pos(int type, long pos, bool crc32)
{
    unsigned char hdr[4];
    pos_to_hdr(hdr, pos);
    return send_bin_header(type, hdr, crc32);
}

/* A byte as it was before escaping, or FRAME_END | ZCRCx, or R_* */
static int zdl_get(int timeout_ms)
{
    int c;

    for (;;)
    {
        c = get_byte(timeout_ms);
        if (c < 0)
            return c;
        if (c == 0x11 || c == 0x13 || c == 0x91 || c == 0x93)
            continue;                   /* XON/XOFF: never data, since every control character is escaped */
        if (c != ZDLE)
            return c;
        break;
    }
    for (;;)
    {
        c = get_byte(timeout_ms);
        if (c < 0)
            return c;
        if (c == 0x11 || c == 0x13 || c == 0x91 || c == 0x93)
            continue;
        break;
    }
    if (c == ZDLE)
    {
        /* ZDLE then four more CANs: cancelled */
        for (int n = 2; n < 5; n++)
            if ((c = get_byte(timeout_ms)) != ZDLE)
                return c < 0 ? c : R_GARBAGE;
        return R_CANCEL;
    }
    if (c >= ZCRCE && c <= ZCRCW)
        return FRAME_END | c;
    if (c == ZRUB0)
        return 0x7f;
    if (c == ZRUB1)
        return 0xff;
    if ((c & 0x60) == 0x40)
        return c ^ 0x40;
    return R_GARBAGE;
}

static int hex_digit(int timeout_ms)
{
    int c = get_byte(timeout_ms);
    if (c < 0)
        return c;
    c &= 0x7f;
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return R_GARBAGE;
}

/* The next header: its type with its four bytes in hdr, or R_*. Skips whatever comes before it. *crc32 says whether
 * it came with CRC-32, which then goes for the data after it too. */
static int get_header(unsigned char hdr[4], int timeout_ms, bool *crc32)
{
    int garbage = 0, cans = 0, c;
    unsigned char bytes[9];

    for (;;)
    {
        c = get_byte(timeout_ms);
        if (c < 0)
            return c;
        if (c == ZDLE)
        {
            if (++cans >= 5)
                return R_CANCEL;
        }
        else
            cans = 0;
        if ((c & 0x7f) != ZPAD)
        {
            if (++garbage > 8192)
                return R_GARBAGE;
            continue;
        }
        while ((c = get_byte(timeout_ms)) >= 0 && (c & 0x7f) == ZPAD)
            ;
        if (c < 0)
            return c;
        if (c != ZDLE)
            continue;
        if ((c = get_byte(timeout_ms)) < 0)
            return c;
        if (c == ZHEX)
        {
            unsigned short crc = 0;
            for (int i = 0; i < 7; i++)
            {
                int hi = hex_digit(timeout_ms), lo;
                if (hi < 0)
                    return hi == R_GARBAGE ? R_BADCRC : hi;
                if ((lo = hex_digit(timeout_ms)) < 0)
                    return lo == R_GARBAGE ? R_BADCRC : lo;
                bytes[i] = (unsigned char)(hi << 4 | lo);
                crc = crc16_byte(crc, bytes[i]);
            }
            if (crc != 0)
                return R_BADCRC;
            if (crc32 != NULL)
                *crc32 = false;
        }
        else if (c == ZBIN || c == ZBIN32)
        {
            int n = c == ZBIN32 ? 9 : 7;
            for (int i = 0; i < n; i++)
            {
                int b = zdl_get(timeout_ms);
                if (b < 0)
                    return b == R_GARBAGE ? R_BADCRC : b;
                if (b & FRAME_END)
                    return R_BADCRC;
                bytes[i] = (unsigned char)b;
            }
            if (c == ZBIN32)
            {
                unsigned long crc = 0xFFFFFFFFUL;
                for (int i = 0; i < 9; i++)
                    crc = crc32_byte(crc, bytes[i]);
                if (crc != 0xDEBB20E3UL)
                    return R_BADCRC;
            }
            else
            {
                unsigned short crc = 0;
                for (int i = 0; i < 7; i++)
                    crc = crc16_byte(crc, bytes[i]);
                if (crc != 0)
                    return R_BADCRC;
            }
            if (crc32 != NULL)
                *crc32 = c == ZBIN32;
        }
        else
            continue;
        memcpy(hdr, bytes + 1, 4);
        return bytes[0];
    }
}

/* One data subpacket into buf: its frame end (ZCRCx) with *len set, or R_* */
static int get_data(unsigned char *buf, int max, int *len, bool crc32)
{
    unsigned short crc16 = 0;
    unsigned long crc = 0xFFFFFFFFUL;
    int n = 0, c, end;

    *len = 0;
    for (;;)
    {
        c = zdl_get(WAIT_MS);
        if (c < 0)
            return c;
        if (c & FRAME_END)
            break;
        if (n == max)
            return R_GARBAGE;
        buf[n++] = (unsigned char)c;
        if (crc32)
            crc = crc32_byte(crc, (unsigned char)c);
        else
            crc16 = crc16_byte(crc16, (unsigned char)c);
    }
    end = c & 0xff;
    if (crc32)
    {
        crc = crc32_byte(crc, (unsigned char)end);
        for (int i = 0; i < 4; i++)
        {
            if ((c = zdl_get(WAIT_MS)) < 0 || (c & FRAME_END))
                return c < 0 && c != R_GARBAGE ? c : R_BADCRC;
            crc = crc32_byte(crc, (unsigned char)c);
        }
        if (crc != 0xDEBB20E3UL)
            return R_BADCRC;
    }
    else
    {
        crc16 = crc16_byte(crc16, (unsigned char)end);
        for (int i = 0; i < 2; i++)
        {
            if ((c = zdl_get(WAIT_MS)) < 0 || (c & FRAME_END))
                return c < 0 && c != R_GARBAGE ? c : R_BADCRC;
            crc16 = crc16_byte(crc16, (unsigned char)c);
        }
        if (crc16 != 0)
            return R_BADCRC;
    }
    *len = n;
    return end;
}

static void put_data(const unsigned char *data, int len, int end, bool crc32)
{
    unsigned short crc16 = 0;
    unsigned long crc = 0xFFFFFFFFUL;

    for (int i = 0; i < len; i++)
    {
        put_esc(data[i]);
        if (crc32)
            crc = crc32_byte(crc, data[i]);
        else
            crc16 = crc16_byte(crc16, data[i]);
    }
    put_raw(ZDLE);
    put_raw((unsigned char)end);
    if (crc32)
    {
        crc = ~crc32_byte(crc, (unsigned char)end);
        for (int i = 0; i < 4; i++)
            put_esc((unsigned char)((crc >> (8 * i)) & 0xff));
    }
    else
    {
        crc16 = crc16_byte(crc16, (unsigned char)end);
        put_esc((unsigned char)(crc16 >> 8));
        put_esc((unsigned char)(crc16 & 0xff));
    }
    if (end == ZCRCW)
        put_raw(0x11);
}

/* Tells the other side to stop: eight CANs, then backspaces over them (as rz and sz do) */
static void send_cancel(void)
{
    for (int i = 0; i < 8; i++)
        put_raw(ZDLE);
    for (int i = 0; i < 8; i++)
        put_raw(8);
    flush_out();
}

static void start(const zm_io_t *io)
{
    g_io = io;
    g_in_len = g_in_pos = 0;
    g_out_len = 0;
    g_gone = false;
}

/* ---- receiving ---- */

/* The file's name from a ZFILE: the part after any folder, and only characters any file system takes */
static bool clean_name(const char *in, char *out, size_t size)
{
    const char *base = in;
    size_t n = 0;

    for (const char *p = in; *p != '\0'; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;
    for (const char *p = base; *p != '\0' && n < size - 1; p++)
    {
        unsigned char c = (unsigned char)*p;
        out[n++] = c > 32 && c < 127 && strchr("<>\"|?*", c) == NULL ? (char)c : '_';
    }
    out[n] = '\0';
    return n > 0 && out[0] != '.';
}

static void send_zrinit(void)
{
    unsigned char hdr[4] = { 0, 0, 0, CANFDX | CANOVIO | CANFC32 | ESCCTL };
    send_hex_header(ZRINIT, hdr);
}

/* One file's data, after its ZFILE: 1 arrived, 0 not (skipped or broken), or R_* to stop */
static int receive_file(FILE *f, long max_size)
{
    static unsigned char buf[BLOCK + 8];
    unsigned char hdr[4];
    long pos = 0;
    int errors = 0;
    bool crc32 = false;

    send_hex_pos(ZRPOS, 0);
    while (errors < 20)
    {
        int type = get_header(hdr, WAIT_MS, &crc32);
        if (type == R_GONE || type == R_CANCEL)
            return type;
        if (type < 0)
        {
            errors++;
            send_hex_pos(ZRPOS, pos);
            continue;
        }
        switch (type)
        {
        case ZDATA:
            if (hdr_to_pos(hdr) != pos)
            {
                errors++;
                send_hex_pos(ZRPOS, pos);
                break;
            }
            for (;;)
            {
                int len, end = get_data(buf, BLOCK, &len, crc32);
                if (end == R_GONE || end == R_CANCEL)
                    return end;
                if (end < 0)
                {
                    errors++;
                    send_hex_pos(ZRPOS, pos);
                    break;
                }
                if (pos + len > max_size)
                {
                    send_cancel();
                    return 0;
                }
                if (fwrite(buf, 1, (size_t)len, f) != (size_t)len)
                {
                    send_cancel();
                    return 0;
                }
                pos += len;
                errors = 0;
                if (end == ZCRCW || end == ZCRCQ)
                    send_hex_pos(ZACK, pos);
                if (end == ZCRCW || end == ZCRCE)
                    break;
            }
            break;
        case ZEOF:
            if (hdr_to_pos(hdr) == pos)
                return 1;
            break;                      /* a ZEOF for data that went missing: the ZRPOS already sent fixes it */
        case ZFILE:
            /* our ZRPOS was lost: the ZFILE again, with its data subpacket */
            {
                int len;
                get_data(buf, BLOCK, &len, crc32);
            }
            send_hex_pos(ZRPOS, pos);
            break;
        case ZFIN:
        case ZABORT:
        case ZFERR:
            return R_CANCEL;
        default:
            break;
        }
    }
    send_cancel();
    return 0;
}

int zm_receive(const zm_io_t *io, const char *dir, long max_size, int max_files)
{
    static unsigned char info[BLOCK + 8];
    unsigned char hdr[4];
    int files = 0, waited = 0;
    bool crc32 = false;

    start(io);
    send_zrinit();
    for (;;)
    {
        int type = get_header(hdr, WAIT_MS, &crc32);

        if (type == R_GONE)
            return ZM_HUNG_UP;
        if (type == R_CANCEL)
            return files;
        if (type == R_TIMEOUT)
        {
            waited += WAIT_MS;
            /* Long enough to pick a file at the start; after a file, the sender's ZFIN may just have been lost */
            if (waited >= (files > 0 ? 2 * WAIT_MS : START_MS))
            {
                send_cancel();
                return g_gone ? ZM_HUNG_UP : files;
            }
            send_zrinit();
            continue;
        }
        if (type < 0)
        {
            send_zrinit();
            continue;
        }
        waited = 0;
        switch (type)
        {
        case ZRQINIT:
            send_zrinit();
            break;
        case ZSINIT:
            {
                int len;
                if (get_data(info, BLOCK, &len, crc32) >= 0)
                    send_hex_pos(ZACK, 1);
                else
                    send_hex_header(ZNAK, (const unsigned char[4]){ 0 });
            }
            break;
        case ZFILE:
            {
                char name[64], path[700];
                long size = 0;
                int len, end = get_data(info, BLOCK, &len, crc32), got;
                FILE *f;
                struct stat st;

                if (end == R_GONE)
                    return ZM_HUNG_UP;
                if (end == R_CANCEL)
                    return files;
                if (end < 0)
                {
                    send_hex_header(ZNAK, (const unsigned char[4]){ 0 });
                    break;
                }
                info[len < BLOCK ? len : BLOCK] = '\0';
                {
                    size_t nlen = strlen((char *)info);
                    if ((int)nlen + 1 < len)
                        size = atol((char *)info + nlen + 1);
                }
                if (files >= max_files || size > max_size || !clean_name((char *)info, name, sizeof(name)))
                {
                    send_hex_pos(ZSKIP, 0);
                    break;
                }
                snprintf(path, sizeof(path), "%s/%s", dir, name);
                for (int i = 1; stat(path, &st) == 0 && i < 100; i++)
                    snprintf(path, sizeof(path), "%s/%s.%d", dir, name, i);
                if ((f = fopen(path, "wb")) == NULL)
                {
                    send_hex_pos(ZSKIP, 0);
                    break;
                }
                got = receive_file(f, max_size);
                fclose(f);
                if (got != 1)
                    remove(path);
                if (got == R_GONE)
                    return ZM_HUNG_UP;
                if (got == R_CANCEL)
                    return files;
                if (got == 1)
                    files++;
                send_zrinit();
            }
            break;
        case ZFIN:
            send_hex_pos(ZFIN, 0);
            /* "OO" ends it; don't wait long for it */
            for (int i = 0; i < 2 && get_byte(1000) >= 0; i++)
                ;
            return g_gone ? ZM_HUNG_UP : files;
        case ZCOMMAND:
            {
                int len;
                get_data(info, BLOCK, &len, crc32);
                send_hex_pos(ZCOMPL, 0);    /* never run */
            }
            break;
        default:
            send_zrinit();
            break;
        }
    }
}

/* ---- sending ---- */

/* Waits for the receiver's answer to something just sent (a damaged header doesn't count); R_TIMEOUT if none */
static int answer(unsigned char hdr[4], int timeout_ms)
{
    for (;;)
    {
        int type = get_header(hdr, timeout_ms, NULL);
        if (type == R_BADCRC || type == R_GARBAGE)
            continue;
        return type;
    }
}

int zm_send(const zm_io_t *io, const char *path)
{
    static unsigned char block[BLOCK];
    unsigned char hdr[4], info[300];
    const char *name = path;
    bool crc32 = false, skipped = false;
    long size, pos, last_rpos, acked;
    int type = R_TIMEOUT, tries, ilen;
    FILE *f;

    for (const char *p = path; *p != '\0'; p++)
        if (*p == '/' || *p == '\\')
            name = p + 1;
    if ((f = fopen(path, "rb")) == NULL)
        return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    start(io);

    /* "rz", then ZRQINIT until the receiver answers with ZRINIT */
    put_raw('r'), put_raw('z'), put_raw('\r');
    for (tries = 0; tries < 6; tries++)
    {
        send_hex_pos(ZRQINIT, 0);
        type = answer(hdr, WAIT_MS);
        while (type == ZCHALLENGE || type == ZRQINIT || type == ZCOMMAND)
        {
            if (type == ZCHALLENGE)
                send_hex_header(ZACK, hdr);
            type = answer(hdr, WAIT_MS);
        }
        if (type == ZRINIT || type == R_GONE || type == R_CANCEL)
            break;
    }
    if (type != ZRINIT)
        goto fail;
    crc32 = (hdr[3] & CANFC32) != 0;

    /* The file: name, then its size */
    ilen = snprintf((char *)info, sizeof(info), "%s", name) + 1;
    ilen += snprintf((char *)info + ilen, sizeof(info) - (size_t)ilen, "%ld", size) + 1;
    for (tries = 0; tries < 10; tries++)
    {
        unsigned char zf[4] = { 0, 0, 0, 1 };   /* ZF0 = ZCBIN: binary */
        send_bin_header(ZFILE, zf, crc32);
        put_data(info, ilen, ZCRCW, crc32);
        flush_out();
        type = answer(hdr, WAIT_MS);
        /* A ZRINIT is likely the answer to an earlier ZRQINIT: if something follows it, that's the answer; if all goes
         * quiet after it, the receiver missed the ZFILE, so round again */
        for (int next; type == ZRINIT && (next = answer(hdr, 1000)) != R_TIMEOUT; )
            type = next;
        if (type == ZRINIT)
            continue;
        if (type == ZRPOS || type == ZSKIP || type == R_GONE || type == R_CANCEL || type == ZABORT || type == ZFERR)
            break;
    }
    if (type == ZSKIP)
    {
        fclose(f);
        skipped = true;
        goto finish;
    }
    if (type != ZRPOS)
        goto fail;
    pos = last_rpos = acked = hdr_to_pos(hdr);

    /* The data, streamed, from wherever the receiver asks; then ZEOF, until it answers ZRINIT. As sz does it: the first
     * frame from a new place is a ZCRCW, and the rest only stream once the receiver has answered it, so a burst of
     * stale ZRPOS (one for each frame that was already on its way) can't keep restarting it. tries counts the ZRPOS
     * that came with no progress since the one before. */
    for (tries = 0; tries < 20; )
    {
        bool restart = false, synced = false;
        if (pos > size)
            pos = size;
        fseek(f, pos, SEEK_SET);
        send_bin_pos(ZDATA, pos, crc32);
        for (;;)
        {
            int n = (int)fread(block, 1, BLOCK, f), end = pos + n >= size ? ZCRCE : synced ? ZCRCG : ZCRCW;
            put_data(block, n, end, crc32);
            pos += n;
            if (!flush_out())
                goto fail;
            type = -100;
            if (end == ZCRCW)
            {
                do
                    type = answer(hdr, WAIT_MS);
                while (type == ZACK && hdr_to_pos(hdr) != pos);
                if (type == ZACK)
                {
                    synced = true;
                    acked = pos;
                    send_bin_pos(ZDATA, pos, crc32);    /* a ZCRCW ended the frame: a new one for the stream */
                    continue;
                }
                if (type == R_TIMEOUT || type == ZNAK)
                {
                    pos -= n;           /* that frame again */
                    tries++;
                    restart = true;
                    break;
                }
            }
            else if (input_waiting())
            {
                type = answer(hdr, 2000);
                if (type == ZRPOS && hdr_to_pos(hdr) < acked)
                    type = -100;        /* from before the receiver said it had that: stale */
                if (type == ZRPOS)
                    put_data(block, 0, ZCRCE, crc32);   /* end the frame we were in before starting over */
            }
            if (type == ZRPOS)
            {
                pos = hdr_to_pos(hdr);
                tries = pos > last_rpos ? 0 : tries + 1;
                last_rpos = pos;
                restart = true;
                break;
            }
            if (type == R_GONE || type == R_CANCEL || type == ZSKIP || type == ZABORT || type == ZFERR)
                goto fail;
            if (pos >= size)
                break;
        }
        if (restart)
            continue;
        send_bin_pos(ZEOF, pos, crc32);
        type = answer(hdr, WAIT_MS);
        while (type == ZACK)
            type = answer(hdr, WAIT_MS);
        if (type == ZRINIT)
            break;
        if (type == ZRPOS)
        {
            pos = hdr_to_pos(hdr);
            tries = pos > last_rpos ? 0 : tries + 1;
            last_rpos = pos;
            continue;
        }
        if (type == R_GONE || type == R_CANCEL || type == ZSKIP || type == ZABORT || type == ZFERR)
            goto fail;
        tries++;                        /* nothing, or a ZNAK: ZDATA from where we are (an empty frame), ZEOF again */
    }
    fclose(f);
    if (tries >= 20)
    {
        send_cancel();
        return g_gone ? ZM_HUNG_UP : 0;
    }

finish:
    /* ZFIN both ways, then "OO" */
    for (tries = 0; tries < 3; tries++)
    {
        send_hex_pos(ZFIN, 0);
        type = answer(hdr, 5000);
        if (type == ZFIN || type == R_GONE || type == R_CANCEL)
            break;
    }
    if (type == ZFIN)
    {
        put_raw('O'), put_raw('O');
        flush_out();
    }
    if (g_gone)
        return ZM_HUNG_UP;
    return skipped ? 0 : 1;

fail:
    fclose(f);
    if (type != R_CANCEL && !g_gone)
        send_cancel();
    return g_gone ? ZM_HUNG_UP : 0;
}
