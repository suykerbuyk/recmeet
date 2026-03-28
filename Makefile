# recmeet — convenience wrapper around CMake + Ninja
#
# Usage:
#   make                          # show help (safe default)
#   make build                    # configure + build (Release)
#   make test                     # build + run unit tests
#   make install                  # build + install to PREFIX
#   make BUILD_TYPE=Debug build   # debug build
#   make RECMEET_USE_LLAMA=OFF build  # disable llama.cpp support
#   make help                     # list all targets

.DEFAULT_GOAL := help

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
PREFIX     ?= $(HOME)/.local
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
ifdef RECMEET_BUILD_WEB
CMAKE_OPTS += -DRECMEET_BUILD_WEB=$(RECMEET_BUILD_WEB)
endif

# ── Targets ─────────────────────────────────────────────────────────
.PHONY: build test integration benchmark install uninstall package-deb package-rpm package-arch clean coverage help daemon-start daemon-stop daemon-status

build:
	cmake -B $(BUILD_DIR) -G Ninja $(CMAKE_OPTS)
	ninja -C $(BUILD_DIR)
	cd tools && go build -o ../$(BUILD_DIR)/recmeet-mcp ./cmd/recmeet-mcp
	cd tools && go build -o ../$(BUILD_DIR)/recmeet-agent ./cmd/recmeet-agent

test:
	cmake -B $(BUILD_DIR) -G Ninja $(CMAKE_OPTS) -DRECMEET_BUILD_TESTS=ON
	ninja -C $(BUILD_DIR)
	./$(BUILD_DIR)/recmeet_tests "~[integration]~[benchmark]"
	cd tools && go test ./... -count=1

integration:
	cmake -B $(BUILD_DIR) -G Ninja $(CMAKE_OPTS) -DRECMEET_BUILD_TESTS=ON
	ninja -C $(BUILD_DIR)
	./$(BUILD_DIR)/recmeet_tests "[integration]"

benchmark:
	cmake -B $(BUILD_DIR) -G Ninja $(CMAKE_OPTS) -DRECMEET_BUILD_TESTS=ON
	ninja -C $(BUILD_DIR)
	./$(BUILD_DIR)/recmeet_tests "[benchmark]"

install: build
	DESTDIR=$(DESTDIR) cmake --install $(BUILD_DIR)
ifndef DESTDIR
	@echo ""
	@echo "--- Downloading default models ---"
	./$(BUILD_DIR)/recmeet --no-daemon --download-models || echo "Warning: model download failed (retry with: recmeet --download-models)"
	@echo ""
	@echo "--- Enabling recmeet-daemon ---"
	systemctl --user daemon-reload 2>/dev/null || true
	systemctl --user enable --now recmeet-daemon.service 2>/dev/null && echo "Daemon enabled and started." || echo "Warning: could not enable daemon (no systemd user session?)"
	@echo ""
	@echo "Install complete. Run 'recmeet --status' to verify."
endif

uninstall:
	@echo "--- Stopping recmeet services ---"
	-systemctl --user disable --now recmeet-tray.service 2>/dev/null || true
	-systemctl --user disable --now recmeet-daemon.service 2>/dev/null || true
	-systemctl --user disable --now recmeet-daemon.socket 2>/dev/null || true
	-pkill -f recmeet-tray 2>/dev/null || true
	-pkill -f recmeet-daemon 2>/dev/null || true
	systemctl --user daemon-reload 2>/dev/null || true
	@echo ""
	@echo "--- Removing installed files from $(DESTDIR)$(PREFIX) ---"
	rm -fv $(DESTDIR)$(PREFIX)/bin/recmeet
	rm -fv $(DESTDIR)$(PREFIX)/bin/recmeet-daemon
	rm -fv $(DESTDIR)$(PREFIX)/bin/recmeet-tray
	rm -fv $(DESTDIR)$(PREFIX)/bin/recmeet-web
	rm -fv $(DESTDIR)$(PREFIX)/share/applications/recmeet-tray.desktop
	rm -fv $(DESTDIR)$(PREFIX)/share/systemd/user/recmeet-daemon.service
	rm -fv $(DESTDIR)$(PREFIX)/share/systemd/user/recmeet-daemon.socket
	rm -fv $(DESTDIR)$(PREFIX)/share/systemd/user/recmeet-tray.service
	rm -fv $(DESTDIR)$(PREFIX)/share/systemd/user/recmeet-web.service
	rm -rfv $(DESTDIR)$(PREFIX)/share/recmeet/web/
	-rmdir $(DESTDIR)$(PREFIX)/share/recmeet 2>/dev/null || true
	rm -fv $(DESTDIR)$(PREFIX)/share/doc/recmeet/LICENSE
	rm -fv $(DESTDIR)$(PREFIX)/share/doc/recmeet/AUTHORS
	-rmdir $(DESTDIR)$(PREFIX)/share/doc/recmeet 2>/dev/null || true
	@echo ""
	@echo "--- Removing auto-downloaded models ---"
	rm -rfv $(HOME)/.local/share/recmeet/models/whisper/
	rm -rfv $(HOME)/.local/share/recmeet/models/sherpa/
	@echo ""
	@echo "Done."
	@echo "  Preserved: ~/.config/recmeet (config), ~/.local/share/recmeet/logs/ (logs)"
	@echo "  Preserved: ~/.local/share/recmeet/models/llama/ (user LLM models)"
	@echo "  Preserved: meetings/ (recordings)"

