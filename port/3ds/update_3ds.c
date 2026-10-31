/*
 * Updates from the bottom screen. CI (.github/workflows/Port.yml) publishes every 3DS build as a GitHub release:
 * "3ds-master-debug" is the newest debug build of master, "3ds-newest" the newest debug build of any branch ("3ds-master"
 * is master's player build, which has no updater, so this never installs it). Each has dkr.3dsx, dkr.cia and build.txt
 * (commit, branch, date, size and sha256 of dkr.3dsx, cia_size and cia_sha256 of dkr.cia).
 *
 * Tapping "Check for updates" reads both build.txt files and offers them. Started from the Homebrew Launcher, the
 * chosen dkr.3dsx is downloaded next to the running one, checked against its size and sha256, and then replaces it.
 * Installed as a .cia, dkr.cia is downloaded to sdmc:/3ds/dkr/update.cia, checked the same way and installed over the
 * running title through the system's installer (AM), as FBI updates itself. Either way the new build runs the next time
 * the game starts. For a private repository the token in sdmc:/3ds/dkr/bugreport.txt (the bug report one) is used; it
 * needs read access to the repository's contents.
 *
 * The rest of the bottom screen's panel lives here too. A player's build (plat.h PLAT_DEBUG_TOOLS) has the panel but
 * no updater, and so neither curl nor mbedtls.
 */
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu_3ds.h"
#include "plat/plat.h"
#if PLAT_DEBUG_TOOLS
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include "net_3ds.h"
#else
#define net_3ds_background(background) ((void) 0) /* No network in a player build. */
#endif

#ifndef PORT_BUILD_VERSION
#define PORT_BUILD_VERSION "unknown"
#endif
#ifndef PORT_BUILD_COMMIT
#define PORT_BUILD_COMMIT ""
#endif

#define CONFIG_PATH "sdmc:/3ds/dkr/bugreport.txt"
#define DEFAULT_REPO "chrispelletier-gif/Diddy-Kong-Racing-3DS"
#define MAX_DOWNLOAD (16 * 1024 * 1024)
#define CIA_DOWNLOAD_PATH "sdmc:/3ds/dkr/update.cia"

void plat_3ds_set_buttons(const char *const *labels, int count);
void plat_3ds_panel_text(const char *text);
void bugreport_watch_thread(const char *name);
void bugreport_request(void);
long port_autorun_enabled(void);
void port_autorun_request(long start);

/*
 * The panel's buttons also lead to everything else on the bottom screen: in the idle state there is "Settings",
 * which opens the Settings menu (menu_3ds.c: anti-aliasing, the profiler, the test run, updates and bug reports),
 * and "Stop test" while a test run is on. Touches anywhere else on the screen do nothing unless the menu is open.
 */
enum UpdateState {
    STATE_IDLE,     /* Settings (/ Stop test). */
    STATE_MENU,     /* The menu is open: Close or Back / Prev / Next. */
    STATE_CONFIRM,  /* Stop the test run / - / cancel. */
    STATE_BUSY,     /* Checking or downloading; taps are ignored. */
    STATE_CHOOSE,   /* master / newest / cancel. */
    STATE_INSTALLED /* "Quit to finish the update". */
};

typedef struct Channel {
    const char *tag;
    const char *label;
    int found;
    char commit[48];
    char branch[96];
    char date[32];
    unsigned long size;
    char sha256[72];
    char buildUrl[256];  /* Where to read build.txt, dkr.3dsx and dkr.cia; for a private repository the API's asset URLs. */
    char binaryUrl[256];
    unsigned long ciaSize;
    char ciaSha256[72];
    char ciaUrl[256];
} Channel;

typedef struct Config {
    char token[256];
    char repo[128];
} Config;

static char sSelfPath[256];
/* What an update renames the running build to until the new one is in place. */
#define PREVIOUS_SUFFIX "-previous.3dsx"
static int sIsCia; // Installed as a .cia rather than started from the Homebrew Launcher.
static volatile int sStarted;
static volatile int sState;
static volatile int sTappedButton = -1; // Or TAP_MENU, at sTapX, sTapY.
static volatile int sTapX, sTapY;
#define TAP_MENU -2
#define TAP_REFRESH -3 // Not a tap: a race started or ended, so the idle buttons change.
static LightEvent sTapEvent;
static Channel sChannels[2] = { { "3ds-master-debug", "master" }, { "3ds-newest", "newest" } };
static int sChosen;

