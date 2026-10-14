/*
 * d3d11_vk.c - the Direct3D 11 subset of d3d11_vk.h, on Vulkan 1.3.
 *
 * Shape. One device, one immediate context, one swap chain, as src/d3d uses
 * them. Every context call records into the current frame's command buffer
 * in call order; Present submits it, presents, and moves to the next of
 * VK11_FRAMES frames, waiting for that frame's previous use to finish.
 *
 * Synchronisation is deliberately coarse, because correctness here is worth
 * more than the last few percent: every image an application can see lives
 * in VK_IMAGE_LAYOUT_GENERAL for its whole life (the swap chain's own images
 * are never seen -- the "back buffer" is an ordinary image, blitted to the
 * swap chain at Present), and a full memory barrier separates any write
 * (a rendering pass, a copy, an upload, a clear) from whatever comes after
 * it. Draws between two barriers sit in one dynamic-rendering pass, which is
 * restarted, with LOAD, whenever the targets change or a transfer intervenes.
 *
 * Memory. DEFAULT resources are device-local and written by copies from a
 * per-frame upload arena. A DYNAMIC buffer's Map(WRITE_DISCARD) is a fresh
 * slice of that arena -- no wait, no copy -- and NO_OVERWRITE hands back the
 * same slice; when a frame's arena is recycled, a buffer still pointing into
 * it is moved to a private "home" allocation so its contents survive, as
 * D3D11's do. A large one (DYNAMIC_VERSION_MIN and up) has allocations of its
 * own instead, one per DISCARD still in flight, so it is never moved: moving
 * is a CPU read of write-combined memory, which is slow. STAGING resources
 * are host-visible buffers; Map(READ) waits for the GPU only when a copy into
 * the resource is still outstanding.
 *
 * Shaders arrive as SPIR-V (d3dcompiler_vk.c). Vertex inputs are matched to
 * input-layout elements, and pixel-shader inputs to vertex-shader outputs, by
 * HLSL semantic, read from the UserSemantic decorations glslang leaves; a
 * pixel shader is re-numbered per vertex shader it is used with. Pipelines
 * are cached on the whole state that goes into them, and the cache is kept
 * on disk next to the SPIR-V cache.
 *
 * Coordinates. D3D's clip space and depth range are Vulkan's; only Y differs,
 * and a negative-height viewport takes care of it. That viewport puts every
 * vertex at the framebuffer position D3D would, so a triangle's winding on
 * screen is D3D's too: FrontCounterClockwise maps straight to frontFace,
 * with no inversion (DXVK does the same).
 *
 * Switches: RECOMP_VK_VALIDATION=1 loads the Khronos validation layer,
 * RECOMP_VK_DEVICE=<n> picks a physical device by index, RECOMP_VK_LOG=1
 * reports pipeline creation.
 */
#include "d3d11_vk_internal.h"
#include "../../platform/host_sdl.h"

#include <vulkan/vulkan.h>
#include <SDL.h>
#include <SDL_vulkan.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define VK11_FRAMES        3
#define ARENA_BLOCK_SIZE   (32u << 20)
/* A DYNAMIC buffer this big takes its own allocations on Map(DISCARD), not
 * the arena's (see Memory above). The title's 4 MB vertex and index rings
 * are discarded every 12 frames or so and written with NO_OVERWRITE in
 * between, so each sat in an arena when it was recycled, and copying it out
 * of write-combined memory took 19 ms -- long enough for Future Perfect to
 * read its vblank clock one tick late and step its world 3 ticks, then 1. */
#define DYNAMIC_VERSION_MIN (256u << 10)
#define DYNAMIC_VERSIONS   8
#define VS_CBUFFER_BASE    VK11_BINDING_CBUFFER
#define PS_CBUFFER_BASE    (VK11_BINDING_CBUFFER + VK11_MAX_CBUFFERS)
#define DUMMY_VB_BINDING   15u
#define MAX_VB             16u
#define MAX_IO             32

const GUID IID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89,
                                   { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

/* ======================================================================
 * Objects
 * ====================================================================== */

enum {
    K_DEVICE = 1, K_CONTEXT, K_SWAPCHAIN, K_BUFFER, K_TEXTURE, K_SRV, K_RTV, K_DSV,
    K_VS, K_PS, K_LAYOUT, K_BLEND, K_DSS, K_RSS, K_SAMPLER, K_BLOB
};

typedef struct Obj {
    uint32_t kind;
    volatile int32_t refs;
    uint64_t serial;            /* unique for the process: pointers get reused */
} Obj;

struct ID3D10Blob {
    Obj o;
    SIZE_T size;
    uint64_t pad;               /* keeps data 8-aligned for SPIR-V words */
    unsigned char data[];
};

/* Format facts: what Vulkan calls it, how big an element (texel or 4x4
 * block) is, which aspects it has and the view swizzle that makes it read
 * as the DXGI format does. */
typedef struct Fmt {
    VkFormat vk;
    uint32_t bpe;               /* bytes per element */
    uint32_t block;             /* 1, or 4 for BCn */
    VkImageAspectFlags aspect;
    VkComponentMapping swz;
} Fmt;

typedef struct Image {
    Obj o;
    int is3d;
    D3D11_TEXTURE2D_DESC d2;
    D3D11_TEXTURE3D_DESC d3;
    Fmt f;
    uint32_t w, h, depth, mips, layers;
    /* Samples per pixel: 1, or a multisampled render target or depth
     * buffer (SampleDesc.Count), which is drawn into and resolved, never
     * sampled or copied. */
    uint32_t samples;
    VkImage img;
    VkDeviceMemory mem;
    /* USAGE_STAGING: a host buffer holding subresource 0, tightly pitched. */
    VkBuffer sbuf;
    VkDeviceMemory smem;
    uint8_t *sptr;
    uint32_t srow, sslice;
    int pending_gpu_write;
    /* Map of a DYNAMIC texture: where the bytes go until Unmap copies them. */
    VkBuffer map_buf;
    VkDeviceSize map_off;
    uint8_t *map_ptr;
    uint32_t map_row, map_sub;
} Image;

typedef struct View {
    Obj o;
    Image *res;                 /* referenced */
    VkImageView view;
    uint32_t mip, mip_count, layer, layer_count;
    uint32_t w, h;              /* of the first mip in the view */
    VkFormat fmt;
    VkImageViewType type;
} View;

typedef struct Buffer {
    Obj o;
    D3D11_BUFFER_DESC desc;
    VkBuffer buf;               /* DEFAULT / IMMUTABLE / STAGING */
    VkDeviceMemory mem;
    uint8_t *host;              /* STAGING */
    int pending_gpu_write;
    /* DYNAMIC */
    VkBuffer home_buf;
    VkDeviceMemory home_mem;
    uint8_t *home_ptr;
    VkBuffer cur_buf;
    VkDeviceSize cur_off;
    uint8_t *cur_ptr;
    int cur_slot;               /* -2 nothing yet, -1 home or a version, else arena frame */
    struct Buffer *dyn_next, *dyn_prev;
    /* DYNAMIC, DYNAMIC_VERSION_MIN and up: what Map(DISCARD) hands out. A
     * version is free again once the GPU has finished frame busy_until. */
    struct {
        VkBuffer buf;
        VkDeviceMemory mem;
        uint8_t *ptr;
        uint64_t busy_until;
    } ver[DYNAMIC_VERSIONS];
    int nver, cur_ver;          /* cur_ver -1: not on a version */
} Buffer;

typedef struct IoVar {
    char sem[40];               /* canonical: NAME#index */
    uint32_t loc;
    uint32_t loc_word;          /* word index of the Location literal */
} IoVar;

typedef struct PsVariant {
    uint64_t vs_serial;
    VkShaderModule module;
} PsVariant;

typedef struct Shader {
    Obj o;
    int stage;                  /* 0 vertex, 1 pixel */
    uint32_t *spv;
    size_t words;
    VkShaderModule module;
    char entry[64];
    int n_in, n_out;
    IoVar in[MAX_IO], out[MAX_IO];
    uint8_t tex_dim[VK11_MAX_TEXTURES];   /* SpvDim (1 2D, 2 3D, 3 cube) + 1; 0 unused */
    PsVariant *var;
    int nvar, capvar;
} Shader;

typedef struct LayoutElem {
    char sem[40];
    VkFormat fmt;
    uint32_t slot, offset, per_instance, step;
} LayoutElem;

typedef struct Layout {
    Obj o;
    uint32_t n;
    LayoutElem e[32];
} Layout;

typedef struct BlendState   { Obj o; D3D11_BLEND_DESC d; } BlendState;
typedef struct DSState      { Obj o; D3D11_DEPTH_STENCIL_DESC d; } DSState;
typedef struct RSState      { Obj o; D3D11_RASTERIZER_DESC d; } RSState;
typedef struct Sampler      { Obj o; D3D11_SAMPLER_DESC d; VkSampler s; } Sampler;

struct ID3D11Device        { Obj o; };
struct ID3D11DeviceContext { Obj o; };
struct IDXGISwapChain      { Obj o; };

/* ======================================================================
 * Global state
 * ====================================================================== */

typedef struct ArenaBlock {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *ptr;
    VkDeviceSize size, used;
    struct ArenaBlock *next;
} ArenaBlock;

typedef enum { G_BUFFER, G_IMAGE, G_VIEW, G_MEMORY, G_MODULE, G_SAMPLER, G_PIPELINE } GarbageKind;
typedef struct Garbage { GarbageKind kind; uint64_t handle; } Garbage;

typedef struct Frame {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    int submitted;
    uint64_t serial;            /* G.rec_serial when it was submitted */
    ArenaBlock *arena;          /* blocks in use this frame */
    VkDescriptorPool *dpools;
    int ndpools, capdpools, cur_dpool;
    Garbage *garbage;
    int ngarbage, capgarbage;
} Frame;

typedef struct PipeKey {
    uint64_t vs, ps, il;
    D3D11_BLEND_DESC blend;
    D3D11_DEPTH_STENCIL_DESC ds;
    D3D11_RASTERIZER_DESC rs;
    uint32_t topology;
    uint32_t nrt;
    VkFormat rt[8];
    VkFormat dsfmt;
    uint32_t samples;           /* of the pass's attachments */
} PipeKey;

typedef struct PipeEntry {
    PipeKey key;
    uint64_t hash;
    VkPipeline pipe;
    struct PipeEntry *next;
} PipeEntry;

#define PIPE_BUCKETS 1024

static struct {
    pthread_mutex_t lock;               /* recursive: Release can run inside a call */
    int ready;
    uint64_t next_serial;

    VkInstance instance;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memprops;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfamily;
    int has_custom_border, has_depth_clamp, has_aniso, has_fill_nonsolid,
        has_bias_clamp, has_dual_src, has_4444;
    VkFormat d24s8;             /* D24_UNORM_S8_UINT, or D32_SFLOAT_S8_UINT */

    /* presentation */
    SDL_Window *window;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D swap_extent;
    uint32_t swap_count;
    VkImage swap_images[8];
    VkSemaphore swap_done[8];
    VkPresentModeKHR present_mode;
    int present_interval;
    int swap_dirty;
    Image *backbuffer;

    /* frames */
    Frame frames[VK11_FRAMES];
    int fi;
    uint64_t rec_serial;        /* the frame being recorded; the first is 1 */
    uint64_t done_serial;       /* every frame up to this one has finished */
    VkCommandBuffer cmd;        /* frames[fi].cmd while recording */
    ArenaBlock *free_blocks;
    Buffer *dyn_head;

    /* binding model */
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipe_layout;
    VkDescriptorUpdateTemplate dtemplate;
    VkPipelineCache pcache;
    char pcache_path[512];
    PipeEntry *pipes[PIPE_BUCKETS];
    int npipes;

    /* defaults for what is not bound */
    VkBuffer zero_buf;
    VkDeviceMemory zero_mem;
    Image *dummy_tex2d, *dummy_cube, *dummy_tex3d;
    VkImageView dummy_view2d, dummy_viewcube, dummy_view3d;
    VkSampler dummy_sampler;

    /* sampler cache (never freed: the renderer asks for few distinct ones) */
    struct { D3D11_SAMPLER_DESC d; VkSampler s; } samplers[512];
    int nsamplers;
} G;

/* The context's bound state. */
static struct {
    View *rtv[8];
    uint32_t nrtv;
    View *dsv;
    BlendState *blend;
    float blend_factor[4];
    DSState *dss;
    uint32_t stencil_ref;
    RSState *rs;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    uint32_t nvp;
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    uint32_t nsc;
    Layout *il;
    Buffer *vb[MAX_VB];
    uint32_t vb_stride[MAX_VB], vb_off[MAX_VB];
    Buffer *ib;
    DXGI_FORMAT ib_fmt;
    uint32_t ib_off;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    Shader *vs, *ps;
    Buffer *vs_cb[VK11_MAX_CBUFFERS], *ps_cb[VK11_MAX_CBUFFERS];
    View *srv[VK11_MAX_TEXTURES];
    Sampler *smp[VK11_MAX_SAMPLERS];

    /* recording state */
    int rendering;
    View *pass_rtv[8];
    uint32_t pass_nrtv;
    View *pass_dsv;
    VkExtent2D pass_extent;
    int need_barrier;
    VkPipeline bound_pipe;
    int dirty_desc;
} S;

static ID3D11Device        s_device    = { { K_DEVICE, 1, 0 } };
static pthread_mutex_t     s_lock_init = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static ID3D11DeviceContext s_context   = { { K_CONTEXT, 1, 0 } };
static IDXGISwapChain      s_swapchain = { { K_SWAPCHAIN, 1, 0 } };

#define LOCK()   pthread_mutex_lock(&s_lock_init)
#define UNLOCK() pthread_mutex_unlock(&s_lock_init)

static int vk_log(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("RECOMP_VK_LOG");
        v = e && *e && *e != '0';
    }
    return v;
}

#define VKCHECK(call, what) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "[VK] %s failed: %d\n", what, (int)r_); fflush(stderr); } } while (0)

static void *obj_new(size_t size, uint32_t kind)
{
    Obj *o = (Obj *)calloc(1, size);
    if (!o)
        return NULL;
    o->kind = kind;
    o->refs = 1;
    o->serial = __atomic_add_fetch(&G.next_serial, 1, __ATOMIC_SEQ_CST);
    return o;
}

/* ======================================================================
 * Formats
 * ====================================================================== */

#define SWZ_ID   { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, \
                   VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY }

static Fmt fmt_info(DXGI_FORMAT f)
{
    Fmt r;
    VkComponentMapping id = SWZ_ID;

    memset(&r, 0, sizeof r);
    r.swz = id;
    r.block = 1;
    r.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:  r.vk = VK_FORMAT_R32G32B32A32_SFLOAT; r.bpe = 16; break;
    case DXGI_FORMAT_R32G32B32A32_UINT:   r.vk = VK_FORMAT_R32G32B32A32_UINT; r.bpe = 16; break;
    case DXGI_FORMAT_R32G32B32A32_SINT:   r.vk = VK_FORMAT_R32G32B32A32_SINT; r.bpe = 16; break;
    case DXGI_FORMAT_R32G32B32_FLOAT:     r.vk = VK_FORMAT_R32G32B32_SFLOAT; r.bpe = 12; break;
    case DXGI_FORMAT_R32G32B32_UINT:      r.vk = VK_FORMAT_R32G32B32_UINT; r.bpe = 12; break;
    case DXGI_FORMAT_R32G32B32_SINT:      r.vk = VK_FORMAT_R32G32B32_SINT; r.bpe = 12; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:  r.vk = VK_FORMAT_R16G16B16A16_SFLOAT; r.bpe = 8; break;
    case DXGI_FORMAT_R16G16B16A16_UNORM:  r.vk = VK_FORMAT_R16G16B16A16_UNORM; r.bpe = 8; break;
    case DXGI_FORMAT_R16G16B16A16_UINT:   r.vk = VK_FORMAT_R16G16B16A16_UINT; r.bpe = 8; break;
    case DXGI_FORMAT_R16G16B16A16_SNORM:  r.vk = VK_FORMAT_R16G16B16A16_SNORM; r.bpe = 8; break;
    case DXGI_FORMAT_R16G16B16A16_SINT:   r.vk = VK_FORMAT_R16G16B16A16_SINT; r.bpe = 8; break;
    case DXGI_FORMAT_R32G32_FLOAT:        r.vk = VK_FORMAT_R32G32_SFLOAT; r.bpe = 8; break;
    case DXGI_FORMAT_R32G32_UINT:         r.vk = VK_FORMAT_R32G32_UINT; r.bpe = 8; break;
    case DXGI_FORMAT_R32G32_SINT:         r.vk = VK_FORMAT_R32G32_SINT; r.bpe = 8; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:   r.vk = VK_FORMAT_A2B10G10R10_UNORM_PACK32; r.bpe = 4; break;
    case DXGI_FORMAT_R10G10B10A2_UINT:    r.vk = VK_FORMAT_A2B10G10R10_UINT_PACK32; r.bpe = 4; break;
    case DXGI_FORMAT_R11G11B10_FLOAT:     r.vk = VK_FORMAT_B10G11R11_UFLOAT_PACK32; r.bpe = 4; break;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:      r.vk = VK_FORMAT_R8G8B8A8_UNORM; r.bpe = 4; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: r.vk = VK_FORMAT_R8G8B8A8_SRGB; r.bpe = 4; break;
    case DXGI_FORMAT_R8G8B8A8_UINT:       r.vk = VK_FORMAT_R8G8B8A8_UINT; r.bpe = 4; break;
    case DXGI_FORMAT_R8G8B8A8_SNORM:      r.vk = VK_FORMAT_R8G8B8A8_SNORM; r.bpe = 4; break;
    case DXGI_FORMAT_R8G8B8A8_SINT:       r.vk = VK_FORMAT_R8G8B8A8_SINT; r.bpe = 4; break;
    case DXGI_FORMAT_R16G16_FLOAT:        r.vk = VK_FORMAT_R16G16_SFLOAT; r.bpe = 4; break;
    case DXGI_FORMAT_R16G16_UNORM:        r.vk = VK_FORMAT_R16G16_UNORM; r.bpe = 4; break;
    case DXGI_FORMAT_R16G16_UINT:         r.vk = VK_FORMAT_R16G16_UINT; r.bpe = 4; break;
    case DXGI_FORMAT_R16G16_SNORM:        r.vk = VK_FORMAT_R16G16_SNORM; r.bpe = 4; break;
    case DXGI_FORMAT_R16G16_SINT:         r.vk = VK_FORMAT_R16G16_SINT; r.bpe = 4; break;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:           r.vk = VK_FORMAT_R32_SFLOAT; r.bpe = 4; break;
    case DXGI_FORMAT_R32_UINT:            r.vk = VK_FORMAT_R32_UINT; r.bpe = 4; break;
    case DXGI_FORMAT_R32_SINT:            r.vk = VK_FORMAT_R32_SINT; r.bpe = 4; break;
    case DXGI_FORMAT_D32_FLOAT:
        r.vk = VK_FORMAT_D32_SFLOAT; r.bpe = 4; r.aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        r.vk = G.d24s8 ? G.d24s8 : VK_FORMAT_D24_UNORM_S8_UINT;
        r.bpe = 4;
        r.aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        break;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:          r.vk = VK_FORMAT_R8G8_UNORM; r.bpe = 2; break;
    case DXGI_FORMAT_R8G8_UINT:           r.vk = VK_FORMAT_R8G8_UINT; r.bpe = 2; break;
    case DXGI_FORMAT_R8G8_SNORM:          r.vk = VK_FORMAT_R8G8_SNORM; r.bpe = 2; break;
    case DXGI_FORMAT_R8G8_SINT:           r.vk = VK_FORMAT_R8G8_SINT; r.bpe = 2; break;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:           r.vk = VK_FORMAT_R16_SFLOAT; r.bpe = 2; break;
    case DXGI_FORMAT_D16_UNORM:
        r.vk = VK_FORMAT_D16_UNORM; r.bpe = 2; r.aspect = VK_IMAGE_ASPECT_DEPTH_BIT; break;
    case DXGI_FORMAT_R16_UNORM:           r.vk = VK_FORMAT_R16_UNORM; r.bpe = 2; break;
    case DXGI_FORMAT_R16_UINT:            r.vk = VK_FORMAT_R16_UINT; r.bpe = 2; break;
    case DXGI_FORMAT_R16_SNORM:           r.vk = VK_FORMAT_R16_SNORM; r.bpe = 2; break;
    case DXGI_FORMAT_R16_SINT:            r.vk = VK_FORMAT_R16_SINT; r.bpe = 2; break;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:            r.vk = VK_FORMAT_R8_UNORM; r.bpe = 1; break;
    case DXGI_FORMAT_R8_UINT:             r.vk = VK_FORMAT_R8_UINT; r.bpe = 1; break;
    case DXGI_FORMAT_R8_SNORM:            r.vk = VK_FORMAT_R8_SNORM; r.bpe = 1; break;
    case DXGI_FORMAT_R8_SINT:             r.vk = VK_FORMAT_R8_SINT; r.bpe = 1; break;
    case DXGI_FORMAT_A8_UNORM:
        /* Reads as (0, 0, 0, a), as DXGI's does. */
        r.vk = VK_FORMAT_R8_UNORM; r.bpe = 1;
        r.swz.r = VK_COMPONENT_SWIZZLE_ZERO; r.swz.g = VK_COMPONENT_SWIZZLE_ZERO;
        r.swz.b = VK_COMPONENT_SWIZZLE_ZERO; r.swz.a = VK_COMPONENT_SWIZZLE_R;
        break;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM: r.vk = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; r.bpe = 8; r.block = 4; break;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM: r.vk = VK_FORMAT_BC2_UNORM_BLOCK; r.bpe = 16; r.block = 4; break;
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM: r.vk = VK_FORMAT_BC3_UNORM_BLOCK; r.bpe = 16; r.block = 4; break;
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM: r.vk = VK_FORMAT_BC4_UNORM_BLOCK; r.bpe = 8; r.block = 4; break;
    case DXGI_FORMAT_BC4_SNORM: r.vk = VK_FORMAT_BC4_SNORM_BLOCK; r.bpe = 8; r.block = 4; break;
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM: r.vk = VK_FORMAT_BC5_UNORM_BLOCK; r.bpe = 16; r.block = 4; break;
    case DXGI_FORMAT_BC5_SNORM: r.vk = VK_FORMAT_BC5_SNORM_BLOCK; r.bpe = 16; r.block = 4; break;
    /* DXGI names packed formats from the low bits up, Vulkan from the high
     * bits down: the same bits, opposite spellings. */
    case DXGI_FORMAT_B5G6R5_UNORM:   r.vk = VK_FORMAT_R5G6B5_UNORM_PACK16; r.bpe = 2; break;
    case DXGI_FORMAT_B5G5R5A1_UNORM: r.vk = VK_FORMAT_A1R5G5B5_UNORM_PACK16; r.bpe = 2; break;
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        r.bpe = 2;
        if (G.has_4444) {
            r.vk = VK_FORMAT_A4R4G4B4_UNORM_PACK16;
        } else {
            /* B4G4R4A4_PACK16 holds the same nibbles in another order. */
            r.vk = VK_FORMAT_B4G4R4A4_UNORM_PACK16;
            r.swz.r = VK_COMPONENT_SWIZZLE_G; r.swz.g = VK_COMPONENT_SWIZZLE_R;
            r.swz.b = VK_COMPONENT_SWIZZLE_A; r.swz.a = VK_COMPONENT_SWIZZLE_B;
        }
        break;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:      r.vk = VK_FORMAT_B8G8R8A8_UNORM; r.bpe = 4; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: r.vk = VK_FORMAT_B8G8R8A8_SRGB; r.bpe = 4; break;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        r.vk = VK_FORMAT_B8G8R8A8_UNORM; r.bpe = 4; r.swz.a = VK_COMPONENT_SWIZZLE_ONE; break;
    case DXGI_FORMAT_R8G8_B8G8_UNORM:     r.vk = VK_FORMAT_B8G8R8G8_422_UNORM; r.bpe = 4; break;
    case DXGI_FORMAT_G8R8_G8B8_UNORM:     r.vk = VK_FORMAT_G8B8G8R8_422_UNORM; r.bpe = 4; break;
    default:
        r.vk = VK_FORMAT_UNDEFINED;
        break;
    }
    return r;
}

static VkFormat vertex_format(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_B8G8R8X8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_A8_UNORM:       return VK_FORMAT_R8_UNORM;
    default:                         return fmt_info(f).vk;
    }
}

static int has_stencil(VkFormat f)
{
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           f == VK_FORMAT_D16_UNORM_S8_UINT;
}

