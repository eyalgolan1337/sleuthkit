# TSK file system layer (`tsk/fs/`)

The file system layer turns a byte range of a disk image into files, directories, metadata records and blocks.
It sits above the image layer (`TSK_IMG_INFO`, see `base-and-img.md`) and the volume and pool layers (`vs-and-pool.md`), and below `TskAuto` and the C++ wrapper classes (`auto-hashdb-cpp.md`).
Every supported file system (NTFS, FAT12/16/32, exFAT, Ext2/3/4, UFS1/2, HFS+, ISO9660, YAFFS2, APFS, a "logical directory" pseudo file system, raw and swap) is exposed through one C struct, `TSK_FS_INFO`, and one set of `tsk_fs_*` functions.
Read this file before you call the `tsk_fs_*` API, change generic code in `tsk/fs/fs_*.c*`, or add or fix a file system implementation.
Line numbers are omitted on purpose; every claim names a file and a symbol you can search for.

## Mental model

### Five data categories

TSK sorts everything in a file system into five categories, and the API is organised around them (`tsk/docs/fs.dox`, section "File System Layers").

| Category | What it is | TSK struct | Address type |
|---|---|---|---|
| File system | Layout and sizes of the whole file system | `TSK_FS_INFO` | byte offset in the image |
| Data unit ("block") | Fixed-size unit that stores content | `TSK_FS_BLOCK` | `TSK_DADDR_T` |
| Metadata ("inode") | One record that describes a file: times, size, where its content is. Unix inode, NTFS MFT entry, FAT directory entry | `TSK_FS_META` | `TSK_INUM_T` |
| File name | A name stored in a parent directory that points to a metadata address | `TSK_FS_NAME` | none (index in a `TSK_FS_DIR`) |
| Application | Journals and similar extras | per file system | per file system |

A **file** (`TSK_FS_FILE`) is just a pair of pointers, `name` and `meta`, plus `fs_info`. Either pointer can be NULL:

- `name` is NULL when the file was opened by metadata address (`tsk_fs_file_open_meta`, `tsk_fs_meta_walk` callbacks).
- `meta` is NULL when a name points to a metadata address that is 0 on an unallocated name, cannot be loaded, or (NTFS) has a different sequence number.

Always test both pointers before dereferencing them.

An **attribute** (`TSK_FS_ATTR`) is a container of content that belongs to a metadata record. Most file systems give each file one attribute. NTFS gives each file many (`$STANDARD_INFORMATION`, `$FILE_NAME`, one `$DATA` per stream, index attributes). HFS+ and APFS use attributes for the data fork, resource fork, extended attributes and compression records. TSK also stores Ext/UFS indirect blocks as a separate attribute (`TSK_FS_ATTR_TYPE_UNIX_INDIR`). An attribute is either **resident** (`TSK_FS_ATTR_RES`, bytes held in `rd.buf`) or **non-resident** (`TSK_FS_ATTR_NONRES`, a linked list of **runs**, `TSK_FS_ATTR_RUN`, each a start block and a length in blocks). Each attribute has a type (`TSK_FS_ATTR_TYPE_ENUM`) and an `id` that is unique within the file; several attributes can share a type.

### Allocated, unallocated, orphan

Names, metadata records and blocks each carry their own allocation state, and the three states are independent.

- A name has exactly one of `TSK_FS_NAME_FLAG_ALLOC` or `TSK_FS_NAME_FLAG_UNALLOC` (deleted).
- A metadata record has `TSK_FS_META_FLAG_ALLOC` or `_UNALLOC`, and independently `_USED` (allocated at least once) or `_UNUSED`.
- An unallocated name that points to an allocated metadata record means either the record was reused by a new file, or the name is the old name of a moved file. Only NTFS sequence numbers can tell these apart. Elsewhere, compare `name->type` with `meta->type` as a heuristic.
- An **orphan file** is an unallocated, used metadata record that no name points to. TSK lists orphans under the virtual directory `$OrphanFiles` (see "Virtual files and `$OrphanFiles`").

### `TSK_FS_INFO` is a vtable "base class"

`TSK_FS_INFO` (`tsk/fs/tsk_fs.h`) holds generic fields (ranges, sizes, type) followed by function pointers that each implementation fills in. Each implementation defines a struct whose first member is `TSK_FS_INFO fs_info` (for example `NTFS_INFO` in `tsk_ntfs.h`, `EXT2FS_INFO` in `tsk_ext2fs.h`, `FATFS_INFO` in `tsk_fatfs.h`), so the generic pointer is cast to the specific one inside that implementation. APFS is the exception: it is a C++ object (`APFSFSCompat` in `apfs_compat.hpp`) that owns a `TSK_FS_INFO` member and is reached through `fs->impl`.

Callers use the `tsk_fs_*` wrappers, not the pointers. The exception is the journal: there are no `tsk_fs_j*` wrappers, so callers test `fs->jopen != NULL` and then call `fs->jopen`, `fs->jentry_walk` or `fs->jblk_walk` directly, as `tools/fstools/jls.cpp` and `jcat.cpp` do.

Fields whose meaning is not obvious:

- `offset` is the byte offset of the file system inside `img_info`.
- `last_block` is what the file system claims; `last_block_act` is clipped to the image. `last_block_act < last_block` means the image is truncated.
- `block_size` is the addressing unit of every `TSK_DADDR_T` in the API. For FAT it is the sector size, not the cluster size.
- `last_inum` is always the address of the virtual `$OrphanFiles` directory, not a real on-disk record (macro `TSK_FS_ORPHANDIR_INUM`).
- `ftype` is always a specific type, never a `_DETECT` value.
- `flags` (`TSK_FS_INFO_FLAG_ENUM`): `HAVE_SEQ` means metadata addresses have sequence numbers (set by NTFS), `HAVE_NANOSEC` means the `*_nano` time fields are filled, `ENCRYPTED` means reads are decrypted.

## File map

Public header: `tsk/fs/tsk_fs.h`. Do not include it directly; include `tsk/libtsk.h`. Its first roughly 1200 lines are the C API; the rest are inline C++ wrapper classes (`TskFsInfo`, `TskFsFile`, ...), covered in `auto-hashdb-cpp.md`.

Internal header: `tsk/fs/tsk_fs_i.h`. It is not installed. It declares the allocators (`tsk_fs_*_alloc`, `tsk_fs_dir_add`, `tsk_fs_attr_set_run`, ...), the orphan helpers, the per-file-system `*_open` prototypes, the `tsk_fs_nofs_*` stubs, `tsk_fs_malloc`/`tsk_fs_free`, and the NTFS USN journal API (`tsk_ntfs_usnjopen`, `tsk_ntfs_usnjentry_walk`, `tsk_fs_usnjls`), which is therefore internal.

`tsk/fs/apfs_fs.h` is a separate installed C header for APFS extras (`tsk_apfs_fsstat`, `tsk_apfs_istat`, `tsk_apfs_list_snapshots`, `tsk_apfs_free_snapshot_list`, `tsk_apfs_set_snapshot`). `tsk/libtsk.h` does not include it.

Generic code:

| File | Responsibility |
|---|---|
| `fs_open.c` | `tsk_fs_open_img[_decrypt]`, `tsk_fs_open_vol[_decrypt]`, the autodetect table `FS_OPENERS`, `tsk_fs_close`, `tsk_fs_malloc`, `tsk_fs_free` |
| `fs_types.c` | Name-to-type tables (`fs_type_table`, `fs_legacy_type_table`) and `tsk_fs_type_*` |
| `fs_io.c` | `tsk_fs_read*`, `tsk_fs_read_block*`, decryption hook, raw-CD pre/post bytes |
| `fs_block.c` | `TSK_FS_BLOCK` alloc/get/free, `tsk_fs_block_walk` dispatch |
| `fs_inode.c` | `TSK_FS_META` alloc/reset/close, `tsk_fs_meta_walk` dispatch |
| `fs_name.c` | `TSK_FS_NAME` alloc/copy/free, `tsk_fs_meta_make_ls`, `fls`-style printing |
| `fs_file.c` | `TSK_FS_FILE` open/close/read/walk, `tsk_fs_file_attr_get*`, owner SID, hashing |
| `fs_dir.c` | `TSK_FS_DIR` open/get/close, `tsk_fs_dir_add`, `tsk_fs_dir_walk`, `$OrphanFiles` construction |
| `fs_attr.c`, `fs_attrlist.c` | Attribute and run management, `tsk_fs_attr_read`, `tsk_fs_attr_walk`, attribute lookup rules |
| `fs_load.c` | `tsk_fs_load_file_action`, a file-walk callback that copies content into a buffer |
| `fs_parse.c` | `tsk_fs_parse_inum` (parses `INUM[-TYPE[-ID]]`) |
| `unix_misc.c` | Helpers shared by Unix-style implementations (`tsk_fs_unix_make_data_run`, `tsk_fs_unix_name_cmp`, `tsk_fs_unix_get_default_attr_type`) |
| `nofs_misc.c` | `tsk_fs_nofs_*` "not supported" stubs used by raw and swap |
| `encryptionHelper.cpp` | BitLocker glue (`handleVolumeEncryption`, `read_and_decrypt_bitlocker_blocks`, `tsk_fs_get_encryption_description`) |
| `walk_cpp.cpp` | C trampolines for C++ wrapper callbacks |
| `*_lib.c` | Library halves of the CLI tools (`tsk_fs_fls`, `tsk_fs_icat`, `tsk_fs_ils`, `tsk_fs_blkls`, `tsk_fs_ifind_*`, `tsk_fs_ffind`, `tsk_fs_usnjls`, ...). They print to stdout and return a status. `ifind_lib.c` also holds `tsk_fs_path2inum`. |

Implementations:

| File system | Files |
|---|---|
| NTFS | `ntfs.c`, `ntfs_dent.cpp`, `usn_journal.c`, `tsk_ntfs.h` |
| FAT12/16/32 and exFAT | `fatfs.c` (shared opener), `fatfs_meta.c`, `fatfs_dent.cpp`, `fatfs_utils.c`, `fatxxfs*`, `exfatfs*`, `tsk_fatfs.h`, `tsk_fatxxfs.h`, `tsk_exfatfs.h` |
| Ext2/3/4 | `ext2fs.c`, `ext2fs_dent.c`, `ext2fs_journal.c`, `tsk_ext2fs.h` |
| UFS1/2 ("FFS") | `ffs.c`, `ffs_dent.c`, `tsk_ffs.h` |
| HFS+ / HFSX | `hfs.c`, `hfs_dent.c`, `hfs_journal.c`, `hfs_unicompare.c`, `decmpfs.c`, `lzvn.c`, `tsk_hfs.h` |
| ISO9660 | `iso9660.c`, `iso9660_dent.c`, `tsk_iso9660.h` |
| YAFFS2 | `yaffs.cpp`, `tsk_yaffs.h` |
| APFS | `apfs.cpp`, `apfs_fs.cpp`, `apfs_compat.cpp`, `apfs_open.cpp`, `tsk_apfs.h(pp)`, `apfs_fs.h(pp)`, `apfs_compat.hpp` |
| Logical directory | `logical_fs.cpp`, `tsk_logical_fs.h` |
| Raw, swap | `rawfs.c`, `swapfs.c` |

Most files are C (`.c`); `ntfs_dent.cpp`, `fatfs_dent.cpp`, `yaffs.cpp`, `logical_fs.cpp`, `walk_cpp.cpp`, `encryptionHelper.cpp` and the APFS files are C++. Headers included by the C files (`tsk_fs.h` outside its `__cplusplus` block, `tsk_fs_i.h`, the per-file-system `tsk_*.h`) must stay valid C. There is no XFS or Btrfs support in this release.

Tests that exercise this layer are the standalone programs in `tests/` (`read_apis.cpp`, `fs_fname_apis.cpp`, `fs_attrlist_apis.cpp`, `fs_thread_test.cpp`, driven by `tests/runtests.sh`, which needs external test images); `unit_tests/` holds CppUnit tests for `tsk/base` only (see `build-and-test.md`). Runnable usage samples are in `samples/`.

## Which API for which task

All signatures are in `tsk/fs/tsk_fs.h`.

