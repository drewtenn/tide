# tide

A competent indie C++ game engine.

## Requirements

- **macOS** (Apple Silicon) or **Linux** (x86_64, GCC 13+)
- **C++20** compiler — Apple Clang 16+, GCC 13+, or MSVC 19.36+
- **CMake** 3.27 or newer
- **Git**
- **vcpkg** (set up below)
- **Ninja** or **Make** (Make ships with Xcode CLT on macOS)

### macOS

```bash
xcode-select --install
brew install cmake ninja pkg-config
```

### Linux

```bash
sudo apt install build-essential cmake ninja-build pkg-config git curl zip unzip tar
```

## Setup

### 1. Install vcpkg

vcpkg is the C++ dependency manager this project uses. Clone it somewhere outside the repo:

```bash
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
```

### 2. Set `VCPKG_ROOT`

Add to your shell rc (`~/.zshrc` or `~/.bashrc`):

```bash
export VCPKG_ROOT="$HOME/vcpkg"
```

Then reload:

```bash
source ~/.zshrc   # or ~/.bashrc
```

Verify:

```bash
echo $VCPKG_ROOT                                  # should print the path
ls $VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake   # should exist
```

### 3. Clone and build

```bash
git clone <repo-url> tide
cd tide
make
```

The first build takes 5–15 minutes — vcpkg compiles all dependencies (glm, spdlog, fmt, glfw3, imgui, tracy, doctest) from source. Subsequent builds reuse the cache and finish in seconds.

## Building

This repo provides a Makefile wrapper around CMake for convenience:

| Command | What it does |
|---|---|
| `make` | Configure and build debug (default) |
| `make build` | Same as `make` |
| `make release` | Build RelWithDebInfo (optimized + debug symbols) |
| `make test` | Build and run tests |
| `make clean` | Delete the build directory |
| `make rebuild` | Clean and rebuild from scratch |
| `make configure` | Run CMake configure only |
| `make run-NAME` | Build and run `samples/NAME/NAME` |
| `make help` | List all targets |

Build output lands in `build/<preset>/` (e.g. `build/macos-debug/`).

### Without the Makefile

The Makefile is a wrapper around CMake presets. You can call CMake directly:

```bash
cmake --preset macos-debug              # configure
cmake --build --preset macos-debug -j   # build
ctest --preset macos-debug               # test
```

Available presets are defined in [CMakePresets.json](CMakePresets.json):
`macos-debug`, `macos-relwithdebinfo`, `linux-debug`, `linux-relwithdebinfo`, `windows-debug`, `windows-relwithdebinfo`.

## Build options

CMake options (pass with `-D<name>=<value>` if invoking CMake directly):

| Option | Default | Description |
|---|---|---|
| `TIDE_BUILD_SAMPLES` | `ON` | Build sample executables |
| `TIDE_BUILD_TESTS` | `ON` | Build unit tests |
| `TIDE_ENABLE_TRACY` | `ON` | Enable Tracy profiler instrumentation |

## Project layout

```
engine/         Engine modules (core, platform, rhi, renderer, audio, …)
samples/        Example applications
tests/          Unit tests (doctest)
cmake/          CMake helpers and vcpkg overlay ports
docs/           Architecture docs and ADRs
```

## Troubleshooting

### `Could not find a package configuration file provided by "glm"`

vcpkg isn't being invoked. Check that `VCPKG_ROOT` is set in your current shell (`echo $VCPKG_ROOT`). If you're invoking `cmake` directly without a preset, you must pass `-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`. The Makefile and presets handle this for you.

### `vcpkg install failed` with `not our ref`

The baseline commit in [vcpkg.json](vcpkg.json) doesn't exist in your local vcpkg checkout. Update vcpkg:

```bash
cd $VCPKG_ROOT && git pull && ./bootstrap-vcpkg.sh
```

### Stale build state after changing CMakeLists.txt

```bash
make clean && make
```

### Build produces no output

Expected during early development — most engine modules are header-only INTERFACE targets until source files land. `make` exits successfully with no compilation work to do.
