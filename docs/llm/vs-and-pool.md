# Volume system and pool layers (`tsk/vs/`, `tsk/pool/`)

A **volume system** (VS) is a partition table: it splits an image into contiguous runs of sectors (partitions, also called volumes). A **pool** is a container whose volumes may use non-contiguous blocks: an APFS container, or an LVM volume group. Both layers sit between the image layer (`TSK_IMG_INFO`, see `base-and-img.md`) and the file system layer (`TSK_FS_INFO`, see `fs.md`), and both are optional: a file system can be opened directly on an image.

Read this file before writing code that enumerates partitions, opens APFS or LVM volumes, or converts a partition or pool volume into a file system handle, and before changing anything under `tsk/vs/` or `tsk/pool/`. Everything here was checked against the source at release 4.13.0.

## Layer stack and where the code lives

```
TSK_IMG_INFO  --tsk_vs_open-->  TSK_VS_INFO  --part_list-->  TSK_VS_PART_INFO
     |                                                            |
     |  tsk_pool_open_img_sing(img, byte_off)   tsk_pool_open_sing(part)
     v                                                            v
              TSK_POOL_INFO  --vol_list-->  TSK_POOL_VOLUME_INFO
                   |  pool->get_img_info(pool, pvol_block)
                   v
              TSK_IMG_INFO (itype == TSK_IMG_TYPE_POOL)  --tsk_fs_open_img*-->  TSK_FS_INFO
```

- Client code includes only `tsk/libtsk.h`. `tsk/vs/tsk_vs.h` says it is not meant to be included directly.
- `tsk/vs/` is plain C. The public API (and the inline C++ wrappers `TskVsInfo` and `TskVsPartInfo`) is in `tsk_vs.h`; internal prototypes are in `tsk_vs_i.h`. `mm_open.c` holds open/close and autodetection, `mm_part.c` the partition list, `mm_io.c` the read functions, `mm_types.c` the type-name table. Each format has one file: `dos.c` (MBR and extended tables), `gpt.c`, `bsd.c`, `sun.c`, `mac.c` (Apple Partition Map).
- `tsk/pool/` is C++17 behind a C API. The C API is in `tsk_pool.h` and implemented in `pool_open.cpp`, `pool_read.cpp`, `pool_types.cpp`. The classes are `TSKPool` (abstract, `tsk_pool.hpp`), `APFSPool` (`tsk_apfs.hpp`, `apfs_pool.cpp`), `LVMPool` (`tsk_lvm.hpp`, `lvm_pool.cpp`), and the template `TSKPoolCompat<T>` (`pool_compat.hpp`) with its subclasses `APFSPoolCompat` and `LVMPoolCompat`, which embed and fill the C struct.
- `tsk/img/pool.hpp` defines `IMG_POOL_INFO`, the image subtype that represents one pool volume. It records `pool_info` and `pvol_block`.

## Units and offsets

Mixing these up is the most common mistake in this area.

| Value | Unit | Relative to |
|---|---|---|
| `tsk_vs_open` `offset` argument, `TSK_VS_INFO.offset` | bytes | start of image |
| `TSK_VS_INFO.block_size` | bytes per sector | may differ from `img_info->sector_size` (see below) |
| `TSK_VS_PART_INFO.start`, `.len` | sectors of `vs->block_size` | `vs->offset`, not the start of the image |
| `tsk_vs_read_block` `a_addr` | sectors | `vs->offset` |
| `tsk_vs_part_read` `a_off` | bytes | start of the partition |
| `tsk_vs_part_read_block` `a_addr` | sectors | start of the partition |
| `tsk_pool_open_img*` `offset`, `TSK_POOL_INFO.img_offset` | bytes | start of image |
| `tsk_pool_read` `a_off` | bytes | start of the pool (`img_offset` is added) |
| `TSK_POOL_INFO.block_size`, `.num_blocks` | APFS: container block size and count. LVM: both are 0. | |
| `TSK_POOL_VOLUME_INFO.block` | APFS: block number of the volume superblock. LVM: logical volume index + 1. | start of the pool (APFS) |
| `tsk_fs_open_img*` `a_offset` | bytes | start of the image that is passed in |