/* ======================================================================
 * Memory
 * ====================================================================== */

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;

    for (i = 0; i < G.memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) &&
            (G.memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static VkDeviceMemory alloc_memory(VkMemoryRequirements req, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo ai;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    int t = mem_type(req.memoryTypeBits, want);

    if (t < 0 && (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        t = mem_type(req.memoryTypeBits, want & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) {
        fprintf(stderr, "[VK] no memory type for 0x%x\n", (unsigned)want);
        return VK_NULL_HANDLE;
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (vkAllocateMemory(G.dev, &ai, NULL, &mem) != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkAllocateMemory(%llu) failed\n", (unsigned long long)req.size);
        return VK_NULL_HANDLE;
    }
    return mem;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                       VkBuffer *buf, VkDeviceMemory *mem, uint8_t **ptr)
{
    VkBufferCreateInfo bi;
    VkMemoryRequirements req;

    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size ? size : 16;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(G.dev, &bi, NULL, buf) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(G.dev, *buf, &req);
    *mem = alloc_memory(req, want);
    if (!*mem) {
        vkDestroyBuffer(G.dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return 0;
    }
    vkBindBufferMemory(G.dev, *buf, *mem, 0);
    if (ptr) {
        *ptr = NULL;
        if (vkMapMemory(G.dev, *mem, 0, VK_WHOLE_SIZE, 0, (void **)ptr) != VK_SUCCESS)
            return 0;
    }
    return 1;
}

#define HOST_MEM (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
#define ALL_BUFFER_USAGE (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | \
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | \
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)

static void garbage_add(GarbageKind kind, uint64_t handle)
{
    Frame *f = &G.frames[G.fi];

    if (!handle)
        return;
    if (f->ngarbage == f->capgarbage) {
        int cap = f->capgarbage ? f->capgarbage * 2 : 256;
        Garbage *g = (Garbage *)realloc(f->garbage, sizeof *g * (size_t)cap);
        if (!g)
            return;                     /* leak rather than crash */
        f->garbage = g;
        f->capgarbage = cap;
    }
    f->garbage[f->ngarbage].kind = kind;
    f->garbage[f->ngarbage].handle = handle;
    f->ngarbage++;
}

static void garbage_run(Frame *f)
{
    int i;

    for (i = 0; i < f->ngarbage; i++) {
        uint64_t h = f->garbage[i].handle;
        switch (f->garbage[i].kind) {
        case G_BUFFER:  vkDestroyBuffer(G.dev, (VkBuffer)h, NULL); break;
        case G_IMAGE:   vkDestroyImage(G.dev, (VkImage)h, NULL); break;
        case G_VIEW:    vkDestroyImageView(G.dev, (VkImageView)h, NULL); break;
        case G_MEMORY:  vkFreeMemory(G.dev, (VkDeviceMemory)h, NULL); break;
        case G_MODULE:  vkDestroyShaderModule(G.dev, (VkShaderModule)h, NULL); break;
        case G_SAMPLER: vkDestroySampler(G.dev, (VkSampler)h, NULL); break;
        case G_PIPELINE: vkDestroyPipeline(G.dev, (VkPipeline)h, NULL); break;
        }
    }
    f->ngarbage = 0;
}

/* ---- the upload arena ---------------------------------------------------- */

static ArenaBlock *arena_block_new(VkDeviceSize min)
{
    ArenaBlock *b, **pp;
    VkDeviceSize size = min > ARENA_BLOCK_SIZE ? min : ARENA_BLOCK_SIZE;

    for (pp = &G.free_blocks; *pp; pp = &(*pp)->next) {
        if ((*pp)->size >= size) {
            b = *pp;
            *pp = b->next;
            b->used = 0;
            b->next = NULL;
            return b;
        }
    }
    b = (ArenaBlock *)calloc(1, sizeof *b);
    if (!b)
        return NULL;
    if (!make_buffer(size, ALL_BUFFER_USAGE, HOST_MEM, &b->buf, &b->mem, &b->ptr)) {
        free(b);
        return NULL;
    }
    b->size = size;
    return b;
}

/* `size` bytes of host-visible memory valid until this frame is recycled. */
static uint8_t *arena_alloc(VkDeviceSize size, VkDeviceSize align,
                            VkBuffer *buf, VkDeviceSize *off)
{
    Frame *f = &G.frames[G.fi];
    ArenaBlock *b = f->arena;
    VkDeviceSize at;

    if (align < 16)
        align = 16;
    if (b) {
        at = (b->used + align - 1) & ~(align - 1);
        if (at + size > b->size)
            b = NULL;
    }
    if (!b) {
        b = arena_block_new(size + align);
        if (!b)
            return NULL;
        b->next = f->arena;
        f->arena = b;
        at = 0;
    }
    b->used = at + size;
    *buf = b->buf;
    *off = at;
    return b->ptr + at;
}

static void arena_reset(Frame *f)
{
    while (f->arena) {
        ArenaBlock *b = f->arena;
        f->arena = b->next;
        b->next = G.free_blocks;
        G.free_blocks = b;
    }
}

/* ======================================================================
 * Command recording helpers
 * ====================================================================== */

static void end_rendering(void)
{
    if (S.rendering) {
        vkCmdEndRendering(G.cmd);
        S.rendering = 0;
        S.need_barrier = 1;
    }
}

/* Everything written before is visible to everything after. */
static void barrier_all(void)
{
    VkMemoryBarrier mb;

    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(G.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    S.need_barrier = 0;
}

/* Before a copy, clear or upload: out of any pass, after earlier writes. */
static void begin_transfer(void)
{
    end_rendering();
    if (S.need_barrier)
        barrier_all();
}

static void end_transfer(void)
{
    S.need_barrier = 1;
}

static void image_layout(VkImage img, VkImageAspectFlags aspect, VkImageLayout from,
                         VkImageLayout to)
{
    VkImageMemoryBarrier ib;

    memset(&ib, 0, sizeof ib);
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    ib.oldLayout = from;
    ib.newLayout = to;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img;
    ib.subresourceRange.aspectMask = aspect;
    ib.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    ib.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdPipelineBarrier(G.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
}

static void begin_commands(void)
{
    VkCommandBufferBeginInfo bi;

    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    G.cmd = G.frames[G.fi].cmd;
    vkBeginCommandBuffer(G.cmd, &bi);
    S.rendering = 0;
    S.bound_pipe = VK_NULL_HANDLE;
    S.dirty_desc = 1;
    S.need_barrier = 1;
}

/* A dynamic buffer whose bytes live in the arena of frame `slot`, which is
 * about to be reused: give it a home of its own, with the same contents. */
static void relocate_dynamic(int slot)
{
    Buffer *b;

    for (b = G.dyn_head; b; b = b->dyn_next) {
        if (b->cur_slot != slot)
            continue;
        if (!b->home_buf &&
            !make_buffer(b->desc.ByteWidth, ALL_BUFFER_USAGE, HOST_MEM,
                         &b->home_buf, &b->home_mem, &b->home_ptr)) {
            b->cur_slot = -2;
            continue;
        }
        memcpy(b->home_ptr, b->cur_ptr, b->desc.ByteWidth);
        b->cur_buf = b->home_buf;
        b->cur_off = 0;
        b->cur_ptr = b->home_ptr;
        b->cur_slot = -1;
    }
}

/* Make frame `fi` current: wait for its last submission, then recycle. */
static void frame_begin(int fi)
{
    Frame *f = &G.frames[fi];

    G.fi = fi;
    if (f->submitted) {
        vkWaitForFences(G.dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
        f->submitted = 0;
        if (f->serial > G.done_serial)
            G.done_serial = f->serial;
    }
    vkResetFences(G.dev, 1, &f->fence);
    garbage_run(f);
    relocate_dynamic(fi);
    arena_reset(f);
    if (f->ndpools) {
        int i;
        for (i = 0; i < f->ndpools; i++)
            vkResetDescriptorPool(G.dev, f->dpools[i], 0);
    }
    f->cur_dpool = 0;
    vkResetCommandPool(G.dev, f->pool, 0);
    begin_commands();
}

/* Submit what is recorded and wait for the GPU (a staging read). The frame
 * carries on in a fresh command buffer; its arena stays valid. */
static void flush_and_wait(void)
{
    VkSubmitInfo si;

    end_rendering();
    vkEndCommandBuffer(G.cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &G.cmd;
    VKCHECK(vkQueueSubmit(G.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit (flush)");
    vkQueueWaitIdle(G.queue);
    vkResetCommandBuffer(G.cmd, 0);
    begin_commands();
}

static VkDescriptorSet alloc_descriptor_set(void)
{
    Frame *f = &G.frames[G.fi];
    VkDescriptorSetAllocateInfo ai;
    VkDescriptorSet set = VK_NULL_HANDLE;

    for (;;) {
        if (f->cur_dpool >= f->ndpools) {
            VkDescriptorPoolSize sizes[3];
            VkDescriptorPoolCreateInfo pi;
            VkDescriptorPool pool;

            sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            sizes[0].descriptorCount = 1024 * VK11_MAX_CBUFFERS * 2;
            sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            sizes[1].descriptorCount = 1024 * VK11_MAX_TEXTURES;
            sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
            sizes[2].descriptorCount = 1024 * VK11_MAX_SAMPLERS;
            memset(&pi, 0, sizeof pi);
            pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            pi.maxSets = 1024;
            pi.poolSizeCount = 3;
            pi.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(G.dev, &pi, NULL, &pool) != VK_SUCCESS)
                return VK_NULL_HANDLE;
            if (f->ndpools == f->capdpools) {
                int cap = f->capdpools ? f->capdpools * 2 : 4;
                VkDescriptorPool *p = (VkDescriptorPool *)realloc(f->dpools, sizeof *p * (size_t)cap);
                if (!p) {
                    vkDestroyDescriptorPool(G.dev, pool, NULL);
                    return VK_NULL_HANDLE;
                }
                f->dpools = p;
                f->capdpools = cap;
            }
            f->dpools[f->ndpools++] = pool;
        }
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = f->dpools[f->cur_dpool];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &G.set_layout;
        if (vkAllocateDescriptorSets(G.dev, &ai, &set) == VK_SUCCESS)
            return set;
        f->cur_dpool++;
    }
}

/* ======================================================================
 * Device
 * ====================================================================== */

typedef struct { SDL_Window *win; unsigned count; const char *names[16]; int ok; } ExtQuery;
typedef struct { SDL_Window *win; VkInstance inst; VkSurfaceKHR surf; int ok; } SurfQuery;
typedef struct { SDL_Window *win; int w, h; } SizeQuery;

/* SDL's video calls belong to its event thread (host_sdl.c). */
static void q_extensions(void *p)
{
    ExtQuery *q = (ExtQuery *)p;
    q->count = 16;
    q->ok = SDL_Vulkan_GetInstanceExtensions(q->win, &q->count, q->names) == SDL_TRUE;
}

static void q_surface(void *p)
{
    SurfQuery *q = (SurfQuery *)p;
    q->ok = SDL_Vulkan_CreateSurface(q->win, q->inst, &q->surf) == SDL_TRUE;
}

static void q_size(void *p)
{
    SizeQuery *q = (SizeQuery *)p;
    SDL_Vulkan_GetDrawableSize(q->win, &q->w, &q->h);
}

static VkExtent2D window_extent(void)
{
    SizeQuery q;
    VkExtent2D e;

    q.win = G.window;
    q.w = q.h = 0;
    host_sdl_run_on_event_thread(q_size, &q);
    e.width = q.w > 0 ? (uint32_t)q.w : 0;
    e.height = q.h > 0 ? (uint32_t)q.h : 0;
    return e;
}

static int format_supports(VkFormat f, VkFormatFeatureFlags want)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(G.phys, f, &fp);
    return (fp.optimalTilingFeatures & want) == want;
}

static int create_instance(void)
{
    ExtQuery eq;
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    const char *layers[1] = { "VK_LAYER_KHRONOS_validation" };
    const char *v = getenv("RECOMP_VK_VALIDATION");
    VkResult r;

    memset(&eq, 0, sizeof eq);
    eq.win = G.window;
    host_sdl_run_on_event_thread(q_extensions, &eq);
    if (!eq.ok) {
        fprintf(stderr, "[VK] SDL_Vulkan_GetInstanceExtensions: %s\n", SDL_GetError());
        return 0;
    }
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "xboxrecomp";
    app.pEngineName = "xboxrecomp d3d11_vk";
    app.apiVersion = VK_API_VERSION_1_3;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = eq.count;
    ci.ppEnabledExtensionNames = eq.names;
    if (v && *v && *v != '0') {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = layers;
    }
    r = vkCreateInstance(&ci, NULL, &G.instance);
    if (r != VK_SUCCESS && ci.enabledLayerCount) {
        fprintf(stderr, "[VK] validation layer unavailable; continuing without it\n");
        ci.enabledLayerCount = 0;
        r = vkCreateInstance(&ci, NULL, &G.instance);
    }
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkCreateInstance failed (%d); is a Vulkan 1.3 driver installed?\n", (int)r);
        return 0;
    }
    return 1;
}

static int pick_device(void)
{
    VkPhysicalDevice devs[16];
    uint32_t n = 16, i, best = UINT32_MAX;
    int best_score = -1;
    const char *want = getenv("RECOMP_VK_DEVICE");

    if (vkEnumeratePhysicalDevices(G.instance, &n, devs) < 0 || n == 0) {
        fprintf(stderr, "[VK] no Vulkan devices\n");
        return 0;
    }
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16, q;
        int score;

        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qf);
        for (q = 0; q < nq; q++) {
            VkBool32 present = VK_FALSE;
            if (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                continue;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, G.surface, &present);
            if (present)
                break;
        }
        if (q == nq)
            continue;
        score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (want && *want && (uint32_t)atoi(want) == i)
            score = 100;
        if (score > best_score) {
            best_score = score;
            best = i;
            G.qfamily = q;
        }
    }
    if (best == UINT32_MAX) {
        fprintf(stderr, "[VK] no Vulkan 1.3 device can present to the window\n");
        return 0;
    }
    G.phys = devs[best];
    vkGetPhysicalDeviceProperties(G.phys, &G.props);
    vkGetPhysicalDeviceMemoryProperties(G.phys, &G.memprops);
    fprintf(stderr, "[VK] device %u: %s (Vulkan %u.%u.%u)\n", best, G.props.deviceName,
            VK_API_VERSION_MAJOR(G.props.apiVersion), VK_API_VERSION_MINOR(G.props.apiVersion),
            VK_API_VERSION_PATCH(G.props.apiVersion));
    return 1;
}

static int has_device_extension(const char *name)
{
    VkExtensionProperties ext[512];
    uint32_t n = 512, i;

    if (vkEnumerateDeviceExtensionProperties(G.phys, NULL, &n, ext) < 0)
        return 0;
    for (i = 0; i < n; i++)
        if (strcmp(ext[i].extensionName, name) == 0)
            return 1;
    return 0;
}

static int create_device(void)
{
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fcb;
    VkPhysicalDeviceFeatures2 e2;
    VkPhysicalDeviceVulkan13Features e13;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT ecb;
    VkDeviceQueueCreateInfo qi;
    VkDeviceCreateInfo di;
    const char *exts[4];
    uint32_t next = 0;
    float prio = 1.0f;

    memset(&f2, 0, sizeof f2);
    memset(&f13, 0, sizeof f13);
    memset(&fcb, 0, sizeof fcb);
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    fcb.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
    f2.pNext = &f13;
    G.has_custom_border = has_device_extension(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (G.has_custom_border)
        f13.pNext = &fcb;
    vkGetPhysicalDeviceFeatures2(G.phys, &f2);
    if (!f13.dynamicRendering) {
        fprintf(stderr, "[VK] the device has no dynamic rendering\n");
        return 0;
    }
    G.has_custom_border = G.has_custom_border && fcb.customBorderColors &&
                          fcb.customBorderColorWithoutFormat;

    memset(&e2, 0, sizeof e2);
    memset(&e13, 0, sizeof e13);
    memset(&ecb, 0, sizeof ecb);
    e2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    e13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    ecb.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
    e2.pNext = &e13;
    e13.dynamicRendering = VK_TRUE;
    /* D3D11 reads out of a constant buffer's range as zero. */
    e2.features.robustBufferAccess = f2.features.robustBufferAccess;
    e2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    e2.features.depthClamp = f2.features.depthClamp;
    e2.features.fillModeNonSolid = f2.features.fillModeNonSolid;
    e2.features.depthBiasClamp = f2.features.depthBiasClamp;
    e2.features.independentBlend = f2.features.independentBlend;
    e2.features.dualSrcBlend = f2.features.dualSrcBlend;
    G.has_aniso = f2.features.samplerAnisotropy;
    G.has_depth_clamp = f2.features.depthClamp;
    G.has_fill_nonsolid = f2.features.fillModeNonSolid;
    G.has_bias_clamp = f2.features.depthBiasClamp;
    G.has_dual_src = f2.features.dualSrcBlend;

    exts[next++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (G.has_custom_border) {
        exts[next++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
        ecb.customBorderColors = VK_TRUE;
        ecb.customBorderColorWithoutFormat = VK_TRUE;
        e13.pNext = &ecb;
    }

    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = G.qfamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.pNext = &e2;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = next;
    di.ppEnabledExtensionNames = exts;
    if (vkCreateDevice(G.phys, &di, NULL, &G.dev) != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkCreateDevice failed\n");
        return 0;
    }
    vkGetDeviceQueue(G.dev, G.qfamily, 0, &G.queue);

    G.d24s8 = format_supports(VK_FORMAT_D24_UNORM_S8_UINT,
                              VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    G.has_4444 = format_supports(VK_FORMAT_A4R4G4B4_UNORM_PACK16,
                                 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    return 1;
}

static int create_binding_model(void)
{
    VkDescriptorSetLayoutBinding b[VK11_MAX_CBUFFERS * 2 + VK11_MAX_TEXTURES + VK11_MAX_SAMPLERS];
    VkDescriptorUpdateTemplateEntry te[4];
    VkDescriptorSetLayoutCreateInfo li;
    VkPipelineLayoutCreateInfo pi;
    VkDescriptorUpdateTemplateCreateInfo ti;
    uint32_t n = 0, i;

    memset(b, 0, sizeof b);
    for (i = 0; i < VK11_MAX_CBUFFERS * 2; i++, n++) {
        b[n].binding = VK11_BINDING_CBUFFER + i;
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[n].descriptorCount = 1;
        b[n].stageFlags = i < VK11_MAX_CBUFFERS ? VK_SHADER_STAGE_VERTEX_BIT
                                                : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (i = 0; i < VK11_MAX_TEXTURES; i++, n++) {
        b[n].binding = VK11_BINDING_TEXTURE + i;
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        b[n].descriptorCount = 1;
        b[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (i = 0; i < VK11_MAX_SAMPLERS; i++, n++) {
        b[n].binding = VK11_BINDING_SAMPLER + i;
        b[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        b[n].descriptorCount = 1;
        b[n].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&li, 0, sizeof li);
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = n;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(G.dev, &li, NULL, &G.set_layout) != VK_SUCCESS)
        return 0;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &G.set_layout;
    if (vkCreatePipelineLayout(G.dev, &pi, NULL, &G.pipe_layout) != VK_SUCCESS)
        return 0;

    /* The whole set in one call: buffers, then images, then samplers, laid
     * out as DescData below. */
    memset(te, 0, sizeof te);
    te[0].dstBinding = VK11_BINDING_CBUFFER;
    te[0].descriptorCount = VK11_MAX_CBUFFERS * 2;
    te[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    te[0].offset = 0;
    te[0].stride = sizeof(VkDescriptorBufferInfo);
    te[1].dstBinding = VK11_BINDING_TEXTURE;
    te[1].descriptorCount = VK11_MAX_TEXTURES;
    te[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    te[1].offset = sizeof(VkDescriptorBufferInfo) * VK11_MAX_CBUFFERS * 2;
    te[1].stride = sizeof(VkDescriptorImageInfo);
    te[2].dstBinding = VK11_BINDING_SAMPLER;
    te[2].descriptorCount = VK11_MAX_SAMPLERS;
    te[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    te[2].offset = te[1].offset + sizeof(VkDescriptorImageInfo) * VK11_MAX_TEXTURES;
    te[2].stride = sizeof(VkDescriptorImageInfo);
    memset(&ti, 0, sizeof ti);
    ti.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO;
    ti.descriptorUpdateEntryCount = 3;
    ti.pDescriptorUpdateEntries = te;
    ti.templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
    ti.descriptorSetLayout = G.set_layout;
    return vkCreateDescriptorUpdateTemplate(G.dev, &ti, NULL, &G.dtemplate) == VK_SUCCESS;
}

static int create_frames(void)
{
    int i;

    for (i = 0; i < VK11_FRAMES; i++) {
        Frame *f = &G.frames[i];
        VkCommandPoolCreateInfo pi;
        VkCommandBufferAllocateInfo ai;
        VkFenceCreateInfo fi;
        VkSemaphoreCreateInfo si;

        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = G.qfamily;
        if (vkCreateCommandPool(G.dev, &pi, NULL, &f->pool) != VK_SUCCESS)
            return 0;
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(G.dev, &ai, &f->cmd) != VK_SUCCESS)
            return 0;
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(G.dev, &fi, NULL, &f->fence) != VK_SUCCESS)
            return 0;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(G.dev, &si, NULL, &f->acquired) != VK_SUCCESS)
            return 0;
    }
    return 1;
}

static void pipeline_cache_init(void)
{
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    VkPipelineCacheCreateInfo ci;
    void *data = NULL;
    size_t size = 0;
    FILE *f;

    if (xdg && *xdg)
        snprintf(G.pcache_path, sizeof G.pcache_path, "%s/xboxrecomp", xdg);
    else if (home && *home)
        snprintf(G.pcache_path, sizeof G.pcache_path, "%s/.cache/xboxrecomp", home);
    if (G.pcache_path[0]) {
        mkdir(G.pcache_path, 0755);
        strncat(G.pcache_path, "/vk_pipeline_cache.bin",
                sizeof G.pcache_path - strlen(G.pcache_path) - 1);
        if ((f = fopen(G.pcache_path, "rb")) != NULL) {
            fseek(f, 0, SEEK_END);
            size = (size_t)ftell(f);
            fseek(f, 0, SEEK_SET);
            data = malloc(size);
            if (data && fread(data, 1, size, f) != size) {
                free(data);
                data = NULL;
                size = 0;
            }
            fclose(f);
        }
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = data ? size : 0;
    ci.pInitialData = data;
    if (vkCreatePipelineCache(G.dev, &ci, NULL, &G.pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        vkCreatePipelineCache(G.dev, &ci, NULL, &G.pcache);
    }
    free(data);
}

static void pipeline_cache_save(void)
{
    size_t size = 0;
    void *data;
    FILE *f;
    char tmp[600];

    if (!G.pcache || !G.pcache_path[0] ||
        vkGetPipelineCacheData(G.dev, G.pcache, &size, NULL) != VK_SUCCESS || !size)
        return;
    data = malloc(size);
    if (!data)
        return;
    if (vkGetPipelineCacheData(G.dev, G.pcache, &size, data) == VK_SUCCESS) {
        snprintf(tmp, sizeof tmp, "%s.%d.tmp", G.pcache_path, (int)getpid());
        if ((f = fopen(tmp, "wb")) != NULL) {
            if (fwrite(data, 1, size, f) == size) {
                fclose(f);
                rename(tmp, G.pcache_path);
            } else {
                fclose(f);
                remove(tmp);
            }
        }
    }
    free(data);
}

/* ---- the swap chain ------------------------------------------------------ */

static VkPresentModeKHR choose_present_mode(int interval)
{
    VkPresentModeKHR modes[16];
    uint32_t n = 16, i;
    static const VkPresentModeKHR no_wait[] = {
        VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
        VK_PRESENT_MODE_FIFO_RELAXED_KHR
    };
    size_t k;

    if (interval > 0)
        return VK_PRESENT_MODE_FIFO_KHR;
    /* A shadow device must not block on the display: the title paces its
     * own loop (docs/technical/vulkan-backend.md 4.10). */
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(G.phys, G.surface, &n, modes) < 0)
        return VK_PRESENT_MODE_FIFO_KHR;
    for (k = 0; k < sizeof no_wait / sizeof no_wait[0]; k++)
        for (i = 0; i < n; i++)
            if (modes[i] == no_wait[k])
                return modes[i];
    fprintf(stderr, "[VK] only FIFO presentation is available; Present will wait for vblank\n");
    return VK_PRESENT_MODE_FIFO_KHR;
}

static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[64];
    uint32_t nf = 64, i;
    VkSwapchainCreateInfoKHR ci;
    VkSwapchainKHR old = G.swapchain;
    VkExtent2D ext;
    VkResult r;

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(G.phys, G.surface, &caps) != VK_SUCCESS)
        return 0;
    ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext = window_extent();
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
        if (ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
        if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    }
    G.swap_extent = ext;
    G.swap_dirty = 0;
    if (ext.width == 0 || ext.height == 0)
        return 0;                       /* minimised: try again next frame */

    G.swap_format = VK_FORMAT_UNDEFINED;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(G.phys, G.surface, &nf, formats) >= 0) {
        for (i = 0; i < nf && G.swap_format == VK_FORMAT_UNDEFINED; i++)
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
                formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)
                G.swap_format = formats[i].format;
        if (G.swap_format == VK_FORMAT_UNDEFINED && nf)
            G.swap_format = formats[0].format;
    }
    G.present_mode = choose_present_mode(G.present_interval);

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = G.surface;
    ci.minImageCount = caps.minImageCount + 1 < 3 ? 3 : caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = G.swap_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                    ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha &
                                                      (~caps.supportedCompositeAlpha + 1));
    ci.presentMode = G.present_mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    r = vkCreateSwapchainKHR(G.dev, &ci, NULL, &G.swapchain);
    if (old)
        vkDestroySwapchainKHR(G.dev, old, NULL);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkCreateSwapchainKHR failed (%d)\n", (int)r);
        G.swapchain = VK_NULL_HANDLE;
        return 0;
    }
    G.swap_count = 8;
    vkGetSwapchainImagesKHR(G.dev, G.swapchain, &G.swap_count, G.swap_images);
    for (i = 0; i < G.swap_count; i++) {
        if (!G.swap_done[i]) {
            VkSemaphoreCreateInfo si;
            memset(&si, 0, sizeof si);
            si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(G.dev, &si, NULL, &G.swap_done[i]);
        }
    }
    return 1;
}

static void recreate_swapchain(void)
{
    vkDeviceWaitIdle(G.dev);
    create_swapchain();
}

/* ======================================================================
 * Images
 * ====================================================================== */

static uint32_t mip_dim(uint32_t d, uint32_t mip)
{
    d >>= mip;
    return d ? d : 1;
}

static uint32_t upload_align(const Fmt *f)
{
    /* bufferOffset must be a multiple of the element size and of 4. */
    if (f->bpe == 12)
        return 48;
    return f->bpe < 16 ? 16 : f->bpe;
}

static void sub_split(const Image *im, UINT sub, uint32_t *mip, uint32_t *layer)
{
    *mip = im->mips ? sub % im->mips : 0;
    *layer = im->mips ? sub / im->mips : 0;
}

/* Copies one subresource (or a box of it) from host memory into the image,
 * through the arena. Pitches are the caller's; the copy is tight. */
static void image_upload(Image *im, UINT sub, const D3D11_BOX *box,
                         const void *data, UINT row_pitch, UINT depth_pitch)
{
    uint32_t mip, layer, x = 0, y = 0, z = 0, w, h, d, rows, rowbytes, r, s;
    VkBuffer buf;
    VkDeviceSize off;
    uint8_t *dst;
    VkBufferImageCopy c;
    const uint8_t *src = (const uint8_t *)data;

    if (!im->img || !data || (im->f.aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        return;                         /* no CPU writes of packed depth+stencil */
    sub_split(im, sub, &mip, &layer);
    w = mip_dim(im->w, mip);
    h = mip_dim(im->h, mip);
    d = im->is3d ? mip_dim(im->depth, mip) : 1;
    if (box) {
        if (box->right <= box->left || box->bottom <= box->top || box->back <= box->front)
            return;
        x = box->left; y = box->top; z = box->front;
        w = box->right - box->left;
        h = box->bottom - box->top;
        d = box->back - box->front;
    }
    rows = (h + im->f.block - 1) / im->f.block;
    rowbytes = (w + im->f.block - 1) / im->f.block * im->f.bpe;
    if (!row_pitch)
        row_pitch = rowbytes;
    if (!depth_pitch)
        depth_pitch = row_pitch * rows;
    dst = arena_alloc((VkDeviceSize)rowbytes * rows * d, upload_align(&im->f), &buf, &off);
    if (!dst)
        return;
    for (s = 0; s < d; s++)
        for (r = 0; r < rows; r++)
            memcpy(dst + ((size_t)s * rows + r) * rowbytes,
                   src + (size_t)s * depth_pitch + (size_t)r * row_pitch, rowbytes);
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = im->f.aspect;
    c.imageSubresource.mipLevel = mip;
    c.imageSubresource.baseArrayLayer = im->is3d ? 0 : layer;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = (int32_t)x;
    c.imageOffset.y = (int32_t)y;
    c.imageOffset.z = (int32_t)z;
    /* A block-compressed copy may stop short of a block at the mip's edge. */
    c.imageExtent.width = w;
    c.imageExtent.height = h;
    c.imageExtent.depth = d;
    begin_transfer();
    vkCmdCopyBufferToImage(G.cmd, buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    end_transfer();
}

static Image *image_new(int is3d, const D3D11_TEXTURE2D_DESC *d2, const D3D11_TEXTURE3D_DESC *d3,
                        const D3D11_SUBRESOURCE_DATA *init)
{
    Image *im = (Image *)obj_new(sizeof(Image), K_TEXTURE);
    VkImageCreateInfo ci;
    VkMemoryRequirements req;
    VkFormatProperties fp;
    DXGI_FORMAT dxgi;
    UINT usage, bind, misc, i, n;

    if (!im)
        return NULL;
    im->is3d = is3d;
    if (is3d) {
        im->d3 = *d3;
        dxgi = d3->Format; usage = d3->Usage; bind = d3->BindFlags; misc = d3->MiscFlags;
        im->w = d3->Width; im->h = d3->Height; im->depth = d3->Depth ? d3->Depth : 1;
        im->layers = 1;
        im->mips = d3->MipLevels;
    } else {
        im->d2 = *d2;
        dxgi = d2->Format; usage = d2->Usage; bind = d2->BindFlags; misc = d2->MiscFlags;
        im->w = d2->Width; im->h = d2->Height; im->depth = 1;
        im->layers = d2->ArraySize ? d2->ArraySize : 1;
        im->mips = d2->MipLevels;
        im->samples = d2->SampleDesc.Count > 1 ? d2->SampleDesc.Count : 1;
        if (im->samples > 1)
            im->mips = im->d2.MipLevels = 1;
    }
    if (!im->samples)
        im->samples = 1;
    if (!im->w) im->w = 1;
    if (!im->h) im->h = 1;
    if (!im->mips) {                    /* 0 = the full chain */
        uint32_t m = im->w > im->h ? im->w : im->h;
        if (im->depth > m) m = im->depth;
        for (im->mips = 1; m > 1; m >>= 1)
            im->mips++;
        if (is3d) im->d3.MipLevels = im->mips; else im->d2.MipLevels = im->mips;
    }
    im->f = fmt_info(dxgi);
    if (im->f.vk == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "[VK] texture format %d has no Vulkan equivalent\n", (int)dxgi);
        free(im);
        return NULL;
    }

    if (usage == D3D11_USAGE_STAGING) {
        uint32_t rows = (im->h + im->f.block - 1) / im->f.block;
        im->srow = (im->w + im->f.block - 1) / im->f.block * im->f.bpe;
        im->sslice = im->srow * rows;
        if (!make_buffer((VkDeviceSize)im->sslice * im->depth,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         HOST_MEM | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                         &im->sbuf, &im->smem, &im->sptr) &&
            !make_buffer((VkDeviceSize)im->sslice * im->depth,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         HOST_MEM, &im->sbuf, &im->smem, &im->sptr)) {
            free(im);
            return NULL;
        }
        memset(im->sptr, 0, (size_t)im->sslice * im->depth);
        if (init && init->pSysMem)
            for (i = 0; i < (im->h + im->f.block - 1) / im->f.block * im->depth; i++)
                memcpy(im->sptr + (size_t)i * im->srow,
                       (const uint8_t *)init->pSysMem + (size_t)i * init->SysMemPitch, im->srow);
        return im;
    }

    vkGetPhysicalDeviceFormatProperties(G.phys, im->f.vk, &fp);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = im->f.vk;
    ci.extent.width = im->w;
    ci.extent.height = im->h;
    ci.extent.depth = im->depth;
    ci.mipLevels = im->mips;
    ci.arrayLayers = im->layers;
    ci.samples = (VkSampleCountFlagBits)im->samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (im->samples == 1 && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((bind & D3D11_BIND_RENDER_TARGET) &&
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((im->f.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) &&
        (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((bind & D3D11_BIND_RENDER_TARGET) && !(ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        fprintf(stderr, "[VK] format %d cannot be rendered to here\n", (int)dxgi);
    if ((misc & D3D11_RESOURCE_MISC_TEXTURECUBE) && !is3d && im->layers % 6 == 0 &&
        im->w == im->h)
        ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(G.dev, &ci, NULL, &im->img) != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkCreateImage(%ux%ux%u fmt %d) failed\n", im->w, im->h,
                im->depth, (int)dxgi);
        free(im);
        return NULL;
    }
    vkGetImageMemoryRequirements(G.dev, im->img, &req);
    im->mem = alloc_memory(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!im->mem) {
        vkDestroyImage(G.dev, im->img, NULL);
        free(im);
        return NULL;
    }
    vkBindImageMemory(G.dev, im->img, im->mem, 0);

    /* Into GENERAL for good, and defined: a D3D11 texture nobody wrote reads
     * as something stable, and so should this one. */
    begin_transfer();
    image_layout(im->img, im->f.aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    {
        VkImageSubresourceRange rr;
        memset(&rr, 0, sizeof rr);
        rr.aspectMask = im->f.aspect;
        rr.levelCount = VK_REMAINING_MIP_LEVELS;
        rr.layerCount = VK_REMAINING_ARRAY_LAYERS;
        if (im->f.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
            VkClearDepthStencilValue v = { 1.0f, 0 };
            vkCmdClearDepthStencilImage(G.cmd, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rr);
        } else if (im->f.block == 1 && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
            VkClearColorValue v;
            memset(&v, 0, sizeof v);
            vkCmdClearColorImage(G.cmd, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rr);
        }
    }
    end_transfer();

    if (init) {
        n = im->mips * im->layers;
        for (i = 0; i < n; i++)
            if (init[i].pSysMem)
                image_upload(im, i, NULL, init[i].pSysMem, init[i].SysMemPitch,
                             init[i].SysMemSlicePitch);
    }
    return im;
}

HRESULT vk11_dev_CreateTexture2D(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d,
                                 const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out)
{
    Image *im;

    (void)dev;
    if (!d || !out)
        return E_INVALIDARG;
    LOCK();
    im = image_new(0, d, NULL, init);
    UNLOCK();
    *out = (ID3D11Texture2D *)im;
    return im ? S_OK : E_INVALIDARG;
}

HRESULT vk11_dev_CreateTexture3D(ID3D11Device *dev, const D3D11_TEXTURE3D_DESC *d,
                                 const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture3D **out)
{
    Image *im;

    (void)dev;
    if (!d || !out)
        return E_INVALIDARG;
    LOCK();
    im = image_new(1, NULL, d, init);
    UNLOCK();
    *out = (ID3D11Texture3D *)im;
    return im ? S_OK : E_INVALIDARG;
}

void vk11_tex2d_GetDesc(ID3D11Texture2D *t, D3D11_TEXTURE2D_DESC *d)
{
    const Image *im = (const Image *)t;
    if (im && d)
        *d = im->d2;
}

HRESULT vk11_dev_CheckMultisampleQualityLevels(ID3D11Device *dev, DXGI_FORMAT f, UINT count,
                                               UINT *levels)
{
    /* What the device can render into at this count; render targets and
     * depth buffers only (Image.samples). */
    Fmt fi = fmt_info(f);
    VkSampleCountFlags ok;

    (void)dev;
    if (!levels)
        return E_INVALIDARG;
    *levels = 0;
    if (count == 1) {
        *levels = 1;
        return S_OK;
    }
    if (fi.vk == VK_FORMAT_UNDEFINED || count > 64 || (count & (count - 1)))
        return S_OK;
    ok = (fi.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
        ? G.props.limits.framebufferDepthSampleCounts & G.props.limits.framebufferStencilSampleCounts
        : G.props.limits.framebufferColorSampleCounts;
    if (ok & count)
        *levels = 1;
    return S_OK;
}

/* ======================================================================
 * Views
 * ====================================================================== */

static VkImageView make_view(Image *im, VkImageViewType type, VkFormat fmt,
                             VkImageAspectFlags aspect, VkComponentMapping swz,
                             uint32_t mip, uint32_t mips, uint32_t layer, uint32_t layers)
{
    VkImageViewCreateInfo vi;
    VkImageView v = VK_NULL_HANDLE;

    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = im->img;
    vi.viewType = type;
    vi.format = fmt;
    vi.components = swz;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.baseMipLevel = mip;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.baseArrayLayer = layer;
    vi.subresourceRange.layerCount = layers;
    if (vkCreateImageView(G.dev, &vi, NULL, &v) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return v;
}

static View *view_new(uint32_t kind, Image *im)
{
    View *v = (View *)obj_new(sizeof(View), kind);
    if (!v)
        return NULL;
    v->res = im;
    vk11_add_ref(im);
    v->fmt = im->f.vk;
    return v;
}

HRESULT vk11_dev_CreateShaderResourceView(ID3D11Device *dev, ID3D11Resource *res,
                                          const D3D11_SHADER_RESOURCE_VIEW_DESC *d,
                                          ID3D11ShaderResourceView **out)
{
    Image *im = (Image *)res;
    View *v;
    VkImageViewType type;
    VkImageAspectFlags aspect;
    uint32_t mip = 0, mips, layer = 0, layers;

    (void)dev;
    if (!im || im->o.kind != K_TEXTURE || !im->img || !out)
        return E_INVALIDARG;
    if (im->samples > 1)
        return E_INVALIDARG;            /* resolved first; never sampled as is */
    LOCK();
    mips = im->mips;
    layers = im->layers;
    if (im->is3d) {
        type = VK_IMAGE_VIEW_TYPE_3D;
    } else if (d && d->ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE) {
        type = VK_IMAGE_VIEW_TYPE_CUBE;
        layers = 6;
    } else if (layers > 1 || (d && d->ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)) {
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    } else {
        type = VK_IMAGE_VIEW_TYPE_2D;
    }
    if (d) {
        switch (d->ViewDimension) {
        case D3D11_SRV_DIMENSION_TEXTURE2D:
            mip = d->Texture2D.MostDetailedMip; mips = d->Texture2D.MipLevels; break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
            mip = d->Texture2DArray.MostDetailedMip; mips = d->Texture2DArray.MipLevels;
            layer = d->Texture2DArray.FirstArraySlice; layers = d->Texture2DArray.ArraySize; break;
        case D3D11_SRV_DIMENSION_TEXTURE3D:
            mip = d->Texture3D.MostDetailedMip; mips = d->Texture3D.MipLevels; break;
        case D3D11_SRV_DIMENSION_TEXTURECUBE:
            mip = d->TextureCube.MostDetailedMip; mips = d->TextureCube.MipLevels; break;
        default:
            break;
        }
    }
    if (mip >= im->mips)
        mip = im->mips - 1;
    if (mips == 0 || mips == (uint32_t)-1 || mip + mips > im->mips)
        mips = im->mips - mip;
    if (layer + layers > im->layers)
        layers = im->layers - layer;
    /* A depth texture samples its depth. */
    aspect = (im->f.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                        : VK_IMAGE_ASPECT_COLOR_BIT;
    v = view_new(K_SRV, im);
    if (v) {
        v->view = make_view(im, type, im->f.vk, aspect, im->f.swz, mip, mips, layer, layers);
        v->type = type;
        v->mip = mip; v->mip_count = mips; v->layer = layer; v->layer_count = layers;
        v->w = mip_dim(im->w, mip);
        v->h = mip_dim(im->h, mip);
        if (!v->view) {
            vk11_release(v);
            v = NULL;
        }
    }
    UNLOCK();
    *out = (ID3D11ShaderResourceView *)v;
    return v ? S_OK : E_INVALIDARG;
}

static View *target_view(uint32_t kind, Image *im, uint32_t mip, uint32_t layer)
{
    View *v;
    VkComponentMapping id = SWZ_ID;
    VkImageAspectFlags aspect = kind == K_DSV ? (im->f.aspect & (VK_IMAGE_ASPECT_DEPTH_BIT |
                                                                VK_IMAGE_ASPECT_STENCIL_BIT))
                                              : VK_IMAGE_ASPECT_COLOR_BIT;

    if (mip >= im->mips || layer >= im->layers)
        return NULL;
    v = view_new(kind, im);
    if (!v)
        return NULL;
    v->view = make_view(im, VK_IMAGE_VIEW_TYPE_2D, im->f.vk, aspect, id, mip, 1, layer, 1);
    v->mip = mip; v->mip_count = 1; v->layer = layer; v->layer_count = 1;
    v->w = mip_dim(im->w, mip);
    v->h = mip_dim(im->h, mip);
    if (!v->view) {
        vk11_release(v);
        return NULL;
    }
    return v;
}

HRESULT vk11_dev_CreateRenderTargetView(ID3D11Device *dev, ID3D11Resource *res,
                                        const D3D11_RENDER_TARGET_VIEW_DESC *d,
                                        ID3D11RenderTargetView **out)
{
    Image *im = (Image *)res;
    uint32_t mip = 0, layer = 0;
    View *v;

    (void)dev;
    if (!im || im->o.kind != K_TEXTURE || !im->img || !out)
        return E_INVALIDARG;
    if (d && d->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
        mip = d->Texture2D.MipSlice;
    else if (d && d->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
        mip = d->Texture2DArray.MipSlice;
        layer = d->Texture2DArray.FirstArraySlice;
    }
    LOCK();
    v = target_view(K_RTV, im, mip, layer);
    UNLOCK();
    *out = (ID3D11RenderTargetView *)v;
    return v ? S_OK : E_INVALIDARG;
}

HRESULT vk11_dev_CreateDepthStencilView(ID3D11Device *dev, ID3D11Resource *res,
                                        const D3D11_DEPTH_STENCIL_VIEW_DESC *d,
                                        ID3D11DepthStencilView **out)
{
    Image *im = (Image *)res;
    uint32_t mip = 0, layer = 0;
    View *v;

    (void)dev;
    if (!im || im->o.kind != K_TEXTURE || !im->img || !out ||
        !(im->f.aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        return E_INVALIDARG;
    if (d && d->ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D)
        mip = d->Texture2D.MipSlice;
    else if (d && d->ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
        mip = d->Texture2DArray.MipSlice;
        layer = d->Texture2DArray.FirstArraySlice;
    }
    LOCK();
    v = target_view(K_DSV, im, mip, layer);
    UNLOCK();
    *out = (ID3D11DepthStencilView *)v;
    return v ? S_OK : E_INVALIDARG;
}

void vk11_view_GetResource(void *view, ID3D11Resource **out)
{
    View *v = (View *)view;
    if (!out)
        return;
    *out = v ? (ID3D11Resource *)v->res : NULL;
    if (*out)
        vk11_add_ref(*out);
}

/* ======================================================================
 * Buffers
 * ====================================================================== */

HRESULT vk11_dev_CreateBuffer(ID3D11Device *dev, const D3D11_BUFFER_DESC *d,
                              const D3D11_SUBRESOURCE_DATA *init, ID3D11Buffer **out)
{
    Buffer *b;
    HRESULT hr = S_OK;

    (void)dev;
    if (!d || !out || !d->ByteWidth)
        return E_INVALIDARG;
    LOCK();
    b = (Buffer *)obj_new(sizeof(Buffer), K_BUFFER);
    if (!b) {
        UNLOCK();
        return E_OUTOFMEMORY;
    }
    b->desc = *d;
    b->cur_slot = -2;
    b->cur_ver = -1;
    switch (d->Usage) {
    case D3D11_USAGE_DYNAMIC:
        b->dyn_next = G.dyn_head;
        if (G.dyn_head)
            G.dyn_head->dyn_prev = b;
        G.dyn_head = b;
        if (init && init->pSysMem) {
            if (make_buffer(d->ByteWidth, ALL_BUFFER_USAGE, HOST_MEM,
                            &b->home_buf, &b->home_mem, &b->home_ptr)) {
                memcpy(b->home_ptr, init->pSysMem, d->ByteWidth);
                b->cur_buf = b->home_buf;
                b->cur_ptr = b->home_ptr;
                b->cur_off = 0;
                b->cur_slot = -1;
            }
        }
        break;
    case D3D11_USAGE_STAGING:
        if (!make_buffer(d->ByteWidth, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT, HOST_MEM, &b->buf, &b->mem, &b->host))
            hr = E_OUTOFMEMORY;
        else if (init && init->pSysMem)
            memcpy(b->host, init->pSysMem, d->ByteWidth);
        break;
    default:
        if (!make_buffer(d->ByteWidth, ALL_BUFFER_USAGE, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         &b->buf, &b->mem, NULL)) {
            hr = E_OUTOFMEMORY;
        } else if (init && init->pSysMem) {
            VkBuffer src;
            VkDeviceSize off;
            uint8_t *p = arena_alloc(d->ByteWidth, 16, &src, &off);
            if (p) {
                VkBufferCopy c;
                memcpy(p, init->pSysMem, d->ByteWidth);
                c.srcOffset = off;
                c.dstOffset = 0;
                c.size = d->ByteWidth;
                begin_transfer();
                vkCmdCopyBuffer(G.cmd, src, b->buf, 1, &c);
                end_transfer();
            }
        }
        break;
    }
    if (FAILED(hr)) {
        UNLOCK();
        vk11_release(b);
        *out = NULL;
        return hr;
    }
    UNLOCK();
    *out = (ID3D11Buffer *)b;
    return S_OK;
}

/* Where a buffer's bytes are for the GPU right now. */
static int buffer_binding(Buffer *b, VkBuffer *buf, VkDeviceSize *off)
{
    if (!b)
        return 0;
    if (b->desc.Usage == D3D11_USAGE_DYNAMIC) {
        if (b->cur_slot == -2) {
            if (!b->home_buf &&
                !make_buffer(b->desc.ByteWidth, ALL_BUFFER_USAGE, HOST_MEM,
                             &b->home_buf, &b->home_mem, &b->home_ptr))
                return 0;
            memset(b->home_ptr, 0, b->desc.ByteWidth);
            b->cur_buf = b->home_buf;
            b->cur_ptr = b->home_ptr;
            b->cur_off = 0;
            b->cur_slot = -1;
        }
        *buf = b->cur_buf;
        *off = b->cur_off;
        return 1;
    }
    if (!b->buf)
        return 0;
    *buf = b->buf;
    *off = 0;
    return 1;
}

/* ======================================================================
 * Object lifetime
 * ====================================================================== */

static void pipelines_forget(uint64_t serial);

ULONG vk11_add_ref(void *object)
{
    Obj *o = (Obj *)object;
    if (!o)
        return 0;
    return (ULONG)__atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
}

static void destroy(Obj *o)
{
    switch (o->kind) {
    case K_BUFFER: {
        Buffer *b = (Buffer *)o;
        int i;
        if (b->desc.Usage == D3D11_USAGE_DYNAMIC) {
            if (b->dyn_prev) b->dyn_prev->dyn_next = b->dyn_next;
            else G.dyn_head = b->dyn_next;
            if (b->dyn_next) b->dyn_next->dyn_prev = b->dyn_prev;
        }
        garbage_add(G_BUFFER, (uint64_t)b->buf);
        garbage_add(G_MEMORY, (uint64_t)b->mem);
        garbage_add(G_BUFFER, (uint64_t)b->home_buf);
        garbage_add(G_MEMORY, (uint64_t)b->home_mem);
        for (i = 0; i < b->nver; i++) {
            garbage_add(G_BUFFER, (uint64_t)b->ver[i].buf);
            garbage_add(G_MEMORY, (uint64_t)b->ver[i].mem);
        }
        break;
    }
    case K_TEXTURE: {
        Image *im = (Image *)o;
        garbage_add(G_IMAGE, (uint64_t)im->img);
        garbage_add(G_MEMORY, (uint64_t)im->mem);
        garbage_add(G_BUFFER, (uint64_t)im->sbuf);
        garbage_add(G_MEMORY, (uint64_t)im->smem);
        break;
    }
    case K_SRV: case K_RTV: case K_DSV: {
        View *v = (View *)o;
        garbage_add(G_VIEW, (uint64_t)v->view);
        vk11_release(v->res);
        break;
    }
    case K_VS: case K_PS: {
        Shader *s = (Shader *)o;
        int i;
        garbage_add(G_MODULE, (uint64_t)s->module);
        for (i = 0; i < s->nvar; i++)
            garbage_add(G_MODULE, (uint64_t)s->var[i].module);
        free(s->var);
        free(s->spv);
        pipelines_forget(o->serial);
        break;
    }
    case K_LAYOUT:
        pipelines_forget(o->serial);
        break;
    case K_DEVICE: case K_CONTEXT: case K_SWAPCHAIN:
        return;                         /* static; lives as long as the process */
    default:
        break;
    }
    free(o);
}

ULONG vk11_release(void *object)
{
    Obj *o = (Obj *)object;
    int32_t left;

    if (!o)
        return 0;
    left = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
    if (left == 0) {
        LOCK();
        destroy(o);
        UNLOCK();
    }
    return left > 0 ? (ULONG)left : 0;
}

/* Bound-object bookkeeping: the context references what it holds. */
#define REBIND(slot, obj) do { void *n_ = (void *)(obj); void *o_ = (void *)(slot); \
    if (n_ != o_) { if (n_) vk11_add_ref(n_); (slot) = (obj); if (o_) vk11_release(o_); } } while (0)

/* ======================================================================
 * Blobs
 * ====================================================================== */

ID3D10Blob *vk11_blob_create(const void *data, SIZE_T size)
{
    ID3D10Blob *b = (ID3D10Blob *)calloc(1, sizeof(ID3D10Blob) + size + 4);
    if (!b)
        return NULL;
    b->o.kind = K_BLOB;
    b->o.refs = 1;
    b->size = size;
    if (data)
        memcpy(b->data, data, size);
    return b;
}

void *vk11_blob_pointer(ID3D10Blob *b) { return b ? b->data : NULL; }
SIZE_T vk11_blob_size(ID3D10Blob *b)   { return b ? b->size : 0; }

/* ======================================================================
 * Shaders
 * ====================================================================== */

/* "TEXCOORD3" -> "TEXCOORD#3", "POSITION" -> "POSITION#0": how D3D11
 * compares semantics (case-insensitive name, separate index). */
static void canon_semantic(const char *name, UINT index, int has_index, char *out, size_t n)
{
    char base[32];
    size_t len = 0, i;

    for (i = 0; name[i] && len < sizeof base - 1; i++)
        base[len++] = (char)((name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i]);
    base[len] = '\0';
    if (!has_index) {
        /* The index is the trailing digits, if any. */
        size_t d = len;
        while (d > 0 && base[d - 1] >= '0' && base[d - 1] <= '9')
            d--;
        index = d < len ? (UINT)atoi(base + d) : 0;
        base[d] = '\0';
    }
    snprintf(out, n, "%s#%u", base, index);
}

/* Reads a shader's interface: every Input/Output variable with a Location
 * and a UserSemantic (built-ins have neither). */
static void reflect(Shader *s)
{
    enum { MAX_IDS = 1 << 16 };
    const uint32_t *w = s->spv;
    size_t i = 5;
    uint32_t bound = s->words > 3 ? w[3] : 0;
    uint32_t *loc_word, *storage, *binding, *ptr_to, *img_dim, *var_type;
    const char **sem;

    s->n_in = s->n_out = 0;
    if (s->words < 5 || w[0] != 0x07230203u || bound == 0 || bound > (1u << 22))
        return;
    memset(s->tex_dim, 0, sizeof s->tex_dim);
    loc_word = (uint32_t *)calloc(bound, sizeof *loc_word);
    storage = (uint32_t *)calloc(bound, sizeof *storage);
    sem = (const char **)calloc(bound, sizeof *sem);
    binding = (uint32_t *)calloc(bound, sizeof *binding);
    ptr_to = (uint32_t *)calloc(bound, sizeof *ptr_to);
    img_dim = (uint32_t *)calloc(bound, sizeof *img_dim);
    var_type = (uint32_t *)calloc(bound, sizeof *var_type);
    if (!loc_word || !storage || !sem || !binding || !ptr_to || !img_dim || !var_type)
        goto out;
    for (storage[0] = 0; i < s->words;) {
        uint32_t op = w[i] & 0xFFFFu, wc = w[i] >> 16;
        if (wc == 0 || i + wc > s->words)
            break;
        if (op == 15 && wc >= 4 && !s->entry[0])                             /* OpEntryPoint */
            snprintf(s->entry, sizeof s->entry, "%s", (const char *)&w[i + 3]);
        else if (op == 71 && wc >= 4 && w[i + 1] < bound && w[i + 2] == 30)   /* Location */
            loc_word[w[i + 1]] = (uint32_t)(i + 3);
        else if (op == 5632 && wc >= 4 && w[i + 1] < bound &&
                 (w[i + 2] == 5635))                                          /* UserSemantic */
            sem[w[i + 1]] = (const char *)&w[i + 3];
        else if (op == 71 && wc >= 4 && w[i + 1] < bound && w[i + 2] == 33)   /* Binding */
            binding[w[i + 1]] = w[i + 3] + 1;
        else if (op == 25 && wc >= 4 && w[i + 1] < bound)                    /* OpTypeImage */
            img_dim[w[i + 1]] = w[i + 3] + 1;
        else if (op == 32 && wc >= 4 && w[i + 1] < bound)                    /* OpTypePointer */
            ptr_to[w[i + 1]] = w[i + 3];
        else if (op == 59 && wc >= 4 && w[i + 2] < bound) {                  /* OpVariable */
            storage[w[i + 2]] = w[i + 3] + 1;
            var_type[w[i + 2]] = w[i + 1];
        }
        i += wc;
    }
    for (i = 0; i < bound; i++) {
        uint32_t t = var_type[i] < bound ? ptr_to[var_type[i]] : 0;
        if (binding[i] && t && t < bound && img_dim[t]) {
            uint32_t b = binding[i] - 1;
            if (b >= VK11_BINDING_TEXTURE && b < VK11_BINDING_TEXTURE + VK11_MAX_TEXTURES)
                s->tex_dim[b - VK11_BINDING_TEXTURE] = (uint8_t)img_dim[t];
        }
    }
    for (i = 0; i < bound; i++) {
        IoVar *v;
        if (!loc_word[i] || !sem[i])
            continue;
        if (storage[i] == 1 + 1 && s->n_in < MAX_IO)            /* Input */
            v = &s->in[s->n_in++];
        else if (storage[i] == 3 + 1 && s->n_out < MAX_IO)      /* Output */
            v = &s->out[s->n_out++];
        else
            continue;
        canon_semantic(sem[i], 0, 0, v->sem, sizeof v->sem);
        v->loc_word = loc_word[i];
        v->loc = w[loc_word[i]];
    }
out:
    free(loc_word);
    free(storage);
    free(sem);
    free(binding);
    free(ptr_to);
    free(img_dim);
    free(var_type);
}

/* The HLSL semantics were only for reflect(): drop the SPV_GOOGLE_*
 * extensions and the decorations they carry, so the module is plain Vulkan
 * SPIR-V that needs no VK_GOOGLE_hlsl_functionality1 on the device. The
 * Location words move with the code, so loc_word is renumbered. */
static void strip_semantics(Shader *s)
{
    uint32_t *out = (uint32_t *)malloc(s->words * 4);
    size_t i = 5, n = 5;
    int k;

    if (!out || s->words < 5)
        goto done;
    memcpy(out, s->spv, 5 * 4);
    while (i < s->words) {
        uint32_t op = s->spv[i] & 0xFFFFu, wc = s->spv[i] >> 16;
        int drop = 0;
        if (wc == 0 || i + wc > s->words)
            break;
        if (op == 5632 || op == 5633)                   /* OpDecorate(Member)String */
            drop = 1;
        else if (op == 10 && wc >= 2 &&                 /* OpExtension SPV_GOOGLE_* */
                 strncmp((const char *)&s->spv[i + 1], "SPV_GOOGLE_", 11) == 0)
            drop = 1;
        if (!drop) {
            for (k = 0; k < s->n_in; k++)
                if (s->in[k].loc_word >= i && s->in[k].loc_word < i + wc)
                    s->in[k].loc_word = (uint32_t)(n + (s->in[k].loc_word - i));
            for (k = 0; k < s->n_out; k++)
                if (s->out[k].loc_word >= i && s->out[k].loc_word < i + wc)
                    s->out[k].loc_word = (uint32_t)(n + (s->out[k].loc_word - i));
            memmove(out + n, s->spv + i, wc * 4);
            n += wc;
        }
        i += wc;
    }
    free(s->spv);
    s->spv = out;
    s->words = n;
    return;
done:
    free(out);
}

static VkShaderModule make_module(const uint32_t *code, size_t words)
{
    VkShaderModuleCreateInfo ci;
    VkShaderModule m = VK_NULL_HANDLE;

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = words * 4;
    ci.pCode = code;
    if (vkCreateShaderModule(G.dev, &ci, NULL, &m) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return m;
}

static HRESULT shader_new(uint32_t kind, const void *code, SIZE_T len, void **out)
{
    Shader *s;

    if (!code || len < 20 || (len & 3) || ((const uint32_t *)code)[0] != 0x07230203u || !out)
        return E_INVALIDARG;
    s = (Shader *)obj_new(sizeof(Shader), kind);
    if (!s)
        return E_OUTOFMEMORY;
    s->stage = kind == K_PS;
    s->words = len / 4;
    s->spv = (uint32_t *)malloc(len);
    if (!s->spv) {
        free(s);
        return E_OUTOFMEMORY;
    }
    memcpy(s->spv, code, len);
    reflect(s);
    strip_semantics(s);
    if (!s->entry[0])
        snprintf(s->entry, sizeof s->entry, "main");
    LOCK();
    s->module = make_module(s->spv, s->words);
    UNLOCK();
    if (!s->module) {
        free(s->spv);
        free(s);
        return E_INVALIDARG;
    }
    *out = s;
    return S_OK;
}

HRESULT vk11_dev_CreateVertexShader(ID3D11Device *dev, const void *code, SIZE_T len,
                                    ID3D11ClassLinkage *link, ID3D11VertexShader **out)
{
    (void)dev; (void)link;
    return shader_new(K_VS, code, len, (void **)out);
}

HRESULT vk11_dev_CreatePixelShader(ID3D11Device *dev, const void *code, SIZE_T len,
                                   ID3D11ClassLinkage *link, ID3D11PixelShader **out)
{
    (void)dev; (void)link;
    return shader_new(K_PS, code, len, (void **)out);
}

/* The pixel shader with its inputs numbered as this vertex shader's
 * outputs, matched by semantic. An input the vertex shader does not write
 * reads an unused location: undefined, as it is in D3D11. */
static VkShaderModule ps_for_vs(Shader *ps, Shader *vs)
{
    uint32_t *code;
    int i, j, spare = 30;
    VkShaderModule m;

    for (i = 0; i < ps->nvar; i++)
        if (ps->var[i].vs_serial == vs->o.serial)
            return ps->var[i].module;
    code = (uint32_t *)malloc(ps->words * 4);
    if (!code)
        return ps->module;
    memcpy(code, ps->spv, ps->words * 4);
    for (i = 0; i < ps->n_in; i++) {
        uint32_t loc = (uint32_t)spare;
        for (j = 0; j < vs->n_out; j++)
            if (strcmp(vs->out[j].sem, ps->in[i].sem) == 0) {
                loc = vs->out[j].loc;
                break;
            }
        if (j == vs->n_out && spare > 0)
            spare--;
        code[ps->in[i].loc_word] = loc;
    }
    m = make_module(code, ps->words);
    free(code);
    if (!m)
        return ps->module;
    if (ps->nvar == ps->capvar) {
        int cap = ps->capvar ? ps->capvar * 2 : 4;
        PsVariant *v = (PsVariant *)realloc(ps->var, sizeof *v * (size_t)cap);
        if (!v) {
            garbage_add(G_MODULE, (uint64_t)m);
            return ps->module;
        }
        ps->var = v;
        ps->capvar = cap;
    }
    ps->var[ps->nvar].vs_serial = vs->o.serial;
    ps->var[ps->nvar].module = m;
    ps->nvar++;
    return m;
}

/* ======================================================================
 * Input layouts and state objects
 * ====================================================================== */

HRESULT vk11_dev_CreateInputLayout(ID3D11Device *dev, const D3D11_INPUT_ELEMENT_DESC *e, UINT n,
                                   const void *code, SIZE_T len, ID3D11InputLayout **out)
{
    Layout *l;
    uint32_t next_offset[MAX_VB];
    UINT i;

    (void)dev; (void)code; (void)len;
    if (!out || n > 32 || (n && !e))
        return E_INVALIDARG;
    l = (Layout *)obj_new(sizeof(Layout), K_LAYOUT);
    if (!l)
        return E_OUTOFMEMORY;
    memset(next_offset, 0, sizeof next_offset);
    for (i = 0; i < n; i++) {
        LayoutElem *le = &l->e[i];
        uint32_t slot = e[i].InputSlot < MAX_VB ? e[i].InputSlot : 0;
        canon_semantic(e[i].SemanticName ? e[i].SemanticName : "", e[i].SemanticIndex, 1,
                       le->sem, sizeof le->sem);
        le->fmt = vertex_format(e[i].Format);
        le->slot = slot;
        le->offset = e[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT
                   ? next_offset[slot] : e[i].AlignedByteOffset;
        next_offset[slot] = le->offset + fmt_info(e[i].Format).bpe;
        le->per_instance = e[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA;
        le->step = e[i].InstanceDataStepRate;
        if (le->fmt == VK_FORMAT_UNDEFINED)
            fprintf(stderr, "[VK] vertex format %d has no Vulkan equivalent\n", (int)e[i].Format);
    }
    l->n = n;
    *out = (ID3D11InputLayout *)l;
    return S_OK;
}

HRESULT vk11_dev_CreateBlendState(ID3D11Device *dev, const D3D11_BLEND_DESC *d, ID3D11BlendState **out)
{
    BlendState *b;
    int i;

    (void)dev;
    if (!d || !out)
        return E_INVALIDARG;
    b = (BlendState *)obj_new(sizeof(BlendState), K_BLEND);
    if (!b)
        return E_OUTOFMEMORY;
    b->d = *d;
    if (!d->IndependentBlendEnable)
        for (i = 1; i < 8; i++)
            b->d.RenderTarget[i] = d->RenderTarget[0];
    *out = (ID3D11BlendState *)b;
    return S_OK;
}

HRESULT vk11_dev_CreateDepthStencilState(ID3D11Device *dev, const D3D11_DEPTH_STENCIL_DESC *d,
                                         ID3D11DepthStencilState **out)
{
    DSState *s;

    (void)dev;
    if (!d || !out)
        return E_INVALIDARG;
    s = (DSState *)obj_new(sizeof(DSState), K_DSS);
    if (!s)
        return E_OUTOFMEMORY;
    s->d = *d;
    *out = (ID3D11DepthStencilState *)s;
    return S_OK;
}

HRESULT vk11_dev_CreateRasterizerState(ID3D11Device *dev, const D3D11_RASTERIZER_DESC *d,
                                       ID3D11RasterizerState **out)
{
    RSState *s;

    (void)dev;
    if (!d || !out)
        return E_INVALIDARG;
    s = (RSState *)obj_new(sizeof(RSState), K_RSS);
    if (!s)
        return E_OUTOFMEMORY;
    s->d = *d;
    *out = (ID3D11RasterizerState *)s;
    return S_OK;
}

static VkSamplerAddressMode address_mode(D3D11_TEXTURE_ADDRESS_MODE m)
{
    switch (m) {
    case D3D11_TEXTURE_ADDRESS_MIRROR:      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case D3D11_TEXTURE_ADDRESS_CLAMP:       return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case D3D11_TEXTURE_ADDRESS_BORDER:      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default:                                return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static VkCompareOp compare_op(D3D11_COMPARISON_FUNC f)
{
    switch (f) {
    case D3D11_COMPARISON_NEVER:         return VK_COMPARE_OP_NEVER;
    case D3D11_COMPARISON_LESS:          return VK_COMPARE_OP_LESS;
    case D3D11_COMPARISON_EQUAL:         return VK_COMPARE_OP_EQUAL;
    case D3D11_COMPARISON_LESS_EQUAL:    return VK_COMPARE_OP_LESS_OR_EQUAL;
    case D3D11_COMPARISON_GREATER:       return VK_COMPARE_OP_GREATER;
    case D3D11_COMPARISON_NOT_EQUAL:     return VK_COMPARE_OP_NOT_EQUAL;
    case D3D11_COMPARISON_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default:                             return VK_COMPARE_OP_ALWAYS;
    }
}

static VkSampler sampler_for(const D3D11_SAMPLER_DESC *d)
{
    VkSamplerCreateInfo ci;
    VkSamplerCustomBorderColorCreateInfoEXT cb;
    VkSampler s = VK_NULL_HANDLE;
    int i;
    unsigned f = (unsigned)d->Filter;

    for (i = 0; i < G.nsamplers; i++)
        if (memcmp(&G.samplers[i].d, d, sizeof *d) == 0)
            return G.samplers[i].s;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = (f & 0x4) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (f & 0x10) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (f & 0x1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = address_mode(d->AddressU);
    ci.addressModeV = address_mode(d->AddressV);
    ci.addressModeW = address_mode(d->AddressW);
    ci.mipLodBias = d->MipLODBias;
    if ((f & 0x40) && G.has_aniso) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = (float)(d->MaxAnisotropy ? d->MaxAnisotropy : 1);
        if (ci.maxAnisotropy > G.props.limits.maxSamplerAnisotropy)
            ci.maxAnisotropy = G.props.limits.maxSamplerAnisotropy;
    }
    if (f & 0x80) {
        ci.compareEnable = VK_TRUE;
        ci.compareOp = compare_op(d->ComparisonFunc);
    }
    ci.minLod = d->MinLOD;
    ci.maxLod = d->MaxLOD;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) {
        const float *c = d->BorderColor;
        if (G.has_custom_border) {
            memset(&cb, 0, sizeof cb);
            cb.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
            memcpy(cb.customBorderColor.float32, c, sizeof(float) * 4);
            cb.format = VK_FORMAT_UNDEFINED;
            ci.pNext = &cb;
            ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        } else if (c[3] >= 0.5f) {
            ci.borderColor = (c[0] + c[1] + c[2] >= 1.5f) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                          : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        }
    }
    if (vkCreateSampler(G.dev, &ci, NULL, &s) != VK_SUCCESS)
        return G.dummy_sampler;
    if (G.nsamplers < (int)(sizeof G.samplers / sizeof G.samplers[0])) {
        G.samplers[G.nsamplers].d = *d;
        G.samplers[G.nsamplers].s = s;
        G.nsamplers++;
    }
    return s;
}

HRESULT vk11_dev_CreateSamplerState(ID3D11Device *dev, const D3D11_SAMPLER_DESC *d,
                                    ID3D11SamplerState **out)
{
    Sampler *s;

    (void)dev;
    if (!d || !out)
        return E_INVALIDARG;
    s = (Sampler *)obj_new(sizeof(Sampler), K_SAMPLER);
    if (!s)
        return E_OUTOFMEMORY;
    s->d = *d;
    LOCK();
    s->s = sampler_for(d);
    UNLOCK();
    *out = (ID3D11SamplerState *)s;
    return S_OK;
}

/* ======================================================================
 * Pipelines
 * ====================================================================== */

static uint64_t key_hash(const PipeKey *k)
{
    const unsigned char *p = (const unsigned char *)k;
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;

    for (i = 0; i < sizeof *k; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void pipelines_forget(uint64_t serial)
{
    int b;

    for (b = 0; b < PIPE_BUCKETS; b++) {
        PipeEntry **pp = &G.pipes[b];
        while (*pp) {
            PipeEntry *e = *pp;
            if (e->key.vs == serial || e->key.ps == serial || e->key.il == serial) {
                *pp = e->next;
                garbage_add(G_PIPELINE, (uint64_t)e->pipe);
                if (S.bound_pipe == e->pipe)
                    S.bound_pipe = VK_NULL_HANDLE;
                free(e);
                G.npipes--;
            } else {
                pp = &e->next;
            }
        }
    }
}

static VkBlendFactor blend_factor(D3D11_BLEND b)
{
    switch (b) {
    case D3D11_BLEND_ZERO:             return VK_BLEND_FACTOR_ZERO;
    case D3D11_BLEND_ONE:              return VK_BLEND_FACTOR_ONE;
    case D3D11_BLEND_SRC_COLOR:        return VK_BLEND_FACTOR_SRC_COLOR;
    case D3D11_BLEND_INV_SRC_COLOR:    return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3D11_BLEND_SRC_ALPHA:        return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_ALPHA:    return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3D11_BLEND_DEST_ALPHA:       return VK_BLEND_FACTOR_DST_ALPHA;
    case D3D11_BLEND_INV_DEST_ALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case D3D11_BLEND_DEST_COLOR:       return VK_BLEND_FACTOR_DST_COLOR;
    case D3D11_BLEND_INV_DEST_COLOR:   return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3D11_BLEND_SRC_ALPHA_SAT:    return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case D3D11_BLEND_BLEND_FACTOR:     return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case D3D11_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case D3D11_BLEND_SRC1_COLOR:       return VK_BLEND_FACTOR_SRC1_COLOR;
    case D3D11_BLEND_INV_SRC1_COLOR:   return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case D3D11_BLEND_SRC1_ALPHA:       return VK_BLEND_FACTOR_SRC1_ALPHA;
    case D3D11_BLEND_INV_SRC1_ALPHA:   return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default:                           return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(D3D11_BLEND_OP o)
{
    switch (o) {
    case D3D11_BLEND_OP_SUBTRACT:     return VK_BLEND_OP_SUBTRACT;
    case D3D11_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case D3D11_BLEND_OP_MIN:          return VK_BLEND_OP_MIN;
    case D3D11_BLEND_OP_MAX:          return VK_BLEND_OP_MAX;
    default:                          return VK_BLEND_OP_ADD;
    }
}

static VkStencilOp stencil_op(D3D11_STENCIL_OP o)
{
    switch (o) {
    case D3D11_STENCIL_OP_ZERO:     return VK_STENCIL_OP_ZERO;
    case D3D11_STENCIL_OP_REPLACE:  return VK_STENCIL_OP_REPLACE;
    case D3D11_STENCIL_OP_INCR_SAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case D3D11_STENCIL_OP_DECR_SAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case D3D11_STENCIL_OP_INVERT:   return VK_STENCIL_OP_INVERT;
    case D3D11_STENCIL_OP_INCR:     return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case D3D11_STENCIL_OP_DECR:     return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:                        return VK_STENCIL_OP_KEEP;
    }
}

static VkStencilOpState stencil_state(const D3D11_DEPTH_STENCILOP_DESC *d, UINT8 rmask, UINT8 wmask)
{
    VkStencilOpState s;

    memset(&s, 0, sizeof s);
    s.failOp = stencil_op(d->StencilFailOp);
    s.depthFailOp = stencil_op(d->StencilDepthFailOp);
    s.passOp = stencil_op(d->StencilPassOp);
    s.compareOp = compare_op(d->StencilFunc);
    s.compareMask = rmask;
    s.writeMask = wmask;
    return s;
}

static VkPrimitiveTopology topology(D3D11_PRIMITIVE_TOPOLOGY t)
{
    switch (t) {
    case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default:                                     return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static VkPipeline build_pipeline(const PipeKey *k)
{
    Shader *vs = S.vs, *ps = S.ps;
    Layout *il = S.il;
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb[MAX_VB];
    VkVertexInputAttributeDescription va[MAX_IO];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vps;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba[8];
    VkPipelineColorBlendStateCreateInfo cb;
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                             VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE };
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineRenderingCreateInfo ri;
    VkGraphicsPipelineCreateInfo pi;
    VkPipeline pipe = VK_NULL_HANDLE;
    uint32_t nvb = 0, nva = 0, i, j, used_slots = 0, need_dummy = 0;

    memset(st, 0, sizeof st);
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs->module;
    st[0].pName = vs->entry;
    st[1] = st[0];
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    if (ps) {
        st[1].module = ps_for_vs(ps, vs);
        st[1].pName = ps->entry;
    }

    /* Vertex inputs, matched to the layout by semantic. An input the layout
     * does not feed reads zeros from a stride-0 dummy binding. */
    for (i = 0; i < (uint32_t)vs->n_in && nva < MAX_IO; i++) {
        const LayoutElem *le = NULL;
        if (il)
            for (j = 0; j < il->n; j++)
                if (strcmp(il->e[j].sem, vs->in[i].sem) == 0 && il->e[j].fmt != VK_FORMAT_UNDEFINED) {
                    le = &il->e[j];
                    break;
                }
        va[nva].location = vs->in[i].loc;
        if (le) {
            va[nva].binding = le->slot;
            va[nva].format = le->fmt;
            va[nva].offset = le->offset;
            if (!(used_slots & (1u << le->slot))) {
                used_slots |= 1u << le->slot;
                vb[nvb].binding = le->slot;
                vb[nvb].stride = 0;                     /* dynamic */
                vb[nvb].inputRate = le->per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                     : VK_VERTEX_INPUT_RATE_VERTEX;
                nvb++;
            }
        } else {
            va[nva].binding = DUMMY_VB_BINDING;
            va[nva].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            va[nva].offset = 0;
            need_dummy = 1;
        }
        nva++;
    }
    if (need_dummy) {
        vb[nvb].binding = DUMMY_VB_BINDING;
        vb[nvb].stride = 0;
        vb[nvb].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        nvb++;
    }
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = nvb;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = nva;
    vi.pVertexAttributeDescriptions = va;

    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = topology((D3D11_PRIMITIVE_TOPOLOGY)k->topology);
    /* D3D11 always cuts an indexed strip at 0xFFFF / 0xFFFFFFFF. */
    ia.primitiveRestartEnable = (ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                                 ia.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);

    memset(&vps, 0, sizeof vps);
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = (!k->rs.DepthClipEnable && G.has_depth_clamp) ? VK_TRUE : VK_FALSE;
    rs.polygonMode = (k->rs.FillMode == D3D11_FILL_WIREFRAME && G.has_fill_nonsolid)
                   ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k->rs.CullMode == D3D11_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT :
                  k->rs.CullMode == D3D11_CULL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    /* The negative viewport keeps D3D's on-screen winding; see the top. */
    rs.frontFace = k->rs.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                               : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = (k->rs.DepthBias != 0 || k->rs.SlopeScaledDepthBias != 0.0f);
    rs.depthBiasConstantFactor = (float)k->rs.DepthBias;
    rs.depthBiasClamp = G.has_bias_clamp ? k->rs.DepthBiasClamp : 0.0f;
    rs.depthBiasSlopeFactor = k->rs.SlopeScaledDepthBias;
    rs.lineWidth = 1.0f;

    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1);
    ms.alphaToCoverageEnable = k->blend.AlphaToCoverageEnable ? VK_TRUE : VK_FALSE;

    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (k->dsfmt != VK_FORMAT_UNDEFINED) {
        ds.depthTestEnable = k->ds.DepthEnable ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = (k->ds.DepthEnable && k->ds.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL);
        ds.depthCompareOp = compare_op(k->ds.DepthFunc);
        if (has_stencil(k->dsfmt) && k->ds.StencilEnable) {
            ds.stencilTestEnable = VK_TRUE;
            ds.front = stencil_state(&k->ds.FrontFace, k->ds.StencilReadMask, k->ds.StencilWriteMask);
            ds.back = stencil_state(&k->ds.BackFace, k->ds.StencilReadMask, k->ds.StencilWriteMask);
        }
    }

    memset(cba, 0, sizeof cba);
    for (i = 0; i < k->nrt; i++) {
        const D3D11_RENDER_TARGET_BLEND_DESC *b = &k->blend.RenderTarget[i];
        cba[i].blendEnable = b->BlendEnable ? VK_TRUE : VK_FALSE;
        cba[i].srcColorBlendFactor = blend_factor(b->SrcBlend);
        cba[i].dstColorBlendFactor = blend_factor(b->DestBlend);
        cba[i].colorBlendOp = blend_op(b->BlendOp);
        cba[i].srcAlphaBlendFactor = blend_factor(b->SrcBlendAlpha);
        cba[i].dstAlphaBlendFactor = blend_factor(b->DestBlendAlpha);
        cba[i].alphaBlendOp = blend_op(b->BlendOpAlpha);
        cba[i].colorWriteMask = b->RenderTargetWriteMask & 0xF;
        if (!G.has_dual_src) {
            if (cba[i].srcColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR) cba[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            if (cba[i].dstColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR) cba[i].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            if (cba[i].srcAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR) cba[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            if (cba[i].dstAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR) cba[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        }
    }
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = k->nrt;
    cb.pAttachments = cba;

    memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = sizeof dyn / sizeof dyn[0];
    dy.pDynamicStates = dyn;

    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ri.colorAttachmentCount = k->nrt;
    ri.pColorAttachmentFormats = k->rt;
    ri.depthAttachmentFormat = k->dsfmt;
    ri.stencilAttachmentFormat = has_stencil(k->dsfmt) ? k->dsfmt : VK_FORMAT_UNDEFINED;

    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.pNext = &ri;
    pi.stageCount = ps ? 2 : 1;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vps;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = G.pipe_layout;
    if (vkCreateGraphicsPipelines(G.dev, G.pcache, 1, &pi, NULL, &pipe) != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkCreateGraphicsPipelines failed\n");
        return VK_NULL_HANDLE;
    }
    if (vk_log())
        fprintf(stderr, "[VK] pipeline %d: vs %llu ps %llu layout %llu, %u targets\n",
                G.npipes + 1, (unsigned long long)k->vs, (unsigned long long)k->ps,
                (unsigned long long)k->il, k->nrt);
    return pipe;
}

static VkPipeline current_pipeline(void)
{
    PipeKey k;
    PipeEntry *e;
    uint64_t h;
    uint32_t i;
    static const D3D11_BLEND_DESC default_blend = {
        FALSE, FALSE,
        { { FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE,
            D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL } }
    };
    static const D3D11_DEPTH_STENCIL_DESC default_ds = {
        TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, FALSE, 0xff, 0xff,
        { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS },
        { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS }
    };
    static const D3D11_RASTERIZER_DESC default_rs = {
        D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE
    };

    if (!S.vs)
        return VK_NULL_HANDLE;
    memset(&k, 0, sizeof k);
    k.vs = S.vs->o.serial;
    k.ps = S.ps ? S.ps->o.serial : 0;
    k.il = S.il ? S.il->o.serial : 0;
    k.blend = S.blend ? S.blend->d : default_blend;
    if (!S.blend)
        for (i = 1; i < 8; i++)
            k.blend.RenderTarget[i] = default_blend.RenderTarget[0];
    k.ds = S.dss ? S.dss->d : default_ds;
    k.rs = S.rs ? S.rs->d : default_rs;
    /* Fields the pipeline ignores stay out of the key. */
    k.rs.ScissorEnable = FALSE;
    k.rs.MultisampleEnable = FALSE;
    k.rs.AntialiasedLineEnable = FALSE;
    k.topology = (uint32_t)S.topo;
    k.nrt = S.pass_nrtv;
    for (i = 0; i < S.pass_nrtv; i++)
        k.rt[i] = S.pass_rtv[i] ? S.pass_rtv[i]->fmt : VK_FORMAT_UNDEFINED;
    k.dsfmt = S.pass_dsv ? S.pass_dsv->fmt : VK_FORMAT_UNDEFINED;
    k.samples = 1;
    for (i = 0; i < S.pass_nrtv; i++)
        if (S.pass_rtv[i]) {
            k.samples = S.pass_rtv[i]->res->samples;
            break;
        }
    if (i == S.pass_nrtv && S.pass_dsv)
        k.samples = S.pass_dsv->res->samples;

    h = key_hash(&k);
    for (e = G.pipes[h % PIPE_BUCKETS]; e; e = e->next)
        if (e->hash == h && memcmp(&e->key, &k, sizeof k) == 0)
            return e->pipe;
    e = (PipeEntry *)calloc(1, sizeof *e);
    if (!e)
        return VK_NULL_HANDLE;
    e->key = k;
    e->hash = h;
    e->pipe = build_pipeline(&k);
    if (!e->pipe) {
        free(e);
        return VK_NULL_HANDLE;
    }
    e->next = G.pipes[h % PIPE_BUCKETS];
    G.pipes[h % PIPE_BUCKETS] = e;
    G.npipes++;
    return e->pipe;
}

/* ======================================================================
 * Context: state
 * ====================================================================== */

static int same_targets(void)
{
    uint32_t i;

    if (S.nrtv != S.pass_nrtv || S.dsv != S.pass_dsv)
        return 0;
    for (i = 0; i < S.nrtv; i++)
        if (S.rtv[i] != S.pass_rtv[i])
            return 0;
    return 1;
}

void vk11_ctx_OMSetRenderTargets(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView *const *rtv,
                                 ID3D11DepthStencilView *dsv)
{
    uint32_t i;

    (void)c;
    LOCK();
    if (n > 8)
        n = 8;
    for (i = 0; i < 8; i++)
        REBIND(S.rtv[i], (i < n && rtv) ? (View *)rtv[i] : NULL);
    S.nrtv = n;
    while (S.nrtv > 0 && !S.rtv[S.nrtv - 1])
        S.nrtv--;
    REBIND(S.dsv, (View *)dsv);
    /* The pass never outlives the views it was begun with. */
    if (S.rendering && !same_targets())
        end_rendering();
    UNLOCK();
}

void vk11_ctx_OMGetRenderTargets(ID3D11DeviceContext *c, UINT n, ID3D11RenderTargetView **rtv,
                                 ID3D11DepthStencilView **dsv)
{
    uint32_t i;

    (void)c;
    LOCK();
    for (i = 0; rtv && i < n; i++) {
        rtv[i] = i < 8 ? (ID3D11RenderTargetView *)S.rtv[i] : NULL;
        if (rtv[i])
            vk11_add_ref(rtv[i]);
    }
    if (dsv) {
        *dsv = (ID3D11DepthStencilView *)S.dsv;
        if (*dsv)
            vk11_add_ref(*dsv);
    }
    UNLOCK();
}

void vk11_ctx_OMSetBlendState(ID3D11DeviceContext *c, ID3D11BlendState *b, const FLOAT f[4], UINT mask)
{
    (void)c; (void)mask;
    LOCK();
    REBIND(S.blend, (BlendState *)b);
    if (f)
        memcpy(S.blend_factor, f, sizeof S.blend_factor);
    else
        S.blend_factor[0] = S.blend_factor[1] = S.blend_factor[2] = S.blend_factor[3] = 1.0f;
    UNLOCK();
}

void vk11_ctx_OMSetDepthStencilState(ID3D11DeviceContext *c, ID3D11DepthStencilState *s, UINT ref)
{
    (void)c;
    LOCK();
    REBIND(S.dss, (DSState *)s);
    S.stencil_ref = ref;
    UNLOCK();
}

void vk11_ctx_RSSetState(ID3D11DeviceContext *c, ID3D11RasterizerState *s)
{
    (void)c;
    LOCK();
    REBIND(S.rs, (RSState *)s);
    UNLOCK();
}

void vk11_ctx_RSSetViewports(ID3D11DeviceContext *c, UINT n, const D3D11_VIEWPORT *vp)
{
    (void)c;
    LOCK();
    if (n > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
        n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    if (vp && n)
        memcpy(S.vp, vp, sizeof *vp * n);
    S.nvp = vp ? n : 0;
    UNLOCK();
}

void vk11_ctx_RSGetViewports(ID3D11DeviceContext *c, UINT *n, D3D11_VIEWPORT *vp)
{
    (void)c;
    LOCK();
    if (n) {
        if (vp) {
            UINT k = *n < S.nvp ? *n : S.nvp;
            memcpy(vp, S.vp, sizeof *vp * k);
            if (*n > k)
                memset(vp + k, 0, sizeof *vp * (*n - k));
        }
        *n = S.nvp;
    }
    UNLOCK();
}

void vk11_ctx_RSSetScissorRects(ID3D11DeviceContext *c, UINT n, const D3D11_RECT *r)
{
    (void)c;
    LOCK();
    if (n > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
        n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    if (r && n)
        memcpy(S.sc, r, sizeof *r * n);
    S.nsc = r ? n : 0;
    UNLOCK();
}

void vk11_ctx_RSGetScissorRects(ID3D11DeviceContext *c, UINT *n, D3D11_RECT *r)
{
    (void)c;
    LOCK();
    if (n) {
        if (r) {
            UINT k = *n < S.nsc ? *n : S.nsc;
            memcpy(r, S.sc, sizeof *r * k);
        }
        *n = S.nsc;
    }
    UNLOCK();
}

void vk11_ctx_IASetInputLayout(ID3D11DeviceContext *c, ID3D11InputLayout *l)
{
    (void)c;
    LOCK();
    REBIND(S.il, (Layout *)l);
    UNLOCK();
}

void vk11_ctx_IASetVertexBuffers(ID3D11DeviceContext *c, UINT first, UINT n, ID3D11Buffer *const *b,
                                 const UINT *strides, const UINT *offsets)
{
    UINT i;

    (void)c;
    LOCK();
    for (i = 0; i < n && first + i < MAX_VB; i++) {
        REBIND(S.vb[first + i], b ? (Buffer *)b[i] : NULL);
        S.vb_stride[first + i] = strides ? strides[i] : 0;
        S.vb_off[first + i] = offsets ? offsets[i] : 0;
    }
    UNLOCK();
}

void vk11_ctx_IASetIndexBuffer(ID3D11DeviceContext *c, ID3D11Buffer *b, DXGI_FORMAT f, UINT off)
{
    (void)c;
    LOCK();
    REBIND(S.ib, (Buffer *)b);
    S.ib_fmt = f;
    S.ib_off = off;
    UNLOCK();
}

void vk11_ctx_IASetPrimitiveTopology(ID3D11DeviceContext *c, D3D11_PRIMITIVE_TOPOLOGY t)
{
    (void)c;
    S.topo = t;
}

void vk11_ctx_VSSetShader(ID3D11DeviceContext *c, ID3D11VertexShader *s,
                          ID3D11ClassInstance *const *ci, UINT n)
{
    (void)c; (void)ci; (void)n;
    LOCK();
    REBIND(S.vs, (Shader *)s);
    UNLOCK();
}

void vk11_ctx_PSSetShader(ID3D11DeviceContext *c, ID3D11PixelShader *s,
                          ID3D11ClassInstance *const *ci, UINT n)
{
    (void)c; (void)ci; (void)n;
    LOCK();
    REBIND(S.ps, (Shader *)s);
    UNLOCK();
}

void vk11_ctx_VSSetConstantBuffers(ID3D11DeviceContext *c, UINT first, UINT n, ID3D11Buffer *const *b)
{
    UINT i;

    (void)c;
    LOCK();
    for (i = 0; i < n && first + i < VK11_MAX_CBUFFERS; i++)
        REBIND(S.vs_cb[first + i], b ? (Buffer *)b[i] : NULL);
    S.dirty_desc = 1;
    UNLOCK();
}

void vk11_ctx_PSSetConstantBuffers(ID3D11DeviceContext *c, UINT first, UINT n, ID3D11Buffer *const *b)
{
    UINT i;

    (void)c;
    LOCK();
    for (i = 0; i < n && first + i < VK11_MAX_CBUFFERS; i++)
        REBIND(S.ps_cb[first + i], b ? (Buffer *)b[i] : NULL);
    S.dirty_desc = 1;
    UNLOCK();
}

void vk11_ctx_PSSetShaderResources(ID3D11DeviceContext *c, UINT first, UINT n,
                                   ID3D11ShaderResourceView *const *v)
{
    UINT i;

    (void)c;
    LOCK();
    for (i = 0; i < n && first + i < VK11_MAX_TEXTURES; i++)
        REBIND(S.srv[first + i], v ? (View *)v[i] : NULL);
    S.dirty_desc = 1;
    UNLOCK();
}

void vk11_ctx_PSSetSamplers(ID3D11DeviceContext *c, UINT first, UINT n, ID3D11SamplerState *const *s)
{
    UINT i;

    (void)c;
    LOCK();
    for (i = 0; i < n && first + i < VK11_MAX_SAMPLERS; i++)
        REBIND(S.smp[first + i], s ? (Sampler *)s[i] : NULL);
    S.dirty_desc = 1;
    UNLOCK();
}

/* ======================================================================
 * Context: drawing
 * ====================================================================== */

static int begin_rendering(void)
{
    VkRenderingAttachmentInfo col[8], dep;
    VkRenderingInfo ri;
    uint32_t i, w = 0x7fffffff, h = 0x7fffffff;
    View *dsv = S.dsv;

    if (S.rendering && same_targets())
        return 1;
    end_rendering();
    if (S.need_barrier)
        barrier_all();
    memset(col, 0, sizeof col);
    memset(&dep, 0, sizeof dep);
    for (i = 0; i < S.nrtv; i++) {
        View *v = S.rtv[i];
        col[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (!v)
            continue;
        col[i].imageView = v->view;
        col[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        col[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        col[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (v->w < w) w = v->w;
        if (v->h < h) h = v->h;
    }
    if (dsv) {
        dep.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        dep.imageView = dsv->view;
        dep.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        dep.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (dsv->w < w) w = dsv->w;
        if (dsv->h < h) h = dsv->h;
    }
    if (w == 0x7fffffff)
        return 0;                       /* nothing bound to draw into */
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = w;
    ri.renderArea.extent.height = h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = S.nrtv;
    ri.pColorAttachments = col;
    if (dsv) {
        ri.pDepthAttachment = &dep;
        if (has_stencil(dsv->fmt))
            ri.pStencilAttachment = &dep;
    }
    vkCmdBeginRendering(G.cmd, &ri);
    S.rendering = 1;
    for (i = 0; i < 8; i++)
        S.pass_rtv[i] = S.rtv[i];
    S.pass_nrtv = S.nrtv;
    S.pass_dsv = S.dsv;
    S.pass_extent = ri.renderArea.extent;
    S.bound_pipe = VK_NULL_HANDLE;      /* dynamic state is per pass here */
    S.dirty_desc = 1;
    return 1;
}

typedef struct DescData {
    VkDescriptorBufferInfo cb[VK11_MAX_CBUFFERS * 2];
    VkDescriptorImageInfo tex[VK11_MAX_TEXTURES];
    VkDescriptorImageInfo smp[VK11_MAX_SAMPLERS];
} DescData;

/* SpvDim + 1, as Shader.tex_dim holds it, for a view type. */
static uint8_t view_dim(VkImageViewType t)
{
    switch (t) {
    case VK_IMAGE_VIEW_TYPE_3D:         return 2 + 1;
    case VK_IMAGE_VIEW_TYPE_CUBE:       return 3 + 1;
    case VK_IMAGE_VIEW_TYPE_2D:         return 1 + 1;
    default:                            return 0;      /* arrays: not declared here */
    }
}

/* What a slot binds: the view, when its shape is what the shader declared
 * (or the shader does not read the slot), else zeros of the right shape --
 * the descriptor's image type must match the shader's in Vulkan. */
static VkImageView texture_for(const View *v, const Shader *ps, uint32_t slot)
{
    uint8_t want = ps ? ps->tex_dim[slot] : 0;

    if (v && (!want || view_dim(v->type) == want))
        return v->view;
    if (want == 3 + 1)
        return G.dummy_viewcube;
    if (want == 2 + 1)
        return G.dummy_view3d;
    return G.dummy_view2d;
}

static void bind_descriptors(void)
{
    DescData d;
    VkDescriptorSet set;
    uint32_t i;

    for (i = 0; i < VK11_MAX_CBUFFERS * 2; i++) {
        Buffer *b = i < VK11_MAX_CBUFFERS ? S.vs_cb[i] : S.ps_cb[i - VK11_MAX_CBUFFERS];
        VkBuffer buf;
        VkDeviceSize off;
        if (b && buffer_binding(b, &buf, &off)) {
            d.cb[i].buffer = buf;
            d.cb[i].offset = off;
            d.cb[i].range = b->desc.ByteWidth;
            if (d.cb[i].range > G.props.limits.maxUniformBufferRange)
                d.cb[i].range = G.props.limits.maxUniformBufferRange;
        } else {
            d.cb[i].buffer = G.zero_buf;
            d.cb[i].offset = 0;
            d.cb[i].range = 65536;
        }
    }
    for (i = 0; i < VK11_MAX_TEXTURES; i++) {
        View *v = S.srv[i];
        d.tex[i].sampler = VK_NULL_HANDLE;
        d.tex[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        d.tex[i].imageView = texture_for(v, S.ps, i);
    }
    for (i = 0; i < VK11_MAX_SAMPLERS; i++) {
        d.smp[i].sampler = S.smp[i] ? S.smp[i]->s : G.dummy_sampler;
        d.smp[i].imageView = VK_NULL_HANDLE;
        d.smp[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    set = alloc_descriptor_set();
    if (!set)
        return;
    vkUpdateDescriptorSetWithTemplate(G.dev, set, G.dtemplate, &d);
    vkCmdBindDescriptorSets(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.pipe_layout, 0, 1, &set,
                            0, NULL);
    S.dirty_desc = 0;
}

/* A texture view's image layout cannot be checked against its declared
 * dimension here (a TextureCube in the shader with a 2D view bound is the
 * caller's mistake in D3D11 too); the descriptor is whatever is bound. */

static int prepare_draw(int indexed)
{
    VkPipeline pipe;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer bufs[MAX_VB];
    VkDeviceSize offs[MAX_VB], strides[MAX_VB];
    uint32_t i;

    if (!S.vs || !begin_rendering())
        return 0;
    pipe = current_pipeline();
    if (!pipe)
        return 0;
    if (pipe != S.bound_pipe) {
        vkCmdBindPipeline(G.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        S.bound_pipe = pipe;
    }

    /* Y up, as D3D: a negative height starting at the bottom edge. */
    if (S.nvp) {
        vp.x = S.vp[0].TopLeftX;
        vp.y = S.vp[0].TopLeftY + S.vp[0].Height;
        vp.width = S.vp[0].Width;
        vp.height = -S.vp[0].Height;
        vp.minDepth = S.vp[0].MinDepth;
        vp.maxDepth = S.vp[0].MaxDepth;
    } else {
        vp.x = 0;
        vp.y = (float)S.pass_extent.height;
        vp.width = (float)S.pass_extent.width;
        vp.height = -(float)S.pass_extent.height;
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
    }
    if (vp.width <= 0.0f)
        vp.width = 1.0f;
    vkCmdSetViewport(G.cmd, 0, 1, &vp);
    if (S.rs && S.rs->d.ScissorEnable && S.nsc) {
        LONG l = S.sc[0].left > 0 ? S.sc[0].left : 0, t = S.sc[0].top > 0 ? S.sc[0].top : 0;
        LONG r = S.sc[0].right, b = S.sc[0].bottom;
        sc.offset.x = l;
        sc.offset.y = t;
        sc.extent.width = r > l ? (uint32_t)(r - l) : 0;
        sc.extent.height = b > t ? (uint32_t)(b - t) : 0;
    } else {
        sc.offset.x = sc.offset.y = 0;
        sc.extent = S.pass_extent;
    }
    vkCmdSetScissor(G.cmd, 0, 1, &sc);
    vkCmdSetBlendConstants(G.cmd, S.blend_factor);
    vkCmdSetStencilReference(G.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, S.stencil_ref);

    /* Every binding slot the pipeline may read, plus the zero binding. */
    for (i = 0; i < MAX_VB; i++) {
        VkBuffer b;
        VkDeviceSize o;
        if (i == DUMMY_VB_BINDING || !S.vb[i] || !buffer_binding(S.vb[i], &b, &o)) {
            bufs[i] = G.zero_buf;
            offs[i] = 0;
            strides[i] = 0;
        } else {
            bufs[i] = b;
            offs[i] = o + S.vb_off[i];
            strides[i] = S.vb_stride[i];
        }
    }
    vkCmdBindVertexBuffers2(G.cmd, 0, MAX_VB, bufs, offs, NULL, strides);
    if (indexed) {
        VkBuffer b;
        VkDeviceSize o;
        if (!S.ib || !buffer_binding(S.ib, &b, &o))
            return 0;
        vkCmdBindIndexBuffer(G.cmd, b, o + S.ib_off,
                             S.ib_fmt == DXGI_FORMAT_R32_UINT ? VK_INDEX_TYPE_UINT32
                                                              : VK_INDEX_TYPE_UINT16);
    }
    bind_descriptors();
    return 1;
}

void vk11_ctx_Draw(ID3D11DeviceContext *c, UINT count, UINT start)
{
    (void)c;
    LOCK();
    if (count && prepare_draw(0))
        vkCmdDraw(G.cmd, count, 1, start, 0);
    UNLOCK();
}

void vk11_ctx_DrawIndexed(ID3D11DeviceContext *c, UINT count, UINT start, INT base)
{
    (void)c;
    LOCK();
    if (count && prepare_draw(1))
        vkCmdDrawIndexed(G.cmd, count, 1, start, base, 0);
    UNLOCK();
}

static VkImageSubresourceRange view_range(const View *v, VkImageAspectFlags aspect)
{
    VkImageSubresourceRange r;
    r.aspectMask = aspect;
    r.baseMipLevel = v->mip;
    r.levelCount = 1;
    r.baseArrayLayer = v->layer;
    r.layerCount = 1;
    return r;
}

void vk11_ctx_ClearRenderTargetView(ID3D11DeviceContext *c, ID3D11RenderTargetView *rtv,
                                    const FLOAT color[4])
{
    View *v = (View *)rtv;
    VkClearColorValue cv;
    VkImageSubresourceRange r;

    (void)c;
    if (!v || !color)
        return;
    LOCK();
    memcpy(cv.float32, color, sizeof cv.float32);
    r = view_range(v, VK_IMAGE_ASPECT_COLOR_BIT);
    begin_transfer();
    vkCmdClearColorImage(G.cmd, v->res->img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
    end_transfer();
    UNLOCK();
}

void vk11_ctx_ClearDepthStencilView(ID3D11DeviceContext *c, ID3D11DepthStencilView *dsv, UINT flags,
                                    FLOAT depth, UINT8 stencil)
{
    View *v = (View *)dsv;
    VkClearDepthStencilValue dv;
    VkImageSubresourceRange r;
    VkImageAspectFlags aspect = 0;

    (void)c;
    if (!v)
        return;
    if (flags & D3D11_CLEAR_DEPTH)
        aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if ((flags & D3D11_CLEAR_STENCIL) && has_stencil(v->fmt))
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!aspect)
        return;
    LOCK();
    dv.depth = depth;
    dv.stencil = stencil;
    r = view_range(v, aspect);
    begin_transfer();
    vkCmdClearDepthStencilImage(G.cmd, v->res->img, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &r);
    end_transfer();
    UNLOCK();
}

/* ======================================================================
 * Context: memory
 * ====================================================================== */

/* Map(DISCARD) of a large DYNAMIC buffer: point it at one of its own
 * allocations that no frame still in flight reads. The one it leaves stays
 * busy until the frame being recorded has finished. 0 when all are busy and
 * none can be added; the arena takes it then. */
static int dynamic_new_version(Buffer *b)
{
    int i;

    if (b->cur_ver >= 0)
        b->ver[b->cur_ver].busy_until = G.rec_serial;
    b->cur_ver = -1;
    for (i = 0; i < b->nver; i++)
        if (b->ver[i].busy_until <= G.done_serial)
            break;
    if (i == b->nver) {
        if (b->nver == DYNAMIC_VERSIONS ||
            !make_buffer(b->desc.ByteWidth, ALL_BUFFER_USAGE, HOST_MEM,
                         &b->ver[i].buf, &b->ver[i].mem, &b->ver[i].ptr))
            return 0;
        b->nver++;
    }
    b->cur_ver = i;
    b->cur_buf = b->ver[i].buf;
    b->cur_off = 0;
    b->cur_ptr = b->ver[i].ptr;
    b->cur_slot = -1;
    return 1;
}

HRESULT vk11_ctx_Map(ID3D11DeviceContext *c, ID3D11Resource *res, UINT sub, D3D11_MAP type,
                     UINT flags, D3D11_MAPPED_SUBRESOURCE *out)
{
    Obj *o = (Obj *)res;
    HRESULT hr = E_INVALIDARG;

    (void)c; (void)flags;
    if (!o || !out)
        return E_INVALIDARG;
    memset(out, 0, sizeof *out);
    LOCK();
    if (o->kind == K_BUFFER) {
        Buffer *b = (Buffer *)o;
        if (b->desc.Usage == D3D11_USAGE_DYNAMIC) {
            if ((type == D3D11_MAP_WRITE_DISCARD || b->cur_slot == -2) &&
                !(b->desc.ByteWidth >= DYNAMIC_VERSION_MIN && dynamic_new_version(b))) {
                VkBuffer buf;
                VkDeviceSize off;
                VkDeviceSize align = G.props.limits.minUniformBufferOffsetAlignment;
                uint8_t *p = arena_alloc(b->desc.ByteWidth, align < 256 ? 256 : align, &buf, &off);
                if (p) {
                    b->cur_buf = buf;
                    b->cur_off = off;
                    b->cur_ptr = p;
                    b->cur_slot = G.fi;
                }
            }
            if (b->cur_ptr) {
                out->pData = b->cur_ptr;
                out->RowPitch = out->DepthPitch = b->desc.ByteWidth;
                hr = S_OK;
            }
        } else if (b->desc.Usage == D3D11_USAGE_STAGING) {
            if (b->pending_gpu_write && type != D3D11_MAP_WRITE) {
                flush_and_wait();
                b->pending_gpu_write = 0;
            }
            out->pData = b->host;
            out->RowPitch = out->DepthPitch = b->desc.ByteWidth;
            hr = S_OK;
        }
    } else if (o->kind == K_TEXTURE) {
        Image *im = (Image *)o;
        if (im->sptr) {
            if (sub == 0) {
                if (im->pending_gpu_write && type != D3D11_MAP_WRITE) {
                    flush_and_wait();
                    im->pending_gpu_write = 0;
                }
                out->pData = im->sptr;
                out->RowPitch = im->srow;
                out->DepthPitch = im->sslice;
                hr = S_OK;
            }
        } else if (im->img && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE)) {
            uint32_t mip, layer, w, h, d, rows;
            VkDeviceSize size;
            sub_split(im, sub, &mip, &layer);
            w = mip_dim(im->w, mip);
            h = mip_dim(im->h, mip);
            d = im->is3d ? mip_dim(im->depth, mip) : 1;
            rows = (h + im->f.block - 1) / im->f.block;
            im->map_row = (w + im->f.block - 1) / im->f.block * im->f.bpe;
            size = (VkDeviceSize)im->map_row * rows * d;
            im->map_ptr = arena_alloc(size, upload_align(&im->f), &im->map_buf, &im->map_off);
            im->map_sub = sub;
            if (im->map_ptr) {
                out->pData = im->map_ptr;
                out->RowPitch = im->map_row;
                out->DepthPitch = im->map_row * rows;
                hr = S_OK;
            }
        }
    }
    UNLOCK();
    return hr;
}

void vk11_ctx_Unmap(ID3D11DeviceContext *c, ID3D11Resource *res, UINT sub)
{
    Obj *o = (Obj *)res;

    (void)c;
    if (!o || o->kind != K_TEXTURE)
        return;
    LOCK();
    {
        Image *im = (Image *)o;
        if (im->img && im->map_ptr && im->map_sub == sub) {
            uint32_t mip, layer;
            VkBufferImageCopy cp;
            sub_split(im, sub, &mip, &layer);
            memset(&cp, 0, sizeof cp);
            cp.bufferOffset = im->map_off;
            cp.imageSubresource.aspectMask = im->f.aspect;
            cp.imageSubresource.mipLevel = mip;
            cp.imageSubresource.baseArrayLayer = im->is3d ? 0 : layer;
            cp.imageSubresource.layerCount = 1;
            cp.imageExtent.width = mip_dim(im->w, mip);
            cp.imageExtent.height = mip_dim(im->h, mip);
            cp.imageExtent.depth = im->is3d ? mip_dim(im->depth, mip) : 1;
            begin_transfer();
            vkCmdCopyBufferToImage(G.cmd, im->map_buf, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
            end_transfer();
            im->map_ptr = NULL;
        }
    }
    UNLOCK();
}

void vk11_ctx_UpdateSubresource(ID3D11DeviceContext *c, ID3D11Resource *res, UINT sub,
                                const D3D11_BOX *box, const void *data, UINT row, UINT depth)
{
    Obj *o = (Obj *)res;

    (void)c;
    if (!o || !data)
        return;
    LOCK();
    if (o->kind == K_TEXTURE) {
        Image *im = (Image *)o;
        if (im->img)
            image_upload(im, sub, box, data, row, depth);
    } else if (o->kind == K_BUFFER) {
        Buffer *b = (Buffer *)o;
        UINT start = box ? box->left : 0;
        UINT end = box ? box->right : b->desc.ByteWidth;
        if (end > b->desc.ByteWidth)
            end = b->desc.ByteWidth;
        if (end > start) {
            if (b->host) {
                memcpy(b->host + start, data, end - start);
            } else if (b->buf) {
                VkBuffer src;
                VkDeviceSize off;
                uint8_t *p = arena_alloc(end - start, 16, &src, &off);
                if (p) {
                    VkBufferCopy cp;
                    memcpy(p, data, end - start);
                    cp.srcOffset = off;
                    cp.dstOffset = start;
                    cp.size = end - start;
                    begin_transfer();
                    vkCmdCopyBuffer(G.cmd, src, b->buf, 1, &cp);
                    end_transfer();
                }
            }
        }
    }
    UNLOCK();
}

/* One subresource (or a box of it) from src to dst, wherever each lives. */
static void copy_region(Image *dst, UINT dsub, UINT dx, UINT dy, UINT dz,
                        Image *src, UINT ssub, const D3D11_BOX *box)
{
    uint32_t smip, slayer, dmip, dlayer, w, h, d, sx = 0, sy = 0, sz = 0;

    sub_split(src, ssub, &smip, &slayer);
    sub_split(dst, dsub, &dmip, &dlayer);
    w = mip_dim(src->w, smip);
    h = mip_dim(src->h, smip);
    d = src->is3d ? mip_dim(src->depth, smip) : 1;
    if (box) {
        sx = box->left; sy = box->top; sz = box->front;
        w = box->right > box->left ? box->right - box->left : 0;
        h = box->bottom > box->top ? box->bottom - box->top : 0;
        d = box->back > box->front ? box->back - box->front : 0;
    }
    if (!w || !h || !d)
        return;
    begin_transfer();
    if (src->img && dst->img) {
        VkImageCopy cp;
        memset(&cp, 0, sizeof cp);
        cp.srcSubresource.aspectMask = src->f.aspect;
        cp.srcSubresource.mipLevel = smip;
        cp.srcSubresource.baseArrayLayer = src->is3d ? 0 : slayer;
        cp.srcSubresource.layerCount = 1;
        cp.srcOffset.x = (int32_t)sx; cp.srcOffset.y = (int32_t)sy; cp.srcOffset.z = (int32_t)sz;
        cp.dstSubresource.aspectMask = dst->f.aspect;
        cp.dstSubresource.mipLevel = dmip;
        cp.dstSubresource.baseArrayLayer = dst->is3d ? 0 : dlayer;
        cp.dstSubresource.layerCount = 1;
        cp.dstOffset.x = (int32_t)dx; cp.dstOffset.y = (int32_t)dy; cp.dstOffset.z = (int32_t)dz;
        cp.extent.width = w; cp.extent.height = h; cp.extent.depth = d;
        if (src->f.bpe == dst->f.bpe && src->f.block == dst->f.block &&
            src->samples == dst->samples)
            vkCmdCopyImage(G.cmd, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->img,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    } else if (src->img && dst->sbuf && dsub == 0 && src->samples == 1) {
        VkBufferImageCopy cp;
        memset(&cp, 0, sizeof cp);
        cp.bufferOffset = (VkDeviceSize)dz * dst->sslice + (VkDeviceSize)(dy / dst->f.block) * dst->srow +
                          (VkDeviceSize)(dx / dst->f.block) * dst->f.bpe;
        cp.bufferRowLength = dst->srow / dst->f.bpe * dst->f.block;
        cp.bufferImageHeight = dst->sslice / dst->srow * dst->f.block;
        cp.imageSubresource.aspectMask = (src->f.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
                                       ? VK_IMAGE_ASPECT_DEPTH_BIT : src->f.aspect;
        cp.imageSubresource.mipLevel = smip;
        cp.imageSubresource.baseArrayLayer = src->is3d ? 0 : slayer;
        cp.imageSubresource.layerCount = 1;
        cp.imageOffset.x = (int32_t)sx; cp.imageOffset.y = (int32_t)sy; cp.imageOffset.z = (int32_t)sz;
        cp.imageExtent.width = w; cp.imageExtent.height = h; cp.imageExtent.depth = d;
        if (src->f.bpe == dst->f.bpe && !has_stencil(src->f.vk)) {
            vkCmdCopyImageToBuffer(G.cmd, src->img, VK_IMAGE_LAYOUT_GENERAL, dst->sbuf, 1, &cp);
            dst->pending_gpu_write = 1;
        }
    } else if (src->sbuf && dst->img && ssub == 0) {
        VkBufferImageCopy cp;
        memset(&cp, 0, sizeof cp);
        cp.bufferOffset = (VkDeviceSize)sz * src->sslice + (VkDeviceSize)(sy / src->f.block) * src->srow +
                          (VkDeviceSize)(sx / src->f.block) * src->f.bpe;
        cp.bufferRowLength = src->srow / src->f.bpe * src->f.block;
        cp.bufferImageHeight = src->sslice / src->srow * src->f.block;
        cp.imageSubresource.aspectMask = dst->f.aspect;
        cp.imageSubresource.mipLevel = dmip;
        cp.imageSubresource.baseArrayLayer = dst->is3d ? 0 : dlayer;
        cp.imageSubresource.layerCount = 1;
        cp.imageOffset.x = (int32_t)dx; cp.imageOffset.y = (int32_t)dy; cp.imageOffset.z = (int32_t)dz;
        cp.imageExtent.width = w; cp.imageExtent.height = h; cp.imageExtent.depth = d;
        if (src->f.bpe == dst->f.bpe && !has_stencil(dst->f.vk))
            vkCmdCopyBufferToImage(G.cmd, src->sbuf, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    } else if (src->sptr && dst->sptr && ssub == 0 && dsub == 0) {
        uint32_t r, rows = (h + src->f.block - 1) / src->f.block;
        uint32_t bytes = (w + src->f.block - 1) / src->f.block * src->f.bpe;
        for (r = 0; r < rows; r++)
            memcpy(dst->sptr + (dy / dst->f.block + r) * dst->srow + dx / dst->f.block * dst->f.bpe,
                   src->sptr + (sy / src->f.block + r) * src->srow + sx / src->f.block * src->f.bpe,
                   bytes);
    }
    end_transfer();
}

void vk11_ctx_CopySubresourceRegion(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT dsub,
                                    UINT dx, UINT dy, UINT dz, ID3D11Resource *src, UINT ssub,
                                    const D3D11_BOX *box)
{
    Obj *d = (Obj *)dst, *s = (Obj *)src;

    (void)c;
    if (!d || !s)
        return;
    LOCK();
    if (d->kind == K_TEXTURE && s->kind == K_TEXTURE) {
        copy_region((Image *)d, dsub, dx, dy, dz, (Image *)s, ssub, box);
    } else if (d->kind == K_BUFFER && s->kind == K_BUFFER) {
        Buffer *db = (Buffer *)d, *sb = (Buffer *)s;
        VkBuffer sbuf, dbuf;
        VkDeviceSize soff, doff;
        UINT start = box ? box->left : 0, end = box ? box->right : sb->desc.ByteWidth;
        if (end > start && buffer_binding(sb, &sbuf, &soff) && buffer_binding(db, &dbuf, &doff)) {
            VkBufferCopy cp;
            cp.srcOffset = soff + start;
            cp.dstOffset = doff + dx;
            cp.size = end - start;
            if (cp.dstOffset - doff + cp.size <= db->desc.ByteWidth) {
                begin_transfer();
                vkCmdCopyBuffer(G.cmd, sbuf, dbuf, 1, &cp);
                end_transfer();
                if (db->host)
                    db->pending_gpu_write = 1;
            }
        }
    }
    UNLOCK();
}

void vk11_ctx_CopyResource(ID3D11DeviceContext *c, ID3D11Resource *dst, ID3D11Resource *src)
{
    Obj *d = (Obj *)dst, *s = (Obj *)src;

    (void)c;
    if (!d || !s)
        return;
    if (d->kind == K_TEXTURE && s->kind == K_TEXTURE) {
        Image *di = (Image *)d, *si = (Image *)s;
        uint32_t n = si->mips * si->layers, i;
        if (di->sptr || si->sptr)
            n = 1;                      /* staging holds subresource 0 only */
        LOCK();
        for (i = 0; i < n && i < di->mips * di->layers; i++)
            copy_region(di, i, 0, 0, 0, si, i, NULL);
        UNLOCK();
    } else {
        vk11_ctx_CopySubresourceRegion(c, dst, 0, 0, 0, 0, src, 0, NULL);
    }
}

void vk11_ctx_ResolveSubresource(ID3D11DeviceContext *c, ID3D11Resource *dst, UINT dsub,
                                 ID3D11Resource *src, UINT ssub, DXGI_FORMAT f)
{
    Obj *d = (Obj *)dst, *s = (Obj *)src;
    Image *di, *si;
    uint32_t smip, slayer, dmip, dlayer;
    VkImageResolve r;

    (void)f;
    if (!d || !s || d->kind != K_TEXTURE || s->kind != K_TEXTURE)
        return;
    di = (Image *)d;
    si = (Image *)s;
    if (si->samples <= 1 || !si->img || !di->img || di->samples != 1) {
        vk11_ctx_CopySubresourceRegion(c, dst, dsub, 0, 0, 0, src, ssub, NULL);
        return;
    }
    LOCK();
    sub_split(si, ssub, &smip, &slayer);
    sub_split(di, dsub, &dmip, &dlayer);
    memset(&r, 0, sizeof r);
    r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.srcSubresource.mipLevel = smip;
    r.srcSubresource.baseArrayLayer = slayer;
    r.srcSubresource.layerCount = 1;
    r.dstSubresource = r.srcSubresource;
    r.dstSubresource.mipLevel = dmip;
    r.dstSubresource.baseArrayLayer = dlayer;
    r.extent.width = mip_dim(si->w, smip) < mip_dim(di->w, dmip) ? mip_dim(si->w, smip)
                                                                   : mip_dim(di->w, dmip);
    r.extent.height = mip_dim(si->h, smip) < mip_dim(di->h, dmip) ? mip_dim(si->h, smip)
                                                                    : mip_dim(di->h, dmip);
    r.extent.depth = 1;
    begin_transfer();
    vkCmdResolveImage(G.cmd, si->img, VK_IMAGE_LAYOUT_GENERAL, di->img,
                      VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    end_transfer();
    UNLOCK();
}

/* ======================================================================
 * Device and swap chain creation
 * ====================================================================== */

static Image *backbuffer_new(uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;

    memset(&td, 0, sizeof td);
    td.Width = w ? w : 640;
    td.Height = h ? h : 480;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return image_new(0, &td, NULL, NULL);
}

static int create_defaults(void)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SAMPLER_DESC sd;
    VkComponentMapping id = SWZ_ID;

    if (!make_buffer(65536, ALL_BUFFER_USAGE, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     &G.zero_buf, &G.zero_mem, NULL))
        return 0;
    begin_transfer();
    vkCmdFillBuffer(G.cmd, G.zero_buf, 0, VK_WHOLE_SIZE, 0);
    end_transfer();

    /* What an unbound texture slot reads: zeros, as in D3D11. */
    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    G.dummy_tex2d = image_new(0, &td, NULL, NULL);
    if (!G.dummy_tex2d)
        return 0;
    G.dummy_view2d = make_view(G.dummy_tex2d, VK_IMAGE_VIEW_TYPE_2D, G.dummy_tex2d->f.vk,
                               VK_IMAGE_ASPECT_COLOR_BIT, id, 0, 1, 0, 1);
    td.ArraySize = 6;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    G.dummy_cube = image_new(0, &td, NULL, NULL);
    if (!G.dummy_cube)
        return 0;
    G.dummy_viewcube = make_view(G.dummy_cube, VK_IMAGE_VIEW_TYPE_CUBE, G.dummy_cube->f.vk,
                                 VK_IMAGE_ASPECT_COLOR_BIT, id, 0, 1, 0, 6);
    {
        D3D11_TEXTURE3D_DESC t3;
        memset(&t3, 0, sizeof t3);
        t3.Width = t3.Height = t3.Depth = 1;
        t3.MipLevels = 1;
        t3.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t3.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        G.dummy_tex3d = image_new(1, NULL, &t3, NULL);
        if (!G.dummy_tex3d)
            return 0;
        G.dummy_view3d = make_view(G.dummy_tex3d, VK_IMAGE_VIEW_TYPE_3D, G.dummy_tex3d->f.vk,
                                   VK_IMAGE_ASPECT_COLOR_BIT, id, 0, 1, 0, 1);
    }

    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    G.dummy_sampler = VK_NULL_HANDLE;
    G.dummy_sampler = sampler_for(&sd);
    return G.dummy_view2d && G.dummy_viewcube && G.dummy_view3d && G.dummy_sampler;
}

static void at_exit(void)
{
    if (G.ready && G.dev) {
        vkDeviceWaitIdle(G.dev);
        pipeline_cache_save();
    }
}

HRESULT D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *adapter, D3D_DRIVER_TYPE driver_type, HMODULE software,
    UINT flags, const D3D_FEATURE_LEVEL *levels, UINT level_count, UINT sdk,
    const DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swap_chain,
    ID3D11Device **device, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context)
{
    SurfQuery sq;

    (void)adapter; (void)driver_type; (void)software; (void)flags;
    (void)levels; (void)level_count; (void)sdk;
    if (!desc || !desc->OutputWindow)
        return E_INVALIDARG;
    LOCK();
    if (!G.ready) {
        G.window = (SDL_Window *)desc->OutputWindow;
        if (!create_instance())
            goto fail;
        memset(&sq, 0, sizeof sq);
        sq.win = G.window;
        sq.inst = G.instance;
        host_sdl_run_on_event_thread(q_surface, &sq);
        if (!sq.ok) {
            fprintf(stderr, "[VK] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
            goto fail;
        }
        G.surface = sq.surf;
        if (!pick_device() || !create_device() || !create_binding_model() || !create_frames())
            goto fail;
        pipeline_cache_init();
        G.fi = 0;
        G.rec_serial = 1;
        begin_commands();
        if (!create_defaults())
            goto fail;
        G.backbuffer = backbuffer_new(desc->BufferDesc.Width, desc->BufferDesc.Height);
        if (!G.backbuffer)
            goto fail;
        G.present_interval = 0;
        create_swapchain();
        S.blend_factor[0] = S.blend_factor[1] = S.blend_factor[2] = S.blend_factor[3] = 1.0f;
        S.topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        atexit(at_exit);
        G.ready = 1;
        fprintf(stderr, "[VK] renderer up: swap chain %ux%u (%s), back buffer %ux%u\n",
                G.swap_extent.width, G.swap_extent.height,
                G.present_mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" :
                G.present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate",
                G.backbuffer->w, G.backbuffer->h);
        fflush(stderr);
    }
    UNLOCK();
    if (swap_chain)
        *swap_chain = &s_swapchain;
    if (device)
        *device = &s_device;
    if (context)
        *context = &s_context;
    if (level)
        *level = D3D_FEATURE_LEVEL_11_0;
    return S_OK;
fail:
    UNLOCK();
    fprintf(stderr, "[VK] no Vulkan renderer\n");
    fflush(stderr);
    return E_FAIL;
}

HRESULT vk11_swap_GetBuffer(IDXGISwapChain *sc, UINT index, const GUID *riid, void **out)
{
    (void)sc; (void)index; (void)riid;
    if (!out)
        return E_INVALIDARG;
    LOCK();
    *out = G.backbuffer;
    if (G.backbuffer)
        vk11_add_ref(G.backbuffer);
    UNLOCK();
    return *out ? S_OK : E_FAIL;
}

HRESULT vk11_swap_ResizeBuffers(IDXGISwapChain *sc, UINT count, UINT w, UINT h, DXGI_FORMAT f,
                                UINT flags)
{
    Image *nb;

    (void)sc; (void)count; (void)f; (void)flags;
    LOCK();
    if (!w || !h) {
        VkExtent2D e = window_extent();
        if (!w) w = e.width;
        if (!h) h = e.height;
    }
    if (G.backbuffer && G.backbuffer->w == w && G.backbuffer->h == h) {
        UNLOCK();
        return S_OK;
    }
    end_rendering();
    nb = backbuffer_new(w, h);
    if (!nb) {
        UNLOCK();
        return E_FAIL;
    }
    if (G.backbuffer)
        vk11_release(G.backbuffer);
    G.backbuffer = nb;
    G.swap_dirty = 1;
    UNLOCK();
    return S_OK;
}

HRESULT vk11_swap_Present(IDXGISwapChain *sc, UINT interval, UINT flags)
{
    static unsigned frames;
    Frame *f;
    uint32_t idx = UINT32_MAX;
    VkSubmitInfo si;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkResult r;

    (void)sc; (void)flags;
    LOCK();
    if (!G.ready) {
        UNLOCK();
        return E_FAIL;
    }
    f = &G.frames[G.fi];
    end_rendering();
    if ((int)interval != G.present_interval) {
        G.present_interval = (int)interval;
        if (choose_present_mode((int)interval) != G.present_mode)
            G.swap_dirty = 1;
    }
    /* Wayland never reports a resize as out-of-date; look now and then. */
    if (G.swapchain && (++frames % 30) == 0) {
        VkExtent2D e = window_extent();
        if (e.width && e.height &&
            (e.width != G.swap_extent.width || e.height != G.swap_extent.height))
            G.swap_dirty = 1;
    }
    if (!G.swapchain || G.swap_dirty)
        recreate_swapchain();
    if (G.swapchain) {
        r = vkAcquireNextImageKHR(G.dev, G.swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &idx);
        if (r == VK_SUBOPTIMAL_KHR)
            G.swap_dirty = 1;
        else if (r != VK_SUCCESS) {
            if (r == VK_ERROR_OUT_OF_DATE_KHR)
                G.swap_dirty = 1;
            idx = UINT32_MAX;
        }
    }
    if (idx != UINT32_MAX) {
        VkImageBlit bl;
        Image *bb = G.backbuffer;

        if (S.need_barrier)
            barrier_all();
        image_layout(G.swap_images[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = 1;
        bl.srcOffsets[1].x = (int32_t)bb->w;
        bl.srcOffsets[1].y = (int32_t)bb->h;
        bl.srcOffsets[1].z = 1;
        bl.dstSubresource = bl.srcSubresource;
        bl.dstOffsets[1].x = (int32_t)G.swap_extent.width;
        bl.dstOffsets[1].y = (int32_t)G.swap_extent.height;
        bl.dstOffsets[1].z = 1;
        vkCmdBlitImage(G.cmd, bb->img, VK_IMAGE_LAYOUT_GENERAL, G.swap_images[idx],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        image_layout(G.swap_images[idx], VK_IMAGE_ASPECT_COLOR_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        S.need_barrier = 1;
    }
    vkEndCommandBuffer(G.cmd);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &G.cmd;
    if (idx != UINT32_MAX) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f->acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &G.swap_done[idx];
    }
    r = vkQueueSubmit(G.queue, 1, &si, f->fence);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkQueueSubmit failed (%d)%s\n", (int)r,
                r == VK_ERROR_DEVICE_LOST ? ": device lost" : "");
        fflush(stderr);
    }
    f->submitted = r == VK_SUCCESS;
    f->serial = G.rec_serial++;
    if (idx != UINT32_MAX && r == VK_SUCCESS) {
        VkPresentInfoKHR pi;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &G.swap_done[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &G.swapchain;
        pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(G.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
            G.swap_dirty = 1;
    }
    frame_begin((G.fi + 1) % VK11_FRAMES);
    UNLOCK();
    return S_OK;
}
