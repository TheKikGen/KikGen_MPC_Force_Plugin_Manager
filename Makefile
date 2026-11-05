# ============================================================================
# THE KIKGEN LABS - MAKEFILE FOR AKAI MPC LIVE / ONE / X / FORCE (ARMv7hf)
# ============================================================================

CXX_ARMV7   := arm-linux-gnueabihf-g++
CXX_AARCH64 := aarch64-linux-gnu-g++
CXX_NATIVE  := g++

CXXFLAGS    := -std=c++17 -O2 -Wall -Wextra -Wno-format-truncation -Iinclude -pthread
LDFLAGS     := -lasound -lpthread -ldl

SRCS        := src/tkgl_gfx.cpp src/tkgl_config.cpp src/tkgl_ui_toolkit.cpp src/main.cpp
TARGET      := mpc_plugin_manager_fb

.PHONY: all armv7 aarch64 native single clean

all: armv7

armv7:
	$(CXX_ARMV7) $(CXXFLAGS) -march=armv7-a -mfpu=neon $(SRCS) -o $(TARGET) $(LDFLAGS)

aarch64:
	$(CXX_AARCH64) $(CXXFLAGS) $(SRCS) -o $(TARGET)_arm64 $(LDFLAGS)

native:
	$(CXX_NATIVE) $(CXXFLAGS) $(SRCS) -o $(TARGET)_x86 $(LDFLAGS)

single:
	$(CXX_ARMV7) -std=c++17 -O2 -Wall -Wextra -Wno-format-truncation -pthread mpc_plugin_manager_fb.cpp -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) $(TARGET)_arm64 $(TARGET)_x86