The byte offset of a partition in the image is always:

```c
TSK_OFF_T off = part->vs->offset + (TSK_OFF_T) part->start * part->vs->block_size;
```

`tsk_fs_open_vol`, `tsk_pool_open_sing` and `tsk_pool_open` all compute exactly this. Use `vs->block_size`, never `img->sector_size`, because `tsk_vs_gpt_open` retries with 512, 1024, ... 8192 bytes when the image sector size fails, and `tsk_vs_mac_open` swaps between 512 and 4096.

## Volume system API

Signatures, copied from `tsk/vs/tsk_vs.h`:

```c
typedef TSK_WALK_RET_ENUM(*TSK_VS_PART_WALK_CB) (TSK_VS_INFO * a_vs,
    const TSK_VS_PART_INFO * a_vs_part, void *a_ptr);

TSK_VS_INFO *tsk_vs_open(TSK_IMG_INFO *, TSK_DADDR_T, TSK_VS_TYPE_ENUM);
void tsk_vs_close(TSK_VS_INFO *);
const TSK_VS_PART_INFO *tsk_vs_part_get(const TSK_VS_INFO *, TSK_PNUM_T idx);
uint8_t tsk_vs_part_walk(TSK_VS_INFO * vs, TSK_PNUM_T start, TSK_PNUM_T last,
    TSK_VS_PART_FLAG_ENUM flags, TSK_VS_PART_WALK_CB action, void *ptr);
ssize_t tsk_vs_read_block(TSK_VS_INFO * a_vs, TSK_DADDR_T a_addr, char *buf, size_t len);
ssize_t tsk_vs_part_read(const TSK_VS_PART_INFO * a_vs_part, TSK_OFF_T a_off, char *buf, size_t len);
ssize_t tsk_vs_part_read_block(const TSK_VS_PART_INFO * a_vs_part, TSK_DADDR_T a_addr, char *buf, size_t len);
```

The type helpers are `tsk_vs_type_toid`, `tsk_vs_type_toid_utf8`, `tsk_vs_type_toname`, `tsk_vs_type_todesc`, `tsk_vs_type_supported` and `tsk_vs_type_print`.

### Opening

- `tsk_vs_open(img, offset, type)` returns NULL on error. A NULL image gives `TSK_ERR_IMG_NOFILE`. An image of type `TSK_IMG_TYPE_LOGICAL` gives `TSK_ERR_VS_UNSUPTYPE`. Every format opener fails with `TSK_ERR_VS_ARG` when `img->sector_size` is 0.
- Only DOS, BSD, Sun, Mac and GPT can be opened. `TSK_VS_TYPE_APFS` and `TSK_VS_TYPE_LVM` exist in `TSK_VS_TYPE_ENUM` but `tsk_vs_open` rejects them with `TSK_ERR_VS_UNSUPTYPE`; use the pool API. `TSK_VS_TYPE_DBFILLER` is a placeholder for database code and is not openable either.
- The enum values are bit flags except `TSK_VS_TYPE_LVM` (0x30), which equals `GPT | APFS`. Do not test for LVM with a bit mask.
- `tsk_vs_type_supported()` returns the OR of the five openable types. `tsk_vs_type_toid*` matches the short names `dos`, `mac`, `bsd`, `sun`, `gpt` exactly (case-sensitive), returns `TSK_VS_TYPE_UNSUPP` for anything else, and does not accept NULL.

### Autodetection order and conflict rules

With `TSK_VS_TYPE_DETECT`, `tsk_vs_open` runs every opener in the order DOS, BSD, GPT, Sun, Mac, and discards the errors of failed probes. The rules for more than one match are:

