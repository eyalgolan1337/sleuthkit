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
 * Message and output strings are those of the earlier XFS module of TSK
 * (develop-4.13) or labels the other file systems' fsstat already prints;
 * an error with no such string carries only its TSK error number.
 *
 * C++14. Nothing here throws: no C++ exception can reach the C callers.
 */

#include "tsk_xfs.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

// xfs_dsb must match the disk layout.
static_assert(sizeof(xfs_dsb) == 264, "");

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

uint8_t
xfs_inode_lookup(TSK_FS_INFO *, TSK_FS_FILE *, TSK_INUM_T)
{
    set_unsupported();
    return 1;
}

uint8_t
xfs_load_attrs(TSK_FS_FILE *)
{
    set_unsupported();
    return 1;
}

TSK_RETVAL_ENUM
xfs_dir_open_meta(TSK_FS_INFO *, TSK_FS_DIR **, TSK_INUM_T, int)
{
    set_unsupported();
    return TSK_ERR;
}

uint8_t
xfs_istat(TSK_FS_INFO *, TSK_FS_ISTAT_FLAG_ENUM, FILE *, TSK_INUM_T,
    TSK_DADDR_T, int32_t)
{
    set_unsupported();
    return 1;
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

/* Prints `label` and the names (xfs_info's) of the known bits of `flags`,
 * as the earlier module did: only if a bit is set. Unknown bits are not
 * printed. */
template <size_t N>
void
print_features(
    FILE *h, const char *label, uint32_t flags, const FeatureName (&names)[N])
{
    if (flags == 0) {
        return;
    }
    tsk_fprintf(h, "%s", label);
    const char *sep = "";
    for (const FeatureName &f : names) {
        if (flags & f.bit) {
            tsk_fprintf(h, "%s%s", sep, f.name);
            sep = ", ";
        }
    }
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
