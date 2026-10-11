/*
 * d3d8_screencopy.c -- the finished frame, copied into a texture.
 *
 * A title that post-processes its own image reads the screen back: it copies
 * the back buffer into a texture and draws that texture over the scene again,
 * for a glow, a soften, a heat haze. On the console the GPU does the copy and
 * the texture's memory is the framebuffer's.
 *
 * Under shadow mode the title's own copy still happens, but it happens in the
 * guest's world, where nothing is rendered -- so the texture it reads back is
 * all zeros, and drawing it over the scene blends black over everything.
 * TimeSplitters 2 does exactly this, three times a frame, and it cost the
 * picture 62% of its brightness before this existed.
 *
 * So the copy is done here as well, from the host's back buffer into the host
 * texture standing in for the title's. It is a draw rather than a resource
 * copy because the two rarely agree on format: the swap chain is RGBA and a
 * title's texture is usually BGRA, which D3D11 will not copy between. Reading
 * with Load at integer pixels also means no sampler is bound, so a texture
 * stage the title set is left alone -- the same rule d3d8_overlay.c follows
 * and for the same reason.
 */
#include "d3d8_internal.h"
#include "d3d8_xbox.h"

#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

static struct {
    int                       tried, failed;
    ID3D11ShaderResourceView *back_srv;      /* the swap chain's back buffer */
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11Buffer             *cb;
    ID3D11BlendState         *blend;
    ID3D11DepthStencilState  *depth;
    ID3D11RasterizerState    *raster;
    unsigned long             copies;
} g;

/* Source pixel = destination pixel * scale + offset. Scale is one when the
 * sizes agree, which they do whenever a title reads back its own screen; the
 * offset is non-zero only for a CopyRects rectangle that moves. */
typedef struct { float scale_x, scale_y, off_x, off_y; } ScreenCopyConstants;

static const char kSource[] =
    "cbuffer ScreenCopy : register(b7) { float4 scale; };\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2((id << 1) & 2, id & 2);\n"   /* one oversized triangle */
    "    VSOut o;\n"
    "    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> screen : register(t9);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    int2 p = int2(i.pos.xy * scale.xy + scale.zw);\n"
    "    return screen.Load(int3(p, 0));\n"
    "}\n";

static void fail(const char *what, HRESULT hr)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 screen copy: %s failed (0x%08lX); a title that reads its "
            "own screen back will see black\n", what, (unsigned long)hr);
    fflush(stderr);
}

static int create(void)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    IDXGISwapChain *swap = d3d8_GetSwapChain();
    ID3D11Texture2D *back = NULL;
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bl;
    D3D11_DEPTH_STENCIL_DESC ds;
    D3D11_RASTERIZER_DESC rd;
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;

    if (g.failed) return 0;
    if (g.tried) return g.back_srv != NULL;
    g.tried = 1;
    if (!dev || !swap) { g.failed = 1; return 0; }

    /* The scene the title has been drawing into, which is the swap chain's
     * own back buffer only while nothing is scaled. Reading the back
     * buffer directly would be wrong above scale 1: the resolve that fills
     * it has not run yet this frame, so it still holds the last one. */
    g.back_srv = d3d8_GetSceneSRV();
    if (!g.back_srv) { fail("GetSceneSRV", E_FAIL); return 0; }
    ID3D11ShaderResourceView_AddRef(g.back_srv);
    (void)back;

    hr = D3DCompile(kSource, sizeof kSource - 1, "screencopy", NULL, NULL,
                    "vs_main", "vs_4_0", 0, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) { fprintf(stderr, "D3D8 screen copy: %s\n",
                           (const char *)ID3D10Blob_GetBufferPointer(err));
                   ID3D10Blob_Release(err); }
        fail("D3DCompile(vs)", hr);
        return 0;
    }
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &g.vs);
    ID3D10Blob_Release(code);
    code = NULL;
    if (FAILED(hr)) { fail("CreateVertexShader", hr); return 0; }

    hr = D3DCompile(kSource, sizeof kSource - 1, "screencopy", NULL, NULL,
                    "ps_main", "ps_4_0", 0, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) { fprintf(stderr, "D3D8 screen copy: %s\n",
                           (const char *)ID3D10Blob_GetBufferPointer(err));
                   ID3D10Blob_Release(err); }
        fail("D3DCompile(ps)", hr);
        return 0;
    }
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
                                        ID3D10Blob_GetBufferSize(code), NULL, &g.ps);
    ID3D10Blob_Release(code);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) { fail("CreatePixelShader", hr); return 0; }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(ScreenCopyConstants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &g.cb);
    if (FAILED(hr)) { fail("CreateBuffer", hr); return 0; }

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
    return 1;
}

