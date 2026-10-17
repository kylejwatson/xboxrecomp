/*
 * kernel_disc.h -- an Xbox disc image (XDVDFS) mounted as the game disc.
 *
 * A player's game is a disc image more often than an extracted folder, and
 * extracting several gigabytes only so the runtime can open them file by file
 * is a step nobody should have to take. With an image mounted, every path the
 * title opens on the disc (D:\, \Device\CdRom0\, ...) is served from the
 * image: kernel_path.c translates such a path to "xboxdisc:/<path on the
 * disc>" instead of a host file, and kernel_file.c hands those to the
 * functions below. Saves never go to the image; see kernel_path.c.
 *
 * The image is read-only and so are its handles: create, overwrite and write
 * fail with STATUS_MEDIA_WRITE_PROTECTED, as a write to a DVD does.
 *
 * Accepted layouts are those of tools/xiso/xdvdfs.py: a bare game partition
 * (xiso), and full dumps where the game partition follows the video one
 * (Redump: 0x18300000).
 */
#ifndef XBOXRECOMP_KERNEL_DISC_H
#define XBOXRECOMP_KERNEL_DISC_H

#include "kernel.h"
#include <stddef.h>
#include <stdint.h>

/* What kernel_path.c puts in front of a path on the mounted image. */
#define XBOX_DISC_PATH_PREFIX "xboxdisc:"

/* Whether `path` is a regular file holding an XDVDFS volume. */
int  xbox_disc_is_image(const char *path);

/* Mount the image (once; a second call with the same path is a no-op).
 * Returns 1, or 0 with a sentence in err. */
int  xbox_disc_mount(const char *image_path, char *err, size_t err_bytes);
int  xbox_disc_mounted(void);
const char *xbox_disc_image_path(void);

/* A whole file from the mounted image, in malloc'd memory. */
int  xbox_disc_read_file(const char *disc_path, void **data, size_t *size);

/* The path on the mounted disc an Xbox path ("D:\\pak\\x.pak") names, or NULL
 * (kernel_path.c; prints nothing). Points into buf. */
const char *xbox_disc_path_of(const char *xbox_path, char *buf, size_t n);

/* The path on the disc a translated host path names ("pak/lv60.pak", "" for
 * the root), or NULL when it is not on the disc. The wide form narrows into
 * buf. */
const char *xbox_disc_host_rel(const char *host_path);
const char *xbox_disc_host_rel_w(const wchar_t *host_path, char *buf, size_t n);

/* ---- the kernel file layer's side ---- */

NTSTATUS xbox_disc_create(const char *disc_path, ACCESS_MASK access,
                          ULONG disposition, ULONG options,
                          HANDLE *out, ULONG_PTR *information);
/* Whether a handle is one of ours. Cheap: an address-range check. */
int      xbox_disc_owns(HANDLE h);
/* The path on the disc a handle was opened with (RootDirectory joins). */
const char *xbox_disc_handle_path(HANDLE h);
NTSTATUS xbox_disc_read(HANDLE h, void *buf, ULONG length,
                        const LARGE_INTEGER *offset, ULONG_PTR *got);
int64_t  xbox_disc_tell(HANDLE h);
void     xbox_disc_seek(HANDLE h, int64_t pos);
NTSTATUS xbox_disc_query_info(HANDLE h, void *info, ULONG length, int info_class,
                              ULONG_PTR *written);
NTSTATUS xbox_disc_set_info(HANDLE h, const void *info, ULONG length, int info_class);
NTSTATUS xbox_disc_query_volume(HANDLE h, void *info, ULONG length, int fs_class,
                                ULONG_PTR *written);
NTSTATUS xbox_disc_query_attributes(const char *disc_path,
                                    PXBOX_FILE_NETWORK_OPEN_INFORMATION info);
/* One entry per call, matching `pattern` (NULL or empty: all), as the host
 * backends do. */
NTSTATUS xbox_disc_query_directory(HANDLE h, void *info, ULONG length,
                                   const char *pattern, size_t pattern_len,
                                   int restart, ULONG_PTR *written);
HANDLE   xbox_disc_duplicate(HANDLE h);
void     xbox_disc_close(HANDLE h);

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH  ((NTSTATUS)0xC0000004L)
#endif
#ifndef STATUS_MEDIA_WRITE_PROTECTED
#define STATUS_MEDIA_WRITE_PROTECTED ((NTSTATUS)0xC00000A2L)
#endif
#ifndef STATUS_NOT_A_DIRECTORY
#define STATUS_NOT_A_DIRECTORY       ((NTSTATUS)0xC0000103L)
#endif
#ifndef STATUS_FILE_IS_A_DIRECTORY
#define STATUS_FILE_IS_A_DIRECTORY   ((NTSTATUS)0xC00000BAL)
#endif

#endif /* XBOXRECOMP_KERNEL_DISC_H */