/* The panel: rows 1-3 are the buttons (screen rows 26-28, y 200-223). */
#define BUTTON_TOP 200
#define BUTTON_BOTTOM 224

static volatile int sButtonCount; // Buttons now shown, each a space then a block of (40 / count - 1) characters.

/*
 * Buttons in the game's style (plat/bottom_screen.c); a label starting with '-' is a button that cannot be tapped,
 * drawn grey ("-" alone: a gap), and one starting with '+' is a switch that is on, drawn green.
 */
static void draw_buttons(const char *const *labels, int count) {
    plat_3ds_set_buttons(labels, count);
    sButtonCount = count;
}

static void draw_text_button(const char *label) {
    draw_buttons(&label, 1);
}

static int sTestRunShown = -1; // Whether the idle buttons include "Stop test".
static volatile int sRacing;    // A race is on, with its HUD on the bottom screen: Pause comes first.
static int sRacingShown;

/* What the idle buttons do. */
enum { IDLE_PAUSE, IDLE_SETTINGS, IDLE_CONTROLS, IDLE_UPDATES, IDLE_STOP_TEST };
static int sIdleActions[3];

/*
 * In a race: Pause and Settings. Otherwise Settings, Controls and Updates (a player's build has no updater), with
 * "Stop test" in the last place while a test run is on.
 */
static void draw_home(void) {
    const char *labels[3];
    int count = 0;
    sTestRunShown = port_autorun_enabled() != 0;
    sRacingShown = sRacing;
    if (sRacingShown) {
        sIdleActions[count] = IDLE_PAUSE;
        labels[count++] = "Pause";
    }
    sIdleActions[count] = IDLE_SETTINGS;
    labels[count++] = "Settings";
    if (!sRacingShown) {
        sIdleActions[count] = IDLE_CONTROLS;
        labels[count++] = "Controls";
    }
    if (sTestRunShown) {
        sIdleActions[count] = IDLE_STOP_TEST;
        labels[count++] = "Stop test";
    } else if (!sRacingShown && PLAT_DEBUG_TOOLS) {
        sIdleActions[count] = IDLE_UPDATES;
        labels[count++] = "Updates";
    }
    draw_buttons(labels, count);
}

static void draw_menu_buttons(void) {
    const char *labels[3];
    labels[0] = "Close";
    labels[1] = menu_3ds_can_scroll(-1) ? "Prev" : "-Prev";
    labels[2] = menu_3ds_can_scroll(1) ? "Next" : "-Next";
    draw_buttons(labels, 3);
}

/* Stop test / - / cancel. */
static void draw_confirm(void) {
    static const char *const sRunning[3] = { "Stop test", "-", "cancel" };
    draw_buttons(sRunning, 3);
    plat_status("Stop the test run? The results so far are sent as a report.");
}

static void show_busy(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void show_busy(const char *fmt, ...) {
    char text[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), port_text(fmt), args);
    va_end(args);
    plat_3ds_panel_text(text);
}

/* "sdmc:/3ds/dkr.3dsx" -> "sdmc:/3ds/dkr-previous.3dsx" (or "....old" for a name that isn't .3dsx). */
static void previous_path(const char *path, char *out, size_t size) {
    size_t length = strlen(path);
    if (length > 5 && strcmp(path + length - 5, ".3dsx") == 0) {
        snprintf(out, size, "%.*s" PREVIOUS_SUFFIX, (int) (length - 5), path);
    } else {
        snprintf(out, size, "%s.old", path);
    }
}

#if PLAT_DEBUG_TOOLS
static void load_config(Config *config) {
    unsigned int size;
    char *text = plat_load_file(CONFIG_PATH, &size), *line, *next;

    memset(config, 0, sizeof(*config));
    snprintf(config->repo, sizeof(config->repo), "%s", DEFAULT_REPO);
    if (text == NULL) {
        return;
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
        for (end = value; end > line && (end[-1] == ' ' || end[-1] == '\t');) {
            *--end = '\0';
        }
        *value++ = '\0';
        while (*value == ' ' || *value == '\t') {
            value++;
        }
        for (end = value + strlen(value); end > value && (end[-1] == ' ' || end[-1] == '\t');) {
            *--end = '\0';
        }
        if (strcmp(line, "token") == 0) {
            snprintf(config->token, sizeof(config->token), "%s", value);
        } else if (strcmp(line, "repo") == 0) {
            snprintf(config->repo, sizeof(config->repo), "%s", value);
        }
    }
    free(text);
}

