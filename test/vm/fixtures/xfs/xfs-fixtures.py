#!/usr/bin/env python3
"""Build and check the XFS fixture images (test/vm/README.md, "XFS fixture images").

Run in the devcontainer (it needs xfsprogs and xz), from anywhere in the
repository, as an ordinary user:

    test/vm/fixtures/xfs/xfs-fixtures.py build  [VARIANT...]  # build, verify, write <variant>.img.xz and .manifest.xz
    test/vm/fixtures/xfs/xfs-fixtures.py verify [VARIANT...]  # verify the committed images against their Manifests
    test/vm/fixtures/xfs/xfs-fixtures.py check  [VARIANT...]  # rebuild in a temporary directory, compare with the committed Manifests

Nothing is mounted and nothing needs root. Each image is made by
`mkfs.xfs -p` from a protofile that this script generates from a fixed tree
description. The description also gives the expected Manifest. The
timestamps, which a protofile can't set, are then written into the inodes
directly. The image is read back with xfs_db alone (no code shared with the
writing side) and checked with `xfs_repair -n`, its directory formats are
checked, and the Manifest read back must equal the expected one under the
comparison step's Strict rules (build/vm-tools/compare-manifests).
"""

import argparse
import calendar
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
COMPARE = os.path.join(REPO, 'build', 'vm-tools', 'compare-manifests')
MANIFEST_MK = os.path.join(REPO, 'test', 'vm', 'manifest', 'manifest.mk')
SELF = os.path.relpath(os.path.abspath(__file__), REPO)

MIB = 1024 * 1024


class Variant:
    """One fixture image: mkfs.xfs options and the geometry they must give."""

    def __init__(self, name, mkfs, size_mib, blocksize, v5, uuid, label, ftype=True):
        self.name = name
        self.mkfs = mkfs
        self.size = size_mib * MIB
        self.blocksize = blocksize
        self.v5 = v5
        self.uuid = uuid
        self.label = label
        # mkfs.xfs defaults: directory blocks are 4 KiB or one file system
        # block if that is larger; inodes are 256 bytes on v4, 512 on v5.
        self.dirblksize = max(4096, blocksize)
        self.inodesize = 512 if v5 else 256
        # bigtime is a v5 default since xfsprogs 5.15; v4 can't have it.
        self.bigtime = v5
        # Directory entries record the file type (a default; v5 always).
        self.ftype = ftype


# mkfs.xfs needs more than 300 MB. With 64 KiB blocks, 320 MiB in four AGs is
# too small for the minimum log, so that variant gets 512 MiB.
VARIANTS = [
    Variant('v4', ['-m', 'crc=0'], 320, 4096, False,
            '5f4b0c1e-0000-4000-8000-000000000004', 'tsk-v4'),
    Variant('v4-512', ['-m', 'crc=0', '-b', 'size=512'], 320, 512, False,
            '5f4b0c1e-0000-4000-8000-000000000512', 'tsk-v4-512'),
    Variant('v4-noftype', ['-m', 'crc=0', '-n', 'ftype=0'], 320, 4096, False,
            '5f4b0c1e-0000-4000-8000-0000000000f0', 'tsk-v4-noft', ftype=False),
    Variant('v5-1k', ['-b', 'size=1024'], 320, 1024, True,
            '5f4b0c1e-0000-4000-8000-000000001024', 'tsk-v5-1k'),
    Variant('v5-64k', ['-b', 'size=65536'], 512, 65536, True,
            '5f4b0c1e-0000-4000-8000-000000065536', 'tsk-v5-64k'),
    Variant('v5', [], 320, 4096, True,
            '5f4b0c1e-0000-4000-8000-000000000005', 'tsk-v5'),
]
VARIANT_BY_NAME = {v.name: v for v in VARIANTS}

# The directory each format is claimed for, in every variant.
DIR_FORMATS = [
    ('/dirs/shortform', 'shortform'),
    ('/dirs/block', 'block'),
    ('/dirs/leaf', 'leaf'),
    ('/dirs/node', 'node'),
    ('/dirs/btree', 'btree'),
]

# The sparse file: holes at the start, in the middle and at the end. The
# offsets are multiples of 64 KiB, so the holes are whole blocks at every
# block size.
SPARSE_UNIT = 65536
SPARSE_SIZE = 8 * SPARSE_UNIT
SPARSE_CHUNKS = [(2 * SPARSE_UNIT, 1000, 'sparse-a'), (5 * SPARSE_UNIT, 1000, 'sparse-b')]
SPARSE_HOLES = [(0, 2 * SPARSE_UNIT), (3 * SPARSE_UNIT, 5 * SPARSE_UNIT), (6 * SPARSE_UNIT, SPARSE_SIZE)]


class Fail(Exception):
    pass


# ---------------------------------------------------------------------------
# Tree description


class Node:
    """An entry of the described tree. perm holds the permission bits only."""

    def __init__(self, name, kind, perm, uid=0, gid=0):
        self.name = name
        self.kind = kind  # file, dir, symlink, chr, blk, fifo
        self.perm = perm
        self.uid = uid
        self.gid = gid
        self.chunks = []  # file: [(offset, bytes)]
        self.size = 0
        self.target = None  # symlink
        self.rdev = None  # chr, blk: (major, minor)
        self.children = []  # dir
        self.times = None  # (atime, mtime, ctime, crtime), each (sec, nsec); None: assigned
        self.path = None

    def add(self, node):
        self.children.append(node)
        return node


