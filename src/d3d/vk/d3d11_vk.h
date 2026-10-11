/*
 * d3d11_vk.h - the part of Direct3D 11 that src/d3d uses, implemented on
 * Vulkan (d3d11_vk.c), for hosts without Direct3D.
 *
 * Why at this seam. Everything in src/d3d above the D3D11 calls -- the Xbox
 * format table, swizzling, P8 palettes, cube and volume layout, the NV2A
 * vertex-program and register-combiner translators -- is shared with the
 * Windows build unchanged; only these 61 entry points, the ones the renderer
 * actually calls (`grep -ohE "ID3D11[A-Za-z0-9]*_[A-Za-z0-9]+"` over src/d3d),
 * are reimplemented. The Windows build never sees this file and is
 * byte-for-byte what it was.
 *
 * Enumerators and structure members carry their D3D11 names and values, so
 * the renderer compiles against this header as it does against <d3d11.h>
 * (src/d3d/vk/include/d3d11.h forwards here). The objects are opaque; the
 * COBJMACROS-style names (ID3D11DeviceContext_Draw and so on) are macros over
 * plain functions, and every object is released with the same call.
 *
 * Semantics that matter and are kept: an immediate context that records in
 * call order; Map(WRITE_DISCARD) gives fresh memory without waiting, and
 * NO_OVERWRITE the same memory; a staging Map(READ) waits for the GPU;
 * UpdateSubresource and the copies are ordered against the draws around
 * them; the context holds a reference on what is bound; HLSL is compiled
 * by D3DCompile (d3dcompiler_vk.c) and linked by semantic, as D3D11 links.
 */
#ifndef XBOXRECOMP_D3D11_VK_H
#define XBOXRECOMP_D3D11_VK_H

#include "platform/xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- objects ------------------------------------------------------------ */

typedef struct ID3D11Device             ID3D11Device;
typedef struct ID3D11DeviceContext      ID3D11DeviceContext;
typedef struct IDXGISwapChain           IDXGISwapChain;
typedef struct ID3D11Resource           ID3D11Resource;
typedef struct ID3D11Buffer             ID3D11Buffer;
typedef struct ID3D11Texture2D          ID3D11Texture2D;
typedef struct ID3D11Texture3D          ID3D11Texture3D;
typedef struct ID3D11ShaderResourceView ID3D11ShaderResourceView;
typedef struct ID3D11RenderTargetView   ID3D11RenderTargetView;
typedef struct ID3D11DepthStencilView   ID3D11DepthStencilView;
typedef struct ID3D11VertexShader       ID3D11VertexShader;
typedef struct ID3D11PixelShader        ID3D11PixelShader;
typedef struct ID3D11InputLayout        ID3D11InputLayout;
typedef struct ID3D11BlendState         ID3D11BlendState;
typedef struct ID3D11DepthStencilState  ID3D11DepthStencilState;
typedef struct ID3D11RasterizerState    ID3D11RasterizerState;
typedef struct ID3D11SamplerState       ID3D11SamplerState;
typedef struct ID3D11ClassLinkage       ID3D11ClassLinkage;
typedef struct ID3D11ClassInstance      ID3D11ClassInstance;
typedef struct IDXGIAdapter             IDXGIAdapter;
typedef struct ID3D10Blob               ID3D10Blob;
typedef ID3D10Blob                      ID3DBlob;
typedef struct ID3DInclude              ID3DInclude;

/* GetBuffer's interface id; only the texture is ever asked for. */
extern const GUID IID_ID3D11Texture2D;

/* ---- enumerations (D3D11 values) ---------------------------------------- */

