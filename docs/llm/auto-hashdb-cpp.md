# TSK automation layer, hash databases, C++ wrapper classes and samples

This file covers four parts of The Sleuth Kit (TSK) C/C++ library: the `TskAuto` traversal class (`tsk/auto/`), the hash database API (`tsk/hashdb/`), the C++ wrapper classes declared in the public headers (`TskImgInfo`, `TskVsInfo`, `TskFsInfo`, `TskFsFile`, ...), and the programs in `samples/`.
Read it before you write code that walks every file in a disk image, looks up hashes, or uses any `Tsk*` C++ class, and before you change code in `tsk/auto/` or `tsk/hashdb/`.
It records what is expensive to find by reading the source: the control flow, ownership rules, return-value exceptions, wrong upstream documentation, and confirmed defects.
The underlying C APIs are covered in `base-and-img.md`, `vs-and-pool.md` and `fs.md`; building and testing are covered in `build-and-test.md`.
Checked against release 4.15.0. Line numbers are given as hints only. Search for the symbol if they have drifted.

## What a library consumer can use

`tsk/libtsk.h` is the umbrella header. It includes `tsk/hashdb/tsk_hashdb.h` and `tsk/auto/tsk_auto.h` (and the base, img, vs, fs and pool headers). Both headers say they are not meant to be included directly; include `<tsk/libtsk.h>`.

The installed headers are listed in `nobase_include_HEADERS` in the top-level `Makefile.am`. For the topics in this file the situation is:

| Header | Installed | Contents |
|---|---|---|
| `tsk/auto/tsk_auto.h` | yes | `TskAuto`, `TSK_FILTER_ENUM` (C++ only) |
| `tsk/hashdb/tsk_hashdb.h` | yes | `tsk_hdb_*` C functions, `TSK_HDB_INFO`, `TskHdbInfo` |
| `tsk/auto/tsk_is_image_supported.h` | yes, but it includes `tsk/tsk_tools_i.h`, which is not installed (it is only in `EXTRA_DIST` of `tsk/Makefile.am`) | `TskIsImageSupported` |
| `tsk/auto/guid.h` | yes | GUID helper class |
| `tsk/auto/tsk_case_db.h`, `tsk_db.h`, `tsk_db_sqlite.h` | no | `TskAutoDb`, `TskCaseDb`, `TskDb`, `TskDbSqlite` |
| `tsk/hashdb/tsk_hash_info.h`, `tsk_hashdb_i.h`, `tsk/auto/tsk_auto_i.h` | no | `TskHashInfo` struct and internal declarations |

Consequences:

- From installed headers, a consumer can use `TskAuto` (by subclassing it) and the `tsk_hdb_*` C functions. `TskIsImageSupported` is only usable when compiling inside the source tree.
- `TskAutoDb`, `TskCaseDb`, `TskDb` and `TskDbSqlite` write an SQLite "case database". They exist mainly to serve the Java bindings/Autopsy and the in-tree `tsk_loaddb` tool, and they are out of scope here. `tsk/auto/tsk_db.h` states that the schema in this code is frozen and no longer matches the Java side.
- SQLite is always part of the link. `tsk/auto/db_sqlite.cpp` and `tsk/hashdb/sqlite_hdb.cpp` are compiled into the single `libtsk` library (through the convenience libraries `libtskauto.la` and `libtskhashdb.la` that `tsk/Makefile.am` merges), and when no system sqlite3 is found the bundled `tsk/auto/sqlite3.c` is compiled in (`tsk/auto/Makefile.am`, the `!HAVE_LIBSQLITE3` block). There is no build of libtsk without SQLite.

## TskAuto: mental model

`TskAuto` (`tsk/auto/tsk_auto.h`, implemented in `tsk/auto/auto.cpp`) walks an image from the top down and calls a virtual method for every file it finds. You use it by subclassing. The only pure virtual method is:

```cpp
virtual TSK_RETVAL_ENUM processFile(TSK_FS_FILE * fs_file, const char *path) = 0;
```

`path` is the path of the parent directory (each level is appended as `name/`), not the path of the file itself. The class is not copyable (private, undefined copy constructor and assignment).

The usual sequence is: construct the subclass, call one `openImage*` method, optionally set filter flags or a password, call one `findFilesIn*` method, read `getErrorList()`, then call `closeImage()` or destroy the object.

### Opening and closing

| Method | Wraps | Ownership of the image |
|---|---|---|
| `openImage(int, const TSK_TCHAR * const images[], TSK_IMG_TYPE_ENUM, unsigned int a_ssize)` | `tsk_img_open` | `TskAuto` owns it and closes it. |
| `openImageUtf8(int, const char *const images[], TSK_IMG_TYPE_ENUM, unsigned int a_ssize)` | `tsk_img_open_utf8` | `TskAuto` owns it and closes it. |
| `openImageHandle(TSK_IMG_INFO *)` | nothing | Borrowed. `TskAuto` never closes it. Always returns 0. |

All three are virtual, clear the error list, and close any image that is already open. `openImage` and `openImageUtf8` return 1 on failure, and the failure is not added to the error list: read it immediately with `tsk_error_get()` or `tsk_error_print()`.

`closeImage()` closes every pool that the traversal kept open (see below) and closes the image only if `TskAuto` opened it. The destructor calls `closeImage()`.

### Traversal flow

The entry points are `findFilesInImg()`, `findFilesInVs(start[, vtype])`, `findFilesInPool(start[, ptype])` and the `findFilesInFs(...)` overloads. `start` is always a byte offset into the image. A "volume system" is a partition table; a "pool" is a container such as APFS or LVM that holds several volumes (see `vs-and-pool.md`).

