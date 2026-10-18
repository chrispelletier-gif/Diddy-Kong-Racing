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

void port_swap16_array(void *ptr, s32 count);
void port_swap32_array(void *ptr, s32 count);

/* ROM access (port/src/os/os_pi.c). */
/* Results of port_rom_load. */
enum { PORT_ROM_OK = 1, PORT_ROM_NOT_FOUND = 0, PORT_ROM_NOT_N64 = -1, PORT_ROM_NOT_DKR = -2, PORT_ROM_WRONG_VERSION = -3 };
s32 port_rom_load(const char *path);
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
void port_audio_output_init(u32 frequency);
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

/* Logging. */
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/* Main memory pool. The game carves its whole heap out of this. */
#define PORT_MAIN_POOL_SIZE (8 * 1024 * 1024)
extern u8 gPortMainPool[];

#endif
