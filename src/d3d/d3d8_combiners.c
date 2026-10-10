/**
 * NV2A Register Combiner to HLSL Pixel Shader Translator - Implementation
 *
 * Translates Xbox NV2A register combiner configurations into HLSL pixel
 * shaders compiled for D3D11. See d3d8_combiners.h for the full model
 * description.
 *
 * Implementation overview:
 *
 * 1. STATE TRACKING
 *    The game sets combiner configuration through either:
 *    (a) SetPixelShader(DWORD token) - a packed DWORD encoding combiner
 *        count and texture modes, with actual stage config in render states
 *    (b) Direct render state writes (D3DRS_PSALPHAINPUTS0..7, etc.)
 *    We parse either path into an NV2ACombinerState structure.
 *
 * 2. HLSL GENERATION
 *    From the combiner state, we emit a complete HLSL pixel shader that:
 *    - Samples textures based on tex_mode per stage
 *    - Walks each active general combiner stage performing AB*CD math
 *    - Executes the final combiner (lerp + add)
 *    - Handles alpha test (fog is the final combiner's own)
 *
 * 3. SHADER CACHE
 *    We hash the full NV2ACombinerState and maintain a fixed-size cache
 *    (128 entries) of compiled ID3D11PixelShader objects. Most Xbox games
 *    use fewer than 20 unique combiner configurations, so this is ample.
 *
 * 4. DRAW INTEGRATION
 *    d3d8_combiners_prepare_draw() is called before each draw. It checks
 *    if state is dirty, rebuilds/looks up the shader, uploads constants,
 *    and binds everything to the D3D11 pipeline.
 */

#include "d3d8_internal.h"
#include "d3d8_combiners.h"
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

#pragma comment(lib, "d3dcompiler.lib")

/* ================================================================
 * Internal State
 * ================================================================ */

/** Current pixel shader token (0 = no combiner shader / fixed-function). */
static DWORD g_ps_token = 0;

/** Current parsed combiner state. */
static NV2ACombinerState g_combiner_state;

/** Dirty flag - set when any PS render state changes. */
static BOOL g_dirty = TRUE;

/** PS constant buffer (uploaded to GPU each draw). */
static ID3D11Buffer *g_combiner_cb = NULL;

/* ================================================================
 * Shader Cache
 *
 * Simple open-addressing hash table with linear probing.
 * 128 entries is generous - most games use <20 unique PS configs.
 * On a full table, the oldest entry is evicted (LRU approximation
 * via frame counter).
 * ================================================================ */

#define COMBINER_CACHE_SIZE 128

typedef struct CombinerCacheEntry {
    BOOL                in_use;
    uint32_t            hash;
    NV2ACombinerState   state;
    ID3D11PixelShader  *shader;
    uint32_t            last_used_frame;
} CombinerCacheEntry;

static CombinerCacheEntry g_cache[COMBINER_CACHE_SIZE];
static uint32_t g_frame_counter = 0;

/* ================================================================
 * Hashing
 *
 * FNV-1a over the combiner state structure. This is fast enough
 * for our purposes and produces good distribution.
 * ================================================================ */

