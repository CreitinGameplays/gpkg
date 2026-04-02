CXX = /usr/bin/g++
STRIP = /usr/bin/strip

PROJECT_ROOT ?= $(abspath $(CURDIR)/..)
GINIT_DIR = $(PROJECT_ROOT)/ginit
GLOBAL_SRC_DIR = $(PROJECT_ROOT)/src
SYS_INFO_HEADER ?= $(GLOBAL_SRC_DIR)/sys_info.h
GPKG_VERSION_HELPER ?= $(PROJECT_ROOT)/tools/gpkg_version.py
ROOTFS ?=
TARGET_ROOTFS := $(strip $(ROOTFS))
ifeq ($(TARGET_ROOTFS),)
TARGET_ROOTFS := $(PROJECT_ROOT)/rootfs
endif
TARGET_CXX_VERSION := $(shell find $(TARGET_ROOTFS)/usr/include/c++ -maxdepth 1 -mindepth 1 -type d -printf '%f\n' 2>/dev/null | grep -E '^[0-9]+$$' | sort -V | tail -n1)
GPKG_VERSION ?=
GPKG_CODENAME ?=
LZMA_STATIC := $(firstword \
	$(wildcard $(TARGET_ROOTFS)/usr/lib/x86_64-linux-gnu/liblzma.a) \
	$(wildcard $(PROJECT_ROOT)/rootfs/usr/lib/x86_64-linux-gnu/liblzma.a))
ifeq ($(strip $(GPKG_VERSION)),)
ifneq ($(wildcard $(GPKG_VERSION_HELPER)),)
GPKG_VERSION := $(shell /usr/bin/python3 $(GPKG_VERSION_HELPER) --root-dir $(PROJECT_ROOT) --export-root $(PROJECT_ROOT)/export 2>/dev/null)
endif
endif
ifeq ($(strip $(GPKG_VERSION)),)
GPKG_VERSION := $(shell sed -n 's/^#define OS_VERSION "\(.*\)"/\1/p' $(SYS_INFO_HEADER) | head -n1)
endif
ifeq ($(strip $(GPKG_CODENAME)),)
GPKG_CODENAME := $(shell sed -n 's/^#define OS_CODENAME "\(.*\)"/\1/p' $(SYS_INFO_HEADER) | head -n1)
endif

CXXFLAGS += -Wall -Wextra -O2 -I./src -I$(GINIT_DIR)/src -I$(GLOBAL_SRC_DIR)
CXXFLAGS += '-DGPKG_VERSION="$(GPKG_VERSION)"' '-DGPKG_CODENAME="$(GPKG_CODENAME)"'
ifneq ($(wildcard $(TARGET_ROOTFS)),)
CXXFLAGS += --sysroot=$(TARGET_ROOTFS)
LDFLAGS += --sysroot=$(TARGET_ROOTFS)
endif
ifneq ($(wildcard $(TARGET_ROOTFS)),)
LIBAPT_PKG_HEADER_DIR := $(firstword \
	$(wildcard $(TARGET_ROOTFS)/usr/include/apt-pkg))
LIBAPT_PKG_RUNTIME_LIB_CANDIDATE := $(firstword \
	$(wildcard $(TARGET_ROOTFS)/usr/lib/x86_64-linux-gnu/libapt-pkg.so.[0-9]*))
else
LIBAPT_PKG_HEADER_DIR := $(firstword \
	$(wildcard /usr/include/apt-pkg))
LIBAPT_PKG_RUNTIME_LIB_CANDIDATE := $(firstword \
	$(wildcard /usr/lib/x86_64-linux-gnu/libapt-pkg.so.[0-9]*))
