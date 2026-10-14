/*
 * d3d8_display.c -- how big the host renders, and the resolve that puts
 * that image on the screen. See d3d8_display.h for why this works at all.
 *
 * The resolve is a box filter: at scale N every output pixel averages the
 * NxN scene pixels it was rasterised from, which is ordered-grid
 * supersampling and antialiases everything the scene contains --
 * geometry edges, alpha-tested cutouts and the title's own 2D layer
 * alike. The tap count is baked into the shader at compile time rather
 * than read from a constant, so the loop unrolls; the scale is fixed for
 * the life of the process, so there is nothing to recompile for.
 */
#include "d3d8_display.h"
#include "recomp_config.h"


#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define D3D8_DISPLAY_MAX_SCALE 8

/* ================================================================
 * Policy
 * ================================================================ */

/* RECOMP_RES_SCALE: a whole multiple of the guest's size (1..8), or the
 * height to render at -- "1080p", "1440", "4k". A height gives the scene
 * that many lines whatever the guest's own size; the width follows the
 * output shape. */
static int parse_resolution(const char *v, UINT *scale, UINT *target_height)
{
    char *endp = NULL;
    long n;

    if (!strcmp(v, "4k") || !strcmp(v, "4K") || !strcmp(v, "uhd") || !strcmp(v, "UHD")) {
        *target_height = 2160;
        return 1;
    }
    if (!strcmp(v, "hd") || !strcmp(v, "HD")) {
        *target_height = 720;
        return 1;
    }
    n = strtol(v, &endp, 10);
    if (endp == v)
        return 0;
    if (*endp == 'p' || *endp == 'P') {
        endp++;
    } else if (!*endp && n >= 1 && n <= D3D8_DISPLAY_MAX_SCALE) {
        *scale = (UINT)n;
        return 1;
    }
    if (*endp || n < 240 || n > 4320)
        return 0;
    *target_height = (UINT)n;
    return 1;
}

static D3D8DisplayPolicy g_policy;
static int               g_policy_read;

const D3D8DisplayPolicy *d3d8_display_policy(void)
{
    if (!g_policy_read) {
        const char *v = recomp_config_lookup("RECOMP_RES_SCALE", "resolution_scale");

        g_policy_read = 1;
        g_policy.scale = 1;
        g_policy.target_height = 0;

        if (v && *v && !parse_resolution(v, &g_policy.scale, &g_policy.target_height)) {
            fprintf(stderr, "D3D8 display: RECOMP_RES_SCALE=%s is neither a whole "
                    "number from 1 to %d nor a height (720p, 1080p, 1440p, 4k, "
                    "<n>p); rendering at the guest's own size\n",
                    v, D3D8_DISPLAY_MAX_SCALE);
            g_policy.scale = 1;
            g_policy.target_height = 0;
        }

        g_policy.anisotropy = 1;
        v = recomp_config_lookup("RECOMP_ANISO", "anisotropy");
        if (v && *v) {
            long a = strtol(v, NULL, 10);

            if (a < 1 || a > 16) {
                fprintf(stderr, "D3D8 display: RECOMP_ANISO=%s is not a whole number "
                        "from 1 to 16; leaving the title's own filtering\n", v);
                a = 1;
            }
            g_policy.anisotropy = (UINT)a;
        }

        g_policy.msaa = 1;
        v = recomp_config_lookup("RECOMP_MSAA", "msaa");
        if (v && *v) {
            long m = strtol(v, NULL, 10);

            if (m != 1 && m != 2 && m != 4 && m != 8) {
                fprintf(stderr, "D3D8 display: RECOMP_MSAA=%s is not 1, 2, 4 or 8; "
                        "no multisampling\n", v);
                m = 1;
            }
            g_policy.msaa = (UINT)m;
        }

        g_policy.widescreen = recomp_config_bool("RECOMP_WIDESCREEN", "widescreen", 0);
        if (g_policy.target_height)
            fprintf(stderr, "D3D8 display: rendering %u lines high, filtered to the "
                    "window at present\n", g_policy.target_height);
        else if (g_policy.scale > 1)
            fprintf(stderr, "D3D8 display: supersampling %ux, box filtered at present\n",
                    g_policy.scale);

        if (g_policy.widescreen)
            fprintf(stderr, "D3D8 display: widescreen, so the title's frame is presented "
                    "at 16:9. A title with no 16:9 mode of its own draws 4:3 and will "
                    "look stretched; leave this off for those.\n");
        if (g_policy.msaa > 1)
            fprintf(stderr, "D3D8 display: %ux multisampling asked for the scene\n",
                    g_policy.msaa);
        if (g_policy.anisotropy > 1)
            fprintf(stderr, "D3D8 display: anisotropic filtering forced to %ux where the "
                    "title filters linearly\n", g_policy.anisotropy);
    }
    return &g_policy;
}

