/*
 * hle_d3d8_push.c -- the NV2A vertex arrays, read from the push buffer.
 *
 * Part of shadow mode (hle_d3d8.c). Every replacement runs the title's own
 * body first, so by the time a draw reaches the shadow renderer the guest push
 * buffer holds exactly what the hardware would be given.
 *
 * The vertex buffer draws in hle_d3d8_vertex.c find their vertices through
 * SetStreamSource: stream 0's buffer and stride. That is the XDK's view, and a
 * title that sets the arrays itself -- reserving space with BeginPush, writing
 * SET_VERTEX_DATA_ARRAY_OFFSET / _FORMAT, ending with EndPush -- gives the
 * hardware a different one: separate arrays per attribute, each with its own
 * stride. TimeSplitters: Future Perfect does this for its world and its
 * characters (one function, 0x0001EB00, sets all sixteen), and under the
 * stream 0 view every such draw read past a 24-byte stride and was skipped.
 *
 * So the push buffer itself is read: at every draw and at EndPush, whatever
 * was written since the last look is walked as NV2A method packets and the
 * array registers are kept. That is ground truth whoever wrote them, the title
 * or the XDK. hle_d3d8_title_vertices then gathers the arrays into one
 * interleaved vertex in the layout the selected program's declaration gives.
 * It is a fallback: it is only used when the stream 0 view cannot hold the
 * declared vertex, which is exactly when the old path skipped the draw.
 *
 * RECOMP_HLE_D3D8_PUSH_DUMP=1 also reports which methods were written, which
 * is the first thing to know about a title that fills its own push buffer.
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"

#ifdef _WIN32
#include "d3d8_xbox.h"
#include "d3d8_vsh.h"
#include "hle_d3d8_record.h"

IDirect3DDevice8 *hle_d3d8_shadow_device(void);

HLE_ORIGINAL(D3DDevice_EndPush);
HLE_IMPORT_VAR(D3D_g_pDevice);

#define PUSH_MAX_METHODS 512
#define NV_ATTRS 16
#define NV2A_PROGRAM_SLOTS 136u
#define CONTIG_BASE 0x80000000u
#define CONTIG_SIZE (64u * 1024u * 1024u)

/* The NV2A vertex arrays as last written: per attribute a DMA offset
 * (0x1720 + 4i) and a format, stride << 8 | count << 4 | type. */
static uint32_t g_arr_offset[NV_ATTRS], g_arr_format[NV_ATTRS];

static struct { uint32_t method; unsigned long writes; uint32_t first; } g_seen[PUSH_MAX_METHODS];
static int g_seen_count;
static unsigned long g_syncs, g_words, g_lost, g_title_draws, g_title_missing;
static unsigned long g_title_moved;
static uint64_t g_trace_t0;   /* RECOMP_HLE_D3D8_ARRAY_TRACE arms 45 s after the first draw */  /* gathered although stream 0 looked wide enough */
static unsigned long g_vdata_sets;   /* current values set from SET_VERTEX_DATA* */
static unsigned long g_const_regs;   /* constant registers replayed from the walk */
static unsigned long g_hw_begins;    /* SET_BEGIN_END(primitive): draws the hardware saw */

/* Title RAM or the contiguous window: the only places a push buffer lives. A
 * walk follows jumps and runs ahead of a pointer it does not trust, so every
 * word it reads is checked first. */
static int readable(uint32_t va, uint64_t bytes)
{
    if (va >= 0x00010000u && (uint64_t)va + bytes <= g_xbox_total_ram)
        return 1;
    return va >= CONTIG_BASE && (uint64_t)va + bytes <= (uint64_t)CONTIG_BASE + CONTIG_SIZE;
}

static int dump_enabled(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("RECOMP_HLE_D3D8_PUSH_DUMP") != NULL;
    return on;
}

