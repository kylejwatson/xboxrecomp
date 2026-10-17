/*
 * Compiled into a game module (not the runtime): the ABI the module was built
 * for and the register block's layout as the lifted code sees it, both
 * checked by recomp_module_load; and, with RECOMP_REGS_IN_MODULE, the
 * register block itself. See recomp_module.h.
 */
#include "recomp_types.h"          /* the lifted code's copy of RecompGuestRegs */
#include "xbox_memory_layout.h"    /* RECOMP_REGS_INIT */
#include "recomp_module.h"

#if defined(_WIN32)
#  define MODULE_EXPORT __declspec(dllexport)
#else
#  define MODULE_EXPORT __attribute__((visibility("default")))
#endif

MODULE_EXPORT const uint32_t recomp_module_abi = RECOMP_MODULE_ABI;
MODULE_EXPORT const uint32_t recomp_module_regs_layout = RECOMP_REGS_LAYOUT;

#ifdef RECOMP_REGS_IN_MODULE
/* The guest registers, one block per thread, owned here: the lifted code
 * names g_regs directly, the runtime asks for this thread's with the
 * function below. */
RECOMP_TLS RecompGuestRegs g_regs = RECOMP_REGS_INIT;

MODULE_EXPORT RecompGuestRegs *recomp_module_regs(void)
{
    return &g_regs;
}
#endif