void d3d8_display_scene_size(UINT guest_w, UINT guest_h,
                             UINT *scene_w, UINT *scene_h)
{
    const D3D8DisplayPolicy *p = d3d8_display_policy();
    UINT h = p->target_height ? p->target_height : guest_h * p->scale;
    UINT w = guest_w * p->scale;

    /* Wider than the guest's own shape when the target is a height or the
     * output is 16:9: every guest-to-host conversion has a factor per axis
     * (rt_scale_x/y, the screen copy, the clear rectangles), and the draw
     * paths end in normalised coordinates, so a 640x480 title re-rasterises
     * at 1920x1080 as readily as at 1280x960. That is what keeps pixels
     * square in widescreen -- an anamorphic 4:3 scene stretched at present
     * would throw away a quarter of the horizontal resolution. Unscaled 4:3
     * stays exactly the guest's size. */
    if (guest_h && (p->target_height || p->widescreen)) {
        uint64_t aw = p->widescreen ? 16 : guest_w, ah = p->widescreen ? 9 : guest_h;

        w = (UINT)(((uint64_t)h * aw + ah / 2) / ah);
        w = (w + 1) & ~1u;              /* even, so the centre is a pixel edge */
    }
    if (scene_w) *scene_w = w ? w : guest_w;
    if (scene_h) *scene_h = h ? h : guest_h;
}

void xbox_D3D8WindowSize(UINT guest_w, UINT guest_h, UINT *width, UINT *height)
{
    UINT sw, sh;

    d3d8_display_scene_size(guest_w, guest_h, &sw, &sh);
    *width = sw;
    *height = sh;
    /* Bigger than the console asked for only when the scene is, and then
     * no taller than 720 lines, which fits any desktop with room for the
     * window's frame; a 4K scene in a 2160-line window would not. */
    if (sh > guest_h && guest_h) {
        UINT h = sh < 720 ? sh : 720;

        if (h < guest_h)
            h = guest_h;
        *width = (UINT)(((uint64_t)sw * h + sh / 2) / sh);
        *height = h;
    }
}

void d3d8_display_output_shape(UINT scene_w, UINT scene_h,
                               UINT *shape_w, UINT *shape_h)
{
    /* The scene is built at the shape it is to be seen at (above), 16:9
     * included, so presenting it at its own shape is right. */
    if (shape_w) *shape_w = scene_w;
    if (shape_h) *shape_h = scene_h;
}

D3D8DisplayFit d3d8_display_fit(UINT shape_w, UINT shape_h, UINT bb_w, UINT bb_h)
{
    D3D8DisplayFit f;

    f.x = f.y = 0;
    f.w = bb_w;
    f.h = bb_h;
    if (!shape_w || !shape_h || !bb_w || !bb_h)
        return f;

    /* Compare shapes in integers: shape_w/shape_h against bb_w/bb_h. */
    if ((uint64_t)shape_w * bb_h > (uint64_t)bb_w * shape_h) {
        /* The window is taller than the picture: bars above and below. */
        f.h = (UINT)(((uint64_t)bb_w * shape_h) / shape_w);
        if (!f.h) f.h = 1;
        f.y = (bb_h - f.h) / 2;
    } else if ((uint64_t)shape_w * bb_h < (uint64_t)bb_w * shape_h) {
        /* The window is wider: bars to the left and right. */
        f.w = (UINT)(((uint64_t)bb_h * shape_w) / shape_h);
        if (!f.w) f.w = 1;
        f.x = (bb_w - f.w) / 2;
    }
    return f;
}

/* ================================================================
 * Resolve
 * ================================================================ */

/* origin: where the picture starts inside the back buffer, subtracted
 * because SV_Position counts from the buffer's corner and not the
 * viewport's. ratio: scene texels per output pixel. texel: one over the
 * scene size, for normalised sampling.
 *
 * TAPS x TAPS bilinear samples spread across each output pixel's
 * footprint. Bilinear rather than Load so a ratio that is not a whole
 * number -- which is most window sizes -- still resolves smoothly, and so
 * a window larger than the scene magnifies cleanly instead of blocking
 * up. At a whole-number ratio with taps to match, this is the box filter
 * that makes the supersampling exact. */