static void note_method(uint32_t method, uint32_t first)
{
    int i;

    for (i = 0; i < g_seen_count; i++)
        if (g_seen[i].method == method) {
            g_seen[i].writes++;
            return;
        }
    if (g_seen_count < PUSH_MAX_METHODS) {
        g_seen[g_seen_count].method = method;
        g_seen[g_seen_count].writes = 1;
        g_seen[g_seen_count].first = first;
        g_seen_count++;
    }
}

static void report(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    int i;

    if (!last) {
        last = now;
        return;
    }
    if (now - last < 10000)
        return;
    last = now;
    fprintf(stderr, "[HLE-D3D8] push buffer: %lu syncs, %lu dwords, %lu lost; "
                    "title-array draws %lu (%lu not where stream 0 said), out of range %lu; "
                    "%lu constant registers, %lu current values; "
                    "%lu hardware draws (BEGIN_END); methods:\n",
            g_syncs, g_words, g_lost, g_title_draws, g_title_moved, g_title_missing, g_const_regs, g_vdata_sets,
            g_hw_begins);
    for (i = 0; i < g_seen_count; i++)
        fprintf(stderr, "[HLE-D3D8]   0x%04X x%lu (first 0x%08X)\n",
                g_seen[i].method, g_seen[i].writes, g_seen[i].first);
}

/* Vertex program constants, as the hardware loads them: SET_TRANSFORM_
 * CONSTANT_LOAD (0x1EA4) sets the register, then SET_TRANSFORM_CONSTANT
 * (0x0B80..0x0BFC) streams float4s into it, the register advancing after
 * every fourth word. The XDK's SetVertexShaderConstant* are replaced and
 * forward their constants, but a title may write them itself:
 * TimeSplitters: Future Perfect writes its characters' bone matrices this
 * way, and with them stale on the host its arms and the character-select
 * models were drawn from whatever matrices were left, or not at all. The
 * walk replays them in push-buffer order, so a constant the XDK also
 * forwarded only arrives twice with the same value. */
static uint32_t g_const_load, g_const_words[4];

/* An input register's current value: what a vertex program reads from a
 * register no enabled array feeds. The hardware takes it from the
 * SET_VERTEX_DATA* methods, and FP writes those itself -- the glow on its
 * first-person weapon colours its vertices from v2 with no colour array, and
 * with v2 left at the host's default it drew as a dark blob mid-screen.
 * Packed forms as xemu decodes them; inside BEGIN_END a write to v0 emits a
 * vertex rather than setting anything, so that one is left alone. */
static uint32_t g_vdata_words[NV_ATTRS][4];
static int g_in_begin;

static void set_current(uint32_t reg, float x, float y, float z, float w)
{
    float f[4];

    if (reg >= NV_ATTRS || (reg == 0u && g_in_begin) || !hle_d3d8_shadow_device())
        return;
    f[0] = x; f[1] = y; f[2] = z; f[3] = w;
    host_vsh_set_vertex_data((int)reg, f);
    g_vdata_sets++;
}

static float word_float(uint32_t w)
{
    float f;
    memcpy(&f, &w, sizeof f);
    return f;
}

