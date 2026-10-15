/*
 * C translations of the hand written MIPS routines in src/hasm/obj_animate.s and src/hasm/obj_shade_fast.s.
 * They follow the assembly instruction by instruction, including its integer wrap-around behaviour.
 *
 * Object animation data is kept in the ROM's big endian byte order (see port/src/game/asset_swap.c), so it is
 * read with explicit big endian loads here.
 */
#include <ultra64.h>
#include "types.h"
#include "structs.h"
#include "objects.h"
#include "camera.h"
#include "math_util.h"
#include "port.h"

extern s32 D_8011D644; // Scratch buffer of interpolated animation offsets, 3 s16 per animated vertex.

#define ANIM_S16(p) ((s16) port_read_be16(p))

/* Writes a vertex colour the way the assembly does: two big endian halfword stores. */
static inline void store_shade(Vertex *v, s32 value) {
    u16 first = (u16) (value | (value << 8));
    u16 second = (u16) (first | 0xFF);
    v->r = first >> 8;
    v->g = first & 0xFF;
    v->b = second >> 8;
    v->a = second & 0xFF;
}

/**
 * Updates the vertices of an animated object model for the object's current animation and frame.
 * Returns 1 if the vertices were updated, 0 if nothing changed.
 */
s32 obj_animate(Object *obj) {
    s32 modelIndex;
    ModelInstance *inst;
    ObjectModel *model;
    s32 frame;   // t9
    s32 animID;  // a3
    s32 keyframe; // a0
    s32 lastLength; // t2
    s32 prevKeyframe; // s0
    s32 frac; // t8
    s32 stride; // t4
    s32 numAnimVerts;
    s16 *work; // s1
    s16 *deltas; // D_8011D644
    s16 *vertIndices;
    s16 *vertIndicesEnd;
    Vertex *baseVerts;
    Vertex *outVerts;
    u8 *anim;
    u8 *src;
    s16 *dst;
    s16 *dstEnd;
    s32 x, y, z, tilt;
    s32 idx;

    modelIndex = obj->modelIndex;
    if (modelIndex < 0) {
        modelIndex = 0;
    }
    if (!(modelIndex < obj->header->numberOfModelIds)) {
        modelIndex = obj->header->numberOfModelIds;
    }
    inst = obj->modelInstances[modelIndex];
    model = inst->objModel;
    if (model->animations == NULL) {
        return 0;
    }
    frame = obj->animFrame;
    animID = obj->animationID;
    if (frame == inst->animationFrameCount && animID == inst->animationID) {
        return 0;
    }
    if (animID < 0) {
        animID = 0;
    }
    if (!(animID < model->numberOfAnimations)) {
        animID = model->numberOfAnimations - 1;
    }
    lastLength = 0;
    if (model->numberOfAnimations > 0) {
        lastLength = model->animations[animID].animLength - 2;
    }
    keyframe = (u32) frame >> 4;
    if (lastLength < keyframe) {
        frame = 0;
        keyframe = 0;
        inst->animationID = -1;
    }
    work = (s16 *) inst->vertices[2];
    if (animID == inst->animationID) {
        prevKeyframe = inst->animationFrame;
    } else {
        prevKeyframe = -1;
    }
    inst->animationID = animID;
    inst->animationFrameCount = frame;
    inst->animationFrame = keyframe;
    frac = frame & 0xF;
    anim = model->animations[animID].animData;
    numAnimVerts = model->numberOfAnimatedVertices;
    vertIndices = (s16 *) model->animatedVertexIndices;
    vertIndicesEnd = vertIndices + model->numberOfVertices;

    if (keyframe == 0 || prevKeyframe == -1) {
        // Rebuild the work buffer from the first keyframe's absolute positions.
        u8 *positions = anim + 0xC;
        s16 *indexPtr = vertIndices;
        baseVerts = model->vertices;
        while (indexPtr < vertIndicesEnd) {
            idx = *indexPtr;
            if (idx != -1) {
                dst = work + idx * 3;
                src = positions + idx * 6;
                dst[0] = baseVerts->x + ANIM_S16(src + 0);
                dst[1] = baseVerts->y + ANIM_S16(src + 2);
                dst[2] = baseVerts->z + ANIM_S16(src + 4);
            }
            indexPtr++;
            baseVerts++;
        }
        prevKeyframe = 0;
    }
    stride = numAnimVerts * 3 + 12;

    // Step forwards through the keyframe deltas.
    if (prevKeyframe < keyframe) {
        u8 *frameData = anim + stride * (prevKeyframe + 2);
        do {
            src = frameData;
            frameData += stride;
            dst = work;
            dstEnd = work + numAnimVerts * 3;
            do {
                dst[0] += (s8) src[0];
                dst[1] += (s8) src[1];
                dst[2] += (s8) src[2];
                dst += 3;
                src += 3;
            } while (dst < dstEnd);
            prevKeyframe++;
        } while (prevKeyframe < keyframe);
    }
    // Or backwards.
    if (keyframe < prevKeyframe) {
        u8 *frameData = anim + stride * (prevKeyframe + 2);
        do {
            frameData -= stride;
            prevKeyframe--;
            src = frameData;
            dst = work;
            dstEnd = work + numAnimVerts * 3;
            do {
                dst[0] -= (s8) src[0];
                dst[1] -= (s8) src[1];
                dst[2] -= (s8) src[2];
                dst += 3;
                src += 3;
            } while (dst < dstEnd);
        } while (keyframe < prevKeyframe);
    }

    // Interpolate towards the next keyframe.
    src = anim + stride * (prevKeyframe + 2);
    deltas = (s16 *) D_8011D644;
    dst = deltas;
    dstEnd = deltas + numAnimVerts * 3;
    do {
        dst[0] = (s16) ((u32) ((s8) src[0] * frac) >> 4);
        dst[1] = (s16) ((u32) ((s8) src[1] * frac) >> 4);
        dst[2] = (s16) ((u32) ((s8) src[2] * frac) >> 4);
        dst += 3;
        src += 3;
    } while (dst < dstEnd);

    // Interpolate the model offset and head tilt stored in each keyframe's header.
    src = anim;
    if (keyframe != 0) {
        src = anim + stride * (keyframe + 1) - 12;
    }
    x = ANIM_S16(src + 0);
    y = ANIM_S16(src + 2);
    z = ANIM_S16(src + 4);
    tilt = ANIM_S16(src + 0xA);
    src += stride;
    if (keyframe == 0) {
        src += stride - 12;
    }
    inst->offsetX = x + (s32) ((u32) ((ANIM_S16(src + 0) - x) * frac) >> 4);
    inst->offsetY = y + (s32) ((u32) ((ANIM_S16(src + 2) - y) * frac) >> 4);
    inst->offsetZ = z + (s32) ((u32) ((ANIM_S16(src + 4) - z) * frac) >> 4);
    inst->headTilt = tilt + (s32) ((u32) ((ANIM_S16(src + 0xA) - tilt) * frac) >> 4);

    // Write the final vertex positions into the other vertex buffer.
    inst->animationTaskNum ^= 1;
    outVerts = inst->vertices[(u8) inst->animationTaskNum];
    while (vertIndices < vertIndicesEnd) {
        idx = *vertIndices++;
        if (idx != -1) {
            s16 *a = work + idx * 3;
            s16 *b = deltas + idx * 3;
            outVerts->x = a[0] + b[0];
            outVerts->y = a[1] + b[1];
            outVerts->z = a[2] + b[2];
        }
        outVerts++;
    }
    return 1;
}

