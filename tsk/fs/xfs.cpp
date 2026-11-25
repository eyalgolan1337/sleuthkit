/*
 * The Sleuth Kit
 *
 * This software is distributed under the Common Public License 1.0
 *
 * Read-only XFS support (the fork's, #11).
 *
 * The opener (superblock checks and the TSK_FS_INFO geometry), fsstat, and
 * the callbacks that later steps of #11 fill in. See docs/llm/fs.md ("XFS").
 *
 * Addresses:
 * - A TSK block address is linear: AG number * sb_agblocks + AG block
 *   number, so block N starts at byte N * block_size of the file system.
 *   XFS's own block numbers (fsblocks) put the AG number above sb_agblklog
 *   bits instead, so they must be converted before use as TSK addresses.
 * - A TSK metadata address is the XFS inode number, unchanged: AG number,
 *   AG block number and inode-in-block number, in sb_agblklog + sb_inopblog
 *   low bits. Most numbers in the range are not inodes. last_inum is one past
 *   the largest inode number the geometry allows, for $OrphanFiles.
 *
 * C++14. Nothing here throws: no C++ exception can reach the C callers.
 */

#include "tsk_xfs.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// The on-disk structures must match the disk layout.
static_assert(sizeof(xfs_dsb) == 264, "");
static_assert(sizeof(xfs_dinode) == XFS_DINODE_CORE_SIZE_V3, "");
static_assert(offsetof(xfs_dinode, di_crc) == XFS_DINODE_CORE_SIZE_V2, "");
static_assert(offsetof(xfs_dinode, di_size) == 56, "");
static_assert(offsetof(xfs_dinode, di_crtime) == 144, "");
static_assert(sizeof(xfs_bmbt_rec) == 16, "");
static_assert(sizeof(xfs_inobt_rec) == 16, "");

namespace {

/* The superblock fields the opener reads, in host order. */
struct SbValues {
    uint32_t magic;
    uint32_t blocksize;
    uint64_t dblocks;
    uint64_t rootino;
    uint32_t agblocks;
    uint32_t agcount;
    uint16_t versionnum;
    uint16_t sectsize;
    uint16_t inodesize;
    uint16_t inopblock;
};

SbValues
sb_values(const xfs_dsb &sb)
{
    SbValues v;
    v.magic = tsk_getu32(TSK_BIG_ENDIAN, sb.sb_magicnum);
    v.blocksize = tsk_getu32(TSK_BIG_ENDIAN, sb.sb_blocksize);
    v.dblocks = tsk_getu64(TSK_BIG_ENDIAN, sb.sb_dblocks);
    v.rootino = tsk_getu64(TSK_BIG_ENDIAN, sb.sb_rootino);
    v.agblocks = tsk_getu32(TSK_BIG_ENDIAN, sb.sb_agblocks);
    v.agcount = tsk_getu32(TSK_BIG_ENDIAN, sb.sb_agcount);
    v.versionnum = tsk_getu16(TSK_BIG_ENDIAN, sb.sb_versionnum);
    v.sectsize = tsk_getu16(TSK_BIG_ENDIAN, sb.sb_sectsize);
    v.inodesize = tsk_getu16(TSK_BIG_ENDIAN, sb.sb_inodesize);
    v.inopblock = tsk_getu16(TSK_BIG_ENDIAN, sb.sb_inopblock);
    return v;
}

/* ceil(log2(n)) for n >= 1, as the kernel computes sb_agblklog. */
unsigned
ceil_log2(uint32_t n)
{
    unsigned log = 0;
    while (log < 32 && (uint64_t{1} << log) < n) {
        ++log;
    }
    return log;
}

/*
 * Checks that the superblock describes a geometry XFS can have: the geometry
 * checks of the kernel's xfs_validate_sb_common, plus the consistency this
 * module relies on to decode inode numbers (sb_inopblog, inodes no larger
 * than a block, at most 32 AG-relative inode bits) and a root inode inside
 * the file system. Mutable counters (icount, ifree, fdblocks) and feature
 * bits are not checked.
 */
enum class SbCheck { GOOD, BAD_BLOCK_SIZE, BAD };

SbCheck
check_superblock(const xfs_dsb &sb)
{
    const SbValues v = sb_values(sb);
    const unsigned version = v.versionnum & XFS_SB_VERSION_NUMBITS;
    if (version != XFS_SB_VERSION_4 && version != XFS_SB_VERSION_5) {
        return SbCheck::BAD;
    }
    if (sb.sb_sectlog < XFS_MIN_SECTORSIZE_LOG ||
        sb.sb_sectlog > XFS_MAX_SECTORSIZE_LOG ||
        v.sectsize != (1u << sb.sb_sectlog)) {
        return SbCheck::BAD;
    }
    if (sb.sb_blocklog < XFS_MIN_BLOCKSIZE_LOG ||
        sb.sb_blocklog > XFS_MAX_BLOCKSIZE_LOG ||
        v.blocksize != (1u << sb.sb_blocklog) ||
        (version == XFS_SB_VERSION_5 && v.blocksize < XFS_MIN_CRC_BLOCKSIZE)) {
        return SbCheck::BAD_BLOCK_SIZE;
    }
    if (sb.sb_inodelog < XFS_DINODE_MIN_LOG ||
        sb.sb_inodelog > XFS_DINODE_MAX_LOG ||
        v.inodesize != (1u << sb.sb_inodelog) ||
        sb.sb_inodelog > sb.sb_blocklog) {
        return SbCheck::BAD;
    }
    if (sb.sb_inopblog != sb.sb_blocklog - sb.sb_inodelog ||
        v.inopblock != (1u << sb.sb_inopblog)) {
        return SbCheck::BAD;
    }
    if (v.agcount == 0) {
        return SbCheck::BAD;
    }
    if (v.agblocks < XFS_MIN_AG_BLOCKS ||
        sb.sb_agblklog != ceil_log2(v.agblocks) ||
        sb.sb_agblklog + sb.sb_inopblog > XFS_MAX_AGINO_BITS) {
        return SbCheck::BAD;
    }
    // The last AG may be shorter, but not shorter than the minimum.
    const uint64_t full = uint64_t{v.agcount} * v.agblocks;
    if (v.dblocks > full || v.dblocks < full - v.agblocks + XFS_MIN_AG_BLOCKS) {
        return SbCheck::BAD;
    }
    if (sb.sb_dirblklog > XFS_MAX_BLOCKSIZE_LOG - sb.sb_blocklog) {
        return SbCheck::BAD;
    }
    if (sb.sb_imax_pct > 100) {
        return SbCheck::BAD;
    }
    // The root inode's AG, and its block in that AG, must exist.
    const unsigned agino_bits = sb.sb_agblklog + sb.sb_inopblog;
    const uint64_t root_ag = v.rootino >> agino_bits;
    const uint64_t root_agbno =
        (v.rootino & ((uint64_t{1} << agino_bits) - 1)) >> sb.sb_inopblog;
    if (v.rootino == 0 || root_ag >= v.agcount || root_agbno >= v.agblocks ||
        root_ag * v.agblocks + root_agbno >= v.dblocks) {
        return SbCheck::BAD;
    }
    return SbCheck::GOOD;
}

/* Not implemented yet: the TSK error number alone. */
void
set_unsupported()
{
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_UNSUPFUNC);
}

