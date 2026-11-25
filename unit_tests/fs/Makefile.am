# Unit tests (CppUnit) for the fs layer, through the public API only: the XFS
# Fixture images in test/vm/fixtures/xfs/ and the Manifest-comparison helper
# (manifest_check.h). See docs/llm/fs.md ("Unit tests").
#
# The helper walks images with the validation setup's harness and compares the
# walk with the comparison step (test/vm/harness/, test/vm/manifest/), so it
# reads and reports the same way as the validate command. The istat tests
# parse istat's output with the CLI tool checks' parser (test/vm/toolcheck/).
VM_DIR = $(top_srcdir)/test/vm
AM_CPPFLAGS = -I$(top_builddir) -I$(top_srcdir) -I$(VM_DIR)/manifest \
    -I$(VM_DIR)/harness -I$(VM_DIR)/toolcheck $(CPPUNIT_CFLAGS) \
    -DFS_TEST_DATA_DIR='"$(abs_builddir)"'
AM_CXXFLAGS += -Wno-unused-command-line-argument
LDADD = ../../tsk/libtsk.la $(CPPUNIT_LIBS)
AM_LDFLAGS = -static

TESTS = test_fs

check_PROGRAMS = test_fs

test_fs_SOURCES = test_fs.cpp fs_test_data.h fs_test_util.h \
    manifest_check.cpp manifest_check.h manifest_check_test.cpp \
    xfs_fixture_data.h xfs_fixtures_test.cpp xfs_inode_test.cpp \
    xfs_dir_test.cpp xfs_file_test.cpp \
    xfs_superblock_test.cpp \
    vm_harness.cpp vm_manifest.cpp vm_compare.cpp vm_sha256.cpp \
    vm_toolcheck.cpp

# Test data, made before the tests run: the Fixture images and their expected
# Manifests unpacked into xfs/ (xz -d writes the images sparse), and the ext
# image of the helper's tests. Nothing here is committed.
XFS_FIXTURE_DIR = $(VM_DIR)/fixtures/xfs
XFS_VARIANTS = v4 v4-512 v4-noftype v5-1k v5-64k v5
XFS_FIXTURE_FILES = \
    $(XFS_FIXTURE_DIR)/v4.img.xz $(XFS_FIXTURE_DIR)/v4.manifest.xz \
    $(XFS_FIXTURE_DIR)/v4-512.img.xz $(XFS_FIXTURE_DIR)/v4-512.manifest.xz \
    $(XFS_FIXTURE_DIR)/v4-noftype.img.xz \
    $(XFS_FIXTURE_DIR)/v4-noftype.manifest.xz \
    $(XFS_FIXTURE_DIR)/v5-1k.img.xz $(XFS_FIXTURE_DIR)/v5-1k.manifest.xz \
    $(XFS_FIXTURE_DIR)/v5-64k.img.xz $(XFS_FIXTURE_DIR)/v5-64k.manifest.xz \
    $(XFS_FIXTURE_DIR)/v5.img.xz $(XFS_FIXTURE_DIR)/v5.manifest.xz

check_DATA = xfs-fixtures.stamp ext-small.img

xfs-fixtures.stamp: $(XFS_FIXTURE_FILES)
	rm -rf xfs xfs.tmp
	mkdir xfs.tmp
	for v in $(XFS_VARIANTS); do \
	    for f in img manifest; do \
	        cp $(XFS_FIXTURE_DIR)/$$v.$$f.xz xfs.tmp/ && \
	        xz -d xfs.tmp/$$v.$$f.xz || exit 1; \
	    done; \
	done
	mv xfs.tmp xfs
	touch $@

ext-small.img: $(srcdir)/make-ext-image.sh
	$(SHELL) $(srcdir)/make-ext-image.sh $@

EXTRA_DIST = make-ext-image.sh

CLEANFILES = xfs-fixtures.stamp ext-small.img

MAINTAINERCLEANFILES = Makefile.in

clean-local:
	-rm -rf xfs xfs.tmp ext-small.img.tmp ext-small.img.tmp.d xfs-sb-*.img \
	    xfs-inode-*.img xfs-dir-*.img
