/*
 * The Sleuth Kit
 *
 * This software is distributed under the Common Public License 1.0
 *
 * Read-only XFS support (the fork's, #11): internal header.
 *
 * On-disk structures of XFS v4 and v5 (big-endian, as in the kernel's
 * libxfs/xfs_format.h) and the XFS_INFO handle. Not installed; only the XFS
 * module includes it. See docs/llm/fs.md ("XFS").
 *
 * Fields are byte arrays read with tsk_getuNN(TSK_BIG_ENDIAN, ...), so the
 * structs have no padding and match the disk byte for byte.
 */
#ifndef TSK_XFS_H
#define TSK_XFS_H

#include "tsk_fs_i.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Superblock magic, "XFSB". */
#define XFS_SB_MAGIC 0x58465342

/* Version number, low four bits of sb_versionnum. */
#define XFS_SB_VERSION_NUMBITS 0x000f
#define XFS_SB_VERSION_4 4
#define XFS_SB_VERSION_5 5

/* Feature bits of sb_versionnum (v4 and v5). */
#define XFS_SB_VERSION_ATTRBIT 0x0010
#define XFS_SB_VERSION_NLINKBIT 0x0020
#define XFS_SB_VERSION_QUOTABIT 0x0040
#define XFS_SB_VERSION_ALIGNBIT 0x0080
#define XFS_SB_VERSION_DALIGNBIT 0x0100
#define XFS_SB_VERSION_SHAREDBIT 0x0200
#define XFS_SB_VERSION_LOGV2BIT 0x0400
#define XFS_SB_VERSION_SECTORBIT 0x0800
#define XFS_SB_VERSION_EXTFLGBIT 0x1000
#define XFS_SB_VERSION_DIRV2BIT 0x2000
#define XFS_SB_VERSION_BORGBIT 0x4000
#define XFS_SB_VERSION_MOREBITSBIT 0x8000

/* sb_features2 (valid when MOREBITSBIT is set). */
#define XFS_SB_VERSION2_LAZYSBCOUNTBIT 0x00000002
#define XFS_SB_VERSION2_ATTR2BIT 0x00000008
#define XFS_SB_VERSION2_PARENTBIT 0x00000010
#define XFS_SB_VERSION2_PROJID32BIT 0x00000080
#define XFS_SB_VERSION2_CRCBIT 0x00000100
#define XFS_SB_VERSION2_FTYPE 0x00000200

/* v5 read-only compatible features. */
#define XFS_SB_FEAT_RO_COMPAT_FINOBT 0x00000001
#define XFS_SB_FEAT_RO_COMPAT_RMAPBT 0x00000002
#define XFS_SB_FEAT_RO_COMPAT_REFLINK 0x00000004
#define XFS_SB_FEAT_RO_COMPAT_INOBTCNT 0x00000008

/* v5 incompatible features. */
#define XFS_SB_FEAT_INCOMPAT_FTYPE 0x00000001
#define XFS_SB_FEAT_INCOMPAT_SPINODES 0x00000002
#define XFS_SB_FEAT_INCOMPAT_META_UUID 0x00000004
#define XFS_SB_FEAT_INCOMPAT_BIGTIME 0x00000008
#define XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR 0x00000010
#define XFS_SB_FEAT_INCOMPAT_NREXT64 0x00000020
#define XFS_SB_FEAT_INCOMPAT_EXCHRANGE 0x00000040
#define XFS_SB_FEAT_INCOMPAT_PARENT 0x00000080
#define XFS_SB_FEAT_INCOMPAT_METADIR 0x00000100

/* v5 log-incompatible features. */
#define XFS_SB_FEAT_INCOMPAT_LOG_XATTRS 0x00000001

/* Geometry limits (xfs_format.h, xfs_sb.c). */
#define XFS_MIN_BLOCKSIZE_LOG 9    /* 512 bytes */
#define XFS_MAX_BLOCKSIZE_LOG 16   /* 64 KiB */
#define XFS_MIN_CRC_BLOCKSIZE 1024 /* v5 */
#define XFS_MIN_SECTORSIZE_LOG 9   /* 512 bytes */
#define XFS_MAX_SECTORSIZE_LOG 15  /* 32 KiB */
#define XFS_DINODE_MIN_LOG 8       /* 256 bytes */
#define XFS_DINODE_MAX_LOG 11      /* 2 KiB */
#define XFS_MIN_AG_BLOCKS 64
#define XFS_MAX_AGINO_BITS 32 /* an AG-relative inode number is 32 bits */