/* Downloads. */

typedef struct Download {
    char *text; /* To memory (up to textCapacity bytes)... */
    size_t textLength, textCapacity;
    FILE *file; /* ...or to a file, with its sha256. */
    mbedtls_sha256_context sha;
    unsigned long written;
    unsigned long expected;
    int lastPercent;
} Download;

static size_t on_data(char *data, size_t size, size_t count, void *user) {
    Download *download = (Download *) user;
    size_t length = size * count;

    if (download->file != NULL) {
        if (download->written + length > MAX_DOWNLOAD || fwrite(data, 1, length, download->file) != length) {
            return 0;
        }
        mbedtls_sha256_update_ret(&download->sha, (const unsigned char *) data, length);
    } else {
        if (download->textLength + length >= download->textCapacity) {
            return 0;
        }
        memcpy(download->text + download->textLength, data, length);
        download->textLength += length;
        download->text[download->textLength] = '\0';
    }
    download->written += length;
    if (download->file != NULL && download->expected > 0) {
        int percent = (int) ((unsigned long long) download->written * 100 / download->expected);
        if (percent / 5 != download->lastPercent / 5) {
            download->lastPercent = percent;
            show_busy("Downloading %s: %d%%", sChannels[sChosen].label, percent);
        }
    }
    return length;
}

/*
 * One libcurl handle serves the requests of a check or a download, so the connection to github.com (and its TLS
 * session) is reused: every new connection costs a handshake of a second or more on the 3DS. Dropped by http_done.
 */
static CURL *sCurl;

static void http_done(void) {
    if (sCurl != NULL) {
        curl_easy_cleanup(sCurl);
        sCurl = NULL;
    }
}

/* GETs url into download. Returns the HTTP status, or 0 with the error in error. */
static long http_get(const Config *config, const char *url, int binary, Download *download, char *error,
                     size_t errorSize) {
    CURL *curl;
    struct curl_slist *headers = NULL;
    char auth[300], curlError[CURL_ERROR_SIZE];
    long status = 0;
    CURLcode code;
    const char *caFile = net_3ds_ca_file();

    if (sCurl == NULL) {
        sCurl = curl_easy_init();
    } else {
        curl_easy_reset(sCurl); // Keeps the open connections and TLS sessions.
    }
    curl = sCurl;
    if (curl == NULL) {
        snprintf(error, errorSize, "out of memory (curl)");
        return 0;
    }
    if (config->token[0] != '\0') {
        // Only sent to the API; libcurl drops it when a download redirects to another host.
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", config->token);
        headers = curl_slist_append(headers, auth);
        headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
        headers = curl_slist_append(headers, binary ? "Accept: application/octet-stream"
                                                    : "Accept: application/vnd.github+json");
    }
    curlError[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dkr-3ds/" PORT_BUILD_VERSION);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, download);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    // Give up on a download that stalls (under 1 KB/s for 30 seconds), not on a slow one.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (caFile != NULL) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, caFile);
    } else {
        struct curl_blob blob;
        blob.data = (void *) net_3ds_ca_pem();
        blob.len = strlen(net_3ds_ca_pem());
        blob.flags = CURL_BLOB_COPY;
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    }
    code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (code != CURLE_OK && (status < 300 || status == 0)) {
        snprintf(error, errorSize, "%s", curlError[0] != '\0' ? curlError : curl_easy_strerror(code));
        status = 0;
    }
    curl_slist_free_all(headers);
    return status;
}

/* The closing quote of the JSON string that starts at p, or NULL. */
static const char *string_end(const char *p) {
    for (; *p != '\0' && *p != '"'; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
        }
    }
    return *p == '"' ? p : NULL;
}

