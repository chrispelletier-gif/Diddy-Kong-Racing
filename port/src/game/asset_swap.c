/*
 * Converts game assets from the ROM's big endian byte order to the native (little endian) order as they are
 * loaded. Every function here is called from the place in the game code where that kind of asset enters RAM.
 *
 * A few kinds of data are deliberately left big endian because the code that reads them does so with explicit
 * byte loads (or a port replacement does):
 *  - texel and palette data (read by the renderer),
 *  - object animation keyframes (read by obj_animate, port/src/game/obj_asm.c),
 *  - ADPCM sample data and anything else that is only ever read a byte at a time.
 */
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "asset_enums.h"
#include "audio.h"
#include "audio_vehicle.h"
#include "font.h"
#include "port.h"
#include "port_swap.h"
#include "port_object_entries.h"

/******************************** Generic helpers ********************************/

#define SWAP16(field) port_swap16(&(field))
#define SWAP32(field) port_swap32(&(field))
#define MAX_BSP_STACK 128 // Levels have at most 128 segments (LEVEL_SEGMENT_MAX in tracks.c).

/*
 * Some data structures are shared between several owners (for example audio envelopes), so they must only be
 * swapped once. This keeps a list of every address swapped through it.
 */
#define MAX_SWAPPED_ONCE 4096
static void *sSwappedOnce[MAX_SWAPPED_ONCE];
static s32 sSwappedOnceCount;
static s32 sSwappedOnceFull;

s32 port_swap_once(void *ptr) {
    s32 i;
    for (i = 0; i < sSwappedOnceCount; i++) {
        if (sSwappedOnce[i] == ptr) {
            return FALSE;
        }
    }
    if (sSwappedOnceCount < MAX_SWAPPED_ONCE) {
        sSwappedOnce[sSwappedOnceCount++] = ptr;
    } else if (!sSwappedOnceFull) {
        // Past this, a shared structure seen again would be swapped twice.
        sSwappedOnceFull = TRUE;
        port_log("asset_swap: swap-once list full (%d): shared data may be swapped twice", MAX_SWAPPED_ONCE);
    }
    return TRUE;
}

void port_swap_once_forget(void *start, s32 size) {
    s32 i;
    for (i = 0; i < sSwappedOnceCount;) {
        if ((u8 *) sSwappedOnce[i] >= (u8 *) start && (u8 *) sSwappedOnce[i] < (u8 *) start + size) {
            sSwappedOnce[i] = sSwappedOnce[--sSwappedOnceCount];
        } else {
            i++;
        }
    }
}

/******************************** Whole sections ********************************/

static void swap_fonts(u8 *data, s32 size) {
    s32 count, i, j;
    u8 *font;

    port_swap32(data);
    count = *(s32 *) data;
    for (i = 0; i < count && (4 + (i + 1) * 0x400) <= size; i++) {
        font = data + 4 + i * 0x400;
        for (j = 0; j < 4; j++) {
            port_swap16(font + 0x20 + j * 2);
        }
        port_swap16_array(font + 0x40, 32);
    }
}

