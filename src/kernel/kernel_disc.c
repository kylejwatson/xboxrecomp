/*
 * kernel_disc.c -- an Xbox disc image (XDVDFS) as the game disc.
 * See kernel_disc.h; the format notes are in docs/formats/disc-image.md and
 * the Python reader tools/xiso/xdvdfs.py reads the same layouts.
 *
 * The whole directory tree is read at mount (a disc has a few thousand
 * entries at most) and never changes, so lookups need no lock. Reads are
 * positioned (pread / ReadFile with an offset), so threads sharing the
 * image's host file do not move each other's position.
 */
#include "kernel_disc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#define SECTOR 2048u
static const char k_magic[] = "MICROSOFT*XBOX*MEDIA";
#define MAGIC_LEN (sizeof k_magic - 1)

/* Where a game partition may start in an image; see xdvdfs.py. */
static const uint64_t k_bases[] = {
    0x00000000ull,      /* bare game partition (xiso, most tools) */
    0x0000FD90ull,      /* some rebuilt images */
    0x00030600ull,      /* video partition first */
    0x0FD90000ull,      /* full disc layout */
    0x18300000ull,      /* Redump full-disc dump */
};

#define ATTR_DIRECTORY 0x10

#if defined(_WIN32)
typedef HANDLE host_file;
#define HOST_FILE_BAD INVALID_HANDLE_VALUE
#else
typedef int host_file;
#define HOST_FILE_BAD (-1)
#endif

typedef struct DiscNode {
    char    *name;
    uint32_t sector;
    uint32_t size;
    uint8_t  attr;
    int      parent;
    int      first_child;
    int      next_sibling;
} DiscNode;

static struct {
    int       mounted;
    char      path[1024];
    uint64_t  base;
    uint64_t  image_size;
    LARGE_INTEGER created;              /* the volume's timestamp, for every file */
    DiscNode *nodes;
    int       count, cap;
    host_file file;
} g_disc;

/* ---------------------------------------------------------------- reading */

#if defined(_WIN32)
static host_file host_open(const char *path)
{
    WCHAR w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024))
        return HOST_FILE_BAD;
    return CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}
static void host_close(host_file f) { CloseHandle(f); }
static uint64_t host_size(host_file f)
{
    LARGE_INTEGER s;
    return GetFileSizeEx(f, &s) ? (uint64_t)s.QuadPart : 0;
}
static int host_pread(host_file f, uint64_t off, void *buf, size_t n)
{
    OVERLAPPED ov;
    DWORD got = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    return ReadFile(f, buf, (DWORD)n, &got, &ov) ? (int)got : -1;
}
#else
static host_file host_open(const char *path) { return open(path, O_RDONLY | O_CLOEXEC); }
static void host_close(host_file f) { close(f); }
static uint64_t host_size(host_file f)
{
    struct stat st;
    return fstat(f, &st) == 0 ? (uint64_t)st.st_size : 0;
}
static int host_pread(host_file f, uint64_t off, void *buf, size_t n)
{
    return (int)pread(f, buf, n, (off_t)off);
}
#endif

/* n bytes at an image offset, looping over short reads; the count read. */
static int raw_read(uint64_t offset, void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        size_t chunk = n - done > (1u << 30) ? (1u << 30) : n - done;
        int r = host_pread(g_disc.file, offset + done, (char *)buf + done, chunk);
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        done += (size_t)r;
    }
    return (int)done;
}

/* The game partition's base in an open image, or -1. */
static int64_t find_base(host_file f)
{
    char probe[MAGIC_LEN];
    size_t i;
    for (i = 0; i < sizeof k_bases / sizeof k_bases[0]; i++) {
        if (host_pread(f, k_bases[i] + 32 * SECTOR, probe, sizeof probe) == (int)sizeof probe &&
            memcmp(probe, k_magic, MAGIC_LEN) == 0)
            return (int64_t)k_bases[i];
    }
    return -1;
}

int xbox_disc_is_image(const char *path)
{
    host_file f;
    int64_t base;
    if (!path || !*path)
        return 0;
    f = host_open(path);
    if (f == HOST_FILE_BAD)
        return 0;
    base = find_base(f);
    host_close(f);
    return base >= 0;
}

/* ----------------------------------------------------------- the tree */