1. DOS is probed in "test" mode (`tsk_vs_dos_open(img, offset, 1)`), which rejects sector 0 when its OEM name field starts with `MSDOS`, `MSWIN` or `NTFS` (or is exactly `FAT`), so that a FAT or NTFS boot sector is not taken for an MBR. Requesting `TSK_VS_TYPE_DOS` explicitly skips this test.
2. BSD replaces DOS without reporting a conflict, because a BSD disk label lives behind a sector that carries the DOS magic.
3. GPT found from its **backup** table (`vs->is_backup == 1`) together with DOS: DOS wins and the GPT result is closed.
4. GPT found from its primary table together with DOS: GPT wins if the DOS table has a partition whose description starts with `GPT Safety` and whose `start` is at most 63 (the protective MBR). Otherwise both are closed and the call fails with `TSK_ERR_VS_MULTTYPE`. GPT together with BSD also fails with `TSK_ERR_VS_MULTTYPE`.
5. Sun or Mac together with anything found earlier fails with `TSK_ERR_VS_MULTTYPE`.
6. If nothing matched, `detectDiskEncryption` (`tsk/util/detect_encryption.h`) inspects the data at `offset`. A known encryption signature gives `TSK_ERR_VS_ENCRYPTED` with the product description as the error string.

A failed autodetect with no encryption signature returns NULL with the TSK error code left at 0; see "Known defects and traps". `TskAuto` (see `auto-hashdb-cpp.md`) treats any NULL other than `TSK_ERR_VS_MULTTYPE` as "no volume system" and tries a file system at the same offset. Callers should do the same.

### The partition list

- `vs->part_list` is a doubly linked list sorted by `start`. `addr` is the 0-based position in that sorted list and is the index used by `tsk_vs_part_get` and `tsk_vs_part_walk`. It is not the slot number in the on-disk table; that is `slot_num`, with `table_num` identifying which table (for example, which DOS extended table).
- Each entry has exactly one kind in `flags` (`TSK_VS_PART_FLAG_ENUM`): `ALLOC` is a real partition; `META` covers sectors that hold partition-table structures (for example the DOS table sector, DOS extended partitions, the GPT header and entry array) and can overlap the other kinds; `UNALLOC` entries are mostly synthesized by `tsk_vs_part_unused`, which every opener calls last and which adds one for every gap between non-`META` entries and for the tail of the image (`mac.c` can also mark a table entry `UNALLOC`). Synthesized entries have `table_num == -1` and `slot_num == -1` and the description `Unallocated`.
- Consequently `part_count` counts all three kinds. Filter with the flags when only real partitions are wanted.
- `desc` is a UTF-8 string owned by the list. For GPT it is derived from the partition type GUID or the entry name; for DOS it is a type name plus the type byte.
- Only the first two entries of a DOS, GPT or BSD table are checked against the image size (search for "first couple" in `dos.c`, `gpt.c`, `bsd.c`). Later entries can start or end beyond the image, so do not assume `start + len` is inside it.

### Walking and reading

- `tsk_vs_part_walk(vs, start, last, flags, cb, ptr)` calls `cb` for entries with `start <= addr <= last` whose flags intersect `flags`. Both bounds are inclusive and must be less than `part_count`, otherwise the call returns 1 with `TSK_ERR_VS_WALK_RNG`. Pass `vs->part_count - 1` as `last` to walk everything. `flags == 0` means all kinds.
- The callback returns `TSK_WALK_CONT`, `TSK_WALK_STOP` (walk returns 0) or `TSK_WALK_ERROR` (walk returns 1). The walk does not set an error code for `TSK_WALK_ERROR`; the callback must.
- `tsk_vs_read_block` and `tsk_vs_part_read_block` require `len` to be a multiple of `vs->block_size` and otherwise return -1 with `TSK_ERR_VS_READ`. All three read functions return the byte count from `tsk_img_read` or -1.
- `tsk_vs_part_get` validates its handle (NULL or bad tag gives `TSK_ERR_VS_ARG`). `tsk_vs_part_walk`, `tsk_vs_close` and the three read functions do not check for NULL or a stale handle.

### Example: walk partitions and open a file system on each

