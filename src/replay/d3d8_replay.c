/*
 * d3d8_replay.c -- replay one captured frame into the host renderer.
 *
 * The point of the whole capture path: change the shader translation, the
 * combiners or the state handling in src/d3d, rebuild, and look at the new
 * image in seconds instead of running the title for five minutes and driving
 * its menus to the moment of interest. A capture replays byte-identically
 * every time, so two images differ only where the code changed.
 *
 *   d3d8_replay <capture> [--out <prefix>] [--loops <n>] [--dump-every]
 *               [--hold] [--quiet]
 *
 *   --out <prefix>   BMP path prefix (default "replay"); files are
 *                    <prefix>NNN.bmp, in the same 24-bit format the shadow
 *                    path's RECOMP_HLE_D3D8_DUMP writes, so a replay image
 *                    and a live frame can be put side by side.
 *   --loops <n>      replay the frame n times (default 1). The device is
 *                    created once; each loop walks the capture again from
 *                    its snapshot.
 *   --dump-every     write a BMP after every loop, not just the last, so a
 *                    frame that is not idempotent shows itself.
 *   --hold           leave the window up until it is closed.
 *   --quiet          only errors.
 *   --no-combiners   draw with the fixed-function pixel path whatever the
 *                    capture's combiner token says.
 *   --draws <n>      execute only the first n draws (state still applied;
 *                    clears after the nth draw are skipped too).
 *   --skip-draw <n>[-<m>]  leave out draw n, or draws n..m (0-based).
 *   --list-draws     print every draw with the state it runs under.
 *
 * A player, not an emulator. A capture holds the calls shadow mode
 * made on the host renderer after all of its Xbox conversion (d3d8_capture.h),
 * so this file has no Xbox knowledge at all: each chunk is one call into
 * src/d3d with the recorded arguments. The only translation is of handles --
 * vertex program handles and texture objects are this process's own, so the
 * recorded ones are mapped to them. If a replayed frame differs from the live
 * one, the difference is in the capture, or in what src/d3d does with the
 * same calls; it cannot come from a second copy of shadow mode's logic.
 *
 * No game files, no recompiled title and no guest memory. It links the host
 * D3D8 layer (src/d3d) directly and creates the device the same way
 * hle_d3d8.c's shadow_create does.
 *
 * Between loops every program, texture and depth surface this tool created
 * is deleted and the back buffer is made the target again, so each loop
 * starts from the capture's snapshot alone. What the snapshot does
 * not carry (d3d8_capture.h, "Not in the format") stays as the device left
 * it, which is the device's default because nothing here sets it either.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_xbox.h"
#include "d3d8_internal.h"
#include "d3d8_vsh.h"
#include "d3d8_combiners.h"
#include "d3d8_capture.h"

#define REPLAY_MAX_PROGRAMS 256
#define REPLAY_STAGES       4
#define REPLAY_MAX_DEPTHS   16   /* hle_d3d8_record.c CAPTURE_MAX_DEPTHS */

static int g_quiet;
/* --no-combiners: ignore the recorded combiner token, so the frame draws with
 * the host's fixed-function pixel path. A capture taken with the title's pixel
 * shaders forwarded (the default; RECOMP_HLE_D3D8_PS=0 turns it off) holds
 * both the combiner states and the token, so one frame can be drawn both ways
 * and compared -- which runs of the title cannot do, since no two land on the
 * same moment. */
static int g_no_combiners;
/* Draw-level bisection: --draws N executes only the first N draws, --skip-draw
 * N (or N-M) leaves out draw N (0-based) or that range, --list-draws prints each draw with the state
 * it runs under. State chunks always run, so a draw left out changes nothing
 * but its own pixels. */
static long g_max_draws = -1;
static long g_skip_draw = -1, g_skip_last = -1;
static int  g_list_draws;
static long g_draw_index;           /* draws seen in this loop */
static DWORD g_cur_vs, g_cur_token;
#define LIST_TEX_IDS 8192
static struct { uint32_t format, width, height; } g_tex_info[LIST_TEX_IDS];
static uint32_t g_stage_tex[2];

static void tex_desc(char *buf, size_t n, uint32_t id)
{
    if (!id)
        snprintf(buf, n, "-");
    else if (id < LIST_TEX_IDS)
        snprintf(buf, n, "%u(%ux%u f%02X)", id, g_tex_info[id].width,
                 g_tex_info[id].height, g_tex_info[id].format);
    else
        snprintf(buf, n, "%u", id);
}

/* 1 if this draw should run, after listing it if asked. */
/* REPLAY_NO_CULL=1 / REPLAY_NO_Z=1: draw every triangle whatever its winding,
 * or whatever is in front of it, to tell missing geometry from hidden geometry. */
static IDirect3DDevice8 *g_dev;