```
findFilesInImg()
  image type is TSK_IMG_TYPE_LOGICAL  -> findFilesInFs(0, TSK_FS_TYPE_LOGICAL)
  otherwise                           -> findFilesInVs(0)

findFilesInVs(start, vtype)
  tsk_vs_open() fails:
      TSK_ERR_VS_ENCRYPTED -> registerError(), then continue below
      TSK_ERR_VS_MULTTYPE  -> registerError(), return 1
      any other error is discarded
      hasPool(start) ? findFilesInPool(start) : findFilesInFs(start)
  tsk_vs_open() succeeds:
      filterVs(vs)                         STOP or SKIP -> return (vs is not closed)
      tsk_vs_part_walk(all partitions, volume filter flags) -> for each partition:
          filterVol(part)                  SKIP -> next partition, STOP -> end walk
          offset = part->start * vs->block_size
          hasPool(offset) ? findFilesInPool(offset)
                          : findFilesInFsRet(offset, TSK_FS_TYPE_DETECT)

findFilesInPool(start, ptype)
  filterPool(pool)                         SKIP -> return TSK_OK, STOP -> return TSK_STOP
                                           (either way the pool is leaked)
  APFS: for each pool volume:
      filterPoolVol(vol)                   SKIP -> next volume, STOP -> return TSK_STOP
      open the volume as an image, apfs_open(), then walk as a file system
      apfs_open() fails -> registerError(), continue with the next volume
  any other pool type, including LVM: registerError(TSK_ERR_POOL_UNSUPTYPE), return TSK_ERR
  on success the pool stays open in m_poolInfos until closeImage()

findFilesInFs / findFilesInFsRet (start, ftype)
  reuse a file system from setExternalFileSystemList() whose offset == start,
  otherwise tsk_fs_open_img_decrypt(img, start, ftype, password)
  then (private findFilesInFsInt):
      filterFs(fs)                         SKIP -> return, STOP -> return TSK_STOP
      tsk_fs_dir_walk(start inode, RECURSE | file filter flags) -> processFile() per entry
```

Callback order for one image is therefore: `filterVs` once, then for each partition `filterVol`, then (`filterPool`, `filterPoolVol` per pool volume) or nothing, then `filterFs`, then `processFile` for each directory entry in directory-walk order. On an image with no volume system, `filterVs` and `filterVol` are never called.

`hasPool(start)` probes by opening and immediately closing a pool at that offset.

### Filter callbacks and their return values

All filters are virtual and return `TSK_FILTER_ENUM`: `TSK_FILTER_CONT` (process the object), `TSK_FILTER_SKIP` (skip this object, continue with the next), `TSK_FILTER_STOP` (stop).

| Filter | Argument | Default |
|---|---|---|
| `filterVs` | `const TSK_VS_INFO *` | CONT |
| `filterVol` | `const TSK_VS_PART_INFO *` | CONT |
| `filterPool` | `const TSK_POOL_INFO *` | **SKIP** |
| `filterPoolVol` | `const TSK_POOL_VOLUME_INFO *` | **SKIP** |
| `filterFs` | `TSK_FS_INFO *` | CONT |

Rules that are not obvious from the header:

- Pools are skipped by default. To traverse APFS you must override both `filterPool` and `filterPoolVol` to return `TSK_FILTER_CONT`. `TskIsImageSupported` in `tsk/auto/is_image_supported.cpp` does exactly this. LVM cannot be traversed by `TskAuto` in 4.15.0: `tsk_pool_open_img_sing` can open an LVM pool when built with `HAVE_LIBVSLVM`, but `findFilesInPool` has no LVM branch and registers `TSK_ERR_POOL_UNSUPTYPE` (see "Known defects and traps").
- A filter returning STOP ends the enclosing walk but does not set the global stop flag. To stop everything, also call `setStopProcessing()`.
- `filterFs` returning SKIP is the way to learn that a file system exists without walking its files.

### Filter flags

- `setVolFilterFlags(TSK_VS_PART_FLAG_ENUM)` is passed to `tsk_vs_part_walk`. The default set in the constructor is `TSK_VS_PART_FLAG_ALLOC | TSK_VS_PART_FLAG_UNALLOC`, so unallocated partitions are probed for file systems too. The Doxygen comment on `setVolFilterFlags` in `auto.cpp` ("Allocated Non-Meta volumes only") is wrong.
- `setFileFilterFlags(TSK_FS_DIR_WALK_FLAG_ENUM)` is passed to `tsk_fs_dir_walk`. `TSK_FS_DIR_WALK_FLAG_RECURSE` is always OR'd in. The default is `RECURSE` alone, and `tsk_fs_dir_walk` turns on both `ALLOC` and `UNALLOC` when neither is set, so by default allocated and deleted names are both reported. See `fs.md` for the other flags (for example `NOORPHAN`).

### processFile and attributes

`processFile` returns `TSK_OK` to continue or `TSK_STOP` to stop. Any other outcome should be reported with `registerError()` and followed by `TSK_OK` or `TSK_STOP`.

- `TSK_STOP` ends only the directory walk of the current file system. The traversal then continues with the next partition unless the stop flag is set.
- `fs_file` and its `fs_info` belong to the walk. File systems that `TskAuto` opened itself are closed when the `findFilesIn*` call that opened them returns, so do not keep `TSK_FS_FILE *` or `TSK_FS_INFO *` pointers after `processFile` returns.
- `fs_file->meta` and `fs_file->name` can each be NULL; check before use (see `fs.md`).

`processAttributes(fs_file, path)` (protected) iterates the file's attributes and calls the virtual `processAttribute(fs_file, fs_attr, path)` for each. It is never called automatically; call it from your `processFile` if you want per-attribute callbacks.

