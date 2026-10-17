/*
 * recomp_module.c -- loading the lifted game as a module (recomp_module.h).
 *
 * Only built into the runtime with RECOMP_MODULE_HOST. In the ordinary static
 * build the generated code defines recomp_lookup itself and this file is
 * empty.
 */
#include "recomp_module.h"

#ifdef RECOMP_MODULE_HOST

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "xbox_memory_layout.h"   /* RecompGuestRegs, recomp_regs_attach */

#if defined(_WIN32)
#  include <windows.h>
#  include <dbghelp.h>
#  define MODULE_OPEN(p)      module_open_win32(p)
#  define MODULE_SYM(h, s)    ((void *)GetProcAddress((HMODULE)(h), (s)))
#  define PATH_SEP_CHAR       '\\'
#else
#  include <dlfcn.h>
#  include <unistd.h>
/* RTLD_NOW: an import the executable does not export is reported here, with
 * its name, rather than as a crash the first time the guest reaches it. */
#  define MODULE_OPEN(p)      dlopen((p), RTLD_NOW | RTLD_LOCAL)
#  define MODULE_SYM(h, s)    dlsym((h), (s))
#  define PATH_SEP_CHAR       '/'
#endif

typedef void (*recomp_func_t)(void);

static RecompModuleSlot *g_slots;
static void *g_module;
static char g_module_path[1024];

#if defined(_WIN32)
/* A UTF-8 path (the module cache is under the player's profile, which may
 * not be ASCII); its dependencies are looked for beside it first. The
 * runtime it imports is this executable, already loaded under its own name
 * (futureperfect_recomp.exe: a renamed executable cannot load the module). */