static int draw_gate(const char *kind, uint32_t prim, uint32_t count, uint32_t stride)
{
    long n = g_draw_index++;
    if (g_dev && getenv("REPLAY_NO_CULL"))
        g_dev->lpVtbl->SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    if (g_dev && getenv("REPLAY_NO_Z"))
        g_dev->lpVtbl->SetRenderState(g_dev, D3DRS_ZFUNC, D3DCMP_ALWAYS);

    if (g_list_draws) {
        const DWORD *rs = d3d8_GetRenderStates();
        char t0[48], t1[48];

        tex_desc(t0, sizeof t0, g_stage_tex[0]);
        tex_desc(t1, sizeof t1, g_stage_tex[1]);
        fprintf(stderr, "[draw %4ld] tex %s %s\n", n, t0, t1);
        fprintf(stderr, "[draw %4ld] %-8s prim %u x%-5u stride %2u  vs 0x%05lX  ps %lu  "
                "blend %lu %lu>%lu  atest %lu ref %lu  z %lu/%lu  fog %lu  cull %lu  prog %08X%s%s\n",
                n, kind, prim, count, stride, (unsigned long)g_cur_vs,
                (unsigned long)(g_no_combiners ? 0 : g_cur_token),
                (unsigned long)rs[D3DRS_ALPHABLENDENABLE], (unsigned long)rs[D3DRS_SRCBLEND],
                (unsigned long)rs[D3DRS_DESTBLEND], (unsigned long)rs[D3DRS_ALPHATESTENABLE],
                (unsigned long)rs[D3DRS_ALPHAREF], (unsigned long)rs[D3DRS_ZENABLE],
                (unsigned long)rs[D3DRS_ZWRITEENABLE], (unsigned long)rs[D3DRS_FOGENABLE],
                (unsigned long)rs[D3DRS_CULLMODE], d3d8_vsh_slot_hash(g_cur_vs),
                (g_max_draws >= 0 && n >= g_max_draws) ? "  (not drawn: --draws)" : "",
                (n >= g_skip_draw && n <= g_skip_last) ? "  (not drawn: --skip-draw)" : "");
        /* REPLAY_SHOW_CONSTANTS=<first>[,<count>]: those registers as this
         * draw sees them (default four: one matrix). */
        if (getenv("REPLAY_SHOW_CONSTANTS")) {
            const float *c = d3d8_vsh_constants();
            unsigned first = 0, cnt = 4, r;
            sscanf(getenv("REPLAY_SHOW_CONSTANTS"), "%u,%u", &first, &cnt);
            for (r = first; r < first + cnt && r < 192u; r++)
                fprintf(stderr, "[draw %4ld] c%u = %g %g %g %g\n", n, r,
                        c[r * 4], c[r * 4 + 1], c[r * 4 + 2], c[r * 4 + 3]);
        }
        /* The states that discard fragments without leaving a mark. */
        fprintf(stderr, "[draw %4ld] zfunc %lu afunc %lu colorwrite 0x%lX stencil %lu "
                "fill %lu shade %lu\n", n,
                (unsigned long)rs[D3DRS_ZFUNC], (unsigned long)rs[D3DRS_ALPHAFUNC],
                (unsigned long)rs[D3DRS_COLORWRITEENABLE], (unsigned long)rs[D3DRS_STENCILENABLE],
                (unsigned long)rs[D3DRS_FILLMODE], (unsigned long)rs[D3DRS_SHADEMODE]);
        if (rs[D3DRS_FOGENABLE]) {
            float fs, fe, fd;
            memcpy(&fs, &rs[D3DRS_FOGSTART], 4);
            memcpy(&fe, &rs[D3DRS_FOGEND], 4);
            memcpy(&fd, &rs[D3DRS_FOGDENSITY], 4);
            fprintf(stderr, "[draw %4ld] fog table %lu vertex %lu range %lu start %g end %g "
                    "density %g color %08lX\n", n, (unsigned long)rs[D3DRS_FOGTABLEMODE],
                    (unsigned long)rs[D3DRS_FOGVERTEXMODE], (unsigned long)rs[D3DRS_RANGEFOGENABLE],
                    fs, fe, fd, (unsigned long)rs[D3DRS_FOGCOLOR]);
        }
    }
    if (g_max_draws >= 0 && n >= g_max_draws)
        return 0;
    return !(n >= g_skip_draw && n <= g_skip_last);
}

static void note(const char *fmt, ...)
{
    va_list ap;

    if (g_quiet)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ window */

static LRESULT CALLBACK replay_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* Unlike shadow mode, replay is single-threaded: there are no guest threads,
 * so the window lives on the same thread that presents and is pumped between
 * loops. */
static HWND replay_window(UINT width, UINT height)
{
    WNDCLASSA wc;
    RECT r;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = replay_wndproc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, MAKEINTRESOURCEA(32512));   /* IDC_ARROW */
    wc.lpszClassName = "xboxrecomp_d3d8_replay";
    RegisterClassA(&wc);

    r.left = 0;
    r.top = 0;
    r.right = (LONG)width;
    r.bottom = (LONG)height;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowA(wc.lpszClassName, "xboxrecomp - D3D8 frame replay",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top,
                         NULL, NULL, wc.hInstance, NULL);
}

static void pump(void)
{
    MSG msg;

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* -------------------------------------------------------------------- dump */

/* The same 24-bit BMP the shadow path writes (hle_d3d8.c, shadow_dump_frame),
 * so images from the two paths are directly comparable. */
static void dump_bmp(IDirect3DDevice8 *dev, const char *path, UINT w, UINT h)
{
    IDirect3DSurface8 *surf = NULL;
    D3DLOCKED_RECT lr;
    uint8_t hdr[54];
    UINT y, x, pad;
    uint32_t filesz;
    FILE *f;

    if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, &surf)) || !surf)
        return;
    if (FAILED(surf->lpVtbl->LockRect(surf, &lr, NULL, D3DLOCK_READONLY))) {
        surf->lpVtbl->Release(surf);
        return;
    }
    pad = (4 - ((w * 3) & 3)) & 3;
    filesz = 54 + (w * 3 + pad) * h;

    f = fopen(path, "wb");
    if (f) {
        memset(hdr, 0, sizeof hdr);
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &filesz, 4);
        hdr[10] = 54;
        hdr[14] = 40;
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        hdr[26] = 1;
        hdr[28] = 24;
        fwrite(hdr, 1, sizeof hdr, f);
        for (y = h; y-- > 0; ) {         /* BMP rows run bottom-up */
            const uint8_t *row = (const uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch;
            for (x = 0; x < w; x++) {
                const uint8_t *p = row + x * 4;
                uint8_t bgr[3] = { p[2], p[1], p[0] };
                fwrite(bgr, 1, 3, f);
            }
            fwrite("\0\0\0", 1, pad, f);
        }
        fclose(f);
        note("[replay] wrote %s\n", path);
    } else {
        fprintf(stderr, "[replay] cannot write %s\n", path);
    }
    surf->lpVtbl->UnlockRect(surf);
    surf->lpVtbl->Release(surf);
}

