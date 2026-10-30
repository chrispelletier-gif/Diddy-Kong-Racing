/*
 * The port's text in the game's own menus in the game's other languages (port.h port_text). The game's menu text comes
 * from the ROM in each language (load_menu_text); what the port adds to those menus (the settings pages of
 * src/menu.c, the plat/options.h rows and the SAVE OPTIONS screen) is written in English, and this gives the French
 * for it when the game is set to FRANCAIS. Like the game's own French it has no accents: its fonts have none.
 *
 * Every string is looked up as it is written in English, so a new row's label, value or hint needs a line here. One
 * that has none shows in English. The 3DS bottom screen (plat/bottom_screen.c and its menus) uses it too. A value
 * with a number is looked up by its format ("%d fps"), where its owner writes it. DKR_LANGUAGE=fr in env.txt (or
 * the environment on PC) shows French whatever the game is set to, for a test.
 */
#include "menu.h"
#include "plat/plat.h"
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

    // Values with a number, by their format.
    { "%d fps", "%d IPS" },
    { "%d frame%s", "%d image%s" },
    { "%d out", "%d devant" },
    { "%d in", "%d derriere" },
    { "%d degrees", "%d degres" },
    { "%c%d lap%s", "%c%d tour%s" },

    // The stereo 3D rows' tips (PR #224).
    { "How deep the 3D looks", "Profondeur de l'effet 3D" },
    { "Higher: the kart comes out of the screen", "Plus haut: le kart sort de l'ecran" },
    { "Where the HUD and text sit in 3D", "Place de l'interface et du texte en 3D" },
    { "Keeps close things from popping out", "Evite que les objets proches ressortent" },
    { "How far auto convergence lets things out", "Jusqu'ou la convergence auto fait ressortir" },

    // The bottom screen's Settings menu (port/3ds/menu_3ds.c, port/3ds/main_3ds.c, port/3ds/saves_3ds.c).
    { "Picture", "Image" },
    { "Screen", "Ecran" },
    { "Controls", "Touches" },
    { "Sound", "Son" },
    { "Race HUD", "Interface" },
    { "Camera", "Camera" },
    { "Start-up", "Intro" },
    { "Saves", "Sauver" },
    { "Tap a value's arrows to change it", "Touchez les fleches pour changer" },
    { "Changes show from the next frame", "Visible des l'image suivante" },
    { "Saved as N64 dkr.eep and dkr.mpk", "Enregistre en dkr.eep et dkr.mpk N64" },
    { "Widescreen, 800 pixels, 3D", "Ecran large, 800 pixels, 3D" },
    { "Renderer (restart)", "Rendu (redemarrer)" },
    { "Game core", "Coeur du jeu" },
    { "System 30%", "Systeme 30%" },
    { "System 50%", "Systeme 50%" },
    { "System 80%", "Systeme 80%" },
    { "Back up now", "Sauvegarder" },
    { "Backup", "Copie" },
    { "Restore backup", "Restaurer une copie" },
    { "Tap again to restore", "Touchez encore: restaurer" },
    { "None", "Aucune" },
    { "No backups yet", "Aucune copie" },
    { "Saves a dated copy, ghosts included", "Copie datee, fantomes compris" },
    { "Folders in sdmc:/3ds/dkr/backup", "Dossiers dans sdmc:/3ds/dkr/backup" },
    { "%d of %d in sdmc:/3ds/dkr/backup", "%d sur %d dans sdmc:/3ds/dkr/backup" },
    { "Overwrites your save, then quits", "Remplace votre partie, puis quitte" },
    { "Your save is backed up first", "Votre partie est copiee avant" },
    { "Not during a test run", "Pas pendant un test" },
    { "The game is busy: try again", "Le jeu est occupe: reessayez" },
    { "That backup's files are too short", "Fichiers de la copie trop courts" },
    { "That backup is gone: pick again", "Copie introuvable: choisissez encore" },
    { "Could not write to the SD card", "Ecriture sur la carte SD impossible" },
    { "Backed up to sdmc:/3ds/dkr/backup", "Copie dans sdmc:/3ds/dkr/backup" },
    { "Restored: the game closes to load it", "Restaure: le jeu quitte pour la charger" },
    { "No sound: DSP firmware not found.", "Pas de son: firmware DSP introuvable." },
    { "Put dspfirm.cdc in sdmc:/3ds/", "Placez dspfirm.cdc dans sdmc:/3ds/" },

    // The bottom screen's buttons (port/3ds/update_3ds.c).
    { "Settings", "Reglages" },
    { "Pause", "Pause" },
    { "Close", "Fermer" },
    { "Prev", "Prec." },
    { "Next", "Suiv." },
    { "Updates", "MAJ" },
    { "Stop test", "Arreter" },
    { "cancel", "Annuler" },

    // A debug build's Test run and Tools pages and their messages (port/3ds/menu_3ds.c, update_3ds.c,
    // bugreport_3ds.c, port/src/game/port_autorun.c). The log keeps the English (plat_status).
    { "Test run", "Tests" },
    { "Tools", "Outils" },
    { "Full test", "Test complet" },
    { "Quick test", "Test rapide" },
    { "Boss fights", "Combats de boss" },
    { "GPU bench", "Banc GPU" },
    { "GPU benchmark", "Banc GPU" },
    { "Switch bench", "Banc d'options" },
    { "Stop the test run", "Arreter le test" },
    { "Every track and character, ~8 hours", "Chaque circuit et perso, ~8 heures" },
    { "5 slowest tracks twice, ~20 minutes", "5 circuits les plus lents x2, ~20 min" },
    { "All 10 boss races once, ~30 minutes", "Les 10 courses de boss, ~30 min" },
    { "3 heavy tracks x 5 GPU settings, ~30 min", "3 circuits lourds x 5 reglages GPU, ~30 min" },
    { "3 heavy tracks x %ld switch sets, ~%ld min", "3 circuits lourds x %ld jeux d'options, ~%ld min" },
    { "The results so far go out as a report", "Les resultats partent en rapport" },
    { "No switches to test right now", "Aucune option a tester pour l'instant" },
    { "Starts at the title; saves not written", "Part du titre; rien n'est sauvegarde" },
    { "Profiler", "Profileur" },
    { "Check for updates", "Mises a jour" },
    { "Send a bug report", "Envoyer un rapport" },
    { "Frame times on this screen", "Temps par image sur cet ecran" },
    { "Install a newer build from GitHub", "Installer une version recente de GitHub" },
    { "Old 3DS: lend it the system core", "Old 3DS: emprunte le coeur systeme" },
    { "Log and snapshot (also SELECT+touch)", "Journal et capture (aussi SELECT+toucher)" },
    { "GPU vertex maths", "Calcul sommets GPU" },
    { "3D made once", "3D calculee une fois" },
    { "+ track", "+ circuit" },
    { "+ track, models", "+ circuit, modeles" },
    { "Test: P30, then P31 track, P34 models", "Test: P30, puis P31 circuit, P34 modeles" },
    { "Test (P33): needs the 3D slider up", "Test (P33): curseur 3D leve" },
    { "Stop the test run? The results so far are sent as a report.",
      "Arreter le test? Les resultats partent en rapport." },
    { "Quit to finish the update", "Quittez pour finir la mise a jour" },
    { "Checking for updates...", "Recherche de mises a jour..." },
    { "Downloading %s: %d%%", "Telechargement %s: %d%%" },
    { "Downloading %s...", "Telechargement %s..." },
    { "Installing %s...", "Installation %s..." },
    { "Closing...", "Fermeture..." },
    { "Update: the game was not started from the SD card", "Mise a jour: jeu non lance depuis la carte SD" },
    { "Update: %s", "Mise a jour: %s" },
    { "Update: %s: %s", "Mise a jour: %s: %s" },
    { "Update: no builds found%s", "Mise a jour: aucune version trouvee%s" },
    { " (a private repo needs the token)", " (depot prive: il faut le jeton)" },
    { "This build: %.7s", "Cette version: %.7s" },
    { " (this)", " (celle-ci)" },
    { "%-6s none", "%-6s aucune" },
    { "Update: cannot write %s", "Mise a jour: ecriture impossible %s" },
    { "Update failed: %s", "Echec de la mise a jour: %s" },
    { "Update failed: could not replace %s", "Echec de la mise a jour: remplacement impossible %s" },
    { "Installed %.7s (%.20s).", "Installee %.7s (%.20s)." },
    { "It runs the next time the game starts.", "Elle demarre au prochain lancement." },
    { "Bug report: %s", "Rapport: %s" },
    { "Bug report: out of memory", "Rapport: memoire pleine" },
    { "Bug report: taking a snapshot", "Rapport: capture en cours" },
    { "Bug report: no token in sdmc:/3ds/dkr/bugreport.txt; saving a snapshot only",
      "Rapport: pas de jeton dans sdmc:/3ds/dkr/bugreport.txt; capture seulement" },
    { "Bug report: saving the snapshot (%d s)", "Rapport: enregistrement de la capture (%d s)" },
    { "Bug report sent: issue #%s", "Rapport envoye: issue #%s" },
    { "Bug report %s: %s", "Rapport %s: %s" },
    { "partly sent", "envoye en partie" },
    { "not sent", "non envoye" },
    { "Saved as sdmc:/3ds/dkr/bugreport_unsent.txt", "Enregistre: sdmc:/3ds/dkr/bugreport_unsent.txt" },
    { "Test run results: no token in bugreport.txt, so they stay on the SD card",
      "Resultats des tests: pas de jeton dans bugreport.txt, ils restent sur la carte SD" },
    { "%s %s: %d races, %d problems", "%s %s: %d courses, %d problemes" },
    { "%s started: race %d of %d", "%s lance: course %d sur %d" },
    { "finished", "termine" },
    { "stopped", "arrete" },
    { "Switch bench: no switches to test right now", "Banc d'options: aucune option a tester" },

    // The bottom screen's controls card and race HUD (port/src/plat/bottom_screen.c).
    { "CONTROLS", "COMMANDES" },
    { "Circle Pad", "Pad circulaire" },
    { "Steer", "Diriger" },
    { "Hop / drift", "Derapage" },
    { "Bug report", "Rapport bug" },
    { "Version %.20s", "Version %.20s" },
    { "Build %.24s", "Version %.24s" },
    { "Build %.32s", "Version %.32s" },
    { "You", "Toi" },
    { "of %d", "sur %d" },
    { "Lap %d", "Tour %d" },
    { "Best lap", "Meilleur tour" },
    { "SPLITS", "TEMPS" },
    { "Best run", "Ton record" },
    { "none yet", "aucun" },
    { "you lead", "en tete" },
    { "behind", "derriere" },
    { "Record", "Record" },
    { "to beat", "a battre" },
    { "Record lap", "Tour record" },
    { "TIME TRIAL", "CHRONO" },
    { "CAR RACE", "VOITURE" },
    { "HOVER RACE", "HYDRO" },
    { "PLANE RACE", "AVION" },
    { "Car race", "Course auto" },
    { "Hover race", "Course hydro" },
    { "Plane race", "Course avion" },
    { "Taj's challenge", "Defi de Taj" },
    { "Win: a new balloon", "Gagne: un ballon" },
    { "Round %d", "Manche %d" },
    { "Silver coins", "Pieces d'argent" },
    { "Out", "Elimine" },
    { "Carrying", "Transporte" },
    { "Get it", "Rentre" },
    { "to your", "au" },
    { "nest", "nid" },
    { "No egg", "Pas d'oeuf" },
    { "Drop it if hit", "Perdu si touche" },
    { "Your health", "Ta sante" },
    { "Hits take bananas", "Touche: bananes perdues" },
    { "Last one left wins", "Le dernier gagne" },
    { "Bring them home", "Ramene-les" },
    { "to your base", "a ta base" },
    { "BATTLE", "BATAILLE" },
    { "Racers left", "En course" },
    { "EGG COLLECTOR", "OEUFS" },
    { "BANANAS", "BANANES" },
    { "First to %d %s", "Premier a %d %s" },
    { "eggs wins", "oeufs gagne" },
    { "wins", "gagne" },
    { "%d to go", "encore %d" },
    { "Doors", "Portes" },
    { "Amulets", "Amulettes" },
    { "%d of 4 pieces", "%d sur 4 morceaux" },
    { "+1 held", "+1 porte" },
    { "Saves and settings: sdmc:/3ds/dkr/", "Donnees: sdmc:/3ds/dkr/" },
    { "Save file %s", "Partie %s" },
    { "Balloons", "Ballons" },
    { "T.T. amulet", "Amulette T.T." },
    { "Wizpig amulet", "Amulette Wizpig" },
    { "cleared", "fini" },
    { "Car", "Voiture" },
    { "Hovercraft", "Hydroglisseur" },
    { "Plane", "Avion" },
    { "Best race", "Meilleure course" },

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
    { "Bottom screen look", "Style de l'ecran du bas" },
    { "Classic", "Classique" },
    { "Glass", "Verre" },
    { "Broadcast", "Television" },
    { "Dock", "Barre" },
    { "The game's own boxes", "Les cadres du jeu" },
    { "Soft glass cards, round buttons", "Cartes en verre, boutons ronds" },
    { "Race TV slabs in red and yellow", "Bandeaux TV rouge et jaune" },
    { "Few boxes, icon bar below", "Peu de cadres, barre d'icones" },
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
    { "No effect", "Sans effet" },
    { "Camera view", "Vue camera" },
    { "Track arrows", "Fleches" },
    { "HUD toggle", "Bascule HUD" },
    { "Start", "Start" },
    { "Look back", "Vue arriere" },
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
    { "First-run setup", "Config. initiale" },
    { "Done", "Faite" },
    { "Next start", "Au demarrage" },
    { "Language, play style and picture again", "Langue, style de jeu et image" },
    { "Shows at the title screen next start", "Au prochain demarrage" },

    // SAVE OPTIONS (src/menu.c port_savemenu_*).
    { "SAVES", "PARTIES" },
    { "GHOSTS", "FANTOMES" },
    { "BACKUPS", "COPIES SD" },
    { "SLOT A", "PARTIE A" },
    { "SLOT B", "PARTIE B" },
    { "SLOT C", "PARTIE C" },
    { "TIME RECORDS", "TEMPS RECORDS" },
    { "BONUSES", "BONUS" },
    { "SAVE ", "SAUV. " },
    { "COPY TO ", "COPIER VERS " },
    { "COPY TO SAVES", "VERS PARTIES" },
    { "COPY TO GHOSTS", "VERS FANTOMES" },
    { "NO SAVES", "AUCUNE SAUVEGARDE" },
    { "EMPTY", "VIDE" },
    { "BACK UP NOW", "SAUVEGARDER" },
    { "NOTHING SAVED YET", "RIEN D'ENREGISTRE" },
    { "COPIES EVERY SAVE", "COPIE TOUTES LES" },
    { "TO THE SD CARD", "SAUVEGARDES SUR SD" },
    { "BEFORE A RESTORE", "AVANT RESTAUR." },
    { "BACKUP", "COPIE SD" },
    { "AND GHOSTS", "ET FANTOMES" },
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

