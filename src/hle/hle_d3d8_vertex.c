/*
 * hle_d3d8_vertex.c -- vertex buffer draws and vertex shader constants,
 * forwarded to the shadow device.
 *
 * Part of shadow mode (hle_d3d8.c, RECOMP_HLE_D3D8=shadow). Every replacement
 * runs the title's own body first.
 *
 * A vertex buffer is a guest X_D3DVertexBuffer (Common, Data, Lock): Data is
 * physical, so the vertices are read at guest 0x80000000 + Data, like texels
 * (Cxbx-Reloaded, GetDataFromXboxResource). Rather than mirror buffers on the
 * host, each draw hands the vertices it needs to the host's UP draws, the same
 * path DrawVerticesUP takes (hle_d3d8.c).
 *
 * DrawIndexedVertices takes a pointer to 16-bit indices and adds the base
 * vertex index set with SetIndices (Cxbx-Reloaded, Direct3D9.cpp). Burnout 2's
 * XDK symbols do not name SetIndices, but every DrawIndexedVertices calls
 * CDevice_SetStateVB with it (7,188 of each in a profiled minute): the lifted
 * DrawIndexedVertices pushes the device's base index, and SetStateVB multiplies
 * its argument by the stride and adds the buffer's Data when it sets the
 * vertex array offsets. So the base is captured there. A title whose symbols
 * do not name that function -- or name only the stdcall CDevice_SetStateVB_8,
 * `this` on the stack -- never updates it; the first indexed buffer draw
 * without it says so once, and draws with base 0.
 *
 * The host binds one stream, stream 0. A vertex program that reads another
 * one -- Outrun 2's road takes v13 from stream 1 -- has those registers copied
 * in beside each stream 0 vertex (shadow_expand_vertices in hle_d3d8.c),
 * which finds them through hle_d3d8_stream_vertices below at the first
 * vertex each buffer draw passes along. FVF draws use only stream 0.
 *
 * Vertex shader constants arrive through fastcall setters: register in ecx,
 * data in edx, and for the NotInline pair a count of floats on the stack.
 * The register is already 0..191, the host's range; Cxbx-Reloaded subtracts
 * 96 on the way in only because its shared setter adds it back.
 *
 * XDK 3925 has only that shared setter, D3DDevice_SetVertexShaderConstant,
 * and it is the exception to the line above: its register is -96-based and
 * the XDK biases it itself, so the replacement at the bottom of this file
 * adds 96 to reach the same range the fastcall ones already arrive in.
 */
#include "platform/xbox_winnt.h"
#include <stdio.h>
#include <string.h>
#include "hle.h"

#if HLE_HOST_RENDERER
#include "d3d8_xbox.h"
#include "d3d8_vsh.h"
#include "hle_d3d8_record.h"

/* From hle_d3d8.c. */
IDirect3DDevice8 *hle_d3d8_shadow_device(void);
void hle_d3d8_shadow_draw(uint32_t xpt, uint32_t count, const void *verts,
                          uint32_t stride, int from_buffer);
void hle_d3d8_shadow_draw_indexed(uint32_t xpt, uint32_t count, const uint16_t *idx,
                                  const void *verts, uint32_t stride, int from_buffer);
void hle_d3d8_shadow_set_first_vertex(uint32_t first);
/* From hle_d3d8_push.c: the vertex arrays the title (or the XDK) wrote to the
 * push buffer, gathered into the declaration's layout when the stream 0 view
 * does not describe them. stream_base is the contiguous-memory offset the
 * stream 0 view puts vertex 0 at, ~0u if there is none. */
const void *hle_d3d8_title_vertices(uint32_t first, uint32_t vertices,
                                    uint32_t stream_base, uint32_t stream_stride,
                                    uint32_t *stride);
/* From hle_d3d8_push.c: leave the enabled attributes' current values at the
 * draw's last vertex, as the hardware does. */
void hle_d3d8_push_after_draw(uint32_t last);

#define CONTIG_BASE 0x80000000u
#define CONTIG_SIZE (64u * 1024u * 1024u)    /* kernel.h XBOX_CONTIG_SIZE */
#define MAX_CONSTANT_REGISTERS 192

