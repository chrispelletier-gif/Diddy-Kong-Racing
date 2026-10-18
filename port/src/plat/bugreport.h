#ifndef PORT_BUGREPORT_H
#define PORT_BUGREPORT_H

/*
 * Sends a bug report as a GitHub issue. The issue body is the readable part (what happened, the recent log); each
 * file is gzip compressed, base64 encoded and posted as one or more comments on the issue, which
 * port/tools/bugreport_fetch.py turns back into files. Uses libcurl; the caller sets up networking first.
 */

#include <stddef.h>

typedef struct BugReportFile {
    const char *name;
    const unsigned char *data;
    unsigned int size;
} BugReportFile;

typedef struct BugReportConfig {
    const char *token;  /* GitHub token that may create issues on the repository. */
    const char *repo;   /* "owner/name". */
    const char *apiUrl; /* "https://api.github.com", or a test server. */
    const char *label;  /* Label put on the issue. */
    const char *caPem;  /* Root certificates (PEM), or NULL for libcurl's default. */
    const char *caFile; /* A certificate file to use instead of caPem, or NULL. */
    const char *userAgent;
} BugReportConfig;

/* Shown while the report is being sent ("Creating the issue", "Sending log.txt (2 of 3)"). */
typedef void (*BugReportProgress)(const char *text);

/*
 * Creates the issue and posts the files. Returns 0 on success, with the issue's web address in result; otherwise
 * nonzero, with what went wrong in result. Files that do not fit in maxAttachmentBytes (compressed) are left out and
 * listed in a final comment.
 */
int bugreport_send(const BugReportConfig *config, const char *title, const char *body, const BugReportFile *files,
                   int fileCount, unsigned int maxAttachmentBytes, BugReportProgress progress, char *result,
                   size_t resultSize);

#endif