/* ------------------------------------------------------------ replay state */

/* One capture texture id, as this process holds it. */
typedef struct {
    IDirect3DBaseTexture8 *tex;
    int                    is_cube;
} ReplayTexture;

typedef struct {
    IDirect3DDevice8   *dev;
    UINT                width, height;

    /* Capture texture id -> this process's texture. Ids are handed out in
     * order by the writer, so a growable array indexed by id is enough. */
    /* A slot holds a 2D texture or a cube; the capture numbers both in one
     * id space, and only a cube can be bound face by face. */
    ReplayTexture      *textures;
    uint32_t            texture_slots;

    /* Recorded program handle -> the handle d3d8_vsh_create_shader gave us. */
    struct { DWORD recorded, ours; } programs[REPLAY_MAX_PROGRAMS];
    int                 program_count;

    /* Capture depth surface id - 1 -> this process's surface. */
    IDirect3DSurface8  *depths[REPLAY_MAX_DEPTHS];
    IDirect3DSurface8  *device_depth;      /* the device's own, not owned */

    unsigned long       kinds[D3D8CAP_CHUNK_KINDS];
    unsigned long       unknown, malformed, failed, unmapped;
} Replay;

static void release_slot(ReplayTexture *slot)
{
    if (slot && slot->tex) {
        slot->tex->lpVtbl->Release(slot->tex);
        slot->tex = NULL;
        slot->is_cube = 0;
    }
}

static ReplayTexture *texture_slot(Replay *r, uint32_t id, int grow)
{
    if (id == 0)
        return NULL;
    if (id >= r->texture_slots) {
        uint32_t n = r->texture_slots ? r->texture_slots : 256;
        ReplayTexture *t;

        if (!grow || id > 0x00FFFFFFu)
            return NULL;
        while (n <= id)
            n *= 2;
        t = realloc(r->textures, (size_t)n * sizeof *t);
        if (!t)
            return NULL;
        memset(t + r->texture_slots, 0, (size_t)(n - r->texture_slots) * sizeof *t);
        r->textures = t;
        r->texture_slots = n;
    }
    return &r->textures[id];
}

static void fill_level(Replay *r, IDirect3DTexture8 *tex, UINT level,
                       const uint8_t *src, uint32_t pitch, uint32_t rows)
{
    D3DLOCKED_RECT lr;
    uint32_t y, row;

    if (FAILED(tex->lpVtbl->LockRect(tex, level, &lr, NULL, 0))) {
        r->failed++;
        return;
    }
    /* The pitch was the host's own at capture time; it only differs here if
     * src/d3d changed its row maths since, and then the narrower wins. */
    if (lr.Pitch == (INT)pitch) {
        memcpy(lr.pBits, src, (size_t)pitch * rows);
    } else {
        row = pitch < (uint32_t)lr.Pitch ? pitch : (uint32_t)lr.Pitch;
        for (y = 0; y < rows; y++)
            memcpy((uint8_t *)lr.pBits + (size_t)y * (size_t)lr.Pitch,
                   src + (size_t)y * pitch, row);
        r->malformed++;
    }
    tex->lpVtbl->UnlockRect(tex, level);
}

static void do_texture(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapTexture *t = c->data;
    const D3D8CapLevel *levels;
    const uint8_t *bytes;
    ReplayTexture *slot;
    IDirect3DTexture8 *tex;
    size_t total = 0, offset = 0;
    uint32_t l;

    if (c->bytes < sizeof *t || !(slot = texture_slot(r, t->id, 1))) {
        r->malformed++;
        return;
    }
    levels = d3d8cap_tail(c, sizeof *t, (size_t)t->levels * sizeof *levels);
    if (!levels) {
        r->malformed++;
        return;
    }
    if (t->id < LIST_TEX_IDS) {
        g_tex_info[t->id].format = t->format;
        g_tex_info[t->id].width = t->width;
        g_tex_info[t->id].height = t->height;
    }
    for (l = 0; l < t->levels; l++) {
        if ((uint64_t)levels[l].pitch * levels[l].rows != levels[l].bytes) {
            r->malformed++;
            return;
        }
        total += levels[l].bytes;
    }
    bytes = d3d8cap_tail(c, sizeof *t + (size_t)t->levels * sizeof *levels, total);
    if (!bytes) {
        r->malformed++;
        return;
    }
    if (g_list_draws && t->width * t->height <= 4u && total >= 4u) {
        /* A tiny texture is usually a constant in disguise -- a fade colour,
         * a tint -- so its bytes are the value that matters. */
        fprintf(stderr, "[texture %u] %ux%u format 0x%02X: %02X %02X %02X %02X\n",
                t->id, t->width, t->height, t->format, bytes[0], bytes[1], bytes[2], bytes[3]);
    }

    /* REPLAY_TEX_DUMP=<id>: write that texture's level 0, as recorded, to
     * tex_<id>.bin, for looking at a texture that samples wrong. */
    {
        const char *want = getenv("REPLAY_TEX_DUMP");
        if (want && (uint32_t)atoi(want) == t->id && t->levels) {
            char name[64];
            FILE *f;
            snprintf(name, sizeof name, "tex_%u.bin", t->id);
            if ((f = fopen(name, "wb")) != NULL) {
                fwrite(bytes, 1, levels[0].bytes, f);
                fclose(f);
            }
            fprintf(stderr, "[texture %u] %ux%u format 0x%02X pitch %u rows %u -> %s\n",
                    t->id, t->width, t->height, t->format, levels[0].pitch,
                    levels[0].rows, name);
        }
    }

    /* An id seen again is the same texture written again (a later loop, or a
     * writer that re-emits): replace it. */
    release_slot(slot);
    tex = NULL;
    if (FAILED(r->dev->lpVtbl->CreateTexture(r->dev, t->width, t->height, t->levels,
                                             t->usage, (D3DFORMAT)t->format,
                                             D3DPOOL_MANAGED, &tex)) || !tex) {
        note("[replay] texture %u (format 0x%02X %ux%u) could not be created\n",
             t->id, t->format, t->width, t->height);
        r->failed++;
        return;
    }
    for (l = 0; l < t->levels; l++) {
        fill_level(r, tex, l, bytes + offset, levels[l].pitch, levels[l].rows);
        offset += levels[l].bytes;
    }
    slot->tex = (IDirect3DBaseTexture8 *)tex;
    slot->is_cube = 0;
}

