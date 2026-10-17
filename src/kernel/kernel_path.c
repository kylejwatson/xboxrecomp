/*
 * kernel_path.c - Xbox device-path translation
 *
 * Translates Xbox device-style paths to host filesystem paths:
 *   \Device\CdRom0\  -> <game_dir>/
 *   D:\               -> <game_dir>/
 *   T:\               -> <save_dir>/TitleData/
 *   U:\               -> <save_dir>/UserData/
 *   Z:\               -> <save_dir>/Cache/
 *
 * The Win32 build emits UTF-16 paths (for CreateFileW); the Linux build
 * emits UTF-8 paths with '/' separators (for open()).
 */

#include "kernel.h"
#include "kernel_disc.h"
#include "xbox_watchpoint.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/*
 * Helper: check if an ANSI string starts with a prefix (case-insensitive).
 * Returns the number of chars consumed from the prefix, or 0 if no match.
 * Platform-independent.
 */
static int match_prefix(const char* path, const char* prefix)
{
    int i = 0;
    while (prefix[i]) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)prefix[i]))
            return 0;
        i++;
    }
    return i;
}

/* A device-path translation rule, shared by both backends. */
typedef struct {
    const char* prefix;     /* Xbox path prefix (backslash form)         */
    int         to_save;    /* 1 = under save_dir, 0 = under game_dir    */
    const char* sub_win;    /* sub-directory, Win32 backslash form       */
    const char* sub_posix;  /* sub-directory, POSIX slash form           */
} path_rule;

