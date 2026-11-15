# ============================================================================
# THE KIKGEN LABS - MULTI-TARGET MAKEFILE
# Standalone Plugins Manager for Akai MPC & Force Devices
# Supported Architectures:
#   - armv7   : Akai MPC Live / Live II / One / One+ / X / Force (ARMv7hf, GLIBC <= 2.27, NEON)
#   - aarch64 : 64-bit ARM (ARMv8 / AArch64, GLIBC <= 2.27)
#   - native  : Host Desktop Linux (x86_64)
#   - single  : Amalgamated single-source build (mpc_plugin_manager_fb.cpp)
# ============================================================================

# Auto-detect Zig compiler
ZIG ?= $(shell which zig 2>/dev/null || echo "$$HOME/.local/bin/zig")

# Toolchain fallbacks
CXX_GNU_ARMV7   ?= arm-linux-gnueabihf-g++
CXX_GNU_AARCH64 ?= aarch64-linux-gnu-g++
CXX_NATIVE      ?= g++
STRIP_ARMV7     ?= arm-linux-gnueabihf-strip
STRIP_AARCH64   ?= aarch64-linux-gnu-strip
STRIP_NATIVE    ?= strip

# Common Flags
COMMON_CXXFLAGS := -std=c++17 -O3 -fPIE -pie -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -Wall -Wextra -Wno-format-truncation -Wno-psabi -Iinclude -pthread
COMMON_LDFLAGS  := -Llib -lasound -lpthread -ldl

# Source files
SRCS            := src/tkgl_gfx.cpp src/tkgl_config.cpp src/tkgl_ui_toolkit.cpp src/main.cpp
SINGLE_SRC      := mpc_plugin_manager_fb.cpp
TARGET          := mpc_plugin_manager_fb

.PHONY: all armv7 armv7-gnu aarch64 native single single-native clean check-glibc ensure-alsa test help

# Default target: Akai MPC & Force ARMv7 via Zig (GLIBC 2.27 clamped)
all: armv7

# Test binary with QEMU ARM
test: $(TARGET)
	@echo "==> Testing $(TARGET) (-v) under QEMU ARM..."
	qemu-arm-static -L /usr/arm-linux-gnueabihf -E LD_LIBRARY_PATH=lib ./$(TARGET) -v

ensure-alsa:
	@if [ ! -d "include/alsa" ]; then \
		if [ -d "/usr/include/alsa" ]; then \
			echo "==> Ensuring ALSA headers in include/alsa..."; \
			cp -r /usr/include/alsa include/; \
		fi; \
	fi

# ----------------------------------------------------------------------------
# 1. AKAI MPC / FORCE (ARMv7hf) - OPTIMIZED ZIG BUILD
# ----------------------------------------------------------------------------
armv7: ensure-alsa $(SRCS)
	@echo "==> Building $(TARGET) for Akai MPC / Force (ARMv7hf, GLIBC 2.27, NEON)..."
	$(ZIG) c++ -target arm-linux-gnueabihf.2.27 -mcpu=cortex_a17 -mfpu=neon-vfpv4 \
		$(COMMON_CXXFLAGS) $(SRCS) -o $(TARGET) $(COMMON_LDFLAGS)
	@if command -v $(STRIP_ARMV7) >/dev/null 2>&1; then \
		$(STRIP_ARMV7) -s $(TARGET); \
	fi
	@echo "[✓] Build complete: $(TARGET)"
	@$(MAKE) --no-print-directory check-glibc BIN=$(TARGET)

# ----------------------------------------------------------------------------
# 2. AKAI MPC / FORCE (ARMv7hf) - GNU CROSS-COMPILER FALLBACK
# ----------------------------------------------------------------------------
armv7-gnu: ensure-alsa $(SRCS)
	@echo "==> Building $(TARGET) using GNU $(CXX_GNU_ARMV7)..."
	$(CXX_GNU_ARMV7) $(COMMON_CXXFLAGS) -march=armv7-a -mfpu=neon $(SRCS) -o $(TARGET) $(COMMON_LDFLAGS)
	@if command -v $(STRIP_ARMV7) >/dev/null 2>&1; then \
		$(STRIP_ARMV7) -s $(TARGET); \
	fi
	@echo "[✓] Build complete: $(TARGET)"
	@$(MAKE) --no-print-directory check-glibc BIN=$(TARGET)