| Task | Use | Notes |
|---|---|---|
| Open a file system at a byte offset | `tsk_fs_open_img(TSK_IMG_INFO *, TSK_OFF_T, TSK_FS_TYPE_ENUM)` | Pass `TSK_FS_TYPE_DETECT` to autodetect |
| Open a partition found by the volume layer | `tsk_fs_open_vol(const TSK_VS_PART_INFO *, TSK_FS_TYPE_ENUM)` | Computes `start * vs->block_size + vs->offset` |
| Open with a password | `tsk_fs_open_img_decrypt(TSK_IMG_INFO *, TSK_OFF_T, TSK_FS_TYPE_ENUM, const char * password)`, `tsk_fs_open_vol_decrypt(...)` | See "Encryption" |
| Open a file by path | `tsk_fs_file_open(TSK_FS_INFO * a_fs, TSK_FS_FILE * a_fs_file, const char *a_path)` | UTF-8, `/` separated |
| Open a file by metadata address | `tsk_fs_file_open_meta(TSK_FS_INFO * fs, TSK_FS_FILE * fs_file, TSK_INUM_T addr)` | Faster; `name` stays NULL |
| Path to metadata address only | `tsk_fs_path2inum(TSK_FS_INFO * a_fs, const char *a_path, TSK_INUM_T * a_result, TSK_FS_NAME * a_fs_name)` | Returns -1 error, 0 found, 1 not found |
| Read file content at an offset | `tsk_fs_file_read(TSK_FS_FILE *, TSK_OFF_T, char *, size_t, TSK_FS_FILE_READ_FLAG_ENUM)` | Default attribute |
| Read a specific attribute or NTFS stream | `tsk_fs_file_read_type(TSK_FS_FILE *, TSK_FS_ATTR_TYPE_ENUM, uint16_t, TSK_OFF_T, char *, size_t, TSK_FS_FILE_READ_FLAG_ENUM)` or `tsk_fs_attr_read(const TSK_FS_ATTR * a_fs_attr, TSK_OFF_T a_offset, char *a_buf, size_t a_len, TSK_FS_FILE_READ_FLAG_ENUM a_flags)` | |
| Get the block addresses of a file | `tsk_fs_file_walk(TSK_FS_FILE * a_fs_file, TSK_FS_FILE_WALK_FLAG_ENUM a_flags, TSK_FS_FILE_WALK_CB a_action, void *a_ptr)` | Or read `attr->nrd.run` directly |
| List one directory | `tsk_fs_dir_open_meta(TSK_FS_INFO * a_fs, TSK_INUM_T a_addr)` or `tsk_fs_dir_open(TSK_FS_INFO * a_fs, const char *a_dir)`, then `tsk_fs_dir_getsize`, `tsk_fs_dir_get` or `tsk_fs_dir_get_name`, `tsk_fs_dir_close` | `tsk_fs_dir_get_name` skips metadata loading |
| Walk a directory tree | `tsk_fs_dir_walk(TSK_FS_INFO * a_fs, TSK_INUM_T a_inode, TSK_FS_DIR_WALK_FLAG_ENUM a_flags, TSK_FS_DIR_WALK_CB a_action, void *a_ptr)` | Start at `fs->root_inum` with `TSK_FS_DIR_WALK_FLAG_RECURSE` |
| Visit every metadata record | `tsk_fs_meta_walk(TSK_FS_INFO * a_fs, TSK_INUM_T a_start, TSK_INUM_T a_end, TSK_FS_META_FLAG_ENUM a_flags, TSK_FS_META_WALK_CB a_cb, void *a_ptr)` | Callback gets `name == NULL` |
| Read one block with its allocation flags | `tsk_fs_block_get(TSK_FS_INFO * fs, TSK_FS_BLOCK * fs_block, TSK_DADDR_T addr)` | Not decrypted, see traps |
| Read raw blocks or bytes | `tsk_fs_read_block(TSK_FS_INFO * a_fs, TSK_DADDR_T a_addr, char *a_buf, size_t a_len)`, `tsk_fs_read(TSK_FS_INFO * a_fs, TSK_OFF_T a_off, char *a_buf, size_t a_len)` | `a_len` of `tsk_fs_read_block` must be a multiple of `block_size`; `a_off` is relative to the file system start |
| Visit blocks by allocation state | `tsk_fs_block_walk(TSK_FS_INFO * a_fs, TSK_DADDR_T a_start_blk, TSK_DADDR_T a_end_blk, TSK_FS_BLOCK_WALK_FLAG_ENUM a_flags, TSK_FS_BLOCK_WALK_CB a_action, void *a_ptr)` | |
| Hash a file | `tsk_fs_file_hash_calc(TSK_FS_FILE *, TSK_FS_HASH_RESULTS *, TSK_BASE_HASH_ENUM)` | Hashes the default attribute |
| NTFS owner SID | `tsk_fs_file_get_owner_sid(TSK_FS_FILE *, char **)` | Caller frees the string; other file systems fail with `TSK_ERR_FS_UNSUPFUNC` |
| Map a type name such as `"ntfs"` | `tsk_fs_type_toid_utf8(const char *)`, `tsk_fs_type_toname(TSK_FS_TYPE_ENUM)` | |

Whole-image processing (every file of every file system in every partition) belongs to `TskAuto`; see `auto-hashdb-cpp.md` before writing your own loop.

### Example: open a file system and read a file by path

```c
#include <stdio.h>
#include "tsk/libtsk.h"

/* Returns 0 on success, 1 on error. fs_offset is in bytes. */
int cat_file(const char *image_path, TSK_OFF_T fs_offset, const char *file_path)
{
    int ret = 1;
    TSK_IMG_INFO *img = tsk_img_open_utf8_sing(image_path, TSK_IMG_TYPE_DETECT, 0);
    TSK_FS_INFO *fs = NULL;
    TSK_FS_FILE *file = NULL;
    char buf[65536];
    TSK_OFF_T off = 0;
    ssize_t n;

    if (img == NULL)
        goto done;
    if ((fs = tsk_fs_open_img(img, fs_offset, TSK_FS_TYPE_DETECT)) == NULL)
        goto done;
    if ((file = tsk_fs_file_open(fs, NULL, file_path)) == NULL)
        goto done;

    /* tsk_fs_file_open guarantees meta != NULL on success. */
    while (off < file->meta->size) {      /* reading at offset >= size is an error */
        n = tsk_fs_file_read(file, off, buf, sizeof(buf),
            TSK_FS_FILE_READ_FLAG_NONE);
        if (n <= 0)
            goto done;
        fwrite(buf, 1, (size_t) n, stdout);
        off += n;
    }
    ret = 0;

done:
    if (ret)
        tsk_error_print(stderr);
    tsk_fs_file_close(file);              /* safe on NULL */
    tsk_fs_close(fs);                     /* safe on NULL */
    if (img)
        tsk_img_close(img);
    return ret;
}
```

### Example: recursive directory walk that reads content safely

```c
static TSK_WALK_RET_ENUM
on_entry(TSK_FS_FILE *fs_file, const char *path, void *ptr)
{
    unsigned long *total = (unsigned long *) ptr;
    char buf[4096];

    /* fs_file, fs_file->name and fs_file->meta are borrowed: valid only until return. */
    if (fs_file->name == NULL || fs_file->name->name == NULL || fs_file->meta == NULL)
        return TSK_WALK_CONT;
    if (TSK_FS_ISDOT(fs_file->name->name))
        return TSK_WALK_CONT;
    if (fs_file->meta->type != TSK_FS_META_TYPE_REG)
        return TSK_WALK_CONT;             /* also skips TSK_FS_META_TYPE_VIRT files */

    printf("%s%s (%" PRIuINUM ")\n", path, fs_file->name->name, fs_file->meta->addr);

    for (TSK_OFF_T off = 0; off < fs_file->meta->size; ) {
        ssize_t n = tsk_fs_file_read(fs_file, off, buf, sizeof(buf),
            TSK_FS_FILE_READ_FLAG_NONE);
        if (n <= 0) {
            tsk_error_reset();            /* one unreadable file should not end the walk */
            break;
        }
        *total += (unsigned long) n;
        off += n;
    }
    return TSK_WALK_CONT;
}

uint8_t walk_all(TSK_FS_INFO *fs, unsigned long *total)
{
    return tsk_fs_dir_walk(fs, fs->root_inum,
        (TSK_FS_DIR_WALK_FLAG_ENUM) (TSK_FS_DIR_WALK_FLAG_ALLOC |
            TSK_FS_DIR_WALK_FLAG_RECURSE | TSK_FS_DIR_WALK_FLAG_NOORPHAN),
        on_entry, total);
}
```

### Example: iterate a file's attributes

```c
void list_attrs(TSK_FS_FILE *file)
{
    int cnt = tsk_fs_file_attr_getsize(file);   /* 0 on error, with the error cleared */
    for (int i = 0; i < cnt; i++) {
        const TSK_FS_ATTR *attr = tsk_fs_file_attr_get_idx(file, i);
        if (attr == NULL)
            continue;
        printf("type=%d id=%d name=%s size=%" PRIdOFF " %s\n",
            (int) attr->type, (int) attr->id,
            attr->name ? attr->name : "(none)", attr->size,
            (attr->flags & TSK_FS_ATTR_RES) ? "resident" : "non-resident");
        /* Read it with tsk_fs_attr_read(attr, 0, buf, len, TSK_FS_FILE_READ_FLAG_NONE). */
    }
}
```

The returned `TSK_FS_ATTR` pointers belong to `file->meta` and die with the file.

## Opening: types, autodetection, conflicts

### Type values

`TSK_FS_TYPE_ENUM` is documented as one bit per type with `_DETECT` values as the OR of the subtypes, but two details break that picture:

- `TSK_FS_TYPE_EXFAT` is `0x0a`, which equals `TSK_FS_TYPE_FAT12 | TSK_FS_TYPE_FAT32`. Never test `ftype & TSK_FS_TYPE_FAT12` or `ftype & TSK_FS_TYPE_FAT32`; compare with `==`. `TSK_FS_TYPE_ISFAT()` is true for exFAT.
- `TSK_FS_TYPE_EXT4` (`0x2000`) and `TSK_FS_TYPE_HFS_LEGACY` (`0x8000`) were added later, so the Ext and HFS groups are not contiguous. Use the `TSK_FS_TYPE_ISxxx(ftype)` macros.

The macro for the logical directory type is `TSK_FS_TYPE_ISDIR`, not `ISLOGICAL`. `TSK_FS_TYPE_UNSUPP` (`0xffffffff`) is what `tsk_fs_type_toid*` return for an unknown name. `tsk_fs_type_toid(const TSK_TCHAR *)` only looks at the first 15 characters.

Type names accepted by `tsk_fs_type_toid_utf8` are in `fs_type_table` in `fs_types.c` (`ntfs`, `fat`, `ext`, `ufs`, `hfs`, `iso9660`, `yaffs2`, `apfs`, `logical`, `raw`, `swap`, plus specific subtypes such as `exfat`, `ext4`, `hfsp`; legacy names such as `linux-ext3` are in `fs_legacy_type_table`). `tsk_fs_type_toname` returns the first table entry with an equal code, so a `_DETECT` code maps to the group name (`TSK_FS_TYPE_FAT_DETECT` gives `"fat"`).

### Autodetection (`tsk_fs_open_img_decrypt` in `fs_open.c`)

With `TSK_FS_TYPE_DETECT`, every opener in the `FS_OPENERS` table is tried, in this order, with the `test` argument set to 1:

NTFS, FAT (FAT12/16/32 then exFAT), Ext2/3/4, UFS, YAFFS2, HFS (only if `TSK_USE_HFS`, default 1 in `tsk_fs_i.h`), ISO9660, APFS (through `apfs_open_auto_detect`, a wrapper that drops the `test` argument).

Rules:

- All openers are tried even after one succeeds. If a second one also succeeds, both handles are closed and the call fails with `TSK_ERR_FS_MULTTYPE` and an error string of the form `"<second> or <first>"`. The caller must retry with an explicit type.
- Errors from openers that fail are discarded, except `TSK_ERR_FS_BITLOCKER_ERROR`, which is kept and returned after the loop (see traps for a side effect).
- If nothing matched: at offset 0 a known unsupported image format gives `TSK_ERR_IMG_UNSUPTYPE`; otherwise `detectVolumeEncryption` (`tsk/util/detect_encryption.c`) gives `TSK_ERR_FS_ENCRYPTED` for a BitLocker, LUKS or FileVault signature, `TSK_ERR_FS_POSSIBLY_ENCRYPTED` when entropy exceeds 7.5, and `TSK_ERR_FS_UNKTYPE` otherwise.
- Raw, swap and logical are never autodetected. Raw and swap must be requested by type.
- If `img_info->itype == TSK_IMG_TYPE_LOGICAL`, only `TSK_FS_TYPE_DETECT` and `TSK_FS_TYPE_LOGICAL` are accepted, and `logical_fs_open` is used.
- APFS is only detected when the image is a pool image; on any other image `apfs_open` fails with `TSK_ERR_FS_ARG` and autodetect moves on.

With an explicit type, the `ISxxx` macros are tested in this order: NTFS, FAT, FFS, EXT, HFS, ISO9660, RAW, SWAP, YAFFS2, APFS. Anything else (including `TSK_FS_TYPE_LOGICAL` on a non-logical image) fails with `TSK_ERR_FS_UNSUPTYPE`.

The `test` argument tells an opener it is being probed. ISO9660 and YAFFS2 read it (it changes their error handling, detection mode or verbose output); HFS only passes it on when it reopens an embedded HFS+ volume; NTFS, FAT, Ext, UFS and APFS ignore it.

### Pools and APFS

`tsk_fs_open_pool` and `tsk_fs_open_pool_decrypt` are declared in `tsk_fs.h` but have no definition. Do not call them. The working sequence, as used in `tools/fstools/fls.cpp`, is:

1. `pool = tsk_pool_open_img_sing(img, byte_offset, pooltype)`
2. `pool_img = pool->get_img_info(pool, pvol_block)`, which returns a `TSK_IMG_INFO` of type `TSK_IMG_TYPE_POOL`
3. `fs = tsk_fs_open_img_decrypt(pool_img, offset, TSK_FS_TYPE_APFS, password)`

`apfs_open` (`apfs_open.cpp`, declared in `tsk_fs_i.h` with four arguments and no `test` flag) requires a pool image and a type equal to `TSK_FS_TYPE_APFS` (which is also the value of `TSK_FS_TYPE_APFS_DETECT`). It ignores the offset argument; `fs->offset` is taken from the pool (`APFSFSCompat` constructor in `apfs_compat.cpp`). Pool details are in `vs-and-pool.md`.

## Encryption

| Scheme | Support | Compile-time gate |
|---|---|---|
| BitLocker on NTFS and FAT | Decrypts with a password or recovery password; a clear key needs no password | `HAVE_LIBMBEDTLS` |
| APFS software encryption | Decrypts with a password | `HAVE_LIBOPENSSL` |
| APFS hardware (T2) encryption | Detected, not decrypted | |
| LUKS, FileVault | Signature detection only, reported as `TSK_ERR_FS_ENCRYPTED` | |

- BitLocker: `ntfs_open` and `fatfs_open` call `handleVolumeEncryption` (`encryptionHelper.cpp`) before reading the boot sector. On success it sets `fs->encryption_type`, `fs->encryption_data` (a `BitlockerParser *`), `TSK_FS_INFO_FLAG_ENCRYPTED` and `block_size`. A wrong password, a missing password, or only unsupported key protectors fail the open with `TSK_ERR_FS_BITLOCKER_ERROR`. Reads are redirected in `tsk_fs_read_block_decrypt` (`fs_io.c`) to `read_and_decrypt_bitlocker_blocks`, because BitLocker relocates some sectors.
- `HAVE_LIBMBEDTLS` is defined only in `win32/libtsk/libtsk.vcxproj`. `configure.ac` has no check for it, so an autotools (Linux, macOS) build compiles the BitLocker handlers as no-ops: a BitLocker volume then fails to open and is reported through the generic "nothing matched" path.
- APFS: the password goes to `APFSFileSystem`. The file system sets `TSK_FS_INFO_FLAG_ENCRYPTED` and `fs->decrypt_block`, and `tsk_fs_read_block_decrypt` calls it per block with the run's `crypto_id`. Without `HAVE_LIBOPENSSL`, `APFSFSCompat::decrypt_block` fails with "crypto library not loaded".
- `HAVE_LIBOPENSSL` is defined only in the `Release|x64` configuration of `win32/libtsk/libtsk.vcxproj`. The OpenSSL check in `configure.ac` is commented out, so autotools builds and the other Visual Studio configurations have no APFS decryption.
- The password is passed to every opener during autodetection too, although the comment on `tsk_fs_open_img_decrypt` says it is "only used if type is specified". Ext, UFS, HFS, ISO9660, YAFFS2, raw and swap ignore it. The non-`_decrypt` functions pass `""`.
- `tsk_fs_get_encryption_description(TSK_FS_INFO* a_fs_info, char* a_desc, size_t a_descLen)` fills a zero-terminated description for BitLocker and leaves the buffer empty otherwise.
- `HAVE_LIBZ` gates zlib decompression of HFS+ and APFS compressed files (`decmpfs.c`, `hfs.c`, `apfs_compat.cpp`).