static uint32_t g_stream0_vb;        /* guest X_D3DVertexBuffer */
static uint32_t g_stream0_stride;
static uint32_t g_stream_vb[16], g_stream_stride[16];   /* every stream, 0 included */
static uint32_t g_base_vertex;
static int      g_base_vertex_seen;  /* CDevice_SetStateVB has run */
static int      g_in_notinline;      /* inside SetVertexShaderConstantNotInline */
static unsigned long g_skip_no_stream, g_skip_range;

/* Title RAM or the contiguous window. */
static int guest_readable(uint32_t va, uint64_t bytes)
{
    if (va >= 0x00010000u && (uint64_t)va + bytes <= g_xbox_total_ram)
        return 1;
    return va >= CONTIG_BASE && (uint64_t)va + bytes <= (uint64_t)CONTIG_BASE + CONTIG_SIZE;
}

/* Host pointer to vertex `first` of the stream 0 buffer, for `vertices`
 * vertices, or NULL (counted) if there is no buffer or it would read outside
 * the contiguous window. */
/* Where the stream 0 view puts vertex `first`, as an offset into contiguous
 * memory -- what the XDK writes to the NV2A array offsets -- or ~0u. */
static uint32_t stream0_base(uint32_t first)
{
    uint32_t data;

    if (!g_stream0_vb || !g_stream0_stride || !guest_readable(g_stream0_vb, 12u))
        return ~0u;
    data = HLE_MEM32(g_stream0_vb + 4u);
    if (!data)
        return ~0u;
    return (uint32_t)(((uint64_t)(data & (CONTIG_SIZE - 1u)) +
                       (uint64_t)first * g_stream0_stride) & (CONTIG_SIZE - 1u));
}

static const void *stream0_vertices(uint32_t first, uint32_t vertices)
{
    uint32_t data, va;
    uint64_t start, bytes;

    if (!g_stream0_vb || !g_stream0_stride || !guest_readable(g_stream0_vb, 12u)) {
        g_skip_no_stream++;
        return NULL;
    }
    data = HLE_MEM32(g_stream0_vb + 4u);
    /* Where the title's own D3DVertexBuffer_Lock2 hands out the vertices. */
    va = data | CONTIG_BASE;
    start = (uint64_t)(va - CONTIG_BASE) + (uint64_t)first * g_stream0_stride;
    bytes = (uint64_t)vertices * g_stream0_stride;
    if (!data || start + bytes > CONTIG_SIZE) {
        g_skip_range++;
        return NULL;
    }
    return HLE_PTR(CONTIG_BASE + (uint32_t)start);
}

/* Host pointer to vertex `first` of any stream's buffer, for `vertices`
 * vertices, with its stride; NULL if there is none or it would read outside
 * the contiguous window. For the registers a program reads from streams other
 * than 0, found at the same index as the stream 0 vertex they go with. */
const void *hle_d3d8_stream_vertices(uint32_t stream, uint32_t first, uint32_t vertices,
                                     uint32_t *stride)
{
    uint32_t vb, data;
    uint64_t start, bytes;

    if (stream >= 16u)
        return NULL;
    vb = g_stream_vb[stream];
    *stride = g_stream_stride[stream];
    if (!vb || !*stride || !guest_readable(vb, 12u))
        return NULL;
    data = HLE_MEM32(vb + 4u);
    start = (uint64_t)((data | CONTIG_BASE) - CONTIG_BASE) + (uint64_t)first * *stride;
    bytes = (uint64_t)vertices * *stride;
    if (!data || start + bytes > CONTIG_SIZE)
        return NULL;
    return HLE_PTR(CONTIG_BASE + (uint32_t)start);
}

static void report(void)
{
    static DWORD last;
    DWORD now = GetTickCount();

    if (!last) {
        last = now;
    } else if (now - last >= 5000) {
        fprintf(stderr, "[HLE-D3D8] shadow buffers: skipped %lu with no stream 0 buffer, "
                "%lu out of range\n", g_skip_no_stream, g_skip_range);
        last = now;
    }
}
#endif /* HLE_HOST_RENDERER */

HLE_ORIGINAL(D3DDevice_SetStreamSource);
HLE_ORIGINAL(CDevice_SetStateVB);
HLE_ORIGINAL(D3DDevice_DrawVertices);
HLE_ORIGINAL(D3DDevice_DrawIndexedVertices);
/* The one generic setter, on XDKs that predate the specialised forms. */
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstant);
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstant1);
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstant4);
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstantNotInline);
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstantNotInlineFast);