static int add_node(const char *name, size_t name_len, uint32_t sector,
                    uint32_t size, uint8_t attr, int parent)
{
    DiscNode *n;
    if (g_disc.count == g_disc.cap) {
        int cap = g_disc.cap ? g_disc.cap * 2 : 256;
        DiscNode *grown = (DiscNode *)realloc(g_disc.nodes, (size_t)cap * sizeof *grown);
        if (!grown)
            return -1;
        g_disc.nodes = grown;
        g_disc.cap = cap;
    }
    n = &g_disc.nodes[g_disc.count];
    n->name = (char *)malloc(name_len + 1);
    if (!n->name)
        return -1;
    memcpy(n->name, name, name_len);
    n->name[name_len] = '\0';
    n->sector = sector;
    n->size = size;
    n->attr = attr;
    n->parent = parent;
    n->first_child = -1;
    n->next_sibling = -1;
    if (parent >= 0) {
        n->next_sibling = g_disc.nodes[parent].first_child;
        g_disc.nodes[parent].first_child = g_disc.count;
    }
    return g_disc.count++;
}

/* Read one directory's entries under node `dir`, then each subdirectory.
 *
 * Entries form a binary tree but are also packed in order inside each
 * sector, so a linear walk that restarts at every sector boundary reads all
 * of them without trusting the tree links -- the same choice xdvdfs.py makes,
 * for the same reason: a bad link must not drop a subtree silently. */
static int read_directory(int dir, int depth)
{
    uint32_t sector = g_disc.nodes[dir].sector, size = g_disc.nodes[dir].size;
    uint8_t *data;
    uint32_t at;
    int first_new = g_disc.count, i, last_new;

    if (depth > 32)
        return 0;
    if (size == 0)
        return 1;
    data = (uint8_t *)malloc(size);
    if (!data)
        return 0;
    if (raw_read(g_disc.base + (uint64_t)sector * SECTOR, data, size) != (int)size) {
        free(data);
        return 0;
    }
    for (at = 0; at < size; at += SECTOR) {
        uint32_t end = size - at < SECTOR ? size - at : SECTOR, off = 0;
        while (off + 0x0E <= end) {
            const uint8_t *e = data + at + off;
            uint16_t l = (uint16_t)(e[0] | e[1] << 8), r = (uint16_t)(e[2] | e[3] << 8);
            uint32_t sec = e[4] | e[5] << 8 | e[6] << 16 | (uint32_t)e[7] << 24;
            uint32_t sz = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24;
            uint8_t attr = e[12], nlen = e[13];
            if ((l == 0xFFFF && r == 0xFFFF) || nlen == 0 || off + 0x0E + nlen > end)
                break;
            if (add_node((const char *)e + 0x0E, nlen, sec, sz, attr, dir) < 0) {
                free(data);
                return 0;
            }
            off += (0x0E + nlen + 3) & ~3u;
        }
    }
    free(data);
    last_new = g_disc.count;
    for (i = first_new; i < last_new; i++)
        if ((g_disc.nodes[i].attr & ATTR_DIRECTORY) && !read_directory(i, depth + 1))
            return 0;
    return 1;
}

