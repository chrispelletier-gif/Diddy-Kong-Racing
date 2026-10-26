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
s32 port_autorun_quit_race(void);
void port_autorun_request(s32 start);
void port_autorun_frame(s32 updateRate);
/* Sets up a one player tracks mode race as the menus would, and returns the menu result that starts it (menu.c). */
s32 port_autorun_setup_race(s32 levelId, s32 character, s32 vehicle);

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
#define PORT_STEP(x) (gPortHalfStep ? (x) * 0.5f : (x))
#define PORT_DECAY(k) (gPortHalfStep ? sqrtf(k) : (k))
#define PORT_LERP(k) (gPortHalfStep ? 1.0f - sqrtf(1.0f - (k)) : (k))
#define PORT_TICK() (!gPortSkipTick)
/* A whole-number "x >> n" each frame (an angle closing a share of its gap): one more shift at half step. */
#define PORT_SHIFT(x, n) (gPortHalfStep ? (x) >> ((n) + 1) : (x) >> (n))
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
void port_describe_game_state(char *buffer, s32 size);

/* Logging. */
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* Main memory pool. The game carves its whole heap out of this. */
#define PORT_MAIN_POOL_SIZE (8 * 1024 * 1024)
extern u8 gPortMainPool[];

#endif