/* --- Callbacks of later steps: each fails with a TSK error. --- */

uint8_t
xfs_inode_walk(TSK_FS_INFO *, TSK_INUM_T, TSK_INUM_T, TSK_FS_META_FLAG_ENUM,
    TSK_FS_META_WALK_CB, void *)
{
    set_unsupported();
    return 1;
}

uint8_t
xfs_block_walk(TSK_FS_INFO *, TSK_DADDR_T, TSK_DADDR_T,
    TSK_FS_BLOCK_WALK_FLAG_ENUM, TSK_FS_BLOCK_WALK_CB, void *)
{
    set_unsupported();
    return 1;
}

TSK_FS_BLOCK_FLAG_ENUM
xfs_block_getflags(TSK_FS_INFO *, TSK_DADDR_T)
{
    // Allocation state is not known yet: no flags.
    return TSK_FS_BLOCK_FLAG_UNUSED;
}

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

enum class Alloc { ALLOCATED, FREE, ERROR };

void
set_corrupt()
{
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_CORRUPT);
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

    xfs_agi agi;
    const TSK_OFF_T agi_off =
        (TSK_OFF_T)ag_block(xfs, a.agno, 0) * fs->block_size +
        XFS_AGI_SECTOR * xfs->sectsize;
    const ssize_t cnt =
        tsk_fs_read(fs, agi_off, reinterpret_cast<char *>(&agi), sizeof(agi));
    if (cnt != (ssize_t)sizeof(agi)) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_FS_READ);
        }
        return Alloc::ERROR;
    }
    const uint32_t level = tsk_getu32(TSK_BIG_ENDIAN, agi.agi_level);
    uint32_t block = tsk_getu32(TSK_BIG_ENDIAN, agi.agi_root);
    if (tsk_getu32(TSK_BIG_ENDIAN, agi.agi_magicnum) != XFS_AGI_MAGIC ||
        tsk_getu32(TSK_BIG_ENDIAN, agi.agi_versionnum) != XFS_AGI_VERSION ||
        tsk_getu32(TSK_BIG_ENDIAN, agi.agi_seqno) != a.agno || level == 0 ||
        level > XFS_BTREE_MAXLEVELS || block >= ag_blocks) {
        set_corrupt();
        return Alloc::ERROR;
    }

    const bool crc = xfs->version == XFS_SB_VERSION_5;
    const uint32_t magic = crc ? XFS_IBT_CRC_MAGIC : XFS_IBT_MAGIC;
    const size_t hdr = crc ? XFS_BTREE_SBLOCK_CRC_LEN : XFS_BTREE_SBLOCK_LEN;
    const size_t space = fs->block_size - hdr;
    std::vector<char> buf(fs->block_size);
    const uint8_t *b = reinterpret_cast<const uint8_t *>(buf.data());
    for (uint32_t want = level - 1;; --want) {
        if (!read_ag_block(xfs, a.agno, block, buf.data())) {
            return Alloc::ERROR;
        }
        const xfs_btree_sblock *h =
            reinterpret_cast<const xfs_btree_sblock *>(b);
        const uint16_t numrecs = tsk_getu16(TSK_BIG_ENDIAN, h->bb_numrecs);
        if (tsk_getu32(TSK_BIG_ENDIAN, h->bb_magic) != magic ||
            tsk_getu16(TSK_BIG_ENDIAN, h->bb_level) != want) {
            set_corrupt();
            return Alloc::ERROR;
        }
        if (want == 0) {
            if (numrecs > space / sizeof(xfs_inobt_rec)) {
                set_corrupt();
                return Alloc::ERROR;
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
            return Alloc::ERROR;
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
            return Alloc::ERROR;
        }
    }
}