/* Finds the next "key": "value" pair at or after p; key and value point into the JSON. Returns where it ends. */
static const char *next_pair(const char *p, const char **key, size_t *keyLength, const char **value,
                             size_t *valueLength) {
    while ((p = strchr(p, '"')) != NULL) {
        const char *k = p + 1, *end = string_end(k), *v;
        if (end == NULL) {
            return NULL;
        }
        for (v = end + 1; *v == ' ' || *v == '\t' || *v == '\r' || *v == '\n'; v++) {
        }
        if (*v != ':') {
            p = end + 1;
            continue;
        }
        for (v++; *v == ' ' || *v == '\t' || *v == '\r' || *v == '\n'; v++) {
        }
        if (*v != '"') {
            p = v;
            continue;
        }
        *key = k;
        *keyLength = end - k;
        *value = v + 1;
        end = string_end(v + 1);
        if (end == NULL) {
            return NULL;
        }
        *valueLength = end - (v + 1);
        return end + 1;
    }
    return NULL;
}

/* In a release's JSON, the API address of the asset called name ("url" comes before "name" in each asset). */
static int find_asset_url(const char *json, const char *name, char *out, size_t outSize) {
    const char *p = json, *key, *value, *url = NULL;
    size_t keyLength, valueLength, urlLength = 0;

    while ((p = next_pair(p, &key, &keyLength, &value, &valueLength)) != NULL) {
        if (keyLength == 3 && memcmp(key, "url", 3) == 0) {
            url = value;
            urlLength = valueLength;
        } else if (keyLength == 4 && memcmp(key, "name", 4) == 0 && valueLength == strlen(name) &&
                   memcmp(value, name, valueLength) == 0) {
            if (url == NULL || urlLength >= outSize) {
                return 0;
            }
            memcpy(out, url, urlLength);
            out[urlLength] = '\0';
            return strstr(out, "/releases/assets/") != NULL;
        }
    }
    return 0;
}

static void read_field(const char *text, const char *name, char *out, size_t outSize) {
    char key[32];
    const char *p;
    size_t length;

    out[0] = '\0';
    snprintf(key, sizeof(key), "%s=", name);
    for (p = text; p != NULL; p = strchr(p, '\n')) {
        if (*p == '\n') {
            p++;
        }
        if (strncmp(p, key, strlen(key)) == 0) {
            p += strlen(key);
            length = strcspn(p, "\r\n");
            if (length >= outSize) {
                length = outSize - 1;
            }
            memcpy(out, p, length);
            out[length] = '\0';
            return;
        }
    }
}

/* Reads the channel's build.txt. Returns 1 if it was found, 0 if the release does not exist, -1 on an error. */
static int check_channel(const Config *config, Channel *channel, char *error, size_t errorSize) {
    static char text[64 * 1024];
    Download download;
    char url[256], size[24];
    long status;

    memset(&download, 0, sizeof(download));
    download.text = text;
    download.textCapacity = sizeof(text);
    text[0] = '\0';
    channel->found = 0;
    if (config->token[0] != '\0') {
        snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/tags/%s", config->repo, channel->tag);
        status = http_get(config, url, 0, &download, error, errorSize);
        if (status == 404) {
            return 0;
        }
        if (status != 200) {
            if (status != 0) {
                snprintf(error, errorSize, "GitHub said %ld%s", status,
                         status == 401 || status == 403 ? " (check the token)" : "");
            }
            return -1;
        }
        if (!find_asset_url(text, "build.txt", channel->buildUrl, sizeof(channel->buildUrl)) ||
            !find_asset_url(text, "dkr.3dsx", channel->binaryUrl, sizeof(channel->binaryUrl))) {
            return 0;
        }
        if (!find_asset_url(text, "dkr.cia", channel->ciaUrl, sizeof(channel->ciaUrl))) {
            channel->ciaUrl[0] = '\0';
        }
    } else {
        snprintf(channel->buildUrl, sizeof(channel->buildUrl), "https://github.com/%s/releases/download/%s/build.txt",
                 config->repo, channel->tag);
        snprintf(channel->binaryUrl, sizeof(channel->binaryUrl),
                 "https://github.com/%s/releases/download/%s/dkr.3dsx", config->repo, channel->tag);
        snprintf(channel->ciaUrl, sizeof(channel->ciaUrl), "https://github.com/%s/releases/download/%s/dkr.cia",
                 config->repo, channel->tag);
    }

    memset(&download, 0, sizeof(download));
    download.text = text;
    download.textCapacity = sizeof(text);
    text[0] = '\0';
    status = http_get(config, channel->buildUrl, 1, &download, error, errorSize);
    if (status == 404) {
        // A private repository answers 404 without a token too.
        return 0;
    }
    if (status != 200) {
        if (status != 0) {
            snprintf(error, errorSize, "GitHub said %ld", status);
        }
        return -1;
    }
    read_field(text, "commit", channel->commit, sizeof(channel->commit));
    read_field(text, "branch", channel->branch, sizeof(channel->branch));
    read_field(text, "date", channel->date, sizeof(channel->date));
    read_field(text, "sha256", channel->sha256, sizeof(channel->sha256));
    read_field(text, "size", size, sizeof(size));
    channel->size = strtoul(size, NULL, 10);
    read_field(text, "cia_sha256", channel->ciaSha256, sizeof(channel->ciaSha256));
    read_field(text, "cia_size", size, sizeof(size));
    channel->ciaSize = strtoul(size, NULL, 10);
    if (channel->commit[0] == '\0' || channel->sha256[0] == '\0' || channel->size == 0) {
        snprintf(error, errorSize, "%s/build.txt is incomplete", channel->tag);
        return -1;
    }
    if (sIsCia && (channel->ciaUrl[0] == '\0' || channel->ciaSha256[0] == '\0' || channel->ciaSize == 0)) {
        // Builds from before the .cia was published.
        snprintf(error, errorSize, "no .cia in this build");
        return -1;
    }
    channel->found = 1;
    return 1;
}