static void first_call(int *seen, const char *name, uint32_t arg)
{
    if (!*seen) {
        *seen = 1;
        fprintf(stderr, "[HLE] %s(0x%X) replaced by name\n", name, arg);
        fflush(stderr);
    }
}

static int original_missing(void (*fn)(void), const char *name)
{
    if (fn)
        return 0;
    fprintf(stderr, "[HLE] %s: original body missing -- regenerate the lift\n", name);
    return 1;
}

/* void D3DDevice_SetStreamSource(UINT StreamNumber,
 *     D3DVertexBuffer *pStreamData, UINT Stride)                            */
HLE_EXPORT(D3DDevice_SetStreamSource)
{
    static int seen;
    uint32_t stream = HLE_ARG(0);
#if HLE_HOST_RENDERER
    uint32_t vb = HLE_ARG(1), stride = HLE_ARG(2);
#endif

    first_call(&seen, "D3DDevice_SetStreamSource", stream);
    if (original_missing(hle_original_D3DDevice_SetStreamSource, "D3DDevice_SetStreamSource"))
        HLE_RETURN(0x80004005u);
    HLE_CALL_ORIGINAL(D3DDevice_SetStreamSource);
#if HLE_HOST_RENDERER
    if (stream == 0u) {
        g_stream0_vb = vb;
        g_stream0_stride = stride;
    }
    if (stream < 16u) {
        g_stream_vb[stream] = vb;
        g_stream_stride[stream] = stride;
    }
#endif
}

/* void CDevice::SetStateVB(DWORD BaseVertexIndex) -- thiscall, D3D internal,
 * called by every DrawIndexedVertices. */
