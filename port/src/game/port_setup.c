/*
 * The first-run setup (port.h): the first time the game reaches its title screen, before START and OPTIONS appear, it
 * asks the player's language, then how they want to play: one of three play styles (Classic, like the N64; Enhanced,
 * the recommended one; Speedy, 60 fps on a New 3DS), or FINE TUNE, a page each for the picture, the screen shape,
 * the camera and the race HUD. The choices are tiles on the bottom screen (plat_set_setup_screen; bottom_screen.c
 * draws them, with pictures made from the game's own art); the one lit is applied at once, so the title demo on the
 * top screen shows it, with its name and what it does in a box under the logo (src/menu.c, port_setup_caption). The
 * circle pad or D-pad and A, B, START and Y work it, or taps on the tiles and the buttons under them.
 *
 * It is done once: setup=1 in settings.txt then (Settings > Start-up > First-run setup brings it back). The 3DS shows
 * it on a first run; the PC build only for DKR_SETUP=1 (plat_first_run_setup), so tests and frame comparisons never
 * meet it. Everything is in English or French, whichever the first page picks (the 3DS's own language at first); that
 * page sets the game's language too.
 */
#include <ultra64.h>
#include "types.h"
#include "audio.h"
#include "menu.h"
#include "port.h"
#include "plat/plat.h"
#include "plat/settings.h"
#include "plat/setup_screen.h"
#include "sound_ids.h"

/* The bottom screen's side (plat_3ds.c; on PC bottom_preview.c draws it for DKR_SETUP_SHOTS). NULL takes it away. */
void plat_set_setup_screen(const SetupScreen *screen);
/* A tap on the setup's tiles or buttons since the last call (bs_setup_hit's SETUP_HIT_* or a tile), or -1. */
int plat_take_setup_tap(void);

enum { RACE_HUD_TOP, RACE_HUD_BOTTOM, RACE_HUD_BOTH }; // port_set_race_hud_setting's values.

void plat_set_bottom_style(int style);
s32 port_bottom_style(void);

/* The fine tuning's pages in order; the picture's is left out where 60 fps can't be had (an Old 3DS). */
#define STEPS 5
static const s32 sSteps[STEPS] = { SETUP_PAGE_PICTURE, SETUP_PAGE_SCREEN, SETUP_PAGE_CAMERA, SETUP_PAGE_HUD,
                                    SETUP_PAGE_LOOK };

static s32 sState = -1; // -1 not decided yet, 0 not showing, 1 showing.
static s32 sPage;       // SETUP_PAGE_*.
static s32 sChosen;     // The tile lit.
static s32 sFrench;
static s32 sStyle = SETUP_STYLE_ENHANCED;
static SetupScreen sScreen;
static s32 sOpened;     // The first page has been opened.

/* English or French, by the language picked. */
static const char *tr(const char *english, const char *french) {
    return sFrench ? french : english;
}

static s32 can_60fps(void) {
    return plat_can_60fps();
}

static s32 first_step(void) {
    return can_60fps() ? 0 : 1;
}

/* Where the page is among the fine tuning's steps (first_step on), or -1. */
static s32 step_of(s32 page) {
    s32 i;
    for (i = first_step(); i < STEPS; i++) {
        if (sSteps[i] == page) {
            return i;
        }
    }
    return -1;
}

static s32 tile_count(s32 page) {
    switch (page) {
        case SETUP_PAGE_STYLE:
            return can_60fps() ? 3 : 2;
        case SETUP_PAGE_SCREEN:
        case SETUP_PAGE_LOOK:
            return 4;
        case SETUP_PAGE_HUD:
            return 3;
        default:
            return 2;
    }
}

/* The race HUD's tiles in their order on the page: the bottom screen first, as the recommended one. */
static const s32 sHudOfTile[3] = { RACE_HUD_BOTTOM, RACE_HUD_TOP, RACE_HUD_BOTH };

