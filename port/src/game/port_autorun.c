/*
 * Unattended test run: races every track with every character, one after another, with the game's AI driving the
 * player's kart, and records a performance profile and the outcome of each race.
 *
 * It is started and stopped with the "Test run" button on the bottom screen of the 3DS, or always runs with:
 *   DKR_AUTORUN=1                   Turns it on (and the profiler with it).
 *   DKR_AUTORUN_TRACKS=all|id,...   Tracks to race (ASSET_LEVEL_* ids). Default: the 20 tracks of the tracks menu.
 *   DKR_AUTORUN_CHARACTERS=all|n,... Characters (Krunch 0, Bumper 1, Tiptup 2, Conker 3, Timber 4, Banjo 5,
 *                                   Drumstick 6, Pipsy 7, T.T. 8, Diddy 9). Default: all ten.
 *   DKR_AUTORUN_VEHICLES=all        Also every other vehicle a track allows (default: only its usual vehicle).
 *   DKR_AUTORUN_TIMEOUT=seconds     A race that hasn't finished by then is stopped and counted as a timeout (300).
 *   DKR_AUTORUN_LOOP=1              Start over after the last race, forever.
 *   DKR_AUTORUN_RESTART=1           Start from the first race instead of resuming.
 *   DKR_AUTORUN_SNAPSHOT=1          Also save a snapshot (GPU and reference images) after each race finishes.
 *   DKR_AUTORUN_TIME_TRIAL=1        Race in time trial (alone, with the ghosts) instead of against the field.
 *
 * The GPU benchmark is a short run instead: the heaviest races of the hardware test runs, each raced once with every
 * render setting of sBenchConfigs (anti-aliasing 4x, 2x or off, 30 or 60 fps), with the
 * profiler's numbers for each set side by side in the results. About 30 minutes. It replaced the anti-aliasing
 * benchmark of roadmap item G0 (AA off and on only). It is started with the "GPU bench" button in the Test run
 * confirmation, or always runs with:
 *   DKR_AUTORUN_BENCH=1               Turns it on (instead of the full run).
 *   DKR_AUTORUN_BENCH_TRACKS=id,...   Tracks (default: Jungle Falls, Crescent Island and Haunted Woods, the slowest
 *                                     tracks of the quick tests after P5, all GPU-bound with 4x anti-aliasing).
 *   DKR_AUTORUN_BENCH_CHARACTER=n     Character (default Bumper, whose races were the heaviest there).
 *   DKR_AUTORUN_BENCH_CONFIGS=n,...   The settings to race (indexes into sBenchConfigs; default all).
 * It keeps its own files (gpu_bench_state.txt, gpu_bench_results.csv with "config" and "pct_60fps" columns), so a
 * full run it interrupts carries on afterwards. DKR_AUTORUN_TIMEOUT, _LOOP and _RESTART apply to it too.
 *
 * The switch bench (roadmap P29) is the GPU benchmark's twin for the renderer's experimental switches (sSwitches): the
 * same heavy races, once with every switch at its usual setting and once with each switch flipped by itself, set in
 * the program at the start of each race (plat_override_switch), so env.txt needs no editing. The races keep the
 * player's render settings, which the results name. About 6 minutes a switch plus 6 for the usual settings. New
 * experiments add a line to sSwitches. It is started with the "Switch bench" button in the Test run confirmation,
 * or always runs with:
 *   DKR_AUTORUN_SWITCH=1              Turns it on (instead of the full run).
 *   DKR_AUTORUN_SWITCH_TRACKS=id,...  Tracks (default: the GPU benchmark's three).
 *   DKR_AUTORUN_SWITCH_CHARACTER=n    Character (default Bumper).
 *   DKR_AUTORUN_SWITCH_CONFIGS=n,...  The races' switch sets: 0 every switch at its usual setting, n switch n-1 of
 *                                     sSwitches flipped, and with two switches or more a last one with all of
 *                                     them flipped together (default all).
 * Its files are switch_bench_state.txt and switch_bench_results.csv (with "config" and "pct_60fps" columns, as the
 * GPU benchmark's).
 *
 * The quick test is the other short run: the slowest tracks of the full run, twice each with one character and the
 * player's own anti-aliasing setting, about 20 minutes. It is for telling whether a change made the slow spots
 * faster: its results compare each track with the quick test before it (usually the previous build). It is started
 * with the "Quick test" button in the Test run confirmation, or always runs with:
 *   DKR_AUTORUN_QUICK=1               Turns it on (instead of the full run).
 *   DKR_AUTORUN_QUICK_TRACKS=id,...   Tracks (default: Haunted Woods, Jungle Falls, Crescent Island, Whale Bay and
 *                                     EverFrost Peak, the slowest of hardware test run issue #12: three GPU-bound car
 *                                     tracks, one renderer-bound hover and one plane track).
 *   DKR_AUTORUN_QUICK_CHARACTER=n     Character (default Bumper, as the anti-aliasing benchmark).
 *   DKR_AUTORUN_QUICK_PASSES=n        Races per track (2).
 * Its files are quick_test_state.txt and quick_test_results.csv (with a "build" column).
 *
 * The boss fights test races every boss once (Tricky, Bluey, Bubbler, Smokey and Wizpig, the first and the second
 * fight of each), with one character, about half an hour. Boss levels aren't on the tracks menu, so the full run
 * never races them. It is started with the "Boss fights" button in the Test run confirmation, or always runs with:
 *   DKR_AUTORUN_BOSS=1                Turns it on (instead of the full run).
 *   DKR_AUTORUN_BOSS_TRACKS=id,...    Levels (default: the ten boss fights).
 *   DKR_AUTORUN_BOSS_CHARACTER=n      Character (default Bumper, as the other short runs).
 *   DKR_AUTORUN_BOSS_PASSES=n         Races per boss (1).
 * Its files are boss_test_state.txt and boss_test_results.csv (with a "build" column).
 *
 * The order is character by character, each over every track, so stopping early still covers every track.
 * Progress is saved before each race (autorun_state.txt), so after a crash or a hang the next launch resumes with the
 * race after the one that failed, and records that race's failure. Each race adds a line to autorun_results.csv,
 * and problems also go to autorun_problems.txt (both in the data directory, sdmc:/3ds/dkr/ on the 3DS).
 * port/tools/autorun_report.py turns the results into a report. The game's own save is never written meanwhile.
 * When the run ends (or is stopped), the results are sent as a GitHub issue if bug reports are set up.
 */
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "asset_enums.h"
#include "game.h"
#include "menu.h"
#include "objects.h"
#include "thread3_main.h"
#include "camera.h"
#include "math_util.h"
#include "tracks.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/profile.h"
#include "gfx/gfx_split.h"
#include <string.h>

#define MAX_RACES 1024
#define MAX_TRACKS 64
#define FINISH_LINGER_SECONDS 8 // After the finish line: the post-race camera and results, then on to the next.
#define UNSTICK_SECONDS 10      // No new checkpoint passed for this long: the kart is put at the next checkpoint.
#define STUCK_SECONDS 60        // No new checkpoint passed for this long (even so) counts as stuck.
#define STATE_FILE "autorun_state.txt"
#define RESULTS_FILE "autorun_results.csv"
#define PROBLEMS_FILE "autorun_problems.txt"
#define HANG_FILE "autorun_hang.txt"
#define BENCH_STATE_FILE "gpu_bench_state.txt"
#define BENCH_RESULTS_FILE "gpu_bench_results.csv"
#define QUICK_STATE_FILE "quick_test_state.txt"
#define QUICK_RESULTS_FILE "quick_test_results.csv"
#define QUICK_PASSES 2
#define BOSS_STATE_FILE "boss_test_state.txt"
#define BOSS_RESULTS_FILE "boss_test_results.csv"
#define BOSS_PASSES 1
#define SWITCH_STATE_FILE "switch_bench_state.txt"
#define SWITCH_RESULTS_FILE "switch_bench_results.csv"

/* Which run: the full test run, the GPU benchmark, the quick test, the boss fights or the switch bench. */
enum Mode { MODE_FULL, MODE_BENCH, MODE_QUICK, MODE_BOSS, MODE_SWITCH, MODE_COUNT };

typedef struct Race {
    s8 track;
    s8 character;
    s8 vehicle;
    s8 config; // The settings for the race (an index into sBenchConfigs, or the switch bench's set), or -1 to leave them.
} Race;

/* The GPU benchmark's render settings. The first is the default ones, which the others are compared with. */
typedef struct BenchConfig {
    s8 aa;    // PLAT_AA_*
    s8 fps60; // The 60 fps cap
    const char *name;
} BenchConfig;

static const BenchConfig sBenchConfigs[] = {
    { PLAT_AA_4X, 0, "AA 4x" },
    { PLAT_AA_2X, 0, "AA 2x" },
    { PLAT_AA_OFF, 0, "AA off" },
    { PLAT_AA_OFF, 1, "60 fps, AA off" },
    { PLAT_AA_2X, 1, "60 fps, AA 2x" },
};
#define BENCH_CONFIGS ((s32) ARRAY_COUNT(sBenchConfigs))

/*
 * The switch bench's experimental switches (plat_switch names, read by the renderer at the start of a frame). Each
 * is raced at its usual setting and, by itself, at its test setting; a switch can set up to three variables together,
 * and several switches can share a variable (one that the others need).
 */
#define BENCH_SWITCH_VARS 3
typedef struct BenchSwitch {
    const char *name; // For the results: what the test setting does.
    const char *env[BENCH_SWITCH_VARS];
    s8 usual, test;
} BenchSwitch;

