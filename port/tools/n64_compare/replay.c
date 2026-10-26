/*
 * Replays one N64 frame captured by ares-headless through the port's renderer, so the two can be compared pixel for
 * pixel: the same display list, drawn by the real microcode and RDP on one side and by gfx_rdp.c and the software
 * renderer (host/gfx_soft.c) on the other.
 *
 *   replay <task_NNNNNN.rdram.gz> <sync_NNNNNN_SSSSSSS.fb> <out_prefix>
 *
 * The .rdram.gz file is RDRAM (big endian) at the start of the graphics task, followed by the 64 byte OSTask. The .fb
 * file is the colour image as the RDP left it at its full sync (big endian RGBA5551), followed by the image's address
 * and width. Writes <out_prefix>_n64.png and <out_prefix>_port.png. If the colour image isn't the one the display list
 * draws to, it says so on stderr (run_replays.sh then tries the next sync).
 *
 * The display list and everything it points to is in N64 byte order, while the interpreter reads native structures
 * through plain pointers, so before the replay each command's data is byte swapped by type (matrices, vertices,
 * triangles, viewports and the commands themselves; textures stay as they are, the interpreter reads them as bytes),
 * and every address is turned into a pointer into the copy of RDRAM. This needs a 32 bit build, like the host port.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

void port_gfx_run_task(void *taskPtr);
void gfxb_soft_save_png(const char *path);
int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

/* What gfx_rdp.c needs from the platform layer. */
volatile unsigned int gPlatCounters[64];
const char *volatile gPlatGfxStage;
volatile int gPlatSnapshotRequested;
void *plat_malloc(size_t size) {
    return malloc(size);
}
void plat_free(void *p) {
    free(p);
}
const char *plat_getenv(const char *name) {
    return getenv(name);
}
void plat_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
unsigned long long plat_time_ns(void) {
    return 0;
}
void prof_add(int stage, unsigned long long ns) {
    (void) stage;
    (void) ns;
}
void prof_count(int counter, unsigned int n) {
    (void) counter;
    (void) n;
}
unsigned long long plat_ticks(void) {
    return 0;
}
unsigned long long plat_ticks_per_second(void) {
    return 1000000000ULL;
}
int prof_enabled(void) {
    return 0;
}
void prof_part(int part, unsigned long long ns) {
    (void) part;
    (void) ns;
}

#define RDRAM_MAX (8 << 20)
#define DL_DEPTH 18

static u8 *sRam;     // RDRAM, converted in place where the interpreter reads typed data
static u8 *sDone;    // Per byte: already converted
static u32 sRamSize;
static u32 sSegments[16];
static int sCimgFound;
static u32 sCimg;

static u8 *load_dump(const char *path, u32 *size) {
    char cmd[600];
    FILE *f;
    u8 *buf = malloc(RDRAM_MAX + 64 + 4096);
    size_t n;
    snprintf(cmd, sizeof(cmd), "gzip -dc '%s'", path);
    f = popen(cmd, "r");
    if (f == NULL || buf == NULL) {
        fprintf(stderr, "can't read %s\n", path);
        exit(1);
    }
    n = fread(buf, 1, RDRAM_MAX + 64 + 4096, f);
    pclose(f);
    if (n < 64 + 4096) {
        fprintf(stderr, "%s is too short\n", path);
        exit(1);
    }
    *size = (u32) n - 64;
    return buf;
}