/* The superblock, at byte 0 of the file system (and of each AG). The
 * superblock goes on with metadir fields (INCOMPAT_METADIR), not read. */
typedef struct {
    uint8_t sb_magicnum[4];
    uint8_t sb_blocksize[4];
    uint8_t sb_dblocks[8];
    uint8_t sb_rblocks[8];
    uint8_t sb_rextents[8];
    uint8_t sb_uuid[16];
    uint8_t sb_logstart[8];
    uint8_t sb_rootino[8];
    uint8_t sb_rbmino[8];
    uint8_t sb_rsumino[8];
    uint8_t sb_rextsize[4];
    uint8_t sb_agblocks[4];
    uint8_t sb_agcount[4];
    uint8_t sb_rbmblocks[4];
    uint8_t sb_logblocks[4];
    uint8_t sb_versionnum[2];
    uint8_t sb_sectsize[2];
    uint8_t sb_inodesize[2];
    uint8_t sb_inopblock[2];
    uint8_t sb_fname[12];
    uint8_t sb_blocklog;
    uint8_t sb_sectlog;
    uint8_t sb_inodelog;
    uint8_t sb_inopblog;
    uint8_t sb_agblklog;
    uint8_t sb_rextslog;
    uint8_t sb_inprogress;
    uint8_t sb_imax_pct;
    uint8_t sb_icount[8];
    uint8_t sb_ifree[8];
    uint8_t sb_fdblocks[8];
    uint8_t sb_frextents[8];
    uint8_t sb_uquotino[8];
    uint8_t sb_gquotino[8];
    uint8_t sb_qflags[2];
    uint8_t sb_flags;
    uint8_t sb_shared_vn;
    uint8_t sb_inoalignmt[4];
    uint8_t sb_unit[4];
    uint8_t sb_width[4];
    uint8_t sb_dirblklog;
    uint8_t sb_logsectlog;
    uint8_t sb_logsectsize[2];
    uint8_t sb_logsunit[4];
    uint8_t sb_features2[4];
    uint8_t sb_bad_features2[4];
    /* v5 from here */
    uint8_t sb_features_compat[4];
    uint8_t sb_features_ro_compat[4];
    uint8_t sb_features_incompat[4];
    uint8_t sb_features_log_incompat[4];
    uint8_t sb_crc[4];
    uint8_t sb_spino_align[4];
    uint8_t sb_pquotino[8];
    uint8_t sb_lsn[8];
    uint8_t sb_meta_uuid[16];
} xfs_dsb;

/* --- Inodes (xfs_format.h, struct xfs_dinode) --- */

/* Inode magic, "IN". */
#define XFS_DINODE_MAGIC 0x494e

/* di_format: how a fork stores its data. */
#define XFS_DINODE_FMT_DEV 0     /* device number, no data */
#define XFS_DINODE_FMT_LOCAL 1   /* the data itself, in the fork */
#define XFS_DINODE_FMT_EXTENTS 2 /* an array of extent records */
#define XFS_DINODE_FMT_BTREE 3   /* the root of an extent B+tree */

/* di_flags. */
#define XFS_DIFLAG_REALTIME 0x0001 /* data on the realtime device */

/* di_flags2 (v3 inodes). */
#define XFS_DIFLAG2_BIGTIME 0x08 /* timestamps are bigtime */
#define XFS_DIFLAG2_NREXT64 0x10 /* 64-bit data fork extent count */

/* A bigtime timestamp counts nanoseconds from this many seconds before the
 * Unix epoch (-INT32_MIN, the earliest classic timestamp). */
#define XFS_BIGTIME_EPOCH_OFFSET 2147483648LL

/* Size of the inode core: the data fork starts right after it. */
#define XFS_DINODE_CORE_SIZE_V2 100 /* di_version 1 and 2 */
#define XFS_DINODE_CORE_SIZE_V3 176 /* di_version 3 (v5 file systems) */

