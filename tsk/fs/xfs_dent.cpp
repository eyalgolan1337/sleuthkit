/*
 * The Sleuth Kit
 *
 * This software is distributed under the Common Public License 1.0
 *
 * Read-only XFS support (the fork's, #11): directories.
 *
 * dir_open_meta: the names of short-form and block directories. See
 * docs/llm/fs.md ("XFS").
 *
 * C++14. Nothing here throws: no C++ exception can reach the C callers.
 */

#include "tsk_xfs.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tsk_xfs {

namespace {

/* --- Directories --- */

/* The name type of a directory entry's file type (with ftype). */
TSK_FS_NAME_TYPE_ENUM
name_type(uint8_t ftype)
{
    switch (ftype) {
    case XFS_DIR3_FT_REG_FILE:
        return TSK_FS_NAME_TYPE_REG;
    case XFS_DIR3_FT_DIR:
        return TSK_FS_NAME_TYPE_DIR;
    case XFS_DIR3_FT_CHRDEV:
        return TSK_FS_NAME_TYPE_CHR;
    case XFS_DIR3_FT_BLKDEV:
        return TSK_FS_NAME_TYPE_BLK;
    case XFS_DIR3_FT_FIFO:
        return TSK_FS_NAME_TYPE_FIFO;
    case XFS_DIR3_FT_SOCK:
        return TSK_FS_NAME_TYPE_SOCK;
    case XFS_DIR3_FT_SYMLINK:
        return TSK_FS_NAME_TYPE_LNK;
    case XFS_DIR3_FT_WHT:
        return TSK_FS_NAME_TYPE_WHT;
    default:
        return TSK_FS_NAME_TYPE_UNDEF;
    }
}

/* The name type of an entry whose file type byte (if the file system has
 * ftype) is `stored`: TSK_FS_NAME_TYPE_UNDEF without ftype, as on ext
 * without filetype. */
TSK_FS_NAME_TYPE_ENUM
entry_type(const XFS_INFO *xfs, uint8_t stored)
{
    return xfs->has_ftype ? name_type(stored) : TSK_FS_NAME_TYPE_UNDEF;
}

/* Adds the allocated name `name` (`len` bytes, stored as is) to `fs_dir`,
 * through `fs_name`, a buffer of XFS_MAXNAMELEN + 1 bytes. */
bool
add_name(TSK_FS_DIR *fs_dir, TSK_FS_NAME *fs_name, const uint8_t *name,
    size_t len, TSK_INUM_T inum, TSK_FS_NAME_TYPE_ENUM type)
{
    memcpy(fs_name->name, name, len);
    fs_name->name[len] = '\0';
    fs_name->meta_addr = inum;
    fs_name->type = type;
    fs_name->flags = TSK_FS_NAME_FLAG_ALLOC;
    return tsk_fs_dir_add(fs_dir, fs_name) == 0;
}

/* Directory content that is not what its format says. */
TSK_RETVAL_ENUM
dir_corrupt()
{
    set_corrupt();
    return TSK_COR;
}

/*
 * The entries of a short-form directory, `size` bytes of `data` (the data
 * fork): "." (the directory, `inum`) and ".." (the parent in the header),
 * both directories as the kernel's readdir reports them, then the stored
 * entries. An entry or header that doesn't fit in `size`, or an empty name,
 * is TSK_COR (TSK_ERR_FS_CORRUPT).
 */
TSK_RETVAL_ENUM
parse_shortform(const XFS_INFO *xfs, TSK_FS_DIR *fs_dir, TSK_FS_NAME *fs_name,
    TSK_INUM_T inum, const uint8_t *data, size_t size)
{
    if (size < XFS_DIR2_SF_PARENT) {
        return dir_corrupt();
    }
    const unsigned count = data[XFS_DIR2_SF_COUNT];
    const bool i8 = data[XFS_DIR2_SF_I8COUNT] != 0;
    const size_t ino_size = i8 ? XFS_INO64_SIZE : XFS_INO32_SIZE;
    const auto get_ino = [i8](const uint8_t *p) -> TSK_INUM_T {
        return i8 ? tsk_getu64(TSK_BIG_ENDIAN, p) & XFS_MAXINUMBER
                  : tsk_getu32(TSK_BIG_ENDIAN, p);
    };
    size_t pos = XFS_DIR2_SF_PARENT + ino_size;
    if (size < pos) {
        return dir_corrupt();
    }
    static const uint8_t dot[] = {'.', '.'};
    if (!add_name(fs_dir, fs_name, dot, 1, inum, TSK_FS_NAME_TYPE_DIR) ||
        !add_name(fs_dir, fs_name, dot, 2, get_ino(data + XFS_DIR2_SF_PARENT),
            TSK_FS_NAME_TYPE_DIR)) {
        return TSK_ERR;
    }
    const size_t ftype_size = xfs->has_ftype ? 1 : 0;
    for (unsigned i = 0; i < count; ++i) {
        if (size - pos < XFS_DIR2_SF_ENTRY_NAME) {
            return dir_corrupt();
        }
        const size_t namelen = data[pos];
        const size_t end =
            pos + XFS_DIR2_SF_ENTRY_NAME + namelen + ftype_size + ino_size;
        if (namelen == 0 || end > size) {
            return dir_corrupt();
        }
        const uint8_t *name = data + pos + XFS_DIR2_SF_ENTRY_NAME;
        const TSK_FS_NAME_TYPE_ENUM type = entry_type(xfs, name[namelen]);
        if (!add_name(fs_dir, fs_name, name, namelen,
                get_ino(name + namelen + ftype_size), type)) {
            return TSK_ERR;
        }
        pos = end;
    }
    return TSK_OK;
}

/*
 * The entries of a block directory's block, `buf` (one directory block), in
 * the order stored, "." and ".." included. Unused regions are skipped. A
 * data block of a leaf or node directory (#41) is TSK_ERR_FS_UNSUPFUNC. A
 * block whose magic, leaf count, unused region or entry doesn't fit, or an
 * empty name, is TSK_COR (TSK_ERR_FS_CORRUPT).
 */
TSK_RETVAL_ENUM
parse_block(const XFS_INFO *xfs, TSK_FS_DIR *fs_dir, TSK_FS_NAME *fs_name,
    const uint8_t *buf)
{
    const bool v5 = xfs->version == XFS_SB_VERSION_5;
    const uint32_t magic = tsk_getu32(TSK_BIG_ENDIAN, buf);
    if (magic == (v5 ? XFS_DIR3_DATA_MAGIC : XFS_DIR2_DATA_MAGIC)) {
        set_unsupported();
        return TSK_ERR;
    }
    if (magic != (v5 ? XFS_DIR3_BLOCK_MAGIC : XFS_DIR2_BLOCK_MAGIC)) {
        return dir_corrupt();
    }
    // The data area runs from the header to the leaf entries before the
    // tail at the end of the block.
    const size_t hdr = v5 ? XFS_DIR3_DATA_HDR_SIZE : XFS_DIR2_DATA_HDR_SIZE;
    const size_t tail_off = xfs->dirblksize - sizeof(xfs_dir2_block_tail);
    const xfs_dir2_block_tail *tail =
        reinterpret_cast<const xfs_dir2_block_tail *>(buf + tail_off);
    const uint32_t leaves = tsk_getu32(TSK_BIG_ENDIAN, tail->count);
    if (leaves > (tail_off - hdr) / XFS_DIR2_LEAF_ENTRY_SIZE) {
        return dir_corrupt();
    }
    const size_t end = tail_off - size_t{leaves} * XFS_DIR2_LEAF_ENTRY_SIZE;
    const size_t ftype_size = xfs->has_ftype ? 1 : 0;
    size_t pos = hdr;
    while (pos < end) {
        if (end - pos < XFS_DIR2_DATA_ALIGN) {
            return dir_corrupt();
        }
        if (tsk_getu16(TSK_BIG_ENDIAN, buf + pos) == XFS_DIR2_DATA_FREE_TAG) {
            const size_t len = tsk_getu16(TSK_BIG_ENDIAN, buf + pos + 2);
            if (len == 0 || len % XFS_DIR2_DATA_ALIGN != 0 || len > end - pos) {
                return dir_corrupt();
            }
            pos += len;
            continue;
        }
        const size_t namelen = buf[pos + XFS_DIR2_DATA_ENTRY_NAME - 1];
        // Inode number, name length, name, file type, 2-byte tag; aligned.
        const size_t size = (XFS_DIR2_DATA_ENTRY_NAME + namelen + ftype_size +
                                2 + XFS_DIR2_DATA_ALIGN - 1) /
            XFS_DIR2_DATA_ALIGN * XFS_DIR2_DATA_ALIGN;
        if (namelen == 0 || size > end - pos) {
            return dir_corrupt();
        }
        const uint8_t *name = buf + pos + XFS_DIR2_DATA_ENTRY_NAME;
        const TSK_FS_NAME_TYPE_ENUM type = entry_type(xfs, name[namelen]);
        if (!add_name(fs_dir, fs_name, name, namelen,
                tsk_getu64(TSK_BIG_ENDIAN, buf + pos), type)) {
            return TSK_ERR;
        }
        pos += size;
    }
    return TSK_OK;
}

/* Reads the entries of directory `fs_dir->fs_file` into `fs_dir`. */
TSK_RETVAL_ENUM
read_dir(const XFS_INFO *xfs, TSK_FS_DIR *fs_dir, TSK_FS_NAME *fs_name)
{
    TSK_FS_FILE *fs_file = fs_dir->fs_file;
    const TSK_FS_META *meta = fs_file->meta;
    const DinodeView d =
        dinode_view(xfs, static_cast<const uint8_t *>(meta->content_ptr));
    switch (d.format) {
    case XFS_DINODE_FMT_LOCAL:
        if (meta->size < 0 || (uint64_t)meta->size > d.fork_size) {
            return dir_corrupt();
        }
        return parse_shortform(
            xfs, fs_dir, fs_name, meta->addr, d.fork, (size_t)meta->size);
    case XFS_DINODE_FMT_EXTENTS: {
        // The size covers the data blocks: one for a block directory, more
        // for a leaf or node directory (#41).
        if (meta->size > (TSK_OFF_T)xfs->dirblksize) {
            set_unsupported();
            return TSK_ERR;
        }
        if (meta->size != (TSK_OFF_T)xfs->dirblksize) {
            return dir_corrupt();
        }
        std::vector<char> buf(xfs->dirblksize);
        const ssize_t cnt = tsk_fs_file_read(fs_file, 0, buf.data(),
            xfs->dirblksize, TSK_FS_FILE_READ_FLAG_NONE);
        if (cnt != (ssize_t)xfs->dirblksize) {
            if (cnt >= 0) {
                tsk_error_reset();
                tsk_error_set_errno(TSK_ERR_FS_READ);
            }
            return TSK_ERR;
        }
        return parse_block(xfs, fs_dir, fs_name,
            reinterpret_cast<const uint8_t *>(buf.data()));
    }
    default:
        // An extent B+tree (#41), or a format a directory doesn't have.
        set_unsupported();
        return TSK_ERR;
    }
}

} // namespace

