/*
 * d3d8_overlay.c -- one line of text over the finished frame.
 *
 * See d3d8_overlay.h for what this touches on the device and what it
 * deliberately does not. The short version: no sampler, no vertex buffer, no
 * input layout worth restoring, and the render target and viewport are put
 * back, so a title's own drawing is unaffected.
 *
 * The text is drawn by GDI into a 32-bit DIB and uploaded to a texture when
 * it changes, which is roughly twice a second for a frame-rate counter. A
 * real font at a real size costs less code than an embedded glyph table and
 * reads better at any window size. Off Windows the same buffer is filled by
 * FreeType, from the monospace font fontconfig names.
 */
#include "d3d8_internal.h"
#include "d3d8_overlay.h"

#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
#endif

#define OVERLAY_W    512
#define OVERLAY_H     28
#define OVERLAY_PAD    8      /* from the top-left corner of the back buffer */

static struct {
    int                       tried;     /* creation attempted */
    int                       failed;    /* and gave up: never try again */
    ID3D11Texture2D          *texture;
    ID3D11ShaderResourceView *srv;
    ID3D11Buffer             *cb;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11BlendState         *blend;
    ID3D11DepthStencilState  *depth;
    ID3D11RasterizerState    *raster;
#if defined(_WIN32)
    HDC                       dc;
    HBITMAP                   bitmap;
    HFONT                     font;
#else
    FT_Library                ft;
    FT_Face                   face;
#endif
    void                     *bits;      /* the DIB's pixels, BGRA top-down */
    int                       text_w;    /* what the last render measured */
    char                      text[128];
} g;

/* b7 in both stages: the four texture stages a title can use are b0..b3 and
 * the vertex layers take the low slots. x,y,w,h are in clip space. */
typedef struct {
    float x, y, w, h;
    float tex_w, tex_h, pad0, pad1;
} OverlayConstants;

static const char kShaderSource[] =
    "cbuffer Overlay : register(b7) {\n"
    "    float4 rect;        // x, y, w, h in clip space\n"
    "    float4 size;        // texture width, height\n"
    "};\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2(id & 1, id >> 1);\n"   /* 0,0  1,0  0,1  1,1 */
    "    VSOut o;\n"
    "    o.pos = float4(rect.x + c.x * rect.z, rect.y - c.y * rect.w, 0, 1);\n"
    "    o.uv  = c * size.xy;\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> tex : register(t8);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    int2 p = int2(i.uv);\n"
    "    float a = tex.Load(int3(p, 0)).r;\n"
    /* The glyphs are white on black, so the red channel is coverage. A dark
     * panel under them keeps the text readable over a bright frame. */
    "    float3 rgb = lerp(float3(0, 0, 0), float3(1, 1, 1), a);\n"
    "    return float4(rgb, max(a, 0.45));\n"
    "}\n";

static void overlay_fail(const char *what, HRESULT hr)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 overlay: %s failed (0x%08lX); the overlay is off for "
            "this run\n", what, (unsigned long)hr);
    fflush(stderr);
    d3d8_overlay_shutdown();
}

static int overlay_create(void)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    D3D11_TEXTURE2D_DESC td;
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bl;
    D3D11_DEPTH_STENCIL_DESC ds;
    D3D11_RASTERIZER_DESC rd;
#if defined(_WIN32)
    BITMAPINFO bi;
#endif
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr;

    if (g.failed) return 0;
    if (g.tried) return g.texture != NULL;
    g.tried = 1;
    if (!dev) { g.failed = 1; return 0; }

    memset(&td, 0, sizeof td);
    td.Width = OVERLAY_W;
    td.Height = OVERLAY_H;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &g.texture);
    if (FAILED(hr)) { overlay_fail("CreateTexture2D", hr); return 0; }
    hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g.texture,
                                               NULL, &g.srv);
    if (FAILED(hr)) { overlay_fail("CreateShaderResourceView", hr); return 0; }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(OverlayConstants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &g.cb);
    if (FAILED(hr)) { overlay_fail("CreateBuffer", hr); return 0; }

    hr = D3DCompile(kShaderSource, sizeof kShaderSource - 1, "overlay", NULL, NULL,
                    "vs_main", "vs_4_0", 0, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "D3D8 overlay: %s\n", (const char *)ID3D10Blob_GetBufferPointer(err));
        if (err) ID3D10Blob_Release(err);
        overlay_fail("D3DCompile(vs)", hr);
        return 0;
    }
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &g.vs);
    ID3D10Blob_Release(code);
    code = NULL;
    if (FAILED(hr)) { overlay_fail("CreateVertexShader", hr); return 0; }

    hr = D3DCompile(kShaderSource, sizeof kShaderSource - 1, "overlay", NULL, NULL,
                    "ps_main", "ps_4_0", 0, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) fprintf(stderr, "D3D8 overlay: %s\n", (const char *)ID3D10Blob_GetBufferPointer(err));
        if (err) ID3D10Blob_Release(err);
        overlay_fail("D3DCompile(ps)", hr);
        return 0;
    }
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
                                        ID3D10Blob_GetBufferSize(code), NULL, &g.ps);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) { overlay_fail("CreatePixelShader", hr); return 0; }
    if (err) ID3D10Blob_Release(err);

    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = ID3D11Device_CreateBlendState(dev, &bl, &g.blend);
    if (FAILED(hr)) { overlay_fail("CreateBlendState", hr); return 0; }

    memset(&ds, 0, sizeof ds);
    ds.DepthEnable = FALSE;
    ds.StencilEnable = FALSE;
    hr = ID3D11Device_CreateDepthStencilState(dev, &ds, &g.depth);
    if (FAILED(hr)) { overlay_fail("CreateDepthStencilState", hr); return 0; }

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(dev, &rd, &g.raster);
    if (FAILED(hr)) { overlay_fail("CreateRasterizerState", hr); return 0; }

