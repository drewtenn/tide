# Convenience wrapper around CMake + vcpkg presets.
# Real build system is CMake; this just shortens the common commands.

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
else ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
else
    $(error Unsupported platform: $(UNAME_S))
endif

PRESET_DEBUG   := $(PLATFORM)-debug
PRESET_RELEASE := $(PLATFORM)-relwithdebinfo
BUILD_DIR      := build
JOBS           := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)

.PHONY: build debug release configure configure-release rebuild clean test test-release run-% help

build: debug

debug: configure
	cmake --build --preset $(PRESET_DEBUG) -j $(JOBS)

release: configure-release
	cmake --build --preset $(PRESET_RELEASE) -j $(JOBS)

configure:
	cmake --preset $(PRESET_DEBUG)

configure-release:
	cmake --preset $(PRESET_RELEASE)

rebuild: clean debug

clean:
	rm -rf $(BUILD_DIR)

test: debug
	ctest --preset $(PRESET_DEBUG) --output-on-failure

test-release: release
	ctest --preset $(PRESET_RELEASE) --output-on-failure

run-%: debug
	cd $(BUILD_DIR)/$(PRESET_DEBUG)/samples/$* && ./$*

help:
	@echo "Targets:"
	@echo "  make            - configure (if needed) and build debug"
	@echo "  make debug      - same as 'make build'"
	@echo "  make release    - build RelWithDebInfo"
	@echo "  make configure  - run cmake configure only (debug)"
	@echo "  make rebuild    - wipe build dir and rebuild"
	@echo "  make clean      - delete build directory"
	@echo "  make test       - build and run tests (debug)"
	@echo "  make run-NAME   - build and run samples/NAME/NAME"
