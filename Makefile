CXX = /usr/bin/g++
STRIP = /usr/bin/strip

PROJECT_ROOT ?= $(abspath $(CURDIR)/..)
GINIT_DIR = $(PROJECT_ROOT)/ginit
GLOBAL_SRC_DIR = $(PROJECT_ROOT)/src
SYS_INFO_HEADER ?= $(GLOBAL_SRC_DIR)/sys_info.h
GPKG_VERSION_HELPER ?= $(PROJECT_ROOT)/tools/gpkg_version.py
ROOTFS ?=
TARGET_CXX_VERSION := $(shell find $(ROOTFS)/usr/include/c++ -maxdepth 1 -mindepth 1 -type d -printf '%f\n' 2>/dev/null | grep -E '^[0-9]+$$' | sort -V | tail -n1)
GPKG_VERSION ?=
GPKG_CODENAME ?=
LZMA_STATIC := $(firstword \
	$(wildcard $(ROOTFS)/usr/lib/x86_64-linux-gnu/liblzma.a) \
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
ifneq ($(strip $(TARGET_CXX_VERSION)),)
CXXFLAGS += -nostdinc++
CXXFLAGS += -isystem $(ROOTFS)/usr/include/c++/$(TARGET_CXX_VERSION)
CXXFLAGS += -isystem $(ROOTFS)/usr/include/x86_64-linux-gnu/c++/$(TARGET_CXX_VERSION)
CXXFLAGS += -isystem $(ROOTFS)/usr/include/c++/$(TARGET_CXX_VERSION)/backward
endif
GPKG_LDFLAGS = -L$(GINIT_DIR)/lib -lgemcore -lssl -lcrypto -lz -lzstd -ldl -lpthread -lcrypt
ifeq ($(strip $(LZMA_STATIC)),)
GPKG_LDFLAGS += -llzma
else
GPKG_LDFLAGS += $(LZMA_STATIC)
endif
WORKER_LDFLAGS = -lssl -lcrypto -lz -lzstd -ldl -lpthread
ifeq ($(strip $(LZMA_STATIC)),)
WORKER_LDFLAGS += -llzma
else
WORKER_LDFLAGS += $(LZMA_STATIC)
endif
ifneq ($(strip $(TARGET_CXX_VERSION)),)
GPKG_LDFLAGS += $(ROOTFS)/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
WORKER_LDFLAGS += $(ROOTFS)/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
endif

SRCDIR = src
OBJDIR = obj
BINDIR = bin
GPKG_FRAGMENTS = $(wildcard $(SRCDIR)/*.ipp)

TARGETS = $(BINDIR)/gpkg $(BINDIR)/gpkg-worker

all: $(BINDIR) $(OBJDIR) $(TARGETS)

$(BINDIR) $(OBJDIR):
	mkdir -p $@

$(BINDIR)/gpkg: $(SRCDIR)/gpkg.cpp $(GPKG_FRAGMENTS)
	$(CXX) $(CXXFLAGS) -o $@ $< $(GPKG_LDFLAGS)
	$(STRIP) $@

$(BINDIR)/gpkg-worker: $(SRCDIR)/gpkg_worker.cpp
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

.PHONY: all install clean