static void track_vertex_data(uint32_t method, uint32_t value)
{
    if (method >= 0x1880u && method < 0x1900u) {          /* DATA2F_M */
        uint32_t reg = (method - 0x1880u) / 8u, k = ((method - 0x1880u) / 4u) & 1u;
        g_vdata_words[reg][k] = value;
        if (k == 1u)
            set_current(reg, word_float(g_vdata_words[reg][0]), word_float(value), 0.0f, 1.0f);
    } else if (method >= 0x1900u && method < 0x1940u) {   /* DATA2S */
        set_current((method - 0x1900u) / 4u, (float)(int16_t)(value & 0xFFFFu),
                    (float)(int16_t)(value >> 16), 0.0f, 1.0f);
    } else if (method >= 0x1940u && method < 0x1980u) {   /* DATA4UB */
        set_current((method - 0x1940u) / 4u, (float)(value & 0xFFu) / 255.0f,
                    (float)((value >> 8) & 0xFFu) / 255.0f,
                    (float)((value >> 16) & 0xFFu) / 255.0f,
                    (float)(value >> 24) / 255.0f);
    } else if (method >= 0x1980u && method < 0x1A00u) {   /* DATA4S_M */
        uint32_t reg = (method - 0x1980u) / 8u, k = ((method - 0x1980u) / 4u) & 1u;
        g_vdata_words[reg][k] = value;
        if (k == 1u)
            set_current(reg, (float)(int16_t)(g_vdata_words[reg][0] & 0xFFFFu),
                        (float)(int16_t)(g_vdata_words[reg][0] >> 16),
                        (float)(int16_t)(value & 0xFFFFu), (float)(int16_t)(value >> 16));
    } else if (method >= 0x1A00u && method < 0x1B00u) {   /* DATA4F_M */
        uint32_t reg = (method - 0x1A00u) / 16u, k = ((method - 0x1A00u) / 4u) & 3u;
        g_vdata_words[reg][k] = value;
        if (k == 3u)
            set_current(reg, word_float(g_vdata_words[reg][0]),
                        word_float(g_vdata_words[reg][1]),
                        word_float(g_vdata_words[reg][2]), word_float(value));
    }
}

/* The transform program memory, as the push buffer fills it: SET_TRANSFORM_
 * PROGRAM_LOAD (0x1E9C) points at a slot, SET_TRANSFORM_PROGRAM (0x0B00..
 * 0x0B7C) streams four words per instruction into it, and SET_TRANSFORM_
 * PROGRAM_START (0x1EA0) says where the next draw begins. FP loads its
 * skinning programs this way, itself, so the host kept transforming its
 * characters with whatever program the XDK had last selected: palette-skinned
 * vertices (a bone offset of -24/-28/-32 in v0) went through a rigid
 * program, and bodies, the character-select models and the first-person
 * shotgun came apart. A slot loaded before the first walk is unknown. */
static uint32_t g_prog_mem[NV2A_PROGRAM_SLOTS][4], g_prog_words[4];
static uint8_t  g_prog_known[NV2A_PROGRAM_SLOTS];
static uint32_t g_prog_load, g_prog_start = NV2A_PROGRAM_SLOTS;

static void track_program(uint32_t method, uint32_t value)
{
    if (method == 0x1E9Cu) {
        g_prog_load = value;
    } else if (method == 0x1EA0u) {
        g_prog_start = value;
    } else {
        uint32_t k = ((method - 0x0B00u) / 4u) & 3u;
        g_prog_words[k] = value;
        if (k == 3u) {
            if (g_prog_load < NV2A_PROGRAM_SLOTS) {
                memcpy(g_prog_mem[g_prog_load], g_prog_words, sizeof g_prog_words);
                g_prog_known[g_prog_load] = 1;
            }
            g_prog_load++;
        }
    }
}

/* The program the next draw runs: from the start slot to the instruction
 * with the final bit (bit 96, word 3 bit 0). 0 when any of it is unknown. */
int hle_d3d8_push_program(const uint32_t **microcode, int *count)
{
    uint32_t slot;

    if (g_prog_start >= NV2A_PROGRAM_SLOTS)
        return 0;
    for (slot = g_prog_start; slot < NV2A_PROGRAM_SLOTS; slot++) {
        if (!g_prog_known[slot])
            return 0;
        if (g_prog_mem[slot][3] & 1u) {
            *microcode = g_prog_mem[g_prog_start];
            *count = (int)(slot - g_prog_start + 1u);
            return 1;
        }
    }
    return 0;
}

