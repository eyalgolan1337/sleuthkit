/*
 * The Sleuth Kit (fork): read-only XFS support, internal header.
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
#define XFS_MIN_BLOCKSIZE_LOG 9     /* 512 bytes */
#define XFS_MAX_BLOCKSIZE_LOG 16    /* 64 KiB */
#define XFS_MIN_CRC_BLOCKSIZE 1024  /* v5 */
#define XFS_MIN_SECTORSIZE_LOG 9    /* 512 bytes */
#define XFS_MAX_SECTORSIZE_LOG 15   /* 32 KiB */
#define XFS_DINODE_MIN_LOG 8        /* 256 bytes */
#define XFS_DINODE_MAX_LOG 11       /* 2 KiB */
#define XFS_DINODE_MIN_CRC_SIZE 512 /* v5 */
#define XFS_MIN_AG_BLOCKS 64
#define XFS_MAX_AGINO_BITS 32 /* an AG-relative inode number is 32 bits */

/* The superblock, at byte 0 of the file system (and of each AG). The
 * metadir fields at the end exist only with INCOMPAT_METADIR. */
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
    /* INCOMPAT_METADIR from here */
    uint8_t sb_metadirino[8];
    uint8_t sb_rgcount[4];
    uint8_t sb_rgextents[4];
    uint8_t sb_rgblklog;
    uint8_t sb_pad[7];
} xfs_dsb;

/* The file system handle. */
typedef struct {
    TSK_FS_INFO fs_info; /* must be first */
    xfs_dsb sb;          /* the primary superblock, as on disk */

    /* Superblock values the module uses, in host order. */
    uint16_t version;  /* 4 or 5 */
    uint32_t agblocks; /* blocks per AG (the last AG may be shorter) */
    uint32_t agcount;
    uint8_t agblklog; /* bits of the AG block number in an fsblock */
    uint8_t inopblog; /* bits of the inode-in-block number */
    uint16_t inode_size;
} XFS_INFO;

#ifdef __cplusplus
}
#endif

#endif
