#ifndef PORT_H
#define PORT_H

/*
 * Shared declarations for the native port. This header is safe to include from game code
 * (it only depends on the libultra types).
 */

#include <PR/ultratypes.h>

#ifndef TARGET_PORT
#define TARGET_PORT 1
#endif

/* Byte swapping helpers. The N64 is big endian, every port target is little endian. */
#define PORT_BSWAP16(x) ((u16) __builtin_bswap16((u16) (x)))
#define PORT_BSWAP32(x) ((u32) __builtin_bswap32((u32) (x)))

static inline void port_swap16(void *ptr) {
    u16 *p = (u16 *) ptr;
    *p = PORT_BSWAP16(*p);
}

static inline void port_swap32(void *ptr) {
    u32 *p = (u32 *) ptr;
    *p = PORT_BSWAP32(*p);
}

static inline u16 port_read_be16(const void *ptr) {
    const u8 *p = (const u8 *) ptr;
    return (u16) ((p[0] << 8) | p[1]);
}

static inline u32 port_read_be32(const void *ptr) {
    const u8 *p = (const u8 *) ptr;
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

/*
 * Float to integer conversion with a defined result for every input: like the 3DS and N64 FPUs, out of range values
 * saturate (NaN gives 0). Converting with a cast instead is undefined when the value doesn't fit, and the optimiser
 * may then assume it always fits: a game value converted to s16 that is out of range must go through this, e.g.
 * (s16) port_f2i32(x), which keeps the low 16 bits like the N64 did.
 */
static inline s32 port_f2i32(f64 x) {
    if (!(x == x)) {
        return 0;
    }
    if (x >= 2147483647.0) {
        return 0x7FFFFFFF;
    }
    if (x <= -2147483648.0) {
        return (s32) 0x80000000;
    }
    return (s32) x;
}

void port_swap16_array(void *ptr, s32 count);
void port_swap32_array(void *ptr, s32 count);

/* ROM access (port/src/os/os_pi.c). */
/* Results of port_rom_load, the failures from least to most telling (the lowest one seen is reported). */
enum {
    PORT_ROM_OK = 1,
    PORT_ROM_NOT_FOUND = 0,
    PORT_ROM_NOT_N64 = -1,
    PORT_ROM_NOT_DKR = -2,
    PORT_ROM_WRONG_VERSION = -3,
    PORT_ROM_DAMAGED = -4
};
s32 port_rom_load(const char *path);
/* Why port_rom_load last turned a ROM down, as a sentence naming the file. */
const char *port_rom_problem(void);
/* The release this build plays, such as "US 1.0". */
const char *port_rom_supported_release(void);
const u8 *port_rom_data(void);
u32 port_rom_size(void);
void port_rom_read(u32 romOffset, void *dst, u32 size);
u32 port_assets_lut_start(void);
u32 port_assets_lut_end(void);

/* OS emulation internals (port/src/os/os_core.c). */
struct OSMesgQueue_s;
void port_int_thread_init(void);
int port_int_release(void);
void port_int_restore(int wasDisabled);
void port_post_mesg(struct OSMesgQueue_s *mq, void *msg);
void port_raise_event(u32 event);
/* Blocks the calling thread until ready() returns true (both with the kernel lock held). */
void port_kwait_until(int (*ready)(void));
/* DKR_LOCKSTEP: retraces wait for every N64 thread and the RCP to be idle (see os_core.c). */
int port_lockstep_enabled(void);
void port_lockstep_wait(void);
unsigned long long port_time_ns(void);
void port_lockstep_dma(u32 numBytes);
void port_vi_retrace(void);

/* Emulated hardware startup. */
void port_vi_init(void);
void port_rcp_init(void);
void port_rcp_wait_idle(void);
/*
 * Frames the game may run ahead of the renderer, 1 or 2 (plat_render_ahead). The game keeps PORT_DL_BUFFERS display
 * list heaps and task structures instead of the N64's two, so that with 2 the list being drawn, the one waiting and
 * the one being built each have their own.
 */
s32 port_render_ahead(void);
#define PORT_DL_BUFFERS 3
void port_rcp_gfx_submitted(void);
int port_rcp_idle(void);
void port_si_init(void);
/* Writes pending save data to disk now (called on exit and when the application is suspended). */
void port_si_flush(void);
void port_ai_init(void);

/* Rendering and audio backends. */
void port_gfx_init(void);
/* Level load timing (game/port_loadtime.c): one log line per load with its steps and the first frame after it. */
void port_load_begin(void);
void port_load_bg_queued(void);
void port_load_step(const char *name);
void port_load_end(s32 levelId, const char *kind);
void port_load_frame_done(void);
void port_load_pick(s32 step);
void port_load_menu_held(s32 held);
void port_load_game_frame(s32 gameMode, s32 levelId);
void port_gfx_run_task(void *task);
void port_audio_run_task(void *task);
/*
 * The rate the game's mixer runs at when it asks for requested (the player can choose a higher one, roadmap O5).
 * Pitches and reverb lengths are scaled by gPortAudioRateScale (requested / chosen) to sound the same.
 */
u32 port_audio_output_rate(u32 requested);
extern float gPortAudioRateScale;
void port_audio_output_init(u32 frequency);
/* The DSP voice engine (port/src/audio/voices.c): the game sizes the synthesizer and decodes its banks with these. */
s32 port_voices_max_pvoices(s32 requested);
void port_voices_prewarm(void *bankFile);
void port_voices_start(u32 synthRate);
void port_audio_output_queue(const s16 *samples, u32 numBytes);
u32 port_audio_output_remaining_bytes(void);

/* Controllers. Fills in up to four N64 style controller states. */
typedef struct PortControllerState {
    u16 buttons;
    s8 stickX;
    s8 stickY;
    u8 connected;
} PortControllerState;
void port_input_poll(PortControllerState *state, s32 maxControllers);
void port_rumble(s32 controller, s32 enable);

/* Save files. */
s32 port_save_read(const char *name, void *data, u32 size);
s32 port_save_write(const char *name, const void *data, u32 size);
/* Whether the data folder has the file at all (an empty one counts). */
s32 port_save_exists(const char *name);
/*
 * The saves on disk are in the N64's byte order, the same as an emulator's .eep and .mpk (os/save_convert.c). These
 * turn a 512 byte EEPROM image either way (it is its own inverse) and a 32 KB Controller Pak image to the N64's order
 * (toN64) or back to the one the game's code reads.
 */
void port_eep_convert(u8 *eeprom);
void port_mpk_convert(u8 *pak, s32 toN64);
#include "port_saves.h"

/* Small libc helpers. */
static inline s32 port_atoi(const char *s) {
    s32 v = 0, neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return neg ? -v : v;
}

/*
 * Multiplayer. One 3DS is one set of controls, so the 2 to 4 player game is hidden until wireless or online play
 * (roadmap Phase 10) can fill the other seats: only player one joins character select, and the magic codes that only
 * do something with more players (DOUBLEVISION, JOINTVENTURE) are left out of code entry, the code list and the
 * credits (src/menu.c). The multiplayer code itself stays compiled and untouched; building with PORT_MULTIPLAYER=1
 * (make ... PORT_MULTIPLAYER=1) brings it all back. Test runs, the switch bench and CI never play multiplayer.
 */
#ifndef PORT_MULTIPLAYER
#define PORT_MULTIPLAYER 0
#endif

/* Test helpers (port/src/game/port_test.c). */
s32 port_warp_next(s32 levelId, s32 *cutsceneId, s16 *timer);
s32 port_warp_active(void);
s32 port_gfx_frame_count(void);
/* Texture pre-decoding (gfx_rdp.c): the game hands over each texture it loads and the memory of each one it frees. */
void port_gfx_texture_loaded(const void *cmd, s32 numCommands);
void port_gfx_texture_freed(const void *start, const void *end);
s32 port_apply_test_save(void *settings);

/* Unattended test run (port/src/game/port_autorun.c). */
s32 port_autorun_enabled(void);
s32 port_autorun_active(void);
s32 port_autorun_title(void);
s32 port_autorun_drives(void);
s32 port_autorun_time_trial(void);
s32 port_autorun_quit_race(void);
void port_autorun_request(s32 start);
s32 port_autorun_switch_count(void);
void port_autorun_frame(s32 updateRate);
/* Sets up a one player tracks mode race as the menus would, and returns the menu result that starts it (menu.c). */
s32 port_autorun_setup_race(s32 levelId, s32 character, s32 vehicle);
/* What the menu has highlighted, for the bottom screen (menu.c). */
enum { PORT_MENU_FOCUS_NONE, PORT_MENU_FOCUS_FILE, PORT_MENU_FOCUS_TRACK };
struct Settings;
s32 port_menu_focus(s32 *levelId, s32 *vehicle, struct Settings **file, char **name);

/*
 * Camera options (port/src/game/port_camera.c). The Modern chase camera (Settings > Camera, saved as camera= in
 * settings.txt; DKR_CAMERA=1 in env.txt for a test) cuts the N64 camera's lag and roll; the look-back button turns
 * player one's chase camera round while held. PORT_BUTTON_LOOK_BACK is a bit the N64 controller never sets: the
 * controls mapping (plat/controls.c, CTRL_N64_LOOK_BACK) puts it in a pad's buttons and os_si.c takes it out again
 * before the game sees them.
 */
#define PORT_BUTTON_LOOK_BACK 0x0040
s32 port_modern_camera(void);
void port_set_modern_camera(s32 on);
s32 port_look_back_held(void);
void port_set_look_back_held(s32 held);
/* The camera wall check (Settings > Camera > Wall check, camwall= in settings.txt, DKR_CAMERA_WALLS for a test): stops
 * the chase camera in front of walls and scenery instead of letting it pass into them as the N64's does. On by
 * default. */
s32 port_camera_wall_check(void);
void port_set_camera_wall_check(s32 on);

/*
 * The intro logos (port/src/game/port_intro.c, roadmap Q10): whether the game skips the Rareware and N64 logo screen and
 * goes from the boot screen to the title screen (skipintro= in settings.txt, DKR_SKIP_INTRO for a test). Off by
 * default. src/menu.c also lets A, B or START cut the logo screen short.
 */
s32 port_skip_intro(void);
void port_set_skip_intro(s32 on);

/*
 * The port's text in the game's menus (port/src/game/port_text.c): english as the game's current language writes it
 * (French when it is set to FRANCAIS), or english itself when there is no translation.
 */
const char *port_text(const char *english);
/* The game's dialogue after it is loaded: wording that names a button the 3DS lacks is changed (port_text.c). */
void port_fix_game_text(char *text, int size);

/*
 * The first-run setup (port/src/game/port_setup.c): language, then a play style or a page each for the picture, the
 * screen shape, the camera and the race HUD, on the bottom screen the first time the title screen shows (setup= in
 * settings.txt; on PC only for DKR_SETUP=1). src/menu.c keeps START and OPTIONS back while it shows, gives it the
 * title screen's input and draws its box (port_setup_caption) under the logo.
 */
s32 port_setup_active(void);
void port_setup_update(u32 buttons, s32 stickX, s32 stickY);
s32 port_setup_caption(const char **name, const char **tag, const char **about);
s32 port_setup_done(void);
void port_setup_reset(void);
/* Debug: asks for the setup again, and runs it now when the title screen is showing. */
void port_setup_restart(void);

/*
 * The race HUD on the 3DS bottom screen (port/src/game/port_race_hud.c, roadmap G9): port_race_hud_frame copies the
 * race's state for the bottom screen once a frame; port_race_hud_map takes the track map's sprite from minimap_init;
 * port_race_hud_hides says which HUD elements (HUD_*, or -1 for the track map) src/game_ui.c leaves off the top screen.
 */
void port_race_hud_frame(s32 updateRate);
void port_race_hud_map(void *model, s32 spriteIndex);
s32 port_race_hud_hides(s32 element);
s32 port_race_hud_setting(void);
void port_set_race_hud_setting(s32 value);

/*
 * Portrait camera (port/src/game/port_portrait.c, DKR_PORTRAIT): a test helper that frames one model alone, for drawing
 * portraits from the game's 3D models. src/tracks.c places the camera and leaves out the track and sky; render_object
 * leaves out every other object.
 */
struct Object;
struct Camera;
s32 port_portrait_active(void);
s32 port_portrait_hides(struct Object *obj);
void port_portrait_camera(struct Camera *cam);

/*
 * The port's settings in the game's menus (src/menu.c, plat/options.h): fills in the Options menu's rows when the
 * menu text is loaded, and says whether a category's page is open (over the Options menu or the pause menu).
 */
void port_options_menu_strings(void);
s32 port_options_menu_open(void);

/*
 * 60 fps physics (src/thread3_main.c sets these once a frame). Much of the racer code takes one fixed step per logic frame
 * (the decomp marks these lines //!@Delta), tuned for 30 fps, where a frame is 2 retraces. At 60 fps a frame is 1
 * retrace, so those steps would happen twice as often: karts would speed up, slow down, steer and spin out twice as
 * fast. While the logic runs at 1 retrace a frame, gPortHalfStep is set and the PORT_* macros below halve the steps:
 * PORT_STEP halves an amount added each frame, PORT_DECAY takes the square root of a factor applied each frame,
 * PORT_LERP gives the per-retrace version of "move this fraction of the way there", and PORT_TICK is false every
 * other frame so a whole-number counter or shift steps at 30 fps. At 2 retraces or more they change nothing, so
 * 30 fps runs as before. DKR_RATE_FIX=0 in env.txt (or the environment on PC) turns them off for a comparison.
 */
extern s32 gPortHalfStep;
extern s32 gPortSkipTick;
/* The logic step of the frame being built, in retraces: the renderer gets it with the frame's display list. */
extern s32 gPortLogicStep;
#define PORT_STEP(x) (gPortHalfStep ? (x) * 0.5f : (x))
#define PORT_DECAY(k) (gPortHalfStep ? sqrtf(k) : (k))
#define PORT_LERP(k) (gPortHalfStep ? 1.0f - sqrtf(1.0f - (k)) : (k))
#define PORT_TICK() (!gPortSkipTick)
/* A whole-number "x >> n" each frame (an angle closing a share of its gap): half the step at half step, but never
   less than the 30 fps step once that is down to 1, so the gap closes as far as it does at 30 fps. */
#define PORT_SHIFT(x, n) (gPortHalfStep ? port_half_shift((x) >> (n)) : (x) >> (n))
static inline s32 port_half_shift(s32 step) {
    return (step / 2 != 0) ? step / 2 : step;
}
/* A whole number added each frame: half of it at half step (odd amounts round toward zero). */
#define PORT_HALF(x) (gPortHalfStep ? (x) / 2 : (x))
void port_set_logic_rate(s32 updateRate);

/*
 * What the game thread is doing (src/thread3_main.c sets it at each step of the main loop), and a line describing the
 * game's state (port_test.c), which the 3DS watchdog logs when no frame has been drawn for a while.
 */
extern const char *volatile gPortGameStage;
/* What the sound thread is doing (for the 3DS watchdog's stall report). */
extern const char *volatile gPortAudioStage;
/* Sound events thrown away on a full queue, and handler runs that found their queue empty (libultra/src/audio/event.c). */
extern volatile u32 gPortAudioEventsDropped;
extern volatile u32 gPortAudioQueueEmpty;
void port_describe_game_state(char *buffer, s32 size);

/* Logging. */
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* Main memory pool. The game carves its whole heap out of this. */
#define PORT_MAIN_POOL_SIZE (8 * 1024 * 1024)
extern u8 gPortMainPool[];

#endif