static uint32_t fnv1a_hash(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 0x811C9DC5u;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

/* Only the structural part of the state is the key: the constant colours
 * that follow it are uploaded per draw and are not in the HLSL, and keying
 * on them made every distinct colour a new entry and a new D3DCompile in a
 * 128-entry table that then evicted and recompiled the same shaders in
 * steady state. */
static uint32_t combiner_state_hash(const NV2ACombinerState *state)
{
    return fnv1a_hash(state, NV2A_COMBINER_KEY_BYTES);
}

static BOOL combiner_state_equal(const NV2ACombinerState *a,
                                 const NV2ACombinerState *b)
{
    return memcmp(a, b, NV2A_COMBINER_KEY_BYTES) == 0;
}

/* How many generated shaders RECOMP_D3D8_PS_DUMP asks to see, and whether it
 * was asked for at all. The switch used to be a flag that meant "two"; a
 * count is what it always wanted to be, and a bare RECOMP_D3D8_PS_DUMP=1
 * still gives a useful dump, just of one shader rather than two. */
static int ps_dump_limit(void)
{
    const char *v = getenv("RECOMP_D3D8_PS_DUMP");
    int n;

    if (!v || !*v)
        return 0;
    n = atoi(v);
    return n > 0 ? n : 2;
}

/* The shader for g_combiner_state as last parsed. The lookup hashes ~1.5 KB
 * per call and was the largest host-side symbol in TimeSplitters 2's
 * profile; the state only changes when a PS render state does, so the
 * result is kept until the next parse. Only this path calls the lookup in
 * the runtime, so the entry cannot be evicted while it is held. */
static ID3D11PixelShader *g_last_shader;

/* ================================================================
 * Color Helpers
 *
 * Convert D3DCOLOR (ARGB packed DWORD) to float4 (RGBA).
 * D3DCOLOR byte layout in memory: BGRA (little-endian ARGB).
 * ================================================================ */

static void d3dcolor_to_float4(DWORD color, float out[4])
{
    out[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    out[1] = ((color >>  8) & 0xFF) / 255.0f; /* G */
    out[2] = ((color >>  0) & 0xFF) / 255.0f; /* B */
    out[3] = ((color >> 24) & 0xFF) / 255.0f; /* A */
}

/* ================================================================
 * Combiner Input Parsing
 *
 * Each combiner input is packed as 8 bits in the render state DWORDs:
 *   [3:0] register select (NV2ACombinerRegister value)
 *   [4]   alpha channel replicate
 *   [7:5] input mapping mode (NV2AInputMapping value)
 * ================================================================ */

static void parse_combiner_input(DWORD packed, NV2ACombinerInput *input)
{
    input->reg       = (NV2ACombinerRegister)(packed & 0xF);
    input->alpha_rep = (packed >> 4) & 1;
    input->mapping   = (NV2AInputMapping)((packed >> 5) & 0x7);
}

/**
 * Parse a 32-bit input register DWORD containing 4 packed inputs.
 *
 * The Xbox packs A first, in the most significant byte:
 *   PS_COMBINERINPUTS(a,b,c,d) = ((a)<<24)|((b)<<16)|((c)<<8)|(d)
 * (Cxbx-Reloaded, XbPixelShader.h). Layout: [31:24]=A [23:16]=B [15:8]=C
 * [7:0]=D.
 *
 * This read them the other way round, exchanging A with D and B with C in
 * every stage and in the final combiner. A stage whose other pair is zero
 * survives that, because AB + CD is symmetric, which is why textured
 * geometry still looked roughly right while the final combiner did not.
 */
static void parse_four_inputs(DWORD dword, NV2ACombinerInput inputs[4])
{
    parse_combiner_input((dword >> 24) & 0xFF, &inputs[0]); /* A */
    parse_combiner_input((dword >> 16) & 0xFF, &inputs[1]); /* B */
    parse_combiner_input((dword >>  8) & 0xFF, &inputs[2]); /* C */
    parse_combiner_input((dword >>  0) & 0xFF, &inputs[3]); /* D */
}

/**
 * Parse a 32-bit output configuration DWORD for one channel.
 *
 * Output DWORD layout (the PS_COMBINEROUTPUT_* flags sit from bit 12):
 *   [3:0]   CD destination register
 *   [7:4]   AB destination register
 *   [11:8]  SUM destination register
 *   [12]    CD dot product flag         (CD_DOT_PRODUCT 0x01)
 *   [13]    AB dot product flag         (AB_DOT_PRODUCT 0x02)
 *   [14]    mux instead of sum          (AB_CD_MUX 0x04)
 *   [17:15] output mapping (scale/bias) (OUTPUTMAPPING_* 0x08-0x38)
 *   [18]    CD blue to alpha            (CD_BLUE_TO_ALPHA 0x40) - not handled
 *   [19]    AB blue to alpha            (AB_BLUE_TO_ALPHA 0x80) - not handled
 */
static void parse_output(DWORD dword, NV2ACombinerOutput *output)
{
    /* PS_COMBINEROUTPUTS(ab,cd,mux_sum,flags) =
     *     ((flags)<<12)|((mux_sum)<<8)|((ab)<<4)|(cd)
     * (Cxbx-Reloaded, XbPixelShader.h), so CD is the low nibble and AB the
     * next one. These two were the other way round here. */
    output->cd_dst     = (NV2ACombinerRegister)((dword >>  0) & 0xF);
    output->ab_dst     = (NV2ACombinerRegister)((dword >>  4) & 0xF);
    output->sum_dst    = (NV2ACombinerRegister)((dword >>  8) & 0xF);
    output->cd_dot     = (dword >> 12) & 1;
    output->ab_dot     = (dword >> 13) & 1;
    output->mux_sum    = (dword >> 14) & 1;
    output->output_map = (NV2AOutputMapping)((dword >> 15) & 0x7);
}

/* ================================================================
 * Token & Render State Parsing
 * ================================================================ */

void d3d8_combiners_parse_token(DWORD token, const DWORD *rs,
                                NV2ACombinerState *state)
{
    int i;
    memset(state, 0, sizeof(*state));

    /* Bits [3:0]: number of active combiner stages (1-8) */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;

    /* Bits [8:23]: texture mode per stage (4 bits each) */
    state->tex_mode[0] = (NV2ATextureMode)((token >>  8) & 0xF);
    state->tex_mode[1] = (NV2ATextureMode)((token >> 12) & 0xF);
    state->tex_mode[2] = (NV2ATextureMode)((token >> 16) & 0xF);
    state->tex_mode[3] = (NV2ATextureMode)((token >> 20) & 0xF);

    /* Bits [24:31]: dot mapping and other flags */
    state->flags = (token >> 24) & 0xFF;

    /* Parse per-stage inputs and outputs from render states */
    d3d8_combiners_from_render_states(rs, state);

    /* Preserve the token-derived fields (from_render_states may overwrite).
     *
     * The texture modes come back only when the token actually carries them.
     * A token with all its mode bits clear means "the modes are in
     * D3DRS_PSTEXTUREMODES", which from_render_states has just read; putting
     * the zeros back defeated that path entirely and left every stage
     * sampling a 2D texture, including the stages with nothing bound. */
    state->num_stages = token & 0xF;
    if (state->num_stages < 1) state->num_stages = 1;
    if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
        state->num_stages = NV2A_MAX_COMBINER_STAGES;
    if (((token >> 8) & 0xFFFF) != 0) {
        state->tex_mode[0] = (NV2ATextureMode)((token >>  8) & 0xF);
        state->tex_mode[1] = (NV2ATextureMode)((token >> 12) & 0xF);
        state->tex_mode[2] = (NV2ATextureMode)((token >> 16) & 0xF);
        state->tex_mode[3] = (NV2ATextureMode)((token >> 20) & 0xF);
    }
    state->flags = (token >> 24) & 0xFF;
}

void d3d8_combiners_from_render_states(const DWORD *rs,
                                       NV2ACombinerState *state)
{
    int i;

    /*
     * If called standalone (not from parse_token), read combiner count
     * from D3DRS_PSCOMBINERCOUNT render state.
     *
     * D3DRS_PSCOMBINERCOUNT layout:
     *   [3:0] number of stages
     *   [8]   unique C0 per stage (1) vs shared (0)
     *   [9]   unique C1 per stage (1) vs shared (0)
     *   [16]  mux_MSB for final combiner (not commonly used)
     */
    if (state->num_stages == 0) {
        state->num_stages = rs[D3DRS_PSCOMBINERCOUNT] & 0xF;
        if (state->num_stages < 1) state->num_stages = 1;
        if (state->num_stages > NV2A_MAX_COMBINER_STAGES)
            state->num_stages = NV2A_MAX_COMBINER_STAGES;
    }

    /* Parse stage inputs */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        parse_four_inputs(rs[D3DRS_PSRGBINPUTS0 + i],
                          state->stages[i].rgb_input);
        parse_four_inputs(rs[D3DRS_PSALPHAINPUTS0 + i],
                          state->stages[i].alpha_input);
    }

    /* Parse stage outputs */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        parse_output(rs[D3DRS_PSRGBOUTPUTS0 + i],
                     &state->stages[i].rgb_output);
        parse_output(rs[D3DRS_PSALPHAOUTPUTS0 + i],
                     &state->stages[i].alpha_output);
    }

    /*
     * Parse final combiner inputs.
     *
     * D3DRS_PSFINALCOMBINERINPUTSABCD packs inputs A,B,C,D as 8 bits each:
     *   [7:0]=A  [15:8]=B  [23:16]=C  [31:24]=D
     *
     * D3DRS_PSFINALCOMBINERINPUTSEFG packs E,F,G:
     *   [7:0]=E  [15:8]=F  [23:16]=G  [31:24]=reserved
     */
    {
        DWORD abcd = rs[D3DRS_PSFINALCOMBINERINPUTSABCD];
        DWORD efg  = rs[D3DRS_PSFINALCOMBINERINPUTSEFG];

        /* Same packing as every other input DWORD: the first input is in the
         * most significant byte. Read low-first, Burnout 2's fog lerp
         * (A = fog alpha, B = r0, C = fog colour, D = zero) came out as
         * "r0 + fog alpha", which saturates to white, and G -- the final
         * alpha -- was read as zero.
         *
         * The low byte of EFG is PS_FINALCOMBINERSETTING (CLAMP_SUM 0x80,
         * COMPLEMENT_V1 0x40, COMPLEMENT_R0 0x20), which is not applied yet: a
         * title relying on those flags gets the uncomplemented, unclamped
         * value. */
        /* A shader that never programs the final combiner leaves both words
         * zero, and every one of TimeSplitters 2's front-end shaders does
         * (count 0x11101, one stage writing r0). Read literally that is
         * A = B = C = D = ZERO and G = ZERO: out = 0 + 0*0 + 1*0 = black with
         * alpha 0, which is what the shadow renderer drew -- a black frame
         * over a progress bar the fixed-function path showed plainly. The
         * console's D3D gives an unprogrammed final combiner the pass-through
         * the shader assembler documents as its default, `xfc r0.a, zero,
         * zero, zero, zero, zero, r0`: colour D = r0, alpha G = r0.a. Do the
         * same. A title that wants black writes ZERO into D explicitly, which
         * is a different word from "nothing written". */
        if (getenv("RECOMP_D3D8_PS_DUMP")) {
            static DWORD seen[32];
            static int nseen;
            int k;
            for (k = 0; k < nseen && seen[k] != (abcd ^ (efg * 31u)); k++)
                ;
            if (k == nseen && nseen < 32) {
                seen[nseen++] = abcd ^ (efg * 31u);
                fprintf(stderr, "NV2A combiners: final combiner words ABCD 0x%08lX EFG 0x%08lX\n",
                        (unsigned long)abcd, (unsigned long)efg);
            }
        }
        if (abcd == 0 && efg == 0) {
            abcd = 0x0000000Cu;              /* D = R0 */
            efg  = 0x00001C00u;              /* G = R0 alpha */
        }
        parse_combiner_input((abcd >> 24) & 0xFF, &state->final_input[0]); /* A */
        parse_combiner_input((abcd >> 16) & 0xFF, &state->final_input[1]); /* B */
        parse_combiner_input((abcd >>  8) & 0xFF, &state->final_input[2]); /* C */
        parse_combiner_input((abcd >>  0) & 0xFF, &state->final_input[3]); /* D */
        parse_combiner_input((efg  >> 24) & 0xFF, &state->final_input[4]); /* E */
        parse_combiner_input((efg  >> 16) & 0xFF, &state->final_input[5]); /* F */
        parse_combiner_input((efg  >>  8) & 0xFF, &state->final_input[6]); /* G */
    }

    /* Per-stage constant colors */
    for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
        state->c0[i] = rs[D3DRS_PSCONSTANT0_0 + i];
        state->c1[i] = rs[D3DRS_PSCONSTANT1_0 + i];
    }

    /* Final combiner uses the constants from the last active stage, or
     * can use its own - for now, store them separately. Games typically
     * share them with the last stage. */
    state->final_c0 = state->c0[state->num_stages > 0 ? state->num_stages - 1 : 0];
    state->final_c1 = state->c1[state->num_stages > 0 ? state->num_stages - 1 : 0];

    /* Read texture modes from render state if not already set by token */
    if (state->tex_mode[0] == 0 && state->tex_mode[1] == 0 &&
        state->tex_mode[2] == 0 && state->tex_mode[3] == 0) {
        DWORD tm = rs[D3DRS_PSTEXTUREMODES];
        state->tex_mode[0] = (NV2ATextureMode)((tm >>  0) & 0xF);
        state->tex_mode[1] = (NV2ATextureMode)((tm >>  4) & 0xF);
        state->tex_mode[2] = (NV2ATextureMode)((tm >>  8) & 0xF);
        state->tex_mode[3] = (NV2ATextureMode)((tm >> 12) & 0xF);
    }
}

