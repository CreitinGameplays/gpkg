# GPKG: GeminiOS Package Manager

`gpkg` is GeminiOS's package manager. It handles repository metadata, dependency
resolution, package downloads, local installation/removal, and package worker
operations.

## Layout

- `src/gpkg.cpp`: main CLI, repository handling, dependency resolution, and installs
- `src/gpkg_worker.cpp`: privileged worker for file extraction, registration, and removal
- `Makefile`: standalone build/install entrypoint for the module

## Build

Build both binaries:

```bash
cd gpkg
make
```

Install into a rootfs:

```bash
cd gpkg
make install DESTDIR=/path/to/rootfs
```

Within the full GeminiOS build, `ports/geminios_complex/build.sh` is the single
integration point that compiles and installs `gpkg`.