static void do_texture_level(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapTextureLevel *t = c->data;
    ReplayTexture *slot;
    const uint8_t *bytes;

    if (c->bytes < sizeof *t || (uint64_t)t->pitch * t->rows != t->bytes ||
        !(bytes = d3d8cap_tail(c, sizeof *t, t->bytes))) {
        r->malformed++;
        return;
    }
    slot = texture_slot(r, t->id, 0);
    if (!slot || !slot->tex || slot->is_cube) {
        r->unmapped++;
        return;
    }
    fill_level(r, (IDirect3DTexture8 *)slot->tex, t->level, bytes, t->pitch, t->rows);
}

static void do_texture_release(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapTextureId *t = c->data;
    ReplayTexture *slot;

    if (c->bytes < sizeof *t) {
        r->malformed++;
        return;
    }
    slot = texture_slot(r, t->id, 0);
    if (!slot || !slot->tex) {
        r->unmapped++;
        return;
    }
    release_slot(slot);
}

/* A cube texture, created empty: the capture records only its shape, because
 * shadow mode mirrors a cube only when the title renders into it, and those
 * draws are in the capture too. */
static void do_cube_texture(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapCubeTexture *t = c->data;
    IDirect3DCubeTexture8 *cube = NULL;
    ReplayTexture *slot;

    if (c->bytes < sizeof *t || !(slot = texture_slot(r, t->id, 1))) {
        r->malformed++;
        return;
    }
    release_slot(slot);
    if (FAILED(r->dev->lpVtbl->CreateCubeTexture(r->dev, t->edge, t->levels,
                                                 t->usage, (D3DFORMAT)t->format,
                                                 D3DPOOL_DEFAULT, &cube)) || !cube) {
        note("[replay] cube texture %u (format 0x%02X edge %u) could not be created\n",
             t->id, t->format, t->edge);
        r->failed++;
        return;
    }
    slot->tex = (IDirect3DBaseTexture8 *)cube;
    slot->is_cube = 1;
}

static void do_depth_surface(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapDepthSurface *d = c->data;
    IDirect3DSurface8 **slot;

    if (c->bytes < sizeof *d || d->id == 0 || d->id > REPLAY_MAX_DEPTHS) {
        r->malformed++;
        return;
    }
    slot = &r->depths[d->id - 1];
    if (*slot) {
        (*slot)->lpVtbl->Release(*slot);
        *slot = NULL;
    }
    if (FAILED(r->dev->lpVtbl->CreateDepthStencilSurface(r->dev, d->width, d->height,
                                                         (D3DFORMAT)d->format,
                                                         D3DMULTISAMPLE_NONE, slot))) {
        *slot = NULL;
        r->failed++;
    }
}

static void do_set_render_target(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapSetRenderTarget *t = c->data;
    IDirect3DSurface8 *surface = NULL, *depth = NULL;
    ReplayTexture *slot;

    if (c->bytes < sizeof *t ||
        (t->depth_id > REPLAY_MAX_DEPTHS && t->depth_id != D3D8CAP_DEPTH_DEVICE)) {
        r->malformed++;
        return;
    }
    if (t->depth_id == D3D8CAP_DEPTH_DEVICE) {
        depth = r->device_depth;
    } else if (t->depth_id) {
        depth = r->depths[t->depth_id - 1];
        if (!depth)
            r->unmapped++;
    }
    /* A target that cannot be made here is drawn to the back buffer, as the
     * writer records a target it cannot name. */
    if (t->texture_id) {
        slot = texture_slot(r, t->texture_id, 0);
        if (!slot || !slot->tex) {
            r->unmapped++;
        } else if (slot->is_cube) {
            IDirect3DCubeTexture8 *cube = (IDirect3DCubeTexture8 *)slot->tex;

            if (FAILED(cube->lpVtbl->GetCubeMapSurface(cube,
                    (D3DCUBEMAP_FACES)t->face, t->level, &surface)))
                surface = NULL;
        } else {
            IDirect3DTexture8 *tex = (IDirect3DTexture8 *)slot->tex;

            if (FAILED(tex->lpVtbl->GetSurfaceLevel(tex, t->level, &surface)))
                surface = NULL;
        }
        if (slot && slot->tex && !surface)
            r->failed++;
        /* Depth goes with the target it was made for: the back buffer gets
         * the device's own, as live shadow mode falls back. */
        if (!surface && depth)
            depth = r->device_depth;
    }
    if (g_list_draws)
        fprintf(stderr, "[target after %ld draws] texture %u level %u face %u depth %u\n",
                g_draw_index, t->texture_id, t->level, t->face, t->depth_id);
    if (FAILED(r->dev->lpVtbl->SetRenderTarget(r->dev, surface, depth)))
        r->failed++;
    if (surface)
        surface->lpVtbl->Release(surface);
}

static int program_find(const Replay *r, DWORD recorded)
{
    int i;

    for (i = 0; i < r->program_count; i++)
        if (r->programs[i].recorded == recorded)
            return i;
    return -1;
}

static void program_forget(Replay *r, int i)
{
    r->programs[i] = r->programs[--r->program_count];
}

