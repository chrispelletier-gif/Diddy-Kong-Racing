#ifndef PORT_OBJECT_ENTRIES_H
#define PORT_OBJECT_ENTRIES_H

#include <PR/ultratypes.h>

typedef struct ObjectEntryLayout {
    u8 size;
    u8 num16;
    u8 num32;
    u8 offsets16[16];
    u8 offsets32[8];
} ObjectEntryLayout;

extern const ObjectEntryLayout gObjectEntryLayouts[128];

#endif