typedef struct {
    float origin_x, origin_y, ratio_x, ratio_y;
    float texel_x, texel_y, inv_taps, pad;
} ResolveConstants;

static const char kSource[] =
    "cbuffer Resolve : register(b7) { float4 p; float4 q; };\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2((id << 1) & 2, id & 2);\n"   /* one oversized triangle */
    "    VSOut o;\n"
    "    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> scene : register(t9);\n"
    "SamplerState smp : register(s9);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    float2 base = (i.pos.xy - p.xy) * p.zw;\n"
    "    float2 step = p.zw / TAPS;\n"
    "    float4 sum = 0;\n"
    "    [unroll] for (int y = 0; y < TAPS; y++)\n"
    "        [unroll] for (int x = 0; x < TAPS; x++)\n"
    "            sum += scene.SampleLevel(smp,\n"
    "                       (base + (float2(x, y) + 0.5) * step) * q.xy, 0);\n"
    "    return sum * q.z;\n"
    "}\n";

static struct {
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    ID3D11Buffer            *cb;
    ID3D11BlendState        *blend;
    ID3D11DepthStencilState *depth;
    ID3D11RasterizerState   *raster;
    ID3D11SamplerState      *sampler;
    UINT                     taps;       /* what the shaders were built for */
    int                      tried, failed;
} g;

static void fail(const char *what, HRESULT hr)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 display: %s failed (0x%08lX); the scene cannot be "
            "resolved and the window will stay blank\n", what, (unsigned long)hr);
    fflush(stderr);
}

static int compile_one(ID3D11Device *dev, const char *entry, const char *target,
                       const D3D_SHADER_MACRO *macros, void **out)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;

    hr = D3DCompile(kSource, sizeof kSource - 1, "display_resolve", macros, NULL,
                    entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) {
            fprintf(stderr, "D3D8 display: %s\n",
                    (const char *)ID3D10Blob_GetBufferPointer(err));
            ID3D10Blob_Release(err);
        }
        fail("D3DCompile", hr);
        return 0;
    }
    if (err) ID3D10Blob_Release(err);

    if (target[0] == 'v')
        hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
                                             ID3D10Blob_GetBufferSize(code), NULL,
                                             (ID3D11VertexShader **)out);
    else
        hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
                                            ID3D10Blob_GetBufferSize(code), NULL,
                                            (ID3D11PixelShader **)out);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) { fail("CreateShader", hr); return 0; }
    return 1;
}

static int create(UINT taps)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    D3D_SHADER_MACRO macros[2];
    char taps_text[16];
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bl;
    D3D11_DEPTH_STENCIL_DESC ds;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    if (g.failed) return 0;
    if (g.tried && g.taps == taps) return g.ps != NULL;
    if (!dev) { g.failed = 1; return 0; }

    /* The tap count follows the window, not the scale: a window of a
     * different shape from the scene gets a different ratio, and one
     * that is resized gets a new one mid-run. Rebuild the two shaders
     * for it and keep everything else. */
    if (g.tried) {
        if (g.vs) { ID3D11VertexShader_Release(g.vs); g.vs = NULL; }
        if (g.ps) { ID3D11PixelShader_Release(g.ps);  g.ps = NULL; }
    }
    g.tried = 1;
    g.taps = taps;

    snprintf(taps_text, sizeof taps_text, "%u", taps);
    macros[0].Name = "TAPS";
    macros[0].Definition = taps_text;
    macros[1].Name = NULL;
    macros[1].Definition = NULL;

    if (!compile_one(dev, "vs_main", "vs_4_0", macros, (void **)&g.vs)) return 0;
    if (!compile_one(dev, "ps_main", "ps_4_0", macros, (void **)&g.ps)) return 0;

    if (g.cb)
        return 1;                       /* rebuild: the rest already exists */

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(ResolveConstants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &g.cb);
    if (FAILED(hr)) { fail("CreateBuffer", hr); return 0; }

    /* Opaque, depthless, unculled: this pass replaces the target. */
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = ID3D11Device_CreateBlendState(dev, &bl, &g.blend);
    if (FAILED(hr)) { fail("CreateBlendState", hr); return 0; }

    memset(&ds, 0, sizeof ds);
    hr = ID3D11Device_CreateDepthStencilState(dev, &ds, &g.depth);
    if (FAILED(hr)) { fail("CreateDepthStencilState", hr); return 0; }

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(dev, &rd, &g.raster);
    if (FAILED(hr)) { fail("CreateRasterizerState", hr); return 0; }

    {
        D3D11_SAMPLER_DESC sm;

        memset(&sm, 0, sizeof sm);
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        hr = ID3D11Device_CreateSamplerState(dev, &sm, &g.sampler);
        if (FAILED(hr)) { fail("CreateSamplerState", hr); return 0; }
    }

    return 1;
}

