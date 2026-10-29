/*
 * The port's text in the game's own menus in the game's other languages (port.h port_text). The game's menu text comes
 * from the ROM in each language (load_menu_text); what the port adds to those menus (the settings pages of
 * src/menu.c, the plat/options.h rows and the SAVE OPTIONS screen) is written in English, and this gives the French
 * for it when the game is set to FRANCAIS. Like the game's own French it has no accents: its fonts have none.
 *
 * Every string is looked up as it is written in English, so a new row's label, value or hint needs a line here. One
 * that has none shows in English. The 3DS bottom screen's Settings menu stays English.
 */
#include "menu.h"
#include "port.h"
#include "types.h"

typedef struct PortText {
    const char *english;
    const char *french;
} PortText;

static const PortText sFrench[] = {
    // src/menu.c: the settings categories and their pages.
    { "VIDEO OPTIONS", "OPTIONS VIDEO" },
    { "SCREEN OPTIONS", "OPTIONS ECRAN" },
    { "CAMERA OPTIONS", "OPTIONS CAMERA" },
    { "CONTROL OPTIONS", "OPTIONS MANETTE" },
    { "GAME OPTIONS", "OPTIONS DU JEU" },
    { "MORE AUDIO OPTIONS", "AUTRES OPTIONS SON" },
    { " PERCENT", " POUR CENT" },
    { "Used from the next time the game starts", "Pris en compte au prochain demarrage" },

    // Values many rows share.
    { "On", "Oui" },
    { "Off", "Non" },
    { "ON", "OUI" },
    { "OFF", "NON" },
    { "Show", "Affiches" },
    { "Skip", "Passes" },

    // SCREEN OPTIONS stereo 3D tuning and CONTROL OPTIONS tilt steering (PR #224); their hints are still English.
    { "3D depth", "Profondeur 3D" },
    { "3D convergence", "Convergence 3D" },
    { "3D HUD depth", "Profondeur 3D du HUD" },
    { "Auto convergence", "Convergence auto" },
    { "Comfort target", "Cible de confort" },
    { "On screen", "Sur l'ecran" },
    { "Tilt steering", "Direction par inclinaison" },
    { "Tilt for full lock", "Inclinaison max" },
    { "Reversed", "Inversee" },

    // VIDEO OPTIONS (port/3ds/menu_3ds.c Picture page, port/host/host_main.c).
    { "Anti-aliasing", "Anticrenelage" },
    { "Frame rate", "Images par seconde" },
    { "30 fps", "30 IPS" },
    { "60 fps", "60 IPS" },
    { "Mipmaps", "Mipmaps" },
    { "Colour boost", "Couleurs vives" },
    { "Low", "Faibles" },
    { "Medium", "Moyennes" },
    { "High", "Fortes" },
    { "Render ahead", "Rendu en avance" },
    { "1 frame", "1 image" },
    { "2 frames", "2 images" },
    { "Smoother edges; the GPU draws 2x", "Bords plus lisses; le GPU dessine 2x" },
    { "Smoother edges; the GPU draws 4x", "Bords plus lisses; le GPU dessine 4x" },
    { "60 when a frame fits (try AA off)", "60 si possible (essayez sans AA)" },
    { "Steady 20 (Old 3DS)", "20 stables (Old 3DS)" },
    { "Tints each level a colour", "Colore chaque niveau" },
    { "No shimmer on distant textures", "Textures lointaines sans scintillement" },
    { "Richer colours on the 3DS screen", "Couleurs plus riches sur l'ecran 3DS" },
    { "Fewer dropped frames, a frame more lag", "Moins d'images perdues, une de retard" },
    { "The N64's timing", "Le rythme de la N64" },

    // SCREEN OPTIONS (port/3ds/menu_3ds.c Screen page, port/src/game/port_race_hud.c).
    { "Widescreen", "Ecran large" },
    { "Wide", "Large" },
    { "Balanced", "Equilibre" },
    { "Zoomed", "Zoome" },
    { "800 pixels", "800 pixels" },
    { "Stereo 3D", "3D stereo" },
    { "HUD", "Interface" },
    { "Screen edges", "Bords" },
    { "Edge trimming", "Coupe des bords" },
    { "4:3 picture in the middle", "Image 4:3 au centre" },
    { "Most to the sides; edges stretch most", "Le plus large; bords les plus etires" },
    { "Between Wide and Zoomed", "Entre Large et Zoome" },
    { "4:3's sides, less top; no extra stretch", "Cotes du 4:3, moins de haut; sans etirer" },
    { "Sharper picture while 3D is off", "Image plus nette sans la 3D" },
    { "Not on this console (2DS)", "Pas sur cette console (2DS)" },
    { "Widescreen HUD out at the sides", "Interface aux bords de l'ecran large" },
    { "Widescreen HUD in the 4:3 middle", "Interface au centre 4:3 en ecran large" },
    { "Cuts triangles far off screen", "Coupe les triangles loin hors ecran" },
    { "With the 3D slider; no anti-aliasing in 3D", "Avec le curseur 3D; sans anticrenelage" },
    { "Race HUD on", "Interface de course" },
    { "Top", "En haut" },
    { "Bottom", "En bas" },
    { "Both", "Les deux" },
    { "Lap, item, map on the top screen", "Tour, objet, carte sur l'ecran du haut" },
    { "On both screens", "Sur les deux ecrans" },
    { "Standings, item and map below", "Classement, objet et carte en bas" },

    // CAMERA OPTIONS (port/src/game/port_camera.c).
    { "Camera style", "Style de camera" },
    { "Modern", "Moderne" },
    { "Original", "Original" },
    { "Wall check", "Eviter les murs" },

    // The audio rows (port/3ds/audio_ndsp.c).
    { "Sound engine (restart)", "Moteur son (redemarrer)" },
    { "Mixing core (restart)", "Coeur de mixage (redemarrer)" },
    { "Mixer rate (restart)", "Frequence de mixage (redemarrer)" },
    { "Smooth resampling", "Son lisse" },
    { "Sound buffer", "Tampon son" },
    { "CPU mixer only", "Mixeur CPU seul" },
    { "DSP voices", "Voix DSP" },
    { "CPU mixer", "Mixeur CPU" },
    { "System", "Systeme" },
    { "Renderer", "Rendu" },
    { "Large", "Grand" },
    { "Normal", "Normal" },

    // CONTROL OPTIONS (port/src/plat/controls.c).
    { "Button layout", "Disposition" },
    { "Standard", "Standard" },
    { "A/B swapped", "A/B inverses" },
    { "L/R swapped", "L/R inverses" },
    { "Custom", "Perso" },
    { "A button", "Bouton A" },
    { "B button", "Bouton B" },
    { "X button", "Bouton X" },
    { "Y button", "Bouton Y" },
    { "L button", "Bouton L" },
    { "R button", "Bouton R" },
    { "ZL button", "Bouton ZL" },
    { "ZR button", "Bouton ZR" },
    { "SELECT button", "Bouton SELECT" },
    { "Accelerate", "Accelerer" },
    { "Brake", "Freiner" },
    { "Use item", "Objet" },
    { "Hop, drift", "Saut, derapage" },
    { "N64 L", "L N64" },
    { "C-Up", "C-Haut" },
    { "C-Down", "C-Bas" },
    { "C-Left", "C-Gauche" },
    { "C-Right", "C-Droite" },
    { "Start", "Start" },
    { "Look back", "Regard arriere" },
    { "Nothing", "Rien" },
    { "Stick deadzone", "Zone morte du stick" },
    { "Stick sensitivity", "Sensibilite du stick" },
    { "Stick response", "Reponse du stick" },
    { "Linear", "Lineaire" },
    { "Soft", "Douce" },
    { "Softer", "Tres douce" },
    { "Quick", "Rapide" },
    { "Reset controls", "Reinitialiser" },

    // The Options menu's own rows (port/src/game/port_intro.c).
    { "Intro logos", "Logos d'intro" },
    { "Start at the title screen", "Demarre a l'ecran titre" },
    { "A, B or START skips them", "A, B ou START pour les passer" },

    // SAVE OPTIONS (src/menu.c port_savemenu_*).
    { "GAME PAK", "LOGICIEL" },
    { "CONTROLLER PAK", "CART. SAUV." },
    { "BACKUPS", "COPIES SD" },
    { "SLOT A", "PARTIE A" },
    { "SLOT B", "PARTIE B" },
    { "SLOT C", "PARTIE C" },
    { "TIME RECORDS", "TEMPS RECORDS" },
    { "BONUSES", "BONUS" },
    { "SAVE ", "SAUV. " },
    { "COPY TO ", "COPIER VERS " },
    { "COPY TO GAME PAK", "VERS LOGICIEL" },
    { "COPY TO PAK", "VERS CART. SAUV." },
    { "NO SAVES", "AUCUNE SAUVEGARDE" },
    { "EMPTY", "VIDE" },
    { "BACK UP NOW", "SAUVEGARDER" },
    { "HAS NO SAVES", "SANS SAUVEGARDE" },
    { "COPIES EVERY SAVE", "COPIE TOUTES LES" },
    { "TO THE SD CARD", "SAUVEGARDES SUR SD" },
    { "BEFORE A RESTORE", "AVANT RESTAUR." },
    { "BACKUP", "COPIE SD" },
    { "AND CONTROLLER PAK", "ET CART. DE SAUV." },
    { "ADVENTURE", "AVENTURE" },
    { "ADVENTURE TWO", "AVENTURE II" },
    { "BEST LAPS AND", "MEILLEURS TOURS" },
    { "COURSE TIMES", "ET TEMPS DE COURSE" },
    { "TIME TRIAL GHOSTS", "FANTOMES ENREGISTRES" },
    { "ADVENTURE TWO,", "AVENTURE II," },
    { "DRUMSTICK AND CODES", "DRUMSTICK ET CODES" },
    { "ANOTHER GAME'S SAVE", "DONNEES D'UN AUTRE JEU" },
    { "RESTORED", "RESTAURE" },
    { "THE GAME CLOSES TO LOAD IT", "LE JEU QUITTE POUR CHARGER" },
    { "RESTORE", "RESTAURER" },
    { "REPLACE YOUR SAVES?", "REMPLACER VOS DONNEES?" },
    { "A YES   B NO", "A OUI   B NON" },
    { "PRESS A TO BACK UP", "A POUR SAUVEGARDER" },
    { "PRESS A FOR OPTIONS", "A POUR LES OPTIONS" },
    { "BACKED UP", "SAUVEGARDE FAITE" },
    { "NOT DURING A TEST RUN", "PAS PENDANT UN TEST" },
    { "COULD NOT WRITE TO SD", "ECRITURE SD IMPOSSIBLE" },
    { "THAT BACKUP IS DAMAGED", "COPIE ENDOMMAGEE" },
    { "THAT BACKUP IS GONE", "COPIE INTROUVABLE" },
    { "ERASED", "EFFACE" },
    { "COPIED", "COPIE" },
};

static s32 same_text(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

const char *port_text(const char *english) {
    u32 i;
    if (english == NULL || get_language() != LANGUAGE_FRENCH) {
        return english;
    }
    for (i = 0; i < sizeof(sFrench) / sizeof(sFrench[0]); i++) {
        if (same_text(sFrench[i].english, english)) {
            return sFrench[i].french;
        }
    }
    return english;
}
