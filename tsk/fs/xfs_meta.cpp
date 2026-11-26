/*
 * The Sleuth Kit
 *
 * This software is distributed under the Common Public License 1.0
 *
 * Read-only XFS support (the fork's, #11): inodes and file content.
 *
 * Inode numbers and AG addresses, inode allocation (the AG's inode B+tree),
 * inode lookup, the inode walk, symlink targets, the data fork as the
 * default attribute, and istat. See xfs.cpp for the address scheme and
 * docs/llm/fs.md ("XFS").
 *
 * C++14. Nothing here throws: no C++ exception can reach the C callers.
 */

#include "tsk_xfs.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

// The on-disk structures must match the disk layout.
static_assert(sizeof(xfs_dinode) == XFS_DINODE_CORE_SIZE_V3, "");
static_assert(offsetof(xfs_dinode, di_crc) == XFS_DINODE_CORE_SIZE_V2, "");
static_assert(offsetof(xfs_dinode, di_size) == 56, "");
static_assert(offsetof(xfs_dinode, di_crtime) == 144, "");
static_assert(sizeof(xfs_bmbt_rec) == 16, "");
static_assert(sizeof(xfs_inobt_rec) == 16, "");
static_assert(sizeof(xfs_dsymlink_hdr) == 56, "");

namespace tsk_xfs {

namespace {

/* --- Addresses --- */

/* The number of blocks of AG `agno`: the last AG may be shorter. */
uint32_t
ag_length(const XFS_INFO *xfs, uint32_t agno)
{
    if (agno + 1 < xfs->agcount) {
        return xfs->agblocks;
    }
    const TSK_FS_INFO *fs = &xfs->fs_info;
    return (uint32_t)(fs->block_count - uint64_t{agno} * xfs->agblocks);
}

/* The TSK (linear) block address of block `agbno` of AG `agno`. */
TSK_DADDR_T
ag_block(const XFS_INFO *xfs, uint32_t agno, uint32_t agbno)
{
    return TSK_DADDR_T{agno} * xfs->agblocks + agbno;
}

/* Reads block `agbno` of AG `agno` (which must exist) into `buf`, one block
 * long. On a short read: TSK_ERR_FS_READ. */
bool
read_ag_block(XFS_INFO *xfs, uint32_t agno, uint32_t agbno, char *buf)
{
    TSK_FS_INFO *fs = &xfs->fs_info;
    const ssize_t cnt =
        tsk_fs_read_block(fs, ag_block(xfs, agno, agbno), buf, fs->block_size);
    if (cnt != (ssize_t)fs->block_size) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_FS_READ);
        }
        return false;
    }
    return true;
}

/* An inode number split into its AG, its block in the AG and its index in
 * the block (see the top of this file). */
struct InodeAddr {
    uint32_t agno;
    uint32_t agbno;
    uint32_t index;
};

/* Splits `inum`; false if no AG block of the file system has that number. */
bool
split_inum(const XFS_INFO *xfs, TSK_INUM_T inum, InodeAddr &a)
{
    const unsigned agino_bits = xfs->agblklog + xfs->inopblog;
    const uint64_t agno = inum >> agino_bits;
    if (agno >= xfs->agcount) {
        return false;
    }
    a.agno = (uint32_t)agno;
    a.agbno = (uint32_t)((inum >> xfs->inopblog) &
        ((uint64_t{1} << xfs->agblklog) - 1));
    a.index = (uint32_t)(inum & ((uint64_t{1} << xfs->inopblog) - 1));
    return a.agbno < ag_length(xfs, a.agno);
}

/* --- Inode allocation: the AG's inode B+tree --- */

enum class Alloc { ALLOCATED, FREE, FAILED };

/*
 * The root block and the number of levels of AG `agno`'s inode B+tree, from
 * its AGI. Errors: a short read (TSK_ERR_FS_READ), an AGI that is not one,
 * not this AG's, or with a root outside the AG or an impossible level
 * (TSK_ERR_FS_CORRUPT).
 */
bool
read_agi(XFS_INFO *xfs, uint32_t agno, uint32_t &root, uint32_t &level)
{
    TSK_FS_INFO *fs = &xfs->fs_info;
    xfs_agi agi;
    const TSK_OFF_T agi_off =
        (TSK_OFF_T)ag_block(xfs, agno, 0) * fs->block_size +
        XFS_AGI_SECTOR * xfs->sectsize;
    const ssize_t cnt =
        tsk_fs_read(fs, agi_off, reinterpret_cast<char *>(&agi), sizeof(agi));
    if (cnt != (ssize_t)sizeof(agi)) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_FS_READ);
        }
        return false;
    }
    level = tsk_getu32(TSK_BIG_ENDIAN, agi.agi_level);
    root = tsk_getu32(TSK_BIG_ENDIAN, agi.agi_root);
    if (tsk_getu32(TSK_BIG_ENDIAN, agi.agi_magicnum) != XFS_AGI_MAGIC ||
        tsk_getu32(TSK_BIG_ENDIAN, agi.agi_versionnum) != XFS_AGI_VERSION ||
        tsk_getu32(TSK_BIG_ENDIAN, agi.agi_seqno) != agno || level == 0 ||
        level > XFS_BTREE_MAXLEVELS || root >= ag_length(xfs, agno)) {
        set_corrupt();
        return false;
    }
    return true;
}

/*
 * Whether the inode at `a` is allocated, as the inode B+tree of its AG
 * says: allocated if a chunk record covers it and its ir_free bit is clear.
 * An inode in no chunk is free. Errors: a short read (TSK_ERR_FS_READ), an
 * AGI or B+tree block that is not one or out of order (TSK_ERR_FS_CORRUPT).
 * Each step down must go down one level, so the walk ends.
 */
