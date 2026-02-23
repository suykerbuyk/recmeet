# Building, Testing, and Packaging recmeet

## How the build system works

recmeet uses a two-stage build process: **CMake** generates build instructions, then **Ninja** executes them.

### CMake vs Ninja — what each one does

**CMake** is a *build system generator*. It reads `CMakeLists.txt` and produces
build files for another tool to consume. CMake itself does not compile anything.
Think of it as a translator that converts our project description into a recipe
a build tool can follow.

**Ninja** is a *build tool* (like Make, but faster). It reads the recipe CMake
generated and runs the actual compiler/linker commands. Ninja's only job is to
figure out which files changed, then run the minimum set of compile commands to
bring everything up to date.

The flow looks like this:

```
CMakeLists.txt          (you write this — declares targets, sources, dependencies)
       │
       ▼
    cmake               (reads CMakeLists.txt, finds compilers/libraries,
       │                 writes build.ninja into the build/ directory)
       ▼
  build/build.ninja     (generated — contains every compile/link command)
       │
       ▼
    ninja               (reads build.ninja, runs gcc/g++ to compile,
                         only rebuilds files that changed)
```

You only re-run CMake when `CMakeLists.txt` changes (new files, new options,
new dependencies). Day-to-day, you just run `ninja` — it's instant if nothing
changed, and recompiles only what's needed otherwise.

### Why `-B build` and `-C build`?

CMake uses **out-of-source builds**: all generated files go into a separate
directory (we use `build/`) so they don't clutter the source tree.

- `cmake -B build` means "put all generated files in `build/`"
- `ninja -C build` means "run ninja inside the `build/` directory"

The `build/` directory is disposable — you can delete it entirely and
regenerate from scratch.

---

## Quick start

### First-time setup

```bash
# 1. Configure (generate build files) — only needed once, or when CMakeLists.txt changes
cmake -B build -G Ninja

# 2. Build everything
ninja -C build
```

That produces three binaries inside `build/`:

| Binary | What it is |
|---|---|
| `build/recmeet` | CLI tool |
| `build/recmeet-tray` | System tray applet |
| `build/recmeet_tests` | Test runner |

### Daily workflow

After the first-time setup, you typically just run:

```bash
ninja -C build                          # rebuild (only recompiles changed files)
./build/recmeet_tests "~[integration]"  # run unit tests
```

Ninja is smart about dependencies — if you edit `src/obsidian.cpp`, it
recompiles only that file and re-links the binaries that use it. A no-op build
(nothing changed) takes milliseconds.

---

## Running tests

### Run all unit tests

```bash
./build/recmeet_tests "~[integration]"
```

The `"~[integration]"` filter *excludes* tests tagged `[integration]` because
those need a live PipeWire/PulseAudio session. This is the command you'll use
most often.

### Run all tests (including integration)

```bash
./build/recmeet_tests
```

Only works when PipeWire is running (i.e., on your desktop, not in a headless
CI environment).

### Run tests for a specific module

Each test file tags its tests. Pass the tag to run just that group:

```bash
./build/recmeet_tests "[cli]"              # CLI argument parsing
./build/recmeet_tests "[transcribe]"       # Transcript formatting
./build/recmeet_tests "[http_client]"      # HTTP client (file:// URIs)
./build/recmeet_tests "[model_manager]"    # Model cache logic
./build/recmeet_tests "[summarize]"        # Prompt building
./build/recmeet_tests "[pipeline]"         # Pipeline helpers
./build/recmeet_tests "[obsidian]"         # Obsidian note output
./build/recmeet_tests "[config]"           # Config loading/saving
./build/recmeet_tests "[audio_file]"       # WAV read/write
./build/recmeet_tests "[audio_mixer]"      # PCM mixing
./build/recmeet_tests "[json]"             # JSON escape/extract
./build/recmeet_tests "[util]"             # Utility functions
./build/recmeet_tests "[device_enum]"      # PulseAudio device listing (integration)
```

### Run a single test by name

```bash
./build/recmeet_tests "parse_cli: --model sets whisper_model"
```

### List all tests without running them

```bash
./build/recmeet_tests --list-tests 2>/dev/null
```

### Build and test in one command

```bash
ninja -C build && ./build/recmeet_tests "~[integration]"
```

The `&&` ensures tests only run if the build succeeded.

---

## CMake options

recmeet has four build options you can toggle at configure time. All features
default to ON and the build type defaults to Release, so a plain
`cmake -B build -G Ninja` gives you a fully-featured optimized build.