/* Called by asset_table_load for every section that gets loaded whole. */
void port_swap_section(u32 section, void *data, s32 size) {
    switch (section) {
        // Offset tables.
        case ASSET_AI_BEHAVIOUR_TABLE:
        case ASSET_TEXTURES_3D_TABLE:
        case ASSET_TEXTURES_2D_TABLE:
        case ASSET_GAME_TEXT_TABLE:
        case ASSET_MENU_TEXT_TABLE:
        case ASSET_SCREENS_TABLE:
        case ASSET_SPRITES_TABLE:
        case ASSET_MISC_TABLE:
        case ASSET_LEVEL_OBJECT_MAPS_TABLE:
        case ASSET_LEVEL_HEADERS_TABLE:
        case ASSET_LEVEL_NAMES_TABLE:
        case ASSET_LEVEL_MODELS_TABLE:
        case ASSET_OBJECT_MODELS_TABLE:
        case ASSET_OBJECT_ANIMATIONS_TABLE:
        case ASSET_OBJECT_HEADERS_TABLE:
        case ASSET_EMPTY_37_TABLE:
        case ASSET_AUDIO_TABLE:
        case ASSET_PARTICLES_TABLE:
        case ASSET_PARTICLE_BEHAVIORS_TABLE:
        case ASSET_WEATHER_PARTICLES:
            port_swap32_array(data, size / 4);
            break;
        case ASSET_TTGHOSTS_TABLE: {
            // { u8 mapId; u8 vehicleId; s32 ghostOffset; }
            s32 i;
            for (i = 0; i + 8 <= size; i += 8) {
                port_swap32((u8 *) data + i + 4);
            }
            break;
        }
        // Lists of 16 bit ids.
        case ASSET_HUD_ELEMENT_IDS:
        case ASSET_MENU_ELEMENT_IDS:
        case ASSET_ANIMATION_IDS:
        case ASSET_LEVEL_OBJECT_TRANSLATION_TABLE:
        case ASSET_DUMMY_PARTICLE_IDS:
            port_swap16_array(data, size / 2);
            break;
        case ASSET_FONTS:
            swap_fonts(data, size);
            break;
        case ASSET_JAPANESE_FONTS_TABLE:
            port_swap_jp_font_table(data, size);
            break;
        case ASSET_PARTICLES:
            port_swap_particles(data, size);
            break;
        case ASSET_PARTICLE_BEHAVIORS:
            port_swap_particle_behaviours(data, size);
            break;
        case ASSET_MISC:
            port_swap_misc_section(data, size);
            break;
        default:
            port_log("asset_table_load: no byte swapping rule for section %d", (int) section);
            break;
    }
}

/******************************** Audio ********************************/

void port_swap_sound_table(SoundData *table, s32 count) {
    s32 i;
    for (i = 0; i < count; i++) {
        SWAP16(table[i].soundBite);
        SWAP16(table[i].range);
    }
}

void port_swap_vehicle_sound(VehicleSoundAsset *asset) {
    s32 i, j;
    for (i = 0; i < 2; i++) {
        SWAP16(asset->soundId[i]);
        for (j = 0; j < 5; j++) {
            SWAP16(asset->pitchLevels[i][j]);
        }
    }
    SWAP16(asset->pitchLateralSpeedScale);
    SWAP16(asset->thrustPitchVel);
    SWAP16(asset->thrustPitchDecay);
    SWAP16(asset->rollAnglePitchScale);
    SWAP16(asset->pitchAnglePitchScale);
    SWAP16(asset->thrustPitchMax);
    SWAP16(asset->thrustPitchSpeedScale);
}

/* Swaps the header of a sequence file, and as many entries as were loaded. */
void port_swap_seq_file(void *fileData, s32 loadedSize) {
    ALSeqFile *file = fileData;
    s32 i, count;
    SWAP16(file->revision);
    SWAP16(file->seqCount);
    count = (loadedSize - 4) / (s32) sizeof(ALSeqData);
    if (count > file->seqCount) {
        count = file->seqCount;
    }
    for (i = 0; i < count; i++) {
        SWAP32(file->seqArray[i].offset);
        SWAP32(file->seqArray[i].len);
    }
}

/* Compressed MIDI sequences start with 16 track offsets and the division. The rest is a byte stream. */
void port_swap_sequence(void *sequence) {
    port_swap32_array(sequence, 17);
}

/* Called by bnkf.c the first time it visits each structure of a sound bank. */
void port_swap_bank_file(void *fileData) {
    ALBankFile *file = fileData;
    s32 i;
    SWAP16(file->revision);
    SWAP16(file->bankCount);
    for (i = 0; i < file->bankCount; i++) {
        SWAP32(file->bankArray[i]);
    }
}

void port_swap_bank(void *bankData) {
    ALBank *bank = bankData;
    s32 i;
    SWAP16(bank->instCount);
    SWAP32(bank->sampleRate);
    SWAP32(bank->percussion);
    for (i = 0; i < bank->instCount; i++) {
        SWAP32(bank->instArray[i]);
    }
}

void port_swap_instrument(void *instData) {
    ALInstrument *inst = instData;
    s32 i;
    SWAP16(inst->bendRange);
    SWAP16(inst->soundCount);
    for (i = 0; i < inst->soundCount; i++) {
        SWAP32(inst->soundArray[i]);
    }
}