Alloc
inode_allocation(XFS_INFO *xfs, const InodeAddr &a)
{
    TSK_FS_INFO *fs = &xfs->fs_info;
    const uint32_t agino = (a.agbno << xfs->inopblog) | a.index;
    const uint32_t ag_blocks = ag_length(xfs, a.agno);

    uint32_t block = 0;
    uint32_t level = 0;
    if (!read_agi(xfs, a.agno, block, level)) {
        return Alloc::FAILED;
    }

    const bool crc = xfs->version == XFS_SB_VERSION_5;
    const uint32_t magic = crc ? XFS_IBT_CRC_MAGIC : XFS_IBT_MAGIC;
    const size_t hdr = crc ? XFS_BTREE_SBLOCK_CRC_LEN : XFS_BTREE_SBLOCK_LEN;
    const size_t space = fs->block_size - hdr;
    std::vector<char> buf(fs->block_size);
    const uint8_t *b = reinterpret_cast<const uint8_t *>(buf.data());
    for (uint32_t want = level - 1;; --want) {
        if (!read_ag_block(xfs, a.agno, block, buf.data())) {
            return Alloc::FAILED;
        }
        const xfs_btree_sblock *h =
            reinterpret_cast<const xfs_btree_sblock *>(b);
        const uint16_t numrecs = tsk_getu16(TSK_BIG_ENDIAN, h->bb_numrecs);
        if (tsk_getu32(TSK_BIG_ENDIAN, h->bb_magic) != magic ||
            tsk_getu16(TSK_BIG_ENDIAN, h->bb_level) != want) {
            set_corrupt();
            return Alloc::FAILED;
        }
        if (want == 0) {
            if (numrecs > space / sizeof(xfs_inobt_rec)) {
                set_corrupt();
                return Alloc::FAILED;
            }
            const xfs_inobt_rec *recs =
                reinterpret_cast<const xfs_inobt_rec *>(b + hdr);
            for (uint16_t i = 0; i < numrecs; ++i) {
                const uint32_t start =
                    tsk_getu32(TSK_BIG_ENDIAN, recs[i].ir_startino);
                if (agino >= start && agino - start < XFS_INODES_PER_CHUNK) {
                    const uint64_t free_mask =
                        tsk_getu64(TSK_BIG_ENDIAN, recs[i].ir_free);
                    return ((free_mask >> (agino - start)) & 1) != 0
                        ? Alloc::FREE
                        : Alloc::ALLOCATED;
                }
            }
            return Alloc::FREE;
        }
        // A node: 4-byte keys, then (after room for the most keys a block
        // holds) 4-byte pointers. Go down the last key not above agino.
        const size_t maxrecs = space / 8;
        if (numrecs == 0 || numrecs > maxrecs) {
            set_corrupt();
            return Alloc::FAILED;
        }
        const uint8_t *keys = b + hdr;
        const uint8_t *ptrs = keys + maxrecs * 4;
        int found = -1;
        for (uint16_t i = 0; i < numrecs; ++i) {
            if (tsk_getu32(TSK_BIG_ENDIAN, keys + 4 * i) > agino) {
                break;
            }
            found = i;
        }
        if (found < 0) {
            return Alloc::FREE;
        }
        block = tsk_getu32(TSK_BIG_ENDIAN, ptrs + 4 * found);
        if (block >= ag_blocks) {
            set_corrupt();
            return Alloc::FAILED;
        }
    }
}

/* The records of an AG's inode B+tree being collected, in inode order. */
struct InobtWalk {
    XFS_INFO *xfs;
    uint32_t agno;
    uint64_t next_agino; /* where the next record may start */
    std::vector<xfs_inobt_rec> recs;
};

/*
 * Appends the records of the inode B+tree subtree at AG block `block`, which
 * must be at `level`, in inode order. Each step down goes down one level,
 * every node has records and each chunk starts after the one before, so a
 * tree that loops back on itself is an error, not an endless walk. A leaf
 * may be empty: the root of an AG without inodes is. Errors: a short read
 * (TSK_ERR_FS_READ); a block outside the AG, of another magic or level, a
 * node with no records, a block with too many, or a chunk that overlaps the one
 * before or is not inside the AG (TSK_ERR_FS_CORRUPT). Chunks need not be
 * aligned to 64 inodes (on v4 they are aligned to blocks).
 */
bool
inobt_subtree(InobtWalk &walk, uint32_t block, unsigned level)
{
    XFS_INFO *xfs = walk.xfs;
    TSK_FS_INFO *fs = &xfs->fs_info;
    const uint32_t ag_blocks = ag_length(xfs, walk.agno);
    if (block >= ag_blocks) {
        set_corrupt();
        return false;
    }
    std::vector<char> buf(fs->block_size);
    if (!read_ag_block(xfs, walk.agno, block, buf.data())) {
        return false;
    }
    const bool crc = xfs->version == XFS_SB_VERSION_5;
    const size_t hdr = crc ? XFS_BTREE_SBLOCK_CRC_LEN : XFS_BTREE_SBLOCK_LEN;
    const size_t space = fs->block_size - hdr;
    const uint8_t *b = reinterpret_cast<const uint8_t *>(buf.data());
    const xfs_btree_sblock *h = reinterpret_cast<const xfs_btree_sblock *>(b);
    const size_t numrecs = tsk_getu16(TSK_BIG_ENDIAN, h->bb_numrecs);
    // A leaf holds 16-byte records, a node 4-byte keys and pointers.
    const size_t maxrecs =
        level == 0 ? space / sizeof(xfs_inobt_rec) : space / 8;
    if (tsk_getu32(TSK_BIG_ENDIAN, h->bb_magic) !=
            (crc ? XFS_IBT_CRC_MAGIC : XFS_IBT_MAGIC) ||
        tsk_getu16(TSK_BIG_ENDIAN, h->bb_level) != level ||
        (level > 0 && numrecs == 0) || numrecs > maxrecs) {
        set_corrupt();
        return false;
    }
    if (level > 0) {
        const uint8_t *ptrs = b + hdr + maxrecs * 4;
        for (size_t i = 0; i < numrecs; ++i) {
            if (!inobt_subtree(walk, tsk_getu32(TSK_BIG_ENDIAN, ptrs + 4 * i),
                    level - 1)) {
                return false;
            }
        }
        return true;
    }
    // The AG's inode numbers end with its last block.
    const uint64_t ag_inodes = uint64_t{ag_blocks} << xfs->inopblog;
    const xfs_inobt_rec *recs =
        reinterpret_cast<const xfs_inobt_rec *>(b + hdr);
    for (size_t i = 0; i < numrecs; ++i) {
        const uint32_t start = tsk_getu32(TSK_BIG_ENDIAN, recs[i].ir_startino);
        if (start < walk.next_agino ||
            uint64_t{start} + XFS_INODES_PER_CHUNK > ag_inodes) {
            set_corrupt();
            return false;
        }
        walk.next_agino = uint64_t{start} + XFS_INODES_PER_CHUNK;
        walk.recs.push_back(recs[i]);
    }
    return true;
}

/* The records of AG `agno`'s inode B+tree, in inode order. Errors: those of
 * read_agi and inobt_subtree. */
bool
inobt_records(XFS_INFO *xfs, uint32_t agno, std::vector<xfs_inobt_rec> &recs)
{
    uint32_t root = 0;
    uint32_t level = 0;
    if (!read_agi(xfs, agno, root, level)) {
        return false;
    }
    InobtWalk walk{xfs, agno, 0, {}};
    if (!inobt_subtree(walk, root, level - 1)) {
        return false;
    }
    recs.swap(walk.recs);
    return true;
}

/* --- Inodes --- */

/* The size of the inode core of an inode version: the data fork follows. */
size_t
dinode_core_size(uint8_t version)
{
    return version >= 3 ? XFS_DINODE_CORE_SIZE_V3 : XFS_DINODE_CORE_SIZE_V2;
}

} // namespace

DinodeView
dinode_view(const XFS_INFO *xfs, const uint8_t *raw)
{
    DinodeView d;
    d.core = reinterpret_cast<const xfs_dinode *>(raw);
    d.mode = tsk_getu16(TSK_BIG_ENDIAN, d.core->di_mode);
    d.version = d.core->di_version;
    d.format = d.core->di_format;
    d.flags = tsk_getu16(TSK_BIG_ENDIAN, d.core->di_flags);
    d.flags2 =
        d.version >= 3 ? tsk_getu64(TSK_BIG_ENDIAN, d.core->di_flags2) : 0;
    d.nextents = (d.flags2 & XFS_DIFLAG2_NREXT64)
        ? tsk_getu64(TSK_BIG_ENDIAN, d.core->di_big_nextents)
        : tsk_getu32(TSK_BIG_ENDIAN, d.core->di_nextents);
    const size_t core = dinode_core_size(d.version);
    d.fork = raw + core;
    d.fork_size = d.core->di_forkoff != 0 ? size_t{d.core->di_forkoff} * 8
                                          : xfs->inodesize - core;
    return d;
}

