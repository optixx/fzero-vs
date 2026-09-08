# Building F-Zero VS

The root Makefile organizes Ares acquisition, patching, compilation, server
compilation, tests, and desktop startup. The C11 server and Ares integration are
implemented in `server/`, `client/`, `shared/` and `patches/ares/`.

## Requirements

macOS: Xcode or its command-line tools, Git, Python 3.9+, CMake 3.28+, Ninja,
and Make. The local launcher/helper currently target macOS and Linux (POSIX).
Install missing build tools yourself, for example `brew install cmake ninja`.
No Makefile target installs global packages or changes system configuration.

The Ares commands follow the upstream [macOS build instructions](https://github.com/ares-emulator/ares/wiki/Build-Instructions-For-macOS).
Ares configuration may download its pinned, hash-verified third-party dependency
bundle into `vendor/ares/.deps`. On Linux, install the prerequisites documented
in the [upstream Linux instructions](https://github.com/ares-emulator/ares/wiki/Build-Instructions-For-Linux).

## Commands

```sh
make help
make fetch                    # official Ares source, exact pinned commit
make patch                    # fetch, then apply the ordered patch series
make ares JOBS=8               # patch, configure with Ninja, build desktop-ui
make server                   # C11 server -> build/server/fzvs-server
make all                      # both components
make test                     # local tests; no downloads or real ROM required
make dry-run-2p                # inspect startup; also dry-run-4p
make test-2p RUN_ARGS='--duration 60'  # build both, then launch; also test-4p
make clean                    # clean compiled outputs, retain source/config/logs
```

Defaults: SNES-only Ares, `RelWithDebInfo`, native host architecture, four build
jobs, CHD and optional shader support disabled. Ares's own dependency discovery
is retained. Override configuration using `BUILD_TYPE=Debug`,
`ARES_CMAKE_ARGS='-DOPTION=VALUE'`, or `SERVER_CMAKE_ARGS='-DFZVS_SANITIZERS=ON'`.
With Ares Debug builds, upstream may require the Xcode Metal compiler tools.
Use `CMAKE=/path/to/cmake` and `PYTHON=/path/to/python3` as needed.

Ares outputs its macOS bundle at `build/ares/desktop-ui/ares.app`; the Makefile
creates `build/bin/ares` as an executable launcher. It uses `exec` to run the
actual binary inside the app bundle so macOS finds its Metal shaders, preserving
arguments, signals and the caller’s working directory. On Linux it runs
`build/ares/desktop-ui/ares`.
See [local-testing.md](local-testing.md) for input assignment and runtime flags.

## Pinning and patches

`config/ares.json` pins the official repository, release tag, and exact commit.
The fetch helper checks the commit even on repeat runs and does not pull the
latest upstream changes. Existing checkouts are reused without network access.
An interrupted clone is retained for inspection, never automatically deleted.

Put ordinary `git diff` patches in `patches/ares/` and list their filenames in
`patches/ares/series`, one per line in dependency order. Blank lines and lines
starting with `#` are ignored. Patches are checked before application; their
hashes are recorded inside the vendor checkout's `.git` directory. Repeating
`make patch` with the same series preserves local edits and skips reapplication.

To develop patches, edit `vendor/ares`, inspect the diff, and export changes as
patch files (include new files in the Git index first). Already applied patches
are staged, so `git diff HEAD` includes both the series and subsequent edits.
Before changing the pin or replacing an applied series, preserve your work and
move the old checkout aside, then fetch/apply into a fresh checkout. The helper
never hard-resets or cleans source changes. Do not modify or reset the patch
marker independently of the checkout. Avoid simultaneous builds and source edits.

## Server source layout

`server/src/server.c` implements the reusable, bounded C11 UDP server core.
`server/include/fzvs_server.h` exposes create/poll/status/events/close/destroy.
`server/src/main.c` is the dedicated executable wrapper for CLI flags, signals,
readiness, stdout and JSON logs. Ares links `fzvs-server-core` directly; it does
not launch the standalone executable. `client/fzvs_session.cpp` owns the embedded
worker, asynchronous DNS, discovery, lifecycle and bounded event snapshots.
`shared/fzvs_protocol.h` supplies the explicitly encoded wire format to both the
C server and C++ client. `client/fzvs_client.cpp` owns the game state machine and
runtime patch restoration; the Ares patch supplies memory and frontend adapters.

`make test` uses local UDP sockets and ASan/UBSan for the codec and client tests.
It needs no ROM. Desktop tests require the supported US original-revision ROM.
See [protocol.md](protocol.md) for synchronization, patch addresses and limits.