HRESULT d3d8_display_resolve(ID3D11ShaderResourceView *scene,
                             UINT scene_w, UINT scene_h,
                             ID3D11RenderTargetView *out,
                             D3D8DisplayFit fit)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11RenderTargetView *saved_rtv = NULL;
    ID3D11DepthStencilView *saved_dsv = NULL;
    D3D11_VIEWPORT saved_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT saved_vps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT vp;
    float blend_factor[4] = { 1, 1, 1, 1 };
    UINT taps;

    if (!ctx || !scene || !out || !fit.w || !fit.h || !scene_h)
        return E_INVALIDARG;

    /* Square taps, from the vertical ratio: the horizontal one is the
     * same whenever the fit preserved the scene's shape, which it does,
     * and averaging a non-square block would soften one axis more than
     * the other. Rounded rather than truncated so a ratio just under a
     * whole number keeps the taps that ratio deserves. */
    taps = (scene_h + fit.h / 2) / fit.h;
    if (taps < 1) taps = 1;
    if (taps > 8) taps = 8;
    if (!create(taps)) return E_FAIL;

    ID3D11DeviceContext_RSGetViewports(ctx, &saved_vps, saved_vp);
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &saved_rtv, &saved_dsv);

    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g.cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        ResolveConstants c;
        c.origin_x = (float)fit.x;
        c.origin_y = (float)fit.y;
        c.ratio_x = (float)scene_w / (float)fit.w;
        c.ratio_y = (float)scene_h / (float)fit.h;
        c.texel_x = 1.0f / (float)scene_w;
        c.texel_y = 1.0f / (float)scene_h;
        c.inv_taps = 1.0f / (float)(taps * taps);
        c.pad = 0.0f;
        memcpy(mapped.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g.cb, 0);
    }

    vp.TopLeftX = (float)fit.x;
    vp.TopLeftY = (float)fit.y;
    vp.Width = (float)fit.w;
    vp.Height = (float)fit.h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &out, NULL);
    /* The bars. Cheaper than tracking whether the window changed shape,
     * and it costs one clear of a buffer that is about to be presented. */
    {
        float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_ClearRenderTargetView(ctx, out, black);
    }
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 7, 1, &g.cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 9, 1, &scene);
    ID3D11DeviceContext_PSSetSamplers(ctx, 9, 1, &g.sampler);
    ID3D11DeviceContext_OMSetBlendState(ctx, g.blend, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g.depth, 0);
    ID3D11DeviceContext_RSSetState(ctx, g.raster);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* The scene must not stay bound as a shader input: it is the render
     * target again as soon as the next frame starts. */
    {
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 9, 1, &none);
    }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &saved_rtv, saved_dsv);
    if (saved_vps)
        ID3D11DeviceContext_RSSetViewports(ctx, saved_vps, saved_vp);
    if (saved_rtv) ID3D11RenderTargetView_Release(saved_rtv);
    if (saved_dsv) ID3D11DepthStencilView_Release(saved_dsv);
    return S_OK;
}

void d3d8_display_shutdown(void)
{
    if (g.vs)     { ID3D11VertexShader_Release(g.vs);           g.vs = NULL; }
    if (g.ps)     { ID3D11PixelShader_Release(g.ps);            g.ps = NULL; }
    if (g.cb)     { ID3D11Buffer_Release(g.cb);                 g.cb = NULL; }
    if (g.blend)  { ID3D11BlendState_Release(g.blend);          g.blend = NULL; }
    if (g.depth)  { ID3D11DepthStencilState_Release(g.depth);   g.depth = NULL; }
    if (g.raster) { ID3D11RasterizerState_Release(g.raster);    g.raster = NULL; }
    if (g.sampler){ ID3D11SamplerState_Release(g.sampler);      g.sampler = NULL; }
    g.tried = 0;
    g.failed = 0;
}

