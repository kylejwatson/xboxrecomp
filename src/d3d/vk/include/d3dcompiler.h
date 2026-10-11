/* <d3dcompiler.h> on a host without Direct3D: D3DCompile compiles HLSL to
 * SPIR-V with shaderc (glslang's HLSL front end); see d3dcompiler_vk.c. */
#ifndef XBOXRECOMP_SHIM_D3DCOMPILER_H
#define XBOXRECOMP_SHIM_D3DCOMPILER_H
#include "../d3d11_vk.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct D3D_SHADER_MACRO { LPCSTR Name, Definition; } D3D_SHADER_MACRO;

#define D3DCOMPILE_DEBUG                 (1u << 0)
#define D3DCOMPILE_SKIP_VALIDATION       (1u << 1)
#define D3DCOMPILE_SKIP_OPTIMIZATION     (1u << 2)
#define D3DCOMPILE_ENABLE_STRICTNESS     (1u << 11)
#define D3DCOMPILE_OPTIMIZATION_LEVEL0   (1u << 14)
#define D3DCOMPILE_OPTIMIZATION_LEVEL1   0u
#define D3DCOMPILE_OPTIMIZATION_LEVEL2   ((1u << 14) | (1u << 15))
#define D3DCOMPILE_OPTIMIZATION_LEVEL3   (1u << 15)

/* code receives SPIR-V, which vk11_dev_Create{Vertex,Pixel}Shader and
 * vk11_dev_CreateInputLayout take as the "bytecode". */
HRESULT D3DCompile(const void *src, SIZE_T src_size, LPCSTR source_name,
                   const D3D_SHADER_MACRO *defines, ID3DInclude *include,
                   LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                   ID3DBlob **code, ID3DBlob **errors);

#ifdef __cplusplus
}
#endif
#endif