static int is_installed(const Channel *channel) {
    return PORT_BUILD_COMMIT[0] != '\0' && strcmp(channel->commit, PORT_BUILD_COMMIT) == 0;
}

static int check(void) {
    Config config;
    char error[200];
    int i, found = 0;

    if (sSelfPath[0] == '\0' && !sIsCia) {
        plat_status("Update: the game was not started from the SD card");
        return 0;
    }
    load_config(&config);
    show_busy("Checking for updates...");
    error[0] = '\0';
    if (net_3ds_begin(error, sizeof(error)) != 0) {
        plat_status("Update: %s", error);
        return 0;
    }
    for (i = 0; i < 2; i++) {
        int result = check_channel(&config, &sChannels[i], error, sizeof(error));
        if (result < 0) {
            plat_status("Update: %s: %s", sChannels[i].label, error);
        }
        found += result > 0;
    }
    http_done();
    net_3ds_end();
    if (!found) {
        plat_status("Update: no builds found%s", config.token[0] == '\0' ? port_text(" (a private repo needs the token)") : "");
        return 0;
    }

    plat_status("This build: %.7s", PORT_BUILD_COMMIT[0] != '\0' ? PORT_BUILD_COMMIT : PORT_BUILD_VERSION);
    for (i = 0; i < 2; i++) {
        const Channel *channel = &sChannels[i];
        if (channel->found) {
            plat_status("%-6s %.7s %.16s %.10s%s", channel->label, channel->commit, channel->branch, channel->date,
                        is_installed(channel) ? port_text(" (this)") : "");
        } else {
            plat_status("%-6s none", channel->label);
        }
    }
    return 1;
}

static void draw_choices(void) {
    const char *labels[3];
    labels[0] = sChannels[0].found ? "master" : "-master";
    labels[1] = sChannels[1].found ? "newest" : "-newest";
    labels[2] = "cancel";
    draw_buttons(labels, 3);
}

/* Downloads url to path, checking it against size and sha256. Returns 1 if it arrived whole. */
static int download_verified(const char *url, unsigned long size, const char *sha256, const char *path,
                             const char *label) {
    Config config;
    Download download;
    char error[200], digest[65];
    unsigned char hash[32];
    long status;
    int i;

    load_config(&config);
    memset(&download, 0, sizeof(download));
    download.expected = size;
    download.lastPercent = -5;
    download.file = fopen(path, "wb");
    if (download.file == NULL) {
        plat_status("Update: cannot write %s", path);
        return 0;
    }
    setvbuf(download.file, NULL, _IOFBF, 64 * 1024);
    mbedtls_sha256_init(&download.sha);
    mbedtls_sha256_starts_ret(&download.sha, 0);

    show_busy("Downloading %s...", label);
    error[0] = '\0';
    if (net_3ds_begin(error, sizeof(error)) != 0) {
        status = 0;
    } else {
        status = http_get(&config, url, 1, &download, error, sizeof(error));
        http_done();
        net_3ds_end();
    }
    mbedtls_sha256_finish_ret(&download.sha, hash);
    mbedtls_sha256_free(&download.sha);
    if (fclose(download.file) != 0 && status == 200) {
        snprintf(error, sizeof(error), "could not finish writing the file");
        status = 0;
    }
    for (i = 0; i < 32; i++) {
        snprintf(digest + i * 2, 3, "%02x", hash[i]);
    }
    if (status == 200 && download.written != size) {
        snprintf(error, sizeof(error), "got %lu of %lu bytes", download.written, size);
        status = 0;
    } else if (status == 200 && strcmp(digest, sha256) != 0) {
        snprintf(error, sizeof(error), "the download is damaged (sha256 differs)");
        status = 0;
    } else if (status != 200 && status != 0) {
        snprintf(error, sizeof(error), "GitHub said %ld", status);
    }
    if (status != 200) {
        remove(path);
        plat_status("Update failed: %s", error);
        return 0;
    }
    return 1;
}

