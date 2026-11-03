/*
 * a1200spike.c -- spike A for the Amiga 1200 door: how many bytes a second does an A1200 cost as JPEG XL tiles, and
 * how much CPU does the emulation itself take?
 *
 * The C64 spike (C64Door/spike/c64spike.c) with PUAE (libretro/libretro-uae) in place of VICE. Runs the core headless
 * for a number of frames, with a scripted joystick, and pushes every frame through the Doom door's JPEG XL tile
 * scheme: 32x8 tiles, the whole frame when more than half changed, otherwise runs of changed tiles merged downwards
 * into rectangles. Each picture costs its JPEG XL bytes plus about 40 bytes of APC wrapping.
 *
 * Unlike the C64 the Amiga's frame changes size as the game changes screen mode (low/high resolution, interlace,
 * overscan), so the frame is whatever the core sends; a frame of a new size goes whole.
 *
 * Several configurations are measured side by side on the same frames:
 *   distance JPEG XL distance 0 (lossless), 1, 3
 *   skip     every frame (50 fps), every 2nd (25), every 4th (12.5); a skipped frame's changes go with the next one
 * The core's own video options (resolution, crop) are set per run with A1200SPIKE_OPTS, e.g.
 *   A1200SPIKE_OPTS="puae_video_resolution=lores,puae_crop=auto"
 *
 * Usage: a1200spike <core.so> <system dir> <image> <frames> <script> [ppm-dir]
 *   script: none | fire (fire pulses) | game (fire pulses for 20 s, then hold right with fire pulses)
 *           | swap@N (at frame N, the next disk of an .m3u list goes in)
 *   ppm-dir: when given, every 500th frame (A1200SPIKE_PPM_EVERY) is written there as a PPM, to check by eye.
 */

#define _POSIX_C_SOURCE 200809L

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jxl_enc.h"
#include "libretro.h"

#define FW 1440                     /* the largest frame the core sends: super-high resolution, interlaced */
#define FH 576
#define TW 32
#define TH 8
#define APC_BYTES 40                /* the DrawJXLBlob command around each picture */
#define FRAME_BYTES 10              /* the cursor-position request after each frame */

static const char *g_sysdir;
static bool g_jxl;                  /* false: no libjxl here, rectangles are costed at 4 bits a pixel (debugging only) */
static uint8_t g_rgb[FH * FW * 3];  /* the latest frame, RGB24, rows FW apart */
static int g_fw, g_fh;              /* what the core sent last */
static enum retro_pixel_format g_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint16_t g_pad;              /* RetroPad bits for port 0 */

/* The sizes the core sent, and for how many frames, to see how often a game changes screen mode */
#define MAX_SIZES 16
static struct { int w, h; long frames; } g_sizes[MAX_SIZES];
static int g_nsizes;

/* ---- one measured configuration ---- */

typedef struct
{
    const char *name;
    float distance;
    int skip;
    uint8_t *ref;                   /* what the caller has */
    int ref_w, ref_h;               /* its size; 0 before the first picture */
    double bytes, enc_ms;
    long frames_sent, full_frames, rects;
    double max_frame;
} config_t;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static size_t send_rect(config_t *c, const uint8_t *img, int w_img, int x, int y, int w, int h)
{
    const uint8_t *out;
    size_t n = g_jxl ? jxl_enc_rgb(img + (y * w_img + x) * 3, w_img * 3, w, h, c->distance, &out) : (size_t)w * h / 2;
    c->rects++;
    return n + APC_BYTES;
}