typedef enum DXGI_FORMAT {
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R32G32B32A32_UINT = 3,
    DXGI_FORMAT_R32G32B32A32_SINT = 4,
    DXGI_FORMAT_R32G32B32_TYPELESS = 5,
    DXGI_FORMAT_R32G32B32_FLOAT = 6,
    DXGI_FORMAT_R32G32B32_UINT = 7,
    DXGI_FORMAT_R32G32B32_SINT = 8,
    DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R16G16B16A16_UNORM = 11,
    DXGI_FORMAT_R16G16B16A16_UINT = 12,
    DXGI_FORMAT_R16G16B16A16_SNORM = 13,
    DXGI_FORMAT_R16G16B16A16_SINT = 14,
    DXGI_FORMAT_R32G32_TYPELESS = 15,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_R32G32_UINT = 17,
    DXGI_FORMAT_R32G32_SINT = 18,
    DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24,
    DXGI_FORMAT_R10G10B10A2_UINT = 25,
    DXGI_FORMAT_R11G11B10_FLOAT = 26,
    DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
    DXGI_FORMAT_R8G8B8A8_UINT = 30,
    DXGI_FORMAT_R8G8B8A8_SNORM = 31,
    DXGI_FORMAT_R8G8B8A8_SINT = 32,
    DXGI_FORMAT_R16G16_TYPELESS = 33,
    DXGI_FORMAT_R16G16_FLOAT = 34,
    DXGI_FORMAT_R16G16_UNORM = 35,
    DXGI_FORMAT_R16G16_UINT = 36,
    DXGI_FORMAT_R16G16_SNORM = 37,
    DXGI_FORMAT_R16G16_SINT = 38,
    DXGI_FORMAT_R32_TYPELESS = 39,
    DXGI_FORMAT_D32_FLOAT = 40,
    DXGI_FORMAT_R32_FLOAT = 41,
    DXGI_FORMAT_R32_UINT = 42,
    DXGI_FORMAT_R32_SINT = 43,
    DXGI_FORMAT_R24G8_TYPELESS = 44,
    DXGI_FORMAT_D24_UNORM_S8_UINT = 45,
    DXGI_FORMAT_R8G8_TYPELESS = 48,
    DXGI_FORMAT_R8G8_UNORM = 49,
    DXGI_FORMAT_R8G8_UINT = 50,
    DXGI_FORMAT_R8G8_SNORM = 51,
    DXGI_FORMAT_R8G8_SINT = 52,
    DXGI_FORMAT_R16_TYPELESS = 53,
    DXGI_FORMAT_R16_FLOAT = 54,
    DXGI_FORMAT_D16_UNORM = 55,
    DXGI_FORMAT_R16_UNORM = 56,
    DXGI_FORMAT_R16_UINT = 57,
    DXGI_FORMAT_R16_SNORM = 58,
    DXGI_FORMAT_R16_SINT = 59,
    DXGI_FORMAT_R8_TYPELESS = 60,
    DXGI_FORMAT_R8_UNORM = 61,
    DXGI_FORMAT_R8_UINT = 62,
    DXGI_FORMAT_R8_SNORM = 63,
    DXGI_FORMAT_R8_SINT = 64,
    DXGI_FORMAT_A8_UNORM = 65,
    DXGI_FORMAT_R8G8_B8G8_UNORM = 68,
    DXGI_FORMAT_G8R8_G8B8_UNORM = 69,
    DXGI_FORMAT_BC1_TYPELESS = 70,
    DXGI_FORMAT_BC1_UNORM = 71,
    DXGI_FORMAT_BC2_TYPELESS = 73,
    DXGI_FORMAT_BC2_UNORM = 74,
    DXGI_FORMAT_BC3_TYPELESS = 76,
    DXGI_FORMAT_BC3_UNORM = 77,
    DXGI_FORMAT_BC4_TYPELESS = 79,
    DXGI_FORMAT_BC4_UNORM = 80,
    DXGI_FORMAT_BC4_SNORM = 81,
    DXGI_FORMAT_BC5_TYPELESS = 82,
    DXGI_FORMAT_BC5_UNORM = 83,
    DXGI_FORMAT_BC5_SNORM = 84,
    DXGI_FORMAT_B5G6R5_UNORM = 85,
    DXGI_FORMAT_B5G5R5A1_UNORM = 86,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
    DXGI_FORMAT_B8G8R8X8_UNORM = 88,
    DXGI_FORMAT_B8G8R8A8_TYPELESS = 90,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
    DXGI_FORMAT_B8G8R8X8_TYPELESS = 92,
    DXGI_FORMAT_B4G4R4A4_UNORM = 115,
    DXGI_FORMAT_FORCE_UINT = 0x7fffffff
} DXGI_FORMAT;

typedef enum D3D11_USAGE {
    D3D11_USAGE_DEFAULT = 0, D3D11_USAGE_IMMUTABLE = 1,
    D3D11_USAGE_DYNAMIC = 2, D3D11_USAGE_STAGING = 3
} D3D11_USAGE;

enum {
    D3D11_BIND_VERTEX_BUFFER = 0x1, D3D11_BIND_INDEX_BUFFER = 0x2,
    D3D11_BIND_CONSTANT_BUFFER = 0x4, D3D11_BIND_SHADER_RESOURCE = 0x8,
    D3D11_BIND_STREAM_OUTPUT = 0x10, D3D11_BIND_RENDER_TARGET = 0x20,
    D3D11_BIND_DEPTH_STENCIL = 0x40, D3D11_BIND_UNORDERED_ACCESS = 0x80
};
enum { D3D11_CPU_ACCESS_WRITE = 0x10000, D3D11_CPU_ACCESS_READ = 0x20000 };
enum { D3D11_RESOURCE_MISC_TEXTURECUBE = 0x4 };
enum { D3D11_CLEAR_DEPTH = 0x1, D3D11_CLEAR_STENCIL = 0x2 };
enum { D3D11_CREATE_DEVICE_DEBUG = 0x2 };
#define D3D11_SDK_VERSION 7
#define D3D11_APPEND_ALIGNED_ELEMENT 0xffffffffu
#define D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE 16
#define D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT 8
#define D3D11_FLOAT32_MAX 3.402823466e+38f
#define D3D11_DEFAULT_STENCIL_READ_MASK 0xff
#define D3D11_DEFAULT_STENCIL_WRITE_MASK 0xff