static void *module_open_win32(const char *path)
{
    wchar_t wpath[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024))
        return NULL;
    return (void *)LoadLibraryExW(wpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
#endif

static void (*g_entry)(void);
static int (*g_dispatch_init)(void);
static recomp_func_t (*g_lookup)(uint32_t);
static recomp_func_t (*g_lookup_manual)(uint32_t);

void recomp_module_register_slot(RecompModuleSlot *slot)
{
    /* Runs from constructors, before main and before any thread. */
    slot->next = g_slots;
    g_slots = slot;
}

static const char *module_error(void)
{
#if defined(_WIN32)
    static char buf[64];
    snprintf(buf, sizeof buf, "error %lu", (unsigned long)GetLastError());
    return buf;
#else
    const char *e = dlerror();
    return e ? e : "unknown error";
#endif
}

/* The directory the executable is in, with a trailing separator. */
static void exe_dir(char *out, size_t bytes)
{
    size_t n = 0;
    out[0] = '\0';
#if defined(_WIN32)
    n = GetModuleFileNameA(NULL, out, (DWORD)bytes);
    if (n == 0 || n >= bytes) { out[0] = '\0'; return; }
#else
    {
        ssize_t r = readlink("/proc/self/exe", out, bytes - 1);
        if (r <= 0) { out[0] = '\0'; return; }
        n = (size_t)r;
        out[n] = '\0';
    }
#endif
    while (n > 0 && out[n - 1] != '/' && out[n - 1] != '\\')
        n--;
    out[n] = '\0';
}

int recomp_module_load(const char *path, const char *default_name,
                       char *err, size_t err_bytes)
{
    const uint32_t *abi;
    RecompModuleSlot *s;
    int filled = 0, missing = 0;

    if (g_module)
        return 1;
    if (!path || !*path)
        path = getenv("RECOMP_GAME_MODULE");
    if (path && *path) {
        snprintf(g_module_path, sizeof g_module_path, "%s", path);
    } else {
        char dir[900];
        exe_dir(dir, sizeof dir);
        snprintf(g_module_path, sizeof g_module_path, "%s%s", dir,
                 default_name ? default_name : "");
    }

    g_module = MODULE_OPEN(g_module_path);
    if (!g_module) {
        /* A shared build carries the kit that makes the module
         * (scripts/make_kit.py); say how to run it. */
        char kit[1024];
        FILE *probe;
        exe_dir(kit, sizeof kit - 16);
        strcat(kit, "kit/kit.json");
        probe = fopen(kit, "rb");
        if (probe) fclose(probe);
        snprintf(err, err_bytes,
                 "Could not load the game module\n%s\n(%s)\n\n"
                 "It is built from your own copy of the game; build it first%s",
                 g_module_path, module_error(),
                 probe ? ":\n\npython3 kit/toolkit/scripts/build_module.py "
                         "--xbe <your disc>/default.xbe" : ".");
        return 0;
    }

    abi = (const uint32_t *)MODULE_SYM(g_module, "recomp_module_abi");
    if (!abi || *abi != RECOMP_MODULE_ABI) {
        snprintf(err, err_bytes,
                 "The game module\n%s\nwas built for a different version of "
                 "the runtime (module ABI %u, runtime %u). Rebuild it.",
                 g_module_path, abi ? (unsigned)*abi : 0u,
                 (unsigned)RECOMP_MODULE_ABI);
        g_module = NULL;
        return 0;
    }

    /* The register block: the same layout on both sides, and owned by the
     * side this runtime expects. A module that owns it while the runtime
     * keeps one too (or neither) would run with two sets of registers. */
    {
        const uint32_t *layout = (const uint32_t *)MODULE_SYM(g_module,
                                                    "recomp_module_regs_layout");
        RecompGuestRegs *(*module_regs)(void) =
            (RecompGuestRegs *(*)(void))MODULE_SYM(g_module, "recomp_module_regs");
#ifdef RECOMP_REGS_IN_MODULE
        const int want_regs = 1;
#else
        const int want_regs = 0;
#endif
        if (!layout || *layout != RECOMP_REGS_LAYOUT || !module_regs != !want_regs) {
            snprintf(err, err_bytes,
                     "The game module\n%s\nwas built for a different version of "
                     "the runtime (%s). Rebuild it.", g_module_path,
                     !layout || *layout != RECOMP_REGS_LAYOUT
                         ? "the guest register layout differs"
                         : want_regs ? "the runtime expects the module to own the registers"
                                     : "the module owns the registers, the runtime keeps them");
            g_module = NULL;
            return 0;
        }
#ifdef RECOMP_REGS_IN_MODULE
        recomp_regs_attach(module_regs);
#endif
    }

    g_entry         = (void (*)(void))MODULE_SYM(g_module, "xbe_entry_point");
    g_dispatch_init = (int (*)(void))MODULE_SYM(g_module, "recomp_dispatch_init");
    g_lookup        = (recomp_func_t (*)(uint32_t))MODULE_SYM(g_module, "recomp_lookup");
    g_lookup_manual = (recomp_func_t (*)(uint32_t))MODULE_SYM(g_module, "recomp_lookup_manual");
    if (!g_entry || !g_lookup) {
        snprintf(err, err_bytes,
                 "The game module\n%s\nhas no %s.", g_module_path,
                 g_entry ? "recomp_lookup" : "xbe_entry_point");
        g_module = NULL;
        return 0;
    }

    /* The HLE's imports. A name the module lacks stays 0, which is what the
     * static build's generated recomp_hle.c says for a symbol the title's XDK
     * symbols could not name, and every user checks for it. */
    for (s = g_slots; s; s = s->next) {
        void *p = MODULE_SYM(g_module, s->symbol);
        if (s->is_function_pointer)
            *(recomp_func_t *)s->slot = p ? *(recomp_func_t *)p : NULL;
        else
            *(uint32_t *)s->slot = p ? *(const uint32_t *)p : 0u;
        if (p) filled++; else missing++;
    }

#if defined(_WIN32)
    /* The fault handler names a host address with SymFromAddr; the symbol
     * handler was started before this module was loaded. */
    SymRefreshModuleList(GetCurrentProcess());
#endif
    fprintf(stderr, "[MODULE] %s: ABI %u, %d HLE imports filled, %d absent\n",
            g_module_path, (unsigned)*abi, filled, missing);
    return 1;
}

int recomp_module_kit_dir(char *out, size_t n)
{
    char probe[1100];
    struct stat st;
    exe_dir(out, n > 8 ? n - 8 : n);
    strcat(out, "kit");
    snprintf(probe, sizeof probe, "%s%ckit.id", out, PATH_SEP_CHAR);
    return stat(probe, &st) == 0;
}

int recomp_module_kit_module_path(uint32_t title_id, const char *module_file,
                                  char *out, size_t n)
{
    char kit[1024], idfile[1100], id[80] = "", base[1024];
    const char *env;
    FILE *f;
    size_t len;

    if (!recomp_module_kit_dir(kit, sizeof kit))
        return 0;
    snprintf(idfile, sizeof idfile, "%s%ckit.id", kit, PATH_SEP_CHAR);
    if (!(f = fopen(idfile, "rb")))
        return 0;
    if (!fgets(id, sizeof id, f))
        id[0] = '\0';
    fclose(f);
    len = strspn(id, "0123456789abcdef");
    if (len < 16)
        return 0;
    id[16] = '\0';
#if defined(_WIN32)
    env = getenv("LOCALAPPDATA");
    snprintf(base, sizeof base, "%s", env && *env ? env : ".");
#else
    env = getenv("XDG_DATA_HOME");
    if (env && *env)
        snprintf(base, sizeof base, "%s", env);
    else
        snprintf(base, sizeof base, "%s/.local/share",
                 getenv("HOME") ? getenv("HOME") : ".");
#endif
    snprintf(out, n, "%s%cxboxrecomp%cmodules%c%08X%c%s%c%s", base, PATH_SEP_CHAR,
             PATH_SEP_CHAR, PATH_SEP_CHAR, (unsigned)title_id, PATH_SEP_CHAR, id,
             PATH_SEP_CHAR, module_file);
    return 1;
}

void recomp_module_xbe_fingerprint(const void *xbe, size_t size, char *out, size_t n)
{
    const uint8_t *x = (const uint8_t *)xbe;
    uint32_t base, timedate, cert, title = 0, version = 0;
    out[0] = '\0';
    if (size < 0x11C)
        return;
    memcpy(&base, x + 0x104, 4);
    memcpy(&timedate, x + 0x114, 4);
    memcpy(&cert, x + 0x118, 4);
    if (cert >= base && (size_t)(cert - base) + 0xB0 <= size) {
        memcpy(&title, x + (cert - base) + 0x08, 4);
        memcpy(&version, x + (cert - base) + 0xAC, 4);
    }
    snprintf(out, n, "%zx %08x %08x %08x", size, (unsigned)timedate, (unsigned)title,
             (unsigned)version);
}

int recomp_module_is_current(const char *module_path, const void *xbe, size_t size)
{
    char idfile[1100], have[128] = "", want[128];
    const char *slash = strrchr(module_path, PATH_SEP_CHAR);
    struct stat st;
    FILE *f;
    if (stat(module_path, &st) != 0 || !slash)
        return 0;
    snprintf(idfile, sizeof idfile, "%.*sxbe.id", (int)(slash - module_path + 1), module_path);
    if (!(f = fopen(idfile, "rb")))
        return 0;
    if (!fgets(have, sizeof have, f))
        have[0] = '\0';
    fclose(f);
    have[strcspn(have, "\r\n")] = '\0';
    recomp_module_xbe_fingerprint(xbe, size, want, sizeof want);
    return want[0] && strcmp(have, want) == 0;
}

const char *recomp_module_path(void)
{
    return g_module ? g_module_path : "";
}

void recomp_module_entry(void)
{
    if (g_entry)
        g_entry();
}

int recomp_module_dispatch_init(void)
{
    return g_dispatch_init ? g_dispatch_init() : 0;
}

/* What the static build's generated code would have defined. The runtime
 * calls these by name (kernel bridge, HLE callbacks); the module's own lifted
 * code calls its own copies directly. */
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    return g_lookup ? g_lookup(xbox_va) : NULL;
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    return g_lookup_manual ? g_lookup_manual(xbox_va) : NULL;
}

#endif /* RECOMP_MODULE_HOST */
