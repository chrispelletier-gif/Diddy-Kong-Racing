/*
 * The two-core renderer (roadmap P21, DKR_SPLIT_RENDER). The renderer has two halves of about the same size: the
 * display list walk (gfx_rdp.c: the RSP's vertex maths, texture decodes, batching) and the backend that turns its
 * draws into GPU commands (gfx_c3d.c on the 3DS). With the switch on, the walk queues its backend calls here instead
 * of making them, and a second renderer thread on the other core (plat.h PLAT_THREAD_RCP2) makes them in the same
 * order: the walk of frame N+1 runs while the backend builds frame N. An Old 3DS, whose one core held the whole
 * renderer and the game (bench #258: renderer 31 ms a frame), has the system core for the other half. On by default
 * there since switch bench #263 (Jungle Falls 21.0 to 29.0 fps, Crescent Island 20.0 to 29.1); off elsewhere.
 *
 * The queue is a byte ring of records, each a backend call with its arguments copied (the walk reuses its batch
 * buffers as soon as a call returns). One thread writes it (the walk's, sHead) and one reads it (sTail). The walk is
 * at most SPLIT_FRAMES_AHEAD frames ahead of the backend. Textures: an upload, share or free is a record like a draw,
 * so it reaches the backend in order; but a texture cache entry is reused as soon as it is freed, and draws still
 * queued point at it, so a free first waits until the backend has caught up (gfx_split_drain): frees are rare (level
 * loads and the cache's eviction), draws are not.
 *
 * Frames the walk must see drawn at once (snapshots, traced frames, the reference renderer's) are made directly,
 * after a drain; so is everything while the switch is off.
 */
#include <string.h>
#include "port.h"
#include "plat/plat.h"
#include "gfx_backend.h"
#include "gfx_split.h"

#define SPLIT_RING_BYTES (2u * 1024u * 1024u)
#define SPLIT_FRAMES_AHEAD 2

enum {
    OP_START,
    OP_END,
    OP_CLEAR_COLOR,
    OP_CLEAR_DEPTH,
    OP_TRIANGLES,
    OP_INDEXED,
    OP_TEX_UPLOAD,
    OP_TEX_FREE,
    OP_TEX_SHARE,
    OP_WRAP // The rest of the ring up to its end is unused: the next record is at the start.
};

typedef struct SplitRecord {
    unsigned int op, bytes; // bytes: of the whole record, header included, a multiple of 8
} SplitRecord;

typedef struct SplitClear {
    short rect[4];
    unsigned char rgba[4];
} SplitClear;

typedef struct SplitDraw {
    GfxDrawState state;
    int numVerts, numTriangles; // OP_TRIANGLES: numVerts is 3 * numTriangles and there are no indices
} SplitDraw;

typedef struct SplitTexture {
    GfxTexture *tex;
    const GfxTexture *src;
} SplitTexture;

static unsigned char *sRing;
static volatile unsigned int sHead, sTail; // Bytes written and read so far (they wrap together)
static PlatSem *sData;                      // A record (or a wake-up) for the backend thread
static PlatSem *sSpace;                     // The backend thread moved on while the walk waited for it
static PlatSem *sFrameSlots;                // Frames the walk may still start ahead of the backend
static volatile int sWalkWaiting;           // The walk waits on sSpace for room (or for the ring to empty)
static volatile int sStarted, sActive;      // The backend thread exists; this frame's calls are queued
static volatile int sBackendIdle;           // The backend thread caught up and waits (or is about to) on sData
static unsigned int sWakeBytes;             // Queued bytes that wake an idle backend thread (DKR_SPLIT_WAKE_KB)
static int sFailed;

/* The profiler's view (gfx_split_take_times): the walk's waits for the backend, the backend's busy time (frame start
 * to frame end, less its waits) and how often it was woken. Clocks are read only around waits, which block anyway. */
static unsigned long long sWalkWaitNs, sBackendBusyNs, sBackendFrameStart, sBackendWaitNs;
static unsigned int sBackendFrames, sBackendWakes;