/* Command list splits (P19) are off by default (#153), early depth (P20) was dropped (#155), the white-line
 * diagnostics (P28) are off for good, and switch bench #176 settled the Phase 7b switches on the New 3DS: the depth
 * clear as a memory fill (P18), the cheaper per-draw settings (P23) and edge trimming dropping what lies wholly
 * outside (P22) are on by default, while textures in VRAM (P17), cache flushes as a system call (P24) and VFP vector
 * transforms (P27) gained nothing and stay off. The Old 3DS (roadmap O8) is another matter: its renderer is CPU-bound
 * on a slower core without a second level cache. There VFP vectors won (#238), and the texture hash kept for 8 frames
 * and cache flushes by system call won together (#241), so all three are on by default on an Old 3DS; redrawing the
 * bottom screen's race HUD less often gained nothing (#241). Rendering ahead by 2 frames won on both of its renderer
 * cores (#244, #245), so it is the Old 3DS default and the bench pins it there. In #251 a steady 20 fps cap matched
 * or beat 30 with far fewer slower frames, so it is the Old 3DS default and the bench races at 20 there. The New
 * 3DS's third top screen buffer (P35) was at most 1 fps ahead but took memory textures needed (#252), so it stays
 * off there. The frame pacer (gfx_c3d.c) still cost frames on two tracks of three, so it is raced off against on.
 * The Old 3DS renderer cuts (P38) each won in #255 (Jungle Falls 24.7 fps with all of them, 20.4 to 22.4 with one
 * off) and the walk and draw breakdowns cost 2.5 to 4 fps there, so those rows are gone. The 20 fps cap now holds
 * (src/video.c; #255 ran 60% of its frames at 30 under it), so 30 is raced against it. This list is the Old 3DS's:
 * its usual 20 fps would pin a New 3DS to 20 too. The GPU route (ideas from other 3DS ports, shelved on the New 3DS
 * after #237 where it gained nothing on a GPU-bound frame): GPU vertex maths (P30) by itself, then with the track's
 * geometry kept on the GPU (P31), then with models recorded the first time they are drawn too (P34); off by default
 * until this bench says otherwise on an Old 3DS.
 * Bench #258/#259 (v21/v22, Old 3DS) settled the rest: the frame pacer stays on (off had more frames slower than 20 fps;
 * still raced, as the bench needs one row),
 * the 20 fps cap stays (30 reached 30 on only 11% of frames), and the GPU route gained nothing there either (P31 equal,
 * P34 6.7 ms more renderer time, P30 tunnel walls missing as on the New 3DS, #234), so it is off and out of the bench.
 * Bench #263 (v24, System 80%): the two-core renderer (P21) ran 29 fps against 20-21, with 93% of frames at 30, and
 * showed the 20 fps cap had never held on the port (its code sat in the N64 branch of src/video.c). So 30 is the Old
 * 3DS's cap and the bench races at 30 there; P21 is still raced while its best renderer core is found.
 * The two-core renderer is now the Old 3DS default, raced off; with the GPU mostly idle (7-9 ms of a 33 ms frame) the
 * looks an Old 3DS has off for speed are raced on one at a time (anti-aliasing 2x, Balanced widescreen, colour boost),
 * and render ahead 1, as the split already keeps two frames in flight.
 * Bench #266 (v27, Jungle Falls): the two-core renderer off fell to 21 fps, so it is out of the bench; anti-aliasing 2x,
 * Balanced widescreen and render ahead 1 each kept 97-98% of frames at 30, so an Old 3DS now races with them as the
 * New 3DS does, and each is raced back to the old setting on the heavier tracks. Colour boost cost nothing (a look,
 * left to the player).
 * The two-core renderer's backend is woken once 16 KB of draws are queued rather than for every draw (each wake-up is
 * a system call on both cores); raced against waking it every time.
 * Back faces culled in the walk (P40, DKR_CPU_CULL, on by default on an Old 3DS) is raced off against on: it stops
 * a command's double sided triangles splitting its draw from the one sided ones (about one draw in eight).
 * The list ends with an empty entry, so it may be otherwise empty. */
static const BenchSwitch sSwitches[] = {
    { "backend woken for every draw (P21)", { "DKR_SPLIT_WAKE_KB", NULL }, 16, 0 },
    { "render ahead 2", { "BENCH_RENDER_AHEAD", NULL }, 1, 2 },
    { "anti-aliasing off", { "BENCH_AA", NULL }, PLAT_AA_2X, PLAT_AA_OFF },
    { "the N64's 4:3", { "BENCH_WIDESCREEN", NULL }, 2, 0 },
    { "back faces culled by the GPU, not the walk (P40 off)", { "DKR_CPU_CULL", NULL }, 1, 0 },
    { NULL, { NULL, NULL, NULL }, 0, 0 },
};
#define SWITCH_CONFIGS ((s32) ARRAY_COUNT(sSwitches)) // The usual settings, then each switch flipped
/* With two switches or more, a last race flips them all together: gains don't always add up. */
#define SWITCH_ALL (SWITCH_CONFIGS > 2 ? SWITCH_CONFIGS : -1)
#define SWITCH_RACES (SWITCH_CONFIGS + (SWITCH_ALL >= 0))
#define MAX_CONFIGS 16

static s32 sMode; // enum Mode

static s32 config_count(void);
static const char *config_name(s32 config);

/* Sets the race's settings: config -1 puts back the player's render settings and every switch's own value. */
static void apply_config(s32 config) {
    s32 i, k;
    if (config < 0 || config >= config_count()) {
        plat_set_gpu_config(-1);
        plat_set_60fps(-1);
        plat_clear_switch_overrides();
        return;
    }
    if (sMode == MODE_SWITCH) {
        // The menus' settings have no say in the bench's races (user rule, 2026-10-29): every race draws with the
        // bench's own render settings: anti-aliasing 2x, 60 fps, Balanced widescreen and render ahead 1 on a New 3DS;
        // anti-aliasing 2x, 30 fps, Balanced widescreen and render ahead 1 on an Old 3DS;
        // 400 pixels, no 3D, mipmaps on, no colour boost, edge trimming on, GPU vertex maths off. Only
        // where the renderer runs (an Old 3DS's Renderer row) needs a restart, so that one is the player's, and the
        // results' graphics column says which. Then every variable at its usual setting, then the race's switches: a
        // variable several switches share (the GPU route's rows) keeps the race's test setting.
        static const struct {
            const char *name;
            s8 value;
        } sBenchRender[] = {
            { "BENCH_WIDE800", 0 },     { "BENCH_MIPMAPS", 1 },  { "BENCH_COLOUR", 0 },    { "BENCH_GUARD_BAND", 1 },
            { "DKR_STEREO3D", 0 },      { "DKR_GPU_TRANSFORM", 0 }, { "DKR_TRACK_MESH", 0 }, { "DKR_MODEL_MESH", 0 },
        };
        plat_set_60fps(!plat_old_3ds());
        for (i = 0; i < (s32) ARRAY_COUNT(sBenchRender); i++) {
            plat_override_switch(sBenchRender[i].name, sBenchRender[i].value);
        }
        plat_override_switch("DKR_FPS_PIN", plat_old_3ds() ? 30 : 60);
        plat_override_switch("BENCH_RENDER_AHEAD", 1);
        plat_override_switch("BENCH_WIDESCREEN", 2);
        plat_override_switch("BENCH_AA", PLAT_AA_2X);
        for (i = 0; i < SWITCH_CONFIGS - 1; i++) {
            for (k = 0; k < BENCH_SWITCH_VARS && sSwitches[i].env[k] != NULL; k++) {
                plat_override_switch(sSwitches[i].env[k], sSwitches[i].usual);
            }
        }
        for (i = 0; i < SWITCH_CONFIGS - 1; i++) {
            if (config == i + 1 || config == SWITCH_ALL) {
                for (k = 0; k < BENCH_SWITCH_VARS && sSwitches[i].env[k] != NULL; k++) {
                    plat_override_switch(sSwitches[i].env[k], sSwitches[i].test);
                }
            }
        }
        plat_set_gpu_config(plat_switch("BENCH_AA", PLAT_AA_OFF)); // The bench's anti-aliasing, or a row's
        return;
    }
    plat_set_gpu_config(sBenchConfigs[config].aa);
    plat_set_60fps(sBenchConfigs[config].fps60);
}

static void set_phase(s32 phase);
static void turn_off(const char *why);

enum Phase {
    PHASE_OFF,
    PHASE_TITLE,    // Waiting for the title screen to start the next race from.
    PHASE_LOADING,  // Race requested, waiting for it to load.
    PHASE_COUNTDOWN,
    PHASE_RACING,
    PHASE_FINISHED, // The kart crossed the line (or the race was stopped); lingering before leaving.
    PHASE_LEAVING   // Asked the game to go back to the title screen.
};

static const char *const sCharacterNames[NUMBER_OF_CHARACTERS] = {
    "Krunch", "Bumper", "Tiptup", "Conker", "Timber", "Banjo", "Drumstick", "Pipsy", "T.T.", "Diddy",
};
static const char *const sVehicleNames[3] = { "car", "hover", "plane" };

enum Request { REQUEST_NONE, REQUEST_START, REQUEST_STOP, REQUEST_BENCH, REQUEST_QUICK, REQUEST_BOSS, REQUEST_SWITCH };

typedef struct ModeInfo {
    const char *name;        // For the bottom screen and the results.
    const char *stateFile;
    const char *resultsFile;
    const char *extraColumn; // The results file's column after the full run's ones, or NULL.
    const char *env;         // Turns it on at every start.
} ModeInfo;

static const ModeInfo sModes[MODE_COUNT] = {
    { "Test run", STATE_FILE, RESULTS_FILE, NULL, "DKR_AUTORUN" },
    { "GPU benchmark", BENCH_STATE_FILE, BENCH_RESULTS_FILE, "config,pct_60fps", "DKR_AUTORUN_BENCH" },
    { "Quick test", QUICK_STATE_FILE, QUICK_RESULTS_FILE, "build", "DKR_AUTORUN_QUICK" },
    { "Boss fights", BOSS_STATE_FILE, BOSS_RESULTS_FILE, "build", "DKR_AUTORUN_BOSS" },
    { "Switch bench", SWITCH_STATE_FILE, SWITCH_RESULTS_FILE, "config,pct_60fps", "DKR_AUTORUN_SWITCH" },
};

/* The benchmarks: their races each have a config (an index into sBenchConfigs, or a switch set). */
static s32 is_bench(void) {
    return sMode == MODE_BENCH || sMode == MODE_SWITCH;
}

static s32 config_count(void) {
    return sMode == MODE_SWITCH ? SWITCH_RACES : BENCH_CONFIGS;
}

static const char *config_name(s32 config) {
    if (config < 0 || config >= config_count()) {
        return "?";
    }
    if (sMode == MODE_SWITCH) {
        return config == 0 ? "usual settings" : config == SWITCH_ALL ? "all switches together" : sSwitches[config - 1].name;
    }
    return sBenchConfigs[config].name;
}

static s32 sEnabled = -1;
static s32 sInitialised;
static volatile s32 sRequest;
static s32 sQuitPending;
static double sFpsSum;
static s32 sFpsCount;
static s32 sPhase;
static Race sRaces[MAX_RACES];
static s32 sNumRaces;
static s32 sNext;  // Index of the race being run, or the next one.
static s32 sRun;   // Run number, counted up each time the list starts over.
static u32 sConfigHash;
static s32 sTimeoutSeconds;
static s32 sTitleFrames;
static unsigned long long sPhaseStartNs;
static unsigned long long sLoadNs;
static s32 sRaceRetraces;  // Game time while racing, in 60ths of a second.
static s32 sStuckRetraces; // Game time since the kart last got further than ever along the course.
// The most checkpoints passed this race. A kart that goes back past a checkpoint loses one, so a kart circling over
// a checkpoint passes it again and again without getting anywhere: only a new best counts as progress.
static s32 sBestProgress;
static f32 sLastPosition[3]; // The kart's position last frame, for saying where it left the race.
static s32 sDriving;         // The kart has got going (a boss first talks to the player for a while).
static s32 sRaceLevel;       // The level the kart got going in.
static s32 sUnsticks;        // Times this race the kart was put at the next checkpoint.
static s32 sLastUnstick;     // Lap * 1000 + the checkpoint the kart was last put at.
static char sUnstuckAt[64]; // Where, as "lap:checkpoint" pairs.
static s32 sLingerRetraces;
static const char *sOutcome;
static s32 sFinished, sProblems;
static ProfStats sRaceStats;
/* The sound's gap and drop counters when the race started (roadmap P9 is judged by them). */
static u32 sAudioGapsStart, sAudioDropsStart;

