#ifndef PORT_SWAP_H
#define PORT_SWAP_H

/*
 * Asset byte swapping for little endian targets (see port/src/game/asset_swap.c).
 * Game code calls these right after an asset has been loaded into RAM.
 */

#include <PR/ultratypes.h>
#include "port.h"

struct SoundData;
struct VehicleSoundAsset;
struct ObjectModel;
struct LevelHeader;
struct LevelModel;
struct ObjectHeader;
struct TextureHeader;
struct Sprite;

s32 port_swap_once(void *ptr);
void port_swap_once_forget(void *start, s32 size);

void port_swap_section(u32 section, void *data, s32 size);
void port_swap_misc_section(void *data, s32 size);
void port_swap_misc_assets(s32 *section, s32 *table, s32 count);
void port_swap_particles(void *data, s32 size);
void port_swap_particle_behaviours(void *data, s32 size);
void port_swap_jp_font_table(void *data, s32 size);

void port_swap_sound_table(struct SoundData *table, s32 count);
void port_swap_vehicle_sound(struct VehicleSoundAsset *asset);
void port_swap_seq_file(void *file, s32 loadedSize);
void port_swap_sequence(void *sequence);
void port_swap_bank_file(void *file);
void port_swap_bank(void *bank);
void port_swap_instrument(void *inst);
void port_swap_sound(void *sound);
void port_swap_envelope(void *env);
void port_swap_wavetable(void *wave);
void port_swap_adpcm_book(void *book);
void port_swap_adpcm_loop(void *loop);
void port_swap_raw_loop(void *loop);

void port_swap_level_header(void *header);
void port_swap_level_model(struct LevelModel *model);
void port_swap_object_model(struct ObjectModel *model);
void port_swap_object_animation(void *anim);
void port_swap_texture_header(struct TextureHeader *tex);
void port_swap_texture(struct TextureHeader *tex, s32 numberOfFrames);
void port_swap_sprite_asset(void *asset);
void port_swap_object_header(struct ObjectHeader *header);
void port_swap_object_map(s32 *map);

#endif