package-deb: build
	cd $(BUILD_DIR) && cpack -G DEB

package-rpm: build
	cd $(BUILD_DIR) && cpack -G RPM

package-arch:
	cd dist/arch && makepkg -sf

daemon-start: build
	./$(BUILD_DIR)/recmeet-daemon &
	@echo "Daemon started (PID $$!)"

daemon-stop:
	@./$(BUILD_DIR)/recmeet --stop 2>/dev/null || pkill -f recmeet-daemon || echo "Daemon not running"

daemon-status:
	@./$(BUILD_DIR)/recmeet --status

coverage:
	cd tools && go test ./... -coverprofile=coverage.out
	cd tools && go tool cover -func=coverage.out

clean:
	rm -f $(BUILD_DIR)/recmeet-mcp $(BUILD_DIR)/recmeet-agent
	rm -f tools/coverage.out
	rm -rf $(BUILD_DIR)
	rm -rf dist/arch/src/ dist/arch/pkg/
	rm -f dist/arch/*.zst
	git checkout dist/arch/PKGBUILD 2>/dev/null || true

help:
	@echo "recmeet build targets:"
	@echo ""
	@echo "  make build         Configure + build (Release)"
	@echo "  make test          Build + run unit tests"
	@echo "  make integration   Build + run integration tests"
	@echo "  make benchmark     Build + run benchmark tests"
	@echo "  make install       Build + install to PREFIX (default: ~/.local)"
	@echo "  make uninstall     Remove installed files from PREFIX"
	@echo "  make daemon-start  Build + start daemon in background"
	@echo "  make daemon-stop   Stop running daemon"
	@echo "  make daemon-status Query daemon status"
	@echo "  make package-deb   Build + create .deb package"
	@echo "  make package-rpm   Build + create .rpm package"
	@echo "  make package-arch  Build Arch package via makepkg"
	@echo "  make coverage      Run Go tests with coverage report"
	@echo "  make clean         Remove build + packaging artifacts"
	@echo "  make help          Show this message"
	@echo ""
	@echo "Quick start:  make build && make test"
	@echo ""
	@echo "Variables (override via make VAR=value):"
	@echo ""
	@echo "  BUILD_DIR          Build directory      (default: build)"
	@echo "  BUILD_TYPE         Release|Debug|...    (default: Release)"
	@echo "  PREFIX             Install prefix       (default: ~/.local)"
	@echo "  DESTDIR            Staging root         (default: empty)"
	@echo "  RECMEET_BUILD_TRAY ON|OFF               (default: ON)"
	@echo "  RECMEET_USE_LLAMA  ON|OFF               (default: ON)"
	@echo "  RECMEET_USE_SHERPA ON|OFF               (default: ON)"
	@echo "  RECMEET_USE_NOTIFY ON|OFF               (default: ON)"
	@echo "  RECMEET_BUILD_TESTS ON|OFF              (default: ON)"
	@echo "  RECMEET_BUILD_WEB  ON|OFF               (default: ON)"