static void do_vs_create(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapVsCreate *p = c->data;
    const DWORD *code;
    DWORD ours = 0;
    int i;

    if (c->bytes < sizeof *p || !p->insn_count ||
        !(code = d3d8cap_tail(c, sizeof *p, (size_t)p->insn_count * 4u * sizeof(DWORD)))) {
        r->malformed++;
        return;
    }
    if (g_list_draws && p->insn_count <= 4) {
        /* A short program's microcode, to check the decoder by hand. */
        uint32_t k;
        fprintf(stderr, "[program 0x%05lX] %u instructions:", (unsigned long)p->handle,
                p->insn_count);
        for (k = 0; k < p->insn_count * 4u; k++)
            fprintf(stderr, "%s%08lX", (k % 4) ? " " : "  ", (unsigned long)code[k]);
        fprintf(stderr, "\n");
    }
    /* A recorded handle is only reused after its delete; a stale mapping
     * would mean a delete was lost, and the old program must not leak. */
    i = program_find(r, p->handle);
    if (i >= 0) {
        d3d8_vsh_delete_shader(r->programs[i].ours);
        program_forget(r, i);
    }
    if (FAILED(d3d8_vsh_create_shader(code, (int)p->insn_count, &ours))) {
        r->failed++;
        return;
    }
    if (r->program_count >= REPLAY_MAX_PROGRAMS) {
        d3d8_vsh_delete_shader(ours);
        r->failed++;
        return;
    }
    r->programs[r->program_count].recorded = p->handle;
    r->programs[r->program_count].ours = ours;
    r->program_count++;
}

static void do_vs_delete(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapVsHandle *p = c->data;
    int i;

    if (c->bytes < sizeof *p) {
        r->malformed++;
        return;
    }
    i = program_find(r, p->handle);
    if (i < 0) {
        r->unmapped++;
        return;
    }
    d3d8_vsh_delete_shader(r->programs[i].ours);
    program_forget(r, i);
}

static void do_vs_declaration(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapVsDeclaration *p = c->data;
    const D3D8CapVsInput *in;
    D3D8VshInput decl[NV2A_VS_MAX_INPUTS];
    uint32_t k;
    int i;

    if (c->bytes < sizeof *p || p->count > NV2A_VS_MAX_INPUTS ||
        !(in = d3d8cap_tail(c, sizeof *p, (size_t)p->count * sizeof *in))) {
        r->malformed++;
        return;
    }
    i = program_find(r, p->handle);
    if (i < 0) {
        r->unmapped++;
        return;
    }
    for (k = 0; k < p->count; k++) {
        decl[k].reg    = in[k].reg;
        decl[k].format = (DXGI_FORMAT)in[k].dxgi_format;
        decl[k].offset = in[k].offset;
    }
    if (g_list_draws) {
        fprintf(stderr, "[declaration] vs 0x%05lX:", (unsigned long)p->handle);
        for (k = 0; k < p->count; k++)
            fprintf(stderr, " v%u@%u:dxgi%u", in[k].reg, in[k].offset, in[k].dxgi_format);
        fprintf(stderr, "\n");
    }
    if (FAILED(d3d8_vsh_set_declaration(r->programs[i].ours, decl, (int)p->count)))
        r->failed++;
}

static void do_set_vertex_shader(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapSetVertexShader *p = c->data;
    DWORD handle;
    int i;

    if (c->bytes < sizeof *p) {
        r->malformed++;
        return;
    }
    handle = p->handle;
    if (d3d8_vsh_is_programmable(handle)) {
        i = program_find(r, handle);
        if (i < 0) {                     /* selecting a program nobody created */
            r->unmapped++;
            return;
        }
        handle = r->programs[i].ours;
    }
    g_cur_vs = p->handle;
    r->dev->lpVtbl->SetVertexShader(r->dev, handle);
}

static void do_draw_up(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapDrawUp *d = c->data;
    const void *verts;
    uint64_t need;

    if (c->bytes < sizeof *d || !d->stride ||
        !(verts = d3d8cap_tail(c, sizeof *d, d->vertex_bytes))) {
        r->malformed++;
        return;
    }
    /* The host reads what its own count says; never more than was recorded. */
    need = (uint64_t)d3d8_up_vertices_read((D3DPRIMITIVETYPE)d->prim_type,
                                           d->prim_count) * d->stride;
    if (need > d->vertex_bytes) {
        r->malformed++;
        return;
    }
    if (!draw_gate("up", d->prim_type, d->prim_count, d->stride))
        return;
    if (g_list_draws) {
        const float *f = (const float *)verts;
        const uint32_t *w = (const uint32_t *)verts;
        uint32_t n = d->stride / 4u;
        fprintf(stderr, "[draw %4ld] v0 = %g %g %g  raw", g_draw_index - 1, f[0], f[1], f[2]);
        for (uint32_t k = 3; k < n && k < 8; k++)
            fprintf(stderr, " %08X", w[k]);
        fprintf(stderr, "\n");
    }
    if (FAILED(r->dev->lpVtbl->DrawPrimitiveUP(r->dev, (D3DPRIMITIVETYPE)d->prim_type,
                                               d->prim_count, verts, d->stride)))
        r->failed++;
}