static void measure(config_t *c, long frame)
{
    int w = g_fw, h = g_fh, tx_n = (w + TW - 1) / TW, ty_n = (h + TH - 1) / TH;
    static uint8_t img[FH * FW * 3];
    static bool tiles[FH / TH][FW / TW];
    bool resized = w != c->ref_w || h != c->ref_h;
    int count = 0;
    size_t bytes = FRAME_BYTES;
    double t0;

    if (frame % c->skip != 0 || w == 0)
        return;

    for (int y = 0; y < h; y++)
        memcpy(img + y * w * 3, g_rgb + y * FW * 3, w * 3);

    for (int ty = 0; ty < ty_n; ty++)
        for (int tx = 0; tx < tx_n; tx++)
        {
            bool changed = resized;
            int x0 = tx * TW, tw = (x0 + TW <= w ? TW : w - x0);
            for (int y = ty * TH; y < (ty + 1) * TH && y < h && !changed; y++)
                changed = memcmp(img + (y * w + x0) * 3, c->ref + (y * w + x0) * 3, tw * 3) != 0;
            tiles[ty][tx] = changed;
            count += changed;
        }
    memcpy(c->ref, img, (size_t)w * h * 3);
    c->ref_w = w;
    c->ref_h = h;
    if (count == 0)
        return;

    t0 = now_ms();
    if (count > 0.5 * tx_n * ty_n)
    {
        bytes += send_rect(c, img, w, 0, 0, w, h);
        c->full_frames++;
    }
    else
        for (int ty = 0; ty < ty_n; ty++)
            for (int tx = 0; tx < tx_n; )
            {
                int x0, x1, ty1;
                if (!tiles[ty][tx])
                {
                    tx++;
                    continue;
                }
                x0 = tx;
                while (tx < tx_n && tiles[ty][tx])
                    tx++;
                x1 = tx;
                for (ty1 = ty + 1; ty1 < ty_n; ty1++)
                {
                    bool same = (x0 == 0 || !tiles[ty1][x0 - 1]) && (x1 == tx_n || !tiles[ty1][x1]);
                    for (int k = x0; k < x1 && same; k++)
                        same = tiles[ty1][k];
                    if (!same)
                        break;
                    for (int k = x0; k < x1; k++)
                        tiles[ty1][k] = false;
                }
                {
                    int px = x0 * TW, py = ty * TH;
                    int pw = (x1 * TW <= w ? x1 * TW : w) - px;
                    int ph = (ty1 * TH <= h ? ty1 * TH : h) - py;
                    bytes += send_rect(c, img, w, px, py, pw, ph);
                }
            }
    c->enc_ms += now_ms() - t0;
    c->bytes += bytes;
    c->frames_sent++;
    if (bytes > c->max_frame)
        c->max_frame = bytes;
}

/* ---- libretro frontend ---- */

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    if (level < RETRO_LOG_WARN)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* The spike's own choices; A1200SPIKE_OPTS ("key=value,key=value") adds to and overrides them */
#define MAX_SET 64
static char *g_set_key[MAX_SET], *g_set_val[MAX_SET];
static int g_nset;

static void set_option(const char *key, const char *val)
{
    for (int i = 0; i < g_nset; i++)
        if (strcmp(g_set_key[i], key) == 0)
        {
            g_set_val[i] = strdup(val);
            return;
        }
    if (g_nset < MAX_SET)
    {
        g_set_key[g_nset] = strdup(key);
        g_set_val[g_nset++] = strdup(val);
    }
}

