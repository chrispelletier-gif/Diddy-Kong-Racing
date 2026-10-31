/* Bug reports as GitHub issues (see bugreport.h). Platform independent apart from the caller's network setup. */
#include "bugreport.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* GitHub refuses bodies over 65536 characters; this leaves room for the wrapper text. */
#define BASE64_PER_COMMENT 56000
#define BASE64_LINE 76

typedef struct StrBuf {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} StrBuf;

static void sb_reserve(StrBuf *sb, size_t extra) {
    size_t need = sb->length + extra + 1;
    char *data;
    if (sb->failed || need <= sb->capacity) {
        return;
    }
    if (need < sb->capacity * 2) {
        need = sb->capacity * 2;
    }
    data = realloc(sb->data, need);
    if (data == NULL) {
        sb->failed = 1;
        return;
    }
    sb->data = data;
    sb->capacity = need;
}

static void sb_add_n(StrBuf *sb, const char *text, size_t n) {
    sb_reserve(sb, n);
    if (sb->failed) {
        return;
    }
    memcpy(sb->data + sb->length, text, n);
    sb->length += n;
    sb->data[sb->length] = '\0';
}

static void sb_add(StrBuf *sb, const char *text) {
    sb_add_n(sb, text, strlen(text));
}

/* Appends text as the inside of a JSON string. */
static void sb_add_json(StrBuf *sb, const char *text) {
    const unsigned char *p;
    for (p = (const unsigned char *) text; *p; p++) {
        char escaped[8];
        switch (*p) {
            case '"':
                sb_add(sb, "\\\"");
                break;
            case '\\':
                sb_add(sb, "\\\\");
                break;
            case '\n':
                sb_add(sb, "\\n");
                break;
            case '\r':
                sb_add(sb, "\\r");
                break;
            case '\t':
                sb_add(sb, "\\t");
                break;
            default:
                if (*p < 0x20) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                    sb_add(sb, escaped);
                } else {
                    sb_add_n(sb, (const char *) p, 1);
                }
                break;
        }
    }
}

static void sb_free(StrBuf *sb) {
    free(sb->data);
    memset(sb, 0, sizeof(*sb));
}

static size_t write_response(char *data, size_t size, size_t count, void *user) {
    StrBuf *sb = (StrBuf *) user;
    // Only the start of a response is looked at; the rest is dropped to save memory.
    if (sb->length < 16384) {
        sb_add_n(sb, data, size * count);
    }
    return size * count;
}

/* POSTs JSON to apiUrl + path. Returns the HTTP status (0 if the request failed), with the response in response. */
static long post_json(const BugReportConfig *config, const char *path, const char *json, StrBuf *response,
                      char *error, size_t errorSize) {
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    char url[512], auth[512], curlError[CURL_ERROR_SIZE];
    long status = 0;
    CURLcode code;

    if (curl == NULL) {
        snprintf(error, errorSize, "out of memory (curl)");
        return 0;
    }
    snprintf(url, sizeof(url), "%s%s", config->apiUrl, path);
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", config->token);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
    curlError[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, config->userAgent != NULL ? config->userAgent : "dkr-bugreport");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) strlen(json));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curlError);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (config->caFile != NULL) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, config->caFile);
    } else if (config->caPem != NULL) {
        struct curl_blob blob;
        blob.data = (void *) config->caPem;
        blob.len = strlen(config->caPem);
        blob.flags = CURL_BLOB_COPY;
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    }
    code = curl_easy_perform(curl);
    if (code != CURLE_OK) {
        snprintf(error, errorSize, "%s", curlError[0] != '\0' ? curlError : curl_easy_strerror(code));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status;
}

/* Copies the string value of "key" in a JSON response (the first one, which for an issue is the issue's own). */
static int json_find_string(const char *json, const char *key, char *out, size_t outSize) {
    char pattern[64];
    const char *p, *end;
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = (json != NULL) ? strstr(json, pattern) : NULL;
    if (p == NULL) {
        return 0;
    }
    p = strchr(p + strlen(pattern), '"');
    if (p == NULL) {
        return 0;
    }
    p++;
    end = strchr(p, '"');
    if (end == NULL || (size_t) (end - p) >= outSize) {
        return 0;
    }
    memcpy(out, p, end - p);
    out[end - p] = '\0';
    return 1;
}