/* Swaps the sound's own offsets. Called before they are converted to pointers. */
void port_swap_sound(void *soundData) {
    ALSound *sound = soundData;
    SWAP32(sound->envelope);
    SWAP32(sound->keyMap);
    SWAP32(sound->wavetable);
}

/* Envelopes can be shared between sounds. Called after the offset has become a pointer. */
void port_swap_envelope(void *envData) {
    ALEnvelope *env = envData;
    if (port_swap_once(env)) {
        SWAP32(env->attackTime);
        SWAP32(env->decayTime);
        SWAP32(env->releaseTime);
    }
}

void port_swap_wavetable(void *waveData) {
    ALWaveTable *wave = waveData;
    SWAP32(wave->base);
    SWAP32(wave->len);
    // The loop and book offsets are swapped here, their contents once they are pointers.
    SWAP32(wave->waveInfo.adpcmWave.loop);
    if (wave->type == AL_ADPCM_WAVE) {
        SWAP32(wave->waveInfo.adpcmWave.book);
    }
}

void port_swap_adpcm_book(void *bookData) {
    ALADPCMBook *book = bookData;
    if (port_swap_once(book)) {
        SWAP32(book->order);
        SWAP32(book->npredictors);
        port_swap16_array(book->book, book->order * book->npredictors * 8);
    }
}

void port_swap_adpcm_loop(void *loopData) {
    ALADPCMloop *loop = loopData;
    if (port_swap_once(loop)) {
        SWAP32(loop->start);
        SWAP32(loop->end);
        SWAP32(loop->count);
        port_swap16_array(loop->state, 16);
    }
}

void port_swap_raw_loop(void *loopData) {
    ALRawLoop *loop = loopData;
    if (port_swap_once(loop)) {
        SWAP32(loop->start);
        SWAP32(loop->end);
        SWAP32(loop->count);
    }
}

/******************************** Particles ********************************/

/* An array of 0x18 byte ParticleDescriptor structures. */
void port_swap_particles(void *data, s32 size) {
    u8 *p = (u8 *) data;
    s32 i;
    for (i = 0; i + 0x18 <= size; i += 0x18) {
        port_swap16_array(p + i + 0x02, 4); // flags, textureID, textureFrameStep, lifeTime
        port_swap16(p + i + 0x0A);          // lifeTimeRange
        port_swap16(p + i + 0x0E);          // opacityTimer
        port_swap32(p + i + 0x10);          // scale
    }
}

/* An array of 0xA0 byte ParticleBehaviour structures. */
void port_swap_particle_behaviours(void *data, s32 size) {
    u8 *p = (u8 *) data;
    s32 i;
    for (i = 0; i + 0xA0 <= size; i += 0xA0, p += 0xA0) {
        port_swap32_array(p + 0x00, 5);  // flags, emitterPos, sourceDistance
        port_swap16_array(p + 0x14, 14); // sourceRotation ... emissionDirAngularVelocity
        port_swap32_array(p + 0x30, 4);  // velocityModifier, emissionSpeed
        port_swap16_array(p + 0x40, 8);  // spawnInterval, burstCount, rotation, angularVelocity
        port_swap32_array(p + 0x50, 5);  // scale, scaleVelocity, movementParam, randomizationFlags, sourceDistanceRange
        port_swap16_array(p + 0x64, 6);  // sourceDirRange, emissionDirRange
        port_swap32_array(p + 0x70, 4);  // emissionSpeedRange, velocityModifierRange
        port_swap16_array(p + 0x80, 6);  // rotationRange, angularVelocityRange
        port_swap32_array(p + 0x8C, 3);  // scaleRange, scaleVelocityRange, movementParamRange
        port_swap32(p + 0x9C);           // colourLoop (misc asset index)
    }
}

/******************************** Misc section ********************************/

enum MiscSwapType { MISC_BYTES, MISC_HALFWORDS, MISC_WORDS, MISC_BOOST, MISC_SHIELD, MISC_COLOUR_CYCLE, MISC_PULSE, MISC_CHEATS };

/*
 * How each misc asset is read by the game (us.v77). Anything not listed is only read a byte at a time.
 * See get_misc_asset() callers, ObjectHeader->miscAssetIds, LevelHeader->unk74/pulseLightData and
 * ParticleBehaviour->colourLoop.
 */