/* The setting a page's tile stands for is applied now, so the title screen shows it. */
static void apply_tile(s32 page, s32 tile) {
    switch (page) {
        case SETUP_PAGE_LANGUAGE:
            // Only the setup's own words change now: the game's language (a text asset to load, and a save to write)
            // is set when it finishes, so moving between the two tiles doesn't hitch.
            sFrench = tile == 1;
            break;
        case SETUP_PAGE_STYLE:
            sStyle = tile;
            if (can_60fps()) {
                plat_set_picture_style(tile == SETUP_STYLE_SPEEDY);
            }
            plat_set_widescreen_style(tile == SETUP_STYLE_CLASSIC ? 0 : 2);
            port_set_modern_camera(tile != SETUP_STYLE_CLASSIC);
            port_set_race_hud_setting(tile == SETUP_STYLE_CLASSIC ? RACE_HUD_TOP : RACE_HUD_BOTTOM);
            break;
        case SETUP_PAGE_PICTURE:
            plat_set_picture_style(tile == 1);
            break;
        case SETUP_PAGE_SCREEN:
            plat_set_widescreen_style(tile);
            break;
        case SETUP_PAGE_CAMERA:
            port_set_modern_camera(tile == 0);
            break;
        case SETUP_PAGE_HUD:
            port_set_race_hud_setting(sHudOfTile[tile]);
            break;
        case SETUP_PAGE_LOOK:
            // The setup's own buttons and boxes change look with it. Written with the rest, at the end.
            settings_set("bottomstyle", tile);
            plat_set_bottom_style(tile);
            break;
    }
}

/* The tile of the page's setting as it is now. */
static s32 current_tile(s32 page) {
    s32 i;
    switch (page) {
        case SETUP_PAGE_LANGUAGE:
            return sFrench;
        case SETUP_PAGE_STYLE:
            return sStyle;
        case SETUP_PAGE_PICTURE:
            return plat_picture_style() != 0;
        case SETUP_PAGE_SCREEN:
            return plat_widescreen_style();
        case SETUP_PAGE_CAMERA:
            return !port_modern_camera();
        case SETUP_PAGE_HUD:
            for (i = 0; i < 3; i++) {
                if (sHudOfTile[i] == port_race_hud_setting()) {
                    return i;
                }
            }
            break;
        case SETUP_PAGE_LOOK:
            return port_bottom_style();
    }
    return 0;
}

/******************************** The words ********************************/

typedef struct TileText {
    const char *name[2];
    const char *tag[2];
    const char *lines[2][SETUP_STYLE_LINES]; /* NULL ends them. */
    const char *about[2];                    /* The top screen's box: what it does. */
} TileText;

static const TileText sLanguageTiles[2] = {
    { { "ENGLISH", "ENGLISH" },
      { "", "" },
      { { "English", NULL }, { "Anglais", NULL } },
      { "Menus and text in English. OPTIONS can change it later.",
        "Les menus et les textes en anglais. Modifiable plus tard dans OPTIONS." } },
    { { "FRANCAIS", "FRANCAIS" },
      { "", "" },
      { { "French", NULL }, { "Francais", NULL } },
      { "Menus and text in French. OPTIONS can change it later.",
        "Les menus et les textes en francais. Modifiable plus tard dans OPTIONS." } },
};

static const TileText sStyleTiles[3] = {
    { { "CLASSIC", "CLASSIQUE" },
      { "LIKE THE N64", "COMME LA N64" },
      { { "4:3 picture", "30 fps, sharp", "Original camera", "Race info on top" },
        { "Image 4:3", "30 ips, nette", "Camera d'origine", "Infos en haut" } },
      { "Diddy Kong Racing as it was in 1997: the N64's picture shape, camera and race info.",
        "Diddy Kong Racing comme en 1997: la forme d'image, la camera et les infos de course de la N64." } },
    { { "ENHANCED", "AMELIORE" },
      { "RECOMMENDED", "RECOMMANDE" },
      { { "Widescreen", "30 fps, sharp", "Modern camera", "Map below" },
        { "Ecran large", "30 ips, nette", "Camera moderne", "Carte en bas" } },
      { "The 3DS way: a wide picture, a steady camera, and the map and standings on the touch screen.",
        "A la 3DS: une image large, une camera stable, la carte et le classement sur l'ecran tactile." } },
    { { "SPEEDY", "RAPIDE" },
      { "NEW 3DS", "NEW 3DS" },
      { { "Widescreen", "60 fps", "Modern camera", "Map below" },
        { "Ecran large", "60 ips", "Camera moderne", "Carte en bas" } },
      { "Everything from Enhanced at 60 frames a second, for the smoothest motion. Edges are rougher.",
        "Comme Ameliore, a 60 images par seconde pour des mouvements plus fluides. Les bords sont moins lisses." } },
};

