/*
 * hle_d3d8_record.h -- the host boundary of shadow mode, and frame capture
 * recorded at it.
 *
 * Every call src/hle makes into the host renderer (src/d3d) goes through one
 * of the wrappers below. Each one forwards the call unchanged and, while a
 * frame is being captured, also writes it into the capture
 * (d3d8_capture.h). Because the wrappers sit after all of shadow mode's Xbox
 * conversion, a capture holds exactly what the host was given, and replay
 * (src/replay) needs no Xbox knowledge to reproduce it.
 *
 * The rule that keeps this true: nothing in src/hle calls a device vtable
 * entry or a d3d8_vsh_* / d3d8_combiners_* setter directly. A direct call is
 * a call the capture cannot see, and a replay that silently differs from the
 * run. The one exception is reading the back buffer for frame dumps
 * (hle_d3d8.c, shadow_dump_frame), which changes no state.
 *
 * Cost when not capturing: one test of a static pointer per call.
 *
 *   RECOMP_D3D8_CAPTURE=<path>     where to write; capture is off without it
 *   RECOMP_D3D8_CAPTURE_SWAP=<n>   which swap to record (default 120)
 *   RECOMP_D3D8_CAPTURE_EVERY=<n>  then one more every n swaps (n >= 2), to
 *                                  <path>_<swap>.d3dcap, at most 24 files
 *   RECOMP_D3D8_CAPTURE_MINDRAWS=<n>  keep a frame only if it drew at least n
 *                                  times; otherwise try the next swap. Every
 *                                  frame is recorded (a full snapshot each)
 *                                  and deleted while it looks, so it is slow,
 *                                  but it finds a title's rare 3D frames
 *
 * Threads: guest threads are host threads, and the wrappers take no lock.
 * That is no worse than the host renderer itself, which takes none either;
 * shadow mode's calls arrive from whichever guest thread draws. A title that
 * draws from two threads at once would interleave its chunks in the capture
 * in the order the calls happened to be made.
 *
 * Windows only, like the rest of shadow mode.
 */
#ifndef XBOXRECOMP_HLE_D3D8_RECORD_H
#define XBOXRECOMP_HLE_D3D8_RECORD_H

#include <stdint.h>
#include "hle.h"

#if HLE_HOST_RENDERER

#include "d3d8_xbox.h"
#include "d3d8_vsh.h"

/* Called once per Swap, before the host Swap, with the swap counter after it
 * was incremented and the host back buffer's size. This is the capture's only
 * frame boundary: recording starts when the counter reaches the requested
 * swap (with a snapshot of the host's state) and the file is closed at the
 * next one. */
void hle_d3d8_capture_swap(unsigned long swaps, uint32_t width, uint32_t height);

/* True while a frame is being recorded. */
int hle_d3d8_capture_active(void);

/* Capture the next frame, whatever swap it turns out to be. This is the
 * answer to "it looks wrong here": a swap number has to be guessed before
 * the run, and nobody knows in advance which swap they will be standing on
 * when they see it. Without RECOMP_D3D8_CAPTURE the file lands beside the
 * executable. */
void hle_d3d8_capture_next_frame(void);

/* ------------------------------------------------------- deferred frames
 *
 * RECOMP_HLE_D3D8_DEFER=1 (or `defer_draws = 1` in the title's settings
 * file): the shadow renderer queues a frame's device calls instead of making
 * them, and runs the queue at the frame's end (hle_d3d8_defer_flush). That is
 * when the NV2A would reach them: the title's own D3D writes commands into a
 * push buffer the GPU consumes later, so a title may write a texture after
 * queueing the draw that samples it and still be correct on the console.
 * Marvel vs Capcom 2 writes each fighter's new animation tiles that way, and
 * drawing at the call showed the previous pose's tiles for one frame.
 *
 * State and draw calls below are queued with copies of what they point at.
 * Creation, locking and vertex-program declarations run at the call. HLE code
 * whose effect depends on guest memory at execution time -- binding a guest
 * texture, choosing its palette -- queues itself with hle_d3d8_defer_op. */
int  hle_d3d8_defer_on(void);
/* On, and not the thread currently running the queue: calls should queue. */
int  hle_d3d8_defer_recording(void);
/* Queue fn(copy of arg). The copy lives until fn has run. */
void hle_d3d8_defer_op(void (*fn)(const void *arg), const void *arg, size_t size);
/* Run everything queued, in order. Safe to call when nothing is. */
void hle_d3d8_defer_flush(void);

/* Before every draw or clear the host carries out (hle_d3d8.c): the host has
 * one back buffer for the title's two frame buffers, so when drawing moves
 * from one to the other, the picture the first held is kept in a texture for
 * a title that samples that frame buffer later. */
void hle_d3d8_fb_before_write(void);
/* That kept picture, for a frame buffer (physical address) the host back
 * buffer no longer holds; NULL when it does hold it, or nothing was kept. */
IDirect3DTexture8 *hle_d3d8_fb_kept(uint32_t phys);
/* The host size of a frame-buffer texture the guest made w x h: scaled
 * like the scene, so supersampling survives a title sampling its frame. */
void hle_d3d8_fb_host_size(UINT w, UINT h, UINT *host_w, UINT *host_h);
/* Counts draws and clears into a frame buffer; a copy of it taken at an
 * older count is out of date. */
unsigned long hle_d3d8_fb_generation(void);

/* ----------------------------------------------------------- device calls */

