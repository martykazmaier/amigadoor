/*
 * core.h -- the Amiga itself: PUAE (libretro-uae) as a libretro core, loaded from puae_libretro.so beside the door.
 * See core.c.
 */

#ifndef AMIGA_CORE_H
#define AMIGA_CORE_H

#include <stdbool.h>
#include <stdint.h>

/* The largest frame the door takes. Held to single lines (core.c), PUAE sends 360x287 for a low-resolution PAL screen,
 * 720x287 for a high-resolution one (Workbench, WHDLoad's messages) and 1440x287 for super-high resolution (Super
 * Stardust's intro scroller), 242 high on NTSC, borders included. 1440 is the most PUAE sends (EMULATOR_MAX_WIDTH);
 * a cap of 768 cut that scroller's right half off (sysop, 2026-10-28). */
#define CORE_MAX_W 1440
#define CORE_MAX_H 296

/* Joystick bits, as the RetroPad has them (B is the Amiga's fire button, A its rarely used second button) */
#define JOY_UP    (1u << 4)
#define JOY_DOWN  (1u << 5)
#define JOY_LEFT  (1u << 6)
#define JOY_RIGHT (1u << 7)
#define JOY_FIRE  (1u << 0)
#define JOY_FIRE2 (1u << 8)

/* Mouse buttons for core_mouse */
#define MOUSE_LEFT  1u
#define MOUSE_RIGHT 2u

typedef struct
{
    bool a1200;                 /* an A1200 with Kickstart 3.1; otherwise an A500 with Kickstart 1.3 */
    bool ntsc;                  /* an NTSC machine; otherwise PAL */
} core_machine_t;

/* Loads the core and starts it with image (an .adf, .lha or .m3u list of disks, the first in DF0:), or with no disk
 * when image is NULL. system_dir is the core's system folder (the Kickstart ROMs). False with a reason on failure. */
bool core_start(const char *core_path, const char *system_dir, const char *image, const core_machine_t *m,
                const char **why);

/* One frame of Amiga time (1/50 s on PAL). Afterwards the newest picture is in core_rgb() and its sound in core_audio(). */
void core_run_frame(void);

/* The newest picture, RGB24, rows CORE_MAX_W pixels apart, core_w() x core_h() of it in use */
const uint8_t *core_rgb(void);
int core_w(void);
int core_h(void);

/* The frame rate and the sound's sample rate the core runs at. */
double core_fps(void);
double core_sample_rate(void);

/* Takes up to max stereo frames of the sound made so far (S16 interleaved). Returns how many. */
int core_audio(int16_t *out, int max);

/* Keys, as libretro key codes (RETROK_*), pressed or let go. */
void core_key(unsigned retrok, bool down);

/* The joystick in the Amiga's joystick port, as JOY_* bits. */
void core_joystick(unsigned bits);

/* The mouse in the Amiga's mouse port: movement since the last call (Amiga low-resolution pixels) and the buttons
 * held now (MOUSE_*). The movement is handed to the Amiga on the next frame. */
void core_mouse(int dx, int dy, unsigned buttons);

/* The disks: how many, which is in DF0:, and swapping to another (eject, change, insert, as a player would). */
int core_disk_count(void);
int core_disk_index(void);
void core_disk_insert(int index);

/* The floppy light (or the hard drive's, for a WHDLoad game): on while it reads or writes */
bool core_drive_led(void);

/* A reset of the Amiga, the disks staying as they are. */
void core_reset(void);

/* Flushes the drive's writes to the image files and unloads the core. */
void core_stop(void);

#endif
