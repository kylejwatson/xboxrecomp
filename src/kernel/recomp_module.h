/*
 * recomp_module.h -- the lifted game as a module loaded at run time.
 *
 * Normally the generated code (src/recomp/gen) and the title's overrides are
 * linked into the executable with the runtime. With XBOXRECOMP_GAME_MODULE
 * the two are split: the executable holds the runtime and the title's main.c,
 * and the lifted code is a shared library built separately, loaded before the
 * guest starts. The executable then contains nothing derived from the game,
 * so it can be shared; the module is built on the player's machine from
 * their own disc.
 *
 * Direction of the links:
 *   module -> runtime  ordinary imports: the executable exports its symbols
 *                      (ENABLE_EXPORTS), including the thread-local guest
 *                      register block g_regs, which the module reads with
 *                      the initial-exec TLS model -- unless the module owns
 *                      the block (RECOMP_REGS_IN_MODULE, needed on Windows,
 *                      where a DLL cannot import thread-local data): then
 *                      the runtime asks it for this thread's block with
 *                      `recomp_module_regs` (xbox_memory_layout.h).
 *   runtime -> module  by name, here: recomp_lookup, recomp_lookup_manual,
 *                      the entry point, and every HLE_IMPORT_VAR /
 *                      HLE_ORIGINAL slot (hle.h), filled in by
 *                      recomp_module_load from the module's symbols.
 *
 * The runtime libraries are built with RECOMP_MODULE_HOST in this mode.
 */
#ifndef XBOXRECOMP_RECOMP_MODULE_H
#define XBOXRECOMP_RECOMP_MODULE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever what the runtime expects of a module changes. A module
 * exports `recomp_module_abi` (src/kernel/recomp_module_abi.c, compiled into
 * the module) and a mismatch is refused at load. */
#define RECOMP_MODULE_ABI 2   /* 2: the registers are one block, g_regs */

/* The register block's layout (RecompGuestRegs: xbox_memory_layout.h and
 * templates/runtime/recomp_types.h each keep a copy) folded into a number.
 * The module exports its value as the lifted code sees the block
 * (`recomp_module_regs_layout`) and the runtime compares it with its own,
 * so the two copies cannot drift apart unnoticed. Needs whichever header
 * defines the block. */
#define RECOMP_REGS_LAYOUT ((uint32_t)( \
    sizeof(RecompGuestRegs) * 1000003u + \
    offsetof(RecompGuestRegs, r_esp) * 7u + offsetof(RecompGuestRegs, r_ebp) * 11u + \
    offsetof(RecompGuestRegs, r_fs_base) * 13u + offsetof(RecompGuestRegs, r_icall_dispatch_form) * 17u + \
    offsetof(RecompGuestRegs, r_df) * 19u + offsetof(RecompGuestRegs, r_fp_cmp) * 23u + \
    offsetof(RecompGuestRegs, r_fp_cc) * 29u + offsetof(RecompGuestRegs, r_fp_stack) * 31u + \
    offsetof(RecompGuestRegs, r_mm0) * 37u + offsetof(RecompGuestRegs, r_xmm0) * 41u + \
    offsetof(RecompGuestRegs, r_xmm7) * 43u))

/* A slot in the runtime that takes a value from the module. hle.h declares
 * one per HLE_IMPORT_VAR / HLE_ORIGINAL in module-host builds and registers it
 * before main; recomp_module_load fills every registered slot. */
typedef struct RecompModuleSlot {
    const char *symbol;              /* e.g. "hle_var_D3D_g_pDevice" */
    int is_function_pointer;         /* 1: hle_original_*, 0: hle_var_* */
    void *slot;                      /* uint32_t* or void (**)(void) */
    struct RecompModuleSlot *next;
} RecompModuleSlot;

void recomp_module_register_slot(RecompModuleSlot *slot);

/* Loads the game module. `path` NULL means: $RECOMP_GAME_MODULE, else
 * `default_name` beside the executable. Returns 1, or 0 with a message for
 * the player in `err`. */
int recomp_module_load(const char *path, const char *default_name,
                       char *err, size_t err_bytes);

/* The path that was loaded (empty before). */
const char *recomp_module_path(void);

/* ---- a shared build: runtime + title kit (scripts/make_kit.py) ----
 *
 * The kit is <exe dir>/kit/. The module it builds lives in the per-user
 * module cache, in a directory named after the kit id, so an updated build
 * never loads a module made by an older kit. Keep these in step with
 * scripts/build_module.py (module_dir, xbe_fingerprint). */

/* <exe dir>/kit when the executable carries a kit (kit/kit.id), else 0. */
int  recomp_module_kit_dir(char *out, size_t n);
/* The kit's module: <user data>/xboxrecomp/modules/<title id>/<first 16 of
 * the kit id>/<module_file>. 0 without a kit. */
int  recomp_module_kit_module_path(uint32_t title_id, const char *module_file,
                                   char *out, size_t n);
/* "<size> <timedate> <title id> <version>" in hex, from an XBE in memory. */
void recomp_module_xbe_fingerprint(const void *xbe, size_t size, char *out, size_t n);
/* Whether the module exists and was built from this XBE (its xbe.id). */
int  recomp_module_is_current(const char *module_path, const void *xbe, size_t size);

/* The module's xbe_entry_point and recomp_dispatch_init. */
void recomp_module_entry(void);
int  recomp_module_dispatch_init(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_MODULE_H */
