OPENBSD_VERSION=7.8
OPENBSD_SDK=$(SDK_PATH)/openbsd-cross-$(OPENBSD_VERSION)

# We can't just use $(shell command -v clang) because GNU Make handles builtins
# in a special way and doesn't know that `command` is a POSIX-standard builtin
# prior to 1af314465e5dfe3e8baa839a32a72e83c04f26ef, first released in v4.2.90.
# At the time of writing, GNU Make v4.2.1 is still being used in supported
# distro releases.
#
# Source: https://lists.gnu.org/archive/html/bug-make/2017-11/msg00017.html
clang_prog=$(shell $(SHELL) $(.SHELLFLAGS) "command -v clang")
clangxx_prog=$(shell $(SHELL) $(.SHELLFLAGS) "command -v clang++")

openbsd_AR=$(shell $(SHELL) $(.SHELLFLAGS) "command -v llvm-ar")
openbsd_DSYMUTIL=$(shell $(SHELL) $(.SHELLFLAGS) "command -v dsymutil")
openbsd_NM=$(shell $(SHELL) $(.SHELLFLAGS) "command -v llvm-nm")
openbsd_OBJCOPY=$(shell $(SHELL) $(.SHELLFLAGS) "command -v llvm-objcopy")
openbsd_OBJDUMP=$(shell $(SHELL) $(.SHELLFLAGS) "command -v llvm-objdump")
openbsd_RANLIB=$(shell $(SHELL) $(.SHELLFLAGS) "command -v llvm-ranlib")
openbsd_STRIP=$(shell $(SHELL) $(.SHELLFLAGS) "command -v llvm-strip")


openbsd_CC=$(clang_prog) --target=$(host) \
              --sysroot=$(OPENBSD_SDK)

openbsd_CXX=$(clangxx_prog) --target=$(host) \
              --sysroot=$(OPENBSD_SDK) -stdlib=libc++

openbsd_CFLAGS=
openbsd_CXXFLAGS=
openbsd_LDFLAGS=-fuse-ld=lld

openbsd_release_CFLAGS=-O2
openbsd_release_CXXFLAGS=$(openbsd_release_CFLAGS)

openbsd_debug_CFLAGS=-O1 -g
openbsd_debug_CXXFLAGS=$(openbsd_debug_CFLAGS)

openbsd_cmake_system_name=OpenBSD