typedef enum D3D11_MAP {
    D3D11_MAP_READ = 1, D3D11_MAP_WRITE = 2, D3D11_MAP_READ_WRITE = 3,
    D3D11_MAP_WRITE_DISCARD = 4, D3D11_MAP_WRITE_NO_OVERWRITE = 5
} D3D11_MAP;

typedef enum D3D11_PRIMITIVE_TOPOLOGY {
    D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED = 0,
    D3D11_PRIMITIVE_TOPOLOGY_POINTLIST = 1,
    D3D11_PRIMITIVE_TOPOLOGY_LINELIST = 2,
    D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP = 3,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST = 4,
    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP = 5
} D3D11_PRIMITIVE_TOPOLOGY;

typedef enum D3D11_BLEND {
    D3D11_BLEND_ZERO = 1, D3D11_BLEND_ONE = 2,
    D3D11_BLEND_SRC_COLOR = 3, D3D11_BLEND_INV_SRC_COLOR = 4,
    D3D11_BLEND_SRC_ALPHA = 5, D3D11_BLEND_INV_SRC_ALPHA = 6,
    D3D11_BLEND_DEST_ALPHA = 7, D3D11_BLEND_INV_DEST_ALPHA = 8,
    D3D11_BLEND_DEST_COLOR = 9, D3D11_BLEND_INV_DEST_COLOR = 10,
    D3D11_BLEND_SRC_ALPHA_SAT = 11,
    D3D11_BLEND_BLEND_FACTOR = 14, D3D11_BLEND_INV_BLEND_FACTOR = 15,
    D3D11_BLEND_SRC1_COLOR = 16, D3D11_BLEND_INV_SRC1_COLOR = 17,
    D3D11_BLEND_SRC1_ALPHA = 18, D3D11_BLEND_INV_SRC1_ALPHA = 19
} D3D11_BLEND;

typedef enum D3D11_BLEND_OP {
    D3D11_BLEND_OP_ADD = 1, D3D11_BLEND_OP_SUBTRACT = 2,
    D3D11_BLEND_OP_REV_SUBTRACT = 3, D3D11_BLEND_OP_MIN = 4, D3D11_BLEND_OP_MAX = 5
} D3D11_BLEND_OP;

enum {
    D3D11_COLOR_WRITE_ENABLE_RED = 1, D3D11_COLOR_WRITE_ENABLE_GREEN = 2,
    D3D11_COLOR_WRITE_ENABLE_BLUE = 4, D3D11_COLOR_WRITE_ENABLE_ALPHA = 8,
    D3D11_COLOR_WRITE_ENABLE_ALL = 15
};

typedef enum D3D11_COMPARISON_FUNC {
    D3D11_COMPARISON_NEVER = 1, D3D11_COMPARISON_LESS = 2,
    D3D11_COMPARISON_EQUAL = 3, D3D11_COMPARISON_LESS_EQUAL = 4,
    D3D11_COMPARISON_GREATER = 5, D3D11_COMPARISON_NOT_EQUAL = 6,
    D3D11_COMPARISON_GREATER_EQUAL = 7, D3D11_COMPARISON_ALWAYS = 8
} D3D11_COMPARISON_FUNC;

typedef enum D3D11_DEPTH_WRITE_MASK {
    D3D11_DEPTH_WRITE_MASK_ZERO = 0, D3D11_DEPTH_WRITE_MASK_ALL = 1
} D3D11_DEPTH_WRITE_MASK;

typedef enum D3D11_STENCIL_OP {
    D3D11_STENCIL_OP_KEEP = 1, D3D11_STENCIL_OP_ZERO = 2,
    D3D11_STENCIL_OP_REPLACE = 3, D3D11_STENCIL_OP_INCR_SAT = 4,
    D3D11_STENCIL_OP_DECR_SAT = 5, D3D11_STENCIL_OP_INVERT = 6,
    D3D11_STENCIL_OP_INCR = 7, D3D11_STENCIL_OP_DECR = 8
} D3D11_STENCIL_OP;

typedef enum D3D11_FILL_MODE { D3D11_FILL_WIREFRAME = 2, D3D11_FILL_SOLID = 3 } D3D11_FILL_MODE;
typedef enum D3D11_CULL_MODE {
    D3D11_CULL_NONE = 1, D3D11_CULL_FRONT = 2, D3D11_CULL_BACK = 3
} D3D11_CULL_MODE;

/* Filter bits: mip linear 0x1, mag linear 0x4, min linear 0x10,
 * anisotropic 0x40, comparison 0x80. */
typedef enum D3D11_FILTER {
    D3D11_FILTER_MIN_MAG_MIP_POINT = 0,
    D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR = 0x1,
    D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT = 0x4,
    D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR = 0x5,
    D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT = 0x10,
    D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR = 0x11,
    D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT = 0x14,
    D3D11_FILTER_MIN_MAG_MIP_LINEAR = 0x15,
    D3D11_FILTER_ANISOTROPIC = 0x55
} D3D11_FILTER;