def pattern(seed, n):
    """n deterministic pseudo-random bytes."""
    out = bytearray()
    i = 0
    while len(out) < n:
        out += hashlib.sha256(('%s/%d' % (seed, i)).encode()).digest()
        i += 1
    return bytes(out[:n])


def file_node(name, perm, uid, gid, data):
    n = Node(name, 'file', perm, uid, gid)
    if data:
        n.chunks = [(0, data)]
    n.size = len(data)
    return n


def dir_entry_size(namelen, ftype=True):
    # xfs_dir2_data_entry: inumber, namelen, name, ftype (if the file system
    # has it), tag; 8-byte aligned.
    return (8 + 1 + namelen + (1 if ftype else 0) + 2 + 7) // 8 * 8


def describe(v):
    """The tree of variant v. Entry counts follow from the geometry, so each
    directory under /dirs lands in the format it is named after (checked
    on the built image)."""
    bs = v.blocksize
    root = Node('', 'dir', 0o755)
    root.add(file_node('empty', 0o644, 0, 0, b''))
    root.add(file_node('small.txt', 0o644, 1000, 1000, b'Hello from an XFS fixture image.\n'))
    root.add(file_node('multiblock.bin', 0o600, 1000, 1000, pattern('multiblock', 3 * bs + bs // 2)))
    sparse = root.add(Node('sparse.bin', 'file', 0o640, 1000, 100))
    sparse.chunks = [(off, pattern(seed, n)) for off, n, seed in SPARSE_CHUNKS]
    sparse.size = SPARSE_SIZE
    root.add(file_node('setuid', 0o4755, 0, 0, b'#!/bin/sh\n'))
    root.add(file_node('setgid', 0o2711, 0, 50, b'#!/bin/sh\n'))
    root.add(file_node('x' * 255, 0o444, 65534, 65534, b'255-byte name\n'))
    root.add(Node('fifo', 'fifo', 0o600, 1000, 1000))
    chr_ = root.add(Node('chardev', 'chr', 0o666))
    chr_.rdev = (1, 3)
    blk = root.add(Node('blockdev', 'blk', 0o660, 0, 6))
    blk.rdev = (7, 0)
    short = root.add(Node('symlink-short', 'symlink', 0o777))
    short.target = 'small.txt'
    # Too long for the inode's literal area: stored in a remote block.
    long_ = root.add(Node('symlink-long', 'symlink', 0o777))
    long_.target = '/'.join('component%02d' % i for i in range(60))

    times = root.add(Node('times', 'dir', 0o755))
    special = [
        ('epoch', (0, 0)),
        ('pre-1970', (-1000000000, 5)),
        ('int32-min', (-(2**31), 0)),
        ('int32-max', (2**31 - 1, 999999999)),
    ]
    if v.bigtime:
        special += [('post-2038', (4102444800, 1)), ('bigtime-max', (16299260424, 999999999))]
    for name, t in special:
        n = times.add(file_node(name, 0o644, 0, 0, name.encode() + b'\n'))
        n.times = (t, t, t, t)

    dirs = root.add(Node('dirs', 'dir', 0o755))
    hdr = 64 if v.v5 else 16  # directory data/leaf block header
    per_block = lambda namelen: (v.dirblksize - hdr) // dir_entry_size(namelen, v.ftype)
    leaf_capacity = (v.dirblksize - hdr) // 8

    def fill(d, count, namelen, prefix, every=0):
        for i in range(count):
            name = ('%s%06d' % (prefix, i)).ljust(namelen, prefix[0])
            # A 1-byte file every `every` entries takes a data block between
            # two directory blocks, so the directory's extents can't merge.
            data = b'x' if every and i % every == 0 else b''
            d.add(file_node(name, 0o644, 0, 0, data))

    # Short-form: a few entries in the inode.
    fill(dirs.add(Node('shortform', 'dir', 0o755)), 3, 8, 's')
    # Block: too big for the inode, one directory block.
    fill(dirs.add(Node('block', 'dir', 0o755)), 20, 16, 'b')
    # Leaf: about 2.5 data blocks, one leaf block. Long names keep the
    # entry count down.
    fill(dirs.add(Node('leaf', 'dir', 0o755)), per_block(240) * 5 // 2, 240, 'l')
    # Node: more entries than one leaf block holds, whatever their names.
    fill(dirs.add(Node('node', 'dir', 0o755)), leaf_capacity * 5 // 4, 8, 'n')
    # B+tree: more extents than fit in the inode (16 bytes each). Long names
    # keep the entry count down here too.
    literal = v.inodesize - (176 if v.v5 else 100)
    nblocks = literal // 16 + 10
    fill(dirs.add(Node('btree', 'dir', 0o755)), per_block(240) * nblocks, 240, 't',
         every=max(1, per_block(240) // 4))

    flatten(root)
    return root


def flatten(root):
    """Sets each node's path and assigns default timestamps in tree order."""
    out = []

    def walk(n, path):
        n.path = path
        out.append(n)
        for c in n.children:
            walk(c, (path.rstrip('/') + '/' + c.name))

    walk(root, '/')
    for i, n in enumerate(out):
        if n.times is None:
            sec = 1600000000 + i * 97
            ns = (i * 7654321 + 123) % 1000000000
            # atime, mtime, ctime, crtime: all different, crtime first.
            n.times = ((sec + 30, ns), (sec, ns), (sec + 10, (ns + 1) % 1000000000), (sec - 10, ns))
    return out


def nodes(root):
    out = []

    def walk(n):
        out.append(n)
        for c in n.children:
            walk(c)

    walk(root)
    return out


# ---------------------------------------------------------------------------
# Protofile


def proto_mode(n):
    kind = {'file': '-', 'dir': 'd', 'symlink': 'l', 'chr': 'c', 'blk': 'b', 'fifo': 'p'}[n.kind]
    if n.perm & 0o1000:
        raise Fail('%s: a protofile cannot set the sticky bit' % n.path)
    return '%s%s%s%03o' % (kind, 'u' if n.perm & 0o4000 else '-', 'g' if n.perm & 0o2000 else '-',
                           n.perm & 0o777)


def write_protofile(root, path, srcdir):
    """mkfs.xfs protofile (xfs_mkfs(8), -p) for the tree, with each regular
    file's content in srcdir."""
    lines = ['/dev/null', '0 0', '%s %d %d' % (proto_mode(root), root.uid, root.gid)]
    counter = [0]

    def entry(n, depth):
        ind = ' ' * depth
        if re.search(r'\s', n.name) or not n.name:
            raise Fail('%s: protofile names cannot contain white space' % n.path)
        head = '%s%s %s %d %d' % (ind, n.name, proto_mode(n), n.uid, n.gid)
        if n.kind == 'file':
            src = os.path.join(srcdir, str(counter[0]))
            counter[0] += 1
            with open(src, 'wb') as f:
                for off, data in n.chunks:
                    f.seek(off)
                    f.write(data)
                f.truncate(n.size)  # a hole at the end stays a hole
            lines.append('%s %s' % (head, src))
        elif n.kind == 'dir':
            lines.append(head)
            for c in n.children:
                entry(c, depth + 1)
            lines.append(ind + ' $')
        elif n.kind == 'symlink':
            if re.search(r'\s', n.target):
                raise Fail('%s: protofile symlink targets cannot contain white space' % n.path)
            lines.append('%s %s' % (head, n.target))
        elif n.kind in ('chr', 'blk'):
            lines.append('%s %d %d' % (head, n.rdev[0], n.rdev[1]))
        else:
            lines.append(head)

    for c in root.children:
        entry(c, 1)
    lines.append('$')
    with open(path, 'w') as f:
        f.write('\n'.join(lines) + '\n')


# ---------------------------------------------------------------------------
# Manifests (format: test/vm/README.md, "Manifest format")

MANIFEST_HEADER = '#tsk-manifest v1'


class Entry:
    def __init__(self, path):
        self.path = path  # bytes
        self.type = None
        self.inode = None
        self.mode = None
        self.uid = None
        self.gid = None
        self.size = None
        self.nlink = None
        self.target = None  # bytes or None
        self.sha256 = None
        self.mtime = None  # (sec, nsec)
        self.ctime = None
        self.crtime = None  # or None
        self.atime = None  # or None


def encode_bytes(raw):
    s = ''.join(chr(b) if 0x21 <= b <= 0x7E and b != 0x25 else '%%%02X' % b for b in raw)
    return '%2D' if s == '-' else s


def fmt_time(t):
    return '-' if t is None else str(t[0] * 1000000000 + t[1])


def format_entry(e):
    fields = [
        encode_bytes(e.path), e.type, str(e.inode), '%04o' % e.mode, str(e.uid), str(e.gid),
        str(e.size), str(e.nlink), '-' if e.target is None else encode_bytes(e.target),
        e.sha256 or '-', fmt_time(e.mtime), fmt_time(e.ctime), fmt_time(e.crtime),
        fmt_time(e.atime)
    ]
    return '\t'.join(fields)


def write_manifest(path, entries, comments):
    with open(path, 'w', encoding='ascii') as f:
        f.write(MANIFEST_HEADER + '\n')
        for c in comments:
            f.write('# %s\n' % c)
        for e in sorted(entries.values(), key=lambda e: e.path):
            f.write(format_entry(e) + '\n')


def expected_entries(root, v):
    """The Manifest the description gives. Inode numbers and directory sizes
    are mkfs's choice; fill_from_image takes them from the image."""
    out = {}
    for n in nodes(root):
        e = Entry(n.path.encode())
        e.type = {'file': 'file', 'dir': 'dir', 'symlink': 'symlink', 'chr': 'chr', 'blk': 'blk',
                  'fifo': 'fifo'}[n.kind]
        e.mode, e.uid, e.gid = n.perm, n.uid, n.gid
        if n.kind == 'dir':
            e.nlink = 2 + sum(1 for c in n.children if c.kind == 'dir')
        else:
            e.nlink = 1
        if n.kind == 'file':
            e.size = n.size
            content = bytearray(n.size)
            for off, data in n.chunks:
                content[off:off + len(data)] = data
            e.sha256 = hashlib.sha256(bytes(content)).hexdigest()
        elif n.kind == 'symlink':
            e.target = n.target.encode()
            e.size = len(e.target)
        elif n.kind != 'dir':
            e.size = 0
        e.atime, e.mtime, e.ctime, crtime = n.times
        e.crtime = crtime if v.v5 else None
        out[e.path] = e
    return out


def fill_from_image(expected, actual):
    for path, e in expected.items():
        a = actual.get(path)
        e.inode = a.inode if a else 0
        if e.type == 'dir':
            e.size = a.size if a else 0


# ---------------------------------------------------------------------------
# Tools


def run(args, **kw):
    kw.setdefault('stdout', subprocess.PIPE)
    kw.setdefault('stderr', subprocess.PIPE)
    p = subprocess.run(args, env=dict(os.environ, TZ='UTC', LC_ALL='C'), **kw)
    return p


def must(args, what, **kw):
    p = run(args, **kw)
    if p.returncode != 0:
        raise Fail('%s failed (exit %d): %s\n%s' %
                   (what, p.returncode, ' '.join(args), (p.stderr or b'').decode(errors='replace')))
    return p


def xfs_db(image, commands, tmpdir):
    """Runs xfs_db read-only on image with the commands (a list of strings),
    each preceded by a marker. Returns {marker index: [output lines]}."""
    script = os.path.join(tmpdir, 'xfs_db.cmd')
    with open(script, 'w', encoding='utf-8', errors='surrogateescape') as f:
        for i, c in enumerate(commands):
            f.write('echo @@%d\n' % i)
            for line in c if isinstance(c, list) else [c]:
                f.write(line + '\n')
    p = must(['xfs_db', '-r', '-f', '-c', 'source ' + script, image], 'xfs_db')
    out = {}
    cur = None
    for line in p.stdout.decode('utf-8', errors='surrogateescape').split('\n'):
        m = re.match(r'^@@(\d+) ?$', line)
        if m:
            cur = int(m.group(1))
            out[cur] = []
        elif cur is not None and line:
            out[cur].append(line)
    if len(out) != len(commands):
        raise Fail('xfs_db ran %d of %d command groups' % (len(out), len(commands)))
    return out


def fields(lines):
    """xfs_db `print` output as {field: value}."""
    out = {}
    for line in lines:
        m = re.match(r'^(\S+) = (.*)$', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def xfs_info(image):
    text = must(['xfs_info', image], 'xfs_info').stdout.decode()
    lines = [re.sub(r'^meta-data=\S+', 'meta-data=IMAGE', l).rstrip() for l in text.splitlines()]
    return text, lines


def check_geometry(v, info):
    """The superblock must have the geometry describe() assumed."""

    def get(pattern):
        m = re.search(pattern, info, re.M)
        if not m:
            raise Fail('xfs_info: no %s' % pattern)
        return int(m.group(1))

    got = {
        'block size': get(r'^data\s+=\s+bsize=(\d+)'),
        'directory block size': get(r'^naming\s+=version 2\s+bsize=(\d+)'),
        'inode size': get(r'isize=(\d+)'),
        'crc': get(r'crc=(\d)'),
        'bigtime': get(r'bigtime=(\d)'),
        'ftype': get(r'ftype=(\d)'),
    }
    want = {
        'block size': v.blocksize,
        'directory block size': v.dirblksize,
        'inode size': v.inodesize,
        'crc': int(v.v5),
        'bigtime': int(v.bigtime),
        'ftype': int(v.ftype),
    }
    for k in want:
        if got[k] != want[k]:
            raise Fail('%s: %s is %d, expected %d' % (v.name, k, got[k], want[k]))


# ---------------------------------------------------------------------------
# Building


def mkfs(v, root, image, tmpdir):
    srcdir = os.path.join(tmpdir, 'src')
    os.mkdir(srcdir)
    proto = os.path.join(tmpdir, 'protofile')
    write_protofile(root, proto, srcdir)
    with open(image, 'wb') as f:
        f.truncate(v.size)
    args = ['mkfs.xfs', '-q', '-f', '-p', proto] + v.mkfs + ['-m', 'uuid=' + v.uuid, '-L', v.label, image]
    p = must(args, 'mkfs.xfs')
    shutil.rmtree(srcdir)
    warnings = [l for l in p.stderr.decode(errors='replace').splitlines() if l.strip()]
    return args, warnings


def walk_image(image, tmpdir):
    """{path bytes: (inode, ls type)} for every entry, read with xfs_db ls."""
    sb = fields(xfs_db(image, [['sb 0', 'print rootino']], tmpdir)[0])
    rootino = int(sb['rootino'])
    found = {b'/': (rootino, 'directory')}
    level = [(b'/', rootino)]
    while level:
        out = xfs_db(image, [['inode %d' % ino, 'ls'] for _, ino in level], tmpdir)
        nxt = []
        unknown = []  # (path, inode) of entries ls has no type for (no ftype)
        for i, (dpath, _) in enumerate(level):
            for line in out[i]:
                m = re.match(r'^\s*\d+\s+(\d+)\s+(\S+)\s+0x[0-9a-fA-F]+\s+(\d+) ', line)
                if not m:
                    raise Fail('xfs_db ls: unexpected line %r' % line)
                namelen = int(m.group(3))  # in bytes
                rest = line[m.end():].encode('utf-8', errors='surrogateescape')
                raw = rest[:namelen]
                if rest[namelen:] != b' (good)':
                    raise Fail('xfs_db ls: bad entry %r' % line)
                if raw in (b'.', b'..'):
                    continue
                path = (dpath if dpath == b'/' else dpath + b'/') + raw
                ino = int(m.group(1))
                if path in found:
                    raise Fail('duplicate path %r' % path)
                found[path] = (ino, m.group(2))
                if m.group(2) == 'directory':
                    nxt.append((path, ino))
                elif m.group(2) == 'unknown':
                    unknown.append((path, ino))
        # Without ftype, ls says "unknown" for every entry: the inode's mode
        # tells the directories.
        if unknown:
            out = xfs_db(image, [['inode %d' % ino, 'print core.mode'] for _, ino in unknown], tmpdir)
            for i, (path, ino) in enumerate(unknown):
                mode = fields(out[i]).get('core.mode')
                if mode is None:
                    raise Fail('%r: xfs_db printed no core.mode' % path)
                if int(mode, 8) & 0o170000 == 0o040000:
                    nxt.append((path, ino))
        level = nxt
    return found


def crc32c_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table


CRC32C = crc32c_table()


def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc = CRC32C[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc ^ 0xFFFFFFFF


XFS_DIFLAG2_BIGTIME = 1 << 3
BIGTIME_EPOCH_OFFSET = 2**31  # bigtime counts nanoseconds from -2^31 s


def encode_time(t, bigtime):
    sec, nsec = t
    if bigtime:
        return struct.pack('>Q', (sec + BIGTIME_EPOCH_OFFSET) * 1000000000 + nsec)
    if not -(2**31) <= sec < 2**31:
        raise Fail('timestamp %d out of range without bigtime' % sec)
    return struct.pack('>iI', sec, nsec)


def set_times(v, image, inode_times, tmpdir):
    """Writes atime, mtime, ctime and (v5) crtime into each inode and updates
    its CRC. A protofile has no timestamps, and xfs_db's `write` can't be
    trusted with them (on 6.13 a bigtime `sec` write stores the raw 64-bit
    value, and negative 32-bit values come out wrong). xfs_db reads them back
    independently in read_image."""
    inos = sorted(inode_times)
    out = xfs_db(image, ['convert ino %d byte' % ino for ino in inos], tmpdir)
    with open(image, 'r+b') as f:
        for i, ino in enumerate(inos):
            m = re.match(r'^0x[0-9a-f]+ \((\d+)\)$', out[i][0]) if out[i] else None
            if not m:
                raise Fail('xfs_db convert: unexpected %r' % out[i])
            off = int(m.group(1))
            f.seek(off)
            buf = bytearray(f.read(v.inodesize))
            if buf[0:2] != b'IN' or buf[4] != (3 if v.v5 else 2):
                raise Fail('inode %d: no inode core at byte %d' % (ino, off))
            bigtime = v.v5 and struct.unpack_from('>Q', buf, 120)[0] & XFS_DIFLAG2_BIGTIME
            if bool(bigtime) != v.bigtime:
                raise Fail('inode %d: bigtime flag is %d' % (ino, bool(bigtime)))
            atime, mtime, ctime, crtime = inode_times[ino]
            for t, at in ((atime, 32), (mtime, 40), (ctime, 48)):
                buf[at:at + 8] = encode_time(t, bigtime)
            if v.v5:
                buf[144:152] = encode_time(crtime, bigtime)
                buf[100:104] = b'\0\0\0\0'
                buf[100:104] = struct.pack('<I', crc32c(bytes(buf)))
            f.seek(off)
            f.write(buf)


# ---------------------------------------------------------------------------
# Reading an image back with xfs_db

S_IFMT = 0o170000
TYPES = {0o100000: 'file', 0o040000: 'dir', 0o120000: 'symlink', 0o020000: 'chr', 0o060000: 'blk',
         0o010000: 'fifo', 0o140000: 'sock'}
TIME_FIELDS = [('atime', 'core.atime'), ('mtime', 'core.mtime'), ('ctime', 'core.ctime'),
               ('crtime', 'v3.crtime')]


def parse_xfs_db_time(sec_text, nsec_text):
    """xfs_db prints seconds as a ctime(3) date (in UTC, as TZ is set)."""
    sec = calendar.timegm(time.strptime(sec_text, '%a %b %d %H:%M:%S %Y'))
    return (sec, int(nsec_text))


def parse_bmap(lines):
    """xfs_db bmap: [(file block, AG, AG block, count, unwritten)]."""
    out = []
    for line in lines:
        m = re.match(r'^data offset (\d+) startblock \d+ \((\d+)/(\d+)\) count (\d+) flag (\d)$', line)
        if not m:
            raise Fail('xfs_db bmap: unexpected line %r' % line)
        out.append(tuple(int(g) for g in m.groups()))
    return out


def read_extents(f, extents, size, sb, skip=0):
    """Content of a file of `size` bytes from its extents. With skip, each
    block starts with a header of that many bytes (v5 remote symlinks)."""
    bs, agblocks = sb['blocksize'], sb['agblocks']
    if skip:
        data = bytearray()
        for fblk, ag, agblk, count, _ in sorted(extents):
            for k in range(count):
                f.seek((ag * agblocks + agblk + k) * bs + skip)
                data += f.read(bs - skip)
        return bytes(data[:size])
    data = bytearray(size)
    for fblk, ag, agblk, count, unwritten in extents:
        start = fblk * bs
        if start >= size or unwritten:
            continue
        n = min(count * bs, size - start)
        f.seek((ag * agblocks + agblk) * bs)
        data[start:start + n] = f.read(n)
    return bytes(data)


def read_image(v, image, tmpdir):
    """The image's Manifest, its inode details and file extents, all from
    xfs_db (directory walk, inode fields, block maps) plus raw reads of the
    mapped blocks."""
    sbf = fields(xfs_db(image, [['sb 0', 'print blocksize agblocks']], tmpdir)[0])
    sb = {'blocksize': int(sbf['blocksize']), 'agblocks': int(sbf['agblocks'])}
    walked = walk_image(image, tmpdir)
    paths = sorted(walked)
    names = ['core.mode', 'core.uid', 'core.gid', 'core.nlinkv2', 'core.size', 'core.format', 'core.nextents']
    for _, f in TIME_FIELDS if v.v5 else TIME_FIELDS[:3]:
        names += [f + '.sec', f + '.nsec']
    if v.v5:
        names.append('v3.crc')
    out = xfs_db(image, [['inode %d' % walked[p][0], 'print ' + ' '.join(names)] for p in paths], tmpdir)
    inodes = {}
    for i, p in enumerate(paths):
        flds = fields(out[i])
        missing = [n for n in names if n not in flds]
        if missing:
            raise Fail('%r: xfs_db printed no %s' % (p, ', '.join(missing)))
        inodes[p] = flds

    types = {p: TYPES.get(int(inodes[p]['core.mode'], 8) & S_IFMT, 'other') for p in paths}
    # Block maps of regular files with data and of remote symlinks.
    need = [p for p in paths if types[p] == 'file' and int(inodes[p]['core.size']) > 0
            or types[p] == 'symlink' and inodes[p]['core.format'].startswith('2 ')]
    bmaps = {}
    if need:
        out = xfs_db(image, [['inode %d' % walked[p][0], 'bmap'] for p in need], tmpdir)
        bmaps = {p: parse_bmap(out[i]) for i, p in enumerate(need)}
    local = [p for p in paths if types[p] == 'symlink' and p not in bmaps]
    symfield = 'u3.symlink' if v.v5 else 'u.symlink'
    targets = {}
    if local:
        out = xfs_db(image, [['inode %d' % walked[p][0], 'print ' + symfield] for p in local], tmpdir)
        for i, p in enumerate(local):
            m = re.match(r'^%s = "(.*)"$' % re.escape(symfield), out[i][0] if out[i] else '')
            if not m:
                raise Fail('%r: xfs_db printed no symlink target' % p)
            targets[p] = m.group(1).encode('utf-8', errors='surrogateescape')

    entries = {}
    with open(image, 'rb') as f:
        for p in paths:
            d = inodes[p]
            if v.v5 and not d['v3.crc'].endswith('(correct)'):
                raise Fail('%r: inode CRC %s' % (p, d['v3.crc']))
            mode = int(d['core.mode'], 8)
            e = Entry(p)
            e.type = types[p]
            e.inode = walked[p][0]
            e.mode = mode & 0o7777
            e.uid, e.gid = int(d['core.uid']), int(d['core.gid'])
            e.nlink = int(d['core.nlinkv2'])
            size = int(d['core.size'])
            e.size = size
            for attr, f_ in TIME_FIELDS if v.v5 else TIME_FIELDS[:3]:
                setattr(e, attr, parse_xfs_db_time(d[f_ + '.sec'], d[f_ + '.nsec']))
            if e.type == 'file':
                e.sha256 = hashlib.sha256(read_extents(f, bmaps.get(p, []), size, sb)).hexdigest()
            elif e.type == 'symlink':
                if p in bmaps:
                    e.target = read_extents(f, bmaps[p], size, sb, skip=56 if v.v5 else 0)
                else:
                    e.target = targets[p]
                if len(e.target) != size:
                    raise Fail('%r: symlink target is %d bytes, size %d' % (p, len(e.target), size))
            entries[p] = e
    return entries, inodes, bmaps, walked


# ---------------------------------------------------------------------------
# Checks on a built image

# Magic numbers (v4, v5) of the block at the leaf offset, and of a single-block
# directory's block.
LEAF_MAGIC = {False: {'0xd2f1': 'leaf', '0xfebe': 'node'}, True: {'0x3df1': 'leaf', '0x3ebe': 'node'}}
BLOCK_MAGIC = {False: '0x58443242', True: '0x58444233'}  # XD2B, XDB3


def first_magic(lines):
    """The magic of the directory block xfs_db printed. If the block is
    unmapped, xfs_db stays on the inode, whose core.magic doesn't count."""
    for line in lines:
        m = re.match(r'^(?!core\.)\S*magic = (0x[0-9a-f]+)$', line)
        if m:
            return m.group(1)
    return None


def check_dir_formats(v, image, inodes, walked, tmpdir):
    """Each directory under /dirs must have the format it is named after:
    short-form (data fork local), block (extents, one directory block whose
    magic is that of a block directory), leaf (extents, a leaf1 block at the
    leaf offset), node (extents, a da-btree node there) or B+tree (data fork
    btree). Returns report lines."""
    leaf_fsblock = (32 << 30) // v.blocksize  # XFS_DIR2_LEAF_OFFSET in file system blocks
    paths = [p.encode() for p, _ in DIR_FORMATS]
    cmds = []
    for p in paths:
        cmds += [['inode %d' % walked[p][0], 'dblock 0', 'print'],
                 ['inode %d' % walked[p][0], 'dblock %d' % leaf_fsblock, 'print']]
    out = xfs_db(image, cmds, tmpdir)
    report = []
    for i, (path, claim) in enumerate(DIR_FORMATS):
        d = inodes[path.encode()]
        fmt = int(d['core.format'].split()[0])
        size, nextents = int(d['core.size']), int(d['core.nextents'])
        first = first_magic(out[2 * i])
        leaf = first_magic(out[2 * i + 1])
        if leaf:
            leaf = LEAF_MAGIC[v.v5].get(leaf, 'magic ' + leaf)
        if fmt == 1:
            got = 'shortform'
        elif fmt == 3:
            got = 'btree'
        elif fmt == 2 and size == v.dirblksize and first == BLOCK_MAGIC[v.v5] and not leaf:
            got = 'block'
        elif fmt == 2:
            got = leaf or 'extents without a leaf block'
        else:
            got = 'data fork format %d' % fmt
        detail = 'format %s, size %d, %d extents%s' % (d['core.format'], size, nextents,
                                                        ', leaf offset: %s' % leaf if leaf else '')
        if got != claim:
            raise Fail('%s: %s is %s, not %s (%s)' % (v.name, path, got, claim, detail))
        report.append('%-16s %-9s  %s' % (path, claim, detail))
    return report


def check_sparse(v, bmaps):
    """sparse.bin must have holes at the start, in the middle and at the end:
    every block holding data is mapped and no block in SPARSE_HOLES is. mkfs
    copies data in the granularity the source file system reports (4 KiB on
    the usual host file systems), so more than the data blocks may be mapped."""
    bs = v.blocksize
    data = set()
    for off, n, _ in SPARSE_CHUNKS:
        data.update(range(off // bs, (off + n - 1) // bs + 1))
    holes = set()
    for start, end in SPARSE_HOLES:
        holes.update(range(start // bs, end // bs))
    mapped = []
    got = set()
    for fblk, _, _, count, _ in sorted(bmaps[b'/sparse.bin']):
        got.update(range(fblk, fblk + count))
        mapped.append('%d-%d' % (fblk, fblk + count - 1))
    if not data <= got or got & holes:
        raise Fail('%s: /sparse.bin maps blocks %s: data blocks %s must be mapped and hole blocks '
                   '%s not' % (v.name, mapped, sorted(data), sorted(holes)))
    return '/sparse.bin      holes     blocks %s of 0-%d mapped' % (', '.join(mapped), SPARSE_SIZE // bs - 1)


def repair_check(v, image):
    p = run(['xfs_repair', '-n', '-f', image])
    if p.returncode != 0:
        raise Fail('%s: xfs_repair -n found problems (exit %d):\n%s' %
                   (v.name, p.returncode, (p.stdout + p.stderr).decode(errors='replace')))


def compare(expected, actual, log):
    p = run([COMPARE, '--ground-truth', expected, '--run', actual, '--mode', 'strict'])
    text = p.stdout.decode(errors='replace') + p.stderr.decode(errors='replace')
    with open(log, 'w') as f:
        f.write(text)
    if p.returncode != 0:
        lines = text.splitlines()
        if len(lines) > 31:
            lines = lines[:30] + ['...', lines[-1]]
        raise Fail('compare-manifests %s %s: exit %d\n%s' % (expected, actual, p.returncode, '\n'.join(lines)))
    return text.strip().splitlines()[-1]


def verify_image(v, image, expected_manifest, tmpdir, read=None):
    """All checks on one image file against one expected Manifest: xfs_repair,
    directory formats, holes, and the Manifest read back. `read` is
    read_image's result if the caller already has it. Returns (path of the
    Manifest read back, report lines)."""
    repair_check(v, image)
    actual, inodes, bmaps, walked = read or read_image(v, image, tmpdir)
    report = ['xfs_repair -n: no problems']
    report += check_dir_formats(v, image, inodes, walked, tmpdir)
    report.append(check_sparse(v, bmaps))
    actual_manifest = os.path.join(tmpdir, v.name + '.read.manifest')
    write_manifest(actual_manifest, actual, [])
    report.append('Manifest read back with xfs_db vs expected Manifest: %s' %
                  compare(expected_manifest, actual_manifest, os.path.join(tmpdir, 'compare.log')))
    return actual_manifest, report


def build_variant(v, tmpdir):
    """Builds and verifies variant v in tmpdir. Returns (image, expected
    Manifest, Manifest read back, report lines)."""
    root = describe(v)
    image = os.path.join(tmpdir, v.name + '.img')
    args, warnings = mkfs(v, root, image, tmpdir)
    info, info_lines = xfs_info(image)
    check_geometry(v, info)
    walked = walk_image(image, tmpdir)
    times = {}
    for n in nodes(root):
        p = n.path.encode()
        if p not in walked:
            raise Fail('%s: %s is missing from the image' % (v.name, n.path))
        times[walked[p][0]] = n.times
    set_times(v, image, times, tmpdir)

    expected = expected_entries(root, v)
    # Inode numbers and directory sizes are mkfs's choice: take them from the
    # image (xfs_db's ls and core.size), then compare everything.
    read = read_image(v, image, tmpdir)
    fill_from_image(expected, read[0])
    version = must(['mkfs.xfs', '-V'], 'mkfs.xfs -V').stdout.decode().strip()
    mkfs_cmd = ' '.join(a if not a.startswith('/') else os.path.basename(a) for a in args)
    comments = [
        'Expected Manifest of the XFS fixture image %s.img.xz, built by %s' % (v.name, SELF),
        'with %s:' % version,
        '  %s' % mkfs_cmd,
        'Inode numbers and directory sizes are read from the image; every other field',
        'comes from the tree description. Directory formats: ' +
        ', '.join('%s %s' % (p, f) for p, f in DIR_FORMATS) + '.',
        'xfs_info:',
    ] + ['  ' + l for l in info_lines]
    manifest = os.path.join(tmpdir, v.name + '.manifest')
    write_manifest(manifest, expected, comments)
    actual_manifest, report = verify_image(v, image, manifest, tmpdir, read)
    report = ['mkfs.xfs: %s' % w for w in warnings] + report
    return image, manifest, actual_manifest, report


# ---------------------------------------------------------------------------
# Commands


def ensure_tools():
    missing = [t for t in ('mkfs.xfs', 'xfs_db', 'xfs_repair', 'xfs_info', 'xz') if not shutil.which(t)]
    if missing:
        raise Fail('missing %s: run this in the devcontainer (built from .devcontainer/Dockerfile '
                   'with xfsprogs)' % ', '.join(missing))
    must(['make', '-s', '-C', REPO, '-f', MANIFEST_MK, 'build/vm-tools/compare-manifests'],
         'building compare-manifests', stdout=None, stderr=None)


def committed(v):
    """The committed image and expected Manifest of v, both xz-compressed."""
    return os.path.join(HERE, v.name + '.img.xz'), os.path.join(HERE, v.name + '.manifest.xz')


def compress(src, dst):
    """xz -9 of src into dst. Single-threaded xz output depends only on the
    input and the options."""
    with open(dst, 'wb') as f:
        must(['xz', '-9', '-T1', '-c', src], 'xz', stdout=f)


def decompress(src, tmpdir, name):
    """Decompresses src into tmpdir/name; the result is sparse."""
    out = os.path.join(tmpdir, name)
    shutil.copyfile(src, out + '.xz')
    must(['xz', '-d', out + '.xz'], 'xz -d')
    return out


def rel(path):
    return os.path.relpath(path, REPO)


def cmd_build(variants, keep):
    """Builds and checks every variant first, then replaces the committed
    files, so a failure leaves them all as they were."""
    if keep:
        os.makedirs(keep, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='xfs-fixture-') as outdir:
        done = []
        for v in variants:
            with tempfile.TemporaryDirectory(prefix='xfs-fixture-') as tmpdir:
                image, manifest, _, report = build_variant(v, tmpdir)
                img_xz, man_xz = committed(v)
                staged = [os.path.join(outdir, os.path.basename(p)) for p in (img_xz, man_xz)]
                compress(image, staged[0])
                compress(manifest, staged[1])
                if keep:
                    shutil.copyfile(image, os.path.join(keep, v.name + '.img'))
                    shutil.copyfile(manifest, os.path.join(keep, v.name + '.manifest'))
                done.append((v, report, list(zip(staged, (img_xz, man_xz)))))
        for v, report, files in done:
            for src, dst in files:
                shutil.move(src, dst)
            print_report(v, report + ['wrote %s' % ', '.join(
                '%s (%d bytes)' % (rel(dst), os.path.getsize(dst)) for _, dst in files)])


def cmd_verify(variants):
    for v in variants:
        img_xz, man_xz = committed(v)
        with tempfile.TemporaryDirectory(prefix='xfs-fixture-') as tmpdir:
            image = decompress(img_xz, tmpdir, v.name + '.img')
            manifest = decompress(man_xz, tmpdir, v.name + '.manifest')
            _, report = verify_image(v, image, manifest, tmpdir)
            print_report(v, ['committed %s and %s' % (rel(img_xz), rel(man_xz))] + report)


def cmd_check(variants):
    for v in variants:
        _, man_xz = committed(v)
        with tempfile.TemporaryDirectory(prefix='xfs-fixture-') as tmpdir:
            _, _, rebuilt, report = build_variant(v, tmpdir)
            manifest = decompress(man_xz, tmpdir, 'committed.manifest')
            report.append('rebuilt image vs committed %s: %s' %
                          (rel(man_xz), compare(manifest, rebuilt, os.path.join(tmpdir, 'c.log'))))
            print_report(v, report)


def print_report(v, lines):
    print('== %s' % v.name)
    for line in lines:
        print('   ' + line)
    sys.stdout.flush()


def main(argv):
    ap = argparse.ArgumentParser(description='Build and check the XFS fixture images.')
    ap.add_argument('command', choices=['build', 'verify', 'check'])
    ap.add_argument('variants', nargs='*', metavar='VARIANT',
                    help='default: all (%s)' % ', '.join(v.name for v in VARIANTS))
    ap.add_argument('--keep', metavar='DIR', help='build only: also copy the uncompressed images and Manifests to DIR')
    a = ap.parse_args(argv)
    try:
        unknown = [n for n in a.variants if n not in VARIANT_BY_NAME]
        if unknown:
            raise Fail('unknown variant %s' % ', '.join(unknown))
        variants = [VARIANT_BY_NAME[n] for n in a.variants] if a.variants else VARIANTS
        ensure_tools()
        if a.command == 'build':
            cmd_build(variants, a.keep)
        elif a.command == 'verify':
            cmd_verify(variants)
        else:
            cmd_check(variants)
    except Fail as e:
        print('FAIL: %s' % e, file=sys.stderr)
        return 1
    print('PASS: %s %s' % (a.command, ' '.join(v.name for v in variants)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
