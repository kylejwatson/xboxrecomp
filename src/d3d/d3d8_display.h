/*
 * d3d8_display.h -- the size the host renders at, and the pass that gets
 * that image onto the screen.
 *
 * The guest presents at its own size (640x480 for most titles) and every
 * draw path already ends in normalised coordinates: vertex programs have
 * their screen-space transform undone, the fixed-function path is
 * transformed by the host, and pre-transformed vertices divide by the
 * guest's presentation size. So a larger host target re-rasterises the
 * same scene rather than magnifying it, and nothing needs resampling.
 *
 * That is the whole mechanism behind both supersampling and, later,
 * widescreen: render the scene at a size of the host's choosing, then
 * resolve it down onto a swap chain that still matches the window.
 */
#ifndef XBOXRECOMP_D3D8_DISPLAY_H
#define XBOXRECOMP_D3D8_DISPLAY_H

#include "d3d8_internal.h"


/* Read once, from the environment, before the first device is created.
 *
 * RECOMP_RES_SCALE=<1..8> is the supersample factor. 1 renders at the
 * guest's own size and must be indistinguishable from having none of this
 * code at all. It may instead be a height -- 720p, 1080p, 1440p, 4k --
 * which renders that many lines at the output's shape.
 */
typedef struct D3D8DisplayPolicy {
    UINT scale;
    /* Lines to render, from a height-valued RECOMP_RES_SCALE; 0 means
     * guest height times scale. */
    UINT target_height;
    /* RECOMP_WIDESCREEN: the console is 16:9, and the title's frame is
     * therefore anamorphic -- authored squeezed, to be stretched back out
     * by the display. Only true of a title with a 16:9 mode of its own,
     * which is why it is off by default; a 4:3-only title told this
     * renders 4:3 content that then gets stretched. The kernel reads the
     * same variable to answer XC_VIDEO. */
    int  widescreen;
    /* RECOMP_ANISO=<1..16>, the anisotropy to force on textures the title
     * already filters linearly. 1 leaves every sampler as the title asked
     * for it. */
    UINT anisotropy;
} D3D8DisplayPolicy;

const D3D8DisplayPolicy *d3d8_display_policy(void);

/* The size the scene is rendered at, for a guest presenting at guest_w by
 * guest_h: scaled, and at 16:9 in widescreen (square pixels, not an
 * anamorphic buffer). Equal to the guest's own size when nothing is scaled
 * and the output is 4:3. */
void d3d8_display_scene_size(UINT guest_w, UINT guest_h,
                             UINT *scene_w, UINT *scene_h);

/* Where the scene lands inside a back buffer of bb_w by bb_h: the largest
 * centred rectangle with the scene's own shape. Equal to the whole buffer
 * when the two already agree; otherwise the difference is the bars. */
typedef struct D3D8DisplayFit { UINT x, y, w, h; } D3D8DisplayFit;

D3D8DisplayFit d3d8_display_fit(UINT shape_w, UINT shape_h, UINT bb_w, UINT bb_h);

/* The shape the scene should appear as. Its own: the scene is built at
 * the output shape. Not a size: only the ratio of the two is used. */
void d3d8_display_output_shape(UINT scene_w, UINT scene_h,
                               UINT *shape_w, UINT *shape_h);

/* Draw `scene` into `fit` inside `out`, box filtered, painting everything
 * outside `fit` black so a window of a different shape gets bars rather
 * than a stretched picture. Binds `out` itself and puts back the render
 * target and viewport it found, which is why the caller must not bind the
 * output first: the state this restores is the scene the next frame draws
 * into. */
HRESULT d3d8_display_resolve(ID3D11ShaderResourceView *scene,
                             UINT scene_w, UINT scene_h,
                             ID3D11RenderTargetView *out,
                             D3D8DisplayFit fit);

void d3d8_display_shutdown(void);


#endif /* XBOXRECOMP_D3D8_DISPLAY_H */
