#ifndef PORT_PLAT_SETUP_SCREEN_H
#define PORT_PLAT_SETUP_SCREEN_H

/*
 * The first-run setup (port/src/game/port_setup.c): the first time the game reaches its title screen it asks the
 * player's language, then how they want to play, as a few tiles on the 3DS bottom screen (bottom_screen.c draws
 * them, bs_set_setup). The game thread fills this in, already in the player's language, so the drawing side needs
 * none of the game's headers; taps come back through bs_setup_hit.
 */

/* What a page asks; each has its own pictures on the tiles. */
enum {
    SETUP_PAGE_LANGUAGE, /* English or French. */
    SETUP_PAGE_STYLE,    /* The play styles: Classic, Enhanced, Speedy. */
    SETUP_PAGE_PICTURE,  /* 30 fps with anti-aliasing, or 60 fps. */
    SETUP_PAGE_SCREEN,   /* Widescreen: 4:3, Wide, Balanced, Zoomed. */
    SETUP_PAGE_CAMERA,   /* Modern or Original. */
    SETUP_PAGE_HUD,      /* The race HUD: bottom screen, top screen, both. */
    SETUP_PAGE_LOOK      /* How the bottom screen looks: Classic, Glass, Broadcast, Dock. */
};

/* What each tile shows on the play style page (SetupScreen.styles). */
enum { SETUP_STYLE_CLASSIC, SETUP_STYLE_ENHANCED, SETUP_STYLE_SPEEDY };

#define SETUP_TILES 4
#define SETUP_STEPS 5
#define SETUP_STYLE_LINES 4

/* bs_setup_hit: a tile's index, or one of these for the three buttons along the bottom. */
enum { SETUP_HIT_NONE = -1, SETUP_HIT_BACK = 100, SETUP_HIT_MIDDLE, SETUP_HIT_NEXT };

typedef struct SetupTile {
    int art;           /* The picture: what the tile stands for (the page's choice: language, style, value). */
    char name[24];     /* In the game's fun font: capitals, no accents. */
    char lines[SETUP_STYLE_LINES][28]; /* Under the name: one or two lines; a style's list (SETUP_STYLE_LINES). */
    int lineCount;
    char tag[24];      /* "RECOMMENDED" and the like; may be empty. */
} SetupTile;

typedef struct SetupScreen {
    int page;
    char question[40];
    int tileCount;
    SetupTile tiles[SETUP_TILES];
    int chosen; /* The tile lit. */
    /* The steps of the fine tuning, as dots with names; stepCount 0 on the first pages. */
    int stepCount, step;
    char steps[SETUP_STEPS][16];
    char buttons[3][20]; /* Back, the middle one and Next; an empty label: no button. */
    char help[64];       /* A line of controls over the buttons. */
} SetupScreen;

#endif
