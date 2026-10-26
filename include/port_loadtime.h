#ifndef PORT_LOADTIME_H
#define PORT_LOADTIME_H
/*
 * Level load timing marks for the game's own code (port/src/game/port_loadtime.c). Each names the step that just
 * finished. On the N64 they do nothing.
 */
#ifdef TARGET_PORT
#include "port.h"
#define PORT_LOAD_BEGIN() port_load_begin()
#define PORT_LOAD_BG_QUEUED() port_load_bg_queued()
#define PORT_LOAD_STEP(name) port_load_step(name)
#define PORT_LOAD_END(levelId, kind) port_load_end(levelId, kind)
#define PORT_LOAD_PICK(step) port_load_pick(step)
#define PORT_LOAD_MENU_HELD(held) port_load_menu_held(held)
#else
#define PORT_LOAD_BEGIN()
#define PORT_LOAD_BG_QUEUED()
#define PORT_LOAD_STEP(name)
#define PORT_LOAD_END(levelId, kind)
#define PORT_LOAD_PICK(step)
#define PORT_LOAD_MENU_HELD(held)
#endif
#endif