static const path_rule s_rules[] = {
    { "\\Device\\CdRom0\\",                   0, NULL,         NULL          },
    /* The DVD drive itself, with nothing after it. A title whose certificate
     * allows only the DVD-X2 media type (TimeSplitters 2: AllowedMedia 0x2)
     * has XAPI open the bare device at start-up and send it a SCSI mode
     * sense for the disc's security page before main ever runs; with no
     * rule this returned STATUS_OBJECT_PATH_NOT_FOUND and the title went
     * back to the dashboard after fifteen kernel calls. Listed after the
     * slashed form so paths under the device keep their own mapping; the
     * empty remainder makes it the game directory, which NtCreateFile opens
     * as a directory handle. */
    { "\\Device\\CdRom0",                     0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition1\\",    0, NULL,         NULL          },
    /* The rest of the disk. Partition 0 is the whole raw device, 2 holds
     * system data, and 3-5 are the per-title caches behind X:, Y: and Z:.
     * Without these the path layer reported "Unrecognized Xbox path" and
     * NtOpenFile returned STATUS_OBJECT_PATH_NOT_FOUND; DSTEAL_JP probes
     * partition0 at startup and treats the failure as fatal.
     *
     * No trailing separator: the probe opens the device itself, with nothing
     * after it. Listed after Partition1 so that keeps its own mapping. */
    /* Partition2 is C:, the system partition. On a console that is where the
     * dashboard and its own assets live, and the dashboard opens them by
     * device path as well as through Y:, so a path *under* partition2 is an
     * ordinary asset read and belongs in the game dir -- the same place Y:
     * already goes. Only the bare device keeps the system-data image, which
     * is why this rule carries the trailing separator and is listed first. */
    { "\\Device\\Harddisk0\\Partition2\\",    0, NULL,         NULL          },
    { "C:\\",                                 0, NULL,         NULL          },
    { "\\??\\C:\\",                           0, NULL,         NULL          },
    { "\\Device\\Harddisk0\\Partition2",     1, "\\SystemData", "/SystemData" },
    { "\\Device\\Harddisk0\\Partition3",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition4",     1, "\\Cache",   "/Cache"      },
    { "\\Device\\Harddisk0\\Partition5",     1, "\\Cache",   "/Cache"      },
    { "D:\\",                                 0, NULL,         NULL          },
    { "d:\\",                                 0, NULL,         NULL          },
    /* Y: is the Xbox dashboard partition; the dashboard opens its assets
     * (e.g. "Y:\default.xip") from there. Map it to the game dir. */
    { "Y:\\",                                 0, NULL,         NULL          },
    { "y:\\",                                 0, NULL,         NULL          },
    { "T:\\",                                 1, "\\TitleData","/TitleData"  },
    { "U:\\",                                 1, "\\UserData", "/UserData"   },
    { "Z:\\",                                 1, "\\Cache",    "/Cache"      },
    { "\\??\\D:\\",                           0, NULL,         NULL          },
    { "\\??\\Y:\\",                           0, NULL,         NULL          },
    { "\\??\\y:\\",                           0, NULL,         NULL          },
    { "\\??\\T:\\",                           1, "\\TitleData","/TitleData"  },
};
#define PATH_RULE_COUNT ((int)(sizeof(s_rules) / sizeof(s_rules[0])))

/*
 * With a disc image mounted (kernel_disc.h), the game side of the rules is the
 * image -- except Partition1, which on a console is the hard disk's E: (UDATA,
 * TDATA) and goes to <save dir>/E, since nothing can be written to an image.
 * An extracted folder keeps the old mapping, saves and all.
 */
static int rule_is_partition1(int i)
{
    return strcmp(s_rules[i].prefix, "\\Device\\Harddisk0\\Partition1\\") == 0;
}

/* "xboxdisc:/<rest>" with '/' separators and no trailing one. */
static void disc_host_path(const char *remainder, char *out, size_t n)
{
    size_t len;
    char *q;
    snprintf(out, n, "%s/%s", XBOX_DISC_PATH_PREFIX, remainder);
    for (q = out; *q; q++)
        if (*q == '\\')
            *q = '/';
    len = strlen(out);
    while (len > sizeof XBOX_DISC_PATH_PREFIX && out[len - 1] == '/')
        out[--len] = '\0';
}

/* A game directory that is a disc image is mounted, once. */
static void mount_if_image(const char *game_dir)
{
    char err[1200];
    if (game_dir && !xbox_disc_mounted() && xbox_disc_is_image(game_dir) &&
        !xbox_disc_mount(game_dir, err, sizeof err))
        fprintf(stderr, "[DISC] %s\n", err);
}

/*
 * Rewrite a path through a drive letter the title mapped for itself.
 *
 * Titles do their own mounting. Wreckless calls IoCreateSymbolicLink at guest
 * 0x000E9B1D to link "\??\Z:" to "\Device\Harddisk0\Partition1\", then loads
 * every asset through z:\. kernel_io.c has recorded that link since it was
 * written, but nothing ever read the table back, so the static rule below --
 * Z: is the cache partition, which is true of the console in general and wrong
 * for this title -- sent every asset open to the save directory and it failed
 * with ERROR_FILE_NOT_FOUND.
 *
 * Accepts "Z:\rest" and "\??\Z:\rest"; the table is keyed on the "\??\Z:" form
 * the kernel actually registers.
 *
 * Deliberately conservative: the rewritten path is returned only when it
 * matches a static rule. A link whose target this layer has no rule for would
 * otherwise turn a path that translated adequately into one that does not
 * translate at all.
 *
 * Returns 1 and fills `out` on success, 0 to leave the path alone.
 */
static int resolve_symlink(const char* xbox_path, char* out, size_t out_size)
{
    char        link[8];
    const char* rest;
    const char* target;
    size_t      tlen, rlen;
    int         i;

    if (!xbox_path || !out || out_size == 0)
        return 0;

    if (match_prefix(xbox_path, "\\??\\") && xbox_path[4] && xbox_path[5] == ':')
        xbox_path += 4;
    if (!xbox_path[0] || xbox_path[1] != ':')
        return 0;
    rest = xbox_path + 2;

    link[0] = '\\';
    link[1] = '?';
    link[2] = '?';
    link[3] = '\\';
    link[4] = (char)toupper((unsigned char)xbox_path[0]);
    link[5] = ':';
    link[6] = '\0';

    target = xbox_LookupSymbolicLink(link);
    if (!target || !target[0])
        return 0;

    while (*rest == '\\' || *rest == '/')
        rest++;

    tlen = strlen(target);
    rlen = strlen(rest);
    if (tlen + rlen + 2 > out_size)
        return 0;

    memcpy(out, target, tlen);
    if (tlen && target[tlen - 1] != '\\' && target[tlen - 1] != '/')
        out[tlen++] = '\\';
    memcpy(out + tlen, rest, rlen);
    out[tlen + rlen] = '\0';

    for (i = 0; i < PATH_RULE_COUNT; i++) {
        if (match_prefix(out, s_rules[i].prefix))
            return 1;           /* the target is somewhere we can place */
    }
    return 0;
}

/* ======================================================================== */
/*
 * The raw disk's partition table, as XAPI reads it from sector 4 of
 * \Device\Harddisk0\Partition0 (both hosts back that device with an image
 * file, Partition0.img in the save directory). The standard retail geometry.
 */
#define XBOX_PART_TABLE_OFFSET 0x800
#define XBOX_PART_IN_USE       0x80000000u

/* Sizes of Partition0..5 in sectors, matching the table (0: whole disk). */
static const ULONGLONG xbox_part_sectors[6] = {
    0,             /* 0: whole disk, sized by the table's write */
    0x00465400ull, /* 1: E: game and saves            */
    0x000FA000ull, /* 2: C: system                    */
    0x00177000ull, /* 3: X: cache                     */
    0x00177000ull, /* 4: Y: cache                     */
    0x00177000ull, /* 5: Z: cache                     */
};

static void xbox_partition_table_sector(unsigned char sector[512])
{
    /* name[16], flags, lba_start, lba_size, reserved -- 32 bytes each */
    static const struct { const char *name; ULONG start, size; } parts[] = {
        { "XBOX_PART_X",  0x00000400, 0x00177000 },  /* X: cache      */
        { "XBOX_PART_Y",  0x00177400, 0x00177000 },  /* Y: cache      */
        { "XBOX_PART_Z",  0x002EE400, 0x00177000 },  /* Z: cache      */
        { "XBOX_PART_C",  0x00465400, 0x000FA000 },  /* C: system     */
        { "XBOX_PART_E",  0x0055F400, 0x00465400 },  /* E: game/save  */
    };
    size_t i;

    memset(sector, 0, 512);
    memcpy(sector, "****PARTINFO****", 16);
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        unsigned char *e = sector + 48 + i * 32;   /* 16 magic + 32 reserved */
        size_t n = strlen(parts[i].name);
        ULONG v;
        memset(e, ' ', 16);
        memcpy(e, parts[i].name, n < 16 ? n : 16);
        v = XBOX_PART_IN_USE; memcpy(e + 16, &v, 4);
        memcpy(e + 20, &parts[i].start, 4);
        memcpy(e + 24, &parts[i].size, 4);
        v = 0; memcpy(e + 28, &v, 4);
    }
}

/* "\Device\Harddisk0\PartitionN" with nothing below it: the digit, else -1.
 * Anything with a path under it is an ordinary filesystem access. */
static int xbox_partition_device_digit(const char *xbox_path)
{
    static const char *prefix = "\\Device\\Harddisk0\\Partition";
    int len = match_prefix(xbox_path, prefix);
    const char *rest;

    if (!len || xbox_path[len] < '0' || xbox_path[len] > '9')
        return -1;
    rest = xbox_path + len + 1;
    if (*rest == '\\' || *rest == '/')
        rest++;
    return *rest ? -1 : xbox_path[len] - '0';
}

/* The path on the mounted disc an Xbox path names, without printing or
 * creating anything (kernel_file.c asks this before the host backends see a
 * path). Returns NULL when no disc is mounted or the path is elsewhere. */
const char *xbox_disc_path_of(const char *xbox_path, char *buf, size_t n)
{
    char linked[512];
    int i, skip;
    if (!xbox_disc_mounted() || !xbox_path)
        return NULL;
    if (resolve_symlink(xbox_path, linked, sizeof linked))
        xbox_path = linked;
    if (xbox_partition_device_digit(xbox_path) >= 0)
        return NULL;
    for (i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (!skip)
            continue;
        if (s_rules[i].to_save || rule_is_partition1(i))
            return NULL;
        disc_host_path(xbox_path + skip, buf, n);
        return xbox_disc_host_rel(buf);
    }
    return NULL;
}

#if defined(_WIN32)
/* ======================================================================== */

#include <shlobj.h>
#include <winioctl.h>

static WCHAR s_game_dir[MAX_PATH];
static WCHAR s_save_dir[MAX_PATH];
static BOOL  s_initialized = FALSE;

/*
 * The raw disk device, \Device\Harddisk0\Partition0.
 *
 * A title opens it to read the Xbox partition table at sector 4 (offset 0x800)
 * and find the cache partitions behind X:, Y: and Z: -- XAPI's utility-drive
 * mount does exactly that, and DSTEAL_JP calls HalReturnToFirmware when the
 * read fails. A directory cannot answer a 512-byte read at a file offset, so
 * the device is backed by an image file instead.
 *
 * The table is the standard retail geometry. This is emulating a device that
 * has to be there, not fabricating anything the title owns.
 */
#define XBOX_DISK_IMAGE_NAME   L"Partition0.img"

static void xbox_write_partition_table(const WCHAR *path)
{
    unsigned char sector[512];
    HANDLE h;
    DWORD written;
    LARGE_INTEGER off;

    h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;

    xbox_partition_table_sector(sector);
    off.QuadPart = XBOX_PART_TABLE_OFFSET;
    if (SetFilePointerEx(h, off, NULL, FILE_BEGIN))
        WriteFile(h, sector, sizeof(sector), &written, NULL);
    CloseHandle(h);
}

/*
 * Map a partition device onto an image file.
 *
 * Matches "\Device\Harddisk0\PartitionN" with nothing below it, which is how a
 * device is opened. Anything with a path under it -- Partition1\TDATA and the
 * like -- is an ordinary filesystem access and falls through to the rules
 * table, which puts it in a directory.
 *
 * Partition0 is the whole disk and carries the partition table written above.
 * The rest start empty, which is what an unformatted partition looks like, so
 * a title that wants a filesystem there formats one.
 */
static BOOL xbox_partition_device_path(const char *xbox_path, WCHAR *out, DWORD n)
{
    int digit = xbox_partition_device_digit(xbox_path);

    if (digit < 0)
        return FALSE;
    swprintf_s(out, n, L"%s\\Partition%c.img", s_save_dir, (WCHAR)('0' + digit));
    return TRUE;
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    WCHAR save_base[MAX_PATH];

    mount_if_image(game_dir);

    /* The fallbacks used to name Burnout 3 specifically, so any other title
     * that passed NULL silently pointed its game dir and its saves at another
     * game's folders. Generic now -- a caller that wants a title-specific
     * location should pass one. */
    if (game_dir) {
        MultiByteToWideChar(CP_UTF8, 0, game_dir, -1, s_game_dir, MAX_PATH);
    } else {
        /* Neutral default: the CWD's "game" subdirectory, which is the
         * layout the game projects use (see templates/new-game). */
        GetCurrentDirectoryW(MAX_PATH, s_game_dir);
    }

    if (save_dir) {
        MultiByteToWideChar(CP_UTF8, 0, save_dir, -1, s_save_dir, MAX_PATH);
    } else {
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, save_base))) {
            swprintf_s(s_save_dir, MAX_PATH, L"%s\\xboxrecomp", save_base);
        } else {
            GetCurrentDirectoryW(MAX_PATH, s_save_dir);
            wcscat_s(s_save_dir, MAX_PATH, L"\\SaveData");
        }
    }

    size_t len = wcslen(s_game_dir);
    if (len > 0 && s_game_dir[len - 1] == L'\\')
        s_game_dir[len - 1] = L'\0';

    len = wcslen(s_save_dir);
    if (len > 0 && s_save_dir[len - 1] == L'\\')
        s_save_dir[len - 1] = L'\0';

    /* Create the save-side directories. T:/U:/Z: map into subdirectories of
     * save_dir, and a title that opens a file there with a create disposition
     * fails if the parent does not exist -- which reads as "cannot create save
     * file" and sends the title down its init-failure path. Halo asserts
     * exactly that at saved games/game_state_xbox.c:97 and then unwinds,
     * clearing global_d3d_device on the way out, so a missing directory
     * surfaces as a graphics failure.
     *
     * Cheap and idempotent: SHCreateDirectoryExW builds intermediates and is
     * happy if they already exist. */
    {
        static const WCHAR *subs[] = { L"TitleData", L"UserData", L"Cache",
                                       L"SystemData" };
        WCHAR image[MAX_PATH];
        WCHAR dir[MAX_PATH];
        SHCreateDirectoryExW(NULL, s_save_dir, NULL);
        for (int i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
            swprintf_s(dir, MAX_PATH, L"%s\\%s", s_save_dir, subs[i]);
            SHCreateDirectoryExW(NULL, dir, NULL);
        }
        swprintf_s(image, MAX_PATH, L"%s\\%s", s_save_dir, XBOX_DISK_IMAGE_NAME);
        xbox_write_partition_table(image);
        /* The other partition devices, sized to the same geometry the table
         * above describes, so a title that asks the device how big it is gets
         * an answer consistent with the table it just read. Marked sparse
         * first: the cache partitions are 750 MB each and none of that is
         * touched until something writes to it. */
        {
            for (int p = 1; p <= 5; p++) {
                HANDLE h;
                DWORD ret;
                LARGE_INTEGER end;

                swprintf_s(image, MAX_PATH, L"%s\\Partition%d.img",
                           s_save_dir, p);
                h = CreateFileW(image, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h == INVALID_HANDLE_VALUE)
                    continue;
                DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                                &ret, NULL);
                end.QuadPart = (LONGLONG)(xbox_part_sectors[p] * 512ull);
                if (SetFilePointerEx(h, end, NULL, FILE_BEGIN))
                    SetEndOfFile(h);
                CloseHandle(h);
            }
        }
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%S, save=%S", s_game_dir, s_save_dir);
}

