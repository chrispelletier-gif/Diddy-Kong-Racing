/* The categories of settings the game's menus show (options.h). */
#include <string.h>

#include "options.h"
#include "plat.h"

#define MAX_SECTIONS 6

typedef struct Category {
    OptionsSection sections[MAX_SECTIONS];
    int count;
} Category;

static Category sCategories[OPTIONS_CATEGORY_COUNT];
static volatile unsigned int sChanges;

void options_add(int category, const OptionsSection *section) {
    Category *c;
    if (category < 0 || category >= OPTIONS_CATEGORY_COUNT) {
        return;
    }
    c = &sCategories[category];
    if (c->count >= MAX_SECTIONS) {
        plat_log("options: no room in category %d", category);
        return;
    }
    c->sections[c->count++] = *section;
}

/* Finds the section and its own row number of row. */
static const OptionsSection *find(int category, int inRace, int row, int *index) {
    const Category *c;
    int s, i;
    if (category < 0 || category >= OPTIONS_CATEGORY_COUNT || row < 0) {
        return NULL;
    }
    c = &sCategories[category];
    for (s = 0; s < c->count; s++) {
        const OptionsSection *section = &c->sections[s];
        int count = section->count();
        for (i = 0; i < count; i++) {
            if (inRace && section->in_race != NULL && !section->in_race(i)) {
                continue;
            }
            if (row-- == 0) {
                *index = i;
                return section;
            }
        }
    }
    return NULL;
}

int options_count(int category, int inRace) {
    const Category *c;
    int s, i, total = 0;
    if (category < 0 || category >= OPTIONS_CATEGORY_COUNT) {
        return 0;
    }
    c = &sCategories[category];
    for (s = 0; s < c->count; s++) {
        const OptionsSection *section = &c->sections[s];
        int count = section->count();
        for (i = 0; i < count; i++) {
            if (!inRace || section->in_race == NULL || section->in_race(i)) {
                total++;
            }
        }
    }
    return total;
}

const char *options_label(int category, int inRace, int row) {
    int i;
    const OptionsSection *section = find(category, inRace, row, &i);
    return section != NULL ? section->label(i) : "";
}

void options_value(int category, int inRace, int row, char *out, int size) {
    int i;
    const OptionsSection *section = find(category, inRace, row, &i);
    if (section != NULL) {
        section->value(i, out, size);
    } else if (size > 0) {
        out[0] = '\0';
    }
}

const char *options_hint(int category, int inRace, int row) {
    int i;
    const OptionsSection *section = find(category, inRace, row, &i);
    return section != NULL && section->hint != NULL ? section->hint(i) : NULL;
}

void options_change(int category, int inRace, int row, int step) {
    int i;
    const OptionsSection *section = find(category, inRace, row, &i);
    if (section != NULL) {
        section->change(i, step < 0 ? -1 : 1);
        sChanges++;
    }
}

unsigned int options_change_count(void) {
    return sChanges;
}