/* Installs the downloaded .cia over the running title. Returns 0 (and leaves the installed one as it was) on failure. */
static int install_cia_file(const char *path, char *error, size_t errorSize) {
    static u8 buffer[64 * 1024];
    Handle handle;
    FILE *file;
    u64 offset = 0;
    size_t length;
    Result res;

    file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error, errorSize, "cannot read %s", path);
        return 0;
    }
    res = amInit();
    if (R_FAILED(res)) {
        fclose(file);
        snprintf(error, errorSize, "the installer did not start (0x%08lX)", (unsigned long) res);
        return 0;
    }
    res = AM_StartCiaInstall(MEDIATYPE_SD, &handle);
    if (R_SUCCEEDED(res)) {
        while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0) {
            u32 written = 0;
            res = FSFILE_Write(handle, &written, offset, buffer, (u32) length, 0);
            if (R_FAILED(res) || written != length) {
                res = R_FAILED(res) ? res : -1;
                break;
            }
            offset += length;
        }
        if (R_SUCCEEDED(res) && ferror(file)) {
            res = -1;
        }
        if (R_SUCCEEDED(res)) {
            res = AM_FinishCiaInstall(handle);
        } else {
            AM_CancelCIAInstall(handle);
        }
    }
    amExit();
    fclose(file);
    if (R_FAILED(res)) {
        snprintf(error, errorSize, "the install failed (0x%08lX)", (unsigned long) res);
        return 0;
    }
    return 1;
}

static int install(Channel *channel) {
    char error[200], newPath[272], oldPath[272];

    if (sIsCia) {
        if (!download_verified(channel->ciaUrl, channel->ciaSize, channel->ciaSha256, CIA_DOWNLOAD_PATH,
                               channel->label)) {
            return 0;
        }
        show_busy("Installing %s...", channel->label);
        error[0] = '\0';
        if (!install_cia_file(CIA_DOWNLOAD_PATH, error, sizeof(error))) {
            remove(CIA_DOWNLOAD_PATH);
            plat_status("Update failed: %s", error);
            return 0;
        }
        remove(CIA_DOWNLOAD_PATH);
        plat_log("update: installed %s (%s) as a .cia", channel->commit, channel->branch);
        plat_status("Installed %.7s (%.20s).", channel->commit, channel->branch);
        plat_status("It runs the next time the game starts.");
        return 1;
    }

    snprintf(newPath, sizeof(newPath), "%s.new", sSelfPath);
    previous_path(sSelfPath, oldPath, sizeof(oldPath));
    if (!download_verified(channel->binaryUrl, channel->size, channel->sha256, newPath, channel->label)) {
        return 0;
    }

    // The running build was read into memory at start, so its file can be replaced. The old one is kept until the
    // new one is in place, under a .3dsx name: if the power goes between the two renames, the Homebrew Launcher still
    // lists it, and it puts itself back when started (recover_install).
    remove(oldPath);
    if (rename(sSelfPath, oldPath) != 0 || rename(newPath, sSelfPath) != 0) {
        rename(oldPath, sSelfPath);
        remove(newPath);
        plat_status("Update failed: could not replace %s", sSelfPath);
        return 0;
    }
    remove(oldPath);
    plat_log("update: installed %s (%s) to %s", channel->commit, channel->branch, sSelfPath);
    plat_status("Installed %.7s (%.20s).", channel->commit, channel->branch);
    plat_status("It runs the next time the game starts.");
    return 1;
}