/* ================================================================
 * HLSL Code Generation
 *
 * Strategy: build the shader string via snprintf into a large buffer.
 * Each section appends to a running offset. This is not the prettiest
 * approach but it's straightforward, debuggable, and has zero
 * external dependencies.
 *
 * Generated shader structure:
 *   1. Texture sampler declarations
 *   2. Constant buffer (matches NV2APSConstants layout)
 *   3. Input struct (SV_POSITION, COLOR0, COLOR1, TEXCOORD0-3)
 *   4. Input mapping helper function
 *   5. Output mapping helper function
 *   6. main():
 *      a. Initialize register file from inputs
 *      b. Execute each general combiner stage
 *      c. Execute final combiner (fog is its A/B/C inputs; the NV2A has no
 *         fog stage after it)
 *      d. Apply alpha test
 *      e. Return result
 * ================================================================ */

/**
 * Emit HLSL to append a string to the output buffer.
 * Returns new offset, or -1 if buffer overflow.
 */
#define EMIT(fmt, ...) do { \
    int _n = snprintf(buf + off, bufsize - off, fmt, ##__VA_ARGS__); \
    if (_n < 0 || off + _n >= bufsize) return -1; \
    off += _n; \
} while (0)

/**
 * Get the HLSL variable name for a register in the NV2A register file.
 *
 * The register file is represented as local float4 variables in the
 * generated shader. This returns the name used in the HLSL code.
 */
static const char *reg_name(NV2ACombinerRegister reg)
{
    switch (reg) {
    case NV2A_REG_ZERO:     return "r_zero";
    case NV2A_REG_C0:       return "r_c0";
    case NV2A_REG_C1:       return "r_c1";
    case NV2A_REG_FOG:      return "r_fog";
    case NV2A_REG_V0:       return "r_v0";
    case NV2A_REG_V1:       return "r_v1";
    case NV2A_REG_T0:       return "r_t0";
    case NV2A_REG_T1:       return "r_t1";
    case NV2A_REG_T2:       return "r_t2";
    case NV2A_REG_T3:       return "r_t3";
    case NV2A_REG_R0:       return "r_r0";
    case NV2A_REG_R1:       return "r_r1";
    case NV2A_REG_EF_PROD:  return "r_ef";
    case NV2A_REG_V1R0_SUM: return "r_v1r0sum";
    default:                return "r_zero";
    }
}

/**
 * Emit HLSL expression for reading a combiner input.
 *
 * An input consists of:
 *   1. Register selection (which variable to read)
 *   2. Channel selection (full RGBA or alpha-replicated)
 *   3. Mapping function (how to transform the value)
 *
 * For alpha-replicate: .aaaa swizzle
 * For normal RGB read in RGB path: .rgb (or .rgba for alpha path)
 *
 * The mapping function is applied inline as an arithmetic expression.
 *
 * @param suffix  ".rgb" for RGB path, ".a" for alpha path (determines swizzle)
 */
