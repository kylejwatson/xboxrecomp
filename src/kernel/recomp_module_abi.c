/*
 * Compiled into a game module (not the runtime): the ABI the module was built
 * for, checked by recomp_module_load. See recomp_module.h.
 */
#include "recomp_module.h"

#if defined(_WIN32)
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const uint32_t recomp_module_abi = RECOMP_MODULE_ABI;