namespace {

/* Whether the raw inode is a sound one for `inum`: the magic, an inode
 * version the file system's version allows (v5: 3 only, v4: 1 or 2), on v3
 * its own inode number, a mode (if `need_mode`: a free inode has none), and
 * a data fork inside the inode. */
bool
dinode_sound(
    const XFS_INFO *xfs, TSK_INUM_T inum, const uint8_t *raw, bool need_mode)
{
    const xfs_dinode *core = reinterpret_cast<const xfs_dinode *>(raw);
    const uint8_t v = core->di_version;
    if (tsk_getu16(TSK_BIG_ENDIAN, core->di_magic) != XFS_DINODE_MAGIC ||
        (xfs->version == XFS_SB_VERSION_5 ? v != 3 : (v != 1 && v != 2)) ||
        (need_mode && tsk_getu16(TSK_BIG_ENDIAN, core->di_mode) == 0)) {
        return false;
    }
    if (v == 3 && tsk_getu64(TSK_BIG_ENDIAN, core->di_ino) != inum) {
        return false;
    }
    return dinode_core_size(v) + size_t{core->di_forkoff} * 8 < xfs->inodesize;
}

/* Reads the inode at `a` (number `inum`) into `raw`, the inode size of the
 * file system. On a short read: TSK_ERR_FS_READ. */
bool
read_dinode(XFS_INFO *xfs, TSK_INUM_T inum, const InodeAddr &a, uint8_t *raw)
{
    TSK_FS_INFO *fs = &xfs->fs_info;
    const TSK_OFF_T addr =
        (TSK_OFF_T)ag_block(xfs, a.agno, a.agbno) * fs->block_size +
        (TSK_OFF_T)a.index * xfs->inodesize;
    const ssize_t cnt =
        tsk_fs_read(fs, addr, reinterpret_cast<char *>(raw), xfs->inodesize);
    if (cnt != (ssize_t)xfs->inodesize) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_FS_READ);
        }
        tsk_error_set_errstr2("xfs_dinode_load: Inode %" PRIuINUM
                              " from %" PRIu64,
            inum, (uint64_t)addr);
        return false;
    }
    return true;
}

/*
 * Reads allocated inode `inum` into `raw` (the inode size of the file
 * system). Errors: an address outside the file system or not an allocated
 * inode (TSK_ERR_FS_INODE_NUM), an inode that is not a sound one
 * (TSK_ERR_FS_INODE_COR), and the errors of inode_allocation and
 * read_dinode.
 */
uint8_t
xfs_dinode_load(XFS_INFO *xfs, TSK_INUM_T inum, uint8_t *raw)
{
    TSK_FS_INFO *fs = &xfs->fs_info;
    InodeAddr a{};
    // last_inum - 1: last_inum is the virtual $OrphanFiles. A number outside
    // the AGs is no inode, like a free one.
    const bool in_range = inum >= fs->first_inum && inum <= fs->last_inum - 1 &&
        split_inum(xfs, inum, a);
    const Alloc alloc = in_range ? inode_allocation(xfs, a) : Alloc::FREE;
    if (alloc == Alloc::FAILED) {
        return 1;
    }
    if (alloc == Alloc::FREE) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_INODE_NUM);
        tsk_error_set_errstr("xfs_dinode_load: address: %" PRIuINUM, inum);
        return 1;
    }
    if (!read_dinode(xfs, inum, a, raw)) {
        return 1;
    }
    if (!dinode_sound(xfs, inum, raw, true)) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
        tsk_error_set_errstr("xfs_dinode_load: address: %" PRIuINUM, inum);
        return 1;
    }
    return 0;
}

/* A timestamp: classic (signed seconds and nanoseconds) or bigtime
 * (nanoseconds since XFS_BIGTIME_EPOCH_OFFSET seconds before the epoch). */
void
decode_time(const xfs_timestamp &t, bool bigtime, time_t &sec, uint32_t &nsec)
{
    if (bigtime) {
        const uint64_t ns = tsk_getu64(TSK_BIG_ENDIAN, t.t);
        sec = (time_t)((int64_t)(ns / 1000000000) - XFS_BIGTIME_EPOCH_OFFSET);
        nsec = (uint32_t)(ns % 1000000000);
    }
    else {
        sec = (time_t)(int32_t)tsk_getu32(TSK_BIG_ENDIAN, t.t);
        nsec = tsk_getu32(TSK_BIG_ENDIAN, t.t + 4);
    }
}

TSK_FS_META_TYPE_ENUM
meta_type(uint16_t mode)
{
    switch (mode & 0170000) {
    case 0100000:
        return TSK_FS_META_TYPE_REG;
    case 0040000:
        return TSK_FS_META_TYPE_DIR;
    case 0120000:
        return TSK_FS_META_TYPE_LNK;
    case 0020000:
        return TSK_FS_META_TYPE_CHR;
    case 0060000:
        return TSK_FS_META_TYPE_BLK;
    case 0010000:
        return TSK_FS_META_TYPE_FIFO;
    case 0140000:
        return TSK_FS_META_TYPE_SOCK;
    default:
        return TSK_FS_META_TYPE_UNDEF;
    }
}

/* Fills `meta` from the raw inode in meta->content_ptr, with `flags`. */
void
dinode_copy(const XFS_INFO *xfs, TSK_FS_META *meta, TSK_INUM_T inum,
    TSK_FS_META_FLAG_ENUM flags)
{
    const DinodeView d =
        dinode_view(xfs, static_cast<const uint8_t *>(meta->content_ptr));
    const xfs_dinode *core = d.core;
    meta->addr = inum;
    meta->flags = flags;
    meta->type = meta_type(d.mode);
    // TSK's permission bits have the Unix values.
    meta->mode = (TSK_FS_META_MODE_ENUM)(d.mode & 07777);
    meta->nlink = d.version == 1
        ? tsk_getu16(TSK_BIG_ENDIAN, core->di_onlink)
        : (int)tsk_getu32(TSK_BIG_ENDIAN, core->di_nlink);
    meta->uid = tsk_getu32(TSK_BIG_ENDIAN, core->di_uid);
    meta->gid = tsk_getu32(TSK_BIG_ENDIAN, core->di_gid);
    meta->size = (TSK_OFF_T)tsk_getu64(TSK_BIG_ENDIAN, core->di_size);
    const bool bigtime = (d.flags2 & XFS_DIFLAG2_BIGTIME) != 0;
    decode_time(core->di_atime, bigtime, meta->atime, meta->atime_nano);
    decode_time(core->di_mtime, bigtime, meta->mtime, meta->mtime_nano);
    decode_time(core->di_ctime, bigtime, meta->ctime, meta->ctime_nano);
    if (d.version >= 3) {
        decode_time(core->di_crtime, bigtime, meta->crtime, meta->crtime_nano);
    }
    meta->content_type = TSK_FS_META_CONTENT_TYPE_DEFAULT;
}

uint8_t load_link(XFS_INFO *xfs, TSK_FS_META *meta);

} // namespace

/*
 * file_add_meta: the inode `inum`, or the virtual $OrphanFiles directory
 * for last_inum. meta->content_ptr keeps the raw inode, which load_attrs
 * and istat read. A symlink gets its target in meta->link.
 */
