# Building and Testing recmeet

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

recmeet has three build options you can toggle at configure time:

```bash
# Disable the system tray applet (skips GTK3/AppIndicator dependency)
cmake -B build -G Ninja -DRECMEET_BUILD_TRAY=OFF

# Disable llama.cpp local summarization
cmake -B build -G Ninja -DRECMEET_USE_LLAMA=OFF

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
