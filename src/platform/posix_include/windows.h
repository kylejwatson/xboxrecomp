/* <windows.h> off Windows: the NT type vocabulary and the POSIX
 * implementations of the Win32 calls the runtime makes (xbox_winnt.h,
 * win32_compat.h). On the include path only for non-Windows builds, so a
 * shared source can keep its #include <windows.h>. */
#ifndef XBOXRECOMP_SHIM_WINDOWS_H
#define XBOXRECOMP_SHIM_WINDOWS_H
#include "../xbox_winnt.h"
#endif