/* The host path the last translation produced.
 *
 * A caller that wants to act on the file a title just opened -- playing an FMV
 * the host can decode itself, say -- has the guest path but not the host one,
 * and re-deriving it would duplicate every rule above. */
/* Thread-local: several threads open files at once, and a plain static let one
 * thread's translation overwrite another's between the translate and the read.
 * That showed up as the FMV trigger firing on roughly two runs in three. */
static __declspec(thread) wchar_t s_last_host_path[MAX_PATH];

static void xbox_remember_host_path(const wchar_t *p)
{
    if (p) wcsncpy_s(s_last_host_path, MAX_PATH, p, _TRUNCATE);
}

const wchar_t *xbox_LastHostPath(void)
{
    return s_last_host_path;
}


BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char*  remainder = NULL;
    const WCHAR* base_dir  = NULL;
    const char*  sub_dir   = NULL;
    int          skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    if (xbox_partition_device_path(xbox_path, host_path_buf, buf_size)) {
        fprintf(stderr, "  [PATH] %s -> partition image\n", xbox_path);
        fflush(stderr);
        return TRUE;
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_win;
            if (!s_rules[i].to_save && xbox_disc_mounted()) {
                if (rule_is_partition1(i)) {
                    base_dir = s_save_dir;
                    sub_dir  = "\\E";
                } else {
                    char disc[MAX_PATH * 2];
                    int k;
                    disc_host_path(remainder, disc, sizeof disc);
                    for (k = 0; disc[k] && k < (int)buf_size - 1; k++)
                        host_path_buf[k] = (WCHAR)(unsigned char)disc[k];
                    host_path_buf[k] = 0;
                    fprintf(stderr, "  [PATH] %s -> disc\n", xbox_path);
                    xbox_watch_note_path(xbox_path);
                    xbox_remember_host_path(host_path_buf);
                    return TRUE;
                }
            }
            goto translate;
        }
    }

    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    MultiByteToWideChar(CP_ACP, 0, xbox_path, -1, host_path_buf, buf_size);
    return TRUE;