```c
#include <tsk/libtsk.h>

static TSK_WALK_RET_ENUM
part_cb(TSK_VS_INFO *vs, const TSK_VS_PART_INFO *part, void *ptr)
{
    (void) vs; (void) ptr;
    printf("%" PRIuPNUM ": start %" PRIuDADDR " len %" PRIuDADDR " sectors (%s)\n",
        part->addr, part->start, part->len, part->desc);

    /* Opens at byte offset vs->offset + start * vs->block_size. */
    TSK_FS_INFO *fs = tsk_fs_open_vol(part, TSK_FS_TYPE_DETECT);
    if (fs == NULL) {
        tsk_error_reset();          /* no recognizable file system: not fatal */
        return TSK_WALK_CONT;
    }
    printf("  file system: %s\n", tsk_fs_type_toname(fs->ftype));
    tsk_fs_close(fs);
    return TSK_WALK_CONT;
}

int list_partitions(const TSK_TCHAR *path)
{
    TSK_IMG_INFO *img = tsk_img_open_sing(path, TSK_IMG_TYPE_DETECT, 0);
    if (img == NULL) { tsk_error_print(stderr); return 1; }

    TSK_VS_INFO *vs = tsk_vs_open(img, 0, TSK_VS_TYPE_DETECT);
    if (vs == NULL) {
        /* The error code may be 0 here. A real tool would now try
         * tsk_fs_open_img(img, 0, TSK_FS_TYPE_DETECT). */
        tsk_img_close(img);
        return 1;
    }

    uint8_t err = tsk_vs_part_walk(vs, 0, vs->part_count - 1,
        TSK_VS_PART_FLAG_ALLOC, part_cb, NULL);
    if (err) tsk_error_print(stderr);

    tsk_vs_close(vs);   /* invalidates every TSK_VS_PART_INFO */
    tsk_img_close(img); /* image last */
    return err;
}
```

### C++ wrappers

`TskVsInfo` and `TskVsPartInfo` are thin inline wrappers in `tsk_vs.h`. `TskVsInfo::open` returns 1 on error and 0 on success. `TskVsInfo::getPart` and `getImgInfo` return objects allocated with `new` that the caller must `delete`; `getPart` returns a wrapper object even when the lookup failed, so check its contents rather than the pointer. The destructor closes the volume system only if this object opened it.

## Pool API

Signatures, copied from `tsk/pool/tsk_pool.h`:

```c
const TSK_POOL_INFO *tsk_pool_open_sing(const TSK_VS_PART_INFO *part, TSK_POOL_TYPE_ENUM type);
const TSK_POOL_INFO *tsk_pool_open(int num_vols, const TSK_VS_PART_INFO *const parts[], TSK_POOL_TYPE_ENUM type);
const TSK_POOL_INFO *tsk_pool_open_img_sing(TSK_IMG_INFO *img, TSK_OFF_T offset, TSK_POOL_TYPE_ENUM type);
const TSK_POOL_INFO *tsk_pool_open_img(int num_imgs, TSK_IMG_INFO *const imgs[], const TSK_OFF_T offsets[], TSK_POOL_TYPE_ENUM type);
void tsk_pool_close(const TSK_POOL_INFO *);
ssize_t tsk_pool_read(TSK_POOL_INFO *a_fs, TSK_OFF_T a_off, char *a_buf, size_t a_len);
TSK_FS_ATTR_RUN *tsk_pool_unallocated_runs(const TSK_POOL_INFO *);
```

The struct `TSK_POOL_INFO` also carries three function pointers that are part of the working API even though the header marks them `\internal`, because no wrapper functions exist:

```c
void (*close)(const struct _TSK_POOL_INFO *);                       /* use tsk_pool_close */
uint8_t (*poolstat)(const struct _TSK_POOL_INFO *pool, FILE *hFile); /* 0 on success, 1 on error */
TSK_IMG_INFO* (*get_img_info)(const struct _TSK_POOL_INFO *pool, TSK_DADDR_T pvol_block);
```

### Opening