/* A timestamp: big-endian seconds (signed, 4 bytes) then nanoseconds (4
 * bytes), or with XFS_DIFLAG2_BIGTIME one big-endian 64-bit count of
 * nanoseconds. */
typedef struct {
    uint8_t t[8];
} xfs_timestamp;

/* The inode core. v1 and v2 inodes end at di_crc. */
typedef struct {
    uint8_t di_magic[2];
    uint8_t di_mode[2];
    uint8_t di_version;
    uint8_t di_format;    /* data fork format */
    uint8_t di_onlink[2]; /* link count of v1 inodes */
    uint8_t di_uid[4];
    uint8_t di_gid[4];
    uint8_t di_nlink[4]; /* link count of v2 and v3 inodes */
    uint8_t di_projid_lo[2];
    uint8_t di_projid_hi[2];
    uint8_t di_big_nextents[8]; /* with XFS_DIFLAG2_NREXT64; else padding */
    xfs_timestamp di_atime;
    xfs_timestamp di_mtime;
    xfs_timestamp di_ctime;
    uint8_t di_size[8];
    uint8_t di_nblocks[8];
    uint8_t di_extsize[4];
    uint8_t di_nextents[4]; /* without XFS_DIFLAG2_NREXT64 */
    uint8_t di_anextents[2];
    uint8_t di_forkoff; /* attr fork offset in 8-byte units; 0: none */
    uint8_t di_aformat;
    uint8_t di_dmevmask[4];
    uint8_t di_dmstate[2];
    uint8_t di_flags[2];
    uint8_t di_gen[4];
    uint8_t di_next_unlinked[4];
    /* v3 from here */
    uint8_t di_crc[4];
    uint8_t di_changecount[8];
    uint8_t di_lsn[8];
    uint8_t di_flags2[8];
    uint8_t di_cowextsize[4];
    uint8_t di_pad2[12];
    xfs_timestamp di_crtime;
    uint8_t di_ino[8];
    uint8_t di_uuid[16];
} xfs_dinode;

/* An extent record (xfs_bmbt_rec): 128 bits, big-endian, from the top: the
 * unwritten flag (1), the file offset in blocks (54), the start fsblock (52)
 * and the length in blocks (21). */
typedef struct {
    uint8_t l0[8];
    uint8_t l1[8];
} xfs_bmbt_rec;

/* An extent B+tree (data fork format XFS_DINODE_FMT_BTREE). Its root is in
 * the fork: the level and the number of records (2 bytes each), then keys
 * (file offsets in blocks, 8 bytes each) and, after room for the most keys
 * the fork holds, pointers (fsblocks, 8 bytes each). The other blocks start
 * with a long-form B+tree block header (24 bytes, 72 on v5 with the CRC
 * fields): the magic, the level, the number of records and the siblings.
 * Nodes go on with keys and pointers laid out as in the root, leaves (level
 * 0) with extent records. */
#define XFS_BMAP_MAGIC 0x424d4150     /* "BMAP" */
#define XFS_BMAP_CRC_MAGIC 0x424d4133 /* "BMA3" */
#define XFS_BMDR_HDR_LEN 4
#define XFS_BTREE_LBLOCK_LEN 24
#define XFS_BTREE_LBLOCK_CRC_LEN 72
#define XFS_BMBT_KEY_SIZE 8
#define XFS_BMBT_PTR_SIZE 8

/* --- Symlinks (xfs_format.h) --- */

/* A target is at most this long (the kernel creates shorter ones only). It
 * is in the data fork if it fits (format local), else in blocks (a remote
 * symlink, format extents). */
#define XFS_SYMLINK_MAXLEN 1024

/* On v5, each extent of a remote symlink starts with this header; the
 * target's bytes follow. v4 blocks hold the bytes alone. */
#define XFS_SYMLINK_MAGIC 0x58534c4d /* "XSLM" */
typedef struct {
    uint8_t sl_magic[4];
    uint8_t sl_offset[4]; /* of this piece in the target */
    uint8_t sl_bytes[4];  /* length of this piece */
    uint8_t sl_crc[4];
    uint8_t sl_uuid[16];
    uint8_t sl_owner[8]; /* the symlink's inode number */
    uint8_t sl_blkno[8];
    uint8_t sl_lsn[8];
} xfs_dsymlink_hdr;