# ----------------------------------------------------------------------------
# 3. 64-BIT ARM (AArch64) - ZIG BUILD
# ----------------------------------------------------------------------------
aarch64: ensure-alsa $(SRCS)
	@echo "==> Building $(TARGET)_arm64 for AArch64 (GLIBC 2.27)..."
	$(ZIG) c++ -target aarch64-linux-gnu.2.27 -mcpu=cortex_a53 \
		$(COMMON_CXXFLAGS) $(SRCS) -o $(TARGET)_arm64 $(COMMON_LDFLAGS)
	@if command -v $(STRIP_AARCH64) >/dev/null 2>&1; then \
		$(STRIP_AARCH64) -s $(TARGET)_arm64; \
	fi
	@echo "[✓] Build complete: $(TARGET)_arm64"
	@$(MAKE) --no-print-directory check-glibc BIN=$(TARGET)_arm64

# ----------------------------------------------------------------------------
# 4. HOST DESKTOP LINUX (x86_64) - NATIVE BUILD
# ----------------------------------------------------------------------------
native: ensure-alsa $(SRCS)
	@echo "==> Building $(TARGET)_x86 for local host..."
	$(CXX_NATIVE) $(COMMON_CXXFLAGS) $(SRCS) -o $(TARGET)_x86 -lasound -lpthread -ldl
	@$(STRIP_NATIVE) -s $(TARGET)_x86 2>/dev/null || true
	@echo "[✓] Build complete: $(TARGET)_x86"

# ----------------------------------------------------------------------------
# 5. SINGLE-FILE AMALGAMATED BUILD (ARMv7 via Zig)
# ----------------------------------------------------------------------------
single: ensure-alsa $(SINGLE_SRC)
	@echo "==> Building amalgamated $(TARGET) from $(SINGLE_SRC)..."
	$(ZIG) c++ -target arm-linux-gnueabihf.2.27 -mcpu=cortex_a17 -mfpu=neon-vfpv4 \
		$(COMMON_CXXFLAGS) $(SINGLE_SRC) -o $(TARGET) $(COMMON_LDFLAGS)
	@if command -v $(STRIP_ARMV7) >/dev/null 2>&1; then \
		$(STRIP_ARMV7) -s $(TARGET); \
	fi
	@echo "[✓] Single-file build complete: $(TARGET)"
	@$(MAKE) --no-print-directory check-glibc BIN=$(TARGET)

# ----------------------------------------------------------------------------
# 6. SINGLE-FILE AMALGAMATED NATIVE BUILD (x86_64)
# ----------------------------------------------------------------------------
single-native: ensure-alsa $(SINGLE_SRC)
	@echo "==> Building amalgamated $(TARGET)_x86 from $(SINGLE_SRC)..."
	$(CXX_NATIVE) $(COMMON_CXXFLAGS) $(SINGLE_SRC) -o $(TARGET)_x86 -lasound -lpthread -ldl
	@$(STRIP_NATIVE) -s $(TARGET)_x86 2>/dev/null || true
	@echo "[✓] Single-file native build complete: $(TARGET)_x86"

# ----------------------------------------------------------------------------
# GLIBC DEPENDENCY AUDIT
# ----------------------------------------------------------------------------
check-glibc:
	@if [ -f "$(BIN)" ]; then \
		echo "--- GLIBC Compatibility Audit ($(BIN)) ---"; \
		MAX_C=$$(strings $(BIN) 2>/dev/null | grep -E '^GLIBC_[0-9.]+' | sort -uV | tail -n1); \
		MAX_CXX=$$(strings $(BIN) 2>/dev/null | grep -E '^GLIBCXX_[0-9.]+' | sort -uV | tail -n1); \
		echo "  Max GLIBC   : $${MAX_C:-None}"; \
		echo "  Max GLIBCXX : $${MAX_CXX:-None}"; \
		echo "  Target Architecture : $$(file $(BIN) | cut -d',' -f2)"; \
	fi

# ----------------------------------------------------------------------------
# CLEAN
# ----------------------------------------------------------------------------
clean:
	rm -f $(TARGET) $(TARGET)_arm64 $(TARGET)_x86 $(TARGET)_single *.o test_*
	@echo "[✓] Clean complete."

# ----------------------------------------------------------------------------
# HELP
# ----------------------------------------------------------------------------
help:
	@echo "Available build targets:"
	@echo "  make              - Build optimized ARMv7 binary for Akai MPC/Force (Zig, GLIBC <= 2.27)"
	@echo "  make armv7        - Same as make (optimized for MPC Live, One, X, Force)"
	@echo "  make test         - Run ARMv7 binary (-v) under QEMU ARM emulation"
	@echo "  make armv7-gnu    - Build for MPC using host arm-linux-gnueabihf-g++"
	@echo "  make aarch64      - Build 64-bit ARM binary (AArch64)"
	@echo "  make native       - Build native x86_64 binary for desktop development"
	@echo "  make single       - Build from single amalgamated mpc_plugin_manager_fb.cpp"
	@echo "  make single-native- Build native x86_64 from mpc_plugin_manager_fb.cpp"
	@echo "  make clean        - Remove built binaries and intermediate files"