Protected helper predicates for use inside `processFile`, all returning 1 or 0: `isDir`, `isFile`, `isDotDir` (the name is `.` or `..`), `isNtfsSystemFiles` (NTFS, name starts with `$`, metadata address below 20), `isFATSystemFiles` (the virtual `$MBR`, `$FAT1`, `$FAT2` entries), `isDefaultType(fs_file, fs_attr)` and `isNonResident(fs_attr)`. `isDefaultType` dereferences `fs_attr` without a NULL check.

### Stopping

`setStopProcessing()` is protected and sets `m_stopAllProcessing`. The flag is checked after every `processFile`, between partitions, between pool volumes and in `processAttributes`. `getStopProcessing()` is public.

- Code outside the class cannot request a stop directly. Expose a public method on your subclass that calls `setStopProcessing()`.
- Nothing in `auto.cpp` ever clears the flag, including `openImage*`. A stopped instance cannot be reused unless the subclass resets the protected member `m_stopAllProcessing` itself.

### Error recording

`TskAuto` does not stop on errors. It records them and continues.

- `registerError()` copies the current thread's TSK error (code, `errstr`, `errstr2`) into an internal list, calls the virtual `handleError()`, resets the TSK error, and returns what `handleError()` returned.
- `handleError()` returns 0 by default. The header says a return of 1 means "stop", but no caller inside `auto.cpp` checks the value returned by `registerError()`. To stop on an error, call `setStopProcessing()` from `handleError()`.
- `getErrorList()` returns `const std::vector<error_record>` by value (a copy on every call), where `error_record` is `{ int code; std::string msg1; std::string msg2; }`. `resetErrorList()` clears it.
- `static std::string errorRecordToString(error_record &)` formats a record. The parameter is a non-const reference, so it cannot take the `const` elements of the vector returned by `getErrorList()`; iterate by value (as in the example below) or copy the record first. It overwrites and then resets the calling thread's TSK error state.
- A file system that fails to open inside an unallocated partition is ignored silently. Inside an allocated partition, or with no volume system, the failure is registered with `errstr2` set to "Sector offset: N" (plus the partition description when known).

Return values of the `uint8_t findFilesIn*` methods are derived from the error list: they return 1 if the list is non-empty, not if this particular call failed. Errors accumulate until `openImage*`, `openImageHandle` or `resetErrorList()` is called, so a second `findFilesIn*` call on the same open image reports 1 if the first one registered anything. `findFilesInFsRet` returns `TSK_RETVAL_ENUM` (`TSK_OK`, `TSK_ERR`, `TSK_STOP`) and follows the same rule for `TSK_ERR`. `findFilesInPool` is an exception; see "Known defects and traps".

### Passwords and encryption

`setFileSystemPassword(std::string)` (by value; `getFileSystemPassword()` returns it) stores a password that is passed to `tsk_fs_open_img_decrypt` for every file system that `findFilesInFs*` opens (see `fs.md` for which file systems use it). The pool path does not use it: `findFilesInPool` opens APFS volumes with an empty password. An APFS volume that fails to open is registered as `TSK_ERR_FS_ENCRYPTED` (with `errstr2` "Block: N") if the volume has `TSK_POOL_VOLUME_FLAG_ENCRYPTED`, otherwise with the open error, and the loop continues with the next volume of that pool.

### Other members

- `setExternalFileSystemList(const std::list<TSK_FS_INFO *>&)` supplies file systems that the caller already opened. `TskAuto` reuses one when its `offset` equals the offset being opened and never closes them.
- `findFilesInFs(TSK_FS_INFO *)` and `findFilesInFs(TSK_FS_INFO *, TSK_INUM_T)` walk a caller-owned file system, the second from a given directory inode.
- `getImageSize()` returns the open image's size in bytes, or -1 if no image is open.
- `getCurVsPartDescr()` (returns `std::string` by value), `getCurVsPartFlag()` and `isCurVsValid()` describe the partition most recently visited. `isCurVsValid()` becomes true at the first partition and is never set back to false; `getCurVsPartFlag()` returns an uninitialized value before that.
- `enableImageWriter(const char *)` only has an effect on Windows. Elsewhere it leaves the writer disabled and still returns `TSK_OK`.
- `m_tag` is a public sanity tag (`TSK_AUTO_TAG`) checked by the static walk callbacks. Do not modify it.

### Example subclass

This compiles against the 4.15.0 headers (checked with `g++ -std=c++17 -fsyntax-only`).

```cpp
#include <tsk/libtsk.h>
#include <cstdio>

class FileLister : public TskAuto {
  public:
    explicit FileLister(unsigned long limit) : m_limit(limit), m_count(0) {}

    // Called once per partition; skip everything that is not an allocated partition.
    TSK_FILTER_ENUM filterVol(const TSK_VS_PART_INFO *part) override {
        return (part->flags & TSK_VS_PART_FLAG_ALLOC) ? TSK_FILTER_CONT : TSK_FILTER_SKIP;
    }

    // Called once per directory entry. fs_file is only valid during this call.
    TSK_RETVAL_ENUM processFile(TSK_FS_FILE *fs_file, const char *path) override {
        if (isDotDir(fs_file) || !isFile(fs_file))
            return TSK_OK;
        printf("%s%s\n", path, fs_file->name->name);
        if (++m_count >= m_limit) {
            setStopProcessing();        // stop the whole image, not only this file system
            return TSK_STOP;
        }
        return TSK_OK;
    }

  private:
    unsigned long m_limit, m_count;
};

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FileLister lister(1000);
    const char *images[] = { argv[1] };
    if (lister.openImageUtf8(1, images, TSK_IMG_TYPE_DETECT, 0)) {
        tsk_error_print(stderr);        // open errors are not put in the error list
        return 1;
    }
    lister.setFileFilterFlags(TSK_FS_DIR_WALK_FLAG_ALLOC);   // allocated names only
    uint8_t rc = lister.findFilesInImg();                    // 1 if any error was registered
    for (TskAuto::error_record e : lister.getErrorList())   // by value: errorRecordToString takes a non-const ref
        fprintf(stderr, "%s\n", TskAuto::errorRecordToString(e).c_str());
    lister.closeImage();
    return rc;
}
```