translate:
    fprintf(stderr, "  [PATH] %s\n", xbox_path);
    fflush(stderr);
    xbox_watch_note_path(xbox_path);   /* RECOMP_WATCH_ARM_ON */
    {
        WCHAR remainder_wide[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, remainder, -1, remainder_wide, MAX_PATH);

        for (WCHAR* p = remainder_wide; *p; p++) {
            if (*p == L'/') *p = L'\\';
        }

        if (sub_dir) {
            WCHAR sub_wide[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, sub_dir, -1, sub_wide, MAX_PATH);
            swprintf_s(host_path_buf, buf_size, L"%s%s\\%s", base_dir, sub_wide, remainder_wide);

            WCHAR dir_path[MAX_PATH];
            swprintf_s(dir_path, MAX_PATH, L"%s%s", base_dir, sub_wide);
            CreateDirectoryW(s_save_dir, NULL);
            CreateDirectoryW(dir_path, NULL);
        } else {
            swprintf_s(host_path_buf, buf_size, L"%s\\%s", base_dir, remainder_wide);
        }

        /* An empty remainder means the title opened the device itself, so
         * the join above leaves a trailing separator -- which CreateFileW
         * rejects even with FILE_FLAG_BACKUP_SEMANTICS, turning an existing
         * directory into STATUS_OBJECT_PATH_NOT_FOUND. */
        {
            size_t n = wcslen(host_path_buf);
            while (n > 1 && (host_path_buf[n - 1] == L'\\'
                             || host_path_buf[n - 1] == L'/'))
                host_path_buf[--n] = L'\0';
        }

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %S", xbox_path, host_path_buf);
        xbox_remember_host_path(host_path_buf);
        return TRUE;
    }
}