HLE_EXPORT(CDevice_SetStateVB)
{
    static int seen;
    uint32_t base = HLE_ARG(0);

    first_call(&seen, "CDevice_SetStateVB", base);
    if (original_missing(hle_original_CDevice_SetStateVB, "CDevice_SetStateVB"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(CDevice_SetStateVB);
#if HLE_HOST_RENDERER
    g_base_vertex = base;
    g_base_vertex_seen = 1;
#endif
}

/* void D3DDevice_DrawVertices(D3DPRIMITIVETYPE PrimitiveType,
 *     UINT StartVertex, UINT VertexCount)                                   */
HLE_EXPORT(D3DDevice_DrawVertices)
{
    static int seen;
    uint32_t xpt = HLE_ARG(0);
#if HLE_HOST_RENDERER
    uint32_t start = HLE_ARG(1), count = HLE_ARG(2);
#endif

    first_call(&seen, "D3DDevice_DrawVertices", xpt);
    if (original_missing(hle_original_D3DDevice_DrawVertices, "D3DDevice_DrawVertices"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_DrawVertices);
#if HLE_HOST_RENDERER
    if (hle_d3d8_shadow_device() && count) {
        uint32_t tstride = 0;
        const void *verts = hle_d3d8_title_vertices(start, count, stream0_base(0u),
                                                    g_stream0_stride, &tstride);
        if (verts) {
            /* The arrays the title set itself, in the declaration's layout. */
            hle_d3d8_shadow_draw(xpt, count, verts, tstride, 1);
        } else if ((verts = stream0_vertices(start, count)) != NULL) {
            hle_d3d8_shadow_set_first_vertex(start);
            hle_d3d8_shadow_draw(xpt, count, verts, g_stream0_stride, 1);
        }
        hle_d3d8_push_after_draw(start + count - 1u);
        report();
    }
#endif
}

/* void D3DDevice_DrawIndexedVertices(D3DPRIMITIVETYPE PrimitiveType,
 *     UINT VertexCount, const WORD *pIndexData)                             */
HLE_EXPORT(D3DDevice_DrawIndexedVertices)
{
    static int seen;
    uint32_t xpt = HLE_ARG(0);
#if HLE_HOST_RENDERER
    uint32_t count = HLE_ARG(1), index_va = HLE_ARG(2);
#endif

    first_call(&seen, "D3DDevice_DrawIndexedVertices", xpt);
    if (original_missing(hle_original_D3DDevice_DrawIndexedVertices,
                         "D3DDevice_DrawIndexedVertices"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_DrawIndexedVertices);
#if HLE_HOST_RENDERER
    if (hle_d3d8_shadow_device() && count && index_va &&
        guest_readable(index_va, (uint64_t)count * 2u)) {
        const uint16_t *idx = (const uint16_t *)HLE_PTR(index_va);
        uint32_t i, vertices = 0;
        const void *verts;

        for (i = 0; i < count; i++)
            if ((uint32_t)idx[i] + 1u > vertices)
                vertices = (uint32_t)idx[i] + 1u;
        if (!g_base_vertex_seen) {
            static int said;
            if (!said++)
                fprintf(stderr, "[HLE-D3D8] shadow buffers: CDevice_SetStateVB never "
                        "ran; indexed buffer draws use base vertex 0\n");
        }
        uint32_t tstride = 0;
        verts = hle_d3d8_title_vertices(0, vertices, stream0_base(g_base_vertex),
                                        g_stream0_stride, &tstride);
        if (verts) {
            hle_d3d8_shadow_draw_indexed(xpt, count, idx, verts, tstride, 1);
        } else if ((verts = stream0_vertices(g_base_vertex, vertices)) != NULL) {
            hle_d3d8_shadow_set_first_vertex(g_base_vertex);
            hle_d3d8_shadow_draw_indexed(xpt, count, idx, verts, g_stream0_stride, 1);
        }
        hle_d3d8_push_after_draw(idx[count - 1u]);
        report();
    }
#endif
}

#if HLE_HOST_RENDERER
static void forward_constants(uint32_t reg, uint32_t data, uint32_t count)
{
    if (!hle_d3d8_shadow_device() || !data || !count || reg >= MAX_CONSTANT_REGISTERS)
        return;
    if (count > MAX_CONSTANT_REGISTERS - reg)
        count = MAX_CONSTANT_REGISTERS - reg;
    if (!guest_readable(data, (uint64_t)count * 16u))
        return;
    host_vsh_set_constant((int)reg, (const float *)HLE_PTR(data), (int)count);
}
#endif

/* void __fastcall D3DDevice_SetVertexShaderConstant1(int Register,
 *     const void *pConstantData) -- one register.                           */
HLE_EXPORT(D3DDevice_SetVertexShaderConstant1)
{
    static int seen;
    uint32_t reg = g_ecx;
#if HLE_HOST_RENDERER
    uint32_t data = g_edx;
#endif

    first_call(&seen, "D3DDevice_SetVertexShaderConstant1", reg);
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstant1,
                         "D3DDevice_SetVertexShaderConstant1"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstant1);
#if HLE_HOST_RENDERER
    forward_constants(reg, data, 1u);
#endif
}

/* void __fastcall D3DDevice_SetVertexShaderConstant1Fast(int Register,
 *     const void *pConstantData) -- one register, without the checks.
 *
 * Some builds have only this form of the single-register setter, beside
 * NotInlineFast: XGRA, Doom 3 and Breakdown name no other, and Outrun 2 has
 * both. Unreplaced, every constant a title set this way stayed zero on the
 * host, and a program that transforms its position by one drew nothing --
 * XGRA's movie quad reached the host every frame and came out black. */
HLE_ORIGINAL(D3DDevice_SetVertexShaderConstant1Fast);
HLE_EXPORT(D3DDevice_SetVertexShaderConstant1Fast)
{
    static int seen;
    uint32_t reg = g_ecx;
#if HLE_HOST_RENDERER
    uint32_t data = g_edx;
#endif

    first_call(&seen, "D3DDevice_SetVertexShaderConstant1Fast", reg);
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstant1Fast,
                         "D3DDevice_SetVertexShaderConstant1Fast"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstant1Fast);
#if HLE_HOST_RENDERER
    forward_constants(reg, data, 1u);
#endif
}

/* void __fastcall D3DDevice_SetVertexShaderConstant4(int Register,
 *     const void *pConstantData) -- four registers.                         */
HLE_EXPORT(D3DDevice_SetVertexShaderConstant4)
{
    static int seen;
    uint32_t reg = g_ecx;
#if HLE_HOST_RENDERER
    uint32_t data = g_edx;
#endif

    first_call(&seen, "D3DDevice_SetVertexShaderConstant4", reg);
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstant4,
                         "D3DDevice_SetVertexShaderConstant4"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstant4);
#if HLE_HOST_RENDERER
    forward_constants(reg, data, 4u);
#endif
}

/* void __fastcall D3DDevice_SetVertexShaderConstantNotInline(int Register,
 *     const void *pConstantData, DWORD ConstantCount) -- ConstantCount counts
 * floats (Cxbx-Reloaded divides it by 4). A count that is not a multiple of
 * four drops the last, partial register here; the XDK passes whole ones.
 *
 * Burnout 2's NotInline calls NotInlineFast (through its thunk), so the inner
 * call does not forward and this one does, once. */
HLE_EXPORT(D3DDevice_SetVertexShaderConstantNotInline)
{
    static int seen;
    uint32_t reg = g_ecx;
#if HLE_HOST_RENDERER
    uint32_t data = g_edx, floats = HLE_ARG(0);
#endif

    first_call(&seen, "D3DDevice_SetVertexShaderConstantNotInline", reg);
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstantNotInline,
                         "D3DDevice_SetVertexShaderConstantNotInline"))
        HLE_RETURN(0u);