static u8 misc_swap_type(s32 index) {
    switch (index) {
        case ASSET_MISC_4:
        case ASSET_MISC_8:
        case ASSET_MISC_RACER_WEIGHT:
        case ASSET_MISC_RACER_HANDLING:
        case ASSET_MISC_RACER_UNUSED_11:
        case ASSET_MISC_17:
        case ASSET_MISC_18:
        case ASSET_MISC_MAGNET_DATA:
        case ASSET_MISC_32:
        case ASSET_MISC_RACER_HITBOX_SIZE:
            return MISC_WORDS;
        case ASSET_MISC_RUMBLE_DATA:
        case ASSET_MISC_23:
        case ASSET_MISC_GHOST_UNLOCK_TIMES:
            return MISC_HALFWORDS;
        case ASSET_MISC_20:
            return MISC_BOOST;
        case ASSET_MISC_SHIELD_DATA:
            return MISC_SHIELD;
        case ASSET_MISC_64:
            return MISC_PULSE;
        case ASSET_MISC_MAGIC_CODES:
            return MISC_CHEATS;
    }
    // Racer acceleration curves and the per object float vectors (ObjectHeader->miscAssetIds[1]).
    if (index >= ASSET_MISC_RACERACCELERATION_UNKNOWN0 && index <= ASSET_MISC_RACERACCELERATION_UNKNOWN2) {
        return MISC_WORDS;
    }
    if (index >= ASSET_MISC_51 && index <= ASSET_MISC_55) {
        return MISC_WORDS;
    }
    // Colour cycles (LevelHeader_70 / ColorLoopEntry / MiscAssetObjectHeader24).
    if (index >= ASSET_MISC_57 && index <= ASSET_MISC_63) {
        return MISC_COLOUR_CYCLE;
    }
    return MISC_BYTES;
}

static void swap_misc_asset(s32 index, u8 *p, s32 size) {
    s32 i;
    switch (misc_swap_type(index)) {
        case MISC_WORDS:
            port_swap32_array(p, size / 4);
            break;
        case MISC_HALFWORDS:
            port_swap16_array(p, size / 2);
            break;
        case MISC_BOOST:
            // Object_Boost, 0x80 bytes each.
            for (i = 0; i + 0x80 <= size; i += 0x80) {
                port_swap32_array(p + i, 27);
                port_swap16_array(p + i + 0x6C, 2);
                port_swap32_array(p + i + 0x74, 3);
            }
            break;
        case MISC_SHIELD:
            // RacerShieldGfx: 4 s16 then 2 f32.
            for (i = 0; i + 16 <= size; i += 16) {
                port_swap16_array(p + i, 4);
                port_swap32_array(p + i + 8, 2);
            }
            break;
        case MISC_COLOUR_CYCLE:
            // s32 count, 3 s32 of state, two colours, then { s32 time; colour } entries.
            port_swap32_array(p, 4);
            for (i = 0x18; i + 4 <= size; i += 8) {
                port_swap32(p + i);
            }
            break;
        case MISC_PULSE:
            // PulsatingLightData: 4 u16, s32, then { u16 value; u16 time; } frames.
            port_swap16_array(p, 4);
            port_swap32(p + 8);
            port_swap16_array(p + 12, (size - 12) / 2);
            break;
        case MISC_CHEATS: {
            // u16 count, then { u16 codeOffset; u16 descriptionOffset; } entries, then the strings.
            s32 count;
            port_swap16(p);
            count = *(u16 *) p;
            port_swap16_array(p + 2, count * 2);
            break;
        }
        default:
            break;
    }
}

/* Called once the misc section and its table are loaded, and the cheats have been decrypted. */
void port_swap_misc_assets(s32 *section, s32 *table, s32 count) {
    s32 i;
    for (i = 0; i < count; i++) {
        swap_misc_asset(i, (u8 *) &section[table[i]], (table[i + 1] - table[i]) * 4);
    }
}

void port_swap_misc_section(void *data, s32 size) {
    // Swapped later by port_swap_misc_assets, once its table is loaded.
    (void) data;
    (void) size;
}

/******************************** Japanese fonts ********************************/