static void track(uint32_t method, uint32_t value)
{
    if (method == 0x1E9Cu || method == 0x1EA0u || (method >= 0x0B00u && method < 0x0B80u)) {
        track_program(method, value);
        return;
    }
    if (method >= 0x1880u && method < 0x1B00u) {
        track_vertex_data(method, value);
        return;
    }
    if (method >= 0x1720u && method < 0x1720u + 4u * NV_ATTRS)
        g_arr_offset[(method - 0x1720u) / 4u] = value;
    else if (method >= 0x1760u && method < 0x1760u + 4u * NV_ATTRS)
        g_arr_format[(method - 0x1760u) / 4u] = value;
    else if (method == 0x17FCu) {
        g_in_begin = value != 0u;
        if (value)
            g_hw_begins++;
    } else if (method == 0x1EA4u)
        g_const_load = value;
    else if (method >= 0x0B80u && method < 0x0C00u) {
        uint32_t k = ((method - 0x0B80u) / 4u) & 3u;
        g_const_words[k] = value;
        if (k == 3u) {
            if (g_const_load < NV2A_VS_MAX_CONSTANTS && hle_d3d8_shadow_device()) {
                float f[4];
                memcpy(f, g_const_words, sizeof f);
                host_vsh_set_constant((int)g_const_load, f, 1);
                g_const_regs++;
            }
            g_const_load++;
        }
    }
}

/* Walk [p, end) as NV2A method packets, a header with the method in its low
 * bits, a count above them, and bit 30 for "same method". Returns where it
 * stopped: `end`, or the word that was a jump (*jump set to its target) or
 * could not be read as a packet (*jump left 0). */
static uint32_t walk(uint32_t p, uint32_t end, uint32_t *jump)
{
    *jump = 0;
    while (p + 4u <= end) {
        uint32_t w;

        if (!readable(p, 4u))
            return p;
        w = HLE_MEM32(p);
        uint32_t method = w & 0x1FFCu, count = (w >> 18) & 0x7FFu, i;
        int non_inc = (w & 0x40000000u) != 0;

        if (!w) {                       /* padding */
            p += 4u;
            continue;
        }
        if ((w & 3u) == 1u) {           /* old-style jump: address in bits 2..28 */
            *jump = CONTIG_BASE | (w & 0x1FFFFFFCu);
            return p;
        }
        if ((w & 3u) || p + 4u + count * 4u > end || !readable(p, 4u + count * 4u))
            return p;
        for (i = 0; i < count; i++) {
            uint32_t m = non_inc ? method : method + i * 4u, v = HLE_MEM32(p + 4u + i * 4u);
            track(m, v);
            if (dump_enabled())
                note_method(m, v);
        }
        g_words += 1u + count;
        p += 4u + count * 4u;
    }
    return p;
}

/* Bring the tracked state up to the device's push pointer. */
void hle_d3d8_push_sync(void)
{
    static uint32_t pos;
    uint32_t cur, device, jump, at;
    int hops;

    if (!hle_var_D3D_g_pDevice)
        return;
    device = HLE_MEM32(hle_var_D3D_g_pDevice);
    if (!device || !readable(device, 4u))
        return;
    cur = HLE_MEM32(device);
    if (!readable(cur, 4u) || cur == pos) {
        if (dump_enabled())
            report();
        return;
    }
    g_syncs++;
    if (!pos) {
        pos = cur;
        return;
    }
    at = pos;
    for (hops = 0; hops < 8 && at != cur; hops++) {
        uint32_t stop;

        if (cur > at && cur - at <= 0x40000u) {
            stop = walk(at, cur, &jump);
        } else {                        /* the buffer wrapped: run to its jump */
            stop = walk(at, at + 0x100000u, &jump);
            if (!jump) {
                g_lost++;
                at = cur;
                break;
            }
        }
        if (jump) {
            at = jump;
            continue;
        }
        if (stop != cur) {              /* an unreadable packet; resume at the end */
            g_lost++;
            at = cur;
        } else {
            at = stop;
        }
    }
    pos = cur;
    if (dump_enabled())
        report();
}

/* void D3DDevice_EndPush(DWORD *pPush) -- stores pPush as the device's push
 * pointer. BeginPush is not replaced: it tail-calls the XDK's space
 * reservation and has no single `ret N`. */