/* ======================================================================== */
#else /* !_WIN32  -- POSIX / Linux */
/* ======================================================================== */

#include <unistd.h>
#include <stdlib.h>
#include <strings.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* Xbox paths are case-insensitive, as Windows' are, and a title asks for
 * "D:\\Pak\\LV60.PAK" when the extracted disc says pak/lv60.pak. Every
 * component that does not exist as spelled is looked up in its directory
 * ignoring case; the first one with no match ends the walk, so a file about
 * to be created keeps the title's spelling under the real parent. */
static void resolve_case(char *path, size_t size)
{
    char out[PATH_MAX], *rest, *comp, *save = NULL;
    size_t len;
    struct stat st;

    if (!path[0] || stat(path, &st) == 0)
        return;
    rest = strdup(path);
    if (!rest)
        return;
    out[0] = '\0';
    if (path[0] == '/')
        snprintf(out, sizeof out, "/");
    for (comp = strtok_r(rest, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
        char probe[PATH_MAX];
        DIR *d;
        struct dirent *de;
        const char *found = NULL;

        len = strlen(out);
        snprintf(probe, sizeof probe, "%s%s%s", out, (len && out[len - 1] != '/') ? "/" : "", comp);
        if (stat(probe, &st) == 0) {
            snprintf(out, sizeof out, "%s", probe);
            continue;
        }
        d = opendir(out[0] ? out : ".");
        while (d && (de = readdir(d)) != NULL) {
            if (strcasecmp(de->d_name, comp) == 0) {
                found = de->d_name;
                break;
            }
        }
        snprintf(probe, sizeof probe, "%s%s%s", out, (len && out[len - 1] != '/') ? "/" : "",
                 found ? found : comp);
        if (d)
            closedir(d);
        snprintf(out, sizeof out, "%s", probe);
        if (!found) {
            /* Nothing matches: the rest is the title's to create. */
            for (comp = strtok_r(NULL, "/", &save); comp; comp = strtok_r(NULL, "/", &save)) {
                len = strlen(out);
                snprintf(out + len, sizeof out - len, "/%s", comp);
            }
            break;
        }
    }
    free(rest);
    if (strlen(out) < size)
        memcpy(path, out, strlen(out) + 1);
}

static char s_game_dir[MAX_PATH];
static char s_save_dir[MAX_PATH];
static BOOL s_initialized = FALSE;

/* As on Windows: the last translated host path, per thread, for the FMV
 * trigger in kernel_bridge.c. wchar_t there, so widened here. */
static __thread wchar_t s_last_host_path[MAX_PATH * 2];

static void xbox_remember_host_path(const char *p)
{
    size_t i;

    for (i = 0; p && p[i] && i < MAX_PATH * 2 - 1; i++)
        s_last_host_path[i] = (wchar_t)(unsigned char)p[i];
    s_last_host_path[i] = 0;
}

const wchar_t *xbox_LastHostPath(void)
{
    return s_last_host_path;
}

/* Strip a single trailing '/' (but never the root '/'). */
static void strip_trailing_slash(char* s)
{
    size_t len = strlen(s);
    if (len > 1 && s[len - 1] == '/')
        s[len - 1] = '\0';
}

/* Recursively create a directory and all missing parents. */
static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "mkdir %s: %s", tmp, strerror(errno));
}