static s32 str_equal(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void make_path(char *out, s32 size, const char *name) {
    plat_snprintf(out, size, "%s/%s", plat_data_dir(), name);
}

static const char *state_file(void) {
    return sModes[sMode].stateFile;
}

static const char *results_file(void) {
    return sModes[sMode].resultsFile;
}

/* Appends text to a file in the data directory. */
static void append_file(const char *name, const char *text) {
    char path[256];
    unsigned int size = 0, length = strlen(text);
    char *old, *buffer;

    make_path(path, sizeof(path), name);
    old = plat_load_file(path, &size);
    buffer = plat_malloc(size + length);
    if (buffer == NULL) {
        plat_free(old);
        return;
    }
    if (old != NULL) {
        bcopy(old, buffer, size);
        plat_free(old);
    } else {
        size = 0;
    }
    bcopy(text, buffer + size, length);
    plat_save_file(path, buffer, size + length);
    plat_free(buffer);
}

static void hash_text(const char *text) {
    if (text == NULL) {
        text = "-";
    }
    for (; *text; text++) {
        sConfigHash = sConfigHash * 31 + (u8) *text;
    }
    sConfigHash = sConfigHash * 31 + ';';
}

/* Parses "all" or a comma separated list into ids. Returns the count, or -1 for "all" / not set. */
static s32 parse_list(const char *list, s8 *ids, s32 maxIds) {
    s32 count = 0;
    if (list == NULL || list[0] == 'a') {
        return -1;
    }
    while (*list && count < maxIds) {
        ids[count++] = port_atoi(list);
        while (*list && *list != ',') {
            list++;
        }
        if (*list == ',') {
            list++;
        }
    }
    return count;
}

/*
 * The short runs: each track a number of times with one character. The benchmark races each track once with each of
 * its render settings; the quick test and the boss fights leave the settings as they are.
 */
static void build_short_list(void) {
    static const s8 sBenchTracks[] = { 29, 10, 31 };        // Jungle Falls, Crescent Island, Haunted Woods
    static const s8 sQuickTracks[] = { 31, 29, 10, 8, 13 }; // Haunted Woods, Jungle Falls, Crescent Island,
                                                            // Whale Bay, EverFrost Peak
    static const s8 sBossTracks[] = {
        ASSET_LEVEL_TRICKYTOPS1, ASSET_LEVEL_BLUEY1, ASSET_LEVEL_BUBBLER1, ASSET_LEVEL_SMOKEY1, ASSET_LEVEL_WIZPIG1,
        ASSET_LEVEL_TRICKYTOPS2, ASSET_LEVEL_BLUEY2, ASSET_LEVEL_BUBBLER2, ASSET_LEVEL_SMOKEY2, ASSET_LEVEL_WIZPIG2,
    };
    s32 quick = sMode == MODE_QUICK, boss = sMode == MODE_BOSS;
    s32 numBenchConfigs = config_count();
    const s8 *defaultTracks = boss ? sBossTracks : quick ? sQuickTracks : sBenchTracks;
    s32 numDefault = boss    ? (s32) ARRAY_COUNT(sBossTracks)
                     : quick ? (s32) ARRAY_COUNT(sQuickTracks)
                             : (s32) ARRAY_COUNT(sBenchTracks);
    const char *prefix = boss                    ? "DKR_AUTORUN_BOSS"
                         : quick                 ? "DKR_AUTORUN_QUICK"
                         : sMode == MODE_SWITCH ? "DKR_AUTORUN_SWITCH"
                                                 : "DKR_AUTORUN_BENCH";
    char name[48];
    s8 tracks[MAX_TRACKS], character[1], configs[MAX_CONFIGS];
    s32 numTracks, numLevels, numWorlds, t, pass, passes, numConfigs;

    plat_snprintf(name, sizeof(name), "%s_TRACKS", prefix);
    numTracks = parse_list(plat_getenv(name), tracks, MAX_TRACKS);
    if (numTracks < 0) {
        for (numTracks = 0; numTracks < numDefault; numTracks++) {
            tracks[numTracks] = defaultTracks[numTracks];
        }
    }
    plat_snprintf(name, sizeof(name), "%s_CHARACTER", prefix);
    if (parse_list(plat_getenv(name), character, 1) != 1 || character[0] < 0 ||
        character[0] >= NUMBER_OF_CHARACTERS) {
        character[0] = 1; // Bumper
    }
    plat_snprintf(name, sizeof(name), "%s_CONFIGS", prefix);
    numConfigs = parse_list(plat_getenv(name), configs, numBenchConfigs);
    if (numConfigs < 0) {
        for (numConfigs = 0; numConfigs < numBenchConfigs; numConfigs++) {
            configs[numConfigs] = numConfigs;
        }
    }
    passes = numConfigs;
    if (quick || boss) {
        plat_snprintf(name, sizeof(name), "%s_PASSES", prefix);
        passes = plat_getenv(name) ? port_atoi(plat_getenv(name)) : boss ? BOSS_PASSES : QUICK_PASSES;
    }
    level_count(&numLevels, &numWorlds);
    sNumRaces = 0;
    for (t = 0; t < numTracks; t++) {
        if (tracks[t] < 0 || tracks[t] >= numLevels) {
            continue;
        }
        for (pass = 0; pass < passes && sNumRaces < MAX_RACES; pass++) {
            sRaces[sNumRaces].track = tracks[t];
            sRaces[sNumRaces].character = character[0];
            sRaces[sNumRaces].vehicle = leveltable_vehicle_default(tracks[t]);
            sRaces[sNumRaces].config = (quick || boss) ? -1 : configs[pass];
            sNumRaces++;
        }
    }
}

static void build_race_list(void) {
    s8 tracks[MAX_TRACKS], characters[NUMBER_OF_CHARACTERS];
    s32 numTracks, numCharacters, c, t, v, usable, i;
    s32 numLevels, numWorlds;
    s32 allVehicles = plat_getenv("DKR_AUTORUN_VEHICLES") != NULL && plat_getenv("DKR_AUTORUN_VEHICLES")[0] == 'a';

    if (sMode != MODE_FULL) {
        build_short_list();
        return;
    }
    numTracks = parse_list(plat_getenv("DKR_AUTORUN_TRACKS"), tracks, MAX_TRACKS);
    if (numTracks < 0) {
        // The tracks menu's own list (every world's four tracks, Future Fun Land included).
        s8 *ids = (s8 *) get_misc_asset(ASSET_MISC_MAIN_TRACKS_IDS);
        for (numTracks = 0; ids[numTracks] != -1 && numTracks < MAX_TRACKS; numTracks++) {
            tracks[numTracks] = ids[numTracks];
        }
    }
    numCharacters = parse_list(plat_getenv("DKR_AUTORUN_CHARACTERS"), characters, NUMBER_OF_CHARACTERS);
    if (numCharacters < 0) {
        for (numCharacters = 0; numCharacters < NUMBER_OF_CHARACTERS; numCharacters++) {
            characters[numCharacters] = numCharacters;
        }
    }
    level_count(&numLevels, &numWorlds);
    sNumRaces = 0;
    for (c = 0; c < numCharacters; c++) {
        if (characters[c] < 0 || characters[c] >= NUMBER_OF_CHARACTERS) {
            continue;
        }
        for (t = 0; t < numTracks; t++) {
            if (tracks[t] < 0 || tracks[t] >= numLevels) {
                continue;
            }
            usable = leveltable_vehicle_usable(tracks[t]);
            for (i = 0; i < 3; i++) {
                // The usual vehicle first, then the others the track allows.
                v = (i == 0) ? leveltable_vehicle_default(tracks[t]) : i - (i <= leveltable_vehicle_default(tracks[t]));
                if (i > 0 && (!allVehicles || !(usable & (1 << v)))) {
                    continue;
                }
                if (sNumRaces < MAX_RACES) {
                    sRaces[sNumRaces].track = tracks[t];
                    sRaces[sNumRaces].character = characters[c];
                    sRaces[sNumRaces].vehicle = v;
                    sRaces[sNumRaces].config = -1;
                    sNumRaces++;
                }
            }
        }
    }
}

/* running: the race under way (-1: none). recorded: its result is already in the results file. */
static void save_state_recorded(s32 running, s32 recorded) {
    char text[176];
    char path[256];
    plat_snprintf(text, sizeof(text),
                  "active=%d\nconfig=%08x\nrun=%d\nnext=%d\nrunning=%d\nrecorded=%d\nfinished=%d\nproblems=%d\n",
                  (int) (sEnabled > 0), (unsigned int) sConfigHash, (int) sRun, (int) sNext, (int) running,
                  (int) recorded, (int) sFinished, (int) sProblems);
    make_path(path, sizeof(path), state_file());
    plat_save_file(path, text, strlen(text));
}

static void save_state(s32 running) {
    save_state_recorded(running, FALSE);
}

static s32 state_value(const char *text, const char *key, s32 fallback) {
    s32 keyLength = strlen(key);
    const char *line = text;
    while (line != NULL && *line) {
        s32 i;
        for (i = 0; i < keyLength && line[i] == key[i]; i++) {}
        if (i == keyLength && line[keyLength] == '=') {
            line += keyLength + 1;
            if (str_equal(key, "config")) {
                u32 v = 0;
                for (; (*line >= '0' && *line <= '9') || (*line >= 'a' && *line <= 'f'); line++) {
                    v = v * 16 + ((*line <= '9') ? *line - '0' : *line - 'a' + 10);
                }
                return (s32) v;
            }
            return port_atoi(line);
        }
        line = strchr(line, '\n');
        if (line != NULL) {
            line++;
        }
    }
    return fallback;
}

static const char *track_name(s32 track) {
    const char *name = level_name(track);
    return (name != NULL) ? name : "?";
}

static void record_problem(const char *what) {
    char text[256];
    Race *race = &sRaces[sNext];
    plat_snprintf(text, sizeof(text), "run %d race %d/%d: %s, %s, %s: %s\n", (int) sRun, (int) sNext + 1,
                  (int) sNumRaces, track_name(race->track), sCharacterNames[(s32) race->character],
                  sVehicleNames[(s32) race->vehicle], what);
    append_file(PROBLEMS_FILE, text);
    port_log("autorun: %s", text);
}

static double ms(const ProfStats *s, s32 stage, u32 frames) {
    return frames ? s->stageNs[stage] / 1e6 / frames : 0.0;
}

static s32 pct(u32 part, u32 whole) {
    return whole ? (s32) (((unsigned long long) part * 100 + whole / 2) / whole) : 0;
}

static const char *aa_label(void) {
    s32 aa = plat_antialias();
    return aa == PLAT_AA_4X ? "4x" : aa == PLAT_AA_2X ? "2x" : "off";
}

/* The graphics settings the frames are drawn with now, for the results (e.g. "aa4x/60fps/ahead2"; an Old 3DS
 * renderer on the system core adds its share, e.g. "/sys50"). */
static const char *graphics_setup(void) {
    static char text[48];
    s32 share = plat_renderer_syscore_share();
    plat_snprintf(text, sizeof(text), "aa%s/%dfps/ahead%d", aa_label(), 60 / plat_frame_retraces(),
                  plat_render_ahead());
    if (share > 0) {
        s32 len = (s32) strlen(text);
        plat_snprintf(text + len, sizeof(text) - len, "/sys%d", (int) share);
    }
    if (gfx_split_active()) {
        s32 len = (s32) strlen(text);
        plat_snprintf(text + len, sizeof(text) - len, "/split");
    }
    return text;
}

/* Writes the results line of the race that just ended. stats may be NULL (the race never got going). */
static void record_result(const char *outcome, s32 position, const ProfStats *stats) {
    char path[256];
    char line[640];
    unsigned int size;
    void *existing;
    Race *race = &sRaces[sNext];
    ProfStats empty;
    u32 slow, i;
    s32 length;

    make_path(path, sizeof(path), results_file());
    existing = plat_load_file(path, &size);
    if (existing == NULL) {
        char header[640];
        plat_snprintf(header, sizeof(header),
                      "run,race,track_id,track,character,vehicle,outcome,race_s,position,load_s,frames,fps,"
                      "pct_30fps,pct_20fps,pct_slower,worst_frame_ms,hitches,logic_ms,scene_ms,frame_wait_ms,"
                      "rdp_ms,texture_ms,gpu_wait_ms,gpu_draw_ms,audio_ms,worst_build_ms,worst_render_ms,"
                      "worst_gpu_ms,triangles,draws,texture_uploads,idle_pct,limit,unsticks,unstuck_at%s%s,"
                      "audio_gaps,audio_drops,sound,graphics,low1pct_fps,paced_60fps_pct,paced_30fps_pct,"
                      "paced_20fps_pct,paced_slower_pct\n",
                      sModes[sMode].extraColumn ? "," : "",
                      sModes[sMode].extraColumn ? sModes[sMode].extraColumn : "");
        append_file(results_file(), header);
    }
    plat_free(existing);
    if (stats == NULL) {
        bzero(&empty, sizeof(empty));
        stats = &empty;
    }
    for (slow = 0, i = 4; i < PROF_RATE_BUCKETS; i++) {
        slow += stats->rateFrames[i];
    }
    plat_snprintf(
        line, sizeof(line),
        "%d,%d,%d,%s,%s,%s,%s,%.1f,%d,%.1f,%u,%.2f,%d,%d,%d,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%u,%u,%u,"
        "%u,%u,%d,%s,%d,%s",
        (int) sRun, (int) sNext + 1, (int) race->track, track_name(race->track),
        sCharacterNames[(s32) race->character], sVehicleNames[(s32) race->vehicle], outcome, sRaceRetraces / 60.0,
        (int) position, sLoadNs / 1e9, stats->gameFrames,
        stats->elapsedNs ? stats->gameFrames * 1e9 / stats->elapsedNs : 0.0,
        (int) pct(stats->rateFrames[1] + stats->rateFrames[2], stats->gameFrames),
        (int) pct(stats->rateFrames[3], stats->gameFrames), (int) pct(slow, stats->gameFrames), stats->worstFrameUs / 1000, stats->hitches,
        ms(stats, PROF_LOGIC, stats->gameFrames), ms(stats, PROF_SCENE, stats->gameFrames),
        ms(stats, PROF_FRAME_WAIT, stats->gameFrames), ms(stats, PROF_RDP, stats->renderedFrames),
        ms(stats, PROF_TEXTURE, stats->renderedFrames), ms(stats, PROF_GPU_WAIT, stats->renderedFrames),
        ms(stats, PROF_GPU_DRAW, stats->renderedFrames), ms(stats, PROF_AUDIO, stats->gameFrames),
        stats->worstBuildUs / 1000, stats->worstRenderUs / 1000, stats->worstGpuUs / 1000,
        stats->renderedFrames ? stats->counters[PROF_TRIANGLES] / stats->renderedFrames : 0,
        stats->renderedFrames ? stats->counters[PROF_DRAWS] / stats->renderedFrames : 0,
        stats->counters[PROF_TEXTURE_UPLOADS], (int) pct((u32) (stats->idleNs / 1000), (u32) (stats->elapsedNs / 1000)),
        stats->gameFrames ? prof_limit(stats) : "-", (int) sUnsticks, sUnstuckAt);
    length = strlen(line);
    if (is_bench()) {
        // The settings the race was meant to have (the log says what the renderer made of them), and the frames at 60.
        plat_snprintf(line + length, sizeof(line) - length, ",%d,%d", (int) race->config,
                      (int) pct(stats->rateFrames[1], stats->gameFrames));
    } else if (sMode == MODE_QUICK || sMode == MODE_BOSS) {
        plat_snprintf(line + length, sizeof(line) - length, ",%s", plat_build_version());
    }
    length = strlen(line);
    // After the mode's own column, so the columns before keep their places in older results files.
    // Then the frame pacing: the 1% low, and the share of game frames that took one retrace, two, three or more (measured
    // between frames, so unlike pct_30fps it counts the frames the game meant to show at 60 but couldn't).
    plat_snprintf(line + length, sizeof(line) - length, ",%u,%u,%s,%s,%.1f,%d,%d,%d,%d\n",
                  stats == &empty ? 0 : (unsigned) (gPlatCounters[PLAT_COUNTER_AUDIO_UNDERRUN] - sAudioGapsStart),
                  stats == &empty ? 0 : (unsigned) (gPlatCounters[PLAT_COUNTER_AUDIO_DROPPED] - sAudioDropsStart),
                  plat_audio_setup(), graphics_setup(), stats->low1FrameUs ? 1e6 / stats->low1FrameUs : 0.0,
                  (int) pct(stats->pacedFrames[0], stats->gameFrames), (int) pct(stats->pacedFrames[1], stats->gameFrames),
                  (int) pct(stats->pacedFrames[2], stats->gameFrames), (int) pct(stats->pacedFrames[3], stats->gameFrames));
    append_file(results_file(), line);
    port_log("autorun: %s", line);
    if (str_equal(outcome, "finished")) {
        sFinished++;
    } else {
        sProblems++;
    }
}

/*
 * Whether the test run is on: DKR_AUTORUN is set, or the bottom screen's button started a run that hasn't ended
 * (autorun_state.txt says active=1, so it survives a crash). Safe to call at any time, before the game is up.
 */
s32 port_autorun_enabled(void) {
    if (sEnabled < 0) {
        const char *value = plat_getenv("DKR_AUTORUN");
        char path[256];
        char text[200];
        s32 length;

        s32 mode;

        // A short run first (asked for, or one that hasn't ended), then the full run.
        sMode = MODE_FULL;
        for (mode = MODE_FULL + 1; mode < MODE_COUNT && sMode == MODE_FULL; mode++) {
            const char *env = plat_getenv(sModes[mode].env);
            if (env != NULL && env[0] != '0') {
                sMode = mode;
            }
        }
        for (mode = MODE_FULL + 1; mode < MODE_COUNT && sMode == MODE_FULL; mode++) {
            make_path(path, sizeof(path), sModes[mode].stateFile);
            length = plat_load_file_start(path, text, sizeof(text) - 1);
            if (length > 0) {
                text[length] = '\0';
                if (state_value(text, "active", 0) != 0) {
                    sMode = mode;
                }
            }
        }
        sEnabled = sMode != MODE_FULL || (value != NULL && value[0] != '0');
        make_path(path, sizeof(path), STATE_FILE);
        length = plat_load_file_start(path, text, sizeof(text) - 1);
        if (!sEnabled && length > 0) {
            text[length] = '\0';
            sEnabled = state_value(text, "active", 0) != 0;
        }
    }
    return sEnabled;
}

static void init(void) {
    char path[256];
    unsigned int size;
    char *state, *hang;
    const char *names[] = { "DKR_AUTORUN_TRACKS", "DKR_AUTORUN_CHARACTERS", "DKR_AUTORUN_VEHICLES" };
    const char *benchNames[] = { "DKR_AUTORUN_BENCH_TRACKS", "DKR_AUTORUN_BENCH_CHARACTER",
                                 "DKR_AUTORUN_BENCH_CONFIGS" };
    const char *quickNames[] = { "DKR_AUTORUN_QUICK_TRACKS", "DKR_AUTORUN_QUICK_CHARACTER",
                                 "DKR_AUTORUN_QUICK_PASSES" };
    const char *bossNames[] = { "DKR_AUTORUN_BOSS_TRACKS", "DKR_AUTORUN_BOSS_CHARACTER", "DKR_AUTORUN_BOSS_PASSES" };
    const char *switchNames[] = { "DKR_AUTORUN_SWITCH_TRACKS", "DKR_AUTORUN_SWITCH_CHARACTER",
                                  "DKR_AUTORUN_SWITCH_CONFIGS" };
    s32 i;

    sInitialised = TRUE;
    sConfigHash = 0;
    if (sMode == MODE_BENCH) {
        hash_text("gpu bench");
        for (i = 0; i < (s32) ARRAY_COUNT(benchNames); i++) {
            hash_text(plat_getenv(benchNames[i]));
        }
    } else if (sMode == MODE_SWITCH) {
        hash_text("switch bench");
        for (i = 0; i < (s32) ARRAY_COUNT(switchNames); i++) {
            hash_text(plat_getenv(switchNames[i]));
        }
        for (i = 1; i < SWITCH_RACES; i++) {
            hash_text(config_name(i)); // A new switch starts a new run.
        }
    } else if (sMode == MODE_QUICK) {
        hash_text("quick test");
        for (i = 0; i < (s32) ARRAY_COUNT(quickNames); i++) {
            hash_text(plat_getenv(quickNames[i]));
        }
    } else if (sMode == MODE_BOSS) {
        hash_text("boss fights");
        for (i = 0; i < (s32) ARRAY_COUNT(bossNames); i++) {
            hash_text(plat_getenv(bossNames[i]));
        }
    } else {
        for (i = 0; i < (s32) ARRAY_COUNT(names); i++) {
            hash_text(plat_getenv(names[i]));
        }
    }
    build_race_list();
    sTimeoutSeconds = plat_getenv("DKR_AUTORUN_TIMEOUT") ? port_atoi(plat_getenv("DKR_AUTORUN_TIMEOUT")) : 300;
    sRun = 1;
    sNext = 0;
    sFinished = 0;
    sProblems = 0;
    sFpsSum = 0.0;
    sFpsCount = 0;

    make_path(path, sizeof(path), state_file());
    state = plat_load_file(path, &size);
    if (state != NULL && plat_getenv("DKR_AUTORUN_RESTART") == NULL) {
        char *text = plat_malloc(size + 1);
        s32 running;
        bcopy(state, text, size);
        text[size] = '\0';
        running = state_value(text, "running", -1);
        if ((u32) state_value(text, "config", 0) == sConfigHash) {
            sRun = state_value(text, "run", 1);
            sNext = state_value(text, "next", 0);
            sFinished = state_value(text, "finished", 0);
            sProblems = state_value(text, "problems", 0);
            if (running >= 0 && running < sNumRaces && state_value(text, "recorded", 0)) {
                // The last launch stopped after the race's result was written (the results screen): carry on after it.
                make_path(path, sizeof(path), HANG_FILE);
                plat_save_file(path, "", 0);
                sNext = running + 1;
                if (sNext >= sNumRaces) {
                    plat_free(text);
                    plat_free(state);
                    turn_off("finished");
                    return;
                }
            } else if (running >= 0 && running < sNumRaces) {
                // The last launch never got to the end of this race: it crashed, hung or was closed.
                const char *outcome = "crash_or_closed";
                sNext = running;
                make_path(path, sizeof(path), HANG_FILE);
                hang = plat_load_file(path, &size);
                if (hang != NULL) {
                    char what[200];
                    outcome = "hang";
                    plat_snprintf(what, sizeof(what), "hang: %.*s", (int) (size < 150 ? size : 150), hang);
                    for (i = 0; what[i]; i++) {
                        if (what[i] == '\n' || what[i] == '\r') {
                            what[i] = ' ';
                        }
                    }
                    record_problem(what);
                    plat_free(hang);
                    plat_save_file(path, "", 0);
                } else {
                    record_problem("the game crashed or was closed during this race");
                }
                record_result(outcome, 0, NULL);
                sNext = running + 1;
                if (sNext >= sNumRaces) {
                    // That was the last race: the run is over.
                    plat_free(text);
                    plat_free(state);
                    turn_off("finished");
                    return;
                }
            }
            if (sNext >= sNumRaces) {
                // The previous run had finished: start a new one.
                sNext = 0;
                sRun++;
                sFinished = 0;
                sProblems = 0;
            }
        } else {
            sRun = state_value(text, "run", 0) + 1;
        }
        plat_free(text);
    }
    plat_free(state);
    port_log("autorun: run %d, %d races, starting at %d", (int) sRun, (int) sNumRaces, (int) sNext + 1);
    set_phase(PHASE_TITLE);
    if (sNumRaces == 0) {
        sPhase = PHASE_OFF;
        sEnabled = 0;
    }
    save_state(-1);
}

/* Nonzero while a test run is going (or about to start). */
s32 port_autorun_active(void) {
    if (!port_autorun_enabled()) {
        return FALSE;
    }
    if (!sInitialised) {
        init();
    }
    return sEnabled && sPhase != PHASE_OFF;
}

/* The switch bench's switches: 0 when it has nothing to test (the Test run menu says so). */
s32 port_autorun_switch_count(void) {
    return SWITCH_CONFIGS - 1;
}

/*
 * From any thread (the bottom screen's buttons): start the test run (1), the GPU benchmark (2), the quick test (3), the
 * boss fights (4) or the switch bench (5), or stop whichever is going (0), at the next game frame.
 */
void port_autorun_request(s32 start) {
    sRequest = start == 5   ? REQUEST_SWITCH
               : start == 4 ? REQUEST_BOSS
               : start == 3 ? REQUEST_QUICK
               : start == 2 ? REQUEST_BENCH
               : start      ? REQUEST_START
                            : REQUEST_STOP;
}

static void set_phase(s32 phase) {
    sPhase = phase;
    sPhaseStartNs = plat_time_ns();
}

static void show_status(void) {
    char text[200];
    char line[3][48];
    char bar[160];
    Race *race = &sRaces[sNext < sNumRaces ? sNext : sNumRaces - 1];
    Object *obj = get_racer_object_by_port(PLAYER_ONE);
    LevelHeader *header = level_header();
    const char *what;
    char lap[24];
    s32 i, filled, length;

    if (sNumRaces == 0) {
        return;
    }
    lap[0] = '\0';
    switch (sPhase) {
        case PHASE_LOADING:
            what = "loading";
            break;
        case PHASE_COUNTDOWN:
            what = "countdown";
            break;
        case PHASE_RACING:
            what = lap;
            if (obj != NULL && header != NULL) {
                plat_snprintf(lap, sizeof(lap), "lap %d/%d %d:%02d", obj->racer->lap + 1, header->laps,
                              (int) (sRaceRetraces / 3600), (int) (sRaceRetraces / 60 % 60));
            }
            break;
        case PHASE_FINISHED:
            what = sOutcome;
            break;
        default:
            what = (plat_time_ns() - sPhaseStartNs > 3000000000ULL) ? "(go to the title screen)" : "next";
            break;
    }
    plat_snprintf(line[0], sizeof(line[0]), "%s %d  race %d/%d  ok %d bad %d",
                  sMode == MODE_BENCH    ? "GPU BENCH"
                  : sMode == MODE_SWITCH ? "SWITCH BENCH"
                  : sMode == MODE_QUICK  ? "QUICK TEST"
                  : sMode == MODE_BOSS  ? "BOSS FIGHTS"
                                        : "TEST RUN",
                  (int) sRun,
                  (int) sNext + 1, (int) sNumRaces, (int) sFinished, (int) sProblems);
    if (is_bench()) {
        plat_snprintf(line[1], sizeof(line[1]), "%s, %s %s", track_name(race->track), config_name(race->config),
                      what);
    } else {
        plat_snprintf(line[1], sizeof(line[1]), "%s %s %s %s", sCharacterNames[(s32) race->character],
                      track_name(race->track), sVehicleNames[(s32) race->vehicle], what);
    }
    // The progress bar: green for the races done, grey for the rest, with the percentage after it.
    filled = sNext * 32 / sNumRaces;
    length = plat_snprintf(bar, sizeof(bar), "\x1b[42m");
    for (i = 0; i < 32; i++) {
        if (i == filled) {
            length += plat_snprintf(bar + length, sizeof(bar) - length, "\x1b[47m");
        }
        bar[length++] = ' ';
    }
    bar[length] = '\0';
    plat_snprintf(line[2], sizeof(line[2]), " %3d%%", (int) (sNext * 100 / sNumRaces));
    line[0][39] = line[1][39] = '\0';
    plat_snprintf(text, sizeof(text), "%s\n%s\n%s\x1b[0m%s", line[0], line[1], bar, line[2]);
    prof_set_status(text);
}

/*
 * Title screen: returns the menu result that starts the next race (as the tracks menu would), or 0. Waits a moment
 * first, so the title screen's own loading is over.
 */
s32 port_autorun_title(void) {
    Race *race;
    if (!port_autorun_active()) {
        return 0;
    }
    if (sPhase == PHASE_LEAVING) {
        set_phase(PHASE_TITLE);
    }
    if (sPhase != PHASE_TITLE) {
        return 0;
    }
    if (++sTitleFrames < 30) {
        return 0;
    }
    sTitleFrames = 0;
    race = &sRaces[sNext];
    port_log("autorun: race %d/%d: %s, %s, %s", (int) sNext + 1, (int) sNumRaces, track_name(race->track),
             sCharacterNames[(s32) race->character], sVehicleNames[(s32) race->vehicle]);
    save_state(sNext);
    if (race->config >= 0) {
        apply_config(race->config); // The renderer switches while the race loads.
    }
    set_phase(PHASE_LOADING);
    sRaceRetraces = 0;
    sStuckRetraces = 0;
    sBestProgress = -0x10000;
    sDriving = FALSE;
    sUnsticks = 0;
    sUnstuckAt[0] = '\0';
    sLingerRetraces = 0;
    show_status();
    return port_autorun_setup_race(race->track, race->character, race->vehicle);
}

/* DKR_AUTORUN_TIME_TRIAL: the races are time trials (src/menu.c, port_autorun_setup_race). */
s32 port_autorun_time_trial(void) {
    const char *env = plat_getenv("DKR_AUTORUN_TIME_TRIAL");
    return env != NULL && port_atoi(env) != 0;
}

/* Nonzero while the player's kart is driven by the AI. */
s32 port_autorun_drives(void) {
    return sEnabled > 0 && sPhase >= PHASE_LOADING && sPhase <= PHASE_FINISHED;
}

/* A plain decimal number ("27.86"), as the results file has them. */
static double parse_decimal(const char *text) {
    double value = 0.0, scale = 1.0;
    s32 negative = *text == '-';
    if (negative) {
        text++;
    }
    for (; *text >= '0' && *text <= '9'; text++) {
        value = value * 10 + (*text - '0');
    }
    if (*text == '.') {
        for (text++; *text >= '0' && *text <= '9'; text++) {
            scale /= 10;
            value += (*text - '0') * scale;
        }
    }
    return negative ? -value : value;
}

/* Columns of the short runs' results files that their summaries use. */
enum BenchColumn {
    COL_RUN = 0,
    COL_TRACK = 3,
    COL_CHARACTER = 4,
    COL_OUTCOME = 6,
    COL_RACE_S = 7,
    COL_POSITION = 8,
    COL_FPS = 11,
    COL_PCT_30FPS = 12,
    COL_WORST_FRAME = 15, // ms
    COL_RDP = 20,
    COL_GPU_DRAW = 23,
    COL_AUDIO = 24,
    COL_WORST_GPU = 27,
    COL_LIMIT = 32,
    COL_UNSTICKS = 33,
    COL_EXTRA = 35, // After the test run's unsticks and unstuck_at: "config" (benchmark) or "build" (quick test)
    COL_COUNT,
    COL_AUDIO_GAPS = COL_COUNT, // Only in results written since the sound columns were added (the benchmark: one on).
    COL_PCT_60FPS = COL_COUNT,  // The benchmark's second column of its own.
};
/* The most columns a line has: the benchmark's two, the sound's three, the graphics settings and the frame pacing's
 * four. */
#define MAX_FIELDS (COL_COUNT + 10)

#define BENCH_MAX_TRACKS 8
#define BENCH_LIMITS 3

typedef struct BenchGroup {
    char track[32];
    char build[40]; // Quick test: the run and its build.
    s32 races;
    double fps, pct30, pct60, rdp, gpu, audio, gaps;
    s32 worstFrame, worstGpu;
    s32 pacedRaces; // Races with the frame pacing columns (results written before they were added have none).
    double low1, paced[4];
    s32 limits[BENCH_LIMITS]; // GPU, renderer CPU, game CPU
} BenchGroup;

static const char *const sLimitNames[BENCH_LIMITS] = { "GPU", "renderer CPU", "game CPU" };

static double bench_avg(double sum, s32 count) {
    return count ? sum / count : 0.0;
}

/* A group's frame pacing cells: the 1% low and the share of frames that took one retrace, two, three or more. */
static const char *pacing_cells(const BenchGroup *g) {
    static char text[48];
    if (g->pacedRaces == 0) {
        return "- | -";
    }
    plat_snprintf(text, sizeof(text), "%.1f | %.0f/%.0f/%.0f/%.0f", bench_avg(g->low1, g->pacedRaces),
                  bench_avg(g->paced[0], g->pacedRaces), bench_avg(g->paced[1], g->pacedRaces),
                  bench_avg(g->paced[2], g->pacedRaces), bench_avg(g->paced[3], g->pacedRaces));
    return text;
}

/* Splits a results line into fields in place. Returns the number of fields. */
static s32 split_fields(char *line, const char **fields) {
    s32 numFields = 0;
    char *c;
    fields[numFields++] = line;
    for (c = line; *c && numFields < MAX_FIELDS; c++) {
        if (*c == ',') {
            *c = '\0';
            fields[numFields++] = c + 1;
        }
    }
    return numFields;
}

/*
 * A short run's summary, from the finished races in its results file (so races from before a crash count too), as
 * Markdown. The benchmark: one line per track and render setting, with the GPU time it saves. The quick
 * test: one line per track for the quick test before this one and one for this one, then the difference.
 */
static void bench_summary(char *out, s32 size) {
    static BenchGroup groups[BENCH_MAX_TRACKS][MAX_CONFIGS];
    char path[256];
    char *data, *copy, *line, *next;
    const char *fields[MAX_FIELDS];
    unsigned int dataSize;
    s32 numTracks = 0, length, t, k, i, run, previousRun = 0;
    s32 quick = sMode == MODE_QUICK;

    bzero(groups, sizeof(groups));
    make_path(path, sizeof(path), results_file());
    data = plat_load_file(path, &dataSize);
    if (data == NULL) {
        return;
    }
    // Made into one string, then split into lines and fields in place (twice for the quick test: first to find the
    // run before this one).
    line = plat_malloc(dataSize + 1);
    copy = plat_malloc(dataSize + 1);
    if (line == NULL || copy == NULL) {
        plat_free(line);
        plat_free(copy);
        plat_free(data);
        return;
    }
    bcopy(data, line, dataSize);
    line[dataSize] = '\0';
    bcopy(data, copy, dataSize);
    copy[dataSize] = '\0';
    plat_free(data);
    data = line;
    if (quick) {
        for (line = copy; line != NULL && *line; line = next) {
            next = strchr(line, '\n');
            if (next != NULL) {
                *next++ = '\0';
            }
            if (split_fields(line, fields) < COL_COUNT || !str_equal(fields[COL_OUTCOME], "finished")) {
                continue;
            }
            run = port_atoi(fields[COL_RUN]);
            if (run < sRun && run > previousRun) {
                previousRun = run;
            }
        }
    }
    plat_free(copy);
    for (line = data; line != NULL && *line; line = next) {
        BenchGroup *g;
        s32 numFields, gapsColumn;
        next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        numFields = split_fields(line, fields);
        if (numFields < COL_COUNT || !str_equal(fields[COL_OUTCOME], "finished")) {
            continue; // The header, or a race that didn't finish.
        }
        run = port_atoi(fields[COL_RUN]);
        if (quick) {
            if (run != sRun && (run != previousRun || previousRun == 0)) {
                continue;
            }
            k = run == sRun;
        } else {
            if (run != sRun) {
                continue;
            }
            k = port_atoi(fields[COL_EXTRA]);
            if (k < 0 || k >= config_count()) {
                continue;
            }
        }
        for (t = 0; t < numTracks && !str_equal(groups[t][0].track, fields[COL_TRACK]); t++) {}
        if (t == numTracks) {
            if (numTracks == BENCH_MAX_TRACKS) {
                continue;
            }
            numTracks++;
            plat_snprintf(groups[t][0].track, sizeof(groups[t][0].track), "%s", fields[COL_TRACK]);
        }
        g = &groups[t][k];
        if (quick) {
            for (i = 0; fields[COL_EXTRA][i] != '\0' && fields[COL_EXTRA][i] != '\r' && i < 24; i++) {}
            plat_snprintf(g->build, sizeof(g->build), "run %d, %.*s", (int) run, (int) i, fields[COL_EXTRA]);
        }
        g->races++;
        g->fps += parse_decimal(fields[COL_FPS]);
        g->pct30 += parse_decimal(fields[COL_PCT_30FPS]);
        if (!quick && numFields > COL_PCT_60FPS) {
            g->pct60 += parse_decimal(fields[COL_PCT_60FPS]);
        }
        g->rdp += parse_decimal(fields[COL_RDP]);
        g->gpu += parse_decimal(fields[COL_GPU_DRAW]);
        g->audio += parse_decimal(fields[COL_AUDIO]);
        gapsColumn = quick ? COL_AUDIO_GAPS : COL_AUDIO_GAPS + 1;
        g->gaps += numFields > gapsColumn ? port_atoi(fields[gapsColumn]) : 0;
        // The frame pacing, after the sound's columns and the graphics settings.
        if (numFields > gapsColumn + 8) {
            g->pacedRaces++;
            g->low1 += parse_decimal(fields[gapsColumn + 4]);
            for (i = 0; i < 4; i++) {
                g->paced[i] += parse_decimal(fields[gapsColumn + 5 + i]);
            }
        }
        if (port_atoi(fields[COL_WORST_FRAME]) > g->worstFrame) {
            g->worstFrame = port_atoi(fields[COL_WORST_FRAME]);
        }
        if (port_atoi(fields[COL_WORST_GPU]) > g->worstGpu) {
            g->worstGpu = port_atoi(fields[COL_WORST_GPU]);
        }
        for (i = 0; i < BENCH_LIMITS; i++) {
            if (str_equal(fields[COL_LIMIT], sLimitNames[i])) {
                g->limits[i]++;
            }
        }
    }
    plat_free(data);

    if (quick) {
        if (previousRun == 0) {
            length = plat_snprintf(out, size,
                                   "\n### Quick test (averages of the finished races)\n\n"
                                   "No earlier quick test on this console to compare with; the next one will be.\n\n"
                                   "| track | run, build | races | fps | 1%% low | frames 60/30/20/slower %% | 30 fps | GPU ms "
                                   "| worst GPU | rdp ms | worst frame | limit | audio ms | sound gaps |\n"
                                   "|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|--:|--:|\n");
        } else {
            length = plat_snprintf(out, size,
                                   "\n### Quick test: run %d against run %d (averages of the finished races)\n\n"
                                   "| track | run, build | races | fps | 1%% low | frames 60/30/20/slower %% | 30 fps | GPU ms "
                                   "| worst GPU | rdp ms | worst frame | limit | audio ms | sound gaps |\n"
                                   "|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|--:|--:|\n",
                                   (int) sRun, (int) previousRun);
        }
    } else if (sMode == MODE_SWITCH) {
        static const char *const sAaNames[3] = { "off", "4x", "2x" };
        s32 aa = plat_antialias();
        length = plat_snprintf(out, size,
                               "\n### Switches (averages of the finished races)\n\n"
                               "Each switch is flipped by itself; saved is against the usual settings' races. "
                               "Render settings, the bench's own in every race (see apply_config): %d fps cap, "
                               "anti-aliasing %s, %s, 400 pixels, 3D off.\n\n"
                               "| track | switch | races | fps | 1%% low | frames 60/30/20/slower %% | 30 fps | 60 fps | GPU ms "
                               "| GPU saved | rdp ms | rdp saved | worst GPU | worst frame | limit |\n"
                               "|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|\n",
                               60 / plat_frame_retraces(), aa >= 0 && aa < 3 ? sAaNames[aa] : "?",
                               plat_old_3ds() ? "the N64's 4:3" : "Balanced widescreen");
    } else {
        length = plat_snprintf(out, size,
                               "\n### Render settings (averages of the finished races)\n\n"
                               "GPU saved is against the first setting, %s.\n\n"
                               "| track | settings | races | fps | 1%% low | frames 60/30/20/slower %% | 30 fps | 60 fps | GPU ms "
                               "| GPU saved | worst GPU | rdp ms | worst frame | limit | audio ms | sound gaps |\n"
                               "|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|--:|--:|\n",
                               sBenchConfigs[0].name);
    }
    if (!quick) {
        for (t = 0; t < numTracks; t++) {
            for (k = 0; k < config_count(); k++) {
                BenchGroup *g = &groups[t][k], *base = &groups[t][0];
                s32 limit = 0;
                if (g->races == 0) {
                    continue;
                }
                for (i = 1; i < BENCH_LIMITS; i++) {
                    if (g->limits[i] > g->limits[limit]) {
                        limit = i;
                    }
                }
                if (length >= size) {
                    return;
                }
                if (sMode == MODE_SWITCH) {
                    length += plat_snprintf(
                        out + length, size - length,
                        "| %s | %s | %d | %.1f | %s | %.0f%% | %.0f%% | %.1f | %s%.1f | %.1f | %s%.1f | %d | %d | %s |\n",
                        groups[t][0].track, config_name(k), (int) g->races, bench_avg(g->fps, g->races),
                        pacing_cells(g), bench_avg(g->pct30, g->races), bench_avg(g->pct60, g->races), bench_avg(g->gpu, g->races),
                        base->races ? "" : "?",
                        base->races ? bench_avg(base->gpu, base->races) - bench_avg(g->gpu, g->races) : 0.0,
                        bench_avg(g->rdp, g->races), base->races ? "" : "?",
                        base->races ? bench_avg(base->rdp, base->races) - bench_avg(g->rdp, g->races) : 0.0,
                        (int) g->worstGpu, (int) g->worstFrame, sLimitNames[limit]);
                    continue;
                }
                length += plat_snprintf(
                    out + length, size - length,
                    "| %s | %s | %d | %.1f | %s | %.0f%% | %.0f%% | %.1f | %s%.1f | %d | %.1f | %d | %s | %.2f | %.0f |\n",
                    groups[t][0].track, config_name(k), (int) g->races, bench_avg(g->fps, g->races),
                    pacing_cells(g), bench_avg(g->pct30, g->races), bench_avg(g->pct60, g->races), bench_avg(g->gpu, g->races),
                    base->races ? "" : "?",
                    base->races ? bench_avg(base->gpu, base->races) - bench_avg(g->gpu, g->races) : 0.0,
                    (int) g->worstGpu, bench_avg(g->rdp, g->races), (int) g->worstFrame, sLimitNames[limit],
                    bench_avg(g->audio, g->races), g->gaps);
            }
        }
        return;
    }
    for (t = 0; t < numTracks; t++) {
        for (k = 0; k < 2; k++) {
            BenchGroup *g = &groups[t][k];
            s32 limit = 0;
            if (g->races == 0) {
                continue;
            }
            for (i = 1; i < BENCH_LIMITS; i++) {
                if (g->limits[i] > g->limits[limit]) {
                    limit = i;
                }
            }
            if (length >= size) {
                return;
            }
            length += plat_snprintf(out + length, size - length,
                                    "| %s | %s | %d | %.1f | %s | %.0f%% | %.1f | %d | %.1f | %d | %s | %.2f | %.0f |\n",
                                    groups[t][0].track, g->build, (int) g->races,
                                    bench_avg(g->fps, g->races), pacing_cells(g), bench_avg(g->pct30, g->races),
                                    bench_avg(g->gpu, g->races), (int) g->worstGpu, bench_avg(g->rdp, g->races),
                                    (int) g->worstFrame, sLimitNames[limit], bench_avg(g->audio, g->races),
                                    g->gaps);
        }
    }
    for (t = 0; t < numTracks && length < size; t++) {
        BenchGroup *off = &groups[t][0], *on = &groups[t][1];
        if (off->races == 0 || on->races == 0) {
            continue;
        }
        length += plat_snprintf(out + length, size - length,
                                "%s%s: %+.1f fps, 30 fps on %+.0f%% of frames, GPU %+.1f ms, rdp %+.1f ms, worst "
                                "frame %+d ms, audio %+.2f ms",
                                t == 0 ? "\n" : "", groups[t][0].track,
                                bench_avg(on->fps, on->races) - bench_avg(off->fps, off->races),
                                bench_avg(on->pct30, on->races) - bench_avg(off->pct30, off->races),
                                bench_avg(on->gpu, on->races) - bench_avg(off->gpu, off->races),
                                bench_avg(on->rdp, on->races) - bench_avg(off->rdp, off->races),
                                (int) (on->worstFrame - off->worstFrame),
                                bench_avg(on->audio, on->races) - bench_avg(off->audio, off->races));
        if (length < size && off->pacedRaces > 0 && on->pacedRaces > 0) {
            length += plat_snprintf(out + length, size - length, ", 1%% low %+.1f fps, frames slower than 30 fps %+.0f%%",
                                    bench_avg(on->low1, on->pacedRaces) - bench_avg(off->low1, off->pacedRaces),
                                    bench_avg(on->paced[2] + on->paced[3], on->pacedRaces) -
                                        bench_avg(off->paced[2] + off->paced[3], off->pacedRaces));
        }
        if (length < size) {
            length += plat_snprintf(out + length, size - length, ".\n");
        }
    }
}

/* The boss fights' summary: one line per race of this run, from its results file, as Markdown. */
static void boss_summary(char *out, s32 size) {
    char path[256];
    char *data, *text, *line, *next;
    const char *fields[MAX_FIELDS];
    unsigned int dataSize;
    s32 length;

    make_path(path, sizeof(path), results_file());
    data = plat_load_file(path, &dataSize);
    if (data == NULL) {
        return;
    }
    text = plat_malloc(dataSize + 1);
    if (text == NULL) {
        plat_free(data);
        return;
    }
    bcopy(data, text, dataSize);
    text[dataSize] = '\0';
    plat_free(data);
    length = plat_snprintf(out, size,
                           "\n### Boss fights\n\n| boss | character | outcome | time s | place | fps | 30 fps | worst frame "
                           "| unsticks |\n|---|---|---|--:|--:|--:|--:|--:|--:|\n");
    for (line = text; line != NULL && *line && length < size; line = next) {
        next = strchr(line, '\n');
        if (next != NULL) {
            *next++ = '\0';
        }
        if (split_fields(line, fields) < COL_COUNT || port_atoi(fields[COL_RUN]) != sRun) {
            continue; // The header, or an earlier run.
        }
        length += plat_snprintf(out + length, size - length, "| %s | %s | %s | %s | %s | %s | %s%% | %s | %s |\n",
                                fields[COL_TRACK], fields[COL_CHARACTER], fields[COL_OUTCOME], fields[COL_RACE_S],
                                fields[COL_POSITION], fields[COL_FPS], fields[COL_PCT_30FPS],
                                fields[COL_WORST_FRAME], fields[COL_UNSTICKS]);
    }
    plat_free(text);
}

/* Sends the results so far as a GitHub issue (when bug reports are set up), and says so on the bottom screen. */
static void send_results(const char *why) {
    char title[128];
    static char summary[6144];
    s32 length;
    plat_snprintf(title, sizeof(title), "[3DS test run] %srun %d: %d of %d races, %d problems (%s)",
                  sMode == MODE_BENCH    ? "GPU benchmark "
                  : sMode == MODE_SWITCH ? "switch bench "
                  : sMode == MODE_QUICK  ? "quick test "
                  : sMode == MODE_BOSS  ? "boss fights "
                                        : "",
                  (int) sRun, (int) (sFinished + sProblems), (int) sNumRaces,
                  (int) sProblems, why);
    length = plat_snprintf(summary, sizeof(summary),
                           "%s %d %s after %d of %d races: %d finished, %d with problems.\n"
                           "Average frame rate of the races finished since the game started: %.1f fps (%d races).\n",
                           sMode == MODE_BENCH    ? "GPU benchmark run"
                           : sMode == MODE_SWITCH ? "Switch bench run"
                           : sMode == MODE_QUICK  ? "Quick test run"
                           : sMode == MODE_BOSS  ? "Boss fights run"
                                                 : "Run",
                           (int) sRun, why,
                           (int) (sFinished + sProblems), (int) sNumRaces, (int) sFinished, (int) sProblems,
                           sFpsCount ? sFpsSum / sFpsCount : 0.0, (int) sFpsCount);
    if (length > 0 && length < (s32) sizeof(summary)) {
        length += plat_snprintf(summary + length, sizeof(summary) - length,
                                "Graphics when sent: anti-aliasing %s, %d fps cap, render ahead %d "
                                "(each race's are in the results file's graphics column).\n",
                                aa_label(), 60 / plat_frame_retraces(),
                                plat_render_ahead());
    }
    if (sMode == MODE_BOSS && length > 0 && length < (s32) sizeof(summary)) {
        boss_summary(summary + length, sizeof(summary) - length);
    } else if (sMode != MODE_FULL && length > 0 && length < (s32) sizeof(summary)) {
        bench_summary(summary + length, sizeof(summary) - length);
    }
    plat_status("%s %s: %d races, %d problems", sModes[sMode].name, why, (int) (sFinished + sProblems), (int) sProblems);
    plat_send_test_results(title, summary);
}

/* Ends the test run: saves are written again and the profiler goes away (unless DKR_PROFILE or its button is on). */
static void turn_off(const char *why) {
    send_results(why);
    sEnabled = 0;
    sInitialised = FALSE;
    set_phase(PHASE_OFF);
    save_state(-1);
    if (is_bench()) {
        apply_config(-1); // Back to the player's settings.
    }
    sMode = MODE_FULL;
    prof_set_status(NULL);
}

/* Called in races: nonzero asks the game to quit to the title screen. */
s32 port_autorun_quit_race(void) {
    if (sQuitPending) {
        sQuitPending = FALSE;
        return TRUE;
    }
    if (sEnabled > 0 && sPhase == PHASE_FINISHED && sLingerRetraces >= FINISH_LINGER_SECONDS * 60) {
        set_phase(PHASE_LEAVING);
        sNext++;
        if (sNext >= sNumRaces) {
            if (plat_getenv("DKR_AUTORUN_LOOP") != NULL) {
                send_results("finished");
                sNext = 0;
                sRun++;
                sFinished = 0;
                sProblems = 0;
            } else {
                port_log("autorun: all %d races done, %d finished, %d problems", (int) sNumRaces, (int) sFinished,
                         (int) sProblems);
                turn_off("finished");
                return TRUE;
            }
        }
        save_state(-1);
        show_status();
        return TRUE;
    }
    return FALSE;
}

static void end_race(const char *outcome, s32 position) {
    prof_window_read(PROF_WINDOW_SESSION, &sRaceStats);
    sOutcome = outcome;
    record_result(outcome, position, sPhase == PHASE_RACING ? &sRaceStats : NULL);
    if (str_equal(outcome, "finished")) {
        if (sRaceStats.elapsedNs > 0) {
            sFpsSum += sRaceStats.gameFrames * 1e9 / sRaceStats.elapsedNs;
            sFpsCount++;
        }
    } else {
        char what[64];
        plat_snprintf(what, sizeof(what), "%s after %d s", outcome, (int) (sRaceRetraces / 60));
        record_problem(what);
    }
    set_phase(PHASE_FINISHED);
    // So that a crash or hang in the results screen isn't recorded as a second result for this race.
    save_state_recorded(sNext, TRUE);
    if (plat_getenv("DKR_AUTORUN_SNAPSHOT") != NULL) {
        gPlatSnapshotRequested = 1;
    }
}

static void handle_request(void) {
    s32 request = sRequest;
    sRequest = REQUEST_NONE;
    if ((request == REQUEST_START || request == REQUEST_BENCH || request == REQUEST_QUICK || request == REQUEST_BOSS ||
         request == REQUEST_SWITCH) &&
        !port_autorun_active()) {
        if (request == REQUEST_SWITCH && SWITCH_CONFIGS == 1) {
            plat_status("Switch bench: no switches to test right now");
            return;
        }
        sMode = request == REQUEST_SWITCH  ? MODE_SWITCH
                : request == REQUEST_BENCH ? MODE_BENCH
                : request == REQUEST_QUICK ? MODE_QUICK
                : request == REQUEST_BOSS  ? MODE_BOSS
                                           : MODE_FULL;
        sEnabled = 1;
        sInitialised = FALSE;
        if (!port_autorun_active()) {
            return; // Nothing to race.
        }
        prof_window_start(PROF_WINDOW_OVERLAY);
        plat_status("%s started: race %d of %d", sModes[sMode].name, (int) sNext + 1,
                    (int) sNumRaces);
        if (get_game_mode() == GAMEMODE_INGAME) {
            sQuitPending = TRUE; // Out of the game, to the title screen where the races start.
        }
    } else if (request == REQUEST_STOP && port_autorun_active()) {
        if (sPhase >= PHASE_LOADING && sPhase <= PHASE_FINISHED && get_game_mode() == GAMEMODE_INGAME) {
            sQuitPending = TRUE;
        }
        port_log("autorun: stopped from the bottom screen at race %d", (int) sNext + 1);
        turn_off("stopped");
    }
}

/*
 * The AI that drives the player's kart sometimes wedges it somewhere a CPU racer never goes (CPU racers use simpler
 * physics, and the AI's own reverse-out only works on the ground). After UNSTICK_SECONDS without a new checkpoint
 * the kart is put just before the next checkpoint, facing along the course, the way the game resets a racer to the
 * start line (func_800230D0). If it gets stuck again before that checkpoint, the checkpoint is counted as passed and
 * the kart is put before the one after it. Each time is noted in the results and in autorun_problems.txt, so the spot
 * is known.
 */
static void unstick(Object *obj, Object_Racer *racer, const char *why) {
    s32 checkpoint = racer->nextCheckpoint;
    CheckpointNode *node;
    Camera *camera;
    char what[128];
    s32 i, length;

    // Put there before and it didn't help (the AI drives back into the same wall): count that checkpoint as passed
    // and put the kart before the one after it instead.
    if (sUnsticks > 0 && sLastUnstick == racer->lap * 1000 + checkpoint && get_checkpoint_count() > 0) {
        // Passed, the way the game counts it (racer.c, after checkpoint_is_passed).
        checkpoint = (checkpoint + 1) % get_checkpoint_count();
        racer->nextCheckpoint = checkpoint;
        if (checkpoint == 0 && racer->courseCheckpoint > 0 && racer->lap < 120) {
            racer->lap++;
        }
        racer->courseCheckpoint++;
    }
    sLastUnstick = racer->lap * 1000 + racer->nextCheckpoint;
    node = find_next_checkpoint_node(checkpoint, racer->isOnAlternateRoute);
    if (node == NULL) {
        return;
    }
    plat_snprintf(what, sizeof(what), "note: put at checkpoint %d of lap %d %s, from %.0f,%.0f,%.0f", (int) checkpoint,
                  (int) racer->lap + 1, why, obj->trans.x_position, obj->trans.y_position, obj->trans.z_position);
    record_problem(what);
    length = strlen(sUnstuckAt);
    plat_snprintf(sUnstuckAt + length, sizeof(sUnstuckAt) - length, "%s%d:%d", length ? " " : "", (int) racer->lap + 1,
                  (int) checkpoint);
    sUnsticks++;
    obj->trans.x_position = node->x - node->rotationXFrac * 20.0f;
    obj->trans.y_position = node->y;
    obj->trans.z_position = node->z - node->rotationZFrac * 20.0f;
    obj->segmentID = get_level_segment_index_from_position(obj->trans.x_position, obj->trans.y_position,
                                                           obj->trans.z_position);
    racer->prev_x_position = obj->trans.x_position;
    racer->prev_y_position = obj->trans.y_position;
    racer->prev_z_position = obj->trans.z_position;
    racer->steerVisualRotation = arctan2_f(node->rotationXFrac, node->rotationZFrac);
    obj->trans.rotation.y_rotation = racer->steerVisualRotation;
    obj->trans.rotation.x_rotation = 0;
    obj->trans.rotation.z_rotation = 0;
    for (i = 0; i < 4; i++) {
        racer->unkD8[i * 3 + 0] = obj->trans.x_position;
        racer->unkD8[i * 3 + 1] = obj->trans.y_position;
        racer->unkD8[i * 3 + 2] = obj->trans.z_position;
    }
    obj->interactObj->x_position = obj->trans.x_position;
    obj->interactObj->y_position = obj->trans.y_position;
    obj->interactObj->z_position = obj->trans.z_position;
    racer->velocity = 0.0f;
    racer->lateral_velocity = 0.0f;
    racer->y_rotation_vel = 0;
    racer->x_rotation_vel = 0;
    racer->z_rotation_vel = 0;
    obj->x_velocity = 0.0f;
    obj->y_velocity = 0.0f;
    obj->z_velocity = 0.0f;
    racer->vehicleID = racer->vehicleIDPrev;
    set_active_camera(racer->playerIndex);
    camera = cam_get_active_camera_no_cutscenes();
    camera->trans.x_position = obj->trans.x_position;
    camera->trans.y_position = obj->trans.y_position;
    camera->trans.z_position = obj->trans.z_position;
}

/*
 * Tricky's race sends the player back to the hub once they fall 400 units below the highest point they reached
 * (vehicle_tricky.c). The AI driving the kart, made for Tricky's own path, sometimes takes it off the side near the
 * summit, so a kart falling well below its best height is put back on the course before the game gives up on it.
 */
extern f32 gTrickyRacerPeakHeight;
static s32 falling_off_tricky(Object *obj) {
    s32 i, count;
    Object **racers;
    if (obj->y_velocity >= 0.0f || obj->trans.y_position + 250.0f >= gTrickyRacerPeakHeight) {
        return FALSE;
    }
    racers = get_racer_objects(&count);
    for (i = 0; i < count; i++) {
        if (racers[i]->racer->vehicleID == VEHICLE_TRICKY) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Called once per game frame, with the number of retraces it took. */
void port_autorun_frame(s32 updateRate) {
    Object *obj;
    Object_Racer *racer;
    s32 progress;
    static s32 sStatusCounter;

    if (sRequest != REQUEST_NONE) {
        handle_request();
    }
    obj = (get_game_mode() == GAMEMODE_INGAME) ? get_racer_object_by_port(PLAYER_ONE) : NULL;
    racer = (obj != NULL) ? obj->racer : NULL;
    {
        // What the player is doing, for bug reports (formatted only when one is made).
        PlatGameNote note = { 0 };
        if (racer != NULL) {
            note.racing = 1;
            note.lap = racer->lap + 1;
            note.checkpoint = racer->nextCheckpoint;
            note.bananas = racer->bananas;
            note.boost = racer->boostTimer;
            note.itemType = racer->balloon_type;
            note.itemCount = racer->balloon_quantity;
            note.shield = racer->shieldTimer;
            note.magnet = racer->magnetTimer;
            note.hit = racer->attackType;
            note.paused = is_game_paused() != 0;
        }
        plat_set_game_note(&note);
    }
    if (!port_autorun_active()) {
        return;
    }
    switch (sPhase) {
        case PHASE_LOADING:
            if (racer != NULL) {
                sLoadNs = plat_time_ns() - sPhaseStartNs;
                set_phase(PHASE_COUNTDOWN);
            } else if (plat_time_ns() - sPhaseStartNs > 120000000000ULL) {
                end_race("load_timeout", 0);
            }
            break;
        case PHASE_COUNTDOWN:
            if (racer != NULL && get_race_countdown() == 0) {
                prof_window_start(PROF_WINDOW_SESSION);
                sAudioGapsStart = gPlatCounters[PLAT_COUNTER_AUDIO_UNDERRUN];
                sAudioDropsStart = gPlatCounters[PLAT_COUNTER_AUDIO_DROPPED];
                set_phase(PHASE_RACING);
            } else if (plat_time_ns() - sPhaseStartNs > 120000000000ULL) {
                // The game left the level, or never started the race (a boss's speech is far shorter than this).
                end_race(racer == NULL ? "left_before_start" : "countdown_timeout", 0);
            }
            break;
        case PHASE_RACING:
            if (racer == NULL) {
                end_race("left_race", 0);
                break;
            }
            sRaceRetraces += updateRate;
            if (sDriving && level_id() != sRaceLevel) {
                // The game took the player out of the race without finishing it (falling off a boss's course sends
                // them to the hub). Boss races start in another level, for the boss's speech, so the race's level is
                // the one the kart got going in.
                char what[96];
                plat_snprintf(what, sizeof(what), "note: the game left the race for level %d, last at %.0f,%.0f,%.0f",
                              (int) level_id(), sLastPosition[0], sLastPosition[1], sLastPosition[2]);
                record_problem(what);
                end_race("left_level", racer->racePosition);
                break;
            }
            sLastPosition[0] = obj->trans.x_position;
            sLastPosition[1] = obj->trans.y_position;
            sLastPosition[2] = obj->trans.z_position;
            progress = racer->courseCheckpoint;
            {
                // DKR_LOG_RACER: logs the kart's checkpoint, speed and place at each checkpoint and every half second,
                // to compare two runs (30 and 60 fps physics, say).
                static s32 sLogRacer = -1, sLastCp = -999, sLastHalf = -1;
                if (sLogRacer < 0) {
                    sLogRacer = plat_getenv("DKR_LOG_RACER") != NULL;
                }
                if (sLogRacer && (progress != sLastCp || sRaceRetraces / 30 != sLastHalf)) {
                    port_log("racer t=%.3f cp=%d v=%.3f lv=%.3f yv=%.3f pos=%.1f,%.1f,%.1f boost=%d bananas=%d",
                             sRaceRetraces / 60.0,
                             (int) progress, racer->velocity, racer->lateral_velocity, obj->y_velocity,
                             obj->trans.x_position, obj->trans.y_position, obj->trans.z_position,
                             (int) racer->boostTimer, (int) racer->bananas);
                    sLastCp = progress;
                    sLastHalf = sRaceRetraces / 30;
                }
            }
            if (!sDriving && (racer->velocity > 1.0f || racer->velocity < -1.0f)) {
                sDriving = TRUE;
                sRaceLevel = level_id();
            }
            if (!sDriving) {
                // Not going yet: no time without progress.
            } else if (progress > sBestProgress) {
                sBestProgress = progress;
                sStuckRetraces = 0;
            } else {
                sStuckRetraces += updateRate;
            }
            if (racer->raceFinished) {
                end_race("finished", racer->finishPosition);
            } else if (sStuckRetraces >= STUCK_SECONDS * 60) {
                end_race("stuck", racer->racePosition);
            } else if (sStuckRetraces >= UNSTICK_SECONDS * 60 &&
                       (sStuckRetraces - updateRate) / (UNSTICK_SECONDS * 60) < sStuckRetraces / (UNSTICK_SECONDS * 60)) {
                char why[48];
                plat_snprintf(why, sizeof(why), "after %d s without progress", UNSTICK_SECONDS);
                unstick(obj, racer, why);
            } else if (falling_off_tricky(obj)) {
                unstick(obj, racer, "as it fell off Tricky's mountain");
            } else if (sRaceRetraces >= sTimeoutSeconds * 60) {
                end_race("timeout", racer->racePosition);
            }
            break;
        case PHASE_FINISHED:
            sLingerRetraces += updateRate;
            if (get_game_mode() != GAMEMODE_INGAME) {
                // Not in a race (a load that never started): straight on to the next one.
                sLingerRetraces = FINISH_LINGER_SECONDS * 60;
                port_autorun_quit_race();
                if (sPhase == PHASE_LEAVING) {
                    set_phase(PHASE_TITLE);
                }
            }
            break;
    }
    if (sStatusCounter++ % 30 == 0) {
        show_status();
    }
}

/* The level and the active camera, for the host renderer's clipping sweep (DKR_CLIP_SWEEP, host/gfx_soft.c). */
void port_sweep_camera(int *level, float pos[3], int *segment) {
    Camera *camera = cam_get_active_camera();
    *level = level_id();
    pos[0] = camera->trans.x_position;
    pos[1] = camera->trans.y_position;
    pos[2] = camera->trans.z_position;
    *segment = camera->cameraSegmentID;
}

/* Whether the vertices are the level model's (track and scenery) rather than an object's, for the clipping sweep. */
s32 port_sweep_level_vertex(const void *vertices) {
    extern LevelModel *gCurrentLevelModel;
    s32 i;
    if (gCurrentLevelModel == NULL) {
        return FALSE;
    }
    for (i = 0; i < gCurrentLevelModel->numberOfSegments; i++) {
        LevelModelSegment *segment = &gCurrentLevelModel->segments[i];
        if ((const Vertex *) vertices >= segment->vertices &&
            (const Vertex *) vertices < segment->vertices + segment->numberOfVertices) {
            return TRUE;
        }
    }
    return FALSE;
}