/* Only the Japanese version loads these: 0x10 byte headers that end with two u32 values. */
void port_swap_jp_font_table(void *data, s32 size) {
    u8 *p = (u8 *) data;
    s32 i;
    for (i = 0; i + 0x10 <= size; i += 0x10) {
        port_swap32(p + i + 8);
        port_swap32(p + i + 12);
    }
}

/******************************** Level headers ********************************/

void port_swap_level_header(void *header) {
    u8 *p = (u8 *) header;
    s32 i;
    static const u16 halfwords[] = { 0x34, 0x36, 0x38, 0x3A, 0x3C, 0x3E, 0x40, 0x42, 0x54, 0x5A, 0x5E, 0x60, 0x62,
                                     0x64, 0x66, 0x68, 0x6E, 0x90, 0x92, 0x96, 0x98, 0x9A, 0xA8, 0xAA, 0xB0, 0xBA };
    port_swap32(p + 0x08);           // course_height
    port_swap32_array(p + 0x74, 7);  // unk74 (misc asset indices)
    port_swap32(p + 0xA4);           // unkA4 (texture id)
    port_swap32(p + 0xAC);           // pulseLightData (misc asset index)
    for (i = 0; i < (s32) (sizeof(halfwords) / sizeof(halfwords[0])); i++) {
        port_swap16(p + halfwords[i]);
    }
}

/******************************** Models ********************************/

static void swap_vertices(Vertex *v, s32 count) {
    s32 i;
    for (i = 0; i < count; i++) {
        SWAP16(v[i].x);
        SWAP16(v[i].y);
        SWAP16(v[i].z);
    }
}

static void swap_triangles(Triangle *t, s32 count) {
    s32 i;
    for (i = 0; i < count; i++) {
        SWAP16(t[i].uv0.u);
        SWAP16(t[i].uv0.v);
        SWAP16(t[i].uv1.u);
        SWAP16(t[i].uv1.v);
        SWAP16(t[i].uv2.u);
        SWAP16(t[i].uv2.v);
    }
}

static void swap_batches(TriangleBatchInfo *b, s32 count) {
    s32 i;
    for (i = 0; i < count; i++) {
        SWAP16(b[i].verticesOffset);
        SWAP16(b[i].facesOffset);
        SWAP32(b[i].flags);
    }
}

static void swap_texture_infos(TextureInfo *t, s32 count) {
    s32 i;
    for (i = 0; i < count; i++) {
        SWAP32(t[i].texture);
    }
}