/* rect: 0 = the whole texture from the whole screen; otherwise the title's
 * pixels src_x,src_y,w,h go to dst_x,dst_y of a texture whose title size is
 * dst_w x dst_h, and nothing else of it is written (a viewport, not a clear). */
static HRESULT copy_region(IDirect3DTexture8 *dst, int rect, int src_x, int src_y,
                           int w, int h, int dst_x, int dst_y, UINT dst_w, UINT dst_h)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    D3D8Texture *tex = (D3D8Texture *)dst;
    ID3D11RenderTargetView *rtv = NULL, *saved_rtv = NULL;
    ID3D11DepthStencilView *saved_dsv = NULL;
    D3D11_VIEWPORT saved_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT saved_vps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RENDER_TARGET_VIEW_DESC rtvd;
    D3D11_MAPPED_SUBRESOURCE mapped;
    ScreenCopyConstants c;
    D3D11_VIEWPORT vp;
    float blend_factor[4] = { 1, 1, 1, 1 };
    UINT back_w = d3d8_GetBackBufferWidth(), back_h = d3d8_GetBackBufferHeight();
    HRESULT hr;

    if (!dev || !ctx || !tex || !tex->d3d11_texture || !back_w || !back_h)
        return E_INVALIDARG;
    if (!create())
        return E_FAIL;

    memset(&rtvd, 0, sizeof rtvd);
    rtvd.Format = tex->dxgi_format;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex->d3d11_texture,
                                             &rtvd, &rtv);
    if (FAILED(hr)) {
        static int said;
        if (!said++)
            fprintf(stderr, "D3D8 screen copy: the destination texture is not a render "
                    "target (0x%08lX); nothing copied\n", (unsigned long)hr);
        return hr;
    }

    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &saved_rtv, &saved_dsv);
    ID3D11DeviceContext_RSGetViewports(ctx, &saved_vps, saved_vp);
    /* RECOMP_D3D8_SCREENCOPY_PROBE=1: what the source holds at the moment of
     * the copy, read back through a staging texture, and whether drawing was
     * going to the scene target at all. The texture layer's own probe reads
     * the destination; when that says "not arriving" this says which side
     * is empty. */
    {
        static int probe = -1;
        static unsigned long said;
        if (probe < 0) {
            const char *v = getenv("RECOMP_D3D8_SCREENCOPY_PROBE");
            probe = (v && atoi(v) > 0) ? atoi(v) : 0;   /* how many copies to report */
        }
        if (probe && said++ < (unsigned long)probe) {
            ID3D11Texture2D *scene = d3d8_GetSceneTexture();
            ID3D11Texture2D *staging = NULL;
            D3D11_TEXTURE2D_DESC sd;
            D3D11_MAPPED_SUBRESOURCE m;
            unsigned long long sum = 0;
            unsigned nonzero = 0, samples = 0;

            if (scene) {
                ID3D11Texture2D_GetDesc(scene, &sd);
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;
                if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &staging))) {
                    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging,
                                                     (ID3D11Resource *)scene);
                    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0,
                                                          D3D11_MAP_READ, 0, &m))) {
                        UINT x, y;
                        for (y = 0; y < sd.Height; y += 16) {
                            const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                            for (x = 0; x < sd.Width; x += 16) {
                                const uint8_t *p = row + (size_t)x * 4u;
                                sum += (unsigned)p[0] + p[1] + p[2];
                                samples += 3;
                                if (p[0] || p[1] || p[2]) nonzero++;
                            }
                        }
                        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
                    }
                    ID3D11Texture2D_Release(staging);
                }
            }
            fprintf(stderr, "D3D8 screen copy probe: scene %ux%u holds mean %.1f/255, %u of %u "
                    "non-zero; current target %s the scene; dst %ux%u scale %g x %g\n",
                    scene ? sd.Width : 0, scene ? sd.Height : 0,
                    samples ? (double)sum / samples : 0.0, nonzero, samples / 3u,
                    saved_rtv == d3d8_GetDefaultRTV() ? "is" : "is NOT",
                    tex->width, tex->height,
                    tex->width ? (double)back_w / tex->width : 0.0,
                    tex->height ? (double)back_h / tex->height : 0.0);
            fflush(stderr);
        }
    }

    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g.cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        c.scale_x = tex->width ? (float)back_w / (float)tex->width : 1.0f;
        c.scale_y = tex->height ? (float)back_h / (float)tex->height : 1.0f;
        c.off_x = c.off_y = 0.0f;
        if (rect) {
            /* Scene pixels per title pixel, and texture pixels per title
             * pixel: the two differ whenever the scene is scaled. */
            UINT gw = d3d8_GetGuestWidth(), gh = d3d8_GetGuestHeight();
            float sx = gw ? (float)back_w / (float)gw : 1.0f;
            float sy = gh ? (float)back_h / (float)gh : 1.0f;
            float tx = dst_w ? (float)tex->width / (float)dst_w : 1.0f;
            float ty = dst_h ? (float)tex->height / (float)dst_h : 1.0f;

            c.scale_x = sx / tx;
            c.scale_y = sy / ty;
            c.off_x = (float)(src_x - dst_x) * sx;
            c.off_y = (float)(src_y - dst_y) * sy;
        }
        memcpy(mapped.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g.cb, 0);
    }

    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = (float)tex->width;
    vp.Height = (float)tex->height;
    if (rect) {
        float tx = dst_w ? (float)tex->width / (float)dst_w : 1.0f;
        float ty = dst_h ? (float)tex->height / (float)dst_h : 1.0f;

        vp.TopLeftX = (float)dst_x * tx;
        vp.TopLeftY = (float)dst_y * ty;
        vp.Width = (float)w * tx;
        vp.Height = (float)h * ty;
    }
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g.ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 7, 1, &g.cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 9, 1, &g.back_srv);
    ID3D11DeviceContext_OMSetBlendState(ctx, g.blend, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g.depth, 0);
    ID3D11DeviceContext_RSSetState(ctx, g.raster);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    /* The back buffer must not stay bound as a shader input while it is the
     * render target again. */
    {
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 9, 1, &none);
    }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &saved_rtv, saved_dsv);
    if (saved_vps)
        ID3D11DeviceContext_RSSetViewports(ctx, saved_vps, saved_vp);
    if (saved_rtv) ID3D11RenderTargetView_Release(saved_rtv);
    if (saved_dsv) ID3D11DepthStencilView_Release(saved_dsv);
    ID3D11RenderTargetView_Release(rtv);
    g.copies++;
    return S_OK;
}