## Conventions

### Return values and errors

- Functions returning a pointer return NULL on error.
- Functions returning `uint8_t` return 1 on error and 0 on success.
- Functions returning `ssize_t` return -1 on error. Reading at an offset at or past the end of the attribute is an error (`TSK_ERR_FS_READ_OFF`), not a 0-byte read. A short positive count is possible.
- `tsk_fs_path2inum` and `tsk_fs_ifind_path` (`int8_t`) return -1 on error, 0 if found, 1 if not found.
- Internal directory code uses `TSK_RETVAL_ENUM`: `TSK_OK`, `TSK_ERR`, `TSK_COR` (corrupt, other data may still be processed).
- The error code and string are thread-local (`tsk_error_get_errno`, `tsk_error_get_errstr`, `tsk_error_print`, `tsk_error_reset`; see `base-and-img.md`). File system codes are `TSK_ERR_FS_*` in `tsk/base/tsk_base.h`.
- `tsk_fs_file_walk`, `tsk_fs_file_walk_type`, `tsk_fs_file_read_type` and `tsk_fs_attr_walk` call `tsk_error_reset()` on entry. `tsk_fs_file_read` does not.
- Read functions return zeros for sparse runs, for `TSK_FS_ATTR_RUN_FLAG_FILLER` runs (runs TSK could not recover), and for bytes past `nrd.initsize`. This is not reported as an error.

### Walk callbacks

All walk callbacks return `TSK_WALK_RET_ENUM`. `TSK_WALK_CONT` continues. `TSK_WALK_STOP` ends the walk and the walk function returns 0. `TSK_WALK_ERROR` ends the walk and the walk function returns 1.

`tsk_fs_dir_walk` (`fs_dir.c`, `tsk_fs_dir_walk_recursive`):

- The `TSK_FS_FILE` passed to the callback is reused for every entry. `fs_file->name` points into the directory's name array and `fs_file->meta` is freed right after the callback returns. Do not close it, do not keep any of the three pointers, and copy what you need. Reading content inside the callback is safe.
- `a_path` is the parent path relative to the start directory: `""` for entries of the start directory, then `"dir/sub/"` with a trailing slash and no leading slash.
- If neither `ALLOC` nor `UNALLOC` is given, both are assumed. An entry is reported when its name flags are a subset of the requested ones.
- `.` and `..` entries are reported; filter them with `TSK_FS_ISDOT(name)`.
- Recursion happens into an entry when the name type is a directory or undefined, the metadata type is a directory, and the name is allocated or both name and metadata are unallocated.
- Errors inside a subdirectory are swallowed (printed only when `tsk_verbose`) and the walk continues, unless the error class is `TSK_ERR_AUX` (out of memory). A return value of 0 therefore does not mean every directory was readable.
- Recursion depth is limited to 128 (`MAX_DEPTH`) and the path to 4096 bytes (`DIR_STRSZ`). Directory loops are detected with a stack of visited addresses and silently skipped.
- Entries of the root directory are not delivered in on-disk order (see traps).

`tsk_fs_meta_walk`: the callback gets a borrowed `TSK_FS_FILE` with `name == NULL`. If no ALLOC/UNALLOC or no USED/UNUSED flag is given, implementations assume both. `TSK_FS_META_FLAG_ORPHAN` is a request flag only and forces UNALLOC and USED.

`tsk_fs_file_walk` and `tsk_fs_attr_walk` (`tsk_fs_attr_walk_nonres`, `tsk_fs_attr_walk_res`):

- The callback is called with chunks of at most `block_size` bytes. `a_buf` is reused between calls.
- `a_addr` is the real block address only when `a_flags` has `TSK_FS_BLOCK_FLAG_RAW`. It is 0 for resident data (`TSK_FS_BLOCK_FLAG_RES`), sparse or filler runs (`TSK_FS_BLOCK_FLAG_SPARSE`), and compressed data.
- Flags (`TSK_FS_FILE_WALK_FLAG_ENUM`): `SLACK` continues to `nrd.allocsize` and stops zeroing past `initsize`; `NOSPARSE` skips sparse chunks; `NOID` ignores the id argument of `tsk_fs_file_walk_type`; `AONLY` delivers addresses without content (see traps).
- A run that points past `fs->last_block` ends the walk with `TSK_ERR_FS_RECOVER` if the file is unallocated and `TSK_ERR_FS_BLK_NUM` otherwise. This is common for deleted files.

### Ownership and lifetime

| Object | Obtained from | Released with | Notes |
|---|---|---|---|
| `TSK_FS_INFO` | `tsk_fs_open_*` | `tsk_fs_close` | Must outlive every file, directory and block made from it. It does not own the `TSK_IMG_INFO`. |
| `TSK_FS_FILE` | `tsk_fs_file_open`, `tsk_fs_file_open_meta`, `tsk_fs_dir_get` | `tsk_fs_file_close` | Frees `meta`, `name` and all attributes. |
| `TSK_FS_DIR` | `tsk_fs_dir_open`, `tsk_fs_dir_open_meta` | `tsk_fs_dir_close` | Frees its names and `fs_dir->fs_file`. |
| `const TSK_FS_NAME *` | `tsk_fs_dir_get_name` | nothing | Points into the directory; valid until `tsk_fs_dir_close`. |
| `const TSK_FS_ATTR *` | `tsk_fs_file_attr_get*` | nothing | Owned by `fs_file->meta`. |
| `TSK_FS_BLOCK` | `tsk_fs_block_get[_flag]` with a NULL block | `tsk_fs_block_free` | `tsk_fs_block_free` crashes on NULL. |
| SID string | `tsk_fs_file_get_owner_sid` | `free` | |

- `tsk_fs_file_open` and `tsk_fs_file_open_meta` accept an existing `TSK_FS_FILE` as their second argument and reuse it (its old `name` is freed, its `meta` reset). On failure the struct is freed only if the function allocated it.
- `tsk_fs_file_close`, `tsk_fs_dir_close` and `tsk_fs_close` return silently on NULL or on a wrong `tag`. The `tag` fields are set on allocation and zeroed on free; this is a debugging aid, not protection against use after free.
- Attributes are loaded lazily. `meta->attr` is valid only when `meta->attr_state == TSK_FS_META_ATTR_STUDIED`. Use the `tsk_fs_file_attr_*` accessors, which call `fs->load_attrs` when needed, instead of reading `meta->attr` directly.

### Attribute selection

- `tsk_fs_file_read`, `tsk_fs_file_walk` and `tsk_fs_file_attr_get` use the type returned by `fs->get_default_attr_type`. For NTFS that is `$DATA` for files and `$INDEX_ROOT` for directories (`ntfs_get_default_attr_type`); for APFS it is `TSK_FS_ATTR_TYPE_APFS_DATA`; for Unix-style file systems `TSK_FS_ATTR_TYPE_DEFAULT`.
- Lookup by type alone (`tsk_fs_attrlist_get` in `fs_attrlist.c`) returns the attribute with the lowest id, except that for `TSK_FS_ATTR_TYPE_NTFS_DATA` an unnamed attribute wins.
- `tsk_fs_file_attr_get_type(file, type, id, id_used)` ignores `id` when `id_used` is 0. The `NOID` read and walk flags map to `id_used = 0`.