uint8_t
xfs_inode_lookup(TSK_FS_INFO *fs, TSK_FS_FILE *a_fs_file, TSK_INUM_T inum)
{
    XFS_INFO *xfs = reinterpret_cast<XFS_INFO *>(fs);
    if (a_fs_file == NULL) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        return 1;
    }
    if (a_fs_file->meta == NULL) {
        a_fs_file->meta = tsk_fs_meta_alloc(xfs->inodesize);
        if (a_fs_file->meta == NULL) {
            return 1;
        }
    }
    else {
        tsk_fs_meta_reset(a_fs_file->meta);
        if (a_fs_file->meta->content_len < xfs->inodesize) {
            TSK_FS_META *meta =
                tsk_fs_meta_realloc(a_fs_file->meta, xfs->inodesize);
            if (meta == NULL) {
                return 1;
            }
            a_fs_file->meta = meta;
        }
    }
    if (inum == TSK_FS_ORPHANDIR_INUM(fs)) {
        return tsk_fs_dir_make_orphan_dir_meta(fs, a_fs_file->meta);
    }
    TSK_FS_META *meta = a_fs_file->meta;
    if (xfs_dinode_load(xfs, inum, static_cast<uint8_t *>(meta->content_ptr))) {
        return 1;
    }
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange): flags
    dinode_copy(xfs, meta, inum,
        (TSK_FS_META_FLAG_ENUM)(TSK_FS_META_FLAG_ALLOC |
            TSK_FS_META_FLAG_USED));
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
    return load_link(xfs, meta);
}

namespace {

/* The state of an inode walk: the request and the file it hands out. */
struct InodeWalk {
    XFS_INFO *xfs;
    TSK_INUM_T start;
    TSK_INUM_T end; /* the last inode number to visit */
    unsigned flags; /* TSK_FS_META_FLAG_ENUM, ALLOC/UNALLOC and USED/UNUSED
                     * both set if neither was */
    TSK_FS_META_WALK_CB action;
    void *ptr;
    TSK_FS_FILE *file;
};

enum class Visit { CONT, STOP, FAILED };

/*
 * Visits inode `inum` if the walk's flags want it: reads it, gives it its
 * flags (allocated inodes ALLOC | USED; free ones UNALLOC, and USED if their
 * ctime is not 0 on disk, as ext decides), fills the walk's file and calls
 * the action. An inode that is not a sound one (a free inode needs no mode)
 * is skipped. Errors: a short read (TSK_ERR_FS_READ), those of load_link,
 * and the action's.
 */
Visit
visit_inode(InodeWalk &walk, TSK_INUM_T inum, bool allocated)
{
    XFS_INFO *xfs = walk.xfs;
    TSK_FS_INFO *fs = &xfs->fs_info;
    unsigned myflags =
        allocated ? TSK_FS_META_FLAG_ALLOC : TSK_FS_META_FLAG_UNALLOC;
    if ((walk.flags & myflags) != myflags) {
        return Visit::CONT;
    }
    InodeAddr a{};
    split_inum(xfs, inum, a);
    TSK_FS_META *meta = walk.file->meta;
    tsk_fs_meta_reset(meta);
    uint8_t *raw = static_cast<uint8_t *>(meta->content_ptr);
    if (!read_dinode(xfs, inum, a, raw)) {
        return Visit::FAILED;
    }
    if (!dinode_sound(xfs, inum, raw, allocated)) {
        return Visit::CONT;
    }
    const xfs_dinode *core = reinterpret_cast<const xfs_dinode *>(raw);
    const bool used =
        allocated || tsk_getu64(TSK_BIG_ENDIAN, core->di_ctime.t) != 0;
    myflags |= used ? TSK_FS_META_FLAG_USED : TSK_FS_META_FLAG_UNUSED;
    if ((walk.flags & myflags) != myflags) {
        return Visit::CONT;
    }
    // Orphans: free inodes that no free name refers to.
    if (!allocated && (walk.flags & TSK_FS_META_FLAG_ORPHAN) &&
        tsk_fs_dir_find_inum_named(fs, inum)) {
        return Visit::CONT;
    }
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): flags
    dinode_copy(xfs, meta, inum, (TSK_FS_META_FLAG_ENUM)myflags);
    if (load_link(xfs, meta)) {
        return Visit::FAILED;
    }
    const TSK_WALK_RET_ENUM ret = walk.action(walk.file, walk.ptr);
    if (ret == TSK_WALK_STOP) {
        return Visit::STOP;
    }
    return ret == TSK_WALK_ERROR ? Visit::FAILED : Visit::CONT;
}

/*
 * Visits the inodes of AG `agno` in the walk's range, in inode order: those
 * of its inode B+tree's chunks, except the holes of sparse chunks, which are
 * not inodes. Errors: those of inobt_records and visit_inode.
 */
Visit
walk_ag(InodeWalk &walk, uint32_t agno)
{
    XFS_INFO *xfs = walk.xfs;
    std::vector<xfs_inobt_rec> recs;
    if (!inobt_records(xfs, agno, recs)) {
        return Visit::FAILED;
    }
    // Sparse chunks: the top 16 bits of ir_counts are the hole mask, one bit
    // for each 4 inodes.
    const bool sparse = xfs->version == XFS_SB_VERSION_5 &&
        (tsk_getu32(TSK_BIG_ENDIAN, xfs->sb.sb_features_incompat) &
            XFS_SB_FEAT_INCOMPAT_SPINODES);
    const unsigned agino_bits = xfs->agblklog + xfs->inopblog;
    const TSK_INUM_T base = TSK_INUM_T{agno} << agino_bits;
    for (const xfs_inobt_rec &rec : recs) {
        const TSK_INUM_T first =
            base + tsk_getu32(TSK_BIG_ENDIAN, rec.ir_startino);
        if (first > walk.end) {
            break;
        }
        if (first + XFS_INODES_PER_CHUNK <= walk.start) {
            continue;
        }
        const uint64_t free_mask = tsk_getu64(TSK_BIG_ENDIAN, rec.ir_free);
        const uint16_t holes =
            sparse ? tsk_getu16(TSK_BIG_ENDIAN, rec.ir_counts) : 0;
        for (unsigned i = 0; i < XFS_INODES_PER_CHUNK; ++i) {
            const TSK_INUM_T inum = first + i;
            if (inum < walk.start || inum > walk.end ||
                ((holes >> (i / XFS_INODES_PER_HOLEMASK_BIT)) & 1)) {
                continue;
            }
            const Visit v =
                visit_inode(walk, inum, ((free_mask >> i) & 1) == 0);
            if (v != Visit::CONT) {
                return v;
            }
        }
    }
    return Visit::CONT;
}

} // namespace

/*
 * inode_walk: the inodes from `start_inum` to `end_inum` in inode order,
 * found through each AG's inode B+tree: the allocated inodes and, as
 * unallocated ones, the free inodes of allocated chunks. Numbers in no chunk
 * are not inodes and are not visited. last_inum is the virtual $OrphanFiles,
 * which XFS doesn't have yet: not visited. Errors: a range outside the
 * file system's (TSK_ERR_FS_WALK_RNG), and those of walk_ag.
 */