static void describe_failure(long status, const StrBuf *response, const char *curlError, char *result,
                             size_t resultSize) {
    char message[160];
    if (status == 0) {
        snprintf(result, resultSize, "could not reach GitHub: %s", curlError);
    } else if (json_find_string(response->data, "message", message, sizeof(message))) {
        snprintf(result, resultSize, "GitHub said %ld: %s", status, message);
    } else {
        snprintf(result, resultSize, "GitHub said %ld", status);
    }
}

/* gzip compresses data. Returns a malloc'd buffer, or NULL. */
static unsigned char *gzip(const unsigned char *data, unsigned int size, unsigned int *outSize) {
    z_stream stream;
    uLong bound;
    unsigned char *out;

    memset(&stream, 0, sizeof(stream));
    if (deflateInit2(&stream, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return NULL;
    }
    bound = deflateBound(&stream, size) + 32;
    out = malloc(bound);
    if (out == NULL) {
        deflateEnd(&stream);
        return NULL;
    }
    stream.next_in = (Bytef *) data;
    stream.avail_in = size;
    stream.next_out = out;
    stream.avail_out = bound;
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&stream);
        free(out);
        return NULL;
    }
    *outSize = stream.total_out;
    deflateEnd(&stream);
    return out;
}

static char *base64(const unsigned char *data, unsigned int size, size_t *outLength) {
    static const char sDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t length = ((size + 2) / 3) * 4;
    char *out = malloc(length + 1);
    unsigned int i;
    char *p = out;

    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i + 2 < size; i += 3) {
        unsigned int v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        *p++ = sDigits[v >> 18];
        *p++ = sDigits[(v >> 12) & 63];
        *p++ = sDigits[(v >> 6) & 63];
        *p++ = sDigits[v & 63];
    }
    if (i < size) {
        unsigned int v = data[i] << 16;
        if (i + 1 < size) {
            v |= data[i + 1] << 8;
        }
        *p++ = sDigits[v >> 18];
        *p++ = sDigits[(v >> 12) & 63];
        *p++ = (i + 1 < size) ? sDigits[(v >> 6) & 63] : '=';
        *p++ = '=';
    }
    *p = '\0';
    *outLength = length;
    return out;
}

static long post_comment(const BugReportConfig *config, const char *commentsPath, const char *text, char *result,
                         size_t resultSize) {
    StrBuf json = { 0 }, response = { 0 };
    char curlError[CURL_ERROR_SIZE];
    long status;

    sb_add(&json, "{\"body\":\"");
    sb_add_json(&json, text);
    sb_add(&json, "\"}");
    if (json.failed) {
        sb_free(&json);
        snprintf(result, resultSize, "out of memory");
        return 0;
    }
    curlError[0] = '\0';
    status = post_json(config, commentsPath, json.data, &response, curlError, sizeof(curlError));
    if (status != 201) {
        describe_failure(status, &response, curlError, result, resultSize);
    }
    sb_free(&json);
    sb_free(&response);
    return status;
}

