# GPKG: GeminiOS Package Manager
MOVED TO GPKG-v2

`gpkg` is GeminiOS's package manager. It handles repository metadata, dependency
resolution, package downloads, and package operations across two backends:
Debian packages are installed natively through `dpkg`, while GeminiOS-native
`.gpkg` packages still use `gpkg-worker`.

## Layout

- `src/gpkg.cpp`: main CLI, repository handling, dependency resolution, and backend dispatch
- `src/gpkg_worker.cpp`: privileged worker for GeminiOS-native `.gpkg` extraction, registration, and removal
- `Makefile`: standalone build/install entrypoint for the module

## Build

Build both binaries:

```bash
cd gpkg
make -j"$(nproc)"
```

Install into a rootfs:

```bash
cd gpkg
make -j"$(nproc)" install DESTDIR=/path/to/rootfs
```

Within the full GeminiOS build, `ports/geminios_complex/build.sh` is the single
integration point that compiles and installs `gpkg`, and it now invokes the
module build with `-j"$(nproc)"`.