uint8_t
xfs_inode_walk(TSK_FS_INFO *fs, TSK_INUM_T start_inum, TSK_INUM_T end_inum,
    TSK_FS_META_FLAG_ENUM flags, TSK_FS_META_WALK_CB a_action, void *a_ptr)
{
    const char *myname = "xfs_inode_walk";
    XFS_INFO *xfs = reinterpret_cast<XFS_INFO *>(fs);
    tsk_error_reset();
    if (start_inum < fs->first_inum || start_inum > fs->last_inum) {
        tsk_error_set_errno(TSK_ERR_FS_WALK_RNG);
        tsk_error_set_errstr(
            "%s: start inode: %" PRIu64 "", myname, (uint64_t)start_inum);
        return 1;
    }
    if (end_inum < fs->first_inum || end_inum > fs->last_inum ||
        end_inum < start_inum) {
        tsk_error_set_errno(TSK_ERR_FS_WALK_RNG);
        tsk_error_set_errstr(
            "%s: end inode: %" PRIu64 "", myname, (uint64_t)end_inum);
        return 1;
    }

    unsigned myflags = flags;
    if (myflags & TSK_FS_META_FLAG_ORPHAN) {
        myflags |= TSK_FS_META_FLAG_UNALLOC | TSK_FS_META_FLAG_USED;
        myflags &= ~(TSK_FS_META_FLAG_ALLOC | TSK_FS_META_FLAG_UNUSED);
    }
    else {
        if ((myflags & (TSK_FS_META_FLAG_ALLOC | TSK_FS_META_FLAG_UNALLOC)) ==
            0) {
            myflags |= TSK_FS_META_FLAG_ALLOC | TSK_FS_META_FLAG_UNALLOC;
        }
        if ((myflags & (TSK_FS_META_FLAG_USED | TSK_FS_META_FLAG_UNUSED)) ==
            0) {
            myflags |= TSK_FS_META_FLAG_USED | TSK_FS_META_FLAG_UNUSED;
        }
    }
    if ((myflags & TSK_FS_META_FLAG_ORPHAN) &&
        tsk_fs_dir_load_inum_named(fs) != TSK_OK) {
        return 1;
    }
    // No $OrphanFiles: the last real inode number is below last_inum.
    if (end_inum == TSK_FS_ORPHANDIR_INUM(fs)) {
        if (start_inum == end_inum) {
            return 0;
        }
        --end_inum;
    }

    TSK_FS_FILE *file = tsk_fs_file_alloc(fs);
    if (file == NULL) {
        return 1;
    }
    file->meta = tsk_fs_meta_alloc(xfs->inodesize);
    if (file->meta == NULL) {
        tsk_fs_file_close(file);
        return 1;
    }
    InodeWalk walk{xfs, start_inum, end_inum, myflags, a_action, a_ptr, file};
    const unsigned agino_bits = xfs->agblklog + xfs->inopblog;
    Visit v = Visit::CONT;
    try {
        for (uint64_t agno = start_inum >> agino_bits;
            agno <= (end_inum >> agino_bits) && agno < xfs->agcount &&
            v == Visit::CONT;
            ++agno) {
            v = walk_ag(walk, (uint32_t)agno);
        }
    }
    catch (const std::bad_alloc &) {
        // The record and block buffers: no exception reaches a C caller.
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_AUX_MALLOC);
        v = Visit::FAILED;
    }
    tsk_fs_file_close(file);
    return v == Visit::FAILED ? 1 : 0;
}

