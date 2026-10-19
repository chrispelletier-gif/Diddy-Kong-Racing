/*
 * Updates from the bottom screen. CI (.github/workflows/Port.yml) publishes every 3DS build as a GitHub release:
 * "3ds-master" is the newest build of master, "3ds-newest" the newest build of any branch. Each has dkr.3dsx and
 * build.txt (commit, branch, date, size and sha256 of dkr.3dsx).
 *
 * Tapping "Check for updates" reads both build.txt files and offers them. The chosen dkr.3dsx is downloaded next to
 * the running one, checked against its size and sha256, and then replaces it; the new build runs the next time the
 * game starts. For a private repository the token in sdmc:/3ds/dkr/bugreport.txt (the bug report one) is used; it
 * needs read access to the repository's contents.
 */
#include <3ds.h>
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_3ds.h"
#include "plat/plat.h"

#ifndef PORT_BUILD_VERSION
#define PORT_BUILD_VERSION "unknown"
#endif
#ifndef PORT_BUILD_COMMIT
#define PORT_BUILD_COMMIT ""
#endif

#define CONFIG_PATH "sdmc:/3ds/dkr/bugreport.txt"
#define DEFAULT_REPO "chrispelletier-gif/Diddy-Kong-Racing"
#define MAX_DOWNLOAD (16 * 1024 * 1024)

void plat_3ds_panel_print(int row, const char *text);
void plat_3ds_request_quit(void);
void bugreport_watch_thread(const char *name);
void bugreport_request(void);
long port_autorun_enabled(void);
void port_autorun_request(long start);
int prof_user_enabled(void);
void prof_set_user_enabled(int on);

/*
 * The panel's buttons also start the other bottom screen actions: in the idle state they are "Bug rep." (the same
 * as SELECT + touch), "Test run" (port_autorun.c, after a confirmation), "Profiler" (turns the profiler readout on
 * or off; green while on) and "Updates". Touches anywhere else on the screen do nothing.
 */
enum UpdateState {
    STATE_IDLE,     /* Bug report / Test run / Profiler / Updates. */
    STATE_CONFIRM,  /* Start or stop the test run / - / cancel. */
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
    char buildUrl[256];  /* Where to read build.txt and dkr.3dsx; for a private repository the API's asset URLs. */
    char binaryUrl[256];
} Channel;

typedef struct Config {
    char token[256];
    char repo[128];
} Config;

static char sSelfPath[256];
static volatile int sStarted;
static volatile int sState;
static volatile int sTappedButton = -1;
static LightEvent sTapEvent;
static Channel sChannels[2] = { { "3ds-master", "master" }, { "3ds-newest", "newest" } };
static int sChosen;

/* The panel: rows 1-3 are the buttons (screen rows 26-28, y 200-223). */
#define BUTTON_TOP 200
#define BUTTON_BOTTOM 224

static volatile int sButtonCount; // Buttons now shown, each a space then a block of (40 / count - 1) characters.

/*
 * Blue blocks three rows high; a label starting with '-' is a button that cannot be tapped, drawn grey, and one
 * starting with '+' is a switch that is on, drawn green.
 */
static void draw_buttons(const char *const *labels, int count) {
    int width = count == 1 ? 38 : 40 / count - 1, i, r;

    for (r = 0; r < 3; r++) {
        char row[160];
        int length = 0;
        for (i = 0; i < count; i++) {
            int enabled = labels[i][0] != '-', on = labels[i][0] == '+';
            const char *text = r == 1 ? labels[i] + (!enabled || on) : "";
            int left = (width - (int) strlen(text)) / 2;
            const char *colour = on ? "\x1b[30;42m" : enabled ? "\x1b[37;44m" : "\x1b[30;47m";
            length += snprintf(row + length, sizeof(row) - length, " %s%*s%-*s\x1b[0m", colour, left, "", width - left,
                               text);
        }
        plat_3ds_panel_print(r + 1, row);
    }
    sButtonCount = count;
}