/* --- Inode allocation (xfs_format.h) --- */

/* The AGI, the AG's inode header, in its third sector. Only its start, the
 * fields read here. */
#define XFS_AGI_MAGIC 0x58414749 /* "XAGI" */
#define XFS_AGI_VERSION 1
#define XFS_AGI_SECTOR 2
typedef struct {
    uint8_t agi_magicnum[4];
    uint8_t agi_versionnum[4];
    uint8_t agi_seqno[4]; /* AG number */
    uint8_t agi_length[4];
    uint8_t agi_count[4];
    uint8_t agi_root[4];  /* inode B+tree root, an AG block */
    uint8_t agi_level[4]; /* inode B+tree levels */
} xfs_agi;

/* Inode B+tree blocks: a short-form B+tree block header (16 bytes, 56 on
 * v5 with the CRC fields), then keys and pointers (nodes) or records
 * (leaves). */
#define XFS_IBT_MAGIC 0x49414254     /* "IABT" */
#define XFS_IBT_CRC_MAGIC 0x49414233 /* "IAB3" */
#define XFS_BTREE_SBLOCK_LEN 16
#define XFS_BTREE_SBLOCK_CRC_LEN 56
#define XFS_BTREE_MAXLEVELS 9
typedef struct {
    uint8_t bb_magic[4];
    uint8_t bb_level[2]; /* 0 for a leaf */
    uint8_t bb_numrecs[2];
} xfs_btree_sblock;

/* An inode B+tree record: a chunk of 64 inodes and which of them are free
 * (bit i of ir_free for inode ir_startino + i; holes of a sparse chunk are
 * free too). A node's key is an ir_startino, its pointer an AG block, both
 * 4 bytes. */
#define XFS_INODES_PER_CHUNK 64
/* With sparse inode chunks (XFS_SB_FEAT_INCOMPAT_SPINODES), ir_counts is a
 * 2-byte hole mask, one bit for each 4 inodes of the chunk that are not
 * there, then the inode count and the free count (1 byte each). */
#define XFS_INODES_PER_HOLEMASK_BIT 4
typedef struct {
    uint8_t ir_startino[4];
    uint8_t ir_counts[4]; /* free count, or sparse holemask and counts */
    uint8_t ir_free[8];
} xfs_inobt_rec;

/* --- Directories (xfs_da_format.h) --- */

#define XFS_MAXNAMELEN 255

/* File types of directory entries (with ftype). */
#define XFS_DIR3_FT_UNKNOWN 0
#define XFS_DIR3_FT_REG_FILE 1
#define XFS_DIR3_FT_DIR 2
#define XFS_DIR3_FT_CHRDEV 3
#define XFS_DIR3_FT_BLKDEV 4
#define XFS_DIR3_FT_FIFO 5
#define XFS_DIR3_FT_SOCK 6
#define XFS_DIR3_FT_SYMLINK 7
#define XFS_DIR3_FT_WHT 8

/* A short-form directory (data fork local): a header of the entry count,
 * the count of entries with 8-byte inode numbers and the parent's inode
 * number, then the entries: name length, offset (2 bytes), the name, the
 * file type (with ftype) and the inode number. Inode numbers are 8 bytes
 * if i8count is not 0, else 4. "." and ".." are not stored. */
#define XFS_DIR2_SF_COUNT 0
#define XFS_DIR2_SF_I8COUNT 1
#define XFS_DIR2_SF_PARENT 2
#define XFS_DIR2_SF_ENTRY_NAME 3 /* namelen and offset come first */
#define XFS_INO32_SIZE 4
#define XFS_INO64_SIZE 8
#define XFS_MAXINUMBER ((1ULL << 56) - 1)

/* A directory that is not short-form is a file of directory blocks: the
 * data blocks, which hold the names, are below this byte offset; the index
 * (leaf and node blocks) starts at it, and the free-space index at twice
 * it. */
#define XFS_DIR2_LEAF_OFFSET (32ULL << 30)

/* Directory block magics: a block directory's single block, and a data
 * block of a leaf, node or B+tree directory. */