static u32 rd32(const u8 *p) {
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

static u32 phys(u32 addr) {
    return (sSegments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x00FFFFFF;
}

static u32 host_ptr(u32 addr) {
    return (u32) (uintptr_t) (sRam + phys(addr));
}

static void swap16(u32 p) {
    u8 t;
    if (p + 2 > sRamSize || sDone[p]) {
        return;
    }
    t = sRam[p];
    sRam[p] = sRam[p + 1];
    sRam[p + 1] = t;
    sDone[p] = sDone[p + 1] = 1;
}

static void swap32(u32 p) {
    u32 v;
    if (p + 4 > sRamSize || sDone[p]) {
        return;
    }
    v = rd32(sRam + p);
    memcpy(sRam + p, &v, 4);
    memset(sDone + p, 1, 4);
}

static void convert_dl(u32 addr, int maxCommands, int depth) {
    u32 p = phys(addr);
    int executed = 0;

    if (depth > DL_DEPTH) {
        return;
    }
    while (maxCommands < 0 || executed < maxCommands) {
        u32 w0, w1, op, a, i, k, count;
        if (p + 24 > sRamSize) {
            fprintf(stderr, "display list runs out of RDRAM at %08X\n", p);
            return;
        }
        if (sDone[p]) {
            // Already converted (a list called twice): only find where it ends.
            memcpy(&w0, sRam + p, 4);
            op = w0 >> 24;
            executed++;
            if (op == 0xE4 || op == 0xE5) {
                p += 16;
                executed += 2;
            }
            p += 8;
            if (op == 0xB8 || (op == 0x06 && ((w0 >> 16) & 1))) {
                return;
            }
            continue;
        }
        w0 = rd32(sRam + p);
        w1 = rd32(sRam + p + 4);
        op = w0 >> 24;
        a = w1;
        executed++;
        switch (op) {
            case 0x01: // G_MTX
                for (i = 0; i < 64; i += 4) {
                    swap32(phys(a) + i);
                }
                w1 = host_ptr(a);
                break;
            case 0x03: // G_MOVEMEM
                if (((w0 >> 16) & 0xFF) == 0x80) { // G_MV_VIEWPORT
                    for (i = 0; i < 16; i += 2) {
                        swap16(phys(a) + i);
                    }
                }
                w1 = host_ptr(a);
                break;
            case 0x04: // G_VTX: s16 x, y, z, u8 r, g, b, a
                count = ((w0 >> 19) & 0x1F) + 1;
                for (i = 0; i < count; i++) {
                    for (k = 0; k < 6; k += 2) {
                        swap16(phys(a) + i * 10 + k);
                    }
                }
                w1 = host_ptr(a);
                break;
            case 0x05: // G_TRIN: u8 flags, vi0, vi1, vi2, then three s16 u, v
                count = (w0 >> 4) & 0xFFF;
                for (i = 0; i < count; i++) {
                    for (k = 4; k < 16; k += 2) {
                        swap16(phys(a) + i * 16 + k);
                    }
                }
                w1 = host_ptr(a);
                break;
            case 0x06: // G_DL
                convert_dl(a, -1, depth + 1);
                w1 = host_ptr(a);
                break;
            case 0x07: // G_DMADL
                convert_dl(a, (w0 >> 16) & 0xFF, depth + 1);
                w1 = host_ptr(a);
                break;
            case 0xBC: // G_MOVEWORD
                if ((w0 & 0xFF) == 0x06) { // G_MW_SEGMENT
                    sSegments[(((w0 >> 8) & 0xFFFF) / 4) & 0xF] = w1 & 0x00FFFFFF;
                }
                break;
            case 0xFF: // G_SETCIMG
                if (!sCimgFound) {
                    sCimgFound = 1;
                    sCimg = phys(a);
                }
                w1 = host_ptr(a);
                break;
            case 0xFD: // G_SETTIMG
            case 0xFE: // G_SETZIMG
                w1 = host_ptr(a);
                break;
            default:
                break;
        }
        memcpy(sRam + p, &w0, 4);
        memcpy(sRam + p + 4, &w1, 4);
        memset(sDone + p, 1, 8);
        if (op == 0xE4 || op == 0xE5) { // Texture rectangles: two G_RDPHALF commands follow
            for (k = 1; k <= 2; k++) {
                swap32(p + k * 8);
                swap32(p + k * 8 + 4);
            }
            p += 16;
            executed += 2;
        }
        p += 8;
        if (op == 0xB8 || (op == 0x06 && ((w0 >> 16) & 1))) { // G_ENDDL, or a G_DL that doesn't return
            return;
        }
    }
}

int main(int argc, char **argv) {
    static u8 fbData[640 * 240 * 2 + 8];
    static u8 out[320 * 240 * 4];
    u32 task[16], dataPtr, cimg, width, x, y;
    char path[512];
    size_t n;
    FILE *fb;

    if (argc < 4) {
        fprintf(stderr, "usage: replay task.rdram.gz sync.fb out_prefix\n");
        return 1;
    }
    sRam = load_dump(argv[1], &sRamSize);
    sDone = calloc(sRamSize + 64, 1);
    dataPtr = rd32(sRam + sRamSize + 0x30); // OSTask.t.data_ptr

    convert_dl(dataPtr, -1, 0);
    if (!sCimgFound) {
        fprintf(stderr, "the display list sets no colour image\n");
        return 1;
    }

    fb = fopen(argv[2], "rb");
    if (fb == NULL) {
        fprintf(stderr, "can't read %s\n", argv[2]);
        return 1;
    }
    n = fread(fbData, 1, sizeof(fbData), fb);
    fclose(fb);
    memcpy(&cimg, fbData + n - 8, 4);
    memcpy(&width, fbData + n - 4, 4);
    if (cimg != sCimg) {
        fprintf(stderr, "note: RDP colour image %08X, display list's first %08X\n", cimg, sCimg);
    }
    for (y = 0; y < 240; y++) {
        for (x = 0; x < 320; x++) {
            u32 o = (y * width + x * width / 320) * 2;
            u16 c = fbData[o] << 8 | fbData[o + 1];
            u8 *d = out + (y * 320 + x) * 4;
            d[0] = ((c >> 11) & 31) * 255 / 31;
            d[1] = ((c >> 6) & 31) * 255 / 31;
            d[2] = ((c >> 1) & 31) * 255 / 31;
            d[3] = 255;
        }
    }
    snprintf(path, sizeof(path), "%s_n64.png", argv[3]);
    stbi_write_png(path, 320, 240, 4, out, 320 * 4);

    memset(task, 0, sizeof(task));
    task[0] = 1;                  // M_GFXTASK
    task[12] = host_ptr(dataPtr); // data_ptr
    port_gfx_run_task(task);
    snprintf(path, sizeof(path), "%s_port.png", argv[3]);
    gfxb_soft_save_png(path);
    return 0;
}
