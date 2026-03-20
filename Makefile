CXX = /usr/bin/g++
STRIP = /usr/bin/strip

PROJECT_ROOT ?= $(abspath $(CURDIR)/..)
GINIT_DIR = $(PROJECT_ROOT)/ginit
GLOBAL_SRC_DIR = $(PROJECT_ROOT)/src
ROOTFS ?=
TARGET_CXX_VERSION := $(shell find $(ROOTFS)/usr/include/c++ -maxdepth 1 -mindepth 1 -type d -printf '%f\n' 2>/dev/null | grep -E '^[0-9]+$$' | sort -V | tail -n1)

CXXFLAGS += -Wall -Wextra -O2 -I./src -I$(GINIT_DIR)/src -I$(GLOBAL_SRC_DIR)
ifneq ($(strip $(TARGET_CXX_VERSION)),)
CXXFLAGS += -nostdinc++
CXXFLAGS += -isystem $(ROOTFS)/usr/include/c++/$(TARGET_CXX_VERSION)
CXXFLAGS += -isystem $(ROOTFS)/usr/include/x86_64-linux-gnu/c++/$(TARGET_CXX_VERSION)
CXXFLAGS += -isystem $(ROOTFS)/usr/include/c++/$(TARGET_CXX_VERSION)/backward
endif
GPKG_LDFLAGS = -L$(GINIT_DIR)/lib -lgemcore -lssl -lcrypto -lz -lzstd -ldl -lpthread -lcrypt
WORKER_LDFLAGS = -lssl -lcrypto -lz -lzstd -ldl -lpthread
ifneq ($(strip $(TARGET_CXX_VERSION)),)
GPKG_LDFLAGS += $(ROOTFS)/lib64/ld-linux-x86-64.so.2
WORKER_LDFLAGS += $(ROOTFS)/lib64/ld-linux-x86-64.so.2
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