namespace {

/* --- File content: the data fork as the default attribute --- */

/* Appends a run to the list ending at `tail`. Sparse runs have address 0. */
bool
append_run(TSK_FS_ATTR_RUN *&head, TSK_FS_ATTR_RUN *&tail, TSK_DADDR_T offset,
    TSK_DADDR_T addr, TSK_DADDR_T len, bool sparse)
{
    TSK_FS_ATTR_RUN *run = tsk_fs_attr_run_alloc();
    if (run == NULL) {
        return false;
    }
    run->offset = offset;
    run->addr = sparse ? 0 : addr;
    run->len = len;
    run->flags =
        sparse ? TSK_FS_ATTR_RUN_FLAG_SPARSE : TSK_FS_ATTR_RUN_FLAG_NONE;
    run->next = NULL;
    if (tail == NULL) {
        head = run;
    }
    else {
        tail->next = run;
    }
    tail = run;
    return true;
}

/* A data fork that is not what its format says. */
uint8_t
fork_corrupt()
{
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
    return 1;
}

/* An extent record, decoded (see xfs_bmbt_rec). */
struct Extent {
    bool unwritten;
    uint64_t startoff; /* file offset in blocks */
    uint64_t fsblock;
    uint64_t count; /* blocks */
};

Extent
decode_extent(const xfs_bmbt_rec &rec)
{
    const uint64_t l0 = tsk_getu64(TSK_BIG_ENDIAN, rec.l0);
    const uint64_t l1 = tsk_getu64(TSK_BIG_ENDIAN, rec.l1);
    Extent e;
    e.unwritten = (l0 >> 63) != 0;
    e.startoff = (l0 & ((uint64_t{1} << 63) - 1)) >> 9;
    e.fsblock = ((l0 & 0x1ff) << 43) | (l1 >> 21);
    e.count = l1 & ((uint64_t{1} << 21) - 1);
    return e;
}

/* The TSK block address of the first of the `count` blocks at `fsblock`;
 * false if they are not all inside one AG. */
bool
fsblock_addr(
    const XFS_INFO *xfs, uint64_t fsblock, uint64_t count, TSK_DADDR_T &addr)
{
    const uint64_t agno = fsblock >> xfs->agblklog;
    const uint64_t agbno = fsblock & ((uint64_t{1} << xfs->agblklog) - 1);
    if (agno >= xfs->agcount ||
        agbno + count > ag_length(xfs, (uint32_t)agno)) {
        return false;
    }
    addr = ag_block(xfs, (uint32_t)agno, (uint32_t)agbno);
    return true;
}

/* --- Symlink targets --- */

enum class Target { READ, NONE, FAILED };

/*
 * Reads the `size` bytes of a remote symlink's target into `out`, from the
 * extent records in data fork `d`: the extents map the target from file
 * block 0 without holes, and on v5 each starts with a header saying which
 * piece of the target follows it. NONE if the extents or a header are not
 * that, or hold fewer bytes. FAILED on a short read (TSK_ERR_FS_READ).
 */
Target
remote_target(XFS_INFO *xfs, const DinodeView &d, char *out, size_t size)
{
    TSK_FS_INFO *fs = &xfs->fs_info;
    if (d.nextents > d.fork_size / sizeof(xfs_bmbt_rec)) {
        return Target::NONE;
    }
    const xfs_bmbt_rec *recs = reinterpret_cast<const xfs_bmbt_rec *>(d.fork);
    const size_t hdr =
        xfs->version == XFS_SB_VERSION_5 ? sizeof(xfs_dsymlink_hdr) : 0;
    char buf[sizeof(xfs_dsymlink_hdr) + XFS_SYMLINK_MAXLEN];
    size_t done = 0;     // bytes of the target read so far
    uint64_t cursor = 0; // file block where the extents end so far
    for (uint64_t i = 0; i < d.nextents && done < size; ++i) {
        const Extent e = decode_extent(recs[i]);
        TSK_DADDR_T addr = 0;
        if (e.count == 0 || e.startoff != cursor ||
            !fsblock_addr(xfs, e.fsblock, e.count, addr)) {
            return Target::NONE;
        }
        cursor += e.count;
        // At most 2^21 blocks of 64 KiB: no overflow.
        const uint64_t space = e.count * fs->block_size - hdr;
        const size_t take = (size_t)std::min<uint64_t>(size - done, space);
        const ssize_t cnt =
            tsk_fs_read(fs, (TSK_OFF_T)addr * fs->block_size, buf, hdr + take);
        if (cnt != (ssize_t)(hdr + take)) {
            if (cnt >= 0) {
                tsk_error_reset();
                tsk_error_set_errno(TSK_ERR_FS_READ);
            }
            return Target::FAILED;
        }
        if (hdr != 0) {
            const xfs_dsymlink_hdr *h =
                reinterpret_cast<const xfs_dsymlink_hdr *>(buf);
            if (tsk_getu32(TSK_BIG_ENDIAN, h->sl_magic) != XFS_SYMLINK_MAGIC ||
                tsk_getu32(TSK_BIG_ENDIAN, h->sl_offset) != done ||
                tsk_getu32(TSK_BIG_ENDIAN, h->sl_bytes) != take) {
                return Target::NONE;
            }
        }
        memcpy(out + done, buf + hdr, take);
        done += take;
    }
    return done == size ? Target::READ : Target::NONE;
}

/*
 * Sets meta->link of a symlink to its target, from the raw inode in
 * meta->content_ptr: the data fork's bytes (format local) or the blocks of
 * a remote symlink (format extents). Control characters become '^', as in
 * ext's targets. Any other file, and a symlink whose target can't be put
 * together (a size no target has, or see remote_target), has none; only a
 * read error fails, as it does in ext's inode lookup.
 */
uint8_t
load_link(XFS_INFO *xfs, TSK_FS_META *meta)
{
    // tsk_fs_meta_reset keeps the target of the inode before.
    free(meta->link);
    meta->link = NULL;
    if (meta->type != TSK_FS_META_TYPE_LNK || meta->size <= 0 ||
        meta->size > XFS_SYMLINK_MAXLEN) {
        return 0;
    }
    const DinodeView d =
        dinode_view(xfs, static_cast<const uint8_t *>(meta->content_ptr));
    const size_t size = (size_t)meta->size;
    char *link = static_cast<char *>(tsk_malloc(size + 1));
    if (link == NULL) {
        return 1;
    }
    Target target = Target::NONE;
    if (d.format == XFS_DINODE_FMT_LOCAL) {
        if (size <= d.fork_size) {
            memcpy(link, d.fork, size);
            target = Target::READ;
        }
    }
    else if (d.format == XFS_DINODE_FMT_EXTENTS) {
        target = remote_target(xfs, d, link, size);
    }
    if (target != Target::READ) {
        free(link);
        return target == Target::FAILED ? 1 : 0;
    }
    link[size] = '\0';
    for (size_t i = 0; link[i] != '\0'; ++i) {
        if (TSK_IS_CNTRL(link[i])) {
            link[i] = '^';
        }
    }
    meta->link = link;
    return 0;
}

/* --- File content: the runs of the extent records --- */

/*
 * The run list of the `count` extent records at `recs`, covering the file's
 * blocks from 0: a sparse run for each gap between extents (holes) and for
 * each unwritten extent (preallocated, reads as zeros), and one for a hole
 * at the end up to `size`. `end_blocks` is where the runs end, which can be
 * past the end of the file (preallocated blocks, a directory's blocks above
 * its size). Errors (TSK_ERR_FS_INODE_COR): an empty, overlapping or
 * out-of-order extent, an extent outside its AG.
 */
uint8_t
extent_runs(const XFS_INFO *xfs, const xfs_bmbt_rec *recs, uint64_t count,
    TSK_OFF_T size, TSK_FS_ATTR_RUN *&head, TSK_DADDR_T &end_blocks)
{
    const TSK_FS_INFO *fs = &xfs->fs_info;
    head = NULL;
    TSK_FS_ATTR_RUN *tail = NULL;
    const auto corrupt = [&head]() {
        tsk_fs_attr_run_free(head);
        head = NULL;
        return fork_corrupt();
    };
    const auto no_memory = [&head]() {
        tsk_fs_attr_run_free(head);
        head = NULL;
        return 1;
    };
    // Blocks beyond this would overflow a byte offset.
    const uint64_t max_blocks = INT64_MAX / fs->block_size;
    uint64_t cursor = 0; // file block where the runs end so far
    for (uint64_t i = 0; i < count; ++i) {
        const Extent e = decode_extent(recs[i]);
        TSK_DADDR_T addr = 0;
        if (e.count == 0 || e.startoff < cursor ||
            e.startoff + e.count > max_blocks ||
            !fsblock_addr(xfs, e.fsblock, e.count, addr)) {
            return corrupt();
        }
        if (e.startoff > cursor &&
            !append_run(head, tail, cursor, 0, e.startoff - cursor, true)) {
            return no_memory();
        }
        if (!append_run(head, tail, e.startoff, addr, e.count, e.unwritten)) {
            return no_memory();
        }
        cursor = e.startoff + e.count;
    }
    const uint64_t file_blocks =
        ((uint64_t)size + fs->block_size - 1) / fs->block_size;
    if (cursor < file_blocks) {
        if (!append_run(head, tail, cursor, 0, file_blocks - cursor, true)) {
            return no_memory();
        }
        cursor = file_blocks;
    }
    end_blocks = cursor;
    return 0;
}

/* The extent B+tree of a data fork being read into `recs`. */
struct BmbtWalk {
    XFS_INFO *xfs;
    uint64_t nextents;              /* the records the inode says it has */
    uint64_t cursor;                /* file block where `recs` end so far */
    std::vector<xfs_bmbt_rec> recs; /* in file order */
};

/*
 * Appends the extent records of the subtree at B+tree block `fsblock`, which
 * must be at `level`, in file order. Each step down goes down one level,
 * every block has records and every record starts at or after the end of
 * the one before, so a tree that loops back on itself is an error, not an
 * endless walk. Errors: a short read (TSK_ERR_FS_READ); a block outside the
 * AGs, of another magic or level, with no records or too many, or records
 * out of order or more than the inode has (TSK_ERR_FS_INODE_COR).
 */
uint8_t
bmbt_subtree(BmbtWalk &walk, uint64_t fsblock, unsigned level)
{
    XFS_INFO *xfs = walk.xfs;
    TSK_FS_INFO *fs = &xfs->fs_info;
    TSK_DADDR_T addr = 0;
    if (!fsblock_addr(xfs, fsblock, 1, addr)) {
        return fork_corrupt();
    }
    std::vector<char> buf(fs->block_size);
    const ssize_t cnt = tsk_fs_read_block(fs, addr, buf.data(), buf.size());
    if (cnt != (ssize_t)buf.size()) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_FS_READ);
        }
        return 1;
    }
    const bool crc = xfs->version == XFS_SB_VERSION_5;
    const size_t hdr = crc ? XFS_BTREE_LBLOCK_CRC_LEN : XFS_BTREE_LBLOCK_LEN;
    const uint8_t *b = reinterpret_cast<const uint8_t *>(buf.data());
    // The long-form header starts like the short-form one.
    const xfs_btree_sblock *h = reinterpret_cast<const xfs_btree_sblock *>(b);
    const size_t numrecs = tsk_getu16(TSK_BIG_ENDIAN, h->bb_numrecs);
    // A node's key and pointer take the room of a leaf's record, so both
    // kinds of block hold the same number.
    static_assert(
        XFS_BMBT_KEY_SIZE + XFS_BMBT_PTR_SIZE == sizeof(xfs_bmbt_rec), "");
    const size_t maxrecs = (buf.size() - hdr) / sizeof(xfs_bmbt_rec);
    if (tsk_getu32(TSK_BIG_ENDIAN, h->bb_magic) !=
            (crc ? XFS_BMAP_CRC_MAGIC : XFS_BMAP_MAGIC) ||
        tsk_getu16(TSK_BIG_ENDIAN, h->bb_level) != level || numrecs == 0 ||
        numrecs > maxrecs) {
        return fork_corrupt();
    }
    if (level > 0) {
        const uint8_t *ptrs = b + hdr + maxrecs * XFS_BMBT_KEY_SIZE;
        for (size_t i = 0; i < numrecs; ++i) {
            if (bmbt_subtree(walk,
                    tsk_getu64(TSK_BIG_ENDIAN, ptrs + i * XFS_BMBT_PTR_SIZE),
                    level - 1)) {
                return 1;
            }
        }
        return 0;
    }
    if (numrecs > walk.nextents - walk.recs.size()) {
        return fork_corrupt();
    }
    const xfs_bmbt_rec *recs = reinterpret_cast<const xfs_bmbt_rec *>(b + hdr);
    for (size_t i = 0; i < numrecs; ++i) {
        const Extent e = decode_extent(recs[i]);
        if (e.count == 0 || e.startoff < walk.cursor) {
            return fork_corrupt();
        }
        walk.cursor = e.startoff + e.count;
        walk.recs.push_back(recs[i]);
    }
    return 0;
}