```bash
# Disable the system tray applet (skips GTK3/AppIndicator dependency)
cmake -B build -G Ninja -DRECMEET_BUILD_TRAY=OFF

# Disable llama.cpp local summarization
cmake -B build -G Ninja -DRECMEET_USE_LLAMA=OFF

# Disable sherpa-onnx speaker diarization (saves build time)
cmake -B build -G Ninja -DRECMEET_USE_SHERPA=OFF

# Disable building tests (skips Catch2 download)
cmake -B build -G Ninja -DRECMEET_BUILD_TESTS=OFF

# Combine multiple options
cmake -B build -G Ninja -DRECMEET_BUILD_TRAY=OFF -DRECMEET_USE_LLAMA=OFF
```

After changing options, rebuild:

```bash
ninja -C build
```

---

## What the build targets are

`CMakeLists.txt` defines four targets and how they relate:

```
recmeet_core  (static library — all the pipeline logic)
    │
    ├── recmeet        (CLI binary = main.cpp + recmeet_core)
    ├── recmeet-tray   (tray binary = tray.cpp + recmeet_core + GTK3)
    └── recmeet_tests  (test binary = tests/*.cpp + recmeet_core + Catch2)
```

`recmeet_core` is compiled once as a static library (`.a` file), then linked
into all three binaries. This means changing a test file only recompiles that
test file and re-links `recmeet_tests` — it doesn't recompile or re-link the
CLI or tray binaries.

---

## Debug and release builds

CMake supports several build types that control optimization and debug info.
The build type defaults to `Release` when none is specified, so a plain
`cmake -B build -G Ninja` already produces optimized binaries.

### Release (default)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Enables `-O3` optimization, no debug assertions. This is what you get by
default and what you'd ship.

### Debug

```bash
cmake -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build-debug
```

Enables `-g` debug symbols and `-O0` (no optimization). Use this when you need
to step through code with `gdb` or `lldb`. Binaries will be larger and slower.

Note: using a separate directory (`build-debug`) lets you keep both a release
and debug build around simultaneously. You can name it anything.

### RelWithDebInfo

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build
```

Enables `-O2` optimization *and* debug symbols. Good middle ground — fast
binaries that you can still attach a debugger to. Useful for profiling.

### Stripping binaries

Release binaries include symbol tables by default (`not stripped`). To reduce
binary size for distribution:

```bash
strip build/recmeet build/recmeet-tray
```

### Summary

| Build type | Optimization | Debug symbols | Use case |
|---|---|---|---|
| `Release` | `-O3` | No | Default, production |
| `Debug` | `-O0` | Yes (`-g`) | Debugging with gdb/lldb |
| `RelWithDebInfo` | `-O2` | Yes (`-g`) | Profiling, crash analysis |
| `MinSizeRel` | `-Os` | No | Smallest binary |

---

## Installing

After building, you can install recmeet into a prefix on the filesystem using
CMake's install step. The install rules use `GNUInstallDirs` for portable paths
and only install recmeet's own files — the vendored libraries (whisper.cpp,
llama.cpp) are statically linked and excluded from the install tree.

### Install to the default prefix (`/usr/local`)

```bash
sudo cmake --install build
```

### Install to a custom prefix (no root required)

```bash
cmake --install build --prefix /tmp/test-install
```

### What gets installed

| File | Destination |
|---|---|
| `recmeet` | `<prefix>/bin/` |
| `recmeet-tray` | `<prefix>/bin/` (only when `RECMEET_BUILD_TRAY=ON`) |
| `recmeet-tray.desktop` | `<prefix>/share/applications/` (only when `RECMEET_BUILD_TRAY=ON`) |
| `LICENSE-MIT` | `<prefix>/share/doc/recmeet/` |
| `LICENSE-APACHE` | `<prefix>/share/doc/recmeet/` |
| `AUTHORS` | `<prefix>/share/doc/recmeet/` |

The `.desktop` file registers `recmeet-tray` with freedesktop-compliant desktop
environments so it appears in application menus and can be configured for
autostart.

---

## Packaging

recmeet supports building distributable packages for Arch Linux and Debian/Ubuntu.
Both packaging paths use the same CMake install rules, so the installed file
layout is identical regardless of how you install.

### Arch Linux

A `PKGBUILD` is provided in `dist/arch/`. It builds from the local git repo
and derives the package version from `git describe`.

```bash
cd dist/arch
makepkg -sf          # build the package
sudo pacman -U recmeet-git-*.pkg.tar.*   # install it
```

The `-s` flag auto-installs missing build dependencies. The `-f` flag forces
a rebuild if a package file already exists.

The PKGBUILD uses `git+file://` to clone from the local repo, so your working
tree doesn't need to be clean — but only committed changes are included.
To publish to the AUR, change the `source` URL to point at a remote git repo.