/**
 * Applies simple directional shading to an object's vertex colours.
 */
void obj_shade_fast(ObjectModel *model, Object *obj, f32 intensity) {
    ShadeProperties *shading = obj->shading;
    TriangleBatchInfo *batch, *batchEnd;
    Vec3s *normals;
    Vertex *verts;
    s32 base, dirX, dirY, dirZ, count, dot, value;

    if (shading == NULL) {
        return;
    }
    base = (s32) (shading->unk0 * intensity * 160.0f);
    dirX = shading->shadowDirX;
    dirY = shading->shadowDirY;
    dirZ = shading->shadowDirZ;
    normals = model->normals;
    verts = obj->curVertData;
    batch = model->batches;
    batchEnd = batch + model->numberOfBatches;
    do {
        count = batch[1].verticesOffset - batch->verticesOffset;
        if (batch->miscData == BATCH_VTX_COL) {
            verts += count;
            if (batch->flags & 0x8000) {
                normals += count;
            }
        } else {
            while (count-- > 0) {
                dot = (normals->x * dirX + normals->y * dirY + normals->z * dirZ) >> 11;
                normals++;
                if (dot <= 0) {
                    value = base;
                } else {
                    value = ((u32) (dot * base) >> 16) + base;
                    if (value >= 0x100) {
                        value = 0xFF;
                    }
                }
                store_shade(verts, value);
                verts++;
            }
        }
        batch++;
    } while (batch < batchEnd);
}

