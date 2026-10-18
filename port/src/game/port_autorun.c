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
#include "port.h"
#include "plat/plat.h"
#include "plat/profile.h"
#include <string.h>

#define MAX_RACES 1024
#define MAX_TRACKS 64
#define FINISH_LINGER_SECONDS 8 // After the finish line: the post-race camera and results, then on to the next.
#define STUCK_SECONDS 60        // No checkpoint passed for this long counts as stuck.
#define STATE_FILE "autorun_state.txt"
#define RESULTS_FILE "autorun_results.csv"
#define PROBLEMS_FILE "autorun_problems.txt"
#define HANG_FILE "autorun_hang.txt"

typedef struct Race {
    s8 track;
    s8 character;
    s8 vehicle;
} Race;

static void set_phase(s32 phase);

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

enum Request { REQUEST_NONE, REQUEST_START, REQUEST_STOP };

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
static s32 sStuckRetraces; // Game time since the kart last reached a new checkpoint.
static s32 sLastProgress;
static s32 sLingerRetraces;
static const char *sOutcome;
static s32 sFinished, sProblems;
static ProfStats sRaceStats;

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

static void build_race_list(void) {
    s8 tracks[MAX_TRACKS], characters[NUMBER_OF_CHARACTERS];
    s32 numTracks, numCharacters, c, t, v, usable, i;
    s32 numLevels, numWorlds;
    s32 allVehicles = plat_getenv("DKR_AUTORUN_VEHICLES") != NULL && plat_getenv("DKR_AUTORUN_VEHICLES")[0] == 'a';

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
                    sNumRaces++;
                }
            }
        }
    }
}