Build options: the PKGBUILD only overrides `-DRECMEET_BUILD_TESTS=OFF` — all
other options inherit the CMake defaults (Release, all features ON). Edit the
`build()` function to change these.

### Debian / Ubuntu

CMake's CPack module can produce `.deb` packages directly from the build
directory. No separate packaging files are needed.

```bash
# 1. Configure a release build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# 2. Build
ninja -C build

# 3. Generate the .deb
cd build && cpack -G DEB
```

This produces a file like `recmeet_0.1.0_amd64.deb` in the build directory.
Install it with:

```bash
sudo dpkg -i recmeet_0.1.0_amd64.deb
sudo apt-get install -f   # resolve any missing dependencies
```

The `.deb` is configured with:

- **`CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON`** — automatically detects shared library
  dependencies by running `dpkg-shlibdeps` on the installed binaries.
- **`CPACK_STRIP_FILES ON`** — strips debug symbols, significantly reducing
  package size.
- **`CPACK_DEBIAN_FILE_NAME DEB-DEFAULT`** — uses the standard Debian naming
  convention (`name_version_arch.deb`).

### Fedora / RHEL

CPack can also produce `.rpm` packages. Build on a Fedora or RHEL system
where `rpmbuild` is available.

```bash
# 1. Configure a release build
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# 2. Build
ninja -C build

# 3. Generate the .rpm
cd build && cpack -G RPM
```

This produces a file like `recmeet-0.1.0-1.x86_64.rpm` in the build directory.
Install it with:

```bash
sudo dnf install ./recmeet-0.1.0-1.x86_64.rpm
```

The `.rpm` is configured with:

- **`CPACK_RPM_PACKAGE_AUTOREQ ON`** — automatically detects shared library
  dependencies (the RPM equivalent of `SHLIBDEPS`).
- **`CPACK_RPM_FILE_NAME RPM-DEFAULT`** — uses the standard RPM naming
  convention (`name-version-release.arch.rpm`).

### Cross-distro note

Packages should be built on their target distro. A `.deb` built on Ubuntu
won't work on Fedora, and a `.rpm` built on Fedora won't work on Debian.
The auto-dependency detection (`SHLIBDEPS` for DEB, `AUTOREQ` for RPM) maps
ELF NEEDED entries to the correct package names for the distro where the build
runs.

### Build dependencies by distro

| Dependency | Arch | Debian/Ubuntu | Fedora/RHEL |
|---|---|---|---|
| PipeWire | `pipewire` | `libpipewire-0.3-dev` | `pipewire-devel` |
| PulseAudio | `libpulse` | `libpulse-dev` | `pulseaudio-libs-devel` |
| libsndfile | `libsndfile` | `libsndfile1-dev` | `libsndfile-devel` |
| curl | `curl` | `libcurl4-openssl-dev` | `libcurl-devel` |
| libnotify | `libnotify` | `libnotify-dev` | `libnotify-devel` |
| AppIndicator | `libayatana-appindicator` | `libayatana-appindicator3-dev` | `libayatana-appindicator-gtk3-devel` |
| GTK3 | `gtk3` | `libgtk-3-dev` | `gtk3-devel` |

---

## Clean build

If something seems wrong or you want to start fresh:

```bash
rm -rf build
cmake -B build -G Ninja
ninja -C build
```

---

## Common problems

**"ninja: error: loading 'build.ninja'"** — You haven't run `cmake -B build -G Ninja` yet, or the `build/` directory was deleted.

**CMake can't find a library** — Install the missing system dependency. On Arch:
```bash
sudo pacman -S pipewire libpulse libsndfile curl libnotify libayatana-appindicator gtk3
```

**Tests fail with "[integration]" errors** — Those tests need a running
PipeWire session. Use `"~[integration]"` to skip them.

**Build is slow on first run** — The first build compiles whisper.cpp and
llama.cpp from source (~100+ files). Subsequent builds only recompile what
changed. Consider installing `ccache` to cache object files across clean builds:
```bash
sudo pacman -S ccache
```
