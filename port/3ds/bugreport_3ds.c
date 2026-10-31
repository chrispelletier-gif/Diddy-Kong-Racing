/*
 * Bug reports straight from the console, as GitHub issues (src/plat/bugreport.c does the sending).
 *
 * - Holding SELECT and touching the bottom screen, or "Send a bug report" in its Settings menu, takes a snapshot and sends it
 *   with the log.
 * - The unattended test run sends its results when it ends, and its crashes without asking at the next start.
 * - A crash writes sdmc:/3ds/dkr/crash.txt (registers, stack, recent log) and ends the program.
 * - If the last run did not end normally (a crash, a freeze that needed the power button), the next start offers to
 *   send its log and crash.txt.
 *
 * Sending needs sdmc:/3ds/dkr/bugreport.txt with a line "token=<GitHub token>" (see the README).
 *
 * A player's build (plat.h PLAT_DEBUG_TOOLS) has only the crash handler and keeps the last run's files: no reports.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "plat/plat.h"
#include "plat/recorder.h"
#include "plat/settings.h"
#include "audio/voices.h"
#if PLAT_DEBUG_TOOLS
#include "plat/bugreport.h"
#include "net_3ds.h"
#endif

#ifndef PORT_BUILD_VERSION
#define PORT_BUILD_VERSION "unknown"
#endif

#define DATA_DIR "sdmc:/3ds/dkr"
#define CONFIG_PATH DATA_DIR "/bugreport.txt"
#define RUNNING_PATH DATA_DIR "/running"
#define DEFAULT_REPO "chrispelletier-gif/Diddy-Kong-Racing-3DS"
#define REPORT_LABEL "3ds-bug-report"
/* Compressed size of all the files of one report; each 45 KB is one comment on the issue. */
#define MAX_ATTACHMENT_BYTES (900 * 1024)
#define MAX_FILES 18

extern u32 __ctru_heap_size;
int port_gfx_frame_count(void);

#if PLAT_DEBUG_TOOLS
enum ReportKind {
    REPORT_BUTTON,       /* SELECT + touch during the game. */
    REPORT_LAST_CRASHED, /* The last run crashed (crash.txt). */
    REPORT_LAST_UNENDED, /* The last run never reached its normal exit. */
    REPORT_TEST_RUN,     /* The results of the unattended test run (port_autorun.c). */
};

typedef struct ReportConfig {
    char token[256];
    char repo[128];
    char apiUrl[128];
} ReportConfig;

/* "name=value" lines; '#' starts a comment. Returns nonzero if there is a token. */
static int load_config(ReportConfig *config) {
    unsigned int size;
    char *text = plat_load_file(CONFIG_PATH, &size), *line, *next;

    memset(config, 0, sizeof(*config));
    snprintf(config->repo, sizeof(config->repo), "%s", DEFAULT_REPO);
    snprintf(config->apiUrl, sizeof(config->apiUrl), "%s", "https://api.github.com");
    if (text == NULL) {
        return 0;
    }
    text = realloc(text, size + 1);
    text[size] = '\0';
    for (line = text; line != NULL && *line; line = next) {
        char *value, *end;
        next = strpbrk(line, "\r\n");
        if (next != NULL) {
            *next++ = '\0';
        }
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        value = strchr(line, '=');
        if (*line == '#' || value == NULL) {
            continue;
        }
        *value++ = '\0';
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        end = value + strlen(value);
        while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        for (end = line + strlen(line); end > line && (end[-1] == ' ' || end[-1] == '\t');) {
            *--end = '\0';
        }
        if (strcmp(line, "token") == 0) {
            snprintf(config->token, sizeof(config->token), "%s", value);
        } else if (strcmp(line, "repo") == 0) {
            snprintf(config->repo, sizeof(config->repo), "%s", value);
        } else if (strcmp(line, "api") == 0) {
            snprintf(config->apiUrl, sizeof(config->apiUrl), "%s", value);
        }
    }
    free(text);
    return config->token[0] != '\0';
}

#endif

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void move_file(const char *from, const char *to) {
    remove(to);
    rename(from, to);
}

/*
 * Crashes. The handler runs on the crashed thread with a stack of its own. It cannot trust the C library's locks or
 * the heap (the crash may have been inside malloc), so it formats into a static buffer and writes with the FS
 * service directly.
 */