static void backend_thread(void *arg);

static inline unsigned int round8(unsigned int n) {
    return (n + 7u) & ~7u;
}

/* Waits until the ring has room for bytes more (SPLIT_RING_BYTES: until it is empty). */
static void wait_for_room(unsigned int bytes) {
    while (SPLIT_RING_BYTES - (sHead - sTail) < bytes) {
        sWalkWaiting = 1;
        __sync_synchronize();
        if (SPLIT_RING_BYTES - (sHead - sTail) >= bytes) {
            sWalkWaiting = 0;
            break;
        }
        plat_sem_post(sData); // Makes sure the backend thread looks at the flag after this
        {
            unsigned long long start = plat_time_ns();
            plat_sem_wait(sSpace);
            sWalkWaitNs += plat_time_ns() - start;
        }
    }
}

/* Room for a record of payload bytes, at a position that doesn't wrap; its payload. */
static void *reserve(unsigned int op, unsigned int payload) {
    unsigned int bytes = sizeof(SplitRecord) + round8(payload);
    unsigned int at = sHead % SPLIT_RING_BYTES;
    SplitRecord *rec;
    if (at + bytes > SPLIT_RING_BYTES) {
        unsigned int rest = SPLIT_RING_BYTES - at;
        wait_for_room(rest);
        rec = (SplitRecord *) (sRing + at);
        rec->op = OP_WRAP;
        rec->bytes = rest;
        __sync_synchronize();
        sHead += rest;
        at = 0;
    }
    wait_for_room(bytes);
    rec = (SplitRecord *) (sRing + at);
    rec->op = op;
    rec->bytes = bytes;
    return rec + 1;
}

/*
 * Hands the record reserve gave over to the backend thread. Waking the backend is a system call on both cores, and
 * the backend, being quicker, would sleep and be woken for nearly every draw; so an idle backend is woken only once
 * sWakeBytes are queued, or at the frame's end (DKR_SPLIT_WAKE_KB=0 = every record, as before). The head is moved
 * before the flag is read, and the backend sets the flag before it looks at the head, so one of them sees the other.
 */
static void commit(void *payload) {
    SplitRecord *rec = (SplitRecord *) payload - 1;
    unsigned int op = rec->op;
    __sync_synchronize(); // The record is written before the head moves past it.
    sHead += rec->bytes;
    __sync_synchronize();
    if (sBackendIdle && (op == OP_END || sHead - sTail >= sWakeBytes)) {
        sBackendIdle = 0;
        plat_sem_post(sData);
    }
}

