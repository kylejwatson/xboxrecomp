/*
 * d3dcompiler_vk.c - D3DCompile for the Vulkan backend.
 *
 * The renderer's shader generators emit HLSL (d3d8_vsh.c, d3d8_combiners.c,
 * d3d8_shaders.c and the small fixed passes), and they stay as they are:
 * shaderc compiles that HLSL to SPIR-V with glslang's HLSL front end. The
 * conventions that make the result fit d3d11_vk.c's single descriptor set:
 *
 *   - registers keep their numbers inside one binding range each:
 *     b<n> -> VK11_BINDING_CBUFFER + n for a vertex shader and
 *     + VK11_MAX_CBUFFERS + n for a pixel shader, t<n> ->
 *     VK11_BINDING_TEXTURE + n, s<n> -> VK11_BINDING_SAMPLER + n
 *     (d3d11_vk_internal.h);
 *   - cbuffers are packed by HLSL's rules, not std140 (hlsl_offsets);
 *   - every entry-point input and output carries its HLSL semantic as a
 *     UserSemantic decoration (hlsl_functionality1), which is what lets
 *     d3d11_vk.c link stages and input layouts by semantic, as D3D11 does;
 *   - no -fvk-invert-y: the backend flips with a negative viewport height.
 *
 * A compile costs milliseconds and a level brings dozens of new programs, so
 * the SPIR-V is kept on disk, keyed by a hash of everything that went into
 * it: $XDG_CACHE_HOME/xboxrecomp/spirv/ (RECOMP_VK_SHADER_CACHE=0 turns it
 * off, =<dir> moves it).
 */
#include "d3d11_vk_internal.h"
#include "include/d3dcompiler.h"

#include <shaderc/shaderc.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Bump when the options below change, so old cache entries are not used. */
#define SPIRV_CACHE_VERSION 2u

static pthread_once_t   s_once = PTHREAD_ONCE_INIT;
static shaderc_compiler_t s_compiler;
static char             s_cache_dir[512];
static pthread_mutex_t  s_lock = PTHREAD_MUTEX_INITIALIZER;