static const TileText sPictureTiles[2] = {
    { { "SHARP", "NETTE" },
      { "RECOMMENDED", "RECOMMANDE" },
      { { "30 fps,", "smooth edges", NULL }, { "30 ips,", "bords lisses", NULL } },
      { "30 frames a second with 4x anti-aliasing: clean edges and a steady picture.",
        "30 images par seconde avec anticrenelage 4x: des bords nets et une image stable." } },
    { { "SMOOTH", "FLUIDE" },
      { "", "" },
      { { "60 fps,", "rougher edges", NULL }, { "60 ips,", "bords moins lisses", NULL } },
      { "60 frames a second for smoother motion. Anti-aliasing is off so that every frame keeps up.",
        "60 images par seconde pour des mouvements plus fluides. Sans anticrenelage, pour suivre le rythme." } },
};

static const TileText sScreenTiles[4] = {
    { { "4:3", "4:3" },
      { "LIKE THE N64", "COMME LA N64" },
      { { "Borders at", "the sides", NULL }, { "Bandes sur", "les cotes", NULL } },
      { "The N64's picture shape in the middle, with borders at the sides. Nothing is stretched.",
        "La forme d'image de la N64 au milieu, avec des bandes sur les cotes. Rien n'est etire." } },
    { { "WIDE", "LARGE" },
      { "", "" },
      { { "Widest,", "edges stretch", NULL }, { "Le plus large,", "bords etires", NULL } },
      { "The widest view of the track. Things near the left and right edges are stretched the most.",
        "La vue la plus large de la piste. Les bords gauche et droit sont les plus etires." } },
    { { "BALANCED", "EQUILIBRE" },
      { "RECOMMENDED", "RECOMMANDE" },
      { { "Wider, sides", "a bit stretched", NULL }, { "Plus large,", "un peu etire", NULL } },
      { "Between Wide and Zoomed: a wider view of the track, with the sides only gently stretched.",
        "Entre Large et Zoom: une vue plus large de la piste, les cotes un peu etires." } },
    { { "ZOOMED", "ZOOM" },
      { "", "" },
      { { "No stretch,", "a bit closer", NULL }, { "Pas etire,", "un peu plus pres", NULL } },
      { "The 4:3 view of the sides, with a little less at the top and bottom. Nothing more is stretched.",
        "La vue des cotes du 4:3, un peu moins en haut et en bas. Rien de plus n'est etire." } },
};

static const TileText sCameraTiles[2] = {
    { { "MODERN", "MODERNE" },
      { "RECOMMENDED", "RECOMMANDE" },
      { { "Steady chase", "camera", NULL }, { "Camera stable", "derriere le kart", NULL } },
      { "A steadier chase camera: no tilting in turns, follows hills sooner and swings out less.",
        "Une camera plus stable: elle ne penche pas dans les virages, suit mieux les bosses et devie moins." } },
    { { "ORIGINAL", "D'ORIGINE" },
      { "LIKE THE N64", "COMME LA N64" },
      { { "Swings and", "leans", NULL }, { "Balance et", "penche", NULL } },
      { "The N64's camera as it was: it swings wide and leans into every turn.",
        "La camera de la N64: elle balance large et penche dans chaque virage." } },
};