### Enum traps

- `TSK_FS_NAME_TYPE_ENUM` and `TSK_FS_META_TYPE_ENUM` use different numbers for the same concepts (name `DIR` is 3 and `REG` is 5; meta `DIR` is 2 and `REG` is 1). Never cast or compare across them. Use `TSK_FS_IS_DIR_NAME(x)` and `TSK_FS_IS_DIR_META(x)`, which also accept the virtual directory types.
- `TSK_FS_ATTR_TYPE_ENUM` has aliases: `NTFS_VVER == NTFS_OBJID` (0x40), `NTFS_SYMLNK == NTFS_REPARSE` (0xC0), `HFS_DEFAULT == DEFAULT` (0x01), and every `APFS_*` value equals the matching `HFS_*` value. A `switch` over them needs the file system type for context.
- `TSK_FS_BLOCK_FLAG_ENUM` (values delivered to callbacks) and `TSK_FS_BLOCK_WALK_FLAG_ENUM` (request flags) share `ALLOC`, `UNALLOC`, `CONT`, `META` values, but `AONLY` is `0x200` in the first and `0x10` in the second.
- In `TSK_FS_META`, `time2` is a union selected by file system (`ext2.dtime`, `hfs.bkup_time`, `ntfs.fn_*`). The `ntfs.fn_*_nano` fields are declared `time_t`, unlike every other `*_nano` field.

### Thread safety

`TSK_FS_INFO` carries locks for its shared caches (`list_inum_named_lock`, `orphan_dir_lock`), and implementations add their own (for example `NTFS_INFO::lock`, `FATFS_INFO::cache_lock` and `dir_lock`, `EXT2FS_INFO::lock`). `tsk/docs/cpp.dox` states that TSK has locks to make it thread safe but that the C interface lets a thread modify shared structures without them. The practical rule that follows from the code: several threads may use one `TSK_FS_INFO` through the API, but a `TSK_FS_FILE`, `TSK_FS_DIR` or `TSK_FS_BLOCK` has no lock and must stay with one thread. `tsk_fs_close` takes no locks, so no other thread may be using the handle. Errors are per thread.

## Virtual files and `$OrphanFiles`

Virtual entries are created by TSK and do not exist on disk. They are identified by type: `TSK_FS_NAME_TYPE_VIRT` / `TSK_FS_META_TYPE_VIRT` for files and `..._VIRT_DIR` for directories.

`$OrphanFiles`:

- Its metadata address is `TSK_FS_ORPHANDIR_INUM(fs)`, which is `fs->last_inum`. Implementations reserve that address by making `inum_count` one larger than the real count.
- It appears as an allocated entry of type `VIRT_DIR` in the root directory. Its metadata has flags `USED | ALLOC`, size 0 (`tsk_fs_dir_make_orphan_dir_name`, `tsk_fs_dir_make_orphan_dir_meta`).
- Its content is built by `tsk_fs_dir_find_orphans` on first access: a recursive walk of all unallocated names from the root records every unallocated metadata address that has a name (`fs->list_inum_named`), then a walk of all `UNALLOC | USED` metadata adds every record that is not in that list. Orphan directories are descended so that their children are not also listed at the top level.
- The first access therefore costs a full directory walk plus a full metadata walk. The result is cached in `fs->orphan_dir` for the life of the handle. A complete recursive `tsk_fs_dir_walk` from the root fills `list_inum_named` as a side effect, which saves the first of the two walks.
- Entries are named from `meta->name2` (the name stored in the metadata record; set by FAT, NTFS and YAFFS2, and by HFS only for its special files) when present, otherwise `OrphanFile-<address>`. Their name flag is `UNALLOC`, their name type is `UNDEF`, and `meta_seq` is `meta->seq - 1`.
- Pass `TSK_FS_DIR_WALK_FLAG_NOORPHAN` to keep a recursive walk from descending into it. The flag only stops the recursion; the `$OrphanFiles` entry itself is still delivered to the callback.

`TSK_FS_ORPHAN_STR` (`"-ORPHAN_FILE-"`) is a different thing: a path prefix used for FAT and NTFS names whose parent directory is unknown.

FAT adds the virtual files `$MBR`, `$FAT1`, `$FAT2` (names in `tsk_fatfs.h`) so the boot sector and the FATs can be read as files.

## Per-file-system notes

**FAT12/16/32 and exFAT**

- Block addresses are sectors, not clusters, because the area before the first cluster must be addressable. `block_size` is the sector size.
- Metadata addresses are derived from the location of the 32-byte directory entry: `FATFS_SECT_2_INODE` in `tsk_fatfs.h` computes `(sector - firstdatasect) * entries_per_sector + 3`. Address 2 (`FATFS_ROOTINO`) is the root directory. Most addresses in the range do not hold a file.
- The last `numfat + 2` addresses (`FATFS_NUM_VIRT_FILES`) are virtual: `$MBR`, one per FAT, and `$OrphanFiles` at `last_inum`.
- `fatfs_open` looks for a boot sector at sector 0, then at the backup locations, sector 6 (FAT32) and sector 12 (exFAT).
- `tsk_fs_dir_add` skips its duplicate check for FAT.
- Path lookup is case-insensitive (`fatfs_name_cmp`).

**NTFS**

- Metadata addresses are MFT entry numbers. `first_inum` is 0 and the root directory is entry 5. `TSK_FS_INFO_FLAG_HAVE_SEQ` is set.
- Each MFT entry has a sequence number (`meta->seq`). NTFS increments it when the entry is freed, so a name written while the file existed carries `seq - 1` once the file is deleted. `name->meta_seq`, `name->par_seq` and `TSK_FS_DIR::seq` carry the name-side values. `ntfs_inode_lookup` (`ntfs.c`) drops `meta` when the name's sequence does not match, applying the minus-one rule for unallocated entries.
- An alternate data stream (ADS) is an additional `$DATA` attribute with a non-NULL `attr->name`. To enumerate streams, iterate attributes and select `type == TSK_FS_ATTR_TYPE_NTFS_DATA`. To read one, use `tsk_fs_attr_read` or `tsk_fs_file_read_type` with its id.
- `tsk_fs_path2inum` accepts `name:stream` in a path component and then requires an attribute with that name to exist. The result is still only a metadata address: `tsk_fs_file_open("/dir/file:stream")` followed by `tsk_fs_file_read` reads the default unnamed `$DATA`, not the named stream.
- `meta->name2` lists the `$FILE_NAME` attributes with parent address and sequence, and `meta->time2.ntfs` holds the `$FILE_NAME` times.
- Path lookup is case-insensitive (`ntfs_name_cmp`). Owner SIDs and the USN journal are NTFS-only features.

**APFS**: needs a pool image (see "Pools and APFS"). No journal functions. `fscheck` reports unsupported. `name->date_added` is APFS-only. Case sensitivity of path lookup follows the volume. Snapshots are selected with `tsk_apfs_set_snapshot` from `apfs_fs.h`.

**HFS**: only HFS+ and HFSX are supported, including one embedded in an HFS wrapper (`hfs_open` reopens at the embedded offset). A plain legacy HFS volume is rejected with `TSK_ERR_FS_MAGIC`, although the type name `hfsl` and `TSK_FS_TYPE_HFS_LEGACY` exist. Case sensitivity follows the volume.