Larger real subclasses are in `tools/autotools/` (`TskRecover` in `tsk_recover.cpp`, `TskGetTimes` in `tsk_gettimes.cpp`, `TskCompareDir` in `tsk_comparedir.h`) and `tools/logicalimager/TskFindFiles.h`. There is no `TskAuto` example in `samples/`.

### TskIsImageSupported

`TskIsImageSupported` (`tsk/auto/is_image_supported.cpp`) is a `TskAuto` subclass that answers "does this image contain anything TSK can read, and is it encrypted?". Its `filterFs` records that a file system was found and returns SKIP, its pool filters return CONT, and its `handleError` classifies encryption (including BitLocker), unsupported-type and multiple-type error codes. The call sequence is `openImage`, `findFilesInImg`, then `isImageSupported()` / `isImageEncrypted()`, `getMessageForIsImageSupportedNat()` or `printResults()`. Its only in-tree consumer is `tools/autotools/tsk_imageinfo.cpp`. It is the best in-tree model for a subclass that inspects structure without reading files.

## Hash databases

A hash database is a set of known file hashes that can be queried by hash value. The API is C, declared in `tsk/hashdb/tsk_hashdb.h`, and dispatched through function pointers stored in `TSK_HDB_INFO`. `tsk/hashdb/tsk_hashdb.c` holds the public functions; each calls the pointer of the same role.

### Database types

`tsk_hdb_open` detects the type from file content (`hdb_determine_db_type`). If more than one text-format test matches, the open fails with `TSK_ERR_HDB_UNKTYPE`.

| `db_type` | Format | Source file | Lookup support | Index |
|---|---|---|---|---|
| `TSK_HDB_DBTYPE_NSRL_ID` | NIST NSRL text | `nsrl.c` | MD5 or SHA-1 | external, type string `nsrl-md5` or `nsrl-sha1` |
| `TSK_HDB_DBTYPE_MD5SUM_ID` | `md5sum` output | `md5sum.c` | MD5 | external, `md5sum` |
| `TSK_HDB_DBTYPE_HK_ID` | HashKeeper | `hashkeeper.c` | MD5 | external, `hk` |
| `TSK_HDB_DBTYPE_ENCASE_ID` | EnCase | `encase.c` | MD5 | external, `encase` |
| `TSK_HDB_DBTYPE_IDXONLY_ID` | an index file with no database | `idxonly.c` | the index's hash type | cannot be rebuilt |
| `TSK_HDB_DBTYPE_SQLITE_ID` | TSK SQLite (`.kdb`) | `sqlite_hdb.cpp` | MD5 only | none needed |

The four text formats and the index-only type share one implementation, `binsrch_index.cpp` (struct `TSK_HDB_BINSRCH_INFO`). `hdb_base.c` supplies default function pointers that every type starts from. In 4.15.0 every file in `tsk/hashdb/` except `binsrch_index.cpp` and `sqlite_hdb.cpp` is compiled as C (`tsk/hashdb/Makefile.am`).

### Index files (text formats)

Text databases cannot be searched directly. A lookup needs an external index: a sorted text file of hash and database offset, searched by binary search.

- The index is named `<db path>-md5.idx` or `<db path>-sha1.idx`. A secondary file `<db path>-md5.idx2` (or `-sha1.idx2`) maps the first three hex digits to offsets and is loaded when present.
- `tsk_hdb_make_index(hdb, type)` builds it. `type` is one of the strings in the table above and must agree with the detected database type; only `nsrl-sha1` produces a SHA-1 index (`hdb_binsrch_idx_initialize`). Building writes `<db path>-<hash>-ns.idx` and sorts it by running the external `sort` program (`/usr/local/bin/sort`, `/usr/bin/sort`, `/bin/sort`, or `C:\WINDOWS\System32\sort.exe`) through `system()` / `CreateProcess` (`hdb_binsrch_idx_finalize`). It fails with "Cannot find sort executable" if none exists.
- One handle holds one open index. Once an index is open, `hdb_binsrch_open_idx` returns success for any requested hash type, and a lookup with a hash of the other length fails with "Hash passed is different size than expected". To query MD5 and SHA-1 of the same NSRL file, open two handles.

### How tsk_hdb_open interprets its path

```c
TSK_HDB_INFO *tsk_hdb_open(TSK_TCHAR *file_path, TSK_HDB_OPEN_ENUM flags);
```

- If `file_path` ends in `-md5.idx` or `-sha1.idx`, it is taken as an index path and the database path is the same string without that suffix.
- With `TSK_HDB_OPEN_NONE`: the database file is opened and its type detected. If the database file does not exist and the path was an index path, the handle is opened index-only. If it does not exist and the path was not an index path, the open fails with `TSK_ERR_HDB_OPEN`.
- With `TSK_HDB_OPEN_IDXONLY`: the handle is always index-only. `idxonly_open` requires the `-md5.idx` or `-sha1.idx` suffix and requires the index header to contain the database-name line; otherwise the open fails.
- An index-only handle answers yes/no. Its lookup callback receives `name == NULL`, and `tsk_hdb_get_db_path` returns NULL for it.

`tsk_hdb_create(TSK_TCHAR *)` creates a new, empty SQLite database. The path must end in `.kdb`.

### Lookups

