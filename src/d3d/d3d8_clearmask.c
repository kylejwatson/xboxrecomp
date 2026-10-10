/*
 * d3d8_clearmask.c -- a colour clear limited to some channels.
 *
 * The Xbox splits D3DCLEAR_TARGET into one bit per channel (0x10 R, 0x20 G,
 * 0x40 B, 0x80 A), so a title can clear alpha alone and keep the picture.
 * TimeSplitters: Future Perfect does that before its on-screen control hints
 * and its shimmering cloaked creatures, to build a destination-alpha mask
 * over the finished scene. Treated as a whole clear, the scene went black
 * behind every hint.
 *
 * D3D11 cannot clear part of a pixel, so this is a draw: one oversized
 * triangle whose pixel shader returns 1, blended as BLEND_FACTOR * 1 with the
 * clear colour as the blend factor, through a write mask of the channels
 * asked for. No constant buffer, and no texture or sampler is touched. Clear
 * rectangles, if any, are scissor rectangles here.
 *
 * Like the screen copy, this binds its own shaders and states and puts back
 * only the render targets, viewports and scissors: every draw binds the
 * rest again.
 */
#include "d3d8_internal.h"
#include "d3d8_xbox.h"

#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

static struct {
    int                      tried, failed;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    ID3D11BlendState        *blend[16];   /* by D3D11 write mask */
    ID3D11DepthStencilState *depth;
    ID3D11RasterizerState   *raster;      /* scissor on */
} g;

static const char kSource[] =
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 c = float2((id << 1) & 2, id & 2);\n"   /* one oversized triangle */
    "    return float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps_main() : SV_Target { return 1; }\n";

static void fail(const char *what, HRESULT hr)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 channel clear: %s failed (0x%08lX); a clear of some "
            "channels clears all four\n", what, (unsigned long)hr);
    fflush(stderr);
}

static int compile(ID3D11Device *dev, const char *entry, const char *target, void **out)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(kSource, sizeof kSource - 1, "clearmask", NULL, NULL,
                            entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) {
            fprintf(stderr, "D3D8 channel clear: %s\n",
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

static int create(void)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    D3D11_DEPTH_STENCIL_DESC ds;
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    if (g.failed) return 0;
    if (g.tried) return g.ps != NULL;
    g.tried = 1;
    if (!dev) { g.failed = 1; return 0; }
    if (!compile(dev, "vs_main", "vs_4_0", (void **)&g.vs) ||
        !compile(dev, "ps_main", "ps_4_0", (void **)&g.ps))
        return 0;

    memset(&ds, 0, sizeof ds);
    hr = ID3D11Device_CreateDepthStencilState(dev, &ds, &g.depth);
    if (FAILED(hr)) { fail("CreateDepthStencilState", hr); return 0; }

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(dev, &rd, &g.raster);
    if (FAILED(hr)) { fail("CreateRasterizerState", hr); return 0; }
    return 1;
}

static ID3D11BlendState *blend_for(UINT mask)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    D3D11_BLEND_DESC bl;

    if (g.blend[mask])
        return g.blend[mask];
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = (UINT8)mask;
    if (FAILED(ID3D11Device_CreateBlendState(dev, &bl, &g.blend[mask])))
        g.blend[mask] = NULL;
    return g.blend[mask];
}

BOOL d3d8_ClearChannels(ID3D11RenderTargetView *rtv, UINT xbox_channels,
                        const float color[4], UINT rect_count, const D3D11_RECT *rects)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11RenderTargetView *saved_rtv = NULL;
    ID3D11DepthStencilView *saved_dsv = NULL;
    D3D11_VIEWPORT saved_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT saved_vps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT saved_sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT saved_scs = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11Resource *res = NULL;
    D3D11_TEXTURE2D_DESC td;
    D3D11_VIEWPORT vp;
    D3D11_RECT whole;
    ID3D11BlendState *blend;
    UINT mask = 0, i;

    if (xbox_channels & 0x10) mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (xbox_channels & 0x20) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (xbox_channels & 0x40) mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (xbox_channels & 0x80) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    if (!mask)
        return TRUE;                     /* nothing asked for */
    if (!ctx || !rtv || !create() || !(blend = blend_for(mask)))
        return FALSE;

    ID3D11RenderTargetView_GetResource(rtv, &res);
    if (!res)
        return FALSE;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
    ID3D11Resource_Release(res);

    ID3D11DeviceContext_RSGetViewports(ctx, &saved_vps, saved_vp);
    ID3D11DeviceContext_RSGetScissorRects(ctx, &saved_scs, saved_sc);
    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &saved_rtv, &saved_dsv);

    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = (float)td.Width;
    vp.Height = (float)td.Height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    whole.left = whole.top = 0;
    whole.right = (LONG)td.Width;
    whole.bottom = (LONG)td.Height;
    if (!rect_count) {
        rects = &whole;
        rect_count = 1;
    }

    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g.ps, NULL, 0);
    ID3D11DeviceContext_OMSetBlendState(ctx, blend, color, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g.depth, 0);
    ID3D11DeviceContext_RSSetState(ctx, g.raster);
    for (i = 0; i < rect_count; i++) {
        ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &rects[i]);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
    }

    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &saved_rtv, saved_dsv);
    if (saved_vps)
        ID3D11DeviceContext_RSSetViewports(ctx, saved_vps, saved_vp);
    ID3D11DeviceContext_RSSetScissorRects(ctx, saved_scs, saved_scs ? saved_sc : NULL);
    if (saved_rtv) ID3D11RenderTargetView_Release(saved_rtv);
    if (saved_dsv) ID3D11DepthStencilView_Release(saved_dsv);
    return TRUE;
}

void d3d8_ClearChannelsShutdown(void)
{
    UINT i;
    if (g.vs)     { ID3D11VertexShader_Release(g.vs);       g.vs = NULL; }
    if (g.ps)     { ID3D11PixelShader_Release(g.ps);        g.ps = NULL; }
    for (i = 0; i < 16; i++)
        if (g.blend[i]) { ID3D11BlendState_Release(g.blend[i]); g.blend[i] = NULL; }
    if (g.depth)  { ID3D11DepthStencilState_Release(g.depth); g.depth = NULL; }
    if (g.raster) { ID3D11RasterizerState_Release(g.raster);  g.raster = NULL; }
    g.tried = g.failed = 0;
}