static void emit_mapped_input(char *buf, int bufsize, int *off,
                               const NV2ACombinerInput *input,
                               const char *suffix, int stage_idx)
{
    const char *rn;
    char swizzle[8];
    char base_expr[128];
    int n;

    rn = reg_name(input->reg);

    /* For per-stage C0/C1, use the stage-indexed constant */
    if (input->reg == NV2A_REG_C0) {
        snprintf(base_expr, sizeof(base_expr), "c0[%d]", stage_idx);
    } else if (input->reg == NV2A_REG_C1) {
        snprintf(base_expr, sizeof(base_expr), "c1[%d]", stage_idx);
    } else {
        snprintf(base_expr, sizeof(base_expr), "%s", rn);
    }

    /* Determine swizzle based on alpha replicate and target channel.
     *
     * The bit selects the channel, and what "not alpha" means depends on the
     * portion: in the RGB portion it is rgb, in the ALPHA portion it is the
     * register's BLUE component, not its alpha (NV_register_combiners, and
     * Cxbx-Reloaded XbPixelShader.cpp, which emits .b there). Reading alpha
     * for those inputs collapsed Burnout 2's alpha chains to zero, so its
     * car, HUD and fade -- every blended draw -- disappeared with the
     * combiners on. */
    if (input->alpha_rep) {
        /* Alpha replicate: use .aaa for RGB, .a for alpha */
        if (strcmp(suffix, ".a") == 0)
            snprintf(swizzle, sizeof(swizzle), ".a");
        else
            snprintf(swizzle, sizeof(swizzle), ".aaa");
    } else if (strcmp(suffix, ".a") == 0) {
        snprintf(swizzle, sizeof(swizzle), ".b");
    } else {
        snprintf(swizzle, sizeof(swizzle), "%s", suffix);
    }

    /* Build the full variable reference */
    char var_ref[160];
    snprintf(var_ref, sizeof(var_ref), "%s%s", base_expr, swizzle);

    /* Apply input mapping */
    switch (input->mapping) {
    case NV2A_MAP_UNSIGNED_IDENTITY:
        /* x - passthrough */
        n = snprintf(buf + *off, bufsize - *off, "max(%s, 0.0)", var_ref);
        break;
    case NV2A_MAP_UNSIGNED_INVERT:
        /* 1 - x, clamped to [0,1] */
        n = snprintf(buf + *off, bufsize - *off,
                     "max(1.0 - %s, 0.0)", var_ref);
        break;
    case NV2A_MAP_EXPAND_NORMAL:
        /* 2x - 1 */
        n = snprintf(buf + *off, bufsize - *off,
                     "(2.0 * max(%s, 0.0) - 1.0)", var_ref);
        break;
    case NV2A_MAP_EXPAND_NEGATE:
        /* 1 - 2x */
        n = snprintf(buf + *off, bufsize - *off,
                     "(1.0 - 2.0 * max(%s, 0.0))", var_ref);
        break;
    case NV2A_MAP_HALFBIAS_NORMAL:
        /* x - 0.5 */
        n = snprintf(buf + *off, bufsize - *off,
                     "(max(%s, 0.0) - 0.5)", var_ref);
        break;
    case NV2A_MAP_HALFBIAS_NEGATE:
        /* 0.5 - x */
        n = snprintf(buf + *off, bufsize - *off,
                     "(0.5 - max(%s, 0.0))", var_ref);
        break;
    case NV2A_MAP_SIGNED_IDENTITY:
        /* x (allow negative values) */
        n = snprintf(buf + *off, bufsize - *off, "%s", var_ref);
        break;
    case NV2A_MAP_SIGNED_NEGATE:
        /* -x */
        n = snprintf(buf + *off, bufsize - *off, "(-%s)", var_ref);
        break;
    default:
        n = snprintf(buf + *off, bufsize - *off, "%s", var_ref);
        break;
    }
    if (n > 0) *off += n;
}

/**
 * Emit HLSL expression for the output mapping (scale/bias).
 */
static const char *output_map_prefix(NV2AOutputMapping map)
{
    switch (map) {
    case NV2A_OUT_IDENTITY:         return "";
    case NV2A_OUT_BIAS:             return "(";
    case NV2A_OUT_SHIFTLEFT_1:      return "(";
    case NV2A_OUT_SHIFTLEFT_1_BIAS: return "((";
    case NV2A_OUT_SHIFTLEFT_2:      return "(";
    case NV2A_OUT_SHIFTLEFT_2_BIAS: return "((";
    case NV2A_OUT_SHIFTRIGHT_1:     return "(";
    case NV2A_OUT_SHIFTRIGHT_1_BIAS: return "((";
    default:                        return "";
    }
}

static const char *output_map_suffix(NV2AOutputMapping map)
{
    switch (map) {
    case NV2A_OUT_IDENTITY:         return "";
    case NV2A_OUT_BIAS:             return " - 0.5)";
    case NV2A_OUT_SHIFTLEFT_1:      return " * 2.0)";
    case NV2A_OUT_SHIFTLEFT_1_BIAS: return " - 0.5) * 2.0)";
    case NV2A_OUT_SHIFTLEFT_2:      return " * 4.0)";
    case NV2A_OUT_SHIFTLEFT_2_BIAS: return " - 0.5) * 4.0)";
    case NV2A_OUT_SHIFTRIGHT_1:     return " * 0.5)";
    case NV2A_OUT_SHIFTRIGHT_1_BIAS: return " - 0.5) * 0.5)";
    default:                        return "";
    }
}