```c
int8_t tsk_hdb_lookup_str(TSK_HDB_INFO *, const char *hash, TSK_HDB_FLAG_ENUM, TSK_HDB_LOOKUP_FN, void *);
int8_t tsk_hdb_lookup_raw(TSK_HDB_INFO *, uint8_t *hash, uint8_t len, TSK_HDB_FLAG_ENUM, TSK_HDB_LOOKUP_FN, void *);
typedef TSK_WALK_RET_ENUM (*TSK_HDB_LOOKUP_FN)(TSK_HDB_INFO *, const char *hash, const char *name, void *);
```

(Parameter names are added here for clarity; the header prototypes omit them.)

- Both return -1 on error, 0 if the hash is not present and 1 if it is.
- `lookup_str` takes a hex string; text databases accept either letter case. `lookup_raw` takes binary bytes with `len` in bytes (16 for MD5, 20 for SHA-1).
- With `TSK_HDB_FLAG_QUICK` the callback is not called and may be NULL. Without it, the callback is called once per matching entry (once per file name in SQLite, or once with `name == NULL` if the entry has no names). `TSK_HDB_FLAG_EXT` is unused.
- Text databases open their index lazily on the first lookup, so an explicit `tsk_hdb_open_idx` is optional. A lookup on a text database with no index returns -1.
- `tsk_hdb_lookup_verbose_str(hdb, hash, void *result)` expects `result` to point to a `TskHashInfo`, a C++ struct declared in the non-installed `tsk/hashdb/tsk_hash_info.h`. For text databases it only performs a quick lookup and fills in the hash field. It is usable only from inside the source tree.

Index queries: `tsk_hdb_has_idx(hdb, htype)` returns 1 if an index of that type can be opened and 0 if not. It works by opening the index, so it has that side effect, and it is subject to the one-index-per-handle rule above. `tsk_hdb_open_idx(hdb, htype)` returns 0 on success and 1 on error. For SQLite both always report that an index exists.

### Example lookup

This compiles against the 4.15.0 headers (checked with `g++ -std=c++17 -fsyntax-only`).

```cpp
#include <tsk/libtsk.h>
#include <cstdio>

static TSK_WALK_RET_ENUM on_hit(TSK_HDB_INFO *, const char *hash, const char *name, void *) {
    printf("%s  %s\n", hash, name ? name : "(no name)");   // name is NULL for index-only DBs
    return TSK_WALK_CONT;
}

int lookup(TSK_TCHAR *db_path, const char *md5_hex) {
    TSK_HDB_INFO *hdb = tsk_hdb_open(db_path, TSK_HDB_OPEN_NONE);
    if (hdb == NULL) { tsk_error_print(stderr); return -1; }

    if (!tsk_hdb_has_idx(hdb, TSK_HDB_HTYPE_MD5_ID)) {      // always 1 for SQLite databases
        fprintf(stderr, "no MD5 index; build one with tsk_hdb_make_index()\n");
        tsk_hdb_close(hdb);
        return -1;
    }
    int8_t r = tsk_hdb_lookup_str(hdb, md5_hex, TSK_HDB_FLAG_QUICK, NULL, NULL);
    if (r == 1)                                             // again, with details
        r = tsk_hdb_lookup_str(hdb, md5_hex, (TSK_HDB_FLAG_ENUM) 0, on_hit, NULL);
    if (r == -1) tsk_error_print(stderr);
    tsk_hdb_close(hdb);
    return r;                                               // -1 error, 0 not found, 1 found
}
```

### Updates and transactions

Only SQLite databases accept updates (`tsk_hdb_accepts_updates` returns 1 for SQLite and 0 for the others). `tsk_hdb_add_entry(hdb, filename, md5, sha1, sha256, comment)` returns 1 on error. The SQLite implementation stores only the MD5, ignores `sha1` and `sha256`, and calls `strlen(md5)` without a NULL check, so `md5` is required even though the Doxygen comment says it can be NULL. `tsk_hdb_begin_transaction` and `tsk_hdb_commit_transaction` return 1 on error. Do not rely on `tsk_hdb_rollback_transaction`; see "Known defects and traps".

### Ownership and thread safety

- The caller owns the `TSK_HDB_INFO *` returned by `tsk_hdb_open` and releases it with `tsk_hdb_close`. Strings returned by `tsk_hdb_get_db_path`, `tsk_hdb_get_display_name` and `tsk_hdb_get_idx_path` point into the handle.
- Each handle has a lock (`TSK_HDB_INFO::lock`) that is real only when the library is built with `TSK_MULTITHREAD_LIB` (see `build-and-test.md`). Index loading, text lookups and SQLite lookups and inserts take it, so one handle can be shared by threads for lookups.
- On non-Windows builds the lock is a non-recursive pthread mutex (`tsk/base/tsk_lock.c`). `hdb_binsrch_lookup_str` calls the lookup callback while holding it, so a callback that performs another lookup on the same text-database handle deadlocks. The SQLite implementation calls the callback after releasing the lock.
- All functions report failure through the per-thread TSK error state (see `base-and-img.md`), using `TSK_ERR_HDB_*` codes.

### TskHdbInfo is not usable

The C++ class `TskHdbInfo` in `tsk_hashdb.h` declares a private copy constructor and no other constructor, so the compiler generates no default constructor and the class cannot be instantiated (confirmed: `TskHdbInfo h;` fails to compile). It also never initializes its handle pointer, and its destructor closes unconditionally. `samples/callback-cpp-style.cpp` itself comments that the class is incomplete. Use the C functions.

## C++ wrapper classes

The public headers `tsk/img/tsk_img.h`, `tsk/vs/tsk_vs.h` and `tsk/fs/tsk_fs.h` each end with C++ classes that wrap the C structs. Every method is a few inline lines that forward to a C function, so the header is the complete documentation of what each method calls. The classes add no locking and no error handling of their own; the claim in `tsk/docs/cpp.dox` that the C++ interface ensures thread-safe use is only true in the sense that the wrapped pointer is private.