static const TileText sHudTiles[3] = {
    { { "BOTTOM", "EN BAS" },
      { "RECOMMENDED", "RECOMMANDE" },
      { { "Map and", "standings", NULL }, { "Carte et", "classement", NULL } },
      { "The map, the standings and your lap times on the touch screen, so the race picture stays clear.",
        "La carte, le classement et vos temps sur l'ecran tactile, pour garder l'image de course degagee." } },
    { { "TOP", "EN HAUT" },
      { "LIKE THE N64", "COMME LA N64" },
      { { "On the race", "picture", NULL }, { "Sur l'image", "de course", NULL } },
      { "Position, laps and the map on the race picture, just like the N64.",
        "Position, tours et carte sur l'image de course, comme sur N64." } },
    { { "BOTH", "LES DEUX" },
      { "", "" },
      { { "Top and", "bottom", NULL }, { "En haut", "et en bas", NULL } },
      { "The N64's race info on top, and the map and standings on the touch screen too.",
        "Les infos de course de la N64 en haut, et la carte et le classement sur l'ecran tactile." } },
};

static const TileText sLookTiles[4] = {
    { { "CLASSIC", "CLASSIC" },
      { "DEFAULT", "DEFAUT" },
      { { "Same as", "the menus", NULL }, { "Comme les", "menus", NULL } },
      { "The bottom screen in the game's own style: its rounded boxes, just like the menus.",
        "L'ecran du bas dans le style du jeu: ses boites arrondies, comme les menus." } },
    { { "GLASS", "VERRE" },
      { "", "" },
      { { "Soft glass", "cards", NULL }, { "Cartes de", "verre", NULL } },
      { "Frosted glass cards and round buttons: soft and light.",
        "Des cartes de verre depoli et des boutons ronds: doux et leger." } },
    { { "RACE TV", "TELE" },
      { "", "" },
      { { "Race TV", "slabs", NULL }, { "Bandeaux", "de course", NULL } },
      { "Broadcast: slanted slabs in red and yellow, like a race on TV.",
        "Television: des bandeaux inclines rouges et jaunes, comme une course a la tele." } },
    { { "DOCK", "BARRE" },
      { "", "" },
      { { "Few boxes,", "icon bar", NULL }, { "Peu de boites,", "barre icones", NULL } },
      { "Almost no boxes: the race info floats, with an icon bar below.",
        "Presque pas de boites: les infos flottent, avec une barre d'icones en dessous." } },
};

static const TileText *tiles_of(s32 page) {
    switch (page) {
        case SETUP_PAGE_LANGUAGE:
            return sLanguageTiles;
        case SETUP_PAGE_STYLE:
            return sStyleTiles;
        case SETUP_PAGE_PICTURE:
            return sPictureTiles;
        case SETUP_PAGE_SCREEN:
            return sScreenTiles;
        case SETUP_PAGE_CAMERA:
            return sCameraTiles;
        case SETUP_PAGE_LOOK:
            return sLookTiles;
        default:
            return sHudTiles;
    }
}

static const char *question_of(s32 page) {
    switch (page) {
        case SETUP_PAGE_LANGUAGE:
            return tr("CHOOSE YOUR LANGUAGE", "CHOISISSEZ LA LANGUE");
        case SETUP_PAGE_STYLE:
            return tr("HOW DO YOU WANT TO PLAY?", "COMMENT VOULEZ-VOUS JOUER?");
        case SETUP_PAGE_PICTURE:
            return tr("HOW SHOULD IT LOOK?", "QUELLE IMAGE?");
        case SETUP_PAGE_SCREEN:
            return tr("WHAT SHAPE OF PICTURE?", "QUELLE FORME D'IMAGE?");
        case SETUP_PAGE_CAMERA:
            return tr("WHICH CAMERA?", "QUELLE CAMERA?");
        case SETUP_PAGE_LOOK:
            return tr("HOW SHOULD THE BOTTOM SCREEN LOOK?", "STYLE DE L'ECRAN DU BAS?");
        default:
            return tr("WHERE SHOULD RACE INFO GO?", "OU AFFICHER LES INFOS?");
    }
}

/* Copies text with the bounds of the field. */
#define COPY(field, text) plat_snprintf((field), sizeof(field), "%s", (text))