static void draw_text_button(const char *label) {
    draw_buttons(&label, 1);
}

static int sTestRunShown = -1; // Whether the idle buttons say "Stop test" (1) or "Test run" (0).

static void draw_home(void) {
    const char *labels[4];
    sTestRunShown = port_autorun_enabled() != 0;
    labels[0] = "Bug rep.";
    labels[1] = sTestRunShown ? "Stop test" : "Test run";
    labels[2] = prof_user_enabled() ? "+Profiler" : "Profiler";
    labels[3] = "Updates";
    draw_buttons(labels, 4);
}

static void draw_confirm(void) {
    const char *labels[3];
    labels[0] = sTestRunShown ? "Stop test" : "Start test";
    labels[1] = sTestRunShown ? "-" : "AA bench";
    labels[2] = "cancel";
    draw_buttons(labels, 3);
    if (sTestRunShown) {
        plat_status("Stop the test run? The results so far are sent as a report.");
    } else {
        plat_status("Test run: the AI races every track with every character (some 8 hours), recording a profile");
        plat_status("of each race and any crash. Saves aren't written meanwhile. It starts from the title screen.");
        plat_status("AA bench: the heaviest races, 4 times each with anti-aliasing off and on (half an hour).");
    }
}

static void show_busy(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void show_busy(const char *fmt, ...) {
    char text[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    plat_3ds_panel_print(1, "");
    plat_3ds_panel_print(2, "");
    plat_3ds_panel_print(3, "");
    {
        char line[80];
        snprintf(line, sizeof(line), "  \x1b[33;1m%s\x1b[0m", text);
        plat_3ds_panel_print(2, line);
    }
}

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

/* GETs url into download. Returns the HTTP status, or 0 with the error in error. */
static long http_get(const Config *config, const char *url, int binary, Download *download, char *error,
                     size_t errorSize) {
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    char auth[300], curlError[CURL_ERROR_SIZE];
    long status = 0;
    CURLcode code;
    const char *caFile = net_3ds_ca_file();

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
    curl_easy_cleanup(curl);
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
    } else {
        snprintf(channel->buildUrl, sizeof(channel->buildUrl), "https://github.com/%s/releases/download/%s/build.txt",
                 config->repo, channel->tag);
        snprintf(channel->binaryUrl, sizeof(channel->binaryUrl),
                 "https://github.com/%s/releases/download/%s/dkr.3dsx", config->repo, channel->tag);
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
    if (channel->commit[0] == '\0' || channel->sha256[0] == '\0' || channel->size == 0) {
        snprintf(error, errorSize, "%s/build.txt is incomplete", channel->tag);
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

    if (sSelfPath[0] == '\0') {
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
    net_3ds_end();
    if (!found) {
        plat_status("Update: no builds found%s", config.token[0] == '\0' ? " (a private repo needs the token)" : "");
        return 0;
    }

    plat_status("This build: %.7s", PORT_BUILD_COMMIT[0] != '\0' ? PORT_BUILD_COMMIT : PORT_BUILD_VERSION);
    for (i = 0; i < 2; i++) {
        const Channel *channel = &sChannels[i];
        if (channel->found) {
            plat_status("%-6s %.7s %.16s %.10s%s", channel->label, channel->commit, channel->branch, channel->date,
                        is_installed(channel) ? " (this)" : "");
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

static int install(Channel *channel) {
    Config config;
    Download download;
    char error[200], newPath[272], oldPath[272], digest[65];
    unsigned char hash[32];
    long status;
    int i;

    load_config(&config);
    snprintf(newPath, sizeof(newPath), "%s.new", sSelfPath);
    snprintf(oldPath, sizeof(oldPath), "%s.old", sSelfPath);
    memset(&download, 0, sizeof(download));
    download.expected = channel->size;
    download.lastPercent = -5;
    download.file = fopen(newPath, "wb");
    if (download.file == NULL) {
        plat_status("Update: cannot write %s", newPath);
        return 0;
    }
    setvbuf(download.file, NULL, _IOFBF, 64 * 1024);
    mbedtls_sha256_init(&download.sha);
    mbedtls_sha256_starts_ret(&download.sha, 0);

    show_busy("Downloading %s...", channel->label);
    error[0] = '\0';
    if (net_3ds_begin(error, sizeof(error)) != 0) {
        status = 0;
    } else {
        status = http_get(&config, channel->binaryUrl, 1, &download, error, sizeof(error));
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
    if (status == 200 && download.written != channel->size) {
        snprintf(error, sizeof(error), "got %lu of %lu bytes", download.written, channel->size);
        status = 0;
    } else if (status == 200 && strcmp(digest, channel->sha256) != 0) {
        snprintf(error, sizeof(error), "the download is damaged (sha256 differs)");
        status = 0;
    } else if (status != 200 && status != 0) {
        snprintf(error, sizeof(error), "GitHub said %ld", status);
    }
    if (status != 200) {
        remove(newPath);
        plat_status("Update failed: %s", error);
        return 0;
    }

    // The running build was read into memory at start, so its file can be replaced. The old one is kept until the
    // new one is in place.
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

/* The touch screen, from the game's controller polling (input_3ds.c). Returns nonzero if the touch was a button. */
int update_3ds_touch(int x, int y) {
    int button, count = sButtonCount, cell = count > 1 ? 40 / count : 40;
    if (!sStarted || y < BUTTON_TOP || y >= BUTTON_BOTTOM) {
        return 0;
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

static void update_thread(void *arg) {
    (void) arg;
    bugreport_watch_thread("update");
    for (;;) {
        int button;
        // Every two seconds without a tap, the test run button is brought up to date (a run ends by itself).
        if (LightEvent_WaitTimeout(&sTapEvent, 2000000000LL) != 0) {
            if (sState == STATE_IDLE && sTestRunShown != (port_autorun_enabled() != 0)) {
                draw_home();
            }
            continue;
        }
        button = sTappedButton;
        switch (sState) {
            case STATE_IDLE:
                if (button == 0) {
                    bugreport_request();
                } else if (button == 1) {
                    sState = STATE_CONFIRM;
                    draw_confirm();
                } else if (button == 2) {
                    prof_set_user_enabled(!prof_user_enabled());
                    draw_home();
                } else {
                    sState = STATE_BUSY;
                    if (check()) {
                        sState = STATE_CHOOSE;
                        draw_choices();
                    } else {
                        sState = STATE_IDLE;
                        draw_home();
                    }
                }
                break;
            case STATE_CONFIRM:
                if (button == 0 || (button == 1 && !sTestRunShown)) {
                    // 2 is the anti-aliasing benchmark. The run turns on or off at the game's next frame.
                    port_autorun_request(!sTestRunShown ? (button == 1 ? 2 : 1) : 0);
                    svcSleepThread(200 * 1000 * 1000);
                }
                if (button != 1 || !sTestRunShown) {
                    sState = STATE_IDLE;
                    draw_home();
                }
                break;
            case STATE_CHOOSE:
                if (button == 2) {
                    sState = STATE_IDLE;
                    draw_home();
                } else if (sChannels[button].found) {
                    sState = STATE_BUSY;
                    sChosen = button;
                    if (install(&sChannels[button])) {
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
                plat_3ds_request_quit();
                break;
        }
    }
}

/* selfPath: the running dkr.3dsx (argv[0] from the Homebrew Launcher), or NULL if not known. */
void update_3ds_start(const char *selfPath) {
    // Only a file on the SD card can be replaced (3dslink, for one, sends the program over the network).
    if (selfPath != NULL && strncmp(selfPath, "sdmc:/", 6) == 0) {
        snprintf(sSelfPath, sizeof(sSelfPath), "%s", selfPath);
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
