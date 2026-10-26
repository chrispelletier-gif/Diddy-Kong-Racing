#ifndef PORT_PLAT_RACE_HUD_H
#define PORT_PLAT_RACE_HUD_H

/*
 * The race HUD on the 3DS bottom screen (roadmap G9): what the game thread knows about the race, copied once a frame
 * (port/src/game/port_race_hud.c) for the bottom screen to draw (bottom_screen.c, bs_set_race_hud). Plain numbers
 * only, so the drawing side needs none of the game's headers.
 */

#define RACE_HUD_RACERS 8
#define RACE_HUD_LAPS 5
#define RACE_HUD_NO_GAP 0x7FFFFFFF
/* A gap of a whole lap or more is sent as this plus the number of laps. */
#define RACE_HUD_LAPPED 0x40000000

/* Racers the ROM has no portrait for: the bosses and Taj (RaceHudRacer.special). */
enum {
    RACE_HUD_NOT_SPECIAL = -1,
    RACE_HUD_TRICKY,
    RACE_HUD_BLUEY,
    RACE_HUD_BUBBLER,
    RACE_HUD_SMOKEY,
    RACE_HUD_WIZPIG,
    RACE_HUD_TAJ,
    RACE_HUD_SPECIALS
};

typedef struct RaceHudRacer {
    signed char character; /* The game's Character (enums.h). */
    signed char special;   /* A boss or Taj (drawn instead of character), or RACE_HUD_NOT_SPECIAL. */
    signed char player;    /* The player's own kart. */
    /*
     * Where the racer is on the track's map: pixels of the map sprite (y down), times 4, relative to the sprite's
     * origin (the bottom screen takes the texture's offset in the sprite off). onMap 0: not shown.
     */
    signed char onMap;
    short mapX, mapY;
    unsigned short heading; /* The arrow's angle on the map (0x10000 a turn, counter-clockwise; 0 up). */
    int gap;                /* 60ths of a second this racer is ahead of the player (negative: behind), or NO_GAP. */
} RaceHudRacer;

typedef struct RaceHud {
    int mapSprite;   /* The track map's sprite (the ROM's index), or -1. */
    int mapMirrored; /* Adventure 2's mirrored tracks: the map is drawn flipped. */
    unsigned char mapColour[3];
    int racerCount;
    RaceHudRacer racers[RACE_HUD_RACERS]; /* In race order: racers[0] leads. */
    int position;                        /* The player's, from 1. */
    int lap, laps;                       /* lap from 1; laps 0 for a race without laps. */
    int raceTime;                        /* 60ths of a second. */
    int lapTimes[RACE_HUD_LAPS];         /* Each finished lap's time, then the current lap's so far. */
    int lapsTimed;                       /* How many of lapTimes are set (finished laps + the current one). */
    int item, itemLevel, itemCount;      /* item -1: none (the game's balloon_type, balloon_level, quantity). */
    int bananas;
    int countdown; /* Before the start. */
    int finished;  /* The player has crossed the line. */
} RaceHud;

#endif