/**
 * Lights an object's vertices using the shadow direction relative to the object's rotation.
 */
void calc_dynamic_lighting_for_object_2(Object *obj, ObjectModel *model, s16 useProjection, f32 intensity) {
    ShadeProperties *shading = obj->shading;
    TriangleBatchInfo *batch, *batchEnd;
    ObjectTransform trans;
    MtxF mtx;
    Vec3f dir;
    Vec3s *normals;
    Vertex *verts;
    f32 scale;
    s32 ambient, diffuse, dirX, dirY, dirZ, count, dot, value;

    if (shading == NULL) {
        return;
    }
    dir.f[0] = (f32) (shading->shadowDirX << 2);
    dir.f[1] = (f32) (shading->shadowDirY << 2);
    dir.f[2] = (f32) (shading->shadowDirZ << 2);
    if (useProjection) {
        mtxf_transform_dir(get_projection_matrix_f32(), &dir, &dir);
    }
    trans.rotation.x_rotation = -obj->trans.rotation.x_rotation;
    trans.rotation.y_rotation = -obj->trans.rotation.y_rotation;
    trans.rotation.z_rotation = -obj->trans.rotation.z_rotation;
    trans.scale = 1.0f;
    trans.x_position = 0.0f;
    trans.y_position = 0.0f;
    trans.z_position = 0.0f;
    mtxf_from_inverse_transform(&mtx, &trans);
    mtxf_transform_dir(&mtx, &dir, &dir);

    scale = shading->unk0 * intensity * 255.0f;
    ambient = (s32) (shading->ambient * scale);
    diffuse = (s32) (shading->diffuse * scale);
    dirX = (s32) dir.f[0];
    dirY = (s32) dir.f[1];
    dirZ = (s32) dir.f[2];
    normals = model->normals;
    verts = obj->curVertData;
    batch = model->batches;
    batchEnd = batch + model->numberOfBatches;
    do {
        count = batch[1].verticesOffset - batch->verticesOffset;
        if (batch->miscData == BATCH_VTX_COL) {
            verts += count;
            if (batch->flags & 0x8000) {
                normals += count;
            }
        } else {
            while (count-- > 0) {
                dot = (normals->x * dirX + normals->y * dirY + normals->z * dirZ) >> 7;
                normals++;
                if (dot <= 0) {
                    value = ambient;
                } else {
                    value = ((u32) (dot * diffuse) >> 21) + ambient;
                    if (value >= 0x100) {
                        value = 0xFF;
                    }
                }
                store_shade(verts, value);
                verts++;
            }
        }
        batch++;
    } while (batch < batchEnd);
}