#else
static int check(void) {
    return 0;
}

static void draw_choices(void) {
}

static int install(Channel *channel) {
    (void) channel;
    return 0;
}
#endif

int plat_3ds_setup_touch(int x, int y);
int plat_3ds_take_close_menu(void);

/* The touch screen, from the game's controller polling (input_3ds.c). Returns nonzero if the touch was a button. */
int update_3ds_touch(int x, int y) {
    int button, count = sButtonCount, cell = count > 1 ? 40 / count : 40;
    if (plat_3ds_setup_touch(x, y)) {
        return 1; // The first-run setup's tiles and buttons take every tap while they show.
    }
    if (!sStarted || y >= BUTTON_BOTTOM) {
        return 0;
    }
    if (y < BUTTON_TOP) {
        if (sState != STATE_MENU) {
            return 0;
        }
        sTapX = x;
        sTapY = y;
        sTappedButton = TAP_MENU;
        LightEvent_Signal(&sTapEvent);
        return 1;
    }
    // Columns: one wide button, or several, each a space then a block (8 pixels a character).
    button = sState == STATE_INSTALLED ? 0 : (x / 8) / cell;
    if (sState == STATE_BUSY || button >= count || (count > 1 && (x / 8) % cell == 0)) {
        return 1;
    }
    sTappedButton = button;
    LightEvent_Signal(&sTapEvent);
    return 1;
}

/* Looks for new builds; returns 1 if the choices are shown, 0 (back to idle) if there are none. */
static int check_updates(void) {
    int found;

    sState = STATE_BUSY;
    net_3ds_background(1);
    found = check();
    net_3ds_background(0);
    if (found) {
        sState = STATE_CHOOSE;
        draw_choices();
        return 1;
    }
    sState = STATE_IDLE;
    return 0;
}

static void update_thread(void *arg) {
    (void) arg;
    bugreport_watch_thread("update");
    for (;;) {
        int button, installed;
        // Every two seconds without a tap, the test run's buttons and rows are brought up to date (a run ends by
        // itself).
        if (LightEvent_WaitTimeout(&sTapEvent, 2000000000LL) != 0) {
            if (sState == STATE_IDLE &&
                (sTestRunShown != (port_autorun_enabled() != 0) || sRacingShown != sRacing)) {
                draw_home();
            } else if (sState == STATE_MENU) {
                menu_3ds_refresh();
            }
            continue;
        }
        button = sTappedButton;
        if (button == TAP_REFRESH) {
            if (sState == STATE_IDLE && sRacingShown != sRacing) {
                draw_home();
            }
            continue;
        }
        if (button == TAP_MENU && sState != STATE_MENU) {
            continue; // The menu closed before this tap was seen.
        }
        switch (sState) {
            case STATE_IDLE:
                if (button < 0 || button >= sButtonCount) {
                    break;
                }
                plat_request_ui_sound(PLAT_UI_SOUND_SELECT);
                if (sIdleActions[button] == IDLE_PAUSE) {
                    plat_request_game_pause();
                } else if (sIdleActions[button] == IDLE_SETTINGS) {
                    sState = STATE_MENU;
                    menu_3ds_open();
                    draw_menu_buttons();
                } else if (sIdleActions[button] == IDLE_CONTROLS) {
                    sState = STATE_MENU;
                    if (!menu_3ds_open_page("Controls")) {
                        menu_3ds_open();
                    }
                    draw_menu_buttons();
                } else if (sIdleActions[button] == IDLE_UPDATES) {
                    if (check_updates()) {
                        break;
                    }
                    draw_home();
                } else {
                    sState = STATE_CONFIRM;
                    draw_confirm();
                }
                break;
            case STATE_MENU: {
                int result = MENU_STAY;
                if (button == TAP_MENU) {
                    result = menu_3ds_tap(sTapX, sTapY);
                } else if (button == 0) {
                    plat_request_ui_sound(PLAT_UI_SOUND_BACK);
                    result = MENU_CLOSE;
                } else if (menu_3ds_can_scroll(button == 1 ? -1 : 1)) {
                    plat_request_ui_sound(PLAT_UI_SOUND_MOVE);
                    menu_3ds_scroll(button == 1 ? -1 : 1);
                }
                if (result == MENU_STAY && plat_3ds_take_close_menu()) {
                    result = MENU_CLOSE;
                }
                if (result == MENU_STAY) {
                    draw_menu_buttons();
                    break;
                }
                menu_3ds_close();
                sState = STATE_IDLE;
                if (result == MENU_BUGREPORT) {
                    bugreport_request();
                } else if (result == MENU_UPDATES && check_updates()) {
                    break;
                }
                draw_home();
                break;
            }
            case STATE_CONFIRM:
                if (button == 1) {
                    break; // The grey button.
                }
                plat_request_ui_sound(button == 0 ? PLAT_UI_SOUND_SELECT : PLAT_UI_SOUND_BACK);
                if (button == 0) {
                    port_autorun_request(0);
                    // The run turns off at the game's next frame.
                    svcSleepThread(200 * 1000 * 1000);
                }
                sState = STATE_IDLE;
                draw_home();
                break;
            case STATE_CHOOSE:
                if (button == 2) {
                    plat_request_ui_sound(PLAT_UI_SOUND_BACK);
                    sState = STATE_IDLE;
                    draw_home();
                } else if (sChannels[button].found) {
                    plat_request_ui_sound(PLAT_UI_SOUND_SELECT);
                    sState = STATE_BUSY;
                    sChosen = button;
                    net_3ds_background(1);
                    installed = install(&sChannels[button]);
                    net_3ds_background(0);
                    if (installed) {
                        sState = STATE_INSTALLED;
                        draw_text_button("Quit to finish the update");
                    } else {
                        sState = STATE_CHOOSE;
                        draw_choices();
                    }
                }
                break;
            case STATE_INSTALLED:
                show_busy("Closing...");
                plat_request_quit();
                break;
        }
    }
}