Common rules:

- Every class holds one raw pointer to the C struct. Copy constructor and assignment are private and undefined, so the classes cannot be copied or moved. Hold them by pointer or construct them in place.
- `open(...)` methods return 0 on success and 1 on error. Most getters return 0 or NULL when the wrapped pointer is NULL.
- A method that returns a `Tsk*` pointer allocates a new wrapper with `new`. The caller must `delete` it, with the exceptions listed in the table below.
- Calling `open()` on a wrapper that is already open overwrites the pointer and leaks the first handle.

### Ownership table

"Owning" means the destructor (or `close()`) calls the C close function on the wrapped handle.

| Class (header) | Constructed how | Destructor behaviour | Safe to `close()` and then destroy |
|---|---|---|---|
| `TskImgInfo` (`tsk_img.h`) | default + `open()`, or from `TSK_IMG_INFO *` | If the pointer is non-NULL, always closes it by calling `m_imgInfo->close(m_imgInfo)` directly, even when built from a borrowed pointer (`m_opened` is ignored). Calling the function pointer instead of `tsk_img_close` skips `tsk_deinit_lock(&cache_lock)`. | has no `close()` |
| `TskVsInfo` (`tsk_vs.h`) | default + `open()`, or from `TSK_VS_INFO *` | Calls `tsk_vs_close` only if `open()` was used. | yes |
| `TskVsPartInfo` (`tsk_vs.h`) | from `TSK_VS_PART_INFO *` | Never closes anything. | n/a |
| `TskFsInfo` (`tsk_fs.h`) | default + `open()`, or from `TSK_FS_INFO *` | Always calls `tsk_fs_close`, even when built from a borrowed pointer. | **no** |
| `TskFsFile` (`tsk_fs.h`) | default + `open()`, from `TSK_FS_FILE *`, or `TskFsDir::getFile()` | Calls `tsk_fs_file_close` only if `open()` was used or it came from `TskFsDir::getFile()`. | yes |
| `TskFsDir` (`tsk_fs.h`) | default + `open()` | Always calls `tsk_fs_dir_close`. | **no** |
| `TskFsBlock` (`tsk_fs.h`) | default + `open()`, or from `const TSK_FS_BLOCK *` | Calls `tsk_fs_block_free` if `m_opened`; the default constructor leaves `m_opened` uninitialized. | n/a |
| `TskFsMeta`, `TskFsName`, `TskFsAttribute`, `TskFsAttrRun`, `TskFsMetaName`, `TskFsJEntry` (`tsk_fs.h`) | from the C pointer | Never close anything; they are views into a `TSK_FS_FILE`. | n/a |

"No" in the last column means `close()` does not clear the pointer, so the destructor calls the C close function a second time on freed memory. `tsk_fs_close` and `tsk_fs_dir_close` check a tag field first, which means the second call reads freed memory. Either call `close()` and never destroy the object (a leak of the wrapper), or, better, never call `close()` and let the destructor do it.

### Methods whose result must not be deleted, or must be

| Method | Returns | What to do |
|---|---|---|
| `TskVsInfo::getImgInfo()` | new `TskImgInfo` around the volume system's image | Deleting it closes the image that the volume system is using. Do not delete it while the image is needed; do not call it repeatedly. |
| `TskFsFile::getFsInfo()` | new `TskFsInfo` around the file's file system | Deleting it closes the file system. Same advice. |
| `TskFsDir::getFile(idx)` | new owning `TskFsFile`, or NULL | Delete it; that closes the underlying `TSK_FS_FILE`. |
| `TskFsDir::getFsFile()` | new non-owning `TskFsFile` | Delete it; nothing else is closed. |
| `TskVsInfo::getPart(idx)` | new non-owning `TskVsPartInfo` | Delete it. It is non-NULL even when the index is invalid (the wrapper then holds NULL). |
| `TskFsFile::getName()`, `getMeta()` | new `TskFsName` / `TskFsMeta` | Delete them. They are non-NULL whenever the file is open, even if the underlying `name` or `meta` is NULL, so `if (file->getMeta())` does not test for metadata. |
| `TskFsFile::getAttr(...)`, `getAttrDefault()` | new `TskFsAttribute` | Delete it. It is non-NULL even when the C call found no attribute. |
| `TskFsMeta::getName2(idx)`, `TskFsAttribute::getRun(idx)` | new `TskFsMetaName` / `TskFsAttrRun` | Delete them. |

### Walk callbacks

`TskVsInfo::vsPartWalk`, `TskFsInfo::dirWalk`, `metaWalk`, `blockWalk`, `TskFsFile::walk` and `TskFsAttribute::walk` take C++ callback types (`TSK_VS_PART_WALK_CPP_CB`, `TSK_FS_DIR_WALK_CPP_CB`, and so on, declared next to each class). The bridge functions in `tsk/fs/walk_cpp.cpp` wrap the C struct in a stack-allocated, non-owning C++ object and pass its address to the callback. The wrapper objects received by a callback are therefore valid only for the duration of that call.

### Other pitfalls

- `TskFsFile::open(TskFsInfo *, TskFsFile *a_fs_file, ...)`: the comment says `a_fs_file` may be NULL, but the code dereferences it. Pass a non-NULL `TskFsFile`.
- `TskFsInfo::open` wraps `tsk_fs_open_img` and `tsk_fs_open_vol`. There is no overload that takes a password; use the C function `tsk_fs_open_img_decrypt` for encrypted file systems.
- `TskImgInfo::open` overloads taking UTF-8 `char` paths exist only under `TSK_WIN32`. On other platforms `TSK_TCHAR` is already `char`.
- `TskVsPartInfo::getStart()` is in sectors. Multiply by `TskVsInfo::getBlockSize()` to get the byte offset that `TskFsInfo::open(TskImgInfo *, TSK_OFF_T, ...)` expects.
- `TskFsMetaName::getParSeq()` has no NULL check, unlike its sibling getters.

