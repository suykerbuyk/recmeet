# recmeet — convenience wrapper around CMake + Ninja
#
# Usage:
#   make                          # configure + build (Release)
#   make test                     # build + run unit tests
#   make install                  # build + install to PREFIX
#   make BUILD_TYPE=Debug         # debug build
#   make RECMEET_USE_LLAMA=OFF    # disable llama.cpp support
#   make help                     # list all targets

# ── Dependency checks ───────────────────────────────────────────────
CMAKE := $(shell command -v cmake 2>/dev/null)
NINJA := $(shell command -v ninja 2>/dev/null)

ifndef CMAKE
$(error cmake not found. Install: pacman -S cmake | apt install cmake | dnf install cmake)
endif
ifndef NINJA
$(error ninja not found. Install: pacman -S ninja | apt install ninja-build | dnf install ninja-build)
endif

# ── Overridable variables ───────────────────────────────────────────
BUILD_DIR  ?= build
BUILD_TYPE ?= Release
PREFIX     ?= /usr/local
DESTDIR    ?=

# ── CMake options accumulator ───────────────────────────────────────
CMAKE_OPTS := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_INSTALL_PREFIX=$(PREFIX)

ifdef RECMEET_BUILD_TRAY
CMAKE_OPTS += -DRECMEET_BUILD_TRAY=$(RECMEET_BUILD_TRAY)
endif
ifdef RECMEET_USE_LLAMA
CMAKE_OPTS += -DRECMEET_USE_LLAMA=$(RECMEET_USE_LLAMA)
endif
ifdef RECMEET_USE_SHERPA
CMAKE_OPTS += -DRECMEET_USE_SHERPA=$(RECMEET_USE_SHERPA)
endif
ifdef RECMEET_USE_NOTIFY
CMAKE_OPTS += -DRECMEET_USE_NOTIFY=$(RECMEET_USE_NOTIFY)
endif
ifdef RECMEET_BUILD_TESTS
CMAKE_OPTS += -DRECMEET_BUILD_TESTS=$(RECMEET_BUILD_TESTS)
endif

# ── Targets ─────────────────────────────────────────────────────────
.PHONY: all test benchmark install package-deb package-rpm package-arch clean help

all:
	cmake -B $(BUILD_DIR) -G Ninja $(CMAKE_OPTS)
	ninja -C $(BUILD_DIR)

test:
	cmake -B $(BUILD_DIR) -G Ninja $(CMAKE_OPTS) -DRECMEET_BUILD_TESTS=ON
	ninja -C $(BUILD_DIR)
	./$(BUILD_DIR)/recmeet_tests "~[integration]~[benchmark]"

benchmark:
	cmake -B $(BUILD_DIR) -G Ninja $(CMAKE_OPTS) -DRECMEET_BUILD_TESTS=ON
	ninja -C $(BUILD_DIR)
	./$(BUILD_DIR)/recmeet_tests "[benchmark]"

install: all
	DESTDIR=$(DESTDIR) cmake --install $(BUILD_DIR)

package-deb: all
	cd $(BUILD_DIR) && cpack -G DEB

package-rpm: all
	cd $(BUILD_DIR) && cpack -G RPM

package-arch:
	cd dist/arch && makepkg -sf

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "recmeet build targets:"
	@echo ""
	@echo "  make              Build (Release)"
	@echo "  make test         Build + run unit tests"
	@echo "  make benchmark    Build + run benchmark tests"
	@echo "  make install      Build + install to PREFIX (default: /usr/local)"
	@echo "  make package-deb  Build + create .deb package"
	@echo "  make package-rpm  Build + create .rpm package"
	@echo "  make package-arch Build Arch package via makepkg"
	@echo "  make clean        Remove build directory"
	@echo "  make help         Show this message"
	@echo ""
	@echo "Variables (override via make VAR=value):"
	@echo ""
	@echo "  BUILD_DIR          Build directory      (default: build)"
	@echo "  BUILD_TYPE         Release|Debug|...    (default: Release)"
	@echo "  PREFIX             Install prefix       (default: /usr/local)"
	@echo "  DESTDIR            Staging root         (default: empty)"
	@echo "  RECMEET_BUILD_TRAY ON|OFF               (default: ON)"
	@echo "  RECMEET_USE_LLAMA  ON|OFF               (default: ON)"
	@echo "  RECMEET_USE_SHERPA ON|OFF               (default: ON)"
	@echo "  RECMEET_USE_NOTIFY ON|OFF               (default: ON)"
	@echo "  RECMEET_BUILD_TESTS ON|OFF              (default: ON)"