/* --- Inodes --- */

/* The fields of a raw inode (the inode size of the file system, as on disk)
 * that the module uses, in host order. */
struct DinodeView {
    const xfs_dinode *core;
    uint16_t mode;
    uint8_t version;
    uint8_t format;
    uint64_t flags2;     /* 0 before v3 */
    uint64_t nextents;   /* data fork extent records */
    const uint8_t *fork; /* the data fork */
    size_t fork_size;    /* its size in bytes */
};

DinodeView
dinode_view(const XFS_INFO *xfs, const uint8_t *raw)
{
    DinodeView d;
    d.core = reinterpret_cast<const xfs_dinode *>(raw);
    d.mode = tsk_getu16(TSK_BIG_ENDIAN, d.core->di_mode);
    d.version = d.core->di_version;
    d.format = d.core->di_format;
    d.flags2 =
        d.version >= 3 ? tsk_getu64(TSK_BIG_ENDIAN, d.core->di_flags2) : 0;
    d.nextents = (d.flags2 & XFS_DIFLAG2_NREXT64)
        ? tsk_getu64(TSK_BIG_ENDIAN, d.core->di_big_nextents)
        : tsk_getu32(TSK_BIG_ENDIAN, d.core->di_nextents);
    const size_t core =
        d.version >= 3 ? XFS_DINODE_CORE_SIZE_V3 : XFS_DINODE_CORE_SIZE_V2;
    d.fork = raw + core;
    d.fork_size = d.core->di_forkoff != 0 ? size_t{d.core->di_forkoff} * 8
                                          : xfs->inodesize - core;
    return d;
}

/* Whether the raw inode is a sound one for `inum`: the magic, an inode
 * version the file system's version allows (v5: 3 only, v4: 1 or 2), on v3
 * its own inode number, a mode, and a data fork inside the inode. */
bool
dinode_sound(const XFS_INFO *xfs, TSK_INUM_T inum, const uint8_t *raw)
{
    const xfs_dinode *core = reinterpret_cast<const xfs_dinode *>(raw);
    const uint8_t v = core->di_version;
    if (tsk_getu16(TSK_BIG_ENDIAN, core->di_magic) != XFS_DINODE_MAGIC ||
        (xfs->version == XFS_SB_VERSION_5 ? v != 3 : (v != 1 && v != 2)) ||
        tsk_getu16(TSK_BIG_ENDIAN, core->di_mode) == 0) {
        return false;
    }
    if (v == 3 && tsk_getu64(TSK_BIG_ENDIAN, core->di_ino) != inum) {
        return false;
    }
    const size_t core_size =
        v == 3 ? XFS_DINODE_CORE_SIZE_V3 : XFS_DINODE_CORE_SIZE_V2;
    return core_size + size_t{core->di_forkoff} * 8 < xfs->inodesize;
}