#if defined(_WIN32)
    /* GDI side: a top-down 32-bit DIB the text is drawn into. */
    g.dc = CreateCompatibleDC(NULL);
    if (!g.dc) { overlay_fail("CreateCompatibleDC", 0); return 0; }
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = OVERLAY_W;
    bi.bmiHeader.biHeight = -OVERLAY_H;          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g.bitmap = CreateDIBSection(g.dc, &bi, DIB_RGB_COLORS, &g.bits, NULL, 0);
    if (!g.bitmap) { overlay_fail("CreateDIBSection", 0); return 0; }
    SelectObject(g.dc, g.bitmap);
    g.font = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (g.font) SelectObject(g.dc, g.font);
    SetBkMode(g.dc, OPAQUE);
    SetBkColor(g.dc, RGB(0, 0, 0));
    SetTextColor(g.dc, RGB(255, 255, 255));
#else
    /* FreeType side: the same top-down BGRA buffer, and a monospace font. */
    g.bits = calloc((size_t)OVERLAY_W * OVERLAY_H, 4);
    if (!g.bits) { overlay_fail("calloc", 0); return 0; }
    if (FT_Init_FreeType(&g.ft) != 0) { overlay_fail("FT_Init_FreeType", 0); return 0; }
    {
        char path[512] = "";
        FcPattern *pat, *match;
        FcResult res;
        FcChar8 *file = NULL;

        if (FcInit() && (pat = FcNameParse((const FcChar8 *)"monospace:weight=semibold")) != NULL) {
            FcConfigSubstitute(NULL, pat, FcMatchPattern);
            FcDefaultSubstitute(pat);
            match = FcFontMatch(NULL, pat, &res);
            if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
                snprintf(path, sizeof path, "%s", (const char *)file);
            if (match) FcPatternDestroy(match);
            FcPatternDestroy(pat);
        }
        if (!path[0] || FT_New_Face(g.ft, path, 0, &g.face) != 0) {
            overlay_fail("finding a monospace font", 0);
            return 0;
        }
        FT_Set_Pixel_Sizes(g.face, 0, 18);
    }
#endif
    return 1;
}

/* Draw the text into the DIB and upload it. Only when it has changed. */
static int overlay_set_text(const char *text)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
#if defined(_WIN32)
    wchar_t wide[128];
    RECT rect;
    SIZE extent;
#endif
    int len, i;

    if (strcmp(text, g.text) == 0)
        return g.text_w > 0;

    len = (int)strlen(text);
    if (len > (int)(sizeof g.text - 1)) len = (int)(sizeof g.text - 1);
    memcpy(g.text, text, (size_t)len);
    g.text[len] = '\0';
#if defined(_WIN32)
    for (i = 0; i < len; i++)
        wide[i] = (wchar_t)(unsigned char)g.text[i];

    rect.left = rect.top = 0;
    rect.right = OVERLAY_W;
    rect.bottom = OVERLAY_H;
    memset(g.bits, 0, (size_t)OVERLAY_W * OVERLAY_H * 4);
    if (!GetTextExtentPoint32W(g.dc, wide, len, &extent))
        extent.cx = OVERLAY_W;
    g.text_w = extent.cx + 12 > OVERLAY_W ? OVERLAY_W : (int)extent.cx + 12;
    ExtTextOutW(g.dc, 6, 4, ETO_OPAQUE, &rect, wide, (UINT)len, NULL);
    GdiFlush();
