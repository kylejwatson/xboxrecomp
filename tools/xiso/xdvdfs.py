"""
XDVDFS (Xbox DVD File System) reader.

Enough of the format to list a disc image and pull files out of it -- which for
this toolkit means `default.xbe` and the occasional companion executable. See
docs/formats/disc-image.md for the format notes this implements.

Why this exists when xdvdfs-cli and extract-xiso both do the job: the pipeline's
first step is "get me the XBE", and requiring a Rust toolchain or a prebuilt
binary for it turns a five-line quickstart into a paragraph of setup. Reading a
tree of fixed-size directory entries is not worth a dependency.

Deliberately not implemented: the video partition and file timestamps. Nothing
in the pipeline reads them. write_image() builds an image from a folder (for
testing the runtime's own reader, src/kernel/kernel_disc.c, without a disc).
"""

import os
import struct

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"

# A disc image may be a bare game partition, or carry a video partition in front
# of it. Rather than guess from the file size, probe the descriptor at each known
# game-partition base and take the one that has the magic.
#
#   0x00000000  bare game partition (what most tools emit)
#   0x0000FD90  some cracked/rebuilt images
#   0x00030600  redump-style, video partition first
#   0x0FD90000  full "xbox disc" layout
#   0x18300000  redump full-disc dump: the game partition follows the ~387 MB
#               DVD-video partition (magic lands at 0x18310000). Confirmed on the
#               Blinx / Conker / Fuzion Frenzy redump images.
BASE_CANDIDATES = (0x00000000, 0x0000FD90, 0x00030600, 0x0FD90000, 0x18300000)

ATTR_DIRECTORY = 0x10


class DirEntry:
    __slots__ = ("name", "sector", "size", "attributes")

    def __init__(self, name, sector, size, attributes):
        self.name = name
        self.sector = sector
        self.size = size
        self.attributes = attributes

    @property
    def is_dir(self):
        return bool(self.attributes & ATTR_DIRECTORY)

    def __repr__(self):
        return "<%s %s %d bytes>" % (
            "dir" if self.is_dir else "file", self.name, self.size)


class XisoError(Exception):
    pass


class Xiso:
    def __init__(self, path):
        self.path = path
        self._f = open(path, "rb")
        self.base = self._find_base()
        self._f.seek(self.base + 32 * SECTOR)
        desc = self._f.read(SECTOR)
        if not desc.startswith(MAGIC):
            raise XisoError("no XDVDFS descriptor at base 0x%X" % self.base)
        self.root_sector, self.root_size = struct.unpack_from("<II", desc, 0x14)

    def close(self):
        self._f.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def _find_base(self):
        for base in BASE_CANDIDATES:
            try:
                self._f.seek(base + 32 * SECTOR)
            except OSError:
                continue
            if self._f.read(len(MAGIC)) == MAGIC:
                return base
        raise XisoError(
            "%s is not an XDVDFS image (no %s at sector 32 of any known "
            "partition base)" % (os.path.basename(self.path), MAGIC.decode()))

    def _read_at(self, sector, size):
        self._f.seek(self.base + sector * SECTOR)
        return self._f.read(size)

    def _read_dir(self, sector, size):
        """Parse one directory's entries.

        Entries form a binary tree, but they are also laid out contiguously
        within each 2048-byte sector, so a linear walk with per-sector resync
        reads all of them without following left/right offsets. That matters
        because a malformed or padded tree link would otherwise drop whole
        subtrees silently -- and losing a file quietly is worse than being slow.
        """
        data = self._read_at(sector, size)
        entries = []
        for base in range(0, len(data), SECTOR):
            chunk = data[base:base + SECTOR]
            off = 0
            while off + 0x0E <= len(chunk):
                l, r, sec, sz, attr, nlen = struct.unpack_from(
                    "<HHIIBB", chunk, off)
                # 0xFFFF/0xFFFF padding, or a zero-length name: end of this
                # sector's entries.
                if (l == 0xFFFF and r == 0xFFFF) or nlen == 0:
                    break
                name = chunk[off + 0x0E:off + 0x0E + nlen]
                if len(name) < nlen:
                    break
                try:
                    name = name.decode("ascii")
                except UnicodeDecodeError:
                    break
                entries.append(DirEntry(name, sec, sz, attr))
                off += (0x0E + nlen + 3) & ~3   # 4-byte aligned
        return entries

    def listdir(self, path=""):
        """Entries in a directory. Empty path is the root."""
        sector, size = self.root_sector, self.root_size
        if path:
            for part in path.replace("\\", "/").strip("/").split("/"):
                match = next((e for e in self._read_dir(sector, size)
                              if e.name.lower() == part.lower() and e.is_dir),
                             None)
                if match is None:
                    raise XisoError("no such directory: %s" % path)
                sector, size = match.sector, match.size
        return self._read_dir(sector, size)

    def find(self, name, path=""):
        """One entry by name, case-insensitively. None if absent."""
        return next((e for e in self.listdir(path)
                     if e.name.lower() == name.lower()), None)

    def read(self, entry):
        return self._read_at(entry.sector, entry.size)

    def extract(self, entry, dest_path):
        os.makedirs(os.path.dirname(os.path.abspath(dest_path)), exist_ok=True)
        with open(dest_path, "wb") as out:
            remaining = entry.size
            self._f.seek(self.base + entry.sector * SECTOR)
            while remaining > 0:
                chunk = self._f.read(min(1 << 20, remaining))
                if not chunk:
                    raise XisoError("image truncated reading %s" % entry.name)
                out.write(chunk)
                remaining -= len(chunk)
        return dest_path

    def walk(self, path=""):
        """Yield (dirpath, entry) for every file, recursively."""
        for e in self.listdir(path):
            sub = (path + "/" + e.name).lstrip("/")
            if e.is_dir:
                yield from self.walk(sub)
            else:
                yield path, e