/*
 * Reads the extent records of the B+tree rooted in data fork `d` into
 * `recs`, in file order. Errors: those of bmbt_subtree; a root that doesn't
 * fit the fork, with no records or of an impossible level, or a tree with
 * fewer records than the inode says (TSK_ERR_FS_INODE_COR); no memory
 * (TSK_ERR_AUX_MALLOC).
 */
uint8_t
bmbt_records(
    XFS_INFO *xfs, const DinodeView &d, std::vector<xfs_bmbt_rec> &recs)
{
    if (d.fork_size < XFS_BMDR_HDR_LEN) {
        return fork_corrupt();
    }
    const unsigned level = tsk_getu16(TSK_BIG_ENDIAN, d.fork);
    const size_t numrecs = tsk_getu16(TSK_BIG_ENDIAN, d.fork + 2);
    const size_t maxrecs = (d.fork_size - XFS_BMDR_HDR_LEN) /
        (XFS_BMBT_KEY_SIZE + XFS_BMBT_PTR_SIZE);
    if (level == 0 || level > XFS_BTREE_MAXLEVELS || numrecs == 0 ||
        numrecs > maxrecs) {
        return fork_corrupt();
    }
    try {
        BmbtWalk walk{xfs, d.nextents, 0, {}};
        const uint8_t *ptrs =
            d.fork + XFS_BMDR_HDR_LEN + maxrecs * XFS_BMBT_KEY_SIZE;
        for (size_t i = 0; i < numrecs; ++i) {
            if (bmbt_subtree(walk,
                    tsk_getu64(TSK_BIG_ENDIAN, ptrs + i * XFS_BMBT_PTR_SIZE),
                    level - 1)) {
                return 1;
            }
        }
        if (walk.recs.size() != d.nextents) {
            return fork_corrupt();
        }
        recs.swap(walk.recs);
        return 0;
    }
    catch (const std::bad_alloc &) {
        // The walk's buffers and records: no exception reaches a C caller.
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_AUX_MALLOC);
        return 1;
    }
}

} // namespace

/*
 * load_attrs: the default (data) attribute from the data fork of the raw
 * inode in meta->content_ptr. Extent lists and extent B+trees are
 * non-resident runs; local data (short-form directories, inline symlinks) is
 * resident; device files have no data. The extents of a file on the realtime
 * device are not in the image: TSK_ERR_FS_UNSUPFUNC. Extended attributes
 * (the attr fork) are not read.
 */
uint8_t
xfs_load_attrs(TSK_FS_FILE *fs_file)
{
    if (fs_file == NULL || fs_file->meta == NULL || fs_file->fs_info == NULL) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        return 1;
    }
    TSK_FS_META *meta = fs_file->meta;
    XFS_INFO *xfs = reinterpret_cast<XFS_INFO *>(fs_file->fs_info);
    if (meta->attr != NULL && meta->attr_state == TSK_FS_META_ATTR_STUDIED) {
        return 0;
    }
    if (meta->content_ptr == NULL || meta->content_len < xfs->inodesize) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        return 1;
    }
    const DinodeView d =
        dinode_view(xfs, static_cast<const uint8_t *>(meta->content_ptr));
    if (meta->size < 0) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
        return 1;
    }

    if ((d.format == XFS_DINODE_FMT_EXTENTS ||
            d.format == XFS_DINODE_FMT_BTREE) &&
        (d.flags & XFS_DIFLAG_REALTIME)) {
        // Its extents are on the realtime device, not in the image.
        set_unsupported();
        return 1;
    }

    TSK_FS_ATTR_RUN *runs = NULL;
    TSK_DADDR_T end_blocks = 0;
    std::vector<xfs_bmbt_rec> tree_recs;
    switch (d.format) {
    case XFS_DINODE_FMT_DEV:
        break;
    case XFS_DINODE_FMT_LOCAL:
        if ((uint64_t)meta->size > d.fork_size) {
            return fork_corrupt();
        }
        break;
    case XFS_DINODE_FMT_EXTENTS:
        if (d.nextents > d.fork_size / sizeof(xfs_bmbt_rec)) {
            return fork_corrupt();
        }
        if (extent_runs(xfs, reinterpret_cast<const xfs_bmbt_rec *>(d.fork),
                d.nextents, meta->size, runs, end_blocks)) {
            return 1;
        }
        break;
    case XFS_DINODE_FMT_BTREE:
        if (bmbt_records(xfs, d, tree_recs) ||
            extent_runs(xfs, tree_recs.data(), tree_recs.size(), meta->size,
                runs, end_blocks)) {
            return 1;
        }
        break;
    default:
        // Formats data forks don't have.
        set_unsupported();
        return 1;
    }

    if (meta->attr != NULL) {
        tsk_fs_attrlist_markunused(meta->attr);
    }
    else if ((meta->attr = tsk_fs_attrlist_alloc()) == NULL) {
        tsk_fs_attr_run_free(runs);
        return 1;
    }
    TSK_FS_ATTR *attr = NULL;
    if (d.format == XFS_DINODE_FMT_LOCAL) {
        // tsk_fs_attr_set_str copies the data; its pointer isn't const.
        attr = tsk_fs_attrlist_getnew(meta->attr, TSK_FS_ATTR_RES);
        if (attr == NULL ||
            tsk_fs_attr_set_str(fs_file, attr, NULL, TSK_FS_ATTR_TYPE_DEFAULT,
                TSK_FS_ATTR_ID_DEFAULT, const_cast<uint8_t *>(d.fork),
                (size_t)meta->size)) {
            return 1;
        }
    }
    else {
        // The runs cover at least the size rounded up to blocks (device
        // files have none); near the largest file size that is more than
        // TSK_OFF_T holds.
        const uint64_t runs_bytes =
            (uint64_t)end_blocks * xfs->fs_info.block_size;
        TSK_OFF_T alloc_size = runs_bytes > (uint64_t)INT64_MAX
            ? INT64_MAX
            : (TSK_OFF_T)runs_bytes;
        if (alloc_size < meta->size) {
            alloc_size = meta->size;
        }
        attr = tsk_fs_attrlist_getnew(meta->attr, TSK_FS_ATTR_NONRES);
        if (attr == NULL) {
            tsk_fs_attr_run_free(runs);
            return 1;
        }
        if (tsk_fs_attr_set_run(fs_file, attr, runs, NULL,
                TSK_FS_ATTR_TYPE_DEFAULT, TSK_FS_ATTR_ID_DEFAULT, meta->size,
                meta->size, alloc_size, TSK_FS_ATTR_FLAG_NONE, 0)) {
            tsk_fs_attr_run_free(runs);
            return 1;
        }
    }
    meta->attr_state = TSK_FS_META_ATTR_STUDIED;
    return 0;
}