/* Fills in what the bottom screen shows for the page and the tile lit. */
static void describe(void) {
    const TileText *texts = tiles_of(sPage);
    s32 i, j, step = step_of(sPage), last = step == STEPS - 1;
    SetupScreen *s = &sScreen;

    static const SetupScreen sEmpty;
    *s = sEmpty;
    s->page = sPage;
    COPY(s->question, question_of(sPage));
    s->tileCount = tile_count(sPage);
    s->chosen = sChosen;
    for (i = 0; i < s->tileCount; i++) {
        SetupTile *t = &s->tiles[i];
        t->art = i;
        COPY(t->name, texts[i].name[sFrench]);
        COPY(t->tag, texts[i].tag[sFrench]);
        for (j = 0; j < SETUP_STYLE_LINES && texts[i].lines[sFrench][j] != NULL; j++) {
            COPY(t->lines[j], texts[i].lines[sFrench][j]);
        }
        t->lineCount = j;
    }
    if (step >= 0) {
        static const char *const sStepNames[STEPS][2] = {
            { "PICTURE", "IMAGE" }, { "SCREEN", "ECRAN" }, { "CAMERA", "CAMERA" }, { "RACE INFO", "INFOS" },
            { "LOOK", "STYLE" }
        };
        for (i = first_step(); i < STEPS; i++) {
            COPY(s->steps[s->stepCount], sStepNames[i][sFrench]);
            if (i == step) {
                s->step = s->stepCount;
            }
            s->stepCount++;
        }
    }
    if (sPage == SETUP_PAGE_LANGUAGE) {
        COPY(s->buttons[2], tr("NEXT", "SUIVANT"));
        COPY(s->help, tr("Choose with the circle pad, then A", "Choisissez avec le stick, puis A"));
    } else if (sPage == SETUP_PAGE_STYLE) {
        COPY(s->buttons[0], tr("BACK", "RETOUR"));
        COPY(s->buttons[1], tr("FINE TUNE", "REGLAGES"));
        COPY(s->buttons[2], tr("START", "JOUER"));
        COPY(s->help, tr("A: start   Y: fine tune   B: back", "A: jouer   Y: reglages   B: retour"));
    } else {
        COPY(s->buttons[0], tr("BACK", "RETOUR"));
        if (!last) {
            COPY(s->buttons[1], tr("FINISH", "TERMINER"));
        }
        COPY(s->buttons[2], last ? tr("FINISH", "TERMINER") : tr("NEXT", "SUIVANT"));
        COPY(s->help, tr("A: next   B: back   START: finish", "A: suivant   B: retour   START: terminer"));
    }
    plat_set_setup_screen(s);
}

/******************************** The pages ********************************/

static void open_page(s32 page) {
    sPage = page;
    sChosen = current_tile(page);
    if (sChosen >= tile_count(page)) {
        sChosen = 0;
    }
    describe();
}

static void finish(void) {
    sState = 0;
    settings_set("setup", 1);
    settings_defer(FALSE);
    set_language(sFrench ? LANGUAGE_FRENCH : LANGUAGE_ENGLISH);
    plat_set_setup_screen(NULL);
}

static void choose(s32 tile) {
    if (tile != sChosen && tile >= 0 && tile < tile_count(sPage)) {
        sChosen = tile;
        apply_tile(sPage, tile);
        describe();
    }
}

/* B, or the Back button. */
static void back(void) {
    s32 step = step_of(sPage);
    if (sPage == SETUP_PAGE_LANGUAGE) {
        return;
    }
    sound_play(SOUND_MENU_BACK3, NULL);
    open_page(sPage == SETUP_PAGE_STYLE ? SETUP_PAGE_LANGUAGE
              : step == first_step()     ? SETUP_PAGE_STYLE
                                         : sSteps[step - 1]);
}

/* A, or the Next button: on to the next page, or done after the play style or the last step. */
static void next(void) {
    s32 step = step_of(sPage);
    sound_play(SOUND_SELECT2, NULL);
    if (sPage == SETUP_PAGE_LANGUAGE) {
        open_page(SETUP_PAGE_STYLE);
    } else if (sPage == SETUP_PAGE_STYLE || step == STEPS - 1) {
        finish();
    } else {
        open_page(sSteps[step + 1]);
    }
}