Recommendation for new code: prefer the C API, optionally with your own RAII holders such as `std::unique_ptr<TSK_FS_INFO, decltype(&tsk_fs_close)>`. (`auto.cpp` in 4.15.0 does not use them; it pairs raw open and close calls by hand, which is the source of the leaks listed under "Known defects and traps".) The wrapper classes give no extra safety and have the ownership defects listed above.

## Samples

The four programs in `samples/` are built by the normal build but not installed (`noinst_PROGRAMS = posix_style callback_style posix_cpp_style callback_cpp_style` in `samples/Makefile.am`, linked statically against `../tsk/libtsk.la`). Each takes one image path, tries a volume system at offset 0, falls back to a file system at offset 0, and reads the content of every regular file (the two callback samples compute its MD5). The hash lookup code in the two callback samples is compiled out (`#define DO_HASHLOOKUP 0`).

| File | API style | Demonstrates | Use as a model? |
|---|---|---|---|
| `posix-style.cpp` | C, open/read/close | `tsk_vs_part_get` loop, `tsk_fs_dir_open_meta` / `tsk_fs_dir_getsize` / `tsk_fs_dir_get`, `tsk_fs_file_read` in chunks, manual recursion guarded by a `TSK_STACK`, every handle closed | Yes. Best example of correct manual ownership. |
| `callback-style.cpp` | C, walk callbacks | `tsk_vs_part_walk`, `tsk_fs_dir_walk` with `RECURSE`, `tsk_fs_file_walk` feeding `TSK_MD5_*`, ignoring `TSK_ERR_FS_RECOVER` read errors on deleted files | Yes, except the disabled hash block. |
| `posix-cpp-style.cpp` | C++ wrappers, open/read/close | `TskFsDir::open/getSize/getFile`, `TskFsFile::read`, `TskVsInfo::getPart` | Only for method names. It never deletes the `TskFsDir` it allocates in `procDir` or the wrappers returned by `getFile`, `getPart`, `getName`, `getMeta`, `getFsInfo`. |
| `callback-cpp-style.cpp` | C++ wrappers, walk callbacks | `TskVsInfo::vsPartWalk`, `TskFsInfo::dirWalk`, `TskFsFile::walk` | Only for method names. See the defects below. |

## Conventions summary

- Functions returning `uint8_t` return 0 for success and 1 for error unless stated otherwise in this file. Hash lookups return `int8_t` -1 / 0 / 1.
- Errors are stored per thread and read with `tsk_error_get()`, `tsk_error_get_errno()` or `tsk_error_print()` (see `base-and-img.md`). `TskAuto::registerError()` and `TskAuto::errorRecordToString()` reset that state.
- A `TskAuto` instance has no internal locking. Use one instance from one thread at a time.
- Whoever opened a handle closes it. `TskAuto` closes only what it opened: images from `openImage*`, file systems it opened during a `findFilesIn*` call, and pools it kept.

## Known defects and traps

All items were confirmed by reading the cited source and re-checked against 4.15.0. None has an upstream fix in this tree.

### tsk/auto

1. **`TskAuto::findFilesInPool` returns `TSK_RETVAL_ENUM` values in a `uint8_t`** (`auto.cpp`, around line 446). It returns `TSK_OK` (0), `TSK_ERR` (1) or `TSK_STOP` (3), while its comment promises 1/0. A caller that treats non-zero as an error misreads a stop as an error. Compare with `TSK_STOP` explicitly, as `TskAuto::vsWalkCb` does.
2. **`TskAuto::findFilesInFs(TSK_OFF_T, TSK_FS_TYPE_ENUM, TSK_INUM_T)` mishandles external file systems** (`auto.cpp`, around line 650). When a file system from `setExternalFileSystemList` matches the offset, it walks from the root inode instead of `a_inum`, and returns a `TSK_RETVAL_ENUM` value (so `TSK_STOP` comes back as 3) instead of 0/1.
3. **`findFilesInPool` reads freed memory for any pool type other than APFS** (`auto.cpp`, the final `else` branch). It calls `tsk_pool_close(pool)` and then formats `pool->ctype` into the `TSK_ERR_POOL_UNSUPTYPE` message. An LVM pool (openable when built with `HAVE_LIBVSLVM`) reaches this branch whenever `filterPool` returns CONT, as in `TskIsImageSupported`.
4. **`findFilesInPool` leaks the pool when `filterPool` returns SKIP or STOP** (`auto.cpp`). The early returns neither close the pool nor store it in `m_poolInfos`. With the default `filterPool` (SKIP), every pool found in the image is leaked.
5. **`findFilesInVs` leaks the volume system when `filterVs` returns SKIP or STOP, or when the stop flag is already set** (`auto.cpp`, the early `return` after `filterVs`; the `tsk_vs_close` calls come only after `tsk_vs_part_walk`).
6. **`findFilesInVs` ignores the return value of its fallback calls** to `findFilesInPool` and `findFilesInFs` when no volume system is found, so a `TSK_STOP` from the fallback is visible only through `getStopProcessing()`.
7. **The stop flag is sticky** (`TskAuto::m_stopAllProcessing`); see "Stopping".
8. **`handleError()`'s return value has no effect** inside `auto.cpp`; see "Error recording".
9. **`TskAuto::getCurVsPartFlag()` reads `m_curVsPartFlag`, which the constructor does not initialize.** `isCurVsValid()` stays true after a volume system was walked, so a later `findFilesInFs` on the same instance classifies open failures using the last partition's flags.
10. **`TskCaseDb::setNSRLHashDb` and `setKnownBadHashDb` return 1 on success** (`case_db.cpp`, `return m_NSRLDb != NULL;`), the opposite of their comments. Relevant only if you maintain the case-database code.
11. **`tsk/auto/tsk_is_image_supported.h` is installed but includes the non-installed `tsk/tsk_tools_i.h`.** Judged from the include list and `Makefile.am`; not tested against an installed tree.
12. **`TskAuto::errorRecordToString` takes `error_record &` (non-const) while `getErrorList()` returns a `const std::vector`** (`tsk_auto.h`), so `for (const auto &e : getErrorList()) errorRecordToString(e)` does not compile. Iterate by value.