void xbox_path_init(const char* game_dir, const char* save_dir)
{
    mount_if_image(game_dir);
    if (game_dir) {
        snprintf(s_game_dir, sizeof(s_game_dir), "%s", game_dir);
    } else {
        /* Neutral default: CWD/"game", the layout game projects use. */
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), ".");
        snprintf(s_game_dir, sizeof(s_game_dir), "%s/game", cwd);
    }

    if (save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    } else {
        /* XDG base-directory spec: $XDG_DATA_HOME or ~/.local/share */
        const char* xdg = getenv("XDG_DATA_HOME");
        if (xdg && xdg[0]) {
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/xboxrecomp", xdg);
        } else {
            const char* home = getenv("HOME");
            snprintf(s_save_dir, sizeof(s_save_dir), "%s/.local/share/xboxrecomp",
                     (home && home[0]) ? home : ".");
        }
    }

    strip_trailing_slash(s_game_dir);
    strip_trailing_slash(s_save_dir);

    /* As on Windows: the save-side directories exist before a title creates
     * a file in one, and the partition devices are image files -- Partition0
     * holds the partition table XAPI reads, the rest are sparse files sized
     * to the geometry it describes. */
    {
        static const char *subs[] = { "TitleData", "UserData", "Cache", "SystemData" };
        char path[MAX_PATH * 2];
        unsigned char sector[512];
        int i, fd;

        mkdir_p(s_save_dir);
        for (i = 0; i < (int)(sizeof(subs) / sizeof(subs[0])); i++) {
            snprintf(path, sizeof(path), "%s/%s", s_save_dir, subs[i]);
            mkdir_p(path);
        }
        snprintf(path, sizeof(path), "%s/Partition0.img", s_save_dir);
        fd = open(path, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) {
            xbox_partition_table_sector(sector);
            if (pwrite(fd, sector, sizeof(sector), XBOX_PART_TABLE_OFFSET) != (ssize_t)sizeof(sector))
                xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "partition table write: %s", strerror(errno));
            close(fd);
        }
        for (i = 1; i <= 5; i++) {
            struct stat st;
            snprintf(path, sizeof(path), "%s/Partition%d.img", s_save_dir, i);
            fd = open(path, O_WRONLY | O_CREAT, 0644);
            if (fd < 0)
                continue;
            if (fstat(fd, &st) == 0 && (ULONGLONG)st.st_size != xbox_part_sectors[i] * 512ull)
                if (ftruncate(fd, (off_t)(xbox_part_sectors[i] * 512ull)) != 0)
                    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "%s: %s", path, strerror(errno));
            close(fd);
        }
    }

    s_initialized = TRUE;
    xbox_log(XBOX_LOG_INFO, XBOX_LOG_PATH, "Path init: game=%s, save=%s",
             s_game_dir, s_save_dir);
}