#else
    memset(g.bits, 0, (size_t)OVERLAY_W * OVERLAY_H * 4);
    {
        /* White on black, coverage in every colour channel, as GDI's. */
        int pen = 6, baseline = 4 + (int)(g.face->size->metrics.ascender >> 6);
        for (i = 0; i < len; i++) {
            FT_GlyphSlot gl;
            int x, y;
            if (FT_Load_Char(g.face, (unsigned char)g.text[i], FT_LOAD_RENDER) != 0)
                continue;
            gl = g.face->glyph;
            for (y = 0; y < (int)gl->bitmap.rows; y++) {
                int py = baseline - gl->bitmap_top + y;
                if (py < 0 || py >= OVERLAY_H)
                    continue;
                for (x = 0; x < (int)gl->bitmap.width; x++) {
                    int px = pen + gl->bitmap_left + x;
                    unsigned char v = gl->bitmap.buffer[y * gl->bitmap.pitch + x];
                    unsigned char *d;
                    if (px < 0 || px >= OVERLAY_W || !v)
                        continue;
                    d = (unsigned char *)g.bits + ((size_t)py * OVERLAY_W + (size_t)px) * 4;
                    d[0] = d[1] = d[2] = v;
                    d[3] = 255;
                }
            }
            pen += (int)(gl->advance.x >> 6);
        }
        g.text_w = pen + 6 > OVERLAY_W ? OVERLAY_W : pen + 6;
    }
#endif

    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g.texture, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return 0;
    for (i = 0; i < OVERLAY_H; i++)
        memcpy((unsigned char *)mapped.pData + (size_t)i * mapped.RowPitch,
               (const unsigned char *)g.bits + (size_t)i * OVERLAY_W * 4,
               (size_t)OVERLAY_W * 4);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g.texture, 0);
    return g.text_w > 0;
}

void d3d8_overlay_draw(const char *text)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11RenderTargetView *saved_rtv = NULL;
    ID3D11DepthStencilView *saved_dsv = NULL;
    ID3D11RenderTargetView *rtv = d3d8_GetDefaultRTV();
    D3D11_VIEWPORT saved_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE mapped;
    OverlayConstants c;
    UINT saved_vps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT width = d3d8_GetBackBufferWidth(), height = d3d8_GetBackBufferHeight();
    float blend_factor[4] = { 1, 1, 1, 1 };

    if (!text || !*text || !ctx || !rtv || !width || !height)
        return;
    if (!overlay_create() || !overlay_set_text(text))
        return;

    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &saved_rtv, &saved_dsv);
    ID3D11DeviceContext_RSGetViewports(ctx, &saved_vps, saved_vp);

    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g.cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        goto restore;
    c.x = -1.0f + 2.0f * (float)OVERLAY_PAD / (float)width;
    c.y =  1.0f - 2.0f * (float)OVERLAY_PAD / (float)height;
    c.w =  2.0f * (float)g.text_w / (float)width;
    c.h =  2.0f * (float)OVERLAY_H / (float)height;
    c.tex_w = (float)g.text_w;
    c.tex_h = (float)OVERLAY_H;
    c.pad0 = c.pad1 = 0.0f;
    memcpy(mapped.pData, &c, sizeof c);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g.cb, 0);

    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, g.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g.ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 7, 1, &g.cb);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 7, 1, &g.cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 8, 1, &g.srv);
    ID3D11DeviceContext_OMSetBlendState(ctx, g.blend, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g.depth, 0);
    ID3D11DeviceContext_RSSetState(ctx, g.raster);
    ID3D11DeviceContext_Draw(ctx, 4, 0);

restore:
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &saved_rtv, saved_dsv);
    if (saved_vps)
        ID3D11DeviceContext_RSSetViewports(ctx, saved_vps, saved_vp);
    if (saved_rtv) ID3D11RenderTargetView_Release(saved_rtv);
    if (saved_dsv) ID3D11DepthStencilView_Release(saved_dsv);
}

void d3d8_overlay_shutdown(void)
{
    if (g.srv)     { ID3D11ShaderResourceView_Release(g.srv);    g.srv = NULL; }
    if (g.texture) { ID3D11Texture2D_Release(g.texture);         g.texture = NULL; }
    if (g.cb)      { ID3D11Buffer_Release(g.cb);                 g.cb = NULL; }
    if (g.vs)      { ID3D11VertexShader_Release(g.vs);           g.vs = NULL; }
    if (g.ps)      { ID3D11PixelShader_Release(g.ps);            g.ps = NULL; }
    if (g.blend)   { ID3D11BlendState_Release(g.blend);          g.blend = NULL; }
    if (g.depth)   { ID3D11DepthStencilState_Release(g.depth);   g.depth = NULL; }
    if (g.raster)  { ID3D11RasterizerState_Release(g.raster);    g.raster = NULL; }
#if defined(_WIN32)
    if (g.bitmap)  { DeleteObject(g.bitmap);                     g.bitmap = NULL; }
    if (g.font)    { DeleteObject(g.font);                       g.font = NULL; }
    if (g.dc)      { DeleteDC(g.dc);                             g.dc = NULL; }
#else
    if (g.face)    { FT_Done_Face(g.face);                       g.face = NULL; }
    if (g.ft)      { FT_Done_FreeType(g.ft);                     g.ft = NULL; }
    free(g.bits);
#endif
    g.bits = NULL;
    g.text[0] = '\0';
    g.text_w = 0;
    g.tried = 0;
}