- A pool can start at any byte offset: offset 0 of an image, or the start of a partition. `tsk_pool_open_sing(part, type)` derives the offset from the partition and calls `tsk_pool_open_img_sing`.
- All four open functions return NULL on error. The array variants exist for pools that span several devices, but both `APFSPool` and `LVMPool` throw unless exactly one member is given, so in practice only single-member pools open.
- `TSK_POOL_TYPE_DETECT` tries APFS, then LVM when LVM support is compiled in. APFS is recognised by reading and checksum-validating the container (NX) superblock; the constructor then selects the checkpoint superblock with the highest transaction id, falling back to block 0.
- Error codes: an explicitly requested type that fails to parse gives `TSK_ERR_POOL_UNKTYPE` with the C++ exception text as the error string. A failed `DETECT`, an unknown type, and `TSK_POOL_TYPE_LVM` in a build without libvslvm all give `TSK_ERR_POOL_UNSUPTYPE`. For a failed `DETECT` the error string is just `"0"`; the real reason is printed to stderr only when `tsk_verbose` is set.
- `tsk_pool_open_sing` and `tsk_pool_open` validate the partition's `vs` pointer and tag (`TSK_ERR_POOL_ARG`). `tsk_pool_open_img` validates nothing: `imgs`, `offsets` and `num_imgs` must be correct.

### Pool volumes

`pool->vol_list` is a `next`/`prev` linked list of `num_vols` entries of `TSK_POOL_VOLUME_INFO` (it is NULL when there are no volumes). The fields mean different things per pool type:

| Field | APFS | LVM |
|---|---|---|
| `block` | block number of the volume superblock | logical volume index + 1 |
| `num_blocks` | blocks allocated to the volume | 0 |
| `desc` | volume name | logical volume name |
| `password_hint` | set only when `TSK_POOL_VOLUME_FLAG_ENCRYPTED` is set, otherwise NULL | NULL |
| `flags` | `ENCRYPTED`, `CASE_SENSITIVE` | 0 |

For both types, `vol->block` is the value to pass as `pvol_block` to `get_img_info`. The command-line tools take the same number through their pool-volume option, where 0 means "no pool"; that is why LVM uses index + 1.

### From pool volume to image to file system

There is no direct "open file system on pool" function that works (see the traps section). The real sequence, used by every tool in `tools/fstools/` and by `TskAuto::findFilesInPool`, is:

1. `pool = tsk_pool_open_img_sing(img, pool_byte_offset, type)` or `tsk_pool_open_sing(part, type)`.
2. `pool_img = pool->get_img_info(pool, vol->block)`. This returns a new `TSK_IMG_INFO` with `itype == TSK_IMG_TYPE_POOL`, or NULL on error.
3. `fs = tsk_fs_open_img_decrypt(pool_img, offset, fstype, password)` (or `tsk_fs_open_img`).

What the pool image represents differs by type:

- **APFS.** The pool image is a pass-through to the whole underlying image: `size` is the underlying image size and reads use the same absolute offsets. It exists to carry `pool_info` and `pvol_block` to the APFS file system code. `apfs_open` (`tsk/fs/apfs_open.cpp`) requires a pool image and `TSK_FS_TYPE_APFS`, ignores the offset argument, and takes the volume from `pvol_block`. Conversely, APFS cannot be opened on a non-pool image.
- **LVM.** The pool image is the logical volume itself: `size` is the logical volume size and offset 0 is the first byte of the volume. Open the file system at offset 0 with any type, including `TSK_FS_TYPE_DETECT`.

### Example: open every APFS volume in a container

```c
#include <tsk/libtsk.h>

/* `part` is a partition that holds an APFS container. When the container
 * starts at a known byte offset instead, use tsk_pool_open_img_sing(). */
int walk_apfs_pool(const TSK_VS_PART_INFO *part, const char *password)
{
    const TSK_POOL_INFO *pool = tsk_pool_open_sing(part, TSK_POOL_TYPE_APFS);
    if (pool == NULL) { tsk_error_print(stderr); return 1; }

    for (const TSK_POOL_VOLUME_INFO *vol = pool->vol_list; vol != NULL;
        vol = vol->next) {
        TSK_IMG_INFO *pool_img = pool->get_img_info(pool, vol->block);
        if (pool_img == NULL) { tsk_error_print(stderr); continue; }

        /* The offset argument is ignored for APFS; the type must be APFS. */
        TSK_FS_INFO *fs = tsk_fs_open_img_decrypt(pool_img, 0,
            TSK_FS_TYPE_APFS, password ? password : "");
        if (fs != NULL) {
            printf("volume %d (%s): root inode %" PRIuINUM "\n",
                vol->index, vol->desc, fs->root_inum);
            tsk_fs_close(fs);
        }
        else {
            tsk_error_print(stderr);
        }
        tsk_img_close(pool_img);    /* before tsk_pool_close */
    }

    tsk_pool_close(pool);           /* frees the pool and vol_list */
    return 0;
}
```