BOOL xbox_translate_path(const char* xbox_path, xbox_host_char* host_path_buf, DWORD buf_size)
{
    const char* remainder = NULL;
    const char* base_dir  = NULL;
    const char* sub_dir   = NULL;
    int         skip;

    if (!xbox_path || !host_path_buf || buf_size == 0)
        return FALSE;

    if (!s_initialized)
        xbox_path_init(NULL, NULL);

    {
        char linked[512];
        if (resolve_symlink(xbox_path, linked, sizeof(linked)))
            return xbox_translate_path(linked, host_path_buf, buf_size);
    }

    {
        int digit = xbox_partition_device_digit(xbox_path);
        if (digit >= 0) {
            snprintf(host_path_buf, buf_size, "%s/Partition%d.img", s_save_dir, digit);
            fprintf(stderr, "  [PATH] %s -> partition image\n", xbox_path);
            fflush(stderr);
            xbox_remember_host_path(host_path_buf);
            return TRUE;
        }
    }

    for (int i = 0; i < PATH_RULE_COUNT; i++) {
        skip = match_prefix(xbox_path, s_rules[i].prefix);
        if (skip) {
            remainder = xbox_path + skip;
            base_dir  = s_rules[i].to_save ? s_save_dir : s_game_dir;
            sub_dir   = s_rules[i].sub_posix;
            if (!s_rules[i].to_save && xbox_disc_mounted()) {
                if (rule_is_partition1(i)) {
                    base_dir = s_save_dir;
                    sub_dir  = "/E";
                } else {
                    disc_host_path(remainder, host_path_buf, buf_size);
                    fprintf(stderr, "  [PATH] %s -> disc\n", xbox_path);
                    xbox_watch_note_path(xbox_path);
                    xbox_remember_host_path(host_path_buf);
                    return TRUE;
                }
            }
            goto translate;
        }
    }

    /* Unrecognized path: pass through, just normalize separators. */
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_PATH, "Unrecognized Xbox path: %s", xbox_path);
    snprintf(host_path_buf, buf_size, "%s", xbox_path);
    for (char* p = host_path_buf; *p; p++)
        if (*p == '\\') *p = '/';
    return TRUE;