#define XFS_DIR2_BLOCK_MAGIC 0x58443242 /* "XD2B" */
#define XFS_DIR3_BLOCK_MAGIC 0x58444233 /* "XDB3" */
#define XFS_DIR2_DATA_MAGIC 0x58443244  /* "XD2D" */
#define XFS_DIR3_DATA_MAGIC 0x58444433  /* "XDD3" */

/* The header of a directory data or block block: the magic and three free
 * space records on v4; on v5 the CRC header, the records and padding. */
#define XFS_DIR2_DATA_HDR_SIZE 16
#define XFS_DIR3_DATA_HDR_SIZE 64

/* In a data area: an unused region starts with this tag and its length
 * (2 bytes each); an entry is the inode number (8 bytes), the name length,
 * the name, the file type (with ftype) and a 2-byte tag, padded to 8. */
#define XFS_DIR2_DATA_FREE_TAG 0xffff
#define XFS_DIR2_DATA_ALIGN 8
#define XFS_DIR2_DATA_ENTRY_NAME 9

/* The end of a block directory's block: leaf entries (hash and address, 8
 * bytes each), then the tail. */
#define XFS_DIR2_LEAF_ENTRY_SIZE 8
typedef struct {
    uint8_t count[4]; /* leaf entries */
    uint8_t stale[4]; /* of them, unused */
} xfs_dir2_block_tail;

/* The file system handle. */
typedef struct {
    TSK_FS_INFO fs_info; /* must be first */
    xfs_dsb sb;          /* the primary superblock, as on disk */

    /* Superblock values the module uses, in host order. */
    uint16_t version;   /* 4 or 5 */
    uint32_t agblocks;  /* blocks per AG (the last AG may be shorter) */
    uint32_t agcount;   /* number of AGs */
    uint16_t sectsize;  /* sector size in bytes */
    uint16_t inodesize; /* inode size in bytes */
    uint8_t agblklog;   /* bits of the AG block number in an fsblock */
    uint8_t inopblog;   /* bits of the inode-in-block number */
    uint32_t dirblksize; /* directory block size in bytes */
    uint8_t has_ftype;   /* 1: directory entries record the file type */
} XFS_INFO;

#ifdef __cplusplus
}

/* --- What the module's files share (xfs.cpp, xfs_meta.cpp, xfs_dent.cpp) --- */

namespace tsk_xfs {

/* Not implemented yet: the TSK error number alone. */
inline void
set_unsupported()
{
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_UNSUPFUNC);
}

/* Metadata that is not what its format says: the TSK error number alone. */
inline void
set_corrupt()
{
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_CORRUPT);
}

/* The fields of a raw inode (the inode size of the file system, as on disk)
 * that the module uses, in host order. */
struct DinodeView {
    const xfs_dinode *core;
    uint16_t mode;
    uint8_t version;
    uint8_t format;
    uint16_t flags;
    uint64_t flags2;     /* 0 before v3 */
    uint64_t nextents;   /* data fork extent records */
    const uint8_t *fork; /* the data fork */
    size_t fork_size;    /* its size in bytes */
};

/* xfs_meta.cpp */
DinodeView dinode_view(const XFS_INFO *xfs, const uint8_t *raw);
uint8_t xfs_inode_lookup(
    TSK_FS_INFO *fs, TSK_FS_FILE *a_fs_file, TSK_INUM_T inum);
uint8_t xfs_inode_walk(TSK_FS_INFO *fs, TSK_INUM_T start_inum,
    TSK_INUM_T end_inum, TSK_FS_META_FLAG_ENUM flags,
    TSK_FS_META_WALK_CB a_action, void *a_ptr);
uint8_t xfs_load_attrs(TSK_FS_FILE *fs_file);
uint8_t xfs_istat(TSK_FS_INFO *fs, TSK_FS_ISTAT_FLAG_ENUM istat_flags, FILE *h,
    TSK_INUM_T inum, TSK_DADDR_T numblock, int32_t sec_skew);

/* xfs_dent.cpp */
TSK_RETVAL_ENUM xfs_dir_open_meta(TSK_FS_INFO *fs, TSK_FS_DIR **a_fs_dir,
    TSK_INUM_T inum, int recursion_depth);

} // namespace tsk_xfs
#endif

#endif
