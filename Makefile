CXX = /usr/bin/g++
STRIP = /usr/bin/strip

PROJECT_ROOT ?= $(abspath $(CURDIR)/..)
GINIT_DIR = $(PROJECT_ROOT)/ginit
GLOBAL_SRC_DIR = $(PROJECT_ROOT)/src

CXXFLAGS += -Wall -Wextra -O2 -I./src -I$(GINIT_DIR)/src -I$(GLOBAL_SRC_DIR)
GPKG_LDFLAGS = -L$(GINIT_DIR)/lib -lgemcore -lssl -lcrypto -lz -lzstd -ldl -lpthread -lcrypt
WORKER_LDFLAGS = -lssl -lcrypto -lz -lzstd -ldl -lpthread

SRCDIR = src
OBJDIR = obj
BINDIR = bin

TARGETS = $(BINDIR)/gpkg $(BINDIR)/gpkg-worker

all: $(BINDIR) $(OBJDIR) $(TARGETS)

$(BINDIR) $(OBJDIR):
	mkdir -p $@

$(BINDIR)/gpkg: $(SRCDIR)/gpkg.cpp
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