### tsk/hashdb

13. **`tsk_hdb_rollback_transaction` commits.** In `tsk_hashdb.c` it calls `hdb_info->commit_transaction(hdb_info)` where it should call `rollback_transaction`.
14. **`sqlite_hdb_lookup_str` returns 1 ("found") for a hash string that is not 32 characters long** (`sqlite_hdb.cpp`). It sets `TSK_ERR_HDB_ARG` and then returns 1 instead of -1; the same happens if its blob allocation fails. Looking up a SHA-1 string in a SQLite database through `tsk_hdb_lookup_str` therefore reports a hit. Check the length yourself before calling. `sqlite_hdb_lookup_bin` correctly returns -1 for a wrong length.
15. **`tsk_hdb_open_idx(NULL, ...)` returns 0**, which is its success value, although it sets `TSK_ERR_HDB_ARG`.
16. **`tsk_hdb_has_idx` answers for whichever index is already open** (`hdb_binsrch_open_idx` returns 0 as soon as `hIdx` is non-NULL, regardless of `htype`). After an MD5 index is open, `tsk_hdb_has_idx(hdb, TSK_HDB_HTYPE_SHA1_ID)` returns 1.
17. **`hdb_binsrch_open_idx_file` releases the handle lock on its error paths and its caller `hdb_binsrch_open_idx` releases it again** (`binsrch_index.cpp`). The lock is unlocked twice when an index is missing or malformed. The same function is also called from `hdb_binsrch_make_idx_idx`.
18. **Text-database lookup callbacks run with the handle lock held**; re-entrant lookups on the same handle deadlock on pthread builds (see "Ownership and thread safety").
19. **`TskHdbInfo` cannot be instantiated** (`tsk_hashdb.h`); see above.

### C++ wrappers

20. **`TskImgInfo::~TskImgInfo` ignores `m_opened`** (`tsk_img.h`) and closes borrowed images, through `m_imgInfo->close` rather than `tsk_img_close`. This makes the result of `TskVsInfo::getImgInfo()` dangerous to delete.
21. **`TskFsInfo::close` and `TskFsDir::close` do not clear their pointer** (`tsk_fs.h`), so `close()` followed by destruction closes twice. `TskFsInfo` also has no owned/borrowed flag, which makes the result of `TskFsFile::getFsInfo()` dangerous to delete.
22. **`TskFsInfo::jblkWalk` and `jentryWalk` reference undefined functions.** `tsk_fs.h` declares `tsk_fs_jblk_walk_cpp_c_cb` and `tsk_fs_jentry_walk_cpp_c_cb`; `tsk/fs/walk_cpp.cpp` defines `tsk_fs_jblk_cpp_c_cb` and `tsk_fs_jentry_cpp_c_cb`. No definition of the declared names exists in the tree, so code that calls these two methods cannot link. The bridge functions that do exist also construct a temporary `TskFsInfo` whose destructor would close the file system.
23. **`TskFsBlock::TskFsBlock()` does not initialize `m_opened`**, which the destructor reads.

### samples

24. **`samples/callback-cpp-style.cpp`, `procFs`**: calls `fs_info->close()` and then `delete fs_info`, the double close of item 21.
25. **`samples/callback-cpp-style.cpp`, `vsAct`**: calls `vs_info->getImgInfo()` for every partition and never deletes the result (deleting it would close the image, item 20). `dirAct` and `procFile` likewise leak the results of `getName()`, `getMeta()` and `getFsInfo()`, and test `getMeta() == NULL`, which is never true for an open file.
26. **`samples/callback-style.cpp`, `main`, disabled hash block**: treats `tsk_hdb_open_idx(...) == 0` as "no index", but 0 means success.
27. **`samples/callback-cpp-style.cpp`, `main`, disabled hash block**: calls `tsk_hdb_idxsetup`, which does not exist, and stores the result of `tsk_hdb_open` in a `TskHdbInfo *`.

### Upstream documentation that is wrong or stale

- `tsk/docs/hashdb.dox` and the comments in `TskHdbInfo` refer to `tsk_hdb_hasindex()` and `tsk_hdb_makeindex()`. The real names are `tsk_hdb_has_idx` and `tsk_hdb_make_index`. The page does not mention SQLite databases, `tsk_hdb_create`, the update functions or the index-only path rules, and it recommends `TskHdbInfo`.
- `tsk/docs/auto.dox` does not mention pools (skipped by default), `setFileSystemPassword`, external file system lists or `findFilesInFsRet`.
- `auto.cpp`: the comment on `setVolFilterFlags` gives the wrong default, and the comments on `findFilesInPool` give the wrong return values.
- `tsk_auto.h`: the comment on `getErrorList` mentions `addToErrorList()`, which does not exist; the method is `registerError()`.
- `tsk_hashdb.c`: the comment on `tsk_hdb_is_idx_only` mentions a `TSK_HDB_OPEN_TRY` option, which does not exist.
- `tsk/docs/cpp.dox` presents the C++ classes as the thread-safe interface; they add no synchronization.