typedef enum D3D11_TEXTURE_ADDRESS_MODE {
    D3D11_TEXTURE_ADDRESS_WRAP = 1, D3D11_TEXTURE_ADDRESS_MIRROR = 2,
    D3D11_TEXTURE_ADDRESS_CLAMP = 3, D3D11_TEXTURE_ADDRESS_BORDER = 4,
    D3D11_TEXTURE_ADDRESS_MIRROR_ONCE = 5
} D3D11_TEXTURE_ADDRESS_MODE;

typedef enum D3D11_INPUT_CLASSIFICATION {
    D3D11_INPUT_PER_VERTEX_DATA = 0, D3D11_INPUT_PER_INSTANCE_DATA = 1
} D3D11_INPUT_CLASSIFICATION;

typedef enum D3D11_RTV_DIMENSION {
    D3D11_RTV_DIMENSION_UNKNOWN = 0, D3D11_RTV_DIMENSION_TEXTURE2D = 4,
    D3D11_RTV_DIMENSION_TEXTURE2DARRAY = 5
} D3D11_RTV_DIMENSION;
typedef enum D3D11_DSV_DIMENSION {
    D3D11_DSV_DIMENSION_UNKNOWN = 0, D3D11_DSV_DIMENSION_TEXTURE2D = 3,
    D3D11_DSV_DIMENSION_TEXTURE2DARRAY = 4
} D3D11_DSV_DIMENSION;
typedef enum D3D11_SRV_DIMENSION {
    D3D11_SRV_DIMENSION_UNKNOWN = 0, D3D11_SRV_DIMENSION_TEXTURE2D = 4,
    D3D11_SRV_DIMENSION_TEXTURE2DARRAY = 5, D3D11_SRV_DIMENSION_TEXTURE3D = 8,
    D3D11_SRV_DIMENSION_TEXTURECUBE = 9
} D3D11_SRV_DIMENSION;

typedef enum D3D_DRIVER_TYPE {
    D3D_DRIVER_TYPE_UNKNOWN = 0, D3D_DRIVER_TYPE_HARDWARE = 1,
    D3D_DRIVER_TYPE_WARP = 5
} D3D_DRIVER_TYPE;
typedef enum D3D_FEATURE_LEVEL { D3D_FEATURE_LEVEL_11_0 = 0xb000 } D3D_FEATURE_LEVEL;

enum { DXGI_USAGE_SHADER_INPUT = 0x10, DXGI_USAGE_RENDER_TARGET_OUTPUT = 0x20 };
typedef enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_DISCARD = 0, DXGI_SWAP_EFFECT_SEQUENTIAL = 1
} DXGI_SWAP_EFFECT;

/* ---- structures ---------------------------------------------------------- */

typedef RECT D3D11_RECT;

typedef struct D3D11_BOX { UINT left, top, front, right, bottom, back; } D3D11_BOX;

typedef struct D3D11_VIEWPORT {
    FLOAT TopLeftX, TopLeftY, Width, Height, MinDepth, MaxDepth;
} D3D11_VIEWPORT;

typedef struct DXGI_SAMPLE_DESC { UINT Count, Quality; } DXGI_SAMPLE_DESC;
typedef struct DXGI_RATIONAL { UINT Numerator, Denominator; } DXGI_RATIONAL;

typedef struct D3D11_BUFFER_DESC {
    UINT ByteWidth;
    D3D11_USAGE Usage;
    UINT BindFlags, CPUAccessFlags, MiscFlags, StructureByteStride;
} D3D11_BUFFER_DESC;

typedef struct D3D11_SUBRESOURCE_DATA {
    const void *pSysMem;
    UINT SysMemPitch, SysMemSlicePitch;
} D3D11_SUBRESOURCE_DATA;

typedef struct D3D11_TEXTURE2D_DESC {
    UINT Width, Height, MipLevels, ArraySize;
    DXGI_FORMAT Format;
    DXGI_SAMPLE_DESC SampleDesc;
    D3D11_USAGE Usage;
    UINT BindFlags, CPUAccessFlags, MiscFlags;
} D3D11_TEXTURE2D_DESC;

typedef struct D3D11_TEXTURE3D_DESC {
    UINT Width, Height, Depth, MipLevels;
    DXGI_FORMAT Format;
    D3D11_USAGE Usage;
    UINT BindFlags, CPUAccessFlags, MiscFlags;
} D3D11_TEXTURE3D_DESC;

typedef struct D3D11_MAPPED_SUBRESOURCE {
    void *pData;
    UINT RowPitch, DepthPitch;
} D3D11_MAPPED_SUBRESOURCE;