/* Posts one file as comments. Returns 0 on success, 1 if it was too big, -1 if posting failed (result says why). */
static int post_file(const BugReportConfig *config, const char *commentsPath, const BugReportFile *file,
                     unsigned int *budget, BugReportProgress progress, char *result, size_t resultSize) {
    unsigned int packedSize = 0;
    unsigned char *packed = gzip(file->data, file->size, &packedSize);
    size_t textLength = 0, offset;
    char *text;
    int parts, part;

    if (packed == NULL) {
        return 1;
    }
    if (packedSize > *budget) {
        free(packed);
        return 1;
    }
    *budget -= packedSize;
    text = base64(packed, packedSize, &textLength);
    free(packed);
    if (text == NULL) {
        return 1;
    }
    parts = (int) ((textLength + BASE64_PER_COMMENT - 1) / BASE64_PER_COMMENT);
    if (parts == 0) {
        parts = 1;
    }
    for (part = 0, offset = 0; part < parts; part++) {
        StrBuf comment = { 0 };
        char line[256];
        size_t end = offset + BASE64_PER_COMMENT;
        if (end > textLength) {
            end = textLength;
        }
        if (progress != NULL) {
            snprintf(line, sizeof(line), "Sending %s (%d of %d)", file->name, part + 1, parts);
            progress(line);
        }
        snprintf(line, sizeof(line),
                 "<details><summary>Attachment %s, part %d of %d</summary>\n\n"
                 "<!-- dkr-attachment name=%s part=%d parts=%d size=%u encoding=gzip+base64 -->\n\n```\n",
                 file->name, part + 1, parts, file->name, part + 1, parts, file->size);
        sb_add(&comment, line);
        for (; offset < end; offset += BASE64_LINE) {
            size_t n = (end - offset < BASE64_LINE) ? end - offset : BASE64_LINE;
            sb_add_n(&comment, text + offset, n);
            sb_add(&comment, "\n");
        }
        offset = end;
        sb_add(&comment, "```\n\n</details>\n");
        if (comment.failed || post_comment(config, commentsPath, comment.data, result, resultSize) != 201) {
            if (comment.failed) {
                snprintf(result, resultSize, "out of memory");
            }
            sb_free(&comment);
            free(text);
            return -1;
        }
        sb_free(&comment);
    }
    free(text);
    return 0;
}

int bugreport_send(const BugReportConfig *config, const char *title, const char *body, const BugReportFile *files,
                   int fileCount, unsigned int maxAttachmentBytes, BugReportProgress progress, char *result,
                   size_t resultSize) {
    StrBuf json = { 0 }, response = { 0 }, skipped = { 0 };
    char path[256], number[32], url[256], curlError[CURL_ERROR_SIZE];
    unsigned int budget = maxAttachmentBytes;
    long status;
    int i;

    if (progress != NULL) {
        progress("Creating the issue");
    }
    sb_add(&json, "{\"title\":\"");
    sb_add_json(&json, title);
    sb_add(&json, "\",\"body\":\"");
    sb_add_json(&json, body);
    sb_add(&json, "\",\"labels\":[\"");
    sb_add_json(&json, config->label);
    sb_add(&json, "\"]}");
    if (json.failed) {
        sb_free(&json);
        snprintf(result, resultSize, "out of memory");
        return -1;
    }
    snprintf(path, sizeof(path), "/repos/%s/issues", config->repo);
    curlError[0] = '\0';
    status = post_json(config, path, json.data, &response, curlError, sizeof(curlError));
    sb_free(&json);
    if (status != 201) {
        describe_failure(status, &response, curlError, result, resultSize);
        sb_free(&response);
        return -1;
    }
    // "number" is not a string; take the issue number from the end of its web address instead.
    if (!json_find_string(response.data, "html_url", url, sizeof(url)) || strrchr(url, '/') == NULL) {
        sb_free(&response);
        snprintf(result, resultSize, "GitHub's answer had no issue address");
        return -1;
    }
    sb_free(&response);
    snprintf(number, sizeof(number), "%s", strrchr(url, '/') + 1);
    snprintf(path, sizeof(path), "/repos/%s/issues/%s/comments", config->repo, number);

    for (i = 0; i < fileCount; i++) {
        int outcome;
        if (files[i].data == NULL) {
            continue;
        }
        outcome = post_file(config, path, &files[i], &budget, progress, result, resultSize);
        if (outcome < 0) {
            // The issue exists; say where, and what did not make it.
            char reason[256];
            snprintf(reason, sizeof(reason), "%s", result);
            snprintf(result, resultSize, "%s (sending %s: %s)", url, files[i].name, reason);
            sb_free(&skipped);
            return 1;
        }
        if (outcome > 0) {
            char line[160];
            snprintf(line, sizeof(line), "- %s (%u bytes)\n", files[i].name, files[i].size);
            sb_add(&skipped, line);
        }
    }
    if (skipped.length > 0) {
        StrBuf text = { 0 };
        sb_add(&text, "These files were too big to attach and are still on the SD card in `sdmc:/3ds/dkr/`:\n\n");
        sb_add(&text, skipped.data);
        if (!text.failed) {
            post_comment(config, path, text.data, curlError, sizeof(curlError));
        }
        sb_free(&text);
    }
    sb_free(&skipped);
    snprintf(result, resultSize, "%s", url);
    return 0;
}
