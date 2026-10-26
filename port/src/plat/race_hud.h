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

/* What is being played: each has its own layout on the bottom screen (RaceHud.mode). */
enum {
    RACE_HUD_MODE_RACE,       /* A race against the field (silver coin races too: RaceHud.silverCoins). */
    RACE_HUD_MODE_TIME_TRIAL, /* Alone against the clock and the ghosts. */
    RACE_HUD_MODE_BOSS,       /* Against a boss, head to head. */
    RACE_HUD_MODE_TAJ,        /* Taj's challenge on the hub. */
    RACE_HUD_MODE_BANANAS,    /* The banana challenge: bring 10 home. */
    RACE_HUD_MODE_BATTLE,     /* The battle arena: the last one with bananas wins. */
    RACE_HUD_MODE_EGGS,       /* The egg collector: 3 eggs in the nest. */
    RACE_HUD_MODE_HUB,        /* Driving round a hub: the adventure's progress. */
    /* Not racing: the menus' companions (the buttons stay the menu's). */
    RACE_HUD_MODE_MENU_SAVE, /* The file select menu: the highlighted adventure's progress (the hub's fields). */
    RACE_HUD_MODE_MENU_TRACK /* The tracks menu: the track's map and records. */
};
#define RACE_HUD_MODE_FIRST_MENU RACE_HUD_MODE_MENU_SAVE

#define RACE_HUD_WORLDS 5 /* Dino Domain to Future Fun Land. */
#define RACE_HUD_DOORS 12 /* Doors marked on a hub's map. */

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

/*
 * Where something is on the track's map: pixels of the map sprite (y down), times 4, relative to the sprite's origin
 * (the bottom screen takes the texture's offset in the sprite off), and which way it faces (0x10000 a turn,
 * counter-clockwise; 0 up). onMap 0: not shown.
 */
typedef struct RaceHudPoint {
    signed char onMap;
    short mapX, mapY;
    unsigned short heading;
} RaceHudPoint;

typedef struct RaceHudRacer {
    signed char character; /* The game's Character (enums.h). */
    signed char special;   /* A boss or Taj (drawn instead of character), or RACE_HUD_NOT_SPECIAL. */
    signed char player;    /* The player's own kart. */
    RaceHudPoint at;
    int gap; /* 60ths of a second this racer is ahead of the player (negative: behind), or NO_GAP. */
    /* The challenges: bananas home or eggs in the nest (battle: bananas left), and whether one is being carried. */
    signed char score, carrying;
    signed char out; /* Knocked out of a battle. */
} RaceHudRacer;

typedef struct RaceHud {
    int mode;        /* RACE_HUD_MODE_*. */
    int mapSprite;   /* The track map's sprite (the ROM's index), or -1. */
    int mapMirrored; /* Adventure 2's mirrored tracks: the map is drawn flipped. */
    unsigned char mapColour[3];
    int racerCount;
    RaceHudRacer racers[RACE_HUD_RACERS]; /* In race order: racers[0] leads. */
    int position;                         /* The player's, from 1. */
    int lap, laps;                        /* lap from 1; laps 0 for a race without laps. */
    int raceTime;                         /* 60ths of a second. */
    int lapTimes[RACE_HUD_LAPS];          /* Each finished lap's time, then the current lap's so far. */
    int lapsTimed;                        /* How many of lapTimes are set (finished laps + the current one). */
    int item, itemLevel, itemCount;       /* item -1: none (the game's balloon_type, balloon_level, quantity). */
    int bananas;
    int countdown; /* Before the start. */
    int finished;  /* The player has crossed the line. */

    int silverCoins; /* A silver coin race: the coins picked up so far; -1 any other race. */
    int round;       /* A boss race: 1 the first, 2 the rematch. */
    int vehicle;     /* The player's vehicle (0 car, 1 hovercraft, 2 plane): Taj's challenge is named by it. */
    int goal;        /* The challenges: the score that wins (10 bananas, 3 eggs; battle 0). */
    int racersLeft;  /* Battle: those not knocked out. */

    /* Time trial. Times in 60ths of a second, -1 when there is none. */
    int record, recordLap; /* The course's best time and best lap with this vehicle. */
    int staffTime;         /* T.T.'s time, when his ghost is out on the track. */
    int bestGap;           /* Ahead of (negative) or behind the best run so far, at the last checkpoint; or NO_GAP. */
    int bestLaps[RACE_HUD_LAPS], bestLapCount, bestTotal; /* That run's laps. */
    RaceHudPoint ghost, staffGhost;

    /* A hub. */
    int world; /* The hub's world: 0 the central area, then Dino Domain to Future Fun Land. */
    int balloons;
    int worldBalloons[RACE_HUD_WORLDS];
    int worldTrophy[RACE_HUD_WORLDS]; /* 0 none, 1 third, 2 second, 3 first place. */
    int worldKeys;                    /* Bit n: world n+1's key. */
    int worldBosses;                  /* Bit n: world n+1's boss beaten (once). */
    int ttAmulet, wizpigAmulet;       /* Pieces, 0 to 4. */
    int doorCount;
    RaceHudPoint doors[RACE_HUD_DOORS], taj;

    /* The menus. */
    int level;     /* The tracks menu's track (the ROM's level id): the bottom screen reads its map from the ROM. */
    char name[32]; /* The track's name, or the save file's. */
    int adventureTwo;
} RaceHud;

#endif