HLE_EXPORT(D3DDevice_EndPush)
{
    if (!hle_original_D3DDevice_EndPush)
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_EndPush);
    hle_d3d8_push_sync();
}

/* RECOMP_HLE_D3D8_ARRAY_CHECK=1: at each draw, compare the declaration the
 * shadow renderer reads from the program object with the vertex arrays the
 * push buffer actually enabled, and print each distinct disagreement once.
 * An attribute the hardware fetches but the declaration lacks reaches the
 * host program as a stale constant value. */
void hle_d3d8_push_check_declaration(uint32_t object)
{
    static int on = -1, said;
    static uint32_t seen[64][2];
    char line[512];
    uint32_t i, hw = 0, decl = 0;
    int n, k;

    if (on < 0)
        on = getenv("RECOMP_HLE_D3D8_ARRAY_CHECK") != NULL;
    if (!on || !object || said >= 64)
        return;
    for (i = 0; i < NV_ATTRS; i++) {
        uint32_t dfmt = HLE_MEM32(object + 20u + i * 16u + 8u);
        if ((g_arr_format[i] & 0xF0u) != 0u)
            hw |= 1u << i;
        if (dfmt > 2u)
            decl |= 1u << i;
    }
    if (hw == decl)
        return;
    for (k = 0; k < said; k++)
        if (seen[k][0] == hw && seen[k][1] == decl)
            return;
    seen[said][0] = hw;
    seen[said][1] = decl;
    said++;
    n = snprintf(line, sizeof line, "[HLE-D3D8] arrays %04X vs declaration %04X:", hw, decl);
    for (i = 0; i < NV_ATTRS && n < (int)sizeof line - 48; i++) {
        uint32_t dfmt = HLE_MEM32(object + 20u + i * 16u + 8u);
        if (((hw ^ decl) >> i) & 1u)
            n += snprintf(line + n, sizeof line - (size_t)n, " v%u hw=%08X@%08X decl=%02X+%u",
                          i, g_arr_format[i], g_arr_offset[i], dfmt,
                          HLE_MEM32(object + 20u + i * 16u + 4u));
    }
    fprintf(stderr, "%s\n", line);
}

/* Bytes an X_D3DVSDT / NV2A array format occupies per element. */
static uint32_t format_bytes(uint32_t fmt)
{
    static const uint8_t per[8] = { 1, 2, 4, 0, 1, 2, 4, 0 };
    uint32_t type = fmt & 0xFu, count = (fmt >> 4) & 0xFu;

    if (type == 6u)
        return 4u;                  /* CMP / NORMPACKED3 */
    return per[type & 7u] * count;
}

/* After an array draw the hardware leaves each enabled attribute's current
 * value at the last vertex it fetched (xemu's pgraph models it the same way),
 * so a later draw that leaves the register unfed reads that. FP colours its
 * first-person weapon glow from v2 with no colour array: on the console it
 * inherits the colour of the draw before; on the host it read (0, 0, 0, 1)
 * and the glow was a black blob in the middle of the screen. `last` is the
 * index of the last vertex, as the arrays count it. */