endif
LIBAPT_PKG_RUNTIME_LIB := $(realpath $(LIBAPT_PKG_RUNTIME_LIB_CANDIDATE))
LIBAPT_PKG_RUNTIME_DIR := $(dir $(LIBAPT_PKG_RUNTIME_LIB))
ifneq ($(strip $(LIBAPT_PKG_HEADER_DIR)),)
ifneq ($(strip $(LIBAPT_PKG_RUNTIME_LIB)),)
CXXFLAGS += -DGPKG_HAVE_WORKING_LIBAPT_PKG_BACKEND
LIBAPT_PKG_LIBS := -L$(LIBAPT_PKG_RUNTIME_DIR) -lapt-pkg -pthread
else
LIBAPT_PKG_LIBS :=
endif
else
LIBAPT_PKG_LIBS :=
endif
ifneq ($(strip $(TARGET_ROOTFS)),)
ifneq ($(wildcard $(TARGET_ROOTFS)/usr/include),)
CXXFLAGS += -I$(TARGET_ROOTFS)/usr/include
endif
endif
ifneq ($(strip $(TARGET_CXX_VERSION)),)
CXXFLAGS += -nostdinc++
CXXFLAGS += -isystem $(TARGET_ROOTFS)/usr/include/c++/$(TARGET_CXX_VERSION)
CXXFLAGS += -isystem $(TARGET_ROOTFS)/usr/include/x86_64-linux-gnu/c++/$(TARGET_CXX_VERSION)
CXXFLAGS += -isystem $(TARGET_ROOTFS)/usr/include/c++/$(TARGET_CXX_VERSION)/backward
endif
GPKG_LDFLAGS = $(LDFLAGS) -L$(GINIT_DIR)/lib -lgemcore -lssl -lcrypto -lz -lzstd -ldl -lpthread -lcrypt $(LIBAPT_PKG_LIBS)
ifeq ($(strip $(LZMA_STATIC)),)
GPKG_LDFLAGS += -llzma
else
GPKG_LDFLAGS += $(LZMA_STATIC)
endif
WORKER_LDFLAGS = $(LDFLAGS) -lssl -lcrypto -lz -lzstd -ldl -lpthread
ifeq ($(strip $(LZMA_STATIC)),)
WORKER_LDFLAGS += -llzma
else
WORKER_LDFLAGS += $(LZMA_STATIC)
endif
ifneq ($(strip $(TARGET_CXX_VERSION)),)
GPKG_LDFLAGS += $(TARGET_ROOTFS)/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
WORKER_LDFLAGS += $(TARGET_ROOTFS)/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
endif

SRCDIR = src
OBJDIR = obj
BINDIR = bin
GPKG_FRAGMENTS = $(wildcard $(SRCDIR)/*.ipp)

TARGETS = $(BINDIR)/gpkg $(BINDIR)/gpkg-worker
BUILD_CONFIG_STAMP = $(OBJDIR)/build-config.stamp

all: $(BINDIR) $(OBJDIR) $(BUILD_CONFIG_STAMP) $(TARGETS)

$(BINDIR) $(OBJDIR):
	mkdir -p $@

FORCE:

$(BUILD_CONFIG_STAMP): FORCE | $(OBJDIR)
	@tmp="$@.tmp"; \
	printf 'GPKG_VERSION=%s\nGPKG_CODENAME=%s\nTARGET_CXX_VERSION=%s\nROOTFS=%s\n' \
		'$(GPKG_VERSION)' '$(GPKG_CODENAME)' '$(TARGET_CXX_VERSION)' '$(TARGET_ROOTFS)' > "$$tmp"; \
	if [ -f "$@" ] && cmp -s "$@" "$$tmp"; then \
		rm -f "$$tmp"; \
	else \
		mv "$$tmp" "$@"; \
	fi

$(BINDIR)/gpkg: $(SRCDIR)/gpkg.cpp $(GPKG_FRAGMENTS) $(BUILD_CONFIG_STAMP)
	$(CXX) $(CXXFLAGS) -o $@ $< $(GPKG_LDFLAGS)
	$(STRIP) $@

$(BINDIR)/gpkg-worker: $(SRCDIR)/gpkg_worker.cpp $(GPKG_FRAGMENTS) $(BUILD_CONFIG_STAMP)
	$(CXX) $(CXXFLAGS) -o $@ $< $(WORKER_LDFLAGS)
	$(STRIP) $@

install: all
	mkdir -p $(DESTDIR)/bin/apps/system
	mkdir -p $(DESTDIR)/bin
	cp $(BINDIR)/gpkg $(DESTDIR)/bin/apps/system/gpkg
	cp $(BINDIR)/gpkg-worker $(DESTDIR)/bin/apps/system/gpkg-worker
	ln -sf /bin/apps/system/gpkg $(DESTDIR)/bin/gpkg

clean:
	rm -rf $(OBJDIR) $(BINDIR)

.PHONY: all install clean FORCE