**Logical directory** (`logical_fs.cpp`): presents a host directory as a file system. It exists only on images of type `TSK_IMG_TYPE_LOGICAL`. Metadata addresses encode a directory number in the high 32 bits and a file number in the low 32 bits (`tsk_logical_fs.h`; root is `LOGICAL_ROOT_INUM`, `0x100000000`). Content reads bypass the image layer through `logicalfs_read`.

**Raw and swap**: one flat block range with no files. Most operations are `tsk_fs_nofs_*` stubs that fail with an "unsupported" error. `fscheck` is NULL.

## Adding or changing a file system implementation

Use `ext2fs.c` (simplest complete Unix-style one) as the template; `yaffs.cpp` shows the same pattern compiled as C++. Steps in order:

1. **Info struct.** In a new `tsk_xxx.h`, define `XXX_INFO` with `TSK_FS_INFO fs_info;` as its first member.
2. **Type.** In `tsk_fs.h`, add an unused bit to `TSK_FS_TYPE_ENUM` as `TSK_FS_TYPE_XXX`, a `TSK_FS_TYPE_XXX_DETECT` value, and a `TSK_FS_TYPE_ISXXX` macro. Add the name to `fs_type_table` in `fs_types.c`. (A comment there asks for the Java `TskData.java` to be kept in sync; Java is out of scope in this fork.)
3. **Opener.** Declare `TSK_FS_INFO *xxx_open(TSK_IMG_INFO *, TSK_OFF_T, TSK_FS_TYPE_ENUM, const char *, uint8_t)` in `tsk_fs_i.h` next to the others. The last two arguments are the password and the `test` (probing) flag. In it:
   - Allocate with `tsk_fs_malloc(sizeof(XXX_INFO))`. It zeroes the struct and initialises both generic locks.
   - On every failure path release with `tsk_fs_free`, and return NULL with an error set. Fail fast and cheaply on a bad magic value, because autodetection calls every opener on every volume.
   - Set `tag = TSK_FS_INFO_TAG`, `img_info`, `offset`, `ftype` (a specific type, never `_DETECT`), `duname`, `flags`, `endian`, `block_size`, `dev_bsize`, `block_count`, `first_block`, `last_block`, `last_block_act` (clip to the image size), `first_inum`, `root_inum`, `last_inum`, `inum_count`, `journ_inum`.
   - Reserve `last_inum` for `$OrphanFiles`: make it one past the last real metadata address.
4. **Vtable.** Assign every pointer the generic code calls without a NULL check: `block_walk`, `block_getflags`, `inode_walk`, `file_add_meta`, `get_default_attr_type`, `load_attrs`, `istat`, `fsstat`, `name_cmp`, `close`. Assign `dir_open_meta` as well; `tsk_fs_dir_open_meta` checks it, but without it no directory can be opened. Assign `fscheck` too (existing implementations other than raw and swap do). `jopen`, `jblk_walk`, `jentry_walk` may stay NULL if there is no journal (tools test `jopen`). `fread_owner_sid` and `decrypt_block` are optional and are NULL-checked. `close` must free implementation data and finish with `tsk_fs_free`.
5. **`file_add_meta(fs, fs_file, addr)`.** Allocate or reuse `fs_file->meta`, fill it, return 1 on error. For `addr == TSK_FS_ORPHANDIR_INUM(fs)` fill it with `tsk_fs_dir_make_orphan_dir_meta` instead.
6. **`load_attrs(fs_file)`.** Build `meta->attr` with `tsk_fs_attrlist_getnew`, then `tsk_fs_attr_set_run` (non-resident) or `tsk_fs_attr_set_str` (resident), and set `meta->attr_state = TSK_FS_META_ATTR_STUDIED` (or `_ERROR`). Runs must be in file-offset order. For block-pointer file systems, `tsk_fs_unix_make_data_run` may fit. For compressed content set `TSK_FS_ATTR_COMP` and provide `attr->r` and `attr->w`; `tsk_fs_attr_read` and `tsk_fs_attr_walk` dispatch to them and fail if they are NULL.
7. **`dir_open_meta(fs, &fs_dir, addr, recursion_depth)`.** Follow `ntfs_dir_open_meta` or `ext2fs_dir_open_meta`:
   - If `*a_fs_dir` is non-NULL call `tsk_fs_dir_reset` and set `addr`; otherwise allocate with `tsk_fs_dir_alloc`.
   - If `addr == TSK_FS_ORPHANDIR_INUM(fs)`, return `tsk_fs_dir_find_orphans(fs, fs_dir)`.
   - Set `fs_dir->fs_file = tsk_fs_file_open_meta(fs, NULL, addr)`.
   - Add each entry with `tsk_fs_dir_add`. It copies the name, fills `par_addr`/`par_seq`, drops a duplicate `(meta_addr, name)` pair (keeping an allocated one over an unallocated one), and fails with `TSK_ERR_FS_LARGE_DIR_ERROR` beyond `MAX_DIR_SIZE_TO_PROCESS` (1,000,000) entries.
   - When listing the root directory, add the `$OrphanFiles` entry made by `tsk_fs_dir_make_orphan_dir_name`.
   - Return `TSK_OK`. Any other value, including `TSK_COR`, makes `tsk_fs_dir_open_meta` discard the whole directory and return NULL.
   - If the implementation must walk directories from inside this function, call `tsk_fs_dir_walk_internal` and pass `recursion_depth` on, so the depth limit of 128 applies.
8. **`inode_walk`.** Default missing flag pairs to both values. Loop to `last_inum - 1` and synthesise the `$OrphanFiles` record separately when the range includes `last_inum` and the flags include `ALLOC` and `USED`. For `TSK_FS_META_FLAG_ORPHAN`, call `tsk_fs_dir_load_inum_named(fs)` first and skip addresses for which `tsk_fs_dir_find_inum_named` returns 1. `ntfs_inode_walk` shows all three.
9. **Registration.** In `fs_open.c`, add a row to `FS_OPENERS` (with the `_DETECT` type) and a `TSK_FS_TYPE_ISXXX` branch in the explicit-type chain of `tsk_fs_open_img_decrypt`. Position in `FS_OPENERS` only affects the order of names in a `TSK_ERR_FS_MULTTYPE` message, since every opener always runs.
10. **Build and tests.** The autotools build is recursive in this release: add sources to `libtskfs_la_SOURCES` in `tsk/fs/Makefile.am`, add an installed header to `nobase_include_HEADERS` in the top-level `Makefile.am`, add both to `win32/libtsk/libtsk.vcxproj`, and add CppUnit tests under `unit_tests/` (a new `unit_tests/fs/` directory; `tests/` holds only the legacy API programs). See `build-and-test.md`.

When changing an existing implementation, keep these invariants: `ftype` is specific; `last_inum` is the orphan directory; every failure path of the opener calls `tsk_fs_free`; a new opener must not accept another file system's volumes, or autodetection of that file system starts failing with `TSK_ERR_FS_MULTTYPE`.

## Where the upstream Doxygen docs are wrong or silent