static __thread const char *sThreadName;
/*
 * Each watched thread has its own handler stack: the kernel puts the exception's details on it, so two threads that
 * crash at once (the game's and the renderer on the other core) must not share one. A thread past the last one shares
 * the last. Only one handler formats and writes at a time (sCrashing); another waits for it, then exits with it.
 */
#define CRASH_STACKS 16
#define CRASH_STACK_SIZE 0x2000
static u8 sCrashStacks[CRASH_STACKS][CRASH_STACK_SIZE] __attribute__((aligned(8)));
static int sCrashStackCount;
static volatile int sCrashing;
static char sCrashText[24 * 1024];

static void crash_write_file(const char *path, const char *text, u32 length) {
    Handle file;
    u32 written;
    if (R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, path),
                                         FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        return;
    }
    FSFILE_SetSize(file, 0);
    FSFILE_Write(file, &written, 0, text, length, FS_WRITE_FLUSH);
    FSFILE_Close(file);
}

void plat_audio_crash_silence(void);

static void crash_handler(ERRF_ExceptionInfo *info, CpuRegisters *regs) {
    static const char *const sTypes[] = { "prefetch abort", "data abort", "undefined instruction", "VFP exception" };
    int length = 0, i;
    const u32 *sp = (const u32 *) regs->sp;

    if (__atomic_exchange_n(&sCrashing, 1, __ATOMIC_ACQUIRE)) {
        // Another thread is writing its crash: that one is reported, and it ends the process.
        for (;;) {
            svcSleepThread(1000000000LL);
        }
    }
#define ADD(...) length += snprintf(sCrashText + length, sizeof(sCrashText) - length, __VA_ARGS__)
    ADD("%s in thread %s\n", (u32) info->type < 4 ? sTypes[info->type] : "exception",
        sThreadName != NULL ? sThreadName : "main");
    ADD("pc %08lx lr %08lx far %08lx fsr %08lx\n", regs->pc, regs->lr, info->far, info->fsr);
    ADD("build %s\n", PORT_BUILD_VERSION);
    ADD("renderer: %s, frame %d\n", gPlatGfxStage != NULL ? gPlatGfxStage : "-", port_gfx_frame_count());
    for (i = 0; i < 13; i++) {
        ADD("r%-2d %08lx%s", i, regs->r[i], (i % 4 == 3) ? "\n" : "  ");
    }
    ADD("sp  %08lx  cpsr %08lx\n\nstack:\n", regs->sp, regs->cpsr);
    // The stack is only read if the stack pointer looks like it points at memory (heap or a thread stack).
    if ((regs->sp & 3) == 0 && regs->sp >= 0x08000000 && regs->sp < 0x10000000) {
        for (i = 0; i < 128; i++) {
            ADD("%08lx%s", sp[i], (i % 8 == 7) ? "\n" : " ");
        }
    }
    ADD("\nrecent log:\n");
    if (length < (int) sizeof(sCrashText) - 1) {
        length += plat_log_recent(sCrashText + length, sizeof(sCrashText) - length);
    }
#undef ADD
    if (length > (int) sizeof(sCrashText) - 1) {
        length = sizeof(sCrashText) - 1;
    }
    crash_write_file("/3ds/dkr/crash.txt", sCrashText, length);
#if PLAT_DEBUG_TOOLS
    // The flight recorder as it is, for the next start's report to turn into a timeline.
    crash_write_file("/3ds/dkr/crash_rec.bin", (const char *) rec_state(), sizeof(RecState));
#endif
    // Silence the DSP's voices (audio_ndsp.c), then back to the Homebrew Launcher; the next start offers to send
    // the report.
    plat_audio_crash_silence();
    svcExitProcess();
}

void bugreport_watch_thread(const char *name) {
    int slot = __atomic_fetch_add(&sCrashStackCount, 1, __ATOMIC_RELAXED);
    if (slot >= CRASH_STACKS) {
        slot = CRASH_STACKS - 1;
    }
    sThreadName = name;
    threadOnException(crash_handler, sCrashStacks[slot] + CRASH_STACK_SIZE, WRITE_DATA_TO_HANDLER_STACK);
}

#if PLAT_DEBUG_TOOLS
/* Building the report. */

typedef struct Report {
    int kind;
    char title[160];
    char *body;
    const char *extra; /* REPORT_TEST_RUN: the run's summary. */
    char *recSummary;  /* The flight recorder's summary (rec_format_summary), or NULL. */
    char game[512];    /* What the game was doing, in words (REPORT_BUTTON). */
    BugReportFile files[MAX_FILES];
    int fileCount;
} Report;