static void do_draw_indexed_up(Replay *r, const D3D8CapChunk *c)
{
    const D3D8CapDrawIndexedUp *d = c->data;
    const void *idx, *verts;
    uint64_t need;

    if (c->bytes < sizeof *d || !d->stride ||
        !(idx = d3d8cap_tail(c, sizeof *d, d->index_bytes)) ||
        !(verts = d3d8cap_tail(c, sizeof *d + (size_t)d->index_bytes, d->vertex_bytes))) {
        r->malformed++;
        return;
    }
    need = (uint64_t)d3d8_up_indices_read((D3DPRIMITIVETYPE)d->prim_type, d->prim_count) *
           (d->index_format == D3DFMT_INDEX32 ? 4u : 2u);
    if (need > d->index_bytes ||
        (uint64_t)d->num_vertices * d->stride > d->vertex_bytes) {
        r->malformed++;
        return;
    }
    if (!draw_gate("indexed", d->prim_type, d->prim_count, d->stride))
        return;
    if (g_list_draws) {
        /* The vertices as the host reads them: a draw whose geometry never
         * lands on screen looks like every other draw in the line above. */
        const uint16_t *ix = (const uint16_t *)idx;
        const float *f = (const float *)((const uint8_t *)verts +
                                         (size_t)ix[0] * d->stride);
        const float *g = (const float *)((const uint8_t *)verts +
                                         (size_t)ix[1] * d->stride);
        fprintf(stderr, "[draw %4ld] verts %u from %u (%u bytes); idx %u %u %u %u; "
                "v[%u] = %g %g %g %g | v[%u] = %g %g %g %g\n", g_draw_index - 1,
                d->num_vertices, d->min_index, d->vertex_bytes,
                ix[0], ix[1], ix[2], ix[3],
                ix[0], f[0], f[1], f[2], f[3], ix[1], g[0], g[1], g[2], g[3]);
        {
            const uint32_t *w = (const uint32_t *)f;
            uint32_t k;
            fprintf(stderr, "[draw %4ld] v[%u] raw", g_draw_index - 1, ix[0]);
            for (k = 0; k < d->stride / 4u && k < 16u; k++)
                fprintf(stderr, " %08X", w[k]);
            fprintf(stderr, "\n");
        }
    }
    if (FAILED(r->dev->lpVtbl->DrawIndexedPrimitiveUP(
            r->dev, (D3DPRIMITIVETYPE)d->prim_type, d->min_index, d->num_vertices,
            d->prim_count, idx, (D3DFORMAT)d->index_format, verts, d->stride)))
        r->failed++;
}