For LVM the loop is the same except that the file system is opened with `tsk_fs_open_img(pool_img, 0, TSK_FS_TYPE_DETECT)`. Check `pool->ctype` at run time to choose; client code does not need the `HAVE_LIBVSLVM` macro.

### Reading and unallocated space

- `tsk_pool_read` reads raw bytes from the underlying image at `a_off` plus the pool's image offset. It takes a non-const `TSK_POOL_INFO *` although the open functions return a const pointer, so a cast is needed. For LVM it is the same raw pass-through over the physical volume, not a read through logical volumes.
- `tsk_pool_unallocated_runs` returns a linked list of `TSK_FS_ATTR_RUN` (`addr` and `len` in pool blocks, `offset` cumulative) that the caller frees with `tsk_fs_attr_run_free`. It returns NULL when there are no runs, on allocation failure, and always for LVM, which does not implement it.
- Neither function checks its pool argument.

### Optional dependencies

| Feature | Macro | Notes |
|---|---|---|
| APFS pools | none | always built |
| LVM pools (`LVMPool`, `LVMPoolCompat`) | `HAVE_LIBVSLVM` | needs libvslvm, which in turn needs libbfio; `configure` fails if libvslvm is requested without libbfio |
| `img_bfio_handle.{h,cpp}` (libbfio adapter over `TSK_IMG_INFO`) | `HAVE_LIBBFIO` | used only by LVM |

The macros come from the generated `tsk/tsk_config.h` via `tsk/base/tsk_base_i.h`; the project files under `win32/` do not define them. The sources test the macro with both `#if HAVE_LIBVSLVM` and `#ifdef HAVE_LIBVSLVM`; keep new code consistent with the file being edited. `pool_types.cpp` lists `lvm` as a pool type name whether or not it is built. See `build-and-test.md` for configure options.

## Conventions shared by both layers

- **Return values.** Open and get functions return NULL on error. `tsk_vs_part_walk` and `poolstat` return 1 on error and 0 on success. Read functions return a byte count or -1. The reason is in the per-thread TSK error state (`tsk_error_get_errno`, `tsk_error_get_errstr`, `tsk_error_print`; see `base-and-img.md`). VS codes are `TSK_ERR_VS_*` and pool codes `TSK_ERR_POOL_*` in `tsk/base/tsk_base.h`.
- **Exceptions.** The pool classes signal errors with C++ exceptions. The C entry points in `pool_open.cpp` and `apfs_open` catch `std::runtime_error` only, and `poolstat`/`getImageInfo` catch `std::exception`. When adding code that is reachable from a C entry point, throw `std::runtime_error` (or a subclass) or catch at the boundary.
- **Ownership.** A `TSK_VS_INFO` and a `TSK_POOL_INFO` borrow their `TSK_IMG_INFO`; neither closes it. Partition entries and their `desc` strings belong to the volume system; pool volume entries and their strings belong to the pool. The image returned by `get_img_info` belongs to the caller and is closed with `tsk_img_close`. A `TSK_FS_INFO` never closes the image it was opened on.
- **Close order.** Close in reverse order of opening: file system, then pool image, then pool, then volume system, then image. The pool image and an APFS file system keep pointers into the pool, and the pool reads through the image.
- **Stale pointers.** `tsk_vs_close` frees every `TSK_VS_PART_INFO`, so pointers obtained from `tsk_vs_part_get` or a walk callback must not be used afterwards. A `TSK_POOL_INFO` is embedded in the C++ pool object, so `tsk_pool_close` frees it too. Closing either handle twice is a use-after-free: `tsk_vs_close` checks only for NULL, and the tag check in `tsk_pool_close` reads freed memory.
- **Threads.** A volume system is not modified after `tsk_vs_open` and its reads go through `tsk_img_read`, which takes the image cache lock, so concurrent reads on one handle follow the image layer's rules. `APFSPool` keeps an unsynchronised block cache (`_block_cache` in `tsk_apfs.hpp`, documented there as not thread safe), and an APFS pool image calls the underlying image's `read` function directly rather than `tsk_img_read`. Do not use one pool, or file systems opened on it, from several threads without external locking.