/*
 * dir_open_meta: the names of directory `inum`. Short-form and block
 * directories are read; leaf, node and B+tree directories (#41) fail with
 * TSK_ERR_FS_UNSUPFUNC. Deleted names are not recovered, and the root has no
 * $OrphanFiles (orphans need the inode walk, #44); opening $OrphanFiles
 * itself fails with TSK_ERR_FS_UNSUPFUNC.
 */
TSK_RETVAL_ENUM
xfs_dir_open_meta(TSK_FS_INFO *fs, TSK_FS_DIR **a_fs_dir, TSK_INUM_T inum, int)
{
    const XFS_INFO *xfs = reinterpret_cast<const XFS_INFO *>(fs);
    if (inum < fs->first_inum || inum > fs->last_inum) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_WALK_RNG);
        tsk_error_set_errstr(
            "xfs_dir_open_meta: inode value: %" PRIuINUM "\n", inum);
        return TSK_ERR;
    }
    if (a_fs_dir == NULL) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        tsk_error_set_errstr("xfs_dir_open_meta: NULL fs_attr argument given");
        return TSK_ERR;
    }
    if (tsk_verbose) {
        tsk_fprintf(stderr,
            "xfs_dir_open_meta: Processing directory %" PRIuINUM "\n", inum);
    }
    TSK_FS_DIR *fs_dir = *a_fs_dir;
    if (fs_dir != NULL) {
        tsk_fs_dir_reset(fs_dir);
        fs_dir->addr = inum;
    }
    else if ((*a_fs_dir = fs_dir = tsk_fs_dir_alloc(fs, inum, 128)) == NULL) {
        return TSK_ERR;
    }
    if (inum == TSK_FS_ORPHANDIR_INUM(fs)) {
        set_unsupported();
        return TSK_ERR;
    }
    fs_dir->fs_file = tsk_fs_file_open_meta(fs, NULL, inum);
    if (fs_dir->fs_file == NULL) {
        tsk_error_errstr2_concat("- xfs_dir_open_meta");
        return TSK_COR;
    }
    if (fs_dir->fs_file->meta->type != TSK_FS_META_TYPE_DIR) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        return TSK_ERR;
    }
    TSK_FS_NAME *fs_name = tsk_fs_name_alloc(XFS_MAXNAMELEN + 1, 0);
    if (fs_name == NULL) {
        return TSK_ERR;
    }
    const TSK_RETVAL_ENUM ret = read_dir(xfs, fs_dir, fs_name);
    tsk_fs_name_free(fs_name);
    return ret;
}

} // namespace tsk_xfs