def _dir_entries_bytes(entries):
    """Lay out one directory's entries as XDVDFS wants them: a binary search
    tree (names compared upper-cased) whose root is the first entry, entries
    4-byte aligned and never crossing a sector, 0xFF padding. `entries` are
    (name, sector, size, attr). Returns bytes, a multiple of SECTOR."""
    order = sorted(entries, key=lambda e: e[0].upper())

    def build(lo, hi):                        # balanced tree over order[lo:hi]
        if lo >= hi:
            return None
        mid = (lo + hi) // 2
        return (mid, build(lo, mid), build(mid + 1, hi))

    pre = []

    def walk(node):
        if node:
            pre.append(node)
            walk(node[1])
            walk(node[2])

    walk(build(0, len(order)))
    offsets, at = {}, 0
    for mid, _, _ in pre:
        n = (0x0E + len(order[mid][0]) + 3) & ~3
        if at // SECTOR != (at + n - 1) // SECTOR:
            at = (at // SECTOR + 1) * SECTOR
        offsets[mid] = at
        at += n
    out = bytearray(b"\xFF" * (((at + SECTOR - 1) // SECTOR) * SECTOR or SECTOR))
    for mid, left, right in pre:
        name, sector, size, attr = order[mid]
        raw = name.encode("ascii")
        struct.pack_into("<HHIIBB", out, offsets[mid],
                         offsets[left[0]] // 4 if left else 0,
                         offsets[right[0]] // 4 if right else 0,
                         sector, size, attr, len(raw))
        o = offsets[mid] + 0x0E
        out[o:o + len(raw)] = raw
        pad = (0x0E + len(raw) + 3) & ~3
        out[o + len(raw):offsets[mid] + pad] = b"\xFF" * (pad - 0x0E - len(raw))
    return bytes(out)


def write_image(src_dir, out_path, base=0, exclude=()):
    """Write the folder src_dir as an XDVDFS image with the game partition at
    `base` (0 for an xiso, 0x18300000 for a Redump-style layout; the space in
    front is left sparse). Names in `exclude` (any case, top level) are left
    out. Returns the number of files written."""
    src_dir = os.path.abspath(src_dir)
    skip = {e.lower() for e in exclude}

    def scan(path, top):
        kids = []
        for name in sorted(os.listdir(path)):
            if top and name.lower() in skip:
                continue
            full = os.path.join(path, name)
            if os.path.isdir(full):
                kids.append([name, True, scan(full, False), 0, 0])
            elif os.path.isfile(full):
                kids.append([name, False, full, os.path.getsize(full), 0])
        return kids

    root = ["", True, scan(src_dir, True), 0, 0]
    # Sectors: directories first, then file data, all from sector 33.
    next_sector = [33]
    dirs, files = [], []

    def size_dirs(node):
        # A directory's size depends only on its names, so lay out with dummy
        # sectors first to learn it.
        node[3] = len(_dir_entries_bytes([(k[0], 0, 0, 0) for k in node[2]]))
        node[4] = next_sector[0]
        next_sector[0] += node[3] // SECTOR
        dirs.append(node)
        for k in node[2]:
            if k[1]:
                size_dirs(k)

    size_dirs(root)
    for d in dirs:
        for k in d[2]:
            if not k[1]:
                k[4] = next_sector[0]
                next_sector[0] += max(1, (k[3] + SECTOR - 1) // SECTOR)
                files.append(k)

    with open(out_path, "wb") as f:
        desc = bytearray(SECTOR)
        desc[0:20] = MAGIC
        struct.pack_into("<IIQ", desc, 0x14, root[4], root[3], 0)
        desc[0x7EC:0x7EC + 20] = MAGIC
        f.seek(base + 32 * SECTOR)
        f.write(desc)
        for d in dirs:
            f.seek(base + d[4] * SECTOR)
            f.write(_dir_entries_bytes([(k[0], k[4], k[3], ATTR_DIRECTORY if k[1] else 0x20)
                                        for k in d[2]]))
        for k in files:
            f.seek(base + k[4] * SECTOR)
            with open(k[2], "rb") as src:
                while True:
                    chunk = src.read(1 << 22)
                    if not chunk:
                        break
                    f.write(chunk)
        f.truncate(base + next_sector[0] * SECTOR)
    return len(files)