static void replay_chunk(Replay *r, const D3D8CapChunk *c)
{
    if (c->type >= D3D8CAP_CHUNK_KINDS) {
        /* Cannot come from a writer of this version; skipped, not fatal. */
        r->unknown++;
        return;
    }
    r->kinds[c->type]++;

    switch (c->type) {
    case D3D8CAP_FRAME_START:
        break;
    case D3D8CAP_CLEAR: {
        const D3D8CapClear *p = c->data;
        const D3D8CapRect *rects = NULL;
        D3DRECT out[16];
        uint32_t i;

        if (c->bytes < sizeof *p || p->rect_count > 16 ||
            (p->rect_count &&
             !(rects = d3d8cap_tail(c, sizeof *p, (size_t)p->rect_count * sizeof *rects)))) {
            r->malformed++;
            break;
        }
        for (i = 0; i < p->rect_count; i++) {
            out[i].x1 = rects[i].x1;
            out[i].y1 = rects[i].y1;
            out[i].x2 = rects[i].x2;
            out[i].y2 = rects[i].y2;
        }
        /* A clear after the --draws limit would wipe the draws kept before
         * it, so it counts as drawing. */
        if (g_list_draws)
            fprintf(stderr, "[clear after %ld draws] flags 0x%X color 0x%08X z %g, %u rect(s)%s%s\n",
                    g_draw_index, p->flags, p->color, p->z, p->rect_count,
                    p->rect_count ? " first " : "",
                    (g_max_draws >= 0 && g_draw_index >= g_max_draws) ? "  (skipped)" : "");
        if (g_list_draws && p->rect_count)
            fprintf(stderr, "[clear]   %d,%d-%d,%d\n", rects[0].x1, rects[0].y1,
                    rects[0].x2, rects[0].y2);
        if (g_max_draws >= 0 && g_draw_index >= g_max_draws)
            break;
        r->dev->lpVtbl->Clear(r->dev, p->rect_count, p->rect_count ? out : NULL,
                              p->flags, p->color, p->z, p->stencil);
        break;
    }
    case D3D8CAP_RENDER_STATE: {
        const D3D8CapRenderState *p = c->data;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        r->dev->lpVtbl->SetRenderState(r->dev, (D3DRENDERSTATETYPE)p->state, p->value);
        break;
    }
    case D3D8CAP_TEXTURE_STAGE_STATE: {
        const D3D8CapStageState *p = c->data;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        r->dev->lpVtbl->SetTextureStageState(r->dev, p->stage,
                                             (D3DTEXTURESTAGESTATETYPE)p->type, p->value);
        break;
    }
    case D3D8CAP_TRANSFORM: {
        const D3D8CapTransform *p = c->data;
        D3DMATRIX m;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        memcpy(&m, p->m, sizeof m);
        r->dev->lpVtbl->SetTransform(r->dev, (D3DTRANSFORMSTATETYPE)p->state, &m);
        break;
    }
    case D3D8CAP_MATERIAL: {
        const D3D8CapMaterial *p = c->data;
        D3DMATERIAL8 m;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        memcpy(&m, p->words, sizeof m);
        r->dev->lpVtbl->SetMaterial(r->dev, &m);
        break;
    }
    case D3D8CAP_LIGHT: {
        const D3D8CapLight *p = c->data;
        D3DLIGHT8 l;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        memcpy(&l, p->words, sizeof l);
        r->dev->lpVtbl->SetLight(r->dev, p->index, &l);
        break;
    }
    case D3D8CAP_LIGHT_ENABLE: {
        const D3D8CapLightEnable *p = c->data;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        r->dev->lpVtbl->LightEnable(r->dev, p->index, p->enable ? TRUE : FALSE);
        break;
    }
    case D3D8CAP_VIEWPORT: {
        const D3D8CapViewport *p = c->data;
        D3DVIEWPORT8 vp;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        vp.X = p->x;
        vp.Y = p->y;
        vp.Width = p->width;
        vp.Height = p->height;
        vp.MinZ = p->min_z;
        vp.MaxZ = p->max_z;
        if (g_list_draws)
            fprintf(stderr, "[viewport] %lu,%lu %lux%lu z %g..%g\n",
                    (unsigned long)vp.X, (unsigned long)vp.Y, (unsigned long)vp.Width,
                    (unsigned long)vp.Height, vp.MinZ, vp.MaxZ);
        r->dev->lpVtbl->SetViewport(r->dev, &vp);
        break;
    }
    case D3D8CAP_SCISSORS: {
        const D3D8CapScissors *p = c->data;
        D3DRECT rect;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        rect.x1 = p->rect.x1; rect.y1 = p->rect.y1;
        rect.x2 = p->rect.x2; rect.y2 = p->rect.y2;
        if (g_list_draws)
            fprintf(stderr, "[scissors] %lu rect(s)%s: %ld,%ld-%ld,%ld\n",
                    (unsigned long)p->count, p->exclusive ? " exclusive" : "",
                    (long)rect.x1, (long)rect.y1, (long)rect.x2, (long)rect.y2);
        xbox_D3D8SetScissors(p->count, p->exclusive ? TRUE : FALSE, &rect);
        break;
    }
    case D3D8CAP_SET_TEXTURE: {
        const D3D8CapSetTexture *p = c->data;
        ReplayTexture *slot;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        if (p->stage < 2)
            g_stage_tex[p->stage] = p->texture_id;
        slot = texture_slot(r, p->texture_id, 0);
        if (p->texture_id && (!slot || !slot->tex))
            r->unmapped++;               /* its TEXTURE chunk failed: bind nothing */
        r->dev->lpVtbl->SetTexture(r->dev, p->stage, slot ? slot->tex : NULL);
        break;
    }
    case D3D8CAP_SET_VERTEX_SHADER:
        do_set_vertex_shader(r, c);
        break;
    case D3D8CAP_DRAW_UP:
        do_draw_up(r, c);
        break;
    case D3D8CAP_DRAW_INDEXED_UP:
        do_draw_indexed_up(r, c);
        break;
    case D3D8CAP_TEXTURE:
        do_texture(r, c);
        break;
    case D3D8CAP_TEXTURE_LEVEL:
        do_texture_level(r, c);
        break;
    case D3D8CAP_TEXTURE_RELEASE:
        do_texture_release(r, c);
        break;
    case D3D8CAP_VS_CREATE:
        do_vs_create(r, c);
        break;
    case D3D8CAP_VS_DELETE:
        do_vs_delete(r, c);
        break;
    case D3D8CAP_VS_DECLARATION:
        do_vs_declaration(r, c);
        break;
    case D3D8CAP_VS_CONSTANTS: {
        const D3D8CapVsConstants *p = c->data;
        const float *data;

        if (c->bytes < sizeof *p || p->count > NV2A_VS_MAX_CONSTANTS ||
            !(data = d3d8cap_tail(c, sizeof *p, (size_t)p->count * 4u * sizeof(float)))) {
            r->malformed++;
            break;
        }
        if (g_list_draws) {
            /* Which registers hold anything, and the first four rows: a
             * frame whose camera matrix never arrived draws nothing and
             * says nothing, and this is where that shows. */
            uint32_t i, nonzero = 0;
            for (i = 0; i < p->count * 4u; i++)
                if (data[i] != 0.0f)
                    nonzero++;
            fprintf(stderr, "[constants] c%u..c%u: %u of %u floats non-zero;"
                    " c%u = %g %g %g %g | c%u = %g %g %g %g\n",
                    p->first_reg, p->first_reg + p->count - 1, nonzero, p->count * 4u,
                    p->first_reg, data[0], data[1], data[2], data[3],
                    p->first_reg + 1, p->count > 1 ? data[4] : 0.f,
                    p->count > 1 ? data[5] : 0.f, p->count > 1 ? data[6] : 0.f,
                    p->count > 1 ? data[7] : 0.f);
            /* REPLAY_ALL_CONSTANTS=1: every register of a bulk upload. */
            if (p->count > 2 && getenv("REPLAY_ALL_CONSTANTS"))
                for (i = 0; i < p->count; i++)
                    fprintf(stderr, "[constants]   c%u = %g %g %g %g\n", p->first_reg + i,
                            data[i * 4], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3]);
        }
        d3d8_vsh_set_constant((int)p->first_reg, data, (int)p->count);
        break;
    }
    case D3D8CAP_VS_SCREENSPACE: {
        const D3D8CapVsScreenspace *p = c->data;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        if (p->enabled)
            d3d8_vsh_set_screenspace(p->scale, p->offset);
        else
            d3d8_vsh_clear_screenspace();
        break;
    }
    case D3D8CAP_VS_VERTEX_DATA: {
        const D3D8CapVsVertexData *p = c->data;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        if (g_list_draws)
            fprintf(stderr, "[vertex data] v%d = %g %g %g %g\n", (int)p->reg,
                    p->value[0], p->value[1], p->value[2], p->value[3]);
        d3d8_vsh_set_vertex_data((int)p->reg, p->value);
        break;
    }
    case D3D8CAP_CUBE_TEXTURE:
        do_cube_texture(r, c);
        break;
    case D3D8CAP_DEPTH_SURFACE:
        do_depth_surface(r, c);
        break;
    case D3D8CAP_SET_RENDER_TARGET:
        do_set_render_target(r, c);
        break;
    case D3D8CAP_PS_TOKEN: {
        const D3D8CapPsToken *p = c->data;

        if (c->bytes < sizeof *p) {
            r->malformed++;
            break;
        }
        g_cur_token = p->token;
        d3d8_combiners_set_pixel_shader(g_no_combiners ? 0 : p->token);
        break;
    }
    default:
        r->unknown++;
        break;
    }
}

/* Everything this loop created goes, so the next loop starts from the
 * capture's snapshot alone. Stages are cleared first: the host keeps a raw
 * pointer to each bound texture. */