int port_text_french(void) {
    static s32 sForced = -1; // DKR_LANGUAGE: 1 French, 0 none; read once.
    if (sForced < 0) {
        const char *env = plat_getenv("DKR_LANGUAGE");
        sForced = env != NULL && (env[0] == 'f' || env[0] == 'F');
    }
    return sForced || get_language() == LANGUAGE_FRENCH;
}

const char *port_text(const char *english) {
    u32 i;
    if (english == NULL || english[0] == '\0' || !port_text_french()) {
        return english;
    }
    for (i = 0; i < sizeof(sFrench) / sizeof(sFrench[0]); i++) {
        if (same_text(sFrench[i].english, english)) {
            return sFrench[i].french;
        }
    }
    return english;
}

/*
 * The game's own dialogue after it is loaded from the ROM (set_current_text). The French Taj text tells the player to
 * press Z to honk, a button the 3DS does not have; it says to honk instead, as the English does. The new words are the
 * same length (padded with spaces, which draw as nothing) so the text and its size are unchanged.
 */
void port_fix_game_text(char *text, int size) {
    static const struct {
        const char *from, *to;
    } sFixes[] = {
        { "APPUYANT SUR Z.", "KLAXONNANT.    " },
        { "APPUYEZ SUR Z", "KLAXONNEZ    " },
    };
    unsigned i;
    int at, k, length;

    for (i = 0; i < sizeof(sFixes) / sizeof(sFixes[0]); i++) {
        for (length = 0; sFixes[i].from[length] != '\0'; length++) {}
        for (at = 0; at + length <= size; at++) {
            for (k = 0; k < length && text[at + k] == sFixes[i].from[k]; k++) {}
            if (k == length) {
                for (k = 0; k < length; k++) {
                    text[at + k] = sFixes[i].to[k];
                }
                break;
            }
        }
    }
}
