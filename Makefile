# ============================================================================
# THE KIKGEN LABS - MAKEFILE FOR AKAI MPC LIVE / ONE / X / FORCE (ARMv7hf)
# ============================================================================

CXX_ARMV7   ?= arm-linux-gnueabihf-g++
CC_ARMV7    ?= arm-linux-gnueabihf-gcc
CXX_AARCH64 ?= aarch64-linux-gnu-g++
CXX_NATIVE  ?= g++
STRIP_ARMV7 ?= arm-linux-gnueabihf-strip

CXXFLAGS    := -std=c++17 -O2 -Wall -Wextra -Wno-format-truncation -Iinclude -pthread
LDFLAGS     := -lasound -lpthread -ldl

SRCS        := src/tkgl_gfx.cpp src/tkgl_config.cpp src/tkgl_ui_toolkit.cpp src/main.cpp
TARGET      := mpc_plugin_manager_fb

.PHONY: all armv7 patched aarch64 native single clean check-glibc

all: armv7

armv7:
	$(CXX_ARMV7) $(CXXFLAGS) -march=armv7-a -mfpu=neon $(SRCS) -o $(TARGET) $(LDFLAGS)
	@$(MAKE) --no-print-directory check-glibc BIN=$(TARGET)

# Patched target: specific case to handle GLIBC conflicts
# target_libs must contain all /usr/lib /lib /lib32 files from the targeted device

csu_stubs.c:
	@printf "void __libc_csu_init(int argc, char **argv, char **envp) {\n    (void)argc; (void)argv; (void)envp;\n}\nvoid __libc_csu_fini(void) {}\n" > $@

csu_stubs.o: csu_stubs.c
	$(CC_ARMV7) -c $< -o $@

patched: $(SRCS) csu_stubs.o
	$(CXX_ARMV7) $(CXXFLAGS) -march=armv7-a -mfpu=neon $(SRCS) csu_stubs.o -o $(TARGET) \
		-L./target_libs -Wl,-rpath-link=./target_libs $(LDFLAGS)
	$(STRIP_ARMV7) $(TARGET)
	@$(MAKE) --no-print-directory check-glibc BIN=$(TARGET)

check-glibc:
	@echo "--- Target binary version dependencies ($(BIN)) ---"
	@MAX_C=$$(strings $(BIN) 2>/dev/null | grep -E '^GLIBC_[0-9.]+' | sort -uV | tail -n1); \
	MAX_CXX=$$(strings $(BIN) 2>/dev/null | grep -E '^GLIBCXX_[0-9.]+' | sort -uV | tail -n1); \
	echo "  Max GLIBC   : $${MAX_C:-None}"; \
	echo "  Max GLIBCXX : $${MAX_CXX:-None}"

aarch64:
	$(CXX_AARCH64) $(CXXFLAGS) $(SRCS) -o $(TARGET)_arm64 $(LDFLAGS)

native:
	$(CXX_NATIVE) $(CXXFLAGS) $(SRCS) -o $(TARGET)_x86 $(LDFLAGS)

single:
	$(CXX_ARMV7) -std=c++17 -O2 -Wall -Wextra -Wno-format-truncation -pthread mpc_plugin_manager_fb.cpp -o $(TARGET)

clean:
	rm -f $(TARGET) $(TARGET)_arm64 $(TARGET)_x86 csu_stubs.c csu_stubs.o