static void end_loop(Replay *r)
{
    uint32_t i;

    for (i = 0; i < REPLAY_STAGES; i++)
        r->dev->lpVtbl->SetTexture(r->dev, i, NULL);
    r->dev->lpVtbl->SetRenderTarget(r->dev, NULL, r->device_depth);
    for (i = 0; i < REPLAY_MAX_DEPTHS; i++)
        if (r->depths[i]) {
            r->depths[i]->lpVtbl->Release(r->depths[i]);
            r->depths[i] = NULL;
        }
    for (i = 1; i < r->texture_slots; i++)
        release_slot(&r->textures[i]);
    while (r->program_count > 0) {
        d3d8_vsh_delete_shader(r->programs[r->program_count - 1].ours);
        r->program_count--;
    }
}

static void report_loop(const Replay *r, int loop)
{
    char line[1024];
    int n, k;

    n = snprintf(line, sizeof line, "[replay] loop %d executed:", loop);
    for (k = 0; k < D3D8CAP_CHUNK_KINDS && n > 0 && n < (int)sizeof line; k++)
        if (r->kinds[k])
            n += snprintf(line + n, sizeof line - (size_t)n, " %s %lu",
                          d3d8cap_chunk_name((uint32_t)k), r->kinds[k]);
    note("%s\n", line);
    if (r->failed || r->malformed || r->unmapped || r->unknown || !g_quiet)
        fprintf(stderr, "[replay] loop %d: %lu host calls failed, %lu malformed chunks, "
                "%lu unmapped handles, %lu unknown chunks\n", loop, r->failed,
                r->malformed, r->unmapped, r->unknown);
}

static void usage(void)
{
    fprintf(stderr,
        "usage: d3d8_replay <capture%s> [--out <prefix>] [--loops <n>]\n"
        "                   [--dump-every] [--hold] [--quiet]\n"
        "                   [--no-combiners] [--draws <n>] [--skip-draw <n>]\n"
        "                   [--list-draws]\n",
        D3D8CAP_EXTENSION);
}

int main(int argc, char **argv)
{
    const char *path = NULL, *prefix = "replay";
    int loops = 1, dump_every = 0, hold = 0, i, loop;
    char err[128], out[512];
    D3D8CapReader *cap;
    const D3D8CapHeader *h;
    Replay r;
    D3DPRESENT_PARAMETERS pp;
    IDirect3D8 *d3d;
    HWND hwnd;
    HRESULT hr;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            prefix = argv[++i];
        else if (!strcmp(argv[i], "--loops") && i + 1 < argc)
            loops = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-every"))
            dump_every = 1;
        else if (!strcmp(argv[i], "--hold"))
            hold = 1;
        else if (!strcmp(argv[i], "--quiet"))
            g_quiet = 1;
        else if (!strcmp(argv[i], "--no-combiners"))
            g_no_combiners = 1;
        else if (!strcmp(argv[i], "--draws") && i + 1 < argc)
            g_max_draws = atol(argv[++i]);
        else if (!strcmp(argv[i], "--skip-draw") && i + 1 < argc)
            {
                char *end;
                g_skip_draw = strtol(argv[++i], &end, 10);
                g_skip_last = *end == '-' ? strtol(end + 1, NULL, 10) : g_skip_draw;
            }
        else if (!strcmp(argv[i], "--list-draws"))
            g_list_draws = 1;
        else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else {
            path = argv[i];
        }
    }
    if (!path || loops < 1) {
        usage();
        return 2;
    }

    cap = d3d8cap_open(path, err, sizeof err);
    if (!cap) {
        fprintf(stderr, "[replay] %s: %s\n", path, err);
        return 1;
    }
    h = d3d8cap_header(cap);
    note("[replay] %s: frame %u, %ux%u, %u chunks\n",
         path, h->frame, h->width, h->height, h->chunk_count);

    memset(&r, 0, sizeof r);
    r.width  = h->width  ? h->width  : 640;
    r.height = h->height ? h->height : 480;

    hwnd = replay_window(r.width, r.height);
    if (!hwnd) {
        fprintf(stderr, "[replay] CreateWindow failed (%lu)\n", GetLastError());
        d3d8cap_close_read(cap);
        return 1;
    }

    /* As hle_d3d8.c's shadow_create. */
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = r.width;
    pp.BackBufferHeight = r.height;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;

    d3d = xbox_Direct3DCreate8(0);
    hr = d3d ? d3d->lpVtbl->CreateDevice(d3d, 0, 1 /* HAL */, hwnd, 0, &pp, &r.dev)
             : E_FAIL;
    g_dev = r.dev;
    if (FAILED(hr) || !r.dev) {
        fprintf(stderr, "[replay] CreateDevice failed (0x%08lX)\n", (unsigned long)hr);
        d3d8cap_close_read(cap);
        return 1;
    }
    xbox_D3D8SetPresentInterval(0);      /* never wait for vblank: this is a tool */
    if (SUCCEEDED(r.dev->lpVtbl->GetDepthStencilSurface(r.dev, &r.device_depth)) &&
        r.device_depth)
        r.device_depth->lpVtbl->Release(r.device_depth);   /* the device keeps it */

    for (loop = 0; loop < loops; loop++) {
        D3D8CapChunk c;

        memset(r.kinds, 0, sizeof r.kinds);
        r.unknown = r.malformed = r.failed = r.unmapped = 0;
        d3d8cap_rewind(cap);
        g_draw_index = 0;
        while (d3d8cap_next(cap, &c))
            replay_chunk(&r, &c);
        if (!r.kinds[D3D8CAP_FRAME_START])
            fprintf(stderr, "[replay] loop %d: no frame_start chunk -- the capture is "
                    "truncated inside its snapshot\n", loop);

        if (dump_every || loop == loops - 1) {
            snprintf(out, sizeof out, "%s%03d.bmp", prefix, loop);
            dump_bmp(r.dev, out, r.width, r.height);
        }
        r.dev->lpVtbl->Swap(r.dev, 0);
        pump();
        report_loop(&r, loop);
        end_loop(&r);
    }

    if (hold) {
        MSG msg;
        note("[replay] holding the window open; close it to exit\n");
        while (GetMessageA(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    free(r.textures);
    d3d8cap_close_read(cap);
    return 0;
}