static void mkdirs(const char *path)
{
    char tmp[512];
    size_t i, n = strlen(path);

    if (n >= sizeof tmp)
        return;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void init_once(void)
{
    const char *env = getenv("RECOMP_VK_SHADER_CACHE");

    s_compiler = shaderc_compiler_initialize();
    if (env && strcmp(env, "0") == 0)
        return;
    if (env && *env) {
        snprintf(s_cache_dir, sizeof s_cache_dir, "%s", env);
    } else {
        const char *xdg = getenv("XDG_CACHE_HOME");
        const char *home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(s_cache_dir, sizeof s_cache_dir, "%s/xboxrecomp/spirv", xdg);
        else if (home && *home)
            snprintf(s_cache_dir, sizeof s_cache_dir, "%s/.cache/xboxrecomp/spirv", home);
    }
    if (s_cache_dir[0])
        mkdirs(s_cache_dir);
}

static uint64_t fnv1a(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static int cache_path(uint64_t key, char *out, size_t n)
{
    if (!s_cache_dir[0])
        return 0;
    snprintf(out, n, "%s/%016llx.spv", s_cache_dir, (unsigned long long)key);
    return 1;
}

static ID3D10Blob *cache_load(uint64_t key)
{
    char path[600];
    FILE *f;
    long size;
    void *buf;
    ID3D10Blob *blob = NULL;

    if (!cache_path(key, path, sizeof path) || !(f = fopen(path, "rb")))
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 20 && (size & 3) == 0 && (buf = malloc((size_t)size)) != NULL) {
        if (fread(buf, 1, (size_t)size, f) == (size_t)size &&
            ((const uint32_t *)buf)[0] == 0x07230203u)
            blob = vk11_blob_create(buf, (SIZE_T)size);
        free(buf);
    }
    fclose(f);
    return blob;
}

static void cache_store(uint64_t key, const void *data, size_t size)
{
    char path[600], tmp[620];
    FILE *f;

    if (!cache_path(key, path, sizeof path))
        return;
    /* Written aside and renamed, so a second process never reads half. */
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
    f = fopen(tmp, "wb");
    if (!f)
        return;
    if (fwrite(data, 1, size, f) == size) {
        fclose(f);
        rename(tmp, path);
    } else {
        fclose(f);
        remove(tmp);
    }
}

HRESULT D3DCompile(const void *src, SIZE_T src_size, LPCSTR source_name,
                   const D3D_SHADER_MACRO *defines, ID3DInclude *include,
                   LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                   ID3DBlob **code, ID3DBlob **errors)
{
    shaderc_compile_options_t opts;
    shaderc_compilation_result_t res;
    shaderc_shader_kind kind;
    uint64_t key = 0xcbf29ce484222325ull;
    uint32_t version = SPIRV_CACHE_VERSION;
    const D3D_SHADER_MACRO *d;
    HRESULT hr = E_FAIL;

    (void)include;
    (void)flags2;
    if (code)
        *code = NULL;
    if (errors)
        *errors = NULL;
    if (!src || !entry || !target || !code)
        return E_INVALIDARG;
    pthread_once(&s_once, init_once);
    if (!s_compiler)
        return E_FAIL;

    if (target[0] == 'v' && target[1] == 's')
        kind = shaderc_vertex_shader;
    else if (target[0] == 'p' && target[1] == 's')
        kind = shaderc_fragment_shader;
    else
        return E_INVALIDARG;

    key = fnv1a(key, &version, sizeof version);
    key = fnv1a(key, src, src_size);
    key = fnv1a(key, entry, strlen(entry) + 1);
    key = fnv1a(key, target, 2);
    for (d = defines; d && d->Name; d++) {
        key = fnv1a(key, d->Name, strlen(d->Name) + 1);
        if (d->Definition)
            key = fnv1a(key, d->Definition, strlen(d->Definition) + 1);
    }

    pthread_mutex_lock(&s_lock);
    if ((*code = cache_load(key)) != NULL) {
        pthread_mutex_unlock(&s_lock);
        return S_OK;
    }

    opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_source_language(opts, shaderc_source_language_hlsl);
    shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan,
                                           shaderc_env_version_vulkan_1_1);
    shaderc_compile_options_set_optimization_level(
        opts, (flags1 & D3DCOMPILE_SKIP_OPTIMIZATION) ? shaderc_optimization_level_zero
                                                      : shaderc_optimization_level_performance);
    shaderc_compile_options_set_hlsl_io_mapping(opts, true);
    shaderc_compile_options_set_hlsl_offsets(opts, true);
    shaderc_compile_options_set_hlsl_functionality1(opts, true);
    shaderc_compile_options_set_auto_bind_uniforms(opts, true);
    shaderc_compile_options_set_binding_base(opts, shaderc_uniform_kind_buffer,
        kind == shaderc_vertex_shader ? VK11_BINDING_CBUFFER
                                      : VK11_BINDING_CBUFFER + VK11_MAX_CBUFFERS);
    shaderc_compile_options_set_binding_base(opts, shaderc_uniform_kind_texture, VK11_BINDING_TEXTURE);
    shaderc_compile_options_set_binding_base(opts, shaderc_uniform_kind_sampler, VK11_BINDING_SAMPLER);
    for (d = defines; d && d->Name; d++)
        shaderc_compile_options_add_macro_definition(opts, d->Name, strlen(d->Name),
                                                     d->Definition ? d->Definition : "1",
                                                     d->Definition ? strlen(d->Definition) : 1);

    res = shaderc_compile_into_spv(s_compiler, (const char *)src, (size_t)src_size, kind,
                                   source_name ? source_name : "shader", entry, opts);
    if (shaderc_result_get_compilation_status(res) == shaderc_compilation_status_success) {
        const char *bytes = shaderc_result_get_bytes(res);
        size_t len = shaderc_result_get_length(res);

        *code = vk11_blob_create(bytes, (SIZE_T)len);
        if (*code) {
            cache_store(key, bytes, len);
            hr = S_OK;
        } else {
            hr = E_OUTOFMEMORY;
        }
    } else {
        const char *msg = shaderc_result_get_error_message(res);
        if (errors && msg)
            *errors = vk11_blob_create(msg, (SIZE_T)strlen(msg) + 1);
        fprintf(stderr, "[VK] D3DCompile(%s, %s, %s) failed:\n%s\n",
                source_name ? source_name : "?", entry, target, msg ? msg : "");
        if (getenv("RECOMP_VK_SHADER_DUMP_FAILED"))
            fprintf(stderr, "---- source ----\n%.*s\n----\n", (int)src_size, (const char *)src);
        fflush(stderr);
        hr = E_FAIL;
    }
    shaderc_result_release(res);
    shaderc_compile_options_release(opts);
    pthread_mutex_unlock(&s_lock);
    return hr;
}
