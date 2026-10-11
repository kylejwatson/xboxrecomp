/*
 * d3d11_vk_internal.h - conventions shared by d3d11_vk.c and
 * d3dcompiler_vk.c. Not for the renderer above them.
 */
#ifndef XBOXRECOMP_D3D11_VK_INTERNAL_H
#define XBOXRECOMP_D3D11_VK_INTERNAL_H

#include "d3d11_vk.h"

/* One descriptor set per draw. HLSL's register spaces each get a range of
 * bindings, so a register number survives the trip, and the two stages'
 * constant buffers get a range each, because D3D11 gives the vertex and the
 * pixel shader their own b0..b7:
 *   vertex b0..b7 -> 0..7, pixel b0..b7 -> 8..15,
 *   t0..t9 -> 16..25, s0..s9 -> 32..41.
 * src/d3d uses b0-b3 and b7, t0-t3, t8 and t9, s0-s3 and s9. */
#define VK11_BINDING_CBUFFER   0u     /* vertex; pixel is + VK11_MAX_CBUFFERS */
#define VK11_BINDING_TEXTURE  16u
#define VK11_BINDING_SAMPLER  32u
#define VK11_MAX_CBUFFERS      8u
#define VK11_MAX_TEXTURES     10u
#define VK11_MAX_SAMPLERS     10u

#endif