void hle_d3d8_push_after_draw(uint32_t last)
{
    static float cur[NV_ATTRS][4];
    uint32_t i;

    if (!hle_d3d8_shadow_device())
        return;
    for (i = 0; i < NV_ATTRS; i++) {
        uint32_t fmt = g_arr_format[i], type = fmt & 0xFu, count = (fmt >> 4) & 0xFu;
        uint32_t stride = (fmt >> 8) & 0xFFu, c;
        uint64_t at = (uint64_t)CONTIG_BASE + (g_arr_offset[i] & (CONTIG_SIZE - 1u)) +
                      (uint64_t)last * stride;
        const uint8_t *p;
        float f[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        if (!count || count > 4u || at + 16u > (uint64_t)CONTIG_BASE + CONTIG_SIZE)
            continue;
        p = (const uint8_t *)HLE_PTR((uint32_t)at);
        switch (type) {
        case 0u:                            /* UB_D3D: B, G, R, A in memory */
            f[0] = p[2] / 255.0f; f[1] = p[1] / 255.0f;
            f[2] = p[0] / 255.0f; f[3] = p[3] / 255.0f;
            break;
        case 4u:                            /* UB_OGL */
            for (c = 0; c < count; c++)
                f[c] = p[c] / 255.0f;
            break;
        case 1u:                            /* S1: normalised shorts */
        case 5u:                            /* S32K: shorts as they are */
            for (c = 0; c < count; c++) {
                int16_t v;
                memcpy(&v, p + 2u * c, sizeof v);
                f[c] = type == 1u ? v / 32767.0f : (float)v;
            }
            break;
        case 2u:                            /* F */
            memcpy(f, p, count * sizeof f[0]);
            break;
        case 6u: {                          /* CMP: 11:11:10 signed */
            uint32_t bits;
            memcpy(&bits, p, sizeof bits);
            f[0] = (float)((int32_t)(bits << 21) >> 21) / 1023.0f;
            f[1] = (float)((int32_t)(bits << 10) >> 21) / 1023.0f;
            f[2] = (float)((int32_t)bits >> 22) / 511.0f;
            break;
        }
        default:
            continue;
        }
        if (memcmp(cur[i], f, sizeof f) == 0)
            continue;
        memcpy(cur[i], f, sizeof f);
        host_vsh_set_vertex_data((int)i, f);
    }
}

/* RECOMP_HLE_D3D8_ARRAY_TRACE_AFTER=<ms> moves the trace window (45 s). */
static uint64_t trace_after_ms(void)
{
    const char *e = getenv("RECOMP_HLE_D3D8_ARRAY_TRACE_AFTER");
    return e ? (uint64_t)strtoul(e, NULL, 10) : 45000u;
}

/* From hle_d3d8.c: the guest vertex shader object of the program selected
 * now, whose 16 attribute slots at +20 hold the declaration; 0 if none. */
uint32_t hle_d3d8_shadow_program_object(void);

/* The vertices [first, first + vertices) of the title's own arrays, laid out
 * as the selected program's declaration says one interleaved vertex looks, so
 * the shadow draw reads them like any stream 0 vertex.
 *
 * Returns NULL, leaving the stream 0 path to it, when every declared
 * attribute's array is exactly where the stream 0 view says: offset
 * stream_base + its declared offset, stride stream_stride. That used to be
 * "unless the declared vertex is larger than stream_stride", which let a
 * stale stream 0 with a wide enough stride stand in for arrays the title set
 * itself: FP's blob shadows (stride 40 under a 28-byte declaration) took
 * their positions from unit normals and floated in front of the camera.
 * Also NULL when there is no program, an array runs outside the contiguous
 * window, or no array stands behind a declared register. The buffer is the
 * caller's until the next call on this thread. */
const void *hle_d3d8_title_vertices(uint32_t first, uint32_t vertices,
                                    uint32_t stream_base, uint32_t stream_stride,
                                    uint32_t *stride_out)
{
    static __declspec(thread) uint8_t *buf;
    static __declspec(thread) size_t cap;
    uint32_t object = hle_d3d8_shadow_program_object(), extent = 0, stride, i, v;
    uint32_t doff[NV_ATTRS], dsize[NV_ATTRS];
    size_t need;

    if (!object && getenv("RECOMP_HLE_D3D8_ARRAY_TRACE") &&
        (uint32_t)atoi(getenv("RECOMP_HLE_D3D8_ARRAY_TRACE")) == stream_stride) {
        static int said;
        if (said++ < 20)
            fprintf(stderr, "[HLE-D3D8] array trace: %u vertices, stride %u, no program object\n",
                    vertices, stream_stride);
    }
    if (!object || !vertices)
        return NULL;
    for (i = 0; i < NV_ATTRS; i++) {
        uint32_t attr = object + 20u + i * 16u, fmt = HLE_MEM32(attr + 8u);

        doff[i] = HLE_MEM32(attr + 4u);
        dsize[i] = fmt > 2u ? format_bytes(fmt) : 0u;
        if (dsize[i] && doff[i] + dsize[i] > extent)
            extent = doff[i] + dsize[i];
    }
    hle_d3d8_push_sync();
    /* RECOMP_HLE_D3D8_ARRAY_TRACE=<stride>: for draws whose stream 0 stride
     * or declared vertex size is that (1: every draw), from 45 s in, print the declaration beside the arrays the hardware has. */
    {
        static int trace = -1, said;
        if (trace < 0) {
            const char *e = getenv("RECOMP_HLE_D3D8_ARRAY_TRACE");
            trace = e ? atoi(e) : 0;
            g_trace_t0 = GetTickCount64();
        }
        if (trace && (trace == 1 || (uint32_t)trace == stream_stride ||
                      (uint32_t)trace == ((extent + 3u) & ~3u)) &&
            said < 400 && GetTickCount64() - g_trace_t0 > trace_after_ms()) {
            said++;
            fprintf(stderr, "[HLE-D3D8] array trace: %u vertices from %u, stream 0 at "
                    "0x%07X stride %u, program 0x%08X\n", vertices, first, stream_base,
                    stream_stride, object);
            for (i = 0; i < NV_ATTRS; i++)
                if (dsize[i] || (g_arr_format[i] & 0xFFu) > 2u)
                    fprintf(stderr, "[HLE-D3D8]   v%-2u decl +%u fmt %02X | hw 0x%07X "
                            "fmt %08X (stride %u)\n", i, doff[i],
                            HLE_MEM32(object + 20u + i * 16u + 8u),
                            g_arr_offset[i] & (CONTIG_SIZE - 1u), g_arr_format[i],
                            (g_arr_format[i] >> 8) & 0xFFu);
        }
    }
    if (!extent || extent > 256u)
        return NULL;
    if (extent <= stream_stride && stream_base != ~0u) {
        int same = 1;
        for (i = 0; i < NV_ATTRS && same; i++) {
            uint32_t afmt = g_arr_format[i];
            if (!dsize[i] || (afmt & 0xFFu) <= 2u)
                continue;
            if (((afmt >> 8) & 0xFFu) != stream_stride ||
                (g_arr_offset[i] & (CONTIG_SIZE - 1u)) !=
                    ((stream_base + doff[i]) & (CONTIG_SIZE - 1u)))
                same = 0;
        }
        if (same)
            return NULL;
        g_title_moved++;
    }
    stride = (extent + 3u) & ~3u;
    need = (size_t)stride * vertices;
    if (need > cap) {
        uint8_t *grown = (uint8_t *)realloc(buf, need);
        if (!grown)
            return NULL;
        buf = grown;
        cap = need;
    }
    memset(buf, 0, need);
    for (i = 0; i < NV_ATTRS; i++) {
        uint32_t afmt = g_arr_format[i], astride = (afmt >> 8) & 0xFFu;
        uint32_t esize = format_bytes(afmt), n = dsize[i] < esize ? dsize[i] : esize;
        uint64_t base, last;

        if (!dsize[i] || !n || (afmt & 0xFFu) <= 2u)
            continue;               /* not declared, or no array behind it */
        base = (uint64_t)CONTIG_BASE + (g_arr_offset[i] & (CONTIG_SIZE - 1u));
        last = base + (uint64_t)(first + vertices - 1u) * astride + n;
        if (last > (uint64_t)CONTIG_BASE + CONTIG_SIZE) {
            g_title_missing++;
            return NULL;
        }
        for (v = 0; v < vertices; v++)
            memcpy(buf + (size_t)v * stride + doff[i],
                   HLE_PTR((uint32_t)(base + (uint64_t)(first + v) * astride)), n);
    }
    g_title_draws++;
    *stride_out = stride;
    return buf;
}

#endif /* _WIN32 */