/* Y, or the middle button: the fine tuning from the play style page; done from a step. */
static void middle(void) {
    if (sPage == SETUP_PAGE_STYLE) {
        sound_play(SOUND_SELECT2, NULL);
        open_page(sSteps[first_step()]);
    } else if (sPage != SETUP_PAGE_LANGUAGE) {
        sound_play(SOUND_SELECT2, NULL);
        finish();
    }
}

s32 port_setup_active(void) {
    if (sState < 0) {
        sState = plat_first_run_setup() && !port_warp_active() && !port_autorun_enabled() &&
                 (plat_getenv("DKR_SETUP") != NULL || settings_get("setup", 0) == 0);
    }
    return sState > 0;
}

void port_setup_reset(void) {
    settings_set("setup", 0);
}

/* Debug builds' Start-up row: asks for the setup again and starts it right away if the title screen is up. */
void port_setup_restart(void) {
    settings_set("setup", 0);
    if (sState != 1) {
        sState = -1;
        sOpened = FALSE;
    }
}

s32 port_setup_done(void) {
    return settings_get("setup", 0) != 0;
}

/*
 * A frame of the title screen while the setup shows: buttons are the menu's buttons just pressed, stickX and stickY
 * its stick steps (src/menu.c's gMenuButtons, gMenuStickX and gMenuStickY). The first frame opens the first page.
 */
void port_setup_update(u32 buttons, s32 stickX, s32 stickY) {
    s32 count, columns, tile, tap;

    if (!port_setup_active()) {
        return;
    }
    if (!sOpened) {
        sOpened = TRUE;
        settings_defer(TRUE); // Every tile applies settings: written once, at the end.
        sFrench = plat_system_french();
        apply_tile(SETUP_PAGE_LANGUAGE, sFrench);
        apply_tile(SETUP_PAGE_STYLE, SETUP_STYLE_ENHANCED);
        open_page(SETUP_PAGE_LANGUAGE);
        return;
    }
    if (port_autorun_enabled()) {
        sState = 0; // A test run is starting from the title screen: no setup in its way, and asked again later.
        settings_defer(FALSE);
        plat_set_setup_screen(NULL);
        return;
    }
    // The D-pad moves as the stick does.
    stickX += (buttons & R_JPAD) ? 1 : (buttons & L_JPAD) ? -1 : 0;
    stickY += (buttons & U_JPAD) ? 1 : (buttons & D_JPAD) ? -1 : 0;
    count = tile_count(sPage);
    columns = count == 4 ? 2 : count;
    tile = sChosen;
    if (stickX != 0) {
        tile += stickX > 0 ? 1 : -1;
    } else if (stickY != 0 && columns < count) {
        tile += stickY > 0 ? -columns : columns; // stickY is +1 for up.
    }
    if (tile != sChosen && tile >= 0 && tile < count) {
        sound_play(SOUND_MENU_PICK2, NULL);
        choose(tile);
    }
    tap = plat_take_setup_tap();
    if (tap >= 0 && tap < count) {
        if (tap != sChosen) {
            sound_play(SOUND_MENU_PICK2, NULL);
            choose(tap);
        }
    } else if (tap == SETUP_HIT_BACK) {
        back();
    } else if (tap == SETUP_HIT_MIDDLE) {
        middle();
    } else if (tap == SETUP_HIT_NEXT || (buttons & A_BUTTON)) {
        next();
    } else if (buttons & B_BUTTON) {
        back();
    } else if (buttons & (U_CBUTTONS | L_CBUTTONS)) {
        middle();
    } else if (buttons & START_BUTTON) {
        if (sPage == SETUP_PAGE_LANGUAGE) {
            next();
        } else {
            sound_play(SOUND_SELECT2, NULL);
            finish();
        }
    }
}

/* The top screen's box: the lit tile's name, its tag and what it does. FALSE when the setup isn't showing. */
s32 port_setup_caption(const char **name, const char **tag, const char **about) {
    const TileText *texts;
    if (sState <= 0 || sScreen.tileCount == 0) {
        return FALSE;
    }
    texts = tiles_of(sPage);
    *name = texts[sChosen].name[sFrench];
    *tag = texts[sChosen].tag[sFrench];
    *about = texts[sChosen].about[sFrench];
    return TRUE;
}