static void save_state(s32 running) {
    char text[160];
    char path[256];
    plat_snprintf(text, sizeof(text),
                  "active=%d\nconfig=%08x\nrun=%d\nnext=%d\nrunning=%d\nfinished=%d\nproblems=%d\n",
                  (int) (sEnabled > 0), (unsigned int) sConfigHash, (int) sRun, (int) sNext, (int) running,
                  (int) sFinished, (int) sProblems);
    make_path(path, sizeof(path), STATE_FILE);
    plat_save_file(path, text, strlen(text));
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

/* Writes the results line of the race that just ended. stats may be NULL (the race never got going). */
static void record_result(const char *outcome, s32 position, const ProfStats *stats) {
    char path[256];
    char line[640];
    unsigned int size;
    void *existing;
    Race *race = &sRaces[sNext];
    ProfStats empty;
    u32 slow, i;

    make_path(path, sizeof(path), RESULTS_FILE);
    existing = plat_load_file(path, &size);
    if (existing == NULL) {
        append_file(RESULTS_FILE,
                    "run,race,track_id,track,character,vehicle,outcome,race_s,position,load_s,frames,fps,pct_30fps,"
                    "pct_20fps,pct_slower,worst_frame_ms,hitches,logic_ms,scene_ms,frame_wait_ms,rdp_ms,texture_ms,"
                    "gpu_wait_ms,gpu_draw_ms,audio_ms,worst_build_ms,worst_render_ms,worst_gpu_ms,triangles,draws,"
                    "texture_uploads,idle_pct,limit\n");
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
        "%u,%u,%d,%s\n",
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
        stats->gameFrames ? prof_limit(stats) : "-");
    append_file(RESULTS_FILE, line);
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

        sEnabled = value != NULL && value[0] != '0';
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
    s32 i;

    sInitialised = TRUE;
    sConfigHash = 0;
    for (i = 0; i < (s32) ARRAY_COUNT(names); i++) {
        hash_text(plat_getenv(names[i]));
    }
    build_race_list();
    sTimeoutSeconds = plat_getenv("DKR_AUTORUN_TIMEOUT") ? port_atoi(plat_getenv("DKR_AUTORUN_TIMEOUT")) : 300;
    sRun = 1;
    sNext = 0;
    sFinished = 0;
    sProblems = 0;
    sFpsSum = 0.0;
    sFpsCount = 0;

    make_path(path, sizeof(path), STATE_FILE);
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
            if (running >= 0 && running < sNumRaces) {
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

/* From any thread (the bottom screen's button): start (1) or stop (0) the test run, at the next game frame. */
void port_autorun_request(s32 start) {
    sRequest = start ? REQUEST_START : REQUEST_STOP;
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
    plat_snprintf(line[0], sizeof(line[0]), "TEST RUN %d  race %d/%d  ok %d bad %d", (int) sRun, (int) sNext + 1,
                  (int) sNumRaces, (int) sFinished, (int) sProblems);
    plat_snprintf(line[1], sizeof(line[1]), "%s %s %s %s", sCharacterNames[(s32) race->character],
                  track_name(race->track), sVehicleNames[(s32) race->vehicle], what);
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
    set_phase(PHASE_LOADING);
    sRaceRetraces = 0;
    sStuckRetraces = 0;
    sLastProgress = -1;
    sLingerRetraces = 0;
    show_status();
    return port_autorun_setup_race(race->track, race->character, race->vehicle);
}

/* Nonzero while the player's kart is driven by the AI. */
s32 port_autorun_drives(void) {
    return sEnabled > 0 && sPhase >= PHASE_LOADING && sPhase <= PHASE_FINISHED;
}

/* Sends the results so far as a GitHub issue (when bug reports are set up), and says so on the bottom screen. */
static void send_results(const char *why) {
    char title[128];
    char summary[512];
    plat_snprintf(title, sizeof(title), "[3DS test run] run %d: %d of %d races, %d problems (%s)", (int) sRun,
                  (int) (sFinished + sProblems), (int) sNumRaces, (int) sProblems, why);
    plat_snprintf(summary, sizeof(summary),
                  "Run %d %s after %d of %d races: %d finished, %d with problems.\n"
                  "Average frame rate of the races finished since the game started: %.1f fps (%d races).\n",
                  (int) sRun, why, (int) (sFinished + sProblems), (int) sNumRaces, (int) sFinished, (int) sProblems,
                  sFpsCount ? sFpsSum / sFpsCount : 0.0, (int) sFpsCount);
    plat_status("Test run %s: %d races, %d problems", why, (int) (sFinished + sProblems), (int) sProblems);
    plat_send_test_results(title, summary);
}

/* Ends the test run: saves are written again and the profiler goes away (unless DKR_PROFILE is set). */
static void turn_off(const char *why) {
    send_results(why);
    sEnabled = 0;
    sInitialised = FALSE;
    set_phase(PHASE_OFF);
    save_state(-1);
    prof_set_status(NULL);
    if (!prof_enabled()) {
        plat_overlay_show("");
    }
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
    if (plat_getenv("DKR_AUTORUN_SNAPSHOT") != NULL) {
        gPlatSnapshotRequested = 1;
    }
}

static void handle_request(void) {
    s32 request = sRequest;
    sRequest = REQUEST_NONE;
    if (request == REQUEST_START && !port_autorun_active()) {
        sEnabled = 1;
        sInitialised = FALSE;
        if (!port_autorun_active()) {
            return; // Nothing to race.
        }
        prof_window_start(PROF_WINDOW_OVERLAY);
        plat_status("Test run started: race %d of %d", (int) sNext + 1, (int) sNumRaces);
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

/* Called once per game frame, with the number of retraces it took. */
void port_autorun_frame(s32 updateRate) {
    Object *obj;
    Object_Racer *racer;
    s32 progress;
    static s32 sStatusCounter;

    if (sRequest != REQUEST_NONE) {
        handle_request();
    }
    if (!port_autorun_active()) {
        return;
    }
    obj = (get_game_mode() == GAMEMODE_INGAME) ? get_racer_object_by_port(PLAYER_ONE) : NULL;
    racer = (obj != NULL) ? obj->racer : NULL;
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
                set_phase(PHASE_RACING);
            }
            break;
        case PHASE_RACING:
            if (racer == NULL) {
                end_race("left_race", 0);
                break;
            }
            sRaceRetraces += updateRate;
            progress = racer->lap * 256 + racer->nextCheckpoint;
            if (progress != sLastProgress) {
                sLastProgress = progress;
                sStuckRetraces = 0;
            } else {
                sStuckRetraces += updateRate;
            }
            if (racer->raceFinished) {
                end_race("finished", racer->finishPosition);
            } else if (sStuckRetraces >= STUCK_SECONDS * 60) {
                end_race("stuck", racer->racePosition);
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