translate:
    fprintf(stderr, "  [PATH] %s\n", xbox_path);
    fflush(stderr);
    xbox_watch_note_path(xbox_path);   /* RECOMP_WATCH_ARM_ON */
    {
        char remainder_posix[MAX_PATH];
        snprintf(remainder_posix, sizeof(remainder_posix), "%s", remainder);

        /* Xbox paths use backslashes -> POSIX slashes. */
        for (char* p = remainder_posix; *p; p++)
            if (*p == '\\') *p = '/';

        if (sub_dir) {
            snprintf(host_path_buf, buf_size, "%s%s/%s",
                     base_dir, sub_dir, remainder_posix);

            /* Ensure the save directory tree exists. */
            char dir_path[MAX_PATH];
            snprintf(dir_path, sizeof(dir_path), "%s%s", base_dir, sub_dir);
            mkdir_p(dir_path);
        } else {
            snprintf(host_path_buf, buf_size, "%s/%s", base_dir, remainder_posix);
        }

        {
            size_t n = strlen(host_path_buf);
            while (n > 1 && host_path_buf[n - 1] == '/')
                host_path_buf[--n] = '\0';
        }
        resolve_case(host_path_buf, buf_size);
        xbox_remember_host_path(host_path_buf);

        XBOX_TRACE(XBOX_LOG_PATH, "%s -> %s", xbox_path, host_path_buf);
        return TRUE;
    }
}

#endif /* _WIN32 */