HRESULT xbox_D3D8CopyBackBufferToTexture(IDirect3DTexture8 *dst)
{
    return copy_region(dst, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

HRESULT xbox_D3D8CopyBackBufferRectToTexture(IDirect3DTexture8 *dst,
                                             int src_x, int src_y, int w, int h,
                                             int dst_x, int dst_y,
                                             UINT dst_w, UINT dst_h)
{
    if (w <= 0 || h <= 0)
        return S_OK;
    return copy_region(dst, 1, src_x, src_y, w, h, dst_x, dst_y, dst_w, dst_h);
}

HRESULT xbox_D3D8CopyTexture(IDirect3DTexture8 *dst, IDirect3DTexture8 *src)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    D3D8Texture *d = (D3D8Texture *)dst, *s = (D3D8Texture *)src;

    if (!ctx || !d || !s || !d->d3d11_texture || !s->d3d11_texture ||
        d->width != s->width || d->height != s->height || d->dxgi_format != s->dxgi_format)
        return E_INVALIDARG;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)d->d3d11_texture,
                                     (ID3D11Resource *)s->d3d11_texture);
    return S_OK;
}

unsigned long xbox_D3D8ScreenCopyCount(void) { return g.copies; }

void xbox_D3D8ScreenCopyShutdown(void)
{
    if (g.back_srv) { ID3D11ShaderResourceView_Release(g.back_srv); g.back_srv = NULL; }
    if (g.vs)       { ID3D11VertexShader_Release(g.vs);             g.vs = NULL; }
    if (g.ps)       { ID3D11PixelShader_Release(g.ps);              g.ps = NULL; }
    if (g.cb)       { ID3D11Buffer_Release(g.cb);                   g.cb = NULL; }
    if (g.blend)    { ID3D11BlendState_Release(g.blend);            g.blend = NULL; }
    if (g.depth)    { ID3D11DepthStencilState_Release(g.depth);     g.depth = NULL; }
    if (g.raster)   { ID3D11RasterizerState_Release(g.raster);      g.raster = NULL; }
    g.tried = 0;
}