/*
 * Reads allocated inode `inum` into `raw` (the inode size of the file
 * system). Errors: an address outside the file system or not an allocated
 * inode (TSK_ERR_FS_INODE_NUM), an inode that is not a sound one
 * (TSK_ERR_FS_INODE_COR), and the errors of inode_allocation.
 */
uint8_t
xfs_dinode_load(XFS_INFO *xfs, TSK_INUM_T inum, uint8_t *raw)
{
    TSK_FS_INFO *fs = &xfs->fs_info;
    InodeAddr a;
    // last_inum - 1: last_inum is the virtual $OrphanFiles.
    if (inum < fs->first_inum || inum > fs->last_inum - 1 ||
        !split_inum(xfs, inum, a)) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_INODE_NUM);
        tsk_error_set_errstr("xfs_dinode_load: address: %" PRIuINUM, inum);
        return 1;
    }
    const Alloc alloc = inode_allocation(xfs, a);
    if (alloc == Alloc::ERROR) {
        return 1;
    }
    if (alloc == Alloc::FREE) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_INODE_NUM);
        tsk_error_set_errstr("xfs_dinode_load: address: %" PRIuINUM, inum);
        return 1;
    }
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
        return 1;
    }
    if (!dinode_sound(xfs, inum, raw)) {
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

/* Fills `meta` from the raw inode in meta->content_ptr. */
void
dinode_copy(const XFS_INFO *xfs, TSK_FS_META *meta, TSK_INUM_T inum)
{
    const DinodeView d =
        dinode_view(xfs, static_cast<const uint8_t *>(meta->content_ptr));
    const xfs_dinode *core = d.core;
    meta->addr = inum;
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange): flags
    meta->flags =
        (TSK_FS_META_FLAG_ENUM)(TSK_FS_META_FLAG_ALLOC | TSK_FS_META_FLAG_USED);
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
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

/*
 * file_add_meta: the inode `inum`, or the virtual $OrphanFiles directory
 * for last_inum. meta->content_ptr keeps the raw inode, which load_attrs
 * and istat read.
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
    dinode_copy(xfs, meta, inum);
    return 0;
}

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

/*
 * The run list of a data fork of `d.nextents` extent records, covering the
 * file's blocks from 0: a sparse run for each gap between extents (holes)
 * and for each unwritten extent (preallocated, reads as zeros), and one for
 * a hole at the end up to `size`. `end_blocks` is where the runs end, which
 * can be past the end of the file (preallocated blocks, a directory's
 * blocks above its size). Errors (TSK_ERR_FS_INODE_COR): more records than
 * the fork holds, an empty, overlapping or out-of-order extent, an extent
 * outside its AG.
 */
uint8_t
extent_runs(const XFS_INFO *xfs, const DinodeView &d, TSK_OFF_T size,
    TSK_FS_ATTR_RUN *&head, TSK_DADDR_T &end_blocks)
{
    const TSK_FS_INFO *fs = &xfs->fs_info;
    head = NULL;
    TSK_FS_ATTR_RUN *tail = NULL;
    const auto corrupt = [&head]() {
        tsk_fs_attr_run_free(head);
        head = NULL;
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
        return 1;
    };
    const auto no_memory = [&head]() {
        tsk_fs_attr_run_free(head);
        head = NULL;
        return 1;
    };
    if (d.nextents > d.fork_size / sizeof(xfs_bmbt_rec)) {
        return corrupt();
    }
    // Blocks beyond this would overflow a byte offset.
    const uint64_t max_blocks = INT64_MAX / fs->block_size;
    const xfs_bmbt_rec *recs = reinterpret_cast<const xfs_bmbt_rec *>(d.fork);
    uint64_t cursor = 0; // file block where the runs end so far
    for (uint64_t i = 0; i < d.nextents; ++i) {
        const uint64_t l0 = tsk_getu64(TSK_BIG_ENDIAN, recs[i].l0);
        const uint64_t l1 = tsk_getu64(TSK_BIG_ENDIAN, recs[i].l1);
        const bool unwritten = (l0 >> 63) != 0;
        const uint64_t startoff = (l0 & ((uint64_t{1} << 63) - 1)) >> 9;
        const uint64_t fsblock = ((l0 & 0x1ff) << 43) | (l1 >> 21);
        const uint64_t count = l1 & ((uint64_t{1} << 21) - 1);
        if (count == 0 || startoff < cursor || startoff + count > max_blocks) {
            return corrupt();
        }
        const uint64_t agno = fsblock >> xfs->agblklog;
        const uint64_t agbno = fsblock & ((uint64_t{1} << xfs->agblklog) - 1);
        if (agno >= xfs->agcount ||
            agbno + count > ag_length(xfs, (uint32_t)agno)) {
            return corrupt();
        }
        if (startoff > cursor &&
            !append_run(head, tail, cursor, 0, startoff - cursor, true)) {
            return no_memory();
        }
        if (!append_run(head, tail, startoff,
                ag_block(xfs, (uint32_t)agno, (uint32_t)agbno), count,
                unwritten)) {
            return no_memory();
        }
        cursor = startoff + count;
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

/*
 * load_attrs: the default (data) attribute from the data fork of the raw
 * inode in meta->content_ptr. Extent lists are non-resident runs; local
 * data (short-form directories, inline symlinks) is resident; device files
 * have no data. Extent B+trees are not read yet (#42): TSK_ERR_FS_UNSUPFUNC.
 * Extended attributes (the attr fork) are not read.
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
    const XFS_INFO *xfs = reinterpret_cast<const XFS_INFO *>(fs_file->fs_info);
    if (meta->attr != NULL && meta->attr_state == TSK_FS_META_ATTR_STUDIED) {
        return 0;
    }
    if (meta->attr_state == TSK_FS_META_ATTR_ERROR) {
        return 1;
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

    TSK_FS_ATTR_RUN *runs = NULL;
    TSK_DADDR_T end_blocks = 0;
    switch (d.format) {
    case XFS_DINODE_FMT_DEV:
        break;
    case XFS_DINODE_FMT_LOCAL:
        if ((uint64_t)meta->size > d.fork_size) {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
            return 1;
        }
        break;
    case XFS_DINODE_FMT_EXTENTS:
        if (extent_runs(xfs, d, meta->size, runs, end_blocks)) {
            return 1;
        }
        break;
    default:
        // XFS_DINODE_FMT_BTREE (#42), and formats data forks don't have.
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
        const TSK_OFF_T block_size = xfs->fs_info.block_size;
        const TSK_OFF_T rounded =
            (meta->size + block_size - 1) / block_size * block_size;
        const TSK_OFF_T runs_end = (TSK_OFF_T)end_blocks * block_size;
        attr = tsk_fs_attrlist_getnew(meta->attr, TSK_FS_ATTR_NONRES);
        if (attr == NULL) {
            tsk_fs_attr_run_free(runs);
            return 1;
        }
        if (tsk_fs_attr_set_run(fs_file, attr, runs, NULL,
                TSK_FS_ATTR_TYPE_DEFAULT, TSK_FS_ATTR_ID_DEFAULT, meta->size,
                meta->size, runs_end > rounded ? runs_end : rounded,
                TSK_FS_ATTR_FLAG_NONE, 0)) {
            tsk_fs_attr_run_free(runs);
            return 1;
        }
    }
    meta->attr_state = TSK_FS_META_ATTR_STUDIED;
    return 0;
}

TSK_RETVAL_ENUM
xfs_dir_open_meta(TSK_FS_INFO *, TSK_FS_DIR **, TSK_INUM_T, int)
{
    set_unsupported();
    return TSK_ERR;
}

/* --- istat --- */

struct PrintAddr {
    FILE *h;
    int idx;
};

/* File walk callback of istat: each block's address, 0 for a hole, eight
 * per line (as ext's istat prints them). Unlike ext's, it doesn't keep to
 * TSK_FS_BLOCK_FLAG_CONT blocks: XFS has no block flags yet (#44), and its
 * file walk reports content blocks only. */
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

uint8_t
xfs_fscheck(TSK_FS_INFO *, FILE *)
{
    set_unsupported();
    return 1;
}

/* --- fsstat --- */

struct FeatureName {
    uint32_t bit;
    const char *name;
};

const FeatureName VERSION_BITS[] = {
    {XFS_SB_VERSION_ATTRBIT, "attr"},
    {XFS_SB_VERSION_NLINKBIT, "nlink"},
    {XFS_SB_VERSION_QUOTABIT, "quota"},
    {XFS_SB_VERSION_ALIGNBIT, "align"},
    {XFS_SB_VERSION_DALIGNBIT, "dalign"},
    {XFS_SB_VERSION_SHAREDBIT, "shared"},
    {XFS_SB_VERSION_LOGV2BIT, "logv2"},
    {XFS_SB_VERSION_SECTORBIT, "sector"},
    {XFS_SB_VERSION_EXTFLGBIT, "extflg"},
    {XFS_SB_VERSION_DIRV2BIT, "dirv2"},
    {XFS_SB_VERSION_BORGBIT, "ascii-ci"},
    {XFS_SB_VERSION_MOREBITSBIT, "morebits"},
};

const FeatureName FEATURES2[] = {
    {XFS_SB_VERSION2_LAZYSBCOUNTBIT, "lazy-count"},
    {XFS_SB_VERSION2_ATTR2BIT, "attr2"},
    {XFS_SB_VERSION2_PARENTBIT, "parent"},
    {XFS_SB_VERSION2_PROJID32BIT, "projid32bit"},
    {XFS_SB_VERSION2_CRCBIT, "crc"},
    {XFS_SB_VERSION2_FTYPE, "ftype"},
};

const FeatureName RO_COMPAT[] = {
    {XFS_SB_FEAT_RO_COMPAT_FINOBT, "finobt"},
    {XFS_SB_FEAT_RO_COMPAT_RMAPBT, "rmapbt"},
    {XFS_SB_FEAT_RO_COMPAT_REFLINK, "reflink"},
    {XFS_SB_FEAT_RO_COMPAT_INOBTCNT, "inobtcount"},
};

const FeatureName INCOMPAT[] = {
    {XFS_SB_FEAT_INCOMPAT_FTYPE, "ftype"},
    {XFS_SB_FEAT_INCOMPAT_SPINODES, "sparse"},
    {XFS_SB_FEAT_INCOMPAT_META_UUID, "meta-uuid"},
    {XFS_SB_FEAT_INCOMPAT_BIGTIME, "bigtime"},
    {XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR, "needsrepair"},
    {XFS_SB_FEAT_INCOMPAT_NREXT64, "nrext64"},
    {XFS_SB_FEAT_INCOMPAT_EXCHRANGE, "exchange"},
    {XFS_SB_FEAT_INCOMPAT_PARENT, "parent"},
    {XFS_SB_FEAT_INCOMPAT_METADIR, "metadir"},
};

/* The bits of `flags` that `names` knows. */
template <size_t N>
uint32_t
known_bits(uint32_t flags, const FeatureName (&names)[N])
{
    uint32_t known = 0;
    for (const FeatureName &f : names) {
        known |= f.bit;
    }
    return flags & known;
}

/* Prints the names (xfs_info's) of the known bits of `flags`, each after
 * `sep`, which is ", " after the first name. Unknown bits are not printed. */
template <size_t N>
void
print_names(
    FILE *h, uint32_t flags, const FeatureName (&names)[N], const char *&sep)
{
    for (const FeatureName &f : names) {
        if (flags & f.bit) {
            tsk_fprintf(h, "%s%s", sep, f.name);
            sep = ", ";
        }
    }
}

/* Prints `label` and the names of the known bits of `flags`, as the earlier
 * module did: only if there is one. */
template <size_t N>
void
print_features(
    FILE *h, const char *label, uint32_t flags, const FeatureName (&names)[N])
{
    if (known_bits(flags, names) == 0) {
        return;
    }
    tsk_fprintf(h, "%s", label);
    const char *sep = "";
    print_names(h, flags, names, sep);
    tsk_fprintf(h, "\n");
}

/* sb_features2, with the bits an old kernel wrote to sb_bad_features2
 * instead (the kernel merges the two at mount, as xfs_info shows them). */
uint32_t
features2(const xfs_dsb &sb)
{
    return tsk_getu32(TSK_BIG_ENDIAN, sb.sb_features2) |
        tsk_getu32(TSK_BIG_ENDIAN, sb.sb_bad_features2);
}

/* The feature flags of sb_versionnum and, if its MOREBITS flag is set,
 * sb_features2, on one line under the label UFS's fsstat uses for its
 * superblock flags. They are all the features a v4 superblock has. */
void
print_flags(FILE *h, const xfs_dsb &sb)
{
    const uint32_t version_bits =
        tsk_getu16(TSK_BIG_ENDIAN, sb.sb_versionnum) & ~XFS_SB_VERSION_NUMBITS;
    const uint32_t more =
        (version_bits & XFS_SB_VERSION_MOREBITSBIT) ? features2(sb) : 0;
    if (known_bits(version_bits, VERSION_BITS) == 0 &&
        known_bits(more, FEATURES2) == 0) {
        return;
    }
    tsk_fprintf(h, "Flags: ");
    const char *sep = "";
    print_names(h, version_bits, VERSION_BITS, sep);
    print_names(h, more, FEATURES2, sep);
    tsk_fprintf(h, "\n");
}

uint8_t
xfs_fsstat(TSK_FS_INFO *fs, FILE *h)
{
    const XFS_INFO *xfs = reinterpret_cast<const XFS_INFO *>(fs);
    const xfs_dsb &sb = xfs->sb;
    const SbValues v = sb_values(sb);
    tsk_error_reset();

    tsk_fprintf(h, "FILE SYSTEM INFORMATION\n");
    tsk_fprintf(h, "--------------------------------------------\n");
    tsk_fprintf(h, "File System Type : %s\n", "XFS");
    // sb_fname has no terminating NUL when all 12 bytes are used.
    char label[sizeof(sb.sb_fname) + 1];
    memcpy(label, sb.sb_fname, sizeof(sb.sb_fname));
    label[sizeof(sb.sb_fname)] = '\0';
    tsk_fprintf(h, "Volume Name : %s\n", label);
    const uint8_t *u = sb.sb_uuid;
    tsk_fprintf(h,
        "Volume ID: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x\n",
        u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10],
        u[11], u[12], u[13], u[14], u[15]);
    tsk_fprintf(h, "Version: %u\n", (unsigned)xfs->version);
    print_flags(h, sb);
    if (xfs->version == XFS_SB_VERSION_5) {
        // No compat features are defined, so there is no compat line.
        print_features(h, "InCompat Features: ",
            tsk_getu32(TSK_BIG_ENDIAN, sb.sb_features_incompat), INCOMPAT);
        print_features(h, "Read Only Compat Features : ",
            tsk_getu32(TSK_BIG_ENDIAN, sb.sb_features_ro_compat), RO_COMPAT);
    }

    tsk_fprintf(h, "\nMETADATA INFORMATION\n");
    tsk_fprintf(h, "--------------------------------------------\n");
    tsk_fprintf(h, "Inode Range: %" PRIuINUM " - %" PRIuINUM "\n",
        fs->first_inum, fs->last_inum);
    tsk_fprintf(h, "Root Inode : %" PRIuINUM "\n", fs->root_inum);
    tsk_fprintf(h, "Inode Count : %" PRIu64 "\n",
        tsk_getu64(TSK_BIG_ENDIAN, sb.sb_icount));
    tsk_fprintf(h, "Free Inode Count : %" PRIu64 "\n",
        tsk_getu64(TSK_BIG_ENDIAN, sb.sb_ifree));
    tsk_fprintf(h, "Inode Size : %" PRIu16 "\n", v.inodesize);
    tsk_fprintf(h, "Inode per Block : %" PRIu16 "\n", v.inopblock);
    tsk_fprintf(h, "Inode max persentage : %" PRIu8 "\n", sb.sb_imax_pct);

    tsk_fprintf(h, "\nCONTENT INFORMATION\n");
    tsk_fprintf(h, "--------------------------------------------\n");
    tsk_fprintf(h, "Block Range : %" PRIuDADDR " - %" PRIuDADDR "\n",
        fs->first_block, fs->last_block);
    if (fs->last_block_act != fs->last_block) {
        tsk_fprintf(h,
            "Total Range in Image: %" PRIuDADDR " - %" PRIuDADDR "\n",
            fs->first_block, fs->last_block_act);
    }
    tsk_fprintf(h, "Block Size : %" PRIu32 "\n", fs->block_size);
    tsk_fprintf(h, "Block Count : %" PRIu64 "\n", v.dblocks);
    tsk_fprintf(h, "Free Block Count : %" PRIu64 "\n",
        tsk_getu64(TSK_BIG_ENDIAN, sb.sb_fdblocks));
    tsk_fprintf(h, "Allocation Group Block Size : %" PRIu32 "\n", v.agblocks);
    tsk_fprintf(h, "Allocation Group Count : %" PRIu32 "\n", v.agcount);
    tsk_fprintf(h, "Sector Size : %" PRIu16 "\n", v.sectsize);
    return 0;
}

void
xfs_close(TSK_FS_INFO *fs)
{
    if (fs == nullptr) {
        return;
    }
    fs->tag = 0;
    tsk_fs_free(fs);
}

} // namespace

/*
 * Opens the XFS file system at byte `offset` of `img_info`. `ftype` must be
 * an XFS type. The password and `test` are not used. Returns NULL with a TSK
 * error if the superblock is not XFS's or describes an impossible geometry.
 */
TSK_FS_INFO *
xfs_open(TSK_IMG_INFO *img_info, TSK_OFF_T offset, TSK_FS_TYPE_ENUM ftype,
    const char * /*a_pass*/, uint8_t /*test*/)
{
    tsk_error_reset();
    if (TSK_FS_TYPE_ISXFS(ftype) == 0) {
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        tsk_error_set_errstr("Invalid FS Type in xfs_open");
        return NULL;
    }
    if (img_info == NULL) {
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        return NULL;
    }
    if (img_info->sector_size == 0) {
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        tsk_error_set_errstr("xfs_open: sector size is 0");
        return NULL;
    }

    XFS_INFO *xfs =
        reinterpret_cast<XFS_INFO *>(tsk_fs_malloc(sizeof(XFS_INFO)));
    if (xfs == NULL) {
        return NULL;
    }
    TSK_FS_INFO *fs = &xfs->fs_info;
    fs->ftype = TSK_FS_TYPE_XFS;
    fs->flags = TSK_FS_INFO_FLAG_NONE;
    fs->img_info = img_info;
    fs->offset = offset;
    fs->tag = TSK_FS_INFO_TAG;
    fs->endian = TSK_BIG_ENDIAN;
    fs->duname = "Block";

    const ssize_t cnt =
        tsk_fs_read(fs, 0, reinterpret_cast<char *>(&xfs->sb), sizeof(xfs->sb));
    if (cnt != (ssize_t)sizeof(xfs->sb)) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_error_set_errno(TSK_ERR_FS_READ);
        }
        tsk_error_set_errstr2("xfs_open: superblock");
        xfs_close(fs);
        return NULL;
    }
    const xfs_dsb &sb = xfs->sb;
    const SbValues v = sb_values(sb);
    if (v.magic != XFS_SB_MAGIC) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_MAGIC);
        tsk_error_set_errstr("not an xfs file system (magic)");
        if (tsk_verbose) {
            tsk_fprintf(stderr, "xfs_open : invalid magic\n");
        }
        xfs_close(fs);
        return NULL;
    }
    const SbCheck check = check_superblock(sb);
    if (check != SbCheck::GOOD) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_CORRUPT);
        if (check == SbCheck::BAD_BLOCK_SIZE) {
            tsk_error_set_errstr("Not an XFS file system (block size)");
            if (tsk_verbose) {
                tsk_fprintf(stderr, "xfs_open : invalid block size\n");
            }
        }
        xfs_close(fs);
        return NULL;
    }

    xfs->version = v.versionnum & XFS_SB_VERSION_NUMBITS;
    xfs->agblocks = v.agblocks;
    xfs->agcount = v.agcount;
    xfs->sectsize = v.sectsize;
    xfs->inodesize = v.inodesize;
    xfs->agblklog = sb.sb_agblklog;
    xfs->inopblog = sb.sb_inopblog;

    // Blocks: linear, see the top of this file.
    fs->block_size = v.blocksize;
    fs->dev_bsize = img_info->sector_size;
    fs->block_count = v.dblocks;
    fs->first_block = 0;
    fs->last_block = fs->last_block_act = v.dblocks - 1;
    const TSK_OFF_T avail = img_info->size - offset;
    if (avail < 0 || (uint64_t)avail / v.blocksize < v.dblocks) {
        fs->last_block_act =
            avail < (TSK_OFF_T)v.blocksize ? 0 : avail / v.blocksize - 1;
    }

    // Inodes: the XFS inode number. last_inum is one past the largest
    // possible inode of the last (possibly shorter) AG, for $OrphanFiles.
    const unsigned agino_bits = xfs->agblklog + xfs->inopblog;
    const uint64_t last_ag_blocks =
        v.dblocks - uint64_t{v.agcount - 1} * v.agblocks;
    const uint64_t max_inum = (uint64_t{v.agcount - 1} << agino_bits) |
        ((last_ag_blocks - 1) << xfs->inopblog) | (uint64_t{v.inopblock} - 1);
    fs->first_inum = 0;
    fs->root_inum = v.rootino;
    fs->last_inum = max_inum + 1;
    fs->inum_count = fs->last_inum - fs->first_inum + 1;
    fs->journ_inum = 0;

    // Volume ID: the user-visible UUID (blkid's), not sb_meta_uuid.
    memcpy(fs->fs_id, sb.sb_uuid, sizeof(sb.sb_uuid));
    fs->fs_id_used = sizeof(sb.sb_uuid);

    fs->inode_walk = xfs_inode_walk;
    fs->block_walk = xfs_block_walk;
    fs->block_getflags = xfs_block_getflags;
    fs->get_default_attr_type = tsk_fs_unix_get_default_attr_type;
    fs->load_attrs = xfs_load_attrs;
    fs->file_add_meta = xfs_inode_lookup;
    fs->dir_open_meta = xfs_dir_open_meta;
    fs->fsstat = xfs_fsstat;
    fs->fscheck = xfs_fscheck;
    fs->istat = xfs_istat;
    fs->name_cmp = tsk_fs_unix_name_cmp;
    fs->close = xfs_close;

    if (tsk_verbose) {
        tsk_fprintf(stderr,
            "inodes %" PRIu64 " root ino %" PRIuINUM " blocks %" PRIu64
            " inodes/block %" PRIu32 "\n",
            tsk_getu64(TSK_BIG_ENDIAN, sb.sb_icount), fs->root_inum, v.dblocks,
            (uint32_t)v.inopblock);
    }
    return fs;
}