## Modifying the library

- **New volume system format.** Add the opener prototype to `tsk_vs_i.h`, the enum value to `TSK_VS_TYPE_ENUM`, a row to `vs_open_table` in `mm_types.c`, a `case` in the non-detect switch of `tsk_vs_open`, a probe with explicit conflict handling in its detect branch, and the source file to `tsk_vs_libtskvs_la_SOURCES` in `Makefile.am`. An opener must allocate the struct with `tsk_malloc` (which zeroes), set `tag`, `vstype`, `img_info`, `offset`, `block_size` and `close`, add entries with `tsk_vs_part_add` (the `desc` string must be heap-allocated because `tsk_vs_part_free` frees it), and finish with `tsk_vs_part_unused`.
- **New pool type.** Derive from `TSKPool`, wrap it in a `TSKPoolCompat<T>` subclass that fills `vol_list` and implements `poolstat` and `getImageInfo`, then add it to `tsk_pool_open_img`, `pool_type_table` in `pool_types.cpp`, and `TSK_POOL_TYPE_ENUM`. `TskAuto::findFilesInPool` (`tsk/auto/auto.cpp`) and the tools branch on `pool->ctype` and need a matching case.
- `Makefile.am` installs `tsk/pool/tsk_lvm.hpp`, `tsk/pool/lvm_pool_compat.hpp` and `tsk/img/pool.hpp`, but these include `tsk/base/tsk_base_i.h` or `tsk/img/tsk_img_i.h`, which are not in the install list. Treat the pool C++ headers as internal; external code should use the C API.

## Where upstream documentation is wrong or missing

- `tsk/docs/vs.dox` (section `vs_open`) says the C++ wrapper for volume systems is `TskImgInfo::open()`/`close()`. The class is `TskVsInfo`.
- `tsk/docs/vs.dox` (section `vs_open2`) says the partition starting offset is relative to the start of the disk image. It is in sectors relative to `vs->offset`.
- `tsk/docs/pool.dox` is a stub that says "This section is not complete". It does not describe `get_img_info` or the file system hand-off.
- `tsk_pool.h` documents the volume tag as `TSK_POOL_VOLUME_INFO_TAG`; the macro that exists is `TSK_POOL_VOL_INFO_TAG`.
- `LVMPool::read` carries a "TODO implement" comment; what it actually does is the raw pass-through described above.

## Known defects and traps

Each item was confirmed in the source. Line numbers are for 4.13.0 and will drift; search for the symbol.

