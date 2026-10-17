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
 *                      registers, which the module reads with the
 *                      initial-exec TLS model.
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
#define RECOMP_MODULE_ABI 1

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

/* The module's xbe_entry_point and recomp_dispatch_init. */
void recomp_module_entry(void);
int  recomp_module_dispatch_init(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_RECOMP_MODULE_H */