/* Called right after a level model has been decompressed, while its pointers are still file offsets. */
void port_swap_level_model(LevelModel *model) {
    u8 *base = (u8 *) model;
    LevelModelSegment *segments;
    s32 i, numSegments;

    SWAP32(model->textures);
    SWAP32(model->segments);
    SWAP32(model->segmentsBoundingBoxes);
    SWAP32(model->unkC);
    SWAP32(model->segmentsBitfields);
    SWAP32(model->segmentsBspTree);
    SWAP16(model->numberOfTextures);
    SWAP16(model->numberOfSegments);
    SWAP16(model->unk1C);
    SWAP16(model->numberOfAnimatedTextures);
    SWAP32(model->minimapSpriteIndex);
    SWAP16(model->minimapRotation);
    SWAP16(model->unk26);
    SWAP32(model->minimapXScale);
    SWAP32(model->minimapYScale);
    port_swap16_array(&model->minimapOffsetXAdv1, 4);
    SWAP32(model->minimapColor);
    port_swap16_array(&model->lowerXBounds, 6);
    SWAP32(model->modelSize);

    numSegments = model->numberOfSegments;
    swap_texture_infos((TextureInfo *) (base + (s32) model->textures), model->numberOfTextures);
    port_swap16_array(base + (s32) model->segmentsBoundingBoxes, numSegments * 6);
    // BSP tree nodes: { s16 left; s16 right; u8 type; u8 segment; s16 split; }. Each node splits the segments in two,
    // so there is one fewer node than segments and the visibility bits follow straight after. Only the nodes the tree
    // reaches from the root are swapped: swapping one per segment used to scramble the first visibility bytes, which
    // left a section of EverFrost Peak (and others) drawing no track at all.
    if (numSegments > 1) {
        BspTreeNode *nodes = (BspTreeNode *) (base + (s32) model->segmentsBspTree);
        s16 stack[MAX_BSP_STACK];
        s32 depth = 0, swapped = 0;
        stack[depth++] = 0;
        while (depth > 0 && swapped < numSegments - 1) {
            BspTreeNode *node = &nodes[stack[--depth]];
            SWAP16(node->leftNode);
            SWAP16(node->rightNode);
            SWAP16(node->splitValue);
            swapped++;
            if (node->leftNode > 0 && node->leftNode < numSegments - 1 && depth < MAX_BSP_STACK) {
                stack[depth++] = node->leftNode;
            }
            if (node->rightNode > 0 && node->rightNode < numSegments - 1 && depth < MAX_BSP_STACK) {
                stack[depth++] = node->rightNode;
            }
        }
    }

    segments = (LevelModelSegment *) (base + (s32) model->segments);
    for (i = 0; i < numSegments; i++) {
        LevelModelSegment *seg = &segments[i];
        SWAP32(seg->vertices);
        SWAP32(seg->triangles);
        SWAP32(seg->unk8);
        SWAP32(seg->batches);
        SWAP32(seg->unk10);
        SWAP32(seg->collisionFacets);
        SWAP32(seg->collisionPlanes);
        SWAP16(seg->numberOfVertices);
        SWAP16(seg->numberOfTriangles);
        SWAP16(seg->numberOfBatches);
        SWAP16(seg->unk28);
        SWAP32(seg->unk2C);
        SWAP16(seg->unk30);
        SWAP16(seg->unk32);
        SWAP32(seg->unk34);
        SWAP16(seg->unk38);
        SWAP32(seg->unk3C);
        swap_vertices((Vertex *) (base + (s32) seg->vertices), seg->numberOfVertices);
        swap_triangles((Triangle *) (base + (s32) seg->triangles), seg->numberOfTriangles);
        swap_batches((TriangleBatchInfo *) (base + (s32) seg->batches), seg->numberOfBatches + 1);
        // CollisionFacetPlanes: four u16 per triangle.
        port_swap16_array(base + (s32) seg->collisionFacets, seg->numberOfTriangles * 4);
    }
}

/* Called right after an object model has been decompressed, while its pointers are still file offsets. */
void port_swap_object_model(ObjectModel *model) {
    u8 *base = (u8 *) model;

    SWAP32(model->textures);
    SWAP32(model->vertices);
    SWAP32(model->triangles);
    SWAP32(model->collisionFacets);
    SWAP32(model->collisionPlanes);
    SWAP32(model->attachPoints);
    SWAP16(model->numberOfAttachPoints);
    SWAP16(model->unk1A);
    SWAP32(model->collisionSpheres);
    SWAP16(model->collisionSpheresSize);
    SWAP16(model->numberOfTextures);
    SWAP16(model->numberOfVertices);
    SWAP16(model->numberOfTriangles);
    SWAP16(model->numberOfBatches);
    SWAP32(model->fileSize);
    SWAP16(model->references);
    SWAP16(model->collisionFacetCount);
    SWAP32(model->batches);
    SWAP32(model->unk3C);
    SWAP32(model->normals);
    SWAP32(model->animations);
    SWAP16(model->numberOfAnimations);
    SWAP16(model->numberOfAnimatedVertices);
    SWAP32(model->animatedVertexIndices);
    SWAP16(model->hasAnimatedTexture);
    SWAP16(model->texOffsetUpdateRate);

    swap_texture_infos((TextureInfo *) (base + (s32) model->textures), model->numberOfTextures);
    swap_vertices((Vertex *) (base + (s32) model->vertices), model->numberOfVertices);
    swap_triangles((Triangle *) (base + (s32) model->triangles), model->numberOfTriangles);
    swap_batches((TriangleBatchInfo *) (base + (s32) model->batches), model->numberOfBatches + 1);
    port_swap16_array(base + (s32) model->attachPoints, model->numberOfAttachPoints);
    port_swap16_array(base + (s32) model->collisionSpheres, model->collisionSpheresSize);
    if ((s32) model->animatedVertexIndices != 0) {
        port_swap16_array(base + (s32) model->animatedVertexIndices, model->numberOfVertices);
    }
}

/* Object animations stay big endian apart from the leading keyframe count (see obj_animate). */
void port_swap_object_animation(void *anim) {
    port_swap32(anim);
}