static int names_equal(const char *a, const char *b, size_t blen)
{
    size_t i;
    for (i = 0; i < blen; i++)
        if (!a[i] || tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return a[blen] == '\0';
}

/* The node a disc path names (either separator, any case), or -1; with
 * parent_missing set when it is a directory on the way that is absent. */
static int lookup(const char *path, int *parent_missing)
{
    int node = 0;
    const char *p = path;
    if (parent_missing)
        *parent_missing = 0;
    while (*p) {
        const char *end;
        int child;
        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            break;
        for (end = p; *end && *end != '/' && *end != '\\'; end++)
            ;
        if (end - p == 1 && *p == '.') {
            p = end;
            continue;
        }
        if (!(g_disc.nodes[node].attr & ATTR_DIRECTORY) && node != 0)
            child = -1;
        else
            for (child = g_disc.nodes[node].first_child; child >= 0;
                 child = g_disc.nodes[child].next_sibling)
                if (names_equal(g_disc.nodes[child].name, p, (size_t)(end - p)))
                    break;
        if (child < 0) {
            if (parent_missing) {
                const char *q = end;
                while (*q == '/' || *q == '\\')
                    q++;
                *parent_missing = *q != '\0';
            }
            return -1;
        }
        node = child;
        p = end;
    }
    return node;
}

static int is_dir(int node)
{
    return node == 0 || (g_disc.nodes[node].attr & ATTR_DIRECTORY);
}

/* ----------------------------------------------------------- mounting */

int xbox_disc_mount(const char *image_path, char *err, size_t err_bytes)
{
    host_file f;
    int64_t base;
    uint8_t desc[SECTOR];

    if (g_disc.mounted)
        return strcmp(g_disc.path, image_path) == 0;
    f = host_open(image_path);
    if (f == HOST_FILE_BAD) {
        snprintf(err, err_bytes, "Could not open %s.", image_path);
        return 0;
    }
    base = find_base(f);
    if (base < 0) {
        host_close(f);
        snprintf(err, err_bytes, "%s is not an Xbox disc image (no XDVDFS volume "
                 "where one is found in xiso and Redump images).", image_path);
        return 0;
    }
    g_disc.file = f;
    g_disc.base = (uint64_t)base;
    g_disc.image_size = host_size(f);
    if (raw_read(g_disc.base + 32 * SECTOR, desc, SECTOR) != (int)SECTOR) {
        snprintf(err, err_bytes, "%s is truncated.", image_path);
        host_close(f);
        return 0;
    }
    memcpy(&g_disc.created, desc + 0x1C, 8);
    g_disc.count = 0;
    {
        uint32_t root_sector = desc[0x14] | desc[0x15] << 8 | desc[0x16] << 16 |
                               (uint32_t)desc[0x17] << 24;
        uint32_t root_size = desc[0x18] | desc[0x19] << 8 | desc[0x1A] << 16 |
                             (uint32_t)desc[0x1B] << 24;
        if (add_node("", 0, root_sector, root_size, ATTR_DIRECTORY, -1) != 0 ||
            !read_directory(0, 0)) {
            snprintf(err, err_bytes, "Could not read the directory tree of %s.", image_path);
            host_close(f);
            return 0;
        }
    }
    snprintf(g_disc.path, sizeof g_disc.path, "%s", image_path);
    g_disc.mounted = 1;
    fprintf(stderr, "[DISC] mounted %s: game partition at 0x%llX, %d entries\n",
            image_path, (unsigned long long)g_disc.base, g_disc.count - 1);
    return 1;
}

int xbox_disc_mounted(void)
{
    return g_disc.mounted;
}

const char *xbox_disc_image_path(void)
{
    return g_disc.mounted ? g_disc.path : NULL;
}

int xbox_disc_read_file(const char *disc_path, void **data, size_t *size)
{
    int node;
    void *buf;
    if (!g_disc.mounted || (node = lookup(disc_path, NULL)) < 0 || is_dir(node))
        return 0;
    buf = malloc(g_disc.nodes[node].size ? g_disc.nodes[node].size : 1);
    if (!buf)
        return 0;
    if (raw_read(g_disc.base + (uint64_t)g_disc.nodes[node].sector * SECTOR, buf,
                 g_disc.nodes[node].size) != (int)g_disc.nodes[node].size) {
        free(buf);
        return 0;
    }
    *data = buf;
    *size = g_disc.nodes[node].size;
    return 1;
}

const char *xbox_disc_host_rel(const char *host_path)
{
    size_t n = sizeof XBOX_DISC_PATH_PREFIX - 1;
    if (!g_disc.mounted || !host_path || strncmp(host_path, XBOX_DISC_PATH_PREFIX, n) != 0)
        return NULL;
    host_path += n;
    while (*host_path == '/' || *host_path == '\\')
        host_path++;
    return host_path;
}

const char *xbox_disc_host_rel_w(const wchar_t *host_path, char *buf, size_t n)
{
    size_t i;
    if (!g_disc.mounted || !host_path || n == 0)
        return NULL;
    for (i = 0; i + 1 < n && host_path[i]; i++)
        buf[i] = host_path[i] < 0x80 ? (char)host_path[i] : '?';
    buf[i] = '\0';
    return xbox_disc_host_rel(buf);
}

/* ------------------------------------------------------------- handles */

/* Handles are slots in a fixed pool, so "is this one ours" is a range check
 * on the pointer and never touches memory a foreign handle points at (on
 * Windows a HANDLE is not a pointer at all). */
#define DISC_HANDLES 1024
typedef struct DiscHandle {
    volatile long in_use;
    int     node;
    int64_t pos;
    int     cursor;                 /* next child to report, by index */
    char    pattern[64];
    char    path[512];              /* as opened, for RootDirectory joins */
} DiscHandle;
static DiscHandle g_handles[DISC_HANDLES];

#if defined(_WIN32)
#  define TAKE_SLOT(p) (InterlockedCompareExchange(&(p)->in_use, 1, 0) == 0)
#else
#  define TAKE_SLOT(p) __sync_bool_compare_and_swap(&(p)->in_use, 0, 1)
#endif

int xbox_disc_owns(HANDLE h)
{
    const char *p = (const char *)h;
    return p >= (const char *)&g_handles[0] && p < (const char *)&g_handles[DISC_HANDLES] &&
           ((DiscHandle *)h)->in_use;
}

static DiscHandle *new_handle(int node)
{
    int i;
    for (i = 0; i < DISC_HANDLES; i++) {
        DiscHandle *d = &g_handles[i];
        if (!d->in_use && TAKE_SLOT(d)) {
            d->node = node;
            d->pos = 0;
            d->cursor = -2;
            d->pattern[0] = '\0';
            d->path[0] = '\0';
            return d;
        }
    }
    return NULL;
}


static void node_path(int node, char *out, size_t n)
{
    char tmp[1024];
    out[0] = '\0';
    while (node > 0) {
        snprintf(tmp, sizeof tmp, "%s%s%s", g_disc.nodes[node].name, out[0] ? "/" : "", out);
        snprintf(out, n, "%s", tmp);
        node = g_disc.nodes[node].parent;
    }
}

const char *xbox_disc_handle_path(HANDLE h)
{
    return xbox_disc_owns(h) ? ((DiscHandle *)h)->path : NULL;
}

static ULONG node_attributes(int node)
{
    /* XDVDFS keeps the FAT attribute byte; a disc file is read-only. */
    ULONG a = g_disc.nodes[node].attr & 0x37;
    if (node == 0)
        a |= XBOX_FILE_ATTRIBUTE_DIRECTORY;
    a |= XBOX_FILE_ATTRIBUTE_READONLY;
    return a;
}

static int64_t node_size(int node)
{
    return is_dir(node) ? 0 : (int64_t)g_disc.nodes[node].size;
}

static void fill_times(LARGE_INTEGER *c, LARGE_INTEGER *a, LARGE_INTEGER *w, LARGE_INTEGER *ch)
{
    *c = *a = *w = *ch = g_disc.created;
}

NTSTATUS xbox_disc_create(const char *disc_path, ACCESS_MASK access,
                          ULONG disposition, ULONG options,
                          HANDLE *out, ULONG_PTR *information)
{
    int parent_missing, node;
    DiscHandle *d;
    (void)access;

    *information = 0;
    node = lookup(disc_path, &parent_missing);
    if (node < 0) {
        /* Creating anything on a DVD fails the same way whether or not the
         * name exists; only a plain open reports it missing. */
        if (disposition != XBOX_FILE_OPEN && disposition != XBOX_FILE_OVERWRITE)
            return STATUS_MEDIA_WRITE_PROTECTED;
        return parent_missing ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (disposition == XBOX_FILE_CREATE)
        return STATUS_OBJECT_NAME_COLLISION;
    if (disposition == XBOX_FILE_SUPERSEDE || disposition == XBOX_FILE_OVERWRITE ||
        disposition == XBOX_FILE_OVERWRITE_IF)
        return STATUS_MEDIA_WRITE_PROTECTED;
    if ((options & XBOX_FILE_DIRECTORY_FILE) && !is_dir(node))
        return STATUS_NOT_A_DIRECTORY;
    if ((options & XBOX_FILE_NON_DIRECTORY_FILE) && is_dir(node))
        return STATUS_FILE_IS_A_DIRECTORY;
    /* Write access on an existing file is let through: the handle simply
     * refuses writes. A title that asks for read/write on the disc and only
     * reads should not fail where the console would not notice. */
    d = new_handle(node);
    if (!d)
        return STATUS_INSUFFICIENT_RESOURCES;
    node_path(node, d->path, sizeof d->path);
    *out = (HANDLE)d;
    *information = 1;                       /* FILE_OPENED */
    return STATUS_SUCCESS;
}

NTSTATUS xbox_disc_read(HANDLE h, void *buf, ULONG length,
                        const LARGE_INTEGER *offset, ULONG_PTR *got)
{
    DiscHandle *d = (DiscHandle *)h;
    const DiscNode *n = &g_disc.nodes[d->node];
    int64_t at = (offset && offset->QuadPart >= 0) ? offset->QuadPart : d->pos;
    uint32_t want = length;
    int r;

    *got = 0;
    if (is_dir(d->node))
        return STATUS_INVALID_DEVICE_REQUEST;
    if (at >= (int64_t)n->size) {
        d->pos = at;
        return length ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    }
    if ((int64_t)want > (int64_t)n->size - at)
        want = (uint32_t)((int64_t)n->size - at);
    r = raw_read(g_disc.base + (uint64_t)n->sector * SECTOR + (uint64_t)at, buf, want);
    if (r < 0)
        return STATUS_UNSUCCESSFUL;
    d->pos = at + r;
    *got = (ULONG_PTR)r;
    return STATUS_SUCCESS;
}

int64_t xbox_disc_tell(HANDLE h)
{
    return ((DiscHandle *)h)->pos;
}

void xbox_disc_seek(HANDLE h, int64_t pos)
{
    ((DiscHandle *)h)->pos = pos < 0 ? 0 : pos;
}

NTSTATUS xbox_disc_query_info(HANDLE h, void *info, ULONG length, int info_class,
                              ULONG_PTR *written)
{
    DiscHandle *d = (DiscHandle *)h;
    int node = d->node;
    int64_t size = node_size(node);
    int64_t alloc = (size + SECTOR - 1) & ~(int64_t)(SECTOR - 1);

    *written = 0;
    switch (info_class) {
    case XboxFileBasicInformation: {
        PXBOX_FILE_BASIC_INFORMATION b = (PXBOX_FILE_BASIC_INFORMATION)info;
        if (length < sizeof *b) return STATUS_INFO_LENGTH_MISMATCH;
        fill_times(&b->CreationTime, &b->LastAccessTime, &b->LastWriteTime, &b->ChangeTime);
        b->FileAttributes = node_attributes(node);
        *written = sizeof *b;
        return STATUS_SUCCESS;
    }
    case XboxFileStandardInformation: {
        PXBOX_FILE_STANDARD_INFORMATION s = (PXBOX_FILE_STANDARD_INFORMATION)info;
        if (length < sizeof *s) return STATUS_INFO_LENGTH_MISMATCH;
        s->EndOfFile.QuadPart = size;
        s->AllocationSize.QuadPart = alloc;
        s->NumberOfLinks = 1;
        s->DeletePending = FALSE;
        s->Directory = is_dir(node) ? TRUE : FALSE;
        *written = sizeof *s;
        return STATUS_SUCCESS;
    }
    case XboxFilePositionInformation: {
        PXBOX_FILE_POSITION_INFORMATION p = (PXBOX_FILE_POSITION_INFORMATION)info;
        if (length < sizeof *p) return STATUS_INFO_LENGTH_MISMATCH;
        p->CurrentByteOffset.QuadPart = d->pos;
        *written = sizeof *p;
        return STATUS_SUCCESS;
    }
    case XboxFileNetworkOpenInformation: {
        PXBOX_FILE_NETWORK_OPEN_INFORMATION o = (PXBOX_FILE_NETWORK_OPEN_INFORMATION)info;
        if (length < sizeof *o) return STATUS_INFO_LENGTH_MISMATCH;
        fill_times(&o->CreationTime, &o->LastAccessTime, &o->LastWriteTime, &o->ChangeTime);
        o->EndOfFile.QuadPart = size;
        o->AllocationSize.QuadPart = alloc;
        o->FileAttributes = node_attributes(node);
        *written = sizeof *o;
        return STATUS_SUCCESS;
    }
    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "disc: NtQueryInformationFile class %d not handled", info_class);
        return STATUS_NOT_IMPLEMENTED;
    }
}

NTSTATUS xbox_disc_set_info(HANDLE h, const void *info, ULONG length, int info_class)
{
    if (info_class == XboxFilePositionInformation) {
        const XBOX_FILE_POSITION_INFORMATION *p = (const XBOX_FILE_POSITION_INFORMATION *)info;
        if (length < sizeof *p)
            return STATUS_INFO_LENGTH_MISMATCH;
        xbox_disc_seek(h, p->CurrentByteOffset.QuadPart);
        return STATUS_SUCCESS;
    }
    /* Size, times, delete: all writes. */
    return STATUS_MEDIA_WRITE_PROTECTED;
}

NTSTATUS xbox_disc_query_volume(HANDLE h, void *info, ULONG length, int fs_class,
                                ULONG_PTR *written)
{
    (void)h;
    *written = 0;
    if (fs_class == XboxFileFsSizeInformation) {
        PXBOX_FILE_FS_SIZE_INFORMATION s = (PXBOX_FILE_FS_SIZE_INFORMATION)info;
        uint64_t bytes = g_disc.image_size > g_disc.base ? g_disc.image_size - g_disc.base : 0;
        if (length < sizeof *s)
            return STATUS_INFO_LENGTH_MISMATCH;
        s->BytesPerSector = SECTOR;
        s->SectorsPerAllocationUnit = 1;
        s->TotalAllocationUnits.QuadPart = (LONGLONG)(bytes / SECTOR);
        s->AvailableAllocationUnits.QuadPart = 0;
        *written = sizeof *s;
        return STATUS_SUCCESS;
    }
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
             "disc: NtQueryVolumeInformationFile class %d not handled", fs_class);
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS xbox_disc_query_attributes(const char *disc_path,
                                    PXBOX_FILE_NETWORK_OPEN_INFORMATION o)
{
    int parent_missing, node = lookup(disc_path, &parent_missing);
    int64_t size;
    if (node < 0)
        return parent_missing ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
    size = node_size(node);
    fill_times(&o->CreationTime, &o->LastAccessTime, &o->LastWriteTime, &o->ChangeTime);
    o->EndOfFile.QuadPart = size;
    o->AllocationSize.QuadPart = (size + SECTOR - 1) & ~(int64_t)(SECTOR - 1);
    o->FileAttributes = node_attributes(node);
    return STATUS_SUCCESS;
}

/* '*' and '?' wildcards, case-insensitive, as FindFirstFile and fnmatch with
 * FNM_CASEFOLD treat a title's search mask. */
static int wild_match(const char *pat, const char *name)
{
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*')
                pat++;
            if (!*pat)
                return 1;
            for (; *name; name++)
                if (wild_match(pat, name))
                    return 1;
            return 0;
        }
        if (!*name)
            return 0;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*name))
            return 0;
        pat++;
        name++;
    }
    return *name == '\0';
}