HRESULT host_Clear(IDirect3DDevice8 *dev, DWORD count, const D3DRECT *rects,
                   DWORD flags, D3DCOLOR color, float z, DWORD stencil);
/* Not recorded: Swap is the frame boundary, and replay presents on its own. */
HRESULT host_Swap(IDirect3DDevice8 *dev, DWORD flags);
HRESULT host_SetRenderState(IDirect3DDevice8 *dev, D3DRENDERSTATETYPE state,
                            DWORD value);
HRESULT host_SetTextureStageState(IDirect3DDevice8 *dev, DWORD stage,
                                  D3DTEXTURESTAGESTATETYPE type, DWORD value);
HRESULT host_SetTransform(IDirect3DDevice8 *dev, D3DTRANSFORMSTATETYPE state,
                          const D3DMATRIX *matrix);
HRESULT host_SetViewport(IDirect3DDevice8 *dev, const D3DVIEWPORT8 *viewport);
/* Fixed-function lighting: the material and up to d3d8_GetNumLights() lights. */
HRESULT host_SetMaterial(IDirect3DDevice8 *dev, const D3DMATERIAL8 *material);
HRESULT host_SetLight(IDirect3DDevice8 *dev, DWORD index, const D3DLIGHT8 *light);
HRESULT host_LightEnable(IDirect3DDevice8 *dev, DWORD index, BOOL enable);
/* The Xbox scissor (xbox_D3D8SetScissors); the first rectangle is recorded. */
void    host_SetScissors(UINT count, BOOL exclusive, const D3DRECT *rects);
HRESULT host_SetTexture(IDirect3DDevice8 *dev, DWORD stage,
                        IDirect3DBaseTexture8 *texture);
HRESULT host_SetVertexShader(IDirect3DDevice8 *dev, DWORD handle);
HRESULT host_DrawPrimitiveUP(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type,
                             UINT prims, const void *vertices, UINT stride);
HRESULT host_DrawIndexedPrimitiveUP(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type,
                                    UINT min_index, UINT num_vertices, UINT prims,
                                    const void *indices, D3DFORMAT index_format,
                                    const void *vertices, UINT stride);

/* Textures. Creation and locking are not recorded as such: a texture is
 * written whole, from the host's own copy, the first time the capture needs
 * it (bound, or bound at the snapshot). An unlock of a texture the capture
 * already holds re-records that level, and a release retires its id. */
HRESULT host_CreateTexture(IDirect3DDevice8 *dev, UINT width, UINT height,
                           UINT levels, DWORD usage, D3DFORMAT format,
                           D3DPOOL pool, IDirect3DTexture8 **texture);
/* A cube texture. Only the cubes a title renders into are mirrored, so a
 * capture records the creation and not the contents: what is drawn into a
 * face is drawn again on replay. */
HRESULT host_CreateCubeTexture(IDirect3DDevice8 *dev, UINT edge, UINT levels,
                               DWORD usage, D3DFORMAT format, D3DPOOL pool,
                               IDirect3DCubeTexture8 **texture);
HRESULT host_LockRect(IDirect3DTexture8 *texture, UINT level,
                      D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags);
HRESULT host_UnlockRect(IDirect3DTexture8 *texture, UINT level);
ULONG   host_ReleaseTexture(IDirect3DTexture8 *texture);

/* Render targets. texture NULL is the back buffer; otherwise level `level` of
 * a texture created with D3DUSAGE_RENDERTARGET (src/d3d, dev_SetRenderTarget),
 * and `face` names the cube face when that texture is a cube. The surface
 * object for the level or face is taken and released inside.
 *
 * depth NULL is no depth; the device's own depth buffer is
 * host_DeviceDepthSurface. A switch is recorded with the texture and depth
 * surface it names, each written the first time; the depth surface is not
 * recorded at creation. */
HRESULT host_SetRenderTarget(IDirect3DDevice8 *dev, IDirect3DBaseTexture8 *texture,
                             UINT level, UINT face, IDirect3DSurface8 *depth);
HRESULT host_CreateDepthStencilSurface(IDirect3DDevice8 *dev, UINT width, UINT height,
                                       D3DFORMAT format, IDirect3DSurface8 **surface);
/* The device's own depth surface, read once while it is still the one bound
 * (call right after creating the device). A capture names it with
 * D3D8CAP_DEPTH_DEVICE rather than as a depth surface of its own. Not a
 * reference the caller owns. */
IDirect3DSurface8 *host_DeviceDepthSurface(IDirect3DDevice8 *dev);

/* -------------------------------------------- vertex programs, combiners */

HRESULT host_vsh_create_shader(const DWORD *microcode, int insn_count, DWORD *handle);
HRESULT host_vsh_delete_shader(DWORD handle);
/* Whether the host program behind handle holds exactly this microcode. */
BOOL    host_vsh_same_microcode(DWORD handle, const DWORD *microcode, int insn_count);
void    host_vsh_set_constant(int first_reg, const float *data, int count);
HRESULT host_vsh_set_declaration(DWORD handle, const D3D8VshInput *inputs, int count);
void    host_vsh_set_screenspace(const float scale[4], const float offset[4]);
void    host_vsh_set_vertex_data(int reg, const float value[4]);
void    host_combiners_set_pixel_shader(DWORD token);

#endif /* HLE_HOST_RENDERER */

#endif /* XBOXRECOMP_HLE_D3D8_RECORD_H */