#if HLE_HOST_RENDERER
    g_in_notinline++;
#endif
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstantNotInline);
#if HLE_HOST_RENDERER
    g_in_notinline--;
    forward_constants(reg, data, floats / 4u);
#endif
}

/* The same, the XDK's faster variant.                                        */
HLE_EXPORT(D3DDevice_SetVertexShaderConstantNotInlineFast)
{
    static int seen;
    uint32_t reg = g_ecx;
#if HLE_HOST_RENDERER
    uint32_t data = g_edx, floats = HLE_ARG(0);
#endif

    first_call(&seen, "D3DDevice_SetVertexShaderConstantNotInlineFast", reg);
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstantNotInlineFast,
                         "D3DDevice_SetVertexShaderConstantNotInlineFast"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstantNotInlineFast);
#if HLE_HOST_RENDERER
    if (!g_in_notinline)
        forward_constants(reg, data, floats / 4u);
#endif
}

/* void __stdcall D3DDevice_SetVertexShaderConstant(INT Register,
 *     const void *pConstantData, DWORD ConstantCount)
 *
 * The one generic setter, on an XDK that predates the specialised forms.
 * Max Payne is XDK 3925: its D3D8 exports this and none of Constant1,
 * Constant4, NotInline or NotInlineFast, so a title there sets every vertex
 * shader constant through a function nothing replaced, and the host saw none
 * of them.
 *
 * ConstantCount counts registers here, not floats -- it is the plain D3D8
 * signature. NotInline's float count is the odd one out, not this.
 *
 * Register is -96-based and biased here, which the specialised forms are not.
 * 3925 starts `mov edx,[ebp+8]; add edx,0x60`, applying the bias itself, while
 * 4721's Constant4 writes its ecx straight into the push buffer and indexes
 * its shadow array with it unchanged. host_vsh_set_constant wants the same
 * 0-based index the specialised forms hand it, so the bias has to be added
 * here or every constant lands 96 registers low -- silently, because the
 * range check below would still pass for most of them.
 *
 * The arguments are logged for the first few calls: this has not yet been
 * seen to run (Max Payne reaches no vertex shader in the frames it renders),
 * so the log is the evidence that the reading above is right.
 */
#define XBOX_VSH_CONSTANT_BIAS 96u

HLE_EXPORT(D3DDevice_SetVertexShaderConstant)
{
    static int seen;
    uint32_t reg   = HLE_ARG(0) + XBOX_VSH_CONSTANT_BIAS;
    uint32_t data  = HLE_ARG(1);
    uint32_t count = HLE_ARG(2);

    first_call(&seen, "D3DDevice_SetVertexShaderConstant", reg);
    {
        static int notes;
        if (notes < 4) {
            notes++;
            fprintf(stderr, "[HLE-D3D8] SetVertexShaderConstant(reg=%d, data=0x%08X,"
                    " count=%u) -> host register %u; ecx=0x%08X edx=0x%08X\n",
                    (int)HLE_ARG(0), data, count, reg, g_ecx, g_edx);
            fflush(stderr);
        }
    }
    if (original_missing(hle_original_D3DDevice_SetVertexShaderConstant,
                         "D3DDevice_SetVertexShaderConstant"))
        HLE_RETURN(0u);
    HLE_CALL_ORIGINAL(D3DDevice_SetVertexShaderConstant);
#if HLE_HOST_RENDERER
    forward_constants(reg, data, count);
#endif
}