1. **`tsk_fs_open_pool` and `tsk_fs_open_pool_decrypt` do not exist.** They are declared in `tsk/fs/tsk_fs.h` (about line 1085) but defined nowhere in the C/C++ tree, so calling them fails at link time. Use `get_img_info` plus `tsk_fs_open_img_decrypt`.
2. **`TSK_ERR_POOL_UNSUPTYPE` has the wrong value.** `tsk/base/tsk_base.h` defines it as `(TSK_ERR_IMG | 1)`, which is numerically `TSK_ERR_IMG_OFFSET`, instead of `(TSK_ERR_POOL | 1)`. Comparing against the macro works, but printed messages and any code that classifies errors by the layer mask treat it as an image error.
3. **Tools test the wrong code after a pool open.** `tools/fstools/fsstat.cpp`, `fls.cpp`, `tools/pooltools/pstat.cpp` and others compare against `TSK_ERR_FS_UNSUPTYPE` after `tsk_pool_open_img_sing`, which never matches. Do not copy that check.
4. **Failed VS autodetection usually leaves the error code at 0.** In the "nothing found" branch of `tsk_vs_open` (`mm_open.c`), `TSK_ERR_VS_UNKTYPE` is set only when `detectDiskEncryption` returns NULL, which happens only when its allocation fails. In the normal case it returns a result of type `ENCRYPTION_DETECTED_NONE`, and `tsk_vs_open` returns NULL with no error set. Test the return value, not the error code.
5. **Leaked DOS handle in autodetection.** In `tsk_vs_open`, when both DOS and BSD open, `prev_vs` is overwritten with the BSD handle and the DOS `TSK_VS_INFO` is never closed (the code that closed it is commented out).
6. **`LVMPoolCompat::getImageInfo` casts the wrong pointer** (`lvm_pool_compat.cpp`). It sets `img_pool_info->impl` to the libvslvm logical volume handle and then does `static_cast<LVMPoolCompat*>(img_pool_info->impl)` to reach `_members[0]`, which reads a libvslvm object as a pool object (undefined behaviour). The APFS version correctly casts `pool_info->impl`. The same function leaks the logical volume handle on its early-return paths.
7. **`getImageInfo` leaks its cache on failure.** In both `APFSPoolCompat::getImageInfo` and `LVMPoolCompat::getImageInfo`, when `tsk_img_copy_image_names` fails the struct is released with `tsk_img_free`, which does not delete the `LegacyCache` allocated just before.
8. **`tsk_vs_part_read` has no bounds check against the partition.** It reads at `a_off` from the partition start and is limited only by the image size, so it reads past the end of the partition. Clamp to `part->len * vs->block_size` in the caller. The block variants are also unchecked.
9. **The trailing unallocated entry ignores `vs->offset`.** `tsk_vs_part_unused` (`mm_part.c`) compares partition ends, which are relative to `vs->offset`, with `img_info->size / block_size`, so for a volume system opened at a non-zero offset the last `Unallocated` entry extends past the end of the image by `vs->offset`.
10. **`TskAuto` drops `vs->offset` for pools.** The partition callback in `tsk/auto/auto.cpp` (the one that calls `hasPool` and `findFilesInPool`) passes `start * vs->block_size` without adding `vs->offset`, unlike `tsk_pool_open_sing`. It is only correct for a volume system at offset 0.
11. **`get_desc` helpers return a string literal on allocation failure.** `dos_get_desc`, `bsd_get_desc` and `sun_get_desc` return `""` when `tsk_malloc` fails, and `tsk_vs_part_free` later passes that pointer to `free`.
12. **Unvalidated arguments.** `tsk_pool_open` does not check individual `parts[i]` for NULL; `tsk_pool_open_img`, `tsk_pool_read` and `tsk_pool_unallocated_runs` check nothing. `tsk_vs_type_toid*` and `tsk_pool_type_toid*` dereference a NULL string.
13. **Misnamed pool type helper.** `pool_types.cpp` defines `TSK_POOL_TYPE_ENUM tsk_fs_type_supported()` with C++ linkage and no declaration in any header. There is no usable `tsk_pool_type_supported`. `tsk_pool_type_print` prints the heading "Supported file system types:".
14. **LVM pool metadata is mostly empty.** `LVMPool` sets `block_size` and `num_blocks` to 0 and `LVMPoolCompat::init_volumes` leaves each volume's `num_blocks` at 0. If libvslvm fails part-way through `init_volumes`, `num_vols` is already set but the list is only partly linked, so iterate with `next`, not by index.
15. **A missing check in the tools.** `fsstat.cpp` and its siblings pass the result of `get_img_info` to `tsk_fs_open_img_decrypt` without a NULL check (marked `FIXME` in `fsstat.cpp`). `tsk_fs_open_img_decrypt` rejects a NULL image with `TSK_ERR_FS_ARG`, but new code should check the pool image itself, as the example above does.