typedef struct D3D11_INPUT_ELEMENT_DESC {
    LPCSTR SemanticName;
    UINT SemanticIndex;
    DXGI_FORMAT Format;
    UINT InputSlot, AlignedByteOffset;
    D3D11_INPUT_CLASSIFICATION InputSlotClass;
    UINT InstanceDataStepRate;
} D3D11_INPUT_ELEMENT_DESC;

typedef struct D3D11_RENDER_TARGET_BLEND_DESC {
    BOOL BlendEnable;
    D3D11_BLEND SrcBlend, DestBlend;
    D3D11_BLEND_OP BlendOp;
    D3D11_BLEND SrcBlendAlpha, DestBlendAlpha;
    D3D11_BLEND_OP BlendOpAlpha;
    UINT8 RenderTargetWriteMask;
} D3D11_RENDER_TARGET_BLEND_DESC;

typedef struct D3D11_BLEND_DESC {
    BOOL AlphaToCoverageEnable, IndependentBlendEnable;
    D3D11_RENDER_TARGET_BLEND_DESC RenderTarget[8];
} D3D11_BLEND_DESC;

typedef struct D3D11_DEPTH_STENCILOP_DESC {
    D3D11_STENCIL_OP StencilFailOp, StencilDepthFailOp, StencilPassOp;
    D3D11_COMPARISON_FUNC StencilFunc;
} D3D11_DEPTH_STENCILOP_DESC;

typedef struct D3D11_DEPTH_STENCIL_DESC {
    BOOL DepthEnable;
    D3D11_DEPTH_WRITE_MASK DepthWriteMask;
    D3D11_COMPARISON_FUNC DepthFunc;
    BOOL StencilEnable;
    UINT8 StencilReadMask, StencilWriteMask;
    D3D11_DEPTH_STENCILOP_DESC FrontFace, BackFace;
} D3D11_DEPTH_STENCIL_DESC;

typedef struct D3D11_RASTERIZER_DESC {
    D3D11_FILL_MODE FillMode;
    D3D11_CULL_MODE CullMode;
    BOOL FrontCounterClockwise;
    INT DepthBias;
    FLOAT DepthBiasClamp, SlopeScaledDepthBias;
    BOOL DepthClipEnable, ScissorEnable, MultisampleEnable, AntialiasedLineEnable;
} D3D11_RASTERIZER_DESC;

typedef struct D3D11_SAMPLER_DESC {
    D3D11_FILTER Filter;
    D3D11_TEXTURE_ADDRESS_MODE AddressU, AddressV, AddressW;
    FLOAT MipLODBias;
    UINT MaxAnisotropy;
    D3D11_COMPARISON_FUNC ComparisonFunc;
    FLOAT BorderColor[4];
    FLOAT MinLOD, MaxLOD;
} D3D11_SAMPLER_DESC;

typedef struct D3D11_TEX2D_SRV { UINT MostDetailedMip, MipLevels; } D3D11_TEX2D_SRV;
typedef struct D3D11_TEX2D_ARRAY_SRV {
    UINT MostDetailedMip, MipLevels, FirstArraySlice, ArraySize;
} D3D11_TEX2D_ARRAY_SRV;
typedef struct D3D11_TEX3D_SRV { UINT MostDetailedMip, MipLevels; } D3D11_TEX3D_SRV;
typedef struct D3D11_TEXCUBE_SRV { UINT MostDetailedMip, MipLevels; } D3D11_TEXCUBE_SRV;

typedef struct D3D11_SHADER_RESOURCE_VIEW_DESC {
    DXGI_FORMAT Format;
    D3D11_SRV_DIMENSION ViewDimension;
    union {
        D3D11_TEX2D_SRV Texture2D;
        D3D11_TEX2D_ARRAY_SRV Texture2DArray;
        D3D11_TEX3D_SRV Texture3D;
        D3D11_TEXCUBE_SRV TextureCube;
    };
} D3D11_SHADER_RESOURCE_VIEW_DESC;

typedef struct D3D11_TEX2D_RTV { UINT MipSlice; } D3D11_TEX2D_RTV;
typedef struct D3D11_TEX2D_ARRAY_RTV { UINT MipSlice, FirstArraySlice, ArraySize; } D3D11_TEX2D_ARRAY_RTV;

typedef struct D3D11_RENDER_TARGET_VIEW_DESC {
    DXGI_FORMAT Format;
    D3D11_RTV_DIMENSION ViewDimension;
    union {
        D3D11_TEX2D_RTV Texture2D;
        D3D11_TEX2D_ARRAY_RTV Texture2DArray;
    };
} D3D11_RENDER_TARGET_VIEW_DESC;

typedef struct D3D11_TEX2D_DSV { UINT MipSlice; } D3D11_TEX2D_DSV;
typedef struct D3D11_TEX2D_ARRAY_DSV { UINT MipSlice, FirstArraySlice, ArraySize; } D3D11_TEX2D_ARRAY_DSV;