- `tsk/docs/fs.dox`, "Virtual Files": says the virtual status of `$OrphanFiles` shows in `TSK_FS_META::flags`. There is no such flag; it shows in `meta->type` (`TSK_FS_META_TYPE_VIRT_DIR`) and `name->type`.
- `tsk/docs/fs.dox`, "Reading File Content": says the file-walk address is 0 for "sparse, compressed, or non-resident" data. The code passes the real address for non-resident raw blocks and 0 for resident, sparse and compressed data.
- `fs_open.c`, comment on `tsk_fs_open_img_decrypt`: says the password is only used when a type is specified. It is also used during autodetection.
- `tsk_fs.h`: `TSK_FS_TYPE_ISAPFS` is defined twice with identical bodies, and the comment above the first definition says "YAFFS2".
- `fs.dox` does not mention the `_decrypt` open variants, `tsk_fs_dir_get_name`, or the reordering of root directory entries.

## Known defects and traps

Each item was confirmed by reading the named symbol in this repository at release 4.15.0.

1. **`tsk_fs_open_pool`, `tsk_fs_open_pool_decrypt`** (`tsk_fs.h`): declared, never defined. Using them is a link error.
2. **BitLocker and APFS decryption are absent from autotools builds** (`encryptionHelper.cpp`, `fs_io.c`, `#ifdef HAVE_LIBMBEDTLS`; `apfs_compat.cpp`, `apfs_fs.cpp`, `#ifdef HAVE_LIBOPENSSL`): both macros are only defined by the Visual Studio project, and `HAVE_LIBOPENSSL` only in its `Release|x64` configuration.
3. **A BitLocker error overrides a successful detection** (`fs_open.c`, `tsk_fs_open_img_decrypt`, variable `haveErrorToPreserve`): if any opener reported `TSK_ERR_FS_BITLOCKER_ERROR`, the function returns NULL with that error after the loop, even when another opener succeeded, and the successful handle `fs_first` is not closed (leak). Only reachable when BitLocker support is compiled in.
4. **`tsk_fs_block_get` and `tsk_fs_block_get_flag` do not decrypt** (`fs_block.c`): they call `tsk_img_read` directly, so on a BitLocker or encrypted APFS volume the buffer is ciphertext (and BitLocker's relocated sectors are not followed). They also ignore `block_pre_size`/`block_post_size`. `tsk_fs_read_block`, `tsk_fs_read` and the file and attribute readers do decrypt.
5. **`tsk_fs_block_get` crashes on a NULL file system** (`fs_block.c`): it calls `a_fs->block_getflags` before the NULL check in `tsk_fs_block_get_flag`. `tsk_fs_block_get_flag` does not check the result of its internal `tsk_fs_block_alloc`, and when the read fails it returns NULL without freeing a block it allocated itself. `tsk_fs_block_free` has no NULL or tag check.
6. **`TSK_FS_FILE_WALK_FLAG_AONLY` gives the callback an unusable buffer pointer** (`fs_attr.c`, `tsk_fs_attr_walk_nonres`, `tsk_fs_attr_walk_res`): no buffer is allocated; the resident walk passes NULL and the non-resident walk passes `&buf[skip_remain]` with `buf == NULL`, that is NULL or NULL plus `nrd.skiplen`. `a_len` is still non-zero. Never touch `a_buf` in an `AONLY` walk, and do not rely on it being NULL.
7. **`tsk_fs_dir_walk` reorders the root directory** (`fs_dir.c`, `prioritizeDirNames`): when the walked directory is `fs->root_inum`, entries whose names start (case-insensitively) with `Users`, `Documents and Settings`, `home` or `ProgramData` come first, then everything else, then `Windows` and `$Orphan...`, then `pagefile...` and `hiberfil...`. The match is a prefix match, so `homework` is also promoted. `tsk_fs_dir_get` index order is not affected.
8. **`tsk_fs_dir_get` can return a file with `meta == NULL`** (`fs_dir.c`): when the name is unallocated with address 0, when loading fails (the error is cleared), or when `meta->seq != name->meta_seq`. That last comparison is a strict equality without the minus-one rule that `ntfs_inode_lookup` applies; reading the two functions together, a deleted NTFS file can come back without `meta` from `tsk_fs_dir_get` while `tsk_fs_dir_walk` delivers it with `meta`. This consequence is derived from the code and was not tested on an image.
9. **Leaks on allocation failure in `tsk_fs_dir_get`** (`fs_dir.c`): if `tsk_fs_name_alloc` or `tsk_fs_name_copy` fails, the new `TSK_FS_FILE` is not freed.
10. **Leak in `tsk_fs_dir_open`** (`fs_dir.c`): if `tsk_fs_dir_open_meta` fails after the path was resolved, the allocated `fs_name` is not freed.
11. **`tsk_fs_file_open` hides sequence mismatches** (`fs_file.c`): after resolving the path it overwrites `name->meta_seq` with `meta->seq`. `tsk_fs_path2inum` prefers an allocated name but falls back to an unallocated one, so opening the path of a deleted file can return the metadata of whatever now occupies that address. Check `name->flags` and `meta->flags`.
12. **`tsk_fs_dir_add` deduplicates** (`fs_dir.c`): two entries with the same metadata address and name collapse into one, and an allocated entry replaces an unallocated one. A directory listing is therefore not a one-to-one image of the on-disk entries (except on FAT).
13. **Depth-limit path in `tsk_fs_dir_walk_recursive`** (`fs_dir.c`): when `MAX_DEPTH` or `DIR_STRSZ` would be exceeded, the function returns `TSK_WALK_ERROR` for the directory being listed, so its remaining entries are skipped; the address just pushed on `stack_seen` is not popped.
14. **`tsk_fs_meta_walk` with a NULL or invalid handle** (`fs_inode.c`) returns 1 without setting an error.
15. **`tsk_fs_file_attr_getsize` hides errors** (`fs_file.c`): it returns 0 and clears the error when attributes cannot be loaded, so "no attributes" and "corrupt file" look the same.
16. **`fatfs_open` falls back to FAT12/16/32 for an explicit exFAT request** (`fatfs.c`, final `if`): the last operand `fatxxfs_open(fatfs) == 0` is unconditional, so it also runs after `exfatfs_open` failed for `TSK_FS_TYPE_EXFAT`, and a second time after both failed for `TSK_FS_TYPE_FAT_DETECT`.
17. **`tsk_fs_ifind_path` leaks on Windows** (`ifind_lib.c`): the UTF-8 copy of the path (`cpath`) is not freed.
18. **`TskFsInfo` always closes its handle** (`tsk_fs.h`, `~TskFsInfo`): wrapping a `TSK_FS_INFO *` you still own in the C++ class leads to a double close. See `auto-hashdb-cpp.md`.
19. **`tsk_fs_fls`, `tsk_fs_ils`, `tsk_fs_icat`, `tsk_fs_ffind`, `tsk_fs_ifind_data`, `tsk_fs_ifind_par`, `tsk_fs_blk*`** print their results to stdout. They are the CLI tools in library form, not query functions; use the walk and read APIs to get data back.
20. **Leaks in `tsk_fs_path2inum`** (`ifind_lib.c`): when a path component is not a directory it returns -1 without closing the `TSK_FS_DIR` it opened. When one directory has several unallocated names that match, an earlier match with a non-zero address is replaced without being closed, so the last unallocated match wins.
21. **No cycle guard when appending runs** (`fs_attr.c`, `tsk_fs_attr_append_run`): the loops that find the end of the run list and renumber offsets follow `next` until NULL. If an implementation appends a run that is already in the list, which happens on some corrupt images (upstream issue #2268), they never end, and calls such as `tsk_fs_file_attr_getsize` hang.