int d3d8_combiners_generate_hlsl(const NV2ACombinerState *state,
                                 char *buf, int bufsize)
{
    int off = 0;
    int i;

    /* ---- Texture samplers ---- */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (state->tex_mode[i] != NV2A_TEXMODE_NONE) {
            if (state->tex_mode[i] == NV2A_TEXMODE_CUBEMAP) {
                EMIT("TextureCube  tex%d : register(t%d);\n", i, i);
            } else if (state->tex_mode[i] == NV2A_TEXMODE_3D) {
                EMIT("Texture3D    tex%d : register(t%d);\n", i, i);
            } else {
                EMIT("Texture2D    tex%d : register(t%d);\n", i, i);
            }
            EMIT("SamplerState samp%d : register(s%d);\n", i, i);
        }
    }
    EMIT("\n");

    /* ---- Constant buffer ---- */
    EMIT("cbuffer CombinerCB : register(b0) {\n");
    EMIT("    float4 c0[8];\n");    /* Per-stage C0 */
    EMIT("    float4 c1[8];\n");    /* Per-stage C1 */
    EMIT("    float4 fc0;\n");      /* Final combiner C0 */
    EMIT("    float4 fc1;\n");      /* Final combiner C1 */
    EMIT("    float4 fog_color;\n");
    EMIT("    float  alpha_ref;\n");
    EMIT("    uint   alpha_func;\n");
    EMIT("    uint   alpha_test_enable;\n");
    EMIT("    uint   fog_enable;\n");
    EMIT("    uint4  alpha_only;\n");
    EMIT("    float4 tex_scale[4];\n");   /* texel -> normalised, linear textures */
    EMIT("};\n\n");

    /* ---- Input structure ---- */
    EMIT("struct PS_IN {\n");
    EMIT("    float4 pos     : SV_POSITION;\n");
    EMIT("    float4 color0  : COLOR0;\n");
    EMIT("    float4 color1  : COLOR1;\n");
    EMIT("    float3 tc0     : TEXCOORD0;\n");
    EMIT("    float3 tc1     : TEXCOORD1;\n");
    EMIT("    float3 tc2     : TEXCOORD2;\n");
    EMIT("    float3 tc3     : TEXCOORD3;\n");
    /* The fog register's alpha is the fog factor the vertex stage
     * interpolated, which both vertex paths write to TEXCOORD4
     * (d3d8_shaders.c for fixed function, d3d8_vsh.c for a program). */
    EMIT("    float  fog     : TEXCOORD4;\n");
    EMIT("};\n\n");

    /* ---- Main function ---- */
    EMIT("float4 main(PS_IN input) : SV_TARGET {\n");

    /* Initialize register file */
    EMIT("    /* Register file initialization */\n");
    EMIT("    float4 r_zero = float4(0, 0, 0, 0);\n");
    EMIT("    float4 r_c0   = c0[0];\n");
    EMIT("    float4 r_c1   = c1[0];\n");
    /* The NV2A fog register is the fog colour in rgb and the interpolated fog
     * factor in alpha. Taking alpha from the fog colour instead made the
     * final combiner's usual lerp -- A = fog alpha, B = shaded colour,
     * C = fog colour -- constant across the frame, which washed the whole
     * image in fog colour or saturated it to white.
     *
     * With fog disabled the factor reads 1 (no fog): the XDK leaves the fog
     * lerp in the final combiner either way, so taking the interpolated value
     * regardless let a vertex stage that writes no fog blank the whole draw to
     * the fog colour -- Burnout 2's menus came out black that way. */
    /* "No fog" depends on which way round the title wrote the lerp. The XDK's
     * own form is A = fog alpha, B = r0, C = fog colour: the factor is how
     * much of the shaded colour survives, and 1 is no fog. TimeSplitters:
     * Future Perfect writes the opposite, A = fog alpha, B = fog colour,
     * C = r0 (final combiner word 0x13030C00): the factor is how much fog
     * there is, and 1 painted its whole world in the fog colour. So with fog
     * disabled the factor is whichever value leaves the shaded colour. */
    {
        const NV2ACombinerInput *fb = &state->final_input[1], *fc = &state->final_input[2];
        int fog_is_amount = fb->reg == NV2A_REG_FOG && !fb->alpha_rep &&
                            fc->reg != NV2A_REG_FOG;

        EMIT("    float4 r_fog  = float4(fog_color.rgb, fog_enable ? input.fog : %s);\n",
             fog_is_amount ? "0.0" : "1.0");
    }

    /* Vertex colors: Xbox D3DCOLOR is BGRA in memory, the vertex shader
     * should have already swizzled to RGBA. */
    EMIT("    float4 r_v0   = input.color0;\n");
    EMIT("    float4 r_v1   = input.color1;\n");

    /* Texture samples */
    for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
        if (state->tex_mode[i] == NV2A_TEXMODE_NONE) {
            EMIT("    float4 r_t%d = float4(0, 0, 0, 0);\n", i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_CUBEMAP) {
            /* Cube map: use the full 3-component reflection/TCI vector */
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d);\n",
                 i, i, i, i);
        } else if (state->tex_mode[i] == NV2A_TEXMODE_3D) {
            /* 3D texture: use the full 3-component coordinate */
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d);\n",
                 i, i, i, i);
        } else {
            EMIT("    float4 r_t%d = tex%d.Sample(samp%d, input.tc%d.xy * tex_scale[%d].xy);\n",
                 i, i, i, i, i);
        }
        /* Preserve disabled stages and sampled alpha. */
        if (state->tex_mode[i] != NV2A_TEXMODE_NONE)
            EMIT("    if (alpha_only[%d]) r_t%d.rgb = 1.0;\n", i, i);
    }

    /* Temporary registers: R0 initialized to T0 (NV2A convention),
     * R1 initialized to zero */
    EMIT("    float4 r_r0 = r_t0;\n");
    EMIT("    float4 r_r1 = float4(0, 0, 0, 0);\n\n");

    /* ---- General combiner stages ---- */
    for (i = 0; i < state->num_stages; i++) {
        const NV2ACombinerInput *rgb_in  = state->stages[i].rgb_input;
        const NV2ACombinerInput *alpha_in = state->stages[i].alpha_input;
        const NV2ACombinerOutput *rgb_out = &state->stages[i].rgb_output;
        const NV2ACombinerOutput *alpha_out = &state->stages[i].alpha_output;

        EMIT("    /* ---- Stage %d ---- */\n", i);

        /* Update per-stage constants (only if this stage uses C0/C1) */
        EMIT("    r_c0 = c0[%d];\n", i);
        EMIT("    r_c1 = c1[%d];\n", i);

        /*
         * RGB path: compute AB and CD products
         *
         * AB_rgb = map(A) * map(B)    (component-wise, or dot3 if ab_dot)
         * CD_rgb = map(C) * map(D)    (component-wise, or dot3 if cd_dot)
         */
        EMIT("    {\n");

        /* AB product */
        EMIT("        float3 a_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[0], ".rgb", i);
        EMIT(";\n");
        EMIT("        float3 b_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[1], ".rgb", i);
        EMIT(";\n");

        if (rgb_out->ab_dot) {
            EMIT("        float3 ab_rgb = float3(dot(a_rgb, b_rgb), "
                 "dot(a_rgb, b_rgb), dot(a_rgb, b_rgb));\n");
        } else {
            EMIT("        float3 ab_rgb = a_rgb * b_rgb;\n");
        }

        /* CD product */
        EMIT("        float3 c_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[2], ".rgb", i);
        EMIT(";\n");
        EMIT("        float3 d_rgb = ");
        emit_mapped_input(buf, bufsize, &off, &rgb_in[3], ".rgb", i);
        EMIT(";\n");

        if (rgb_out->cd_dot) {
            EMIT("        float3 cd_rgb = float3(dot(c_rgb, d_rgb), "
                 "dot(c_rgb, d_rgb), dot(c_rgb, d_rgb));\n");
        } else {
            EMIT("        float3 cd_rgb = c_rgb * d_rgb;\n");
        }

        /* Sum or mux */
        if (rgb_out->mux_sum) {
            /* MUX: select AB if R0.a >= 0.5, else CD */
            EMIT("        float3 sum_rgb = (r_r0.a >= 0.5) ? ab_rgb : cd_rgb;\n");
        } else {
            EMIT("        float3 sum_rgb = ab_rgb + cd_rgb;\n");
        }

        /* Apply output mapping (scale/bias) */
        const char *omp = output_map_prefix(rgb_out->output_map);
        const char *oms = output_map_suffix(rgb_out->output_map);

        /* Write to destination registers */
        if (rgb_out->ab_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = %sab_rgb%s;\n",
                 reg_name(rgb_out->ab_dst), omp, oms);
        }
        if (rgb_out->cd_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = %scd_rgb%s;\n",
                 reg_name(rgb_out->cd_dst), omp, oms);
        }
        if (rgb_out->sum_dst != NV2A_REG_ZERO) {
            EMIT("        %s.rgb = %ssum_rgb%s;\n",
                 reg_name(rgb_out->sum_dst), omp, oms);
        }

        EMIT("    }\n");

        /*
         * Alpha path: same structure but scalar operations.
         * Uses .a swizzle for all reads/writes.
         */
        EMIT("    {\n");

        EMIT("        float a_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[0], ".a", i);
        EMIT(";\n");
        EMIT("        float b_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[1], ".a", i);
        EMIT(";\n");
        EMIT("        float ab_a = a_a * b_a;\n");

        EMIT("        float c_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[2], ".a", i);
        EMIT(";\n");
        EMIT("        float d_a = ");
        emit_mapped_input(buf, bufsize, &off, &alpha_in[3], ".a", i);
        EMIT(";\n");
        EMIT("        float cd_a = c_a * d_a;\n");

        if (alpha_out->mux_sum) {
            EMIT("        float sum_a = (r_r0.a >= 0.5) ? ab_a : cd_a;\n");
        } else {
            EMIT("        float sum_a = ab_a + cd_a;\n");
        }

        /* Alpha output mapping */
        omp = output_map_prefix(alpha_out->output_map);
        oms = output_map_suffix(alpha_out->output_map);

        if (alpha_out->ab_dst != NV2A_REG_ZERO) {
            EMIT("        %s.a = %sab_a%s;\n",
                 reg_name(alpha_out->ab_dst), omp, oms);
        }
        if (alpha_out->cd_dst != NV2A_REG_ZERO) {
            EMIT("        %s.a = %scd_a%s;\n",
                 reg_name(alpha_out->cd_dst), omp, oms);
        }
        if (alpha_out->sum_dst != NV2A_REG_ZERO) {
            EMIT("        %s.a = %ssum_a%s;\n",
                 reg_name(alpha_out->sum_dst), omp, oms);
        }

        EMIT("    }\n\n");
    }

    /* ---- Final combiner ----
     *
     * The NV2A final combiner computes:
     *   result.rgb = D + lerp(C, B, A)
     *              = D + A*B + (1-A)*C
     *   result.a   = G.a
     *
     * Additionally, E*F is computed and made available as the EF_PROD
     * register, and V1+R0 is available as V1R0_SUM. These are computed
     * BEFORE the final combiner reads its inputs.
     */
    EMIT("    /* ---- Final Combiner ---- */\n");

    /* Compute specials: EF product and V1R0 sum */
    EMIT("    float4 r_ef = float4(0, 0, 0, 0);\n");
    EMIT("    float4 r_v1r0sum = float4(0, 0, 0, 0);\n");

    /* E * F product */
    EMIT("    {\n");
    EMIT("        float4 e_val = float4(");
    emit_mapped_input(buf, bufsize, &off, &state->final_input[4], ".rgb", state->num_stages - 1);
    EMIT(", ");
    emit_mapped_input(buf, bufsize, &off, &state->final_input[4], ".a", state->num_stages - 1);
    EMIT(");\n");
    EMIT("        float4 f_val = float4(");
    emit_mapped_input(buf, bufsize, &off, &state->final_input[5], ".rgb", state->num_stages - 1);
    EMIT(", ");
    emit_mapped_input(buf, bufsize, &off, &state->final_input[5], ".a", state->num_stages - 1);
    EMIT(");\n");
    EMIT("        r_ef = e_val * f_val;\n");
    EMIT("    }\n");

    /* V1 + R0 sum (clamped to [0,1]) */
    EMIT("    r_v1r0sum = saturate(r_v1 + r_r0);\n\n");

    /* Final combiner: result.rgb = D + A*B + (1-A)*C */
    /* Use last stage index for C0/C1 references in final combiner */
    {
        int fc_stage = state->num_stages > 0 ? state->num_stages - 1 : 0;

        EMIT("    float4 result;\n");
        EMIT("    {\n");

        /* Read final combiner inputs A, B, C, D */
        EMIT("        float3 fc_a = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[0], ".rgb", fc_stage);
        EMIT(";\n");
        EMIT("        float3 fc_b = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[1], ".rgb", fc_stage);
        EMIT(";\n");
        EMIT("        float3 fc_c = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[2], ".rgb", fc_stage);
        EMIT(";\n");
        EMIT("        float3 fc_d = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[3], ".rgb", fc_stage);
        EMIT(";\n");

        /* result.rgb = D + lerp(C, B, A) = D + A*B + (1-A)*C */
        EMIT("        result.rgb = saturate(fc_d + fc_a * fc_b + (1.0 - fc_a) * fc_c);\n");

        /* result.a = G.a */
        EMIT("        result.a = ");
        emit_mapped_input(buf, bufsize, &off, &state->final_input[6], ".a", fc_stage);
        EMIT(";\n");

        EMIT("    }\n\n");
    }

    /* ---- Fog ---- */
    /* No fog after the final combiner. The NV2A has no fog stage of its own:
     * the XDK puts fog into the final combiner (A = fog alpha, B = r0,
     * C = fog colour), and that has already run. Blending again here fogged
     * every pixel twice. */

    /* ---- Alpha test ---- */
    EMIT("    /* Alpha test */\n");
    EMIT("    if (alpha_test_enable) {\n");
    EMIT("        bool alpha_pass = true;\n");
    EMIT("        if      (alpha_func == 1u) alpha_pass = false;\n");
    EMIT("        else if (alpha_func == 2u) alpha_pass = (result.a <  alpha_ref);\n");
    EMIT("        else if (alpha_func == 3u) alpha_pass = (result.a == alpha_ref);\n");
    EMIT("        else if (alpha_func == 4u) alpha_pass = (result.a <= alpha_ref);\n");
    EMIT("        else if (alpha_func == 5u) alpha_pass = (result.a >  alpha_ref);\n");
    EMIT("        else if (alpha_func == 6u) alpha_pass = (result.a != alpha_ref);\n");
    EMIT("        else if (alpha_func == 7u) alpha_pass = (result.a >= alpha_ref);\n");
    EMIT("        if (!alpha_pass) discard;\n");
    EMIT("    }\n\n");

    {
        /* Debug switch, RECOMP_D3D8_PS_SHOW=v0|v1|t0|t1|r0|r1: replaces
         * the result with one register, so a frame shows what the combiners
         * were actually handed rather than what they made of it. Also foga
         * (the fog factor), fogon (white where fog is enabled), tc0 (stage 0
         * coordinates, wrapped) and tc0raw (their size, /16). */
        static const char *show = (const char *)-1;
        static const char *names[] = { "v0", "v1", "t0", "t1", "r0", "r1" };
        size_t n;

        if (show == (const char *)-1)
            show = getenv("RECOMP_D3D8_PS_SHOW");
        for (n = 0; show && n < sizeof names / sizeof names[0]; n++) {
            if (strcmp(show, names[n]) == 0) {
                EMIT("    result.rgb = r_%s.rgb;\n", names[n]);
                EMIT("    result.a = 1.0;\n");
                break;
            }
        }
        if (show && n < sizeof names / sizeof names[0]) {
            /* nothing: one of the register views above matched */
        } else if (show && strlen(show) == 3 && show[2] == 'a') {
            /* r0a, v0a, t0a ...: that register's alpha as grey, which is
             * what a blended draw is multiplied by. */
            char reg[3] = { show[0], show[1], 0 };

            for (n = 0; n < sizeof names / sizeof names[0]; n++)
                if (strcmp(reg, names[n]) == 0) {
                    EMIT("    result.rgb = r_%s.aaa;\n", names[n]);
                    EMIT("    result.a = 1.0;\n");
                    break;
                }
        }
        if (show && strcmp(show, "foga") == 0) {     /* the fog factor */
            EMIT("    result.rgb = r_fog.aaa;\n");
            EMIT("    result.a = 1.0;\n");
        } else if (show && strcmp(show, "tc0") == 0) {    /* stage 0 coordinates */
            EMIT("    result.rgb = float3(frac(input.tc0.xy), saturate(input.tc0.z));\n");
            EMIT("    result.a = 1.0;\n");
        } else if (show && strcmp(show, "tc0raw") == 0) { /* coordinate size */
            EMIT("    result.rgb = saturate(abs(input.tc0.xyz) / 16.0);\n");
            EMIT("    result.a = 1.0;\n");
        } else if (show && strcmp(show, "fograw") == 0) { /* oFog / 16, unclamped source */
            EMIT("    result.rgb = float3(saturate(input.fog / 16.0), saturate(-input.fog / 16.0), frac(input.fog));\n");
            EMIT("    result.a = 1.0;\n");
        } else if (show && strcmp(show, "fogon") == 0) {  /* fog enabled: white */
            EMIT("    result.rgb = fog_enable ? 1.0 : 0.0;\n");
            EMIT("    result.a = 1.0;\n");
        }
    }

    EMIT("    return result;\n");
    EMIT("}\n");

    return off;
}

#undef EMIT

/* ================================================================
 * Shader Compilation & Cache
 * ================================================================ */

static ID3D11PixelShader *compile_combiner_shader(const NV2ACombinerState *state)
{
    /* 16KB should be more than enough for any combiner shader */
    char hlsl[16384];
    ID3DBlob *code = NULL;
    ID3DBlob *errors = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr;
    int len;

    len = d3d8_combiners_generate_hlsl(state, hlsl, sizeof(hlsl));
    if (len < 0) {
        fprintf(stderr, "NV2A combiners: HLSL generation failed (buffer overflow)\n");
        return NULL;
    }

    {
        /* Debug switch, RECOMP_D3D8_PS_DUMP=<n>: prints the source of the
         * first n shaders built (n defaults to 2). The source is otherwise
         * only printed when the compile fails, which says nothing about a
         * shader that compiles and draws the wrong colour.
         *
         * This is called on a cache miss, so the order here is the order
         * shaders are *built*, which is not the order they are drawn with and
         * not the title's own pixel shader numbering. Reading a dump as
         * belonging to a particular draw is therefore a guess -- one that
         * cost a wrong diagnosis of the TimeSplitters 2 brightness bug. The
         * hash below is the same value d3d8_combiners_apply() prints when it
         * binds a shader, so draw and source can be matched instead. */
        static int dumped, limit = -1;

        if (limit < 0)
            limit = ps_dump_limit();
        if (dumped < limit) {
            dumped++;
            fprintf(stderr, "NV2A combiners: shader %08lX: state stages %d, "
                    "tex_mode %d %d %d %d, "
                    "c0[0] 0x%08lX c1[0] 0x%08lX, final_c0 0x%08lX final_c1 0x%08lX\n",
                    (unsigned long)combiner_state_hash(state),
                    state->num_stages, (int)state->tex_mode[0], (int)state->tex_mode[1],
                    (int)state->tex_mode[2], (int)state->tex_mode[3],
                    (unsigned long)state->c0[0], (unsigned long)state->c1[0],
                    (unsigned long)state->final_c0, (unsigned long)state->final_c1);
            fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End HLSL ---\n", hlsl);
            fflush(stderr);
        }
    }

    {
        long long started = d3d8_compile_clock();
        hr = D3DCompile(hlsl, (SIZE_T)len, "ps_combiner",
                        NULL, NULL, "main", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                        &code, &errors);
        d3d8_compile_note(0, started);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: HLSL compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors)
                       : "unknown error");
        /* Dump the generated source for debugging */
        fprintf(stderr, "--- Generated HLSL ---\n%s\n--- End HLSL ---\n", hlsl);
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }
    if (errors) ID3D10Blob_Release(errors);

    hr = ID3D11Device_CreatePixelShader(
        d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(code),
        ID3D10Blob_GetBufferSize(code),
        NULL, &ps);
    ID3D10Blob_Release(code);

    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: CreatePixelShader failed: 0x%08lX\n", hr);
        return NULL;
    }

    return ps;
}

ID3D11PixelShader *d3d8_combiners_get_shader(const NV2ACombinerState *state)
{
    uint32_t hash = combiner_state_hash(state);
    uint32_t idx = hash & (COMBINER_CACHE_SIZE - 1);
    int probe;

    /* Linear probe lookup */
    for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
        uint32_t slot = (idx + probe) & (COMBINER_CACHE_SIZE - 1);
        CombinerCacheEntry *entry = &g_cache[slot];

        if (!entry->in_use) {
            /* Cache miss - compile and insert */
            ID3D11PixelShader *ps = compile_combiner_shader(state);
            if (!ps) return NULL;

            entry->in_use = TRUE;
            entry->hash = hash;
            memcpy(&entry->state, state, sizeof(NV2ACombinerState));
            entry->shader = ps;
            entry->last_used_frame = g_frame_counter;
            return ps;
        }

        if (entry->hash == hash && combiner_state_equal(&entry->state, state)) {
            /* Cache hit */
            entry->last_used_frame = g_frame_counter;
            return entry->shader;
        }
    }

    /*
     * Table is full - evict the least recently used entry.
     * This is rare in practice (most games use <20 configs).
     */
    {
        uint32_t lru_slot = idx;
        uint32_t lru_frame = UINT32_MAX;
        ID3D11PixelShader *ps;
        CombinerCacheEntry *entry;

        for (probe = 0; probe < COMBINER_CACHE_SIZE; probe++) {
            if (g_cache[probe].last_used_frame < lru_frame) {
                lru_frame = g_cache[probe].last_used_frame;
                lru_slot = probe;
            }
        }

        entry = &g_cache[lru_slot];
        if (entry->shader) {
            ID3D11PixelShader_Release(entry->shader);
        }

        ps = compile_combiner_shader(state);
        if (!ps) return NULL;

        entry->hash = hash;
        memcpy(&entry->state, state, sizeof(NV2ACombinerState));
        entry->shader = ps;
        entry->last_used_frame = g_frame_counter;
        return ps;
    }
}

/* ================================================================
 * Initialization / Shutdown
 * ================================================================ */

/* Runtime compile timing (d3d8_internal.h). */
static volatile LONG     g_compiles[3];
static volatile LONG64   g_compile_ticks[3];
static LONG64            g_compile_qpf, g_compile_last_report;

long long d3d8_compile_clock(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void d3d8_compile_note(int kind, long long started)
{
    LARGE_INTEGER now;

    if (kind < 0 || kind > 2)
        return;
    QueryPerformanceCounter(&now);
    if (!g_compile_qpf) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_compile_qpf = f.QuadPart;
        g_compile_last_report = now.QuadPart;
    }
    InterlockedIncrement(&g_compiles[kind]);
    InterlockedAdd64(&g_compile_ticks[kind], now.QuadPart - started);
    if (now.QuadPart - g_compile_last_report >= 5 * g_compile_qpf) {
        g_compile_last_report = now.QuadPart;
        fprintf(stderr, "[D3D8] runtime shader compiles so far: combiner %ld (%.0f ms), "
                "fixed-function %ld (%.0f ms), vertex program %ld (%.0f ms)\n",
                g_compiles[0], g_compile_ticks[0] * 1000.0 / g_compile_qpf,
                g_compiles[1], g_compile_ticks[1] * 1000.0 / g_compile_qpf,
                g_compiles[2], g_compile_ticks[2] * 1000.0 / g_compile_qpf);
        fflush(stderr);
    }
}

HRESULT d3d8_combiners_init(void)
{
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    memset(g_cache, 0, sizeof(g_cache));
    memset(&g_combiner_state, 0, sizeof(g_combiner_state));
    g_ps_token = 0;
    g_dirty = TRUE;
    g_last_shader = NULL;
    g_frame_counter = 0;

    /* Create the PS constant buffer for combiner shaders.
     * Size must match NV2APSConstants, rounded up to 16-byte alignment. */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = (sizeof(NV2APSConstants) + 15) & ~15;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL,
                                   &g_combiner_cb);
    if (FAILED(hr)) {
        fprintf(stderr, "NV2A combiners: Failed to create constant buffer: "
                "0x%08lX\n", hr);
        return hr;
    }

    fprintf(stderr, "NV2A combiners: Initialized (cache size=%d)\n",
            COMBINER_CACHE_SIZE);
    return S_OK;
}

void d3d8_combiners_shutdown(void)
{
    int i;

    /* Release all cached shaders */
    for (i = 0; i < COMBINER_CACHE_SIZE; i++) {
        if (g_cache[i].in_use && g_cache[i].shader) {
            ID3D11PixelShader_Release(g_cache[i].shader);
        }
    }
    memset(g_cache, 0, sizeof(g_cache));
    g_last_shader = NULL;

    if (g_combiner_cb) {
        ID3D11Buffer_Release(g_combiner_cb);
        g_combiner_cb = NULL;
    }

    fprintf(stderr, "NV2A combiners: Shut down\n");
}

/* ================================================================
 * Draw Integration
 * ================================================================ */

void d3d8_combiners_set_pixel_shader(DWORD token)
{
    if (token != g_ps_token) {
        g_ps_token = token;
        g_dirty = TRUE;
    }
}

DWORD d3d8_combiners_get_pixel_shader(void)
{
    return g_ps_token;
}

BOOL d3d8_combiners_active(void)
{
    return g_ps_token != 0;
}

void d3d8_combiners_mark_dirty(void)
{
    g_dirty = TRUE;
}

BOOL d3d8_combiners_prepare_draw(void)
{
    ID3D11DeviceContext *ctx;
    ID3D11PixelShader *ps;
    D3D11_MAPPED_SUBRESOURCE mapped;
    const DWORD *rs;
    HRESULT hr;
    int i;

    /* Not using combiner shaders - fall back to fixed-function */
    if (g_ps_token == 0)
        return FALSE;

    ctx = d3d8_GetD3D11Context();
    if (!ctx || !g_combiner_cb)
        return FALSE;

    rs = d3d8_GetRenderStates();

    /* Rebuild combiner state from token + render states if dirty */
    if (g_dirty) {
        d3d8_combiners_parse_token(g_ps_token, rs, &g_combiner_state);
        g_dirty = FALSE;
        g_last_shader = NULL;
    }

    /* Get or compile the pixel shader for this combiner state */
    if (!g_last_shader)
        g_last_shader = d3d8_combiners_get_shader(&g_combiner_state);
    ps = g_last_shader;

    /* Under RECOMP_D3D8_PS_DUMP, say which shader each draw actually binds.
     * Printed only when it changes, so the log stays readable and still
     * interleaves with the replay's own per-draw lines: that pairing is what
     * identifies the shader behind a particular draw, which the dump order
     * on its own does not. */
    if (ps) {
        static int want = -1;
        static uint32_t last_printed;
        uint32_t hash;

        if (want < 0)
            want = ps_dump_limit() > 0;
        if (want) {
            hash = combiner_state_hash(&g_combiner_state);
            if (hash != last_printed) {
                last_printed = hash;
                fprintf(stderr, "NV2A combiners: binding shader %08lX\n",
                        (unsigned long)hash);
                fflush(stderr);
            }
        }
    }
    if (!ps) {
        fprintf(stderr, "NV2A combiners: Failed to get shader, "
                "falling back to FFP\n");
        return FALSE;
    }

    /* Bind the combiner pixel shader */
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);

    /* Update the PS constant buffer with current values.
     *
     * Even though the shader structure doesn't change, the constant
     * values (C0, C1, fog, alpha ref) can change every frame via
     * render state writes. So we always re-upload. */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_combiner_cb,
                                0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        NV2APSConstants *cb = (NV2APSConstants *)mapped.pData;

        /* Per-stage constants */
        for (i = 0; i < NV2A_MAX_COMBINER_STAGES; i++) {
            d3dcolor_to_float4(g_combiner_state.c0[i], cb->c0[i]);
            d3dcolor_to_float4(g_combiner_state.c1[i], cb->c1[i]);
        }

        /* Final combiner constants */
        d3dcolor_to_float4(g_combiner_state.final_c0, cb->final_c0);
        d3dcolor_to_float4(g_combiner_state.final_c1, cb->final_c1);

        /* Fog color from render state */
        d3dcolor_to_float4(rs[D3DRS_FOGCOLOR], cb->fog_color);

        /* Alpha test parameters */
        cb->alpha_ref = rs[D3DRS_ALPHAREF] / 255.0f;
        cb->alpha_func = rs[D3DRS_ALPHAFUNC];
        cb->alpha_test_enable = rs[D3DRS_ALPHATESTENABLE] ? 1 : 0;
        cb->fog_enable = rs[D3DRS_FOGENABLE] ? 1 : 0;
        for (i = 0; i < NV2A_MAX_TEXTURES; i++) {
            IDirect3DBaseTexture8 *tex = d3d8_GetStageTexture(i);
            D3DFORMAT format = d3d8_base_format(tex);
            UINT w = 0, h = 0;

            cb->alpha_only[i] = format == D3DFMT_A8 || format == D3DFMT_LIN_A8;
            /* Linear textures are addressed in texels on the NV2A; see
             * NV2APSConstants.tex_scale. */
            cb->tex_scale[i][0] = cb->tex_scale[i][1] = 1.0f;
            cb->tex_scale[i][2] = cb->tex_scale[i][3] = 0.0f;
            if (tex && d3d8_format_is_linear(format) && d3d8_base_size(tex, &w, &h) && w && h) {
                cb->tex_scale[i][0] = 1.0f / (float)w;
                cb->tex_scale[i][1] = 1.0f / (float)h;
            }
        }

        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_combiner_cb, 0);
    }

    /* Bind the constant buffer to PS slot 0 */
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_combiner_cb);

    /* Advance frame counter for LRU tracking */
    g_frame_counter++;

    return TRUE;
}