typedef struct D3D11_DEPTH_STENCIL_VIEW_DESC {
    DXGI_FORMAT Format;
    D3D11_DSV_DIMENSION ViewDimension;
    UINT Flags;
    union {
        D3D11_TEX2D_DSV Texture2D;
        D3D11_TEX2D_ARRAY_DSV Texture2DArray;
    };
} D3D11_DEPTH_STENCIL_VIEW_DESC;

typedef struct DXGI_MODE_DESC {
    UINT Width, Height;
    DXGI_RATIONAL RefreshRate;
    DXGI_FORMAT Format;
    UINT ScanlineOrdering, Scaling;
} DXGI_MODE_DESC;

typedef struct DXGI_SWAP_CHAIN_DESC {
    DXGI_MODE_DESC BufferDesc;
    DXGI_SAMPLE_DESC SampleDesc;
    UINT BufferUsage, BufferCount;
    HWND OutputWindow;              /* the SDL_Window * (src/platform/host_sdl.h) */
    BOOL Windowed;
    DXGI_SWAP_EFFECT SwapEffect;
    UINT Flags;
} DXGI_SWAP_CHAIN_DESC;

/* ---- functions ----------------------------------------------------------- */

HRESULT D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
    const DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swap_chain,
    ID3D11Device **device, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context);

/* Every object: one reference count, one release. */
ULONG vk11_add_ref(void *object);
ULONG vk11_release(void *object);

HRESULT vk11_dev_CreateBuffer(ID3D11Device *, const D3D11_BUFFER_DESC *,
                              const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);