/******************************** Textures and sprites ********************************/

/* Swaps the fields of a single texture header. The texel data itself stays big endian. */
void port_swap_texture_header(TextureHeader *tex) {
    SWAP16(tex->flags);
    SWAP16(tex->ciPaletteOffset);
    SWAP16(tex->numberOfCommands);
    SWAP32(tex->cmd);
    SWAP16(tex->numOfTextures);
    SWAP16(tex->frameAdvanceDelay);
    SWAP16(tex->textureSize);
}

/* Swaps every frame header of a freshly loaded (and decompressed) texture. */
void port_swap_texture(TextureHeader *tex, s32 numberOfFrames) {
    s32 i;
    for (i = 0; i < numberOfFrames; i++) {
        port_swap_texture_header(tex);
        tex = (TextureHeader *) ((u8 *) tex + tex->textureSize);
    }
}

void port_swap_sprite_asset(void *asset) {
    port_swap16_array(asset, 4); // baseTextureId, numberOfFrames, anchor
    port_swap32((u8 *) asset + 8);
}

/******************************** Objects ********************************/

extern u32 *gAssetsLookupTable;
extern s32 *gAssetsObjectHeadersTable;
extern s16 *gAssetsLvlObjTranslationTable;
extern s32 gAssetsLvlObjTranslationTableLength;

/* Reads bytes from an asset section straight out of the ROM. */
static void read_asset_bytes(u32 section, u32 offset, void *dst, u32 size) {
    port_rom_read(port_assets_lut_end() + gAssetsLookupTable[section + 1] + offset, dst, size);
}

void port_swap_object_header(ObjectHeader *header) {
    u8 *base = (u8 *) header;
    u8 *p;
    s32 i;

    port_swap32_array(base, 12);         // 0x00 - 0x2F
    port_swap16_array(base + 0x30, 5);  // 0x30 - 0x39
    // 0x3A - 0x3D are the shading colour and intensity bytes.
    port_swap16_array(base + 0x3E, 10); // 0x3E - 0x51
    port_swap32_array(base + (s32) header->modelIds, header->numberOfModelIds);
    port_swap32_array(base + (s32) header->vehiclePartIds, header->attachPointCount);
    // Particle entries are read as two words that get split with shifts.
    port_swap32_array(base + (s32) header->objectParticles, header->particleCount * 2);
    // Light sources (ObjectHeader24, 0x18 bytes each).
    for (i = 0; i < header->numLightSources; i++) {
        p = base + (s32) header->unk24 + i * 0x18;
        port_swap16(p + 6);
        port_swap16_array(p + 0xC, 6);
    }
}

/* Returns the behaviour of the object that a level object map entry spawns. */
static s32 object_entry_behaviour(u8 *entry) {
    s32 objType = entry[0] | ((entry[1] & 0x80) << 1);
    s32 headerIndex;
    u8 behaviour;

    if (objType > gAssetsLvlObjTranslationTableLength) {
        objType = 0;
    }
    headerIndex = gAssetsLvlObjTranslationTable[objType];
    read_asset_bytes(ASSET_OBJECTS, gAssetsObjectHeadersTable[headerIndex] + 0x54, &behaviour, 1);
    return behaviour & 0x7F;
}

/* Called right after a level object map has been decompressed. */
void port_swap_object_map(s32 *map) {
    u8 *entry, *end;
    const ObjectEntryLayout *layout;
    s32 i, size;

    port_swap32(map);
    entry = (u8 *) map + 16;
    end = entry + map[0];
    while (entry < end) {
        size = entry[1] & 0x7F;
        if (size == 0) {
            break;
        }
        layout = &gObjectEntryLayouts[object_entry_behaviour(entry)];
        if (layout->size != size) {
            // Unknown layout. At least swap the common position fields.
            port_log("object map: entry size %d does not match its behaviour (%d)", (int) size, (int) layout->size);
            port_swap16_array(entry + 2, 3);
        } else {
            for (i = 0; i < layout->num16; i++) {
                port_swap16(entry + layout->offsets16[i]);
            }
            for (i = 0; i < layout->num32; i++) {
                port_swap32(entry + layout->offsets32[i]);
            }
        }
        entry += size;
    }
}
