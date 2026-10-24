/*
 * Scripted input for automated testing, shared by every platform. DKR_INPUT (see plat_getenv) is a list of
 * "poll:BUTTONS" steps separated by spaces, e.g. "200:START 260: 300:A 330:". BUTTONS is a '+' separated list of
 * A B Z START L R CU CD CL CR DU DD DL DR, LOOK (the look-back camera) or STICK=x,y. Each step lasts until the next
 * one.
 */
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name;
    unsigned short bit;
} sButtonNames[] = {
    { "A", 0x8000 },  { "B", 0x4000 },  { "Z", 0x2000 },  { "START", 0x1000 }, { "DU", 0x0800 },
    { "DD", 0x0400 }, { "DL", 0x0200 }, { "DR", 0x0100 }, { "L", 0x0020 },     { "R", 0x0010 },
    { "CU", 0x0008 }, { "CD", 0x0004 }, { "CL", 0x0002 }, { "CR", 0x0001 },    { "LOOK", 0x0040 },
};

int plat_input_script(int poll, unsigned short *buttons, signed char *stickX, signed char *stickY) {
    const char *script = plat_getenv("DKR_INPUT");
    const char *p;
    char token[64];
    int bestPoll = -1;
    char best[64] = "";
    char *tok;

    if (script == NULL) {
        return 0;
    }
    for (p = script; *p;) {
        int n = 0, stepPoll;
        char *colon;
        while (*p == ' ') {
            p++;
        }
        while (*p && *p != ' ' && n < 63) {
            token[n++] = *p++;
        }
        token[n] = 0;
        colon = strchr(token, ':');
        if (colon == NULL) {
            continue;
        }
        *colon = 0;
        stepPoll = atoi(token);
        if (stepPoll <= poll && stepPoll > bestPoll) {
            bestPoll = stepPoll;
            strcpy(best, colon + 1);
        }
    }
    for (tok = strtok(best, "+"); tok != NULL; tok = strtok(NULL, "+")) {
        unsigned i;
        int x, y;
        if (sscanf(tok, "STICK=%d,%d", &x, &y) == 2) {
            *stickX = (signed char) x;
            *stickY = (signed char) y;
        }
        for (i = 0; i < sizeof(sButtonNames) / sizeof(sButtonNames[0]); i++) {
            if (strcmp(tok, sButtonNames[i].name) == 0) {
                *buttons |= sButtonNames[i].bit;
            }
        }
    }
    return 1;
}