static void backend_thread(void *arg) {
    (void) arg;
    for (;;) {
        sBackendIdle = 1;
        __sync_synchronize(); // The flag is set before the head is looked at (see commit).
        if (sTail == sHead) {
            unsigned long long start = plat_time_ns();
            plat_sem_wait(sData);
            sBackendWaitNs += plat_time_ns() - start;
            sBackendWakes++;
        }
        sBackendIdle = 0;
        while (sTail != sHead) {
            SplitRecord *rec;
            unsigned int op, bytes;
            void *p;
            __sync_synchronize(); // The record is read after the head that covers it.
            rec = (SplitRecord *) (sRing + sTail % SPLIT_RING_BYTES);
            op = rec->op;
            bytes = rec->bytes;
            p = rec + 1;
            switch (op) {
                case OP_START:
                    sBackendFrameStart = plat_time_ns();
                    sBackendWaitNs = 0;
                    gfxb_start_frame();
                    break;
                case OP_END: {
                    unsigned long long now;
                    plat_set_frame_step(*(int *) p);
                    gfxb_end_frame();
                    now = plat_time_ns();
                    sBackendBusyNs += now - sBackendFrameStart - sBackendWaitNs;
                    sBackendFrames++;
                    break;
                }
                case OP_CLEAR_COLOR: {
                    SplitClear *c = p;
                    gfxb_clear_color(c->rect, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                    break;
                }
                case OP_CLEAR_DEPTH:
                    gfxb_clear_depth(((SplitClear *) p)->rect);
                    break;
                case OP_TRIANGLES:
                case OP_INDEXED: {
                    SplitDraw *d = p;
                    const GfxVertex *verts = (const GfxVertex *) (d + 1);
                    if (op == OP_TRIANGLES) {
                        gfxb_draw_triangles(&d->state, verts, d->numTriangles);
                    } else {
                        gfxb_draw_indexed(&d->state, verts, d->numVerts, (const unsigned short *) (verts + d->numVerts),
                                          d->numTriangles);
                    }
                    break;
                }
                case OP_TEX_UPLOAD:
                    gfxb_texture_upload(((SplitTexture *) p)->tex);
                    break;
                case OP_TEX_FREE:
                    gfxb_texture_free(((SplitTexture *) p)->tex);
                    break;
                case OP_TEX_SHARE:
                    gfxb_texture_share(((SplitTexture *) p)->tex, ((SplitTexture *) p)->src);
                    break;
                default:
                    break;
            }
            // Done with the record before the walk may write over it (op and bytes were read before, as locals).
            __sync_synchronize();
            sTail += bytes;
            if (op == OP_END) {
                plat_sem_post(sFrameSlots);
            }
            if (sWalkWaiting) {
                sWalkWaiting = 0;
                plat_sem_post(sSpace);
            }
        }
        if (sWalkWaiting) {
            sWalkWaiting = 0;
            plat_sem_post(sSpace);
        }
    }
}

/* Starts the backend thread the first time the switch is on; FALSE if it can't (then everything stays direct). */
static int start_backend(void) {
    if (sStarted) {
        return TRUE;
    }
    if (sFailed) {
        return FALSE;
    }
    sRing = plat_malloc(SPLIT_RING_BYTES);
    if (sRing == NULL) {
        plat_log("two-core renderer: no memory for its queue");
        sFailed = TRUE;
        return FALSE;
    }
    sData = plat_sem_create(0);
    sSpace = plat_sem_create(0);
    sFrameSlots = plat_sem_create(SPLIT_FRAMES_AHEAD);
    plat_thread_create(backend_thread, NULL, 0, PLAT_THREAD_RCP2, "rdp2");
    sStarted = TRUE;
    return TRUE;
}

void gfx_split_drain(void) {
    if (sStarted && sHead != sTail) {
        wait_for_room(SPLIT_RING_BYTES);
    }
}

int gfx_split_frame_start(int direct) {
    int want = !direct && plat_switch("DKR_SPLIT_RENDER", plat_old_3ds()) != 0;
    if (want && !sActive) {
        // The other core's share comes first: without it the second thread can't be made there.
        plat_split_render_cores(TRUE);
        want = start_backend();
        if (!want) {
            plat_split_render_cores(FALSE);
        }
    }
    if (!want) {
        gfx_split_drain();
        if (sActive) {
            plat_split_render_cores(FALSE);
        }
        sActive = FALSE;
        gfxb_start_frame();
        return FALSE;
    }
    if (!sActive) {
        plat_log("two-core renderer: on (the walk and the GPU commands on separate cores)");
    }
    sActive = TRUE;
    sWakeBytes = (unsigned int) plat_switch("DKR_SPLIT_WAKE_KB", 16) * 1024u;
    if (!plat_sem_try_wait(sFrameSlots)) {
        unsigned long long start = plat_time_ns();
        plat_sem_wait(sFrameSlots);
        sWalkWaitNs += plat_time_ns() - start;
    }
    commit(reserve(OP_START, 0));
    return TRUE;
}

void gfx_split_frame_end(void) {
    int *step;
    if (!sActive) {
        gfxb_end_frame();
        return;
    }
    step = reserve(OP_END, sizeof(int));
    *step = plat_frame_step();
    commit(step);
}

int gfx_split_active(void) {
    return sActive;
}

void gfx_split_take_times(unsigned long long *walkWaitNs, unsigned long long *backendBusyNs,
                          unsigned int *backendFrames, unsigned int *backendWakes) {
    *walkWaitNs = sWalkWaitNs;
    *backendBusyNs = sBackendBusyNs;
    *backendFrames = sBackendFrames;
    *backendWakes = sBackendWakes;
    sWalkWaitNs = sBackendBusyNs = 0; // Read without a lock: a frame caught mid-update is off by a little
    sBackendFrames = sBackendWakes = 0;
}

void gfx_split_clear_color(const short rect[4], unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    SplitClear *c;
    if (!sActive) {
        gfxb_clear_color(rect, r, g, b, a);
        return;
    }
    c = reserve(OP_CLEAR_COLOR, sizeof(SplitClear));
    memcpy(c->rect, rect, sizeof(c->rect));
    c->rgba[0] = r;
    c->rgba[1] = g;
    c->rgba[2] = b;
    c->rgba[3] = a;
    commit(c);
}

void gfx_split_clear_depth(const short rect[4]) {
    SplitClear *c;
    if (!sActive) {
        gfxb_clear_depth(rect);
        return;
    }
    c = reserve(OP_CLEAR_DEPTH, sizeof(SplitClear));
    memcpy(c->rect, rect, sizeof(c->rect));
    commit(c);
}

void gfx_split_draw_triangles(const GfxDrawState *state, const GfxVertex *verts, int numTriangles) {
    SplitDraw *d;
    unsigned int vertBytes = (unsigned int) numTriangles * 3u * sizeof(GfxVertex);
    if (!sActive || sizeof(SplitDraw) + vertBytes + 64u > SPLIT_RING_BYTES / 4u) {
        gfx_split_drain();
        gfxb_draw_triangles(state, verts, numTriangles);
        return;
    }
    d = reserve(OP_TRIANGLES, sizeof(SplitDraw) + vertBytes);
    d->state = *state;
    d->numVerts = numTriangles * 3;
    d->numTriangles = numTriangles;
    memcpy(d + 1, verts, vertBytes);
    commit(d);
}

void gfx_split_draw_indexed(const GfxDrawState *state, const GfxVertex *verts, int numVerts,
                            const unsigned short *indices, int numTriangles) {
    SplitDraw *d;
    unsigned int vertBytes = (unsigned int) numVerts * sizeof(GfxVertex);
    unsigned int indexBytes = (unsigned int) numTriangles * 3u * sizeof(unsigned short);
    if (!sActive || sizeof(SplitDraw) + vertBytes + indexBytes + 64u > SPLIT_RING_BYTES / 4u) {
        gfx_split_drain();
        gfxb_draw_indexed(state, verts, numVerts, indices, numTriangles);
        return;
    }
    d = reserve(OP_INDEXED, sizeof(SplitDraw) + vertBytes + indexBytes);
    d->state = *state;
    d->numVerts = numVerts;
    d->numTriangles = numTriangles;
    memcpy(d + 1, verts, vertBytes);
    memcpy((unsigned char *) (d + 1) + vertBytes, indices, indexBytes);
    commit(d);
}

static void texture_op(unsigned int op, GfxTexture *tex, const GfxTexture *src) {
    SplitTexture *t = reserve(op, sizeof(SplitTexture));
    t->tex = tex;
    t->src = src;
    commit(t);
}

void gfx_split_texture_upload(GfxTexture *tex) {
    if (!sActive) {
        gfxb_texture_upload(tex);
        return;
    }
    texture_op(OP_TEX_UPLOAD, tex, NULL);
}

void gfx_split_texture_share(GfxTexture *dst, const GfxTexture *src) {
    if (!sActive) {
        gfxb_texture_share(dst, src);
        return;
    }
    texture_op(OP_TEX_SHARE, dst, src);
}

void gfx_split_texture_free(GfxTexture *tex) {
    if (!sActive) {
        gfxb_texture_free(tex);
        return;
    }
    // Draws still queued may point at the texture, whose cache entry is reused once this returns: the backend
    // catches up first, then the free is made here, with the backend idle.
    gfx_split_drain();
    gfxb_texture_free(tex);
}