static void init_options(void)
{
    const char *env = getenv("A1200SPIKE_OPTS");
    set_option("puae_model", "A1200");
    set_option("puae_video_standard", "PAL");
    set_option("puae_video_resolution", "lores");
    set_option("puae_video_vresolution", "single");
    set_option("puae_crop", "disabled");
    set_option("puae_floppy_speed", "100");
    set_option("puae_floppy_sound", "100");          /* 100: muted (the option is attenuation) */
    if (env)
    {
        char *s = strdup(env), *tok, *save;
        for (tok = strtok_r(s, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
        {
            char *eq = strchr(tok, '=');
            if (eq)
            {
                *eq = 0;
                set_option(tok, eq + 1);
            }
        }
    }
}

static const char *option(const char *key)
{
    for (int i = 0; i < g_nset; i++)
        if (strcmp(g_set_key[i], key) == 0)
            return g_set_val[i];
    return NULL;
}

/* Every option's declared default, as a real frontend would answer: the core's own fallbacks are not always the same */
#define MAX_OPTS 512
static const char *g_opt_key[MAX_OPTS], *g_opt_def[MAX_OPTS];
static int g_nopts;

static void add_default(const char *key, const char *def)
{
    if (key && g_nopts < MAX_OPTS)
    {
        g_opt_key[g_nopts] = key;
        g_opt_def[g_nopts++] = def;
    }
}

static const char *default_of(const char *key)
{
    for (int i = 0; i < g_nopts; i++)
        if (strcmp(g_opt_key[i], key) == 0)
            return g_opt_def[i];
    return NULL;
}

/* The core's disk swapping, for multi-disk images given as an .m3u list */
static struct retro_disk_control_ext_callback g_disk;
static bool g_have_disk;
static long g_ppm_every = 500;

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        *(unsigned *)data = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        g_disk = *(const struct retro_disk_control_ext_callback *)data;
        g_have_disk = true;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
        memcpy(&g_disk, data, sizeof(struct retro_disk_control_callback));
        g_have_disk = true;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
    {
        const struct retro_core_options_v2 *o = cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 ? data
            : ((const struct retro_core_options_v2_intl *)data)->us;
        for (const struct retro_core_option_v2_definition *d = o->definitions; d && d->key; d++)
            add_default(d->key, d->default_value);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        for (const struct retro_core_option_definition *d = data; d && d->key; d++)
            add_default(d->key, d->default_value);
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_sysdir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        g_fmt = *(const enum retro_pixel_format *)data;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        struct retro_variable *v = data;
        v->value = option(v->key);
        if (!v->value)
            v->value = default_of(v->key);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    default:
        return false;
    }
}

static long g_video_calls, g_video_null;

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    g_video_calls++;
    g_video_null += !data;
    if (!data)
        return;                                         /* a repeated frame: keep the last one */
    if (w > FW || h > FH)
    {
        fprintf(stderr, "frame %ux%u is bigger than %dx%d\n", w, h, FW, FH);
        exit(1);
    }
    g_fw = (int)w;
    g_fh = (int)h;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
        {
            uint8_t *d = g_rgb + (y * FW + x) * 3;
            if (g_fmt == RETRO_PIXEL_FORMAT_XRGB8888)
            {
                uint32_t p = ((const uint32_t *)((const uint8_t *)data + y * pitch))[x];
                d[0] = p >> 16;
                d[1] = p >> 8;
                d[2] = p;
            }
            else
            {
                uint16_t p = ((const uint16_t *)((const uint8_t *)data + y * pitch))[x];
                if (g_fmt == RETRO_PIXEL_FORMAT_RGB565)
                {
                    d[0] = (uint8_t)(((p >> 11) & 31) << 3);
                    d[1] = (uint8_t)(((p >> 5) & 63) << 2);
                }
                else
                {
                    d[0] = (uint8_t)(((p >> 10) & 31) << 3);
                    d[1] = (uint8_t)(((p >> 5) & 31) << 3);
                }
                d[2] = (uint8_t)((p & 31) << 3);
            }
        }
}

static size_t g_audio_frames;
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; g_audio_frames++; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; g_audio_frames += n; return n; }
static void poll_cb(void) {}

static int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)g_pad;
    return (g_pad >> id) & 1;
}

/* The scripted joystick: fire held 5 frames out of every 100; "game" also holds right after 20 s */
static void script(const char *name, long frame)
{
    bool pulse = frame % 100 < 5;
    g_pad = 0;
    if (strcmp(name, "fire") == 0 || strcmp(name, "game") == 0)
        if (pulse)
            g_pad |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
    if (strcmp(name, "game") == 0 && frame > 20 * 50)
        g_pad |= 1 << ((frame / 250) % 4 == 3 ? RETRO_DEVICE_ID_JOYPAD_LEFT : RETRO_DEVICE_ID_JOYPAD_RIGHT);
}

static void write_ppm(const char *dir, long frame)
{
    char path[512];
    FILE *f;
    snprintf(path, sizeof(path), "%s/f%05ld.ppm", dir, frame);
    if (!(f = fopen(path, "wb")))
        return;
    fprintf(f, "P6\n%d %d\n255\n", g_fw, g_fh);
    for (int y = 0; y < g_fh; y++)
        fwrite(g_rgb + y * FW * 3, 3, g_fw, f);
    fclose(f);
}

static void count_size(void)
{
    for (int i = 0; i < g_nsizes; i++)
        if (g_sizes[i].w == g_fw && g_sizes[i].h == g_fh)
        {
            g_sizes[i].frames++;
            return;
        }
    if (g_nsizes < MAX_SIZES)
    {
        g_sizes[g_nsizes].w = g_fw;
        g_sizes[g_nsizes].h = g_fh;
        g_sizes[g_nsizes++].frames = 1;
    }
}

#define SYM(name) do { if (!(name = (__typeof__(name))dlsym(core, #name))) { fprintf(stderr, "no %s\n", #name); return 1; } } while (0)