HRESULT vk11_dev_CreateTexture2D(ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
                                 const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
HRESULT vk11_dev_CreateTexture3D(ID3D11Device *, const D3D11_TEXTURE3D_DESC *,
                                 const D3D11_SUBRESOURCE_DATA *, ID3D11Texture3D **);
HRESULT vk11_dev_CreateShaderResourceView(ID3D11Device *, ID3D11Resource *,
                                          const D3D11_SHADER_RESOURCE_VIEW_DESC *,
                                          ID3D11ShaderResourceView **);
HRESULT vk11_dev_CreateRenderTargetView(ID3D11Device *, ID3D11Resource *,
                                        const D3D11_RENDER_TARGET_VIEW_DESC *,
                                        ID3D11RenderTargetView **);
HRESULT vk11_dev_CreateDepthStencilView(ID3D11Device *, ID3D11Resource *,
                                        const D3D11_DEPTH_STENCIL_VIEW_DESC *,
                                        ID3D11DepthStencilView **);
HRESULT vk11_dev_CreateVertexShader(ID3D11Device *, const void *, SIZE_T,
                                    ID3D11ClassLinkage *, ID3D11VertexShader **);
HRESULT vk11_dev_CreatePixelShader(ID3D11Device *, const void *, SIZE_T,
                                   ID3D11ClassLinkage *, ID3D11PixelShader **);
HRESULT vk11_dev_CreateInputLayout(ID3D11Device *, const D3D11_INPUT_ELEMENT_DESC *, UINT,
                                   const void *, SIZE_T, ID3D11InputLayout **);
HRESULT vk11_dev_CreateBlendState(ID3D11Device *, const D3D11_BLEND_DESC *, ID3D11BlendState **);
HRESULT vk11_dev_CreateDepthStencilState(ID3D11Device *, const D3D11_DEPTH_STENCIL_DESC *,
                                         ID3D11DepthStencilState **);
HRESULT vk11_dev_CreateRasterizerState(ID3D11Device *, const D3D11_RASTERIZER_DESC *,
                                       ID3D11RasterizerState **);
HRESULT vk11_dev_CreateSamplerState(ID3D11Device *, const D3D11_SAMPLER_DESC *,
                                    ID3D11SamplerState **);
HRESULT vk11_dev_CheckMultisampleQualityLevels(ID3D11Device *, DXGI_FORMAT, UINT, UINT *);

void vk11_ctx_OMSetRenderTargets(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                 ID3D11DepthStencilView *);
void vk11_ctx_OMGetRenderTargets(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView **,
                                 ID3D11DepthStencilView **);
void vk11_ctx_OMSetBlendState(ID3D11DeviceContext *, ID3D11BlendState *, const FLOAT[4], UINT);
void vk11_ctx_OMSetDepthStencilState(ID3D11DeviceContext *, ID3D11DepthStencilState *, UINT);
void vk11_ctx_RSSetState(ID3D11DeviceContext *, ID3D11RasterizerState *);
void vk11_ctx_RSSetViewports(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
void vk11_ctx_RSGetViewports(ID3D11DeviceContext *, UINT *, D3D11_VIEWPORT *);
void vk11_ctx_RSSetScissorRects(ID3D11DeviceContext *, UINT, const D3D11_RECT *);
void vk11_ctx_RSGetScissorRects(ID3D11DeviceContext *, UINT *, D3D11_RECT *);
void vk11_ctx_IASetInputLayout(ID3D11DeviceContext *, ID3D11InputLayout *);
void vk11_ctx_IASetVertexBuffers(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *,
                                 const UINT *, const UINT *);
void vk11_ctx_IASetIndexBuffer(ID3D11DeviceContext *, ID3D11Buffer *, DXGI_FORMAT, UINT);
void vk11_ctx_IASetPrimitiveTopology(ID3D11DeviceContext *, D3D11_PRIMITIVE_TOPOLOGY);
void vk11_ctx_VSSetShader(ID3D11DeviceContext *, ID3D11VertexShader *,
                          ID3D11ClassInstance *const *, UINT);
void vk11_ctx_PSSetShader(ID3D11DeviceContext *, ID3D11PixelShader *,
                          ID3D11ClassInstance *const *, UINT);
void vk11_ctx_VSSetConstantBuffers(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
void vk11_ctx_PSSetConstantBuffers(ID3D11DeviceContext *, UINT, UINT, ID3D11Buffer *const *);
void vk11_ctx_PSSetShaderResources(ID3D11DeviceContext *, UINT, UINT,
                                   ID3D11ShaderResourceView *const *);
void vk11_ctx_PSSetSamplers(ID3D11DeviceContext *, UINT, UINT, ID3D11SamplerState *const *);
void vk11_ctx_Draw(ID3D11DeviceContext *, UINT, UINT);
void vk11_ctx_DrawIndexed(ID3D11DeviceContext *, UINT, UINT, INT);
void vk11_ctx_ClearRenderTargetView(ID3D11DeviceContext *, ID3D11RenderTargetView *, const FLOAT[4]);
void vk11_ctx_ClearDepthStencilView(ID3D11DeviceContext *, ID3D11DepthStencilView *, UINT,
                                    FLOAT, UINT8);
HRESULT vk11_ctx_Map(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT,
                     D3D11_MAPPED_SUBRESOURCE *);
void vk11_ctx_Unmap(ID3D11DeviceContext *, ID3D11Resource *, UINT);
void vk11_ctx_UpdateSubresource(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *,
                                const void *, UINT, UINT);
void vk11_ctx_CopyResource(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
void vk11_ctx_CopySubresourceRegion(ID3D11DeviceContext *, ID3D11Resource *, UINT, UINT, UINT,
                                    UINT, ID3D11Resource *, UINT, const D3D11_BOX *);
void vk11_ctx_ResolveSubresource(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                 ID3D11Resource *, UINT, DXGI_FORMAT);

void vk11_tex2d_GetDesc(ID3D11Texture2D *, D3D11_TEXTURE2D_DESC *);
void vk11_view_GetResource(void *view, ID3D11Resource **);

HRESULT vk11_swap_Present(IDXGISwapChain *, UINT, UINT);
HRESULT vk11_swap_GetBuffer(IDXGISwapChain *, UINT, const GUID *, void **);
HRESULT vk11_swap_ResizeBuffers(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);

/* Blobs (D3DCompile's output and errors). */
void  *vk11_blob_pointer(ID3D10Blob *);
SIZE_T vk11_blob_size(ID3D10Blob *);
ID3D10Blob *vk11_blob_create(const void *data, SIZE_T size);

/* ---- the COBJMACROS names ------------------------------------------------ */

#define VK11_RELEASE_(p)  vk11_release((void *)(p))
#define VK11_ADDREF_(p)   vk11_add_ref((void *)(p))

#define ID3D11Device_Release(p)                 VK11_RELEASE_(p)
#define ID3D11DeviceContext_Release(p)          VK11_RELEASE_(p)
#define IDXGISwapChain_Release(p)               VK11_RELEASE_(p)
#define ID3D11Resource_Release(p)               VK11_RELEASE_(p)
#define ID3D11Buffer_Release(p)                 VK11_RELEASE_(p)
#define ID3D11Texture2D_Release(p)              VK11_RELEASE_(p)
#define ID3D11Texture2D_AddRef(p)               VK11_ADDREF_(p)
#define ID3D11Texture3D_Release(p)              VK11_RELEASE_(p)
#define ID3D11ShaderResourceView_Release(p)     VK11_RELEASE_(p)
#define ID3D11ShaderResourceView_AddRef(p)      VK11_ADDREF_(p)
#define ID3D11RenderTargetView_Release(p)       VK11_RELEASE_(p)
#define ID3D11DepthStencilView_Release(p)       VK11_RELEASE_(p)
#define ID3D11VertexShader_Release(p)           VK11_RELEASE_(p)
#define ID3D11PixelShader_Release(p)            VK11_RELEASE_(p)
#define ID3D11InputLayout_Release(p)            VK11_RELEASE_(p)
#define ID3D11BlendState_Release(p)             VK11_RELEASE_(p)
#define ID3D11DepthStencilState_Release(p)      VK11_RELEASE_(p)
#define ID3D11RasterizerState_Release(p)        VK11_RELEASE_(p)
#define ID3D11SamplerState_Release(p)           VK11_RELEASE_(p)
#define ID3D11SamplerState_AddRef(p)            VK11_ADDREF_(p)
#define ID3D10Blob_Release(p)                   VK11_RELEASE_(p)
#define ID3D10Blob_GetBufferPointer(p)          vk11_blob_pointer(p)
#define ID3D10Blob_GetBufferSize(p)             vk11_blob_size(p)

#define ID3D11Texture2D_GetDesc(p, d)           vk11_tex2d_GetDesc(p, d)
#define ID3D11RenderTargetView_GetResource(p, r) vk11_view_GetResource((void *)(p), r)

#define ID3D11Device_CreateBuffer               vk11_dev_CreateBuffer
#define ID3D11Device_CreateTexture2D            vk11_dev_CreateTexture2D
#define ID3D11Device_CreateTexture3D            vk11_dev_CreateTexture3D
#define ID3D11Device_CreateShaderResourceView   vk11_dev_CreateShaderResourceView
#define ID3D11Device_CreateRenderTargetView     vk11_dev_CreateRenderTargetView
#define ID3D11Device_CreateDepthStencilView     vk11_dev_CreateDepthStencilView
#define ID3D11Device_CreateVertexShader         vk11_dev_CreateVertexShader
#define ID3D11Device_CreatePixelShader          vk11_dev_CreatePixelShader
#define ID3D11Device_CreateInputLayout          vk11_dev_CreateInputLayout
#define ID3D11Device_CreateBlendState           vk11_dev_CreateBlendState
#define ID3D11Device_CreateDepthStencilState    vk11_dev_CreateDepthStencilState
#define ID3D11Device_CreateRasterizerState      vk11_dev_CreateRasterizerState
#define ID3D11Device_CreateSamplerState         vk11_dev_CreateSamplerState
#define ID3D11Device_CheckMultisampleQualityLevels vk11_dev_CheckMultisampleQualityLevels

#define ID3D11DeviceContext_OMSetRenderTargets      vk11_ctx_OMSetRenderTargets
#define ID3D11DeviceContext_OMGetRenderTargets      vk11_ctx_OMGetRenderTargets
#define ID3D11DeviceContext_OMSetBlendState         vk11_ctx_OMSetBlendState
#define ID3D11DeviceContext_OMSetDepthStencilState  vk11_ctx_OMSetDepthStencilState
#define ID3D11DeviceContext_RSSetState              vk11_ctx_RSSetState
#define ID3D11DeviceContext_RSSetViewports          vk11_ctx_RSSetViewports
#define ID3D11DeviceContext_RSGetViewports          vk11_ctx_RSGetViewports
#define ID3D11DeviceContext_RSSetScissorRects       vk11_ctx_RSSetScissorRects
#define ID3D11DeviceContext_RSGetScissorRects       vk11_ctx_RSGetScissorRects
#define ID3D11DeviceContext_IASetInputLayout        vk11_ctx_IASetInputLayout
#define ID3D11DeviceContext_IASetVertexBuffers      vk11_ctx_IASetVertexBuffers
#define ID3D11DeviceContext_IASetIndexBuffer        vk11_ctx_IASetIndexBuffer
#define ID3D11DeviceContext_IASetPrimitiveTopology  vk11_ctx_IASetPrimitiveTopology
#define ID3D11DeviceContext_VSSetShader             vk11_ctx_VSSetShader
#define ID3D11DeviceContext_PSSetShader             vk11_ctx_PSSetShader
#define ID3D11DeviceContext_VSSetConstantBuffers    vk11_ctx_VSSetConstantBuffers
#define ID3D11DeviceContext_PSSetConstantBuffers    vk11_ctx_PSSetConstantBuffers
#define ID3D11DeviceContext_PSSetShaderResources    vk11_ctx_PSSetShaderResources
#define ID3D11DeviceContext_PSSetSamplers           vk11_ctx_PSSetSamplers
#define ID3D11DeviceContext_Draw                    vk11_ctx_Draw
#define ID3D11DeviceContext_DrawIndexed             vk11_ctx_DrawIndexed
#define ID3D11DeviceContext_ClearRenderTargetView   vk11_ctx_ClearRenderTargetView
#define ID3D11DeviceContext_ClearDepthStencilView   vk11_ctx_ClearDepthStencilView
#define ID3D11DeviceContext_Map                     vk11_ctx_Map
#define ID3D11DeviceContext_Unmap                   vk11_ctx_Unmap
#define ID3D11DeviceContext_UpdateSubresource       vk11_ctx_UpdateSubresource
#define ID3D11DeviceContext_CopyResource            vk11_ctx_CopyResource
#define ID3D11DeviceContext_CopySubresourceRegion   vk11_ctx_CopySubresourceRegion
#define ID3D11DeviceContext_ResolveSubresource      vk11_ctx_ResolveSubresource

#define IDXGISwapChain_Present                      vk11_swap_Present
#define IDXGISwapChain_GetBuffer                    vk11_swap_GetBuffer
#define IDXGISwapChain_ResizeBuffers                vk11_swap_ResizeBuffers

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D11_VK_H */