/* Whether a saved recent frame (width, height, then BGR8) is more than one 400-row picture. */
static int recent_frame_is_large(const char *name) {
    char path[160];
    u32 header[2] = { 0, 0 };
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", DATA_DIR, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    if (fread(header, sizeof(header), 1, f) != 1) {
        header[1] = 0;
    }
    fclose(f);
    return header[1] > 400;
}

static void add_file(Report *report, const char *name) {
    char path[256];
    unsigned int size = 0;
    unsigned char *data;
    BugReportFile *file;

    if (report->fileCount >= MAX_FILES) {
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", DATA_DIR, name);
    data = plat_load_file(path, &size);
    if (data == NULL) {
        return;
    }
    // GPU captures of an anti-aliased frame are twice the size of the screen in each direction. They are shrunk the
    // way the display does it, so the report stays small; diff_frames.py takes either size.
    if (strstr(name, "_gpu.bin") != NULL && size >= 8) {
        u32 w, h, x, y;
        memcpy(&w, data, 4);
        memcpy(&h, data + 4, 4);
        if (w > 240 && w % 2 == 0 && h % 2 == 0 && size >= 8 + w * h * 4) {
            const u8 *in = data + 8;
            u8 *out = data + 8;
            for (y = 0; y < h / 2; y++) {
                for (x = 0; x < w / 2; x++) {
                    int c;
                    for (c = 0; c < 4; c++) {
                        const u8 *p = in + ((y * 2) * w + x * 2) * 4 + c;
                        out[(y * (w / 2) + x) * 4 + c] = (p[0] + p[4] + p[w * 4] + p[w * 4 + 4] + 2) / 4;
                    }
                }
            }
            w /= 2;
            h /= 2;
            memcpy(data, &w, 4);
            memcpy(data + 4, &h, 4);
            size = 8 + w * h * 4;
        }
    }
    file = &report->files[report->fileCount++];
    file->name = strdup(name);
    file->data = data;
    file->size = size;
}

static void free_report(Report *report) {
    int i;
    for (i = 0; i < report->fileCount; i++) {
        free((void *) report->files[i].name);
        free((void *) report->files[i].data);
    }
    free(report->body);
    free(report->recSummary);
    report->body = NULL;
    report->recSummary = NULL;
    report->fileCount = 0;
}

/*
 * A flight recorder state (plat/recorder.h) as timeline.csv among the files, and its summary in the body. A state
 * saved by a crash or a hang is checked first: one from another build's layout is skipped.
 */
static void attach_recording(Report *report, const RecState *state, const char *fileName) {
    char path[160];
    char *csv;
    if (state == NULL || state->magic != REC_MAGIC || state->version != REC_VERSION || state->size != sizeof(RecState)) {
        return;
    }
    csv = rec_format_csv(state);
    if (csv != NULL) {
        snprintf(path, sizeof(path), "%s/%s", DATA_DIR, fileName);
        if (plat_save_file(path, csv, strlen(csv))) {
            add_file(report, fileName);
        }
        free(csv);
    }
    free(report->recSummary);
    report->recSummary = rec_format_summary(state);
}

/* A recorder state that a crash or a hang wrote to the SD card, as attach_recording takes it. */
static void attach_saved_recording(Report *report, const char *savedName, const char *fileName) {
    char path[160];
    unsigned int size = 0;
    RecState *state;
    snprintf(path, sizeof(path), "%s/%s", DATA_DIR, savedName);
    state = plat_load_file(path, &size);
    if (state != NULL && size == sizeof(RecState)) {
        attach_recording(report, state, fileName);
    }
    free(state);
}

/* The last lines of text, at most maxLength bytes, starting at a whole line. */
static const char *tail(const char *text, size_t maxLength) {
    size_t length = strlen(text);
    const char *start, *newline;
    if (length <= maxLength) {
        return text;
    }
    start = text + length - maxLength;
    newline = strchr(start, '\n');
    return newline != NULL ? newline + 1 : start;
}

static void build_body(Report *report, const char *crashText, const char *recentLog) {
    static const char *const sKinds[] = {
        "Sent from the game (SELECT + touch)",
        "The last run crashed; sent at the next start",
        "The last run did not end normally (a freeze, or the power button); sent at the next start",
        "Results of the unattended test run",
    };
    static const char *const sKindNames[] = { "button", "crash", "unended", "testrun" };
    size_t capacity = 60000, length = 0;
    char *body = malloc(capacity);
    bool isNew3DS = false;
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    int i;

    if (body == NULL) {
        return;
    }
    APT_CheckNew3DS(&isNew3DS);
#define ADD(...)                                                                                                       \
    do {                                                                                                               \
        if (length < capacity) {                                                                                       \
            length += snprintf(body + length, capacity - length, __VA_ARGS__);                                         \
        }                                                                                                              \
    } while (0)
    ADD("<!-- dkr-bug-report v1 kind=%s -->\n", sKindNames[report->kind]);
    ADD("**What:** %s\n", sKinds[report->kind]);
    ADD("**Build:** `%s`\n", PORT_BUILD_VERSION);
    ADD("**Console:** %s\n", isNew3DS ? "New 3DS" : "Old 3DS");
    if (tm != NULL) {
        ADD("**Console clock:** %04d-%02d-%02d %02d:%02d\n", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min);
    }
    if (report->kind == REPORT_BUTTON) {
        struct mallinfo info = mallinfo();
        ADD("**Frame:** %d, renderer %s\n", port_gfx_frame_count(), gPlatGfxStage != NULL ? gPlatGfxStage : "-");
        ADD("**Counters:** vi %u, gfx %u/%u, audio %u, sys %u, sound gaps %u, drops %u, texture failures %u\n",
            gPlatCounters[PLAT_COUNTER_VI], gPlatCounters[PLAT_COUNTER_GFX_QUEUED],
            gPlatCounters[PLAT_COUNTER_GFX_DONE], gPlatCounters[PLAT_COUNTER_AUDIO], gPlatCounters[PLAT_COUNTER_MAIN],
            gPlatCounters[PLAT_COUNTER_AUDIO_UNDERRUN], gPlatCounters[PLAT_COUNTER_AUDIO_DROPPED],
            gPlatCounters[PLAT_COUNTER_TEXTURE_FAILED]);
        ADD("**Free memory:** gpu %lu KB, heap %lu KB\n", (unsigned long) (linearSpaceFree() / 1024),
            (unsigned long) ((info.fordblks + (__ctru_heap_size - info.arena)) / 1024));
        if (pv_active()) {
            ADD("**Voices:** %s\n", pv_status());
        }
        if (report->game[0] != '\0') {
            ADD("**Game:** %s\n", report->game);
        }
    }
    {
        static char settings[2048];
        settings_describe(settings, sizeof(settings));
        ADD("**Settings now:** %s\n", settings[0] != '\0' ? settings : "(none saved: all defaults)");
        ADD("**Up:** %llu s; debug build\n", plat_time_ns() / 1000000000ULL);
    }
    if (report->extra != NULL) {
        ADD("\n### Test run\n\n%s\n", report->extra);
    }
    if (report->recSummary != NULL) {
        ADD("\n### Before the report (flight recorder; every frame is in `timeline.csv`)\n\n%s",
            tail(report->recSummary, 6000));
    }
    if (crashText != NULL) {
        ADD("\n### Crash\n\n```\n%s\n```\n", tail(crashText, 12000));
    }
    if (recentLog != NULL && recentLog[0] != '\0') {
        size_t room = (length + 4000 < capacity) ? capacity - length - 4000 : 0;
        ADD("\n### Log (last lines)\n\n```\n%s\n```\n", tail(recentLog, room > 30000 ? 30000 : room));
    }
    if (report->fileCount > 0) {
        ADD("\n### Files\n\nAttached below as comments; `port/tools/bugreport_fetch.py <issue>` saves them.\n\n");
        for (i = 0; i < report->fileCount; i++) {
            ADD("- `%s` (%u bytes)\n", report->files[i].name, report->files[i].size);
        }
    }
#undef ADD
    if (length >= capacity) {
        body[capacity - 1] = '\0';
    }
    report->body = body;
}

/* Sending. */

static void (*sShow)(const char *text);

static void show_progress(const char *text) {
    char line[160];
    snprintf(line, sizeof(line), port_text("Bug report: %s"), port_text(text));
    sShow(line);
}

/* Sets up the network, sends, and takes it down again. Returns 0 on success; result has the issue or the error. */
static int send_report(const ReportConfig *config, Report *report, char *result, size_t resultSize) {
    BugReportConfig send;
    int status;

    if (net_3ds_begin(result, resultSize) != 0) {
        return -1;
    }
    memset(&send, 0, sizeof(send));
    send.token = config->token;
    send.repo = config->repo;
    send.apiUrl = config->apiUrl;
    send.label = REPORT_LABEL;
    send.caPem = net_3ds_ca_pem();
    send.caFile = net_3ds_ca_file();
    send.userAgent = "dkr-3ds/" PORT_BUILD_VERSION;
    status = bugreport_send(&send, report->title, report->body, report->files, report->fileCount,
                            MAX_ATTACHMENT_BYTES, show_progress, result, resultSize);
    net_3ds_end();
    return status;
}

/* Keeps a report that could not be sent, so it is not lost. */
static void save_unsent(const Report *report) {
    FILE *file = fopen(DATA_DIR "/bugreport_unsent.txt", "w");
    if (file != NULL) {
        fprintf(file, "%s\n\n%s\n", report->title, report->body != NULL ? report->body : "");
        fclose(file);
    }
}

static int send_and_show(const ReportConfig *config, Report *report) {
    char result[300];
    int status;

    if (report->body == NULL) {
        sShow("Bug report: out of memory");
        return -1;
    }
    status = send_report(config, report, result, sizeof(result));
    if (status == 0) {
        char line[360];
        const char *number = strrchr(result, '/');
        snprintf(line, sizeof(line), port_text("Bug report sent: issue #%s"), number != NULL ? number + 1 : result);
        sShow(line);
        plat_log("bug report sent: %s", result);
    } else {
        char line[360];
        snprintf(line, sizeof(line), port_text("Bug report %s: %s"), port_text(status > 0 ? "partly sent" : "not sent"),
                 result);
        sShow(line);
        plat_log("%s", line);
        if (status < 0) {
            save_unsent(report);
            sShow("Saved as sdmc:/3ds/dkr/bugreport_unsent.txt");
        }
    }
    return status;
}

/* The start of the program: the previous run's files, and the offer to report it. */

void plat_3ds_console_line(const char *text);

static void show_console(const char *text) {
    plat_3ds_console_line(text);
}

/*
 * An unattended run is going (port_autorun.c: the test run, GPU bench, quick test, boss fights or switch bench, each
 * with its own state file): reports are sent without asking, as nobody is there to answer.
 */
static int test_run_active(void) {
    static const char *const sStateFiles[] = { "autorun_state.txt", "gpu_bench_state.txt", "quick_test_state.txt",
                                               "boss_test_state.txt", "switch_bench_state.txt" };
    char text[200], path[96];
    unsigned int i;
    for (i = 0; i < sizeof(sStateFiles) / sizeof(sStateFiles[0]); i++) {
        int length;
        snprintf(path, sizeof(path), DATA_DIR "/%s", sStateFiles[i]);
        length = plat_load_file_start(path, text, sizeof(text) - 1);
        if (length > 0) {
            text[length] = '\0';
            if (strncmp(text, "active=1", 8) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

#endif

/* Returns nonzero if anything was shown on the bottom screen. */
int bugreport_boot(void) {
    int lastUnended = file_exists(RUNNING_PATH);
    FILE *marker;

    // Nothing has been logged yet, so log.txt is still the last run's.
    // So are the files a hang leaves (gfx_c3d.c and the watchdog in plat_3ds.c).
    if (lastUnended) {
        move_file(DATA_DIR "/log.txt", DATA_DIR "/last_log.txt");
        move_file(DATA_DIR "/crash.txt", DATA_DIR "/last_crash.txt");
        move_file(DATA_DIR "/hang_log.txt", DATA_DIR "/last_hang_log.txt");
        move_file(DATA_DIR "/gpu_hang.txt", DATA_DIR "/last_gpu_hang.txt");
        move_file(DATA_DIR "/gpu_hang_cmd.bin", DATA_DIR "/last_gpu_hang_cmd.bin");
        move_file(DATA_DIR "/crash_rec.bin", DATA_DIR "/last_crash_rec.bin");
        move_file(DATA_DIR "/hang_rec.bin", DATA_DIR "/last_hang_rec.bin");
    } else {
        remove(DATA_DIR "/crash.txt");
        remove(DATA_DIR "/last_hang_log.txt");
        remove(DATA_DIR "/last_gpu_hang.txt");
        remove(DATA_DIR "/last_gpu_hang_cmd.bin");
        remove(DATA_DIR "/last_crash_rec.bin");
        remove(DATA_DIR "/last_hang_rec.bin");
    }
    remove(DATA_DIR "/crash_rec.bin");
    remove(DATA_DIR "/hang_rec.bin");
    remove(DATA_DIR "/hang_log.txt");
    remove(DATA_DIR "/gpu_hang.txt");
    remove(DATA_DIR "/gpu_hang_cmd.bin");
    marker = fopen(RUNNING_PATH, "w");
    if (marker != NULL) {
        fclose(marker);
    }
#if !PLAT_DEBUG_TOOLS
    // A player's build keeps the last run's files, but has no bug reports to send them with.
    return 0;
#else
    int crashed, unattended;
    ReportConfig config;
    if (!lastUnended || !load_config(&config)) {
        return 0;
    }

    crashed = file_exists(DATA_DIR "/last_crash.txt");
    show_console(crashed ? "The game crashed last time." : "The game did not exit normally last time.");
    unattended = test_run_active();
    if (unattended) {
        show_console("The test run goes on: sending a report.");
    } else {
        show_console("Send a bug report with its log?");
        show_console("A: Send     B: Skip");
    }
    while (!unattended) {
        u32 down;
        if (!aptMainLoop()) {
            return 1;
        }
        hidScanInput();
        down = hidKeysDown();
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
        if (down & KEY_B) {
            return 1;
        }
        if (down & KEY_A) {
            break;
        }
    }

    {
        Report report;
        char *crashText = NULL;
        unsigned int size;

        memset(&report, 0, sizeof(report));
        report.kind = crashed ? REPORT_LAST_CRASHED : REPORT_LAST_UNENDED;
        if (crashed) {
            crashText = plat_load_file(DATA_DIR "/last_crash.txt", &size);
            if (crashText != NULL) {
                crashText = realloc(crashText, size + 1);
                crashText[size] = '\0';
            }
        }
        snprintf(report.title, sizeof(report.title), "[3DS bug report] %s",
                 crashed                                         ? "crash"
                 : file_exists(DATA_DIR "/last_gpu_hang.txt")    ? "GPU hang"
                 : file_exists(DATA_DIR "/last_hang_log.txt")    ? "the game hung"
                                                                 : "the game did not exit normally");
        if (crashText != NULL) {
            // The first line says what crashed.
            char first[100];
            snprintf(first, sizeof(first), "%s", crashText);
            first[strcspn(first, "\n")] = '\0';
            snprintf(report.title, sizeof(report.title), "[3DS bug report] crash: %s", first);
        }
        // The flight recorder as the crash or the hang left it (the crash's is the later one).
        if (file_exists(DATA_DIR "/last_crash_rec.bin")) {
            attach_saved_recording(&report, "last_crash_rec.bin", "last_timeline.csv");
        } else {
            attach_saved_recording(&report, "last_hang_rec.bin", "last_timeline.csv");
        }
        add_file(&report, "last_log.txt");
        add_file(&report, "last_crash.txt");
        add_file(&report, "env.txt");
        add_file(&report, "last_hang_log.txt");
        add_file(&report, "last_gpu_hang.txt");
        add_file(&report, "last_gpu_hang_cmd.bin");
        {
            // The log file stops after the start-up lines, so its end is in the body too. After a hang, the
            // watchdog saved the latest lines, which are the ones that matter.
            unsigned int logSize;
            char *log = plat_load_file(DATA_DIR "/last_hang_log.txt", &logSize);
            if (log == NULL) {
                log = plat_load_file(DATA_DIR "/last_log.txt", &logSize);
            }
            if (log != NULL) {
                log = realloc(log, logSize + 1);
                log[logSize] = '\0';
            }
            build_body(&report, crashText, log);
            free(log);
        }
        free(crashText);
        sShow = show_console;
        show_console("Sending...");
        if (send_and_show(&config, &report) >= 0) {
            remove(DATA_DIR "/last_crash.txt");
        }
        free_report(&report);
        show_console("");
        if (!unattended) {
            show_console("Press A to start the game.");
        }
        while (!unattended && aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & (KEY_A | KEY_START)) {
                break;
            }
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
    }
    return 1;
#endif
}

/* Called when the program ends normally. */
void bugreport_clean_exit(void) {
    remove(RUNNING_PATH);
}

#if PLAT_DEBUG_TOOLS
/*
 * During the game. A thread of the highest application priority watches the buttons by reading the HID shared
 * memory, so a report can be sent even when the game itself has stopped reading the controls.
 */
static volatile int sBusy;

static void show_status(const char *text) {
    plat_status("%s", text);
}

static u32 peek_keys(void) {
    u32 keys = 0, id;
    id = hidSharedMem[4];
    if (id > 7) {
        id = 7;
    }
    keys = hidSharedMem[10 + id * 4];
    id = hidSharedMem[42 + 4];
    if (id > 7) {
        id = 7;
    }
    if (hidSharedMem[42 + 8 + id * 2 + 1]) {
        keys |= KEY_TOUCH;
    }
    return keys;
}

void plat_log_drain(void);

static void report_now(void) {
    ReportConfig config;
    Report report;
    int haveToken = load_config(&config);
    int before = gPlatSnapshotSaved, waited, quiet = 0;
    unsigned int logged;
    static char recent[32 * 1024];

    static RecState sRecording;
    char game[512];

    rec_press("bug report requested");
    show_status(haveToken ? "Bug report: taking a snapshot"
                          : "Bug report: no token in sdmc:/3ds/dkr/bugreport.txt; saving a snapshot only");
    plat_log("bug report requested");
    // The log as it was before the snapshot, which fills it with the frame's draws (those are in its own file).
    plat_log_recent(recent, sizeof(recent));
    gPlatSnapshotRequested = 1;
    // What the game is doing, and the flight recorder half a second on (a few frames past the press, before the
    // snapshot's slow frames push the time before it out).
    rec_describe_game(game, sizeof(game), 500);
    svcSleepThread(500 * 1000 * 1000);
    rec_copy(&sRecording);
    // A snapshot writes a line for every display list command and draw, which takes the SD card some 20 seconds. So
    // the report waits as long as the log keeps growing (up to two minutes), and gives up after 5 quiet seconds: then
    // the renderer is stuck and the report goes without a snapshot.
    logged = plat_log_total();
    for (waited = 0; waited < 1200 && quiet < 50 && gPlatSnapshotSaved == before; waited++) {
        svcSleepThread(100 * 1000 * 1000);
        if (plat_log_total() != logged) {
            logged = plat_log_total();
            quiet = 0;
        } else {
            quiet++;
        }
        if (waited % 50 == 49) {
            plat_status("Bug report: saving the snapshot (%d s)", (waited + 1) / 10);
            logged = plat_log_total(); // The line above is not progress.
        }
    }
    if (!haveToken) {
        return;
    }

    memset(&report, 0, sizeof(report));
    report.kind = REPORT_BUTTON;
    plat_log_drain(); // The snapshot's files are written behind the renderer's back.
    snprintf(report.game, sizeof(report.game), "%s", game);
    // First, as it is small and usually tells the most.
    attach_recording(&report, &sRecording, "timeline.csv");
    if (gPlatSnapshotSaved != before) {
        char name[64];
        int frame = gPlatSnapshotSaved, largeRecent;
        snprintf(report.title, sizeof(report.title), "[3DS bug report] frame %d", frame);
        snprintf(name, sizeof(name), "diff_%05d_log.txt", frame);
        add_file(&report, name);
        // The recent frame most likely to show the glitch, before the larger files (see save_recent_frames). In 3D (both
        // eyes) or at 800 pixels it is twice the size, so it goes after them instead of using up the attachment budget.
        snprintf(name, sizeof(name), "diff_%05d_recent1.bin", frame);
        largeRecent = recent_frame_is_large(name);
        if (!largeRecent) {
            add_file(&report, name);
        }
        snprintf(name, sizeof(name), "diff_%05d_ref.png", frame);
        add_file(&report, name);
        snprintf(name, sizeof(name), "diff_%05d_gpu.bin", frame);
        add_file(&report, name);
        snprintf(name, sizeof(name), "diff_%05d_cmd.bin", frame);
        add_file(&report, name);
        // The frames of the last few seconds most like a flash (gfx_c3d.c, history_save): a glitch seen for a moment
        // is over before the snapshot. After the snapshot's own files, so it can't push them out of the budget.
        snprintf(name, sizeof(name), "diff_%05d_flash.bin", frame);
        add_file(&report, name);
        if (largeRecent) {
            snprintf(name, sizeof(name), "diff_%05d_recent1.bin", frame);
            add_file(&report, name);
        }
    } else {
        snprintf(report.title, sizeof(report.title), "[3DS bug report] no snapshot (renderer: %s)",
                 gPlatGfxStage != NULL ? gPlatGfxStage : "-");
    }
    add_file(&report, "log.txt");
    if (gPlatSnapshotSaved != before) {
        char name[64];
        snprintf(name, sizeof(name), "diff_%05d_recent2.bin", gPlatSnapshotSaved);
        add_file(&report, name);
    }
    add_file(&report, "env.txt");
    add_file(&report, "settings.txt");
    add_file(&report, "gpu_hang.txt");
    add_file(&report, "gpu_hang_cmd.bin");
    build_body(&report, NULL, recent);
    sShow = show_status;
    send_and_show(&config, &report);
    free_report(&report);
}

static volatile int sReportRequested;
static volatile int sResultsRequested;
static char sResultsTitle[160];
static char sResultsSummary[6144];

/* "Send a bug report" in the Settings menu (menu_3ds.c, via update_3ds.c): the same as SELECT + touch. */
void bugreport_request(void) {
    sReportRequested = 1;
}

/* Called by the test run when it ends; the report is sent from the button thread. */
void plat_send_test_results(const char *title, const char *summary) {
    if (sResultsRequested) {
        // An earlier one hasn't started sending yet: this one replaces it.
        plat_log("bug report: test results replaced before they were sent: %s", sResultsTitle);
    }
    sResultsRequested = 0;
    __sync_synchronize();
    snprintf(sResultsTitle, sizeof(sResultsTitle), "%s", title);
    snprintf(sResultsSummary, sizeof(sResultsSummary), "%s", summary);
    __sync_synchronize();
    sResultsRequested = 1;
}

/* Sends the requested results. They are copied first, so a run that ends while they are sent asks for its own. */
static void send_test_results(void) {
    ReportConfig config;
    Report report;
    static char recent[16 * 1024];
    static char sTitle[sizeof(sResultsTitle)], sSummary[sizeof(sResultsSummary)];

    memcpy(sTitle, sResultsTitle, sizeof(sTitle));
    memcpy(sSummary, sResultsSummary, sizeof(sSummary));
    __sync_synchronize();
    sResultsRequested = 0;

    if (!load_config(&config)) {
        show_status("Test run results: no token in bugreport.txt, so they stay on the SD card");
        return;
    }
    memset(&report, 0, sizeof(report));
    report.kind = REPORT_TEST_RUN;
    report.extra = sSummary;
    snprintf(report.title, sizeof(report.title), "%s", sTitle);
    add_file(&report, "autorun_results.csv");
    add_file(&report, "gpu_bench_results.csv");
    add_file(&report, "quick_test_results.csv");
    add_file(&report, "boss_test_results.csv");
    add_file(&report, "switch_bench_results.csv");
    add_file(&report, "autorun_problems.txt");
    add_file(&report, "env.txt");
    add_file(&report, "settings.txt");
    {
        // The flight recorder's last half minute (the end of the last race), and its hitch counts since start-up.
        static RecState sRecording;
        rec_copy(&sRecording);
        attach_recording(&report, &sRecording, "timeline.csv");
    }
    add_file(&report, "autorun_hitches.txt"); // Last: the largest after a long run.
    plat_log_recent(recent, sizeof(recent));
    build_body(&report, NULL, recent);
    sShow = show_status;
    send_and_show(&config, &report);
    free_report(&report);
}

static void button_thread(void *arg) {
    u32 last = 0;
    (void) arg;
    bugreport_watch_thread("bug report");
    for (;;) {
        u32 keys = peek_keys();
        // SELECT held and the touch just started (not in a player's build, plat.h), or the Settings menu's row.
        if (((PLAT_DEBUG_TOOLS && (keys & KEY_SELECT) && (keys & KEY_TOUCH) && !(last & KEY_TOUCH)) ||
             sReportRequested) &&
            !sBusy) {
            sBusy = 1;
            net_3ds_background(1);
            report_now();
            net_3ds_background(0);
            sReportRequested = 0;
            sBusy = 0;
        }
        if (sResultsRequested && !sBusy) {
            sBusy = 1;
            net_3ds_background(1);
            send_test_results();
            net_3ds_background(0);
            sBusy = 0;
        }
        last = keys;
        svcSleepThread(50 * 1000 * 1000);
    }
}

void bugreport_start(void) {
    // 64 KB: the TLS handshake needs a deep stack.
    if (threadCreate(button_thread, NULL, 64 * 1024, 0x18, 0, true) == NULL) {
        plat_log("bug report: could not create its thread");
    }
}
#else
void bugreport_request(void) {
}

void plat_send_test_results(const char *title, const char *summary) {
    (void) title;
    (void) summary;
}

void bugreport_start(void) {
}
#endif