int main(int argc, char **argv)
{
    void (*retro_set_environment)(retro_environment_t);
    void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t);
    void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t);
    void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_set_controller_port_device)(unsigned, unsigned);
    void (*retro_init)(void);
    bool (*retro_load_game)(const struct retro_game_info *);
    void (*retro_run)(void);
    void *core;
    long frames;
    double emu_ms = 0, emu_max = 0;
    struct retro_game_info game = { 0 };
    static config_t cfg[] = {
        { "d0 50fps", 0.0f, 1 }, { "d1 50fps", 1.0f, 1 }, { "d3 50fps", 3.0f, 1 },
        { "d0 25fps", 0.0f, 2 }, { "d1 25fps", 1.0f, 2 }, { "d3 25fps", 3.0f, 2 },
        { "d0 12fps", 0.0f, 4 }, { "d1 12fps", 1.0f, 4 }, { "d3 12fps", 3.0f, 4 },
    };
    const int ncfg = sizeof(cfg) / sizeof(cfg[0]);

    if (argc < 6)
    {
        fprintf(stderr, "usage: %s <core.so> <system dir> <image> <frames> <none|fire|game|swap@N> [ppm-dir]\n",
                argv[0]);
        return 2;
    }
    if (!(g_jxl = jxl_enc_available()))
        fprintf(stderr, "libjxl not found: costing pictures at 4 bits a pixel instead\n");
    g_sysdir = argv[2];
    if (getenv("A1200SPIKE_PPM_EVERY"))
        g_ppm_every = atol(getenv("A1200SPIKE_PPM_EVERY"));
    init_options();
    frames = atol(argv[4]);
    if (!(core = dlopen(argv[1], RTLD_NOW)))
    {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    SYM(retro_set_environment);
    SYM(retro_set_video_refresh);
    SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch);
    SYM(retro_set_input_poll);
    SYM(retro_set_input_state);
    SYM(retro_set_controller_port_device);
    SYM(retro_init);
    SYM(retro_load_game);
    SYM(retro_run);

    retro_set_environment(env_cb);
    retro_init();
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(poll_cb);
    retro_set_input_state(input_cb);
    game.path = argv[3];
    if (!retro_load_game(&game))
    {
        fprintf(stderr, "load failed\n");
        return 1;
    }
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    for (int i = 0; i < ncfg; i++)
        cfg[i].ref = malloc((size_t)FW * FH * 3);

    for (long f = 0; f < frames; f++)
    {
        double t0 = now_ms(), dt;
        script(argv[5], f);
        if (strncmp(argv[5], "swap@", 5) == 0 && f == atol(argv[5] + 5) && g_have_disk)
        {
            unsigned next = (g_disk.get_image_index() + 1) % g_disk.get_num_images();
            g_disk.set_eject_state(true);
            g_disk.set_image_index(next);
            g_disk.set_eject_state(false);
            fprintf(stderr, "frame %ld: swapped to disk %u of %u\n", f, next + 1, g_disk.get_num_images());
        }
        retro_run();
        dt = now_ms() - t0;
        emu_ms += dt;
        if (dt > emu_max && f > 50)
            emu_max = dt;
        count_size();
        if (argc > 6 && f % g_ppm_every == 0)
            write_ppm(argv[6], f);
        for (int i = 0; i < ncfg; i++)
            measure(&cfg[i], f);
    }

    printf("image %s, %ld frames (%.1f s of Amiga time), audio %.0f Hz\n", argv[3], frames, frames / 50.0,
           g_audio_frames / (frames / 50.0));
    printf("options:");
    for (int i = 0; i < g_nset; i++)
        printf(" %s=%s", g_set_key[i], g_set_val[i]);
    printf("\ncore options: %d defaults known; video: %ld calls, %ld repeats, pixel format %d\n", g_nopts,
           g_video_calls, g_video_null, (int)g_fmt);
    printf("frame sizes:");
    for (int i = 0; i < g_nsizes; i++)
        printf(" %dx%d (%ld frames)", g_sizes[i].w, g_sizes[i].h, g_sizes[i].frames);
    printf("\nemulation: %.2f ms a frame on average (%.0f%% of one core at 50 fps), worst %.1f ms\n", emu_ms / frames,
           emu_ms / frames * 5.0, emu_max);
    printf("%-10s %9s %9s %9s %7s %8s %9s\n", "config", "KB/s", "avg KB", "max KB", "full%", "rects/f", "enc ms/f");
    for (int i = 0; i < ncfg; i++)
    {
        config_t *c = &cfg[i];
        double secs = frames / 50.0, sent = c->frames_sent ? (double)c->frames_sent : 1;
        printf("%-10s %9.1f %9.2f %9.2f %6.0f%% %8.1f %9.2f\n", c->name, c->bytes / 1024 / secs, c->bytes / 1024 / sent,
               c->max_frame / 1024, 100.0 * c->full_frames / sent, c->rects / sent,
               c->enc_ms / (frames / (double)c->skip));
    }
    return 0;
}
