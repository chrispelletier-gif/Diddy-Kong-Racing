#ifndef _COLLISION_H_
#define _COLLISION_H_

#include "types.h"
#include "structs.h"

#define MAX_COLLISION_CANDIDATES 500

/*
 * Collision candidates are either level segments or collision facets. On the N64 a segment is stored as a
 * physical address (MSB clear) and a facet as a KSEG0 pointer (MSB set), so the sign tells them apart.
 * Native ports have no KSEG0, so the MSB is set explicitly for facets.
 */
#ifdef TARGET_PORT
#define COLLISION_SEGMENT_ENTRY(seg) ((s32) (uintptr_t) (seg))
#define COLLISION_SEGMENT_PTR(entry) ((LevelModelSegment *) (uintptr_t) (entry))
#define COLLISION_FACET_ENTRY(facet) ((s32) ((uintptr_t) (facet) | 0x80000000))
#define COLLISION_FACET_PTR(entry) ((void *) ((uintptr_t) (entry) & 0x7FFFFFFF))
#else
#define COLLISION_SEGMENT_ENTRY(seg) ((s32) K0_TO_PHYS(seg))
#define COLLISION_SEGMENT_PTR(entry) ((LevelModelSegment *) PHYS_TO_K0(entry))
#define COLLISION_FACET_ENTRY(facet) ((s32) (facet))
#define COLLISION_FACET_PTR(entry) ((void *) (entry))
#endif

enum CollisionModes { COLLISION_MODE_DEFAULT, COLLISION_MODE_1, COLLISION_MODE_NO_WALLS };

s32 generate_collision_candidates(s32 numPoints, Vec3f *origins, Vec3f *targets, s32 vehicleID);
s32 compute_grid_overlap_mask(LevelModelSegmentBoundingBox *bbox, s32 x1, s32 z1, s32 x2, s32 z2);
s32 resolve_collisions(Vec3f *origin, Vec3f *target, f32 *radius, s8 *surface, s32 numEntries, s32 *numCollisions);

#endif