namespace {

/* --- istat --- */

struct PrintAddr {
    FILE *h;
    int idx;
};

/* File walk callback of istat: each block's address, 0 for a hole, eight
 * per line (as ext's istat prints them). Unlike ext's, it doesn't keep to
 * TSK_FS_BLOCK_FLAG_CONT blocks: XFS blocks are only ALLOC (see
 * xfs_block_getflags), and its file walk reports content blocks only. */
TSK_WALK_RET_ENUM
print_addr_act(TSK_FS_FILE *fs_file, TSK_OFF_T, TSK_DADDR_T addr, char *,
    size_t size, TSK_FS_BLOCK_FLAG_ENUM, void *a_ptr)
{
    PrintAddr *print = static_cast<PrintAddr *>(a_ptr);
    const TSK_FS_INFO *fs = fs_file->fs_info;
    for (size_t done = 0; done < size; done += fs->block_size) {
        tsk_fprintf(print->h, "%" PRIuDADDR " ",
            addr != 0 ? addr + done / fs->block_size : 0);
        if (++print->idx == 8) {
            tsk_fprintf(print->h, "\n");
            print->idx = 0;
        }
    }
    return TSK_WALK_CONT;
}

/* The inode times, with nanoseconds; crtime only on v3 inodes. */
void
print_times(FILE *h, const TSK_FS_META *meta, bool crtime)
{
    char buf[128];
    tsk_fprintf(h, "Accessed:\t%s\n",
        tsk_fs_time_to_str_subsecs(meta->atime, meta->atime_nano, buf));
    tsk_fprintf(h, "File Modified:\t%s\n",
        tsk_fs_time_to_str_subsecs(meta->mtime, meta->mtime_nano, buf));
    tsk_fprintf(h, "Inode Modified:\t%s\n",
        tsk_fs_time_to_str_subsecs(meta->ctime, meta->ctime_nano, buf));
    if (crtime) {
        tsk_fprintf(h, "File Created:\t%s\n",
            tsk_fs_time_to_str_subsecs(meta->crtime, meta->crtime_nano, buf));
    }
}

} // namespace

/*
 * istat: the inode's metadata in the lines ext's istat prints, then the
 * blocks of an extent-mapped data fork ("Direct Blocks:", or its runs with
 * TSK_FS_ISTAT_RUNLIST). `numblock`, if not 0, is the number of blocks to
 * list instead of the size's. A content error is printed, not returned.
 */
uint8_t
xfs_istat(TSK_FS_INFO *fs, TSK_FS_ISTAT_FLAG_ENUM istat_flags, FILE *h,
    TSK_INUM_T inum, TSK_DADDR_T numblock, int32_t sec_skew)
{
    const XFS_INFO *xfs = reinterpret_cast<const XFS_INFO *>(fs);
    tsk_error_reset();
    TSK_FS_FILE *fs_file = tsk_fs_file_open_meta(fs, NULL, inum);
    if (fs_file == NULL) {
        return 1;
    }
    TSK_FS_META *meta = fs_file->meta;
    // $OrphanFiles is virtual: no inode behind it.
    const bool real = inum != TSK_FS_ORPHANDIR_INUM(fs);
    DinodeView d{};
    if (real) {
        d = dinode_view(xfs, static_cast<const uint8_t *>(meta->content_ptr));
    }

    tsk_fprintf(h, "inode: %" PRIuINUM "\n", inum);
    tsk_fprintf(h, "%sAllocated\n",
        (meta->flags & TSK_FS_META_FLAG_ALLOC) ? "" : "Not ");
    if (real) {
        InodeAddr a{};
        split_inum(xfs, inum, a);
        tsk_fprintf(h, "Group: %" PRIu32 "\n", a.agno);
        tsk_fprintf(h, "Generation Id: %" PRIu32 "\n",
            tsk_getu32(TSK_BIG_ENDIAN, d.core->di_gen));
    }
    if (meta->link != NULL && meta->link[0] != '\0') {
        tsk_fprintf(h, "symbolic link to: %s\n", meta->link);
    }
    tsk_fprintf(
        h, "uid / gid: %" PRIuUID " / %" PRIuGID "\n", meta->uid, meta->gid);
    char ls[12];
    tsk_fs_meta_make_ls(meta, ls, sizeof(ls));
    tsk_fprintf(h, "mode: %s\n", ls);
    tsk_fprintf(h, "size: %" PRIdOFF "\n", meta->size);
    tsk_fprintf(h, "num of links: %d\n", meta->nlink);

    const bool crtime = real && d.version >= 3;
    if (sec_skew != 0) {
        tsk_fprintf(h, "\nAdjusted Inode Times:\n");
        TSK_FS_META skewed = *meta;
        skewed.atime -= sec_skew;
        skewed.mtime -= sec_skew;
        skewed.ctime -= sec_skew;
        skewed.crtime -= sec_skew;
        print_times(h, &skewed, crtime);
        tsk_fprintf(h, "\nOriginal Inode Times:\n");
    }
    else {
        tsk_fprintf(h, "\nInode Times:\n");
    }
    print_times(h, meta, crtime);

    if (real &&
        (d.format == XFS_DINODE_FMT_EXTENTS ||
            d.format == XFS_DINODE_FMT_BTREE)) {
        if (numblock > 0) {
            meta->size = (TSK_OFF_T)(numblock * fs->block_size);
        }
        tsk_fprintf(h, "\nDirect Blocks:\n");
        const TSK_FS_ATTR *attr =
            tsk_fs_file_attr_get_type(fs_file, TSK_FS_ATTR_TYPE_DEFAULT, 0, 0);
        if (attr == NULL) {
            tsk_fprintf(h, "\nError reading file:  ");
            tsk_error_print(h);
            tsk_error_reset();
        }
        else if (istat_flags & TSK_FS_ISTAT_RUNLIST) {
            if (tsk_fs_attr_print(attr, h)) {
                tsk_fprintf(h, "\nError creating run lists\n");
                tsk_error_print(h);
                tsk_error_reset();
            }
        }
        else {
            PrintAddr print = {h, 0};
            if (tsk_fs_file_walk(fs_file, TSK_FS_FILE_WALK_FLAG_AONLY,
                    print_addr_act, &print)) {
                tsk_fprintf(h, "\nError reading file:  ");
                tsk_error_print(h);
                tsk_error_reset();
            }
            else if (print.idx != 0) {
                tsk_fprintf(h, "\n");
            }
        }
    }
    tsk_fs_file_close(fs_file);
    return 0;
}

} // namespace tsk_xfs