NTSTATUS xbox_disc_query_directory(HANDLE h, void *info, ULONG length,
                                   const char *pattern, size_t pattern_len,
                                   int restart, ULONG_PTR *written)
{
    DiscHandle *d = (DiscHandle *)h;
    PXBOX_FILE_DIRECTORY_INFORMATION e = (PXBOX_FILE_DIRECTORY_INFORMATION)info;
    ULONG header = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
    int node;
    size_t name_len;

    *written = 0;
    if (!is_dir(d->node))
        return STATUS_INVALID_PARAMETER;
    if (restart || d->cursor == -2) {
        /* The first call fixes the mask, as both host backends do. */
        if (pattern && pattern_len) {
            if (pattern_len >= sizeof d->pattern)
                pattern_len = sizeof d->pattern - 1;
            memcpy(d->pattern, pattern, pattern_len);
            d->pattern[pattern_len] = '\0';
        } else {
            strcpy(d->pattern, "*");
        }
        d->cursor = g_disc.nodes[d->node].first_child;
    }
    for (node = d->cursor; node >= 0; node = g_disc.nodes[node].next_sibling)
        if (wild_match(d->pattern, g_disc.nodes[node].name))
            break;
    if (node < 0) {
        d->cursor = -1;
        return STATUS_NO_MORE_FILES;
    }
    d->cursor = g_disc.nodes[node].next_sibling;

    if (length < header)
        return STATUS_BUFFER_OVERFLOW;
    memset(e, 0, length < header + 64 ? length : header);
    name_len = strlen(g_disc.nodes[node].name);
    fill_times(&e->CreationTime, &e->LastAccessTime, &e->LastWriteTime, &e->ChangeTime);
    e->EndOfFile.QuadPart = node_size(node);
    e->AllocationSize.QuadPart = (e->EndOfFile.QuadPart + SECTOR - 1) & ~(int64_t)(SECTOR - 1);
    e->FileAttributes = node_attributes(node);
    e->FileNameLength = (ULONG)name_len;
    if (header + name_len <= length)
        memcpy(e->FileName, g_disc.nodes[node].name, name_len);
    *written = header + name_len;
    return STATUS_SUCCESS;
}

HANDLE xbox_disc_duplicate(HANDLE h)
{
    DiscHandle *src = (DiscHandle *)h, *d = new_handle(src->node);
    if (!d)
        return NULL;
    d->pos = src->pos;
    memcpy(d->path, src->path, sizeof d->path);
    return (HANDLE)d;
}

void xbox_disc_close(HANDLE h)
{
    ((DiscHandle *)h)->in_use = 0;
}