/* From the game thread (plat_set_race_hud) when a race with the bottom screen's HUD starts or ends. */
void update_3ds_race_changed(int racing) {
    sRacing = racing;
    if (sStarted) {
        sTappedButton = TAP_REFRESH;
        LightEvent_Signal(&sTapEvent);
    }
}

/* selfPath: the running dkr.3dsx (argv[0] from the Homebrew Launcher), or NULL if not known (always for a .cia). */
/*
 * Finishes an update that the power cut between its two renames. Started as dkr-previous.3dsx with no dkr.3dsx: this
 * build goes back to being dkr.3dsx (the downloaded one, which didn't get there, is dropped). Started as dkr.3dsx:
 * whatever an earlier update left is removed.
 */
static void recover_install(void) {
    char path[272], other[272];
    size_t length = strlen(sSelfPath), suffix = strlen(PREVIOUS_SUFFIX);
    FILE *f;

    if (length > suffix && strcmp(sSelfPath + length - suffix, PREVIOUS_SUFFIX) == 0) {
        snprintf(path, sizeof(path), "%.*s.3dsx", (int) (length - suffix), sSelfPath);
        if ((f = fopen(path, "rb")) != NULL) {
            fclose(f);
            return;
        }
        snprintf(other, sizeof(other), "%s.new", path);
        remove(other);
        if (rename(sSelfPath, path) == 0) {
            plat_log("update: an update was cut short, %s put back", path);
            snprintf(sSelfPath, sizeof(sSelfPath), "%s", path);
        }
        return;
    }
    previous_path(sSelfPath, path, sizeof(path));
    remove(path);
    snprintf(path, sizeof(path), "%s.new", sSelfPath);
    remove(path);
}

void update_3ds_start(const char *selfPath) {
    sIsCia = !envIsHomebrew();
    // Only a file on the SD card can be replaced (3dslink, for one, sends the program over the network).
    if (selfPath != NULL && strncmp(selfPath, "sdmc:/", 6) == 0) {
        snprintf(sSelfPath, sizeof(sSelfPath), "%s", selfPath);
        if (!sIsCia) {
            recover_install();
        }
    }
    LightEvent_Init(&sTapEvent, RESET_ONESHOT);
    sState = STATE_IDLE;
    draw_home();
    sStarted = 1;
    // 64 KB: the TLS handshake needs a deep stack.
    if (threadCreate(update_thread, NULL, 64 * 1024, 0x18, 0, true) == NULL) {
        plat_log("update: could not create its thread");
    }
}
