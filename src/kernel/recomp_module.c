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

#if defined(_WIN32)
#  include <windows.h>
#  define MODULE_OPEN(p)      ((void *)LoadLibraryA(p))
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

    fprintf(stderr, "[MODULE] %s: ABI %u, %d HLE imports filled, %d absent\n",
            g_module_path, (unsigned)*abi, filled, missing);
    return 1;
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
