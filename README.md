# The KikGen Labs — MPC & Force Standalone Plugin Manager (Release V1.1.4 — v1.1.4)

Standalone VST2 / Addin / Patch manager for **Akai MPC Live / Live II / One / One+ / X / Key 37 / Key 61 / Force** written in native **C++17** (Linux Framebuffer `/dev/fb0`, ALSA RawMIDI & `evdev` Touchscreen, zero external build dependencies).

- **Release Tag**: `V1.1.4` (`v1.1.4`)
- **Key Enhancements in V1.1.3**:
  - **Online & Local Modes (Toggle)**: Tap the `[ONLINE]` / `[LOCAL]` button in the top bar to switch between online catalog browsing and 100% offline local operation.
  - **Offline Local Refresh**: In `LOCAL` mode, pressing `[REFRESH]` updates the local UI and installation state without pinging or requiring internet access.
  - **Binary Mirroring / Caching (`CACHE ALL` / `CACHE ZIP`)**: In `ONLINE` mode, download and store complete plugin archives (`plugins/<id>/<id>.zip`) directly on your external USB drive.
  - **Offline Direct Installation**: If a plugin binary is present locally on disk (`isCached`), it can be installed immediately offline without downloading.
  - **Binary Archive Purge (`PURGE ZIP`)**: Safely remove the local `.zip` archive to free USB storage without affecting already-installed `/Synth` files.
  - **Safe MPC.settings XML Handling**: strictly safeguards existing user `<Location>` entries in `<SynthContentLocations>`.

---
<img width="941" height="584" alt="image" src="https://github.com/user-attachments/assets/9f419a45-d5d2-4f16-bcf0-c20d69b04984" />

## 1. Package Architecture & Directory Tree

```text
KikGen_MPC_Force_Plugin_Manager/
├── README.md                     # Complete documentation (this file)
├── Makefile                      # Cross-compilation Makefile (ARMv7hf, AArch64, x86_64, Single-File)
├── mpc_plugins.conf              # Configuration & local catalog file (JSON + code blocks, ping_host)
├── mpc_plugin_manager_fb.cpp     # Amalgamated single-file C++17 source build
├── include/
│   ├── tkgl_gfx.hpp              # 2D Framebuffer graphics engine (anti-aliased SDF, EPX font, PNG/PPM decoder)
│   ├── tkgl_config.hpp           # mpc_plugins.conf parser, online catalog client, verifier & installer
│   └── tkgl_ui_toolkit.hpp       # Touch & MIDI UI toolkit (List view, Detail view, popups, screensaver)
├── src/
│   ├── tkgl_gfx.cpp              # /dev/fb0 Framebuffer rendering implementation
│   ├── tkgl_config.cpp           # Path resolution (PLG_MAN_DIR), libcurl IPv4 downloader, ping_host check & MPC.settings engine
│   ├── tkgl_ui_toolkit.cpp       # Plugin cards, Detail view (Install/Reinstall/Uninstall/Repair) & live logs
│   └── main.cpp                  # Entry point, ALSA Private MIDI & evdev touchscreen threads
├── plugins/                      # Local catalog cache (tkgl_catalog.json) and per-plugin assets
│   └── <plugin-id>/
│       ├── thumb.png             # Downloaded plugin screenshot thumbnail (fetched via libcurl IPv4 on REFRESH if missing)
│       ├── manifest.json         # Plugin metadata
│       ├── install.sh            # Standalone installation script
│       └── uninstall.sh          # Standalone uninstallation script
└── Synths/                       # Default local directory for VST plugin installations
```

---

## 2. Compilation (ARMv7hf Cross-Compilation)

### Prerequisites
On your Linux development machine (Debian/Ubuntu) or build environment:
```bash
sudo apt-get update
sudo apt-get install -y g++-arm-linux-gnueabihf libasound2-dev
```

### `make` Targets
- **Standard modular build for MPC / Force (ARMv7hf + NEON)**:
  ```bash
  make
  # or: make armv7
  ```
- **Single-file amalgamated build (`mpc_plugin_manager_fb.cpp`)**:
  ```bash
  make single
  ```
- **Clean build artifacts**:
  ```bash
  make clean
  ```

The compiled binary is named `mpc_plugin_manager_fb`.

---

## 3. Running on MPC / Force & `PLG_MAN_DIR` Support (`noexec` USB Drives)

### Startup Prompt & Version Identification
On launch (or with `--version` / `-v`), the binary outputs its identification banner with the compilation timestamp:
```text
The KikGen Labs - Plugins Manager for Akai MPC and Force Devices
Version: V1.1.2 (v1.1.2) - Compiled on Nov  7 2026 02:53:01
```

### Case A — Direct Execution (`ext4` partition or executable storage)
If the manager directory resides on a partition with execution permissions:
```bash
./mpc_plugin_manager_fb
```
By default, the binary automatically resolves its root working directory via `/proc/self/exe`.

### Case B — Execution from `/tmp` (VFAT/FAT32 USB Drive or SD Card mounted `noexec`)
On MPC OS, USB flash drives and SD cards formatted as `vfat` / `exfat` are typically mounted with the `noexec` flag, which prevents executing binaries directly from the USB drive.

To run the manager from `/tmp` while keeping all configuration, thumbnails, and local folders on your USB drive, export the **`PLG_MAN_DIR`** environment variable pointing to the root Plugin Manager folder on your USB/SD storage:

```sh
#!/bin/sh
# Example launcher script (start_manager.sh) on a VFAT USB drive:
export PLG_MAN_DIR="/media/USB_DRIVE/KikGen_MPC_Force_Plugin_Manager"

cp -f "$PLG_MAN_DIR/mpc_plugin_manager_fb" /tmp/mpc_plugin_manager_fb
chmod 755 /tmp/mpc_plugin_manager_fb
/tmp/mpc_plugin_manager_fb
```

#### Root Directory Resolution Priority (`ConfigParser::getManagerRootDir()`):
1. **`PLG_MAN_DIR`**: If the `PLG_MAN_DIR` environment variable is set and non-empty, its value defines the root directory (`mpc_plugins.conf`, `plugins/`, `Synths/`).
2. **`/proc/self/exe`**: Otherwise, the directory containing the running executable is used.
3. **`getcwd()`**: Fallback to the current working directory.

#### Automatic `<SynthContentLocations>` Verification on Startup:
Because the Akai MPC OS application periodically cleans up `MPC.settings`, the Plugin Manager automatically verifies on every launch (`ConfigParser::ensureManagerSynthsInMpcSettings()`) that `<Location><ManagerRootDir>/Synths</Location>` is present inside `<VALUE name="SynthContentLocations"><SynthContentLocations>`. If missing, it creates a human-readable backup (`MPC.settings.bak-YYYYMMDDHHMMSS`), validates the XML structure, and restores the `Synths` location so plugins stored on the USB drive remain visible to the MPC.

---

## 4. Internet Connectivity Check (`ping_host`), Online Catalog Refresh (`REFRESH`) & `MPC.settings` Verification

Pressing the **`[REFRESH]`** button in the top bar performs a complete online synchronization and integrity audit:
1. **Internet Connectivity Verification via `ping_host`**:
   - Before any refresh or online download, `ConfigParser::checkInternetReachable()` pings the host configured under `"ping_host"` in `mpc_plugins.conf` (default: `"github.com"`).
   - Even if the local Wi-Fi or Ethernet interface has an IP address, no refresh is permitted unless Internet access is confirmed via `ping`. The header displays `ONLINE` (green) or `OFFLINE` (red) accordingly.
2. **Catalog Download via `libcurl` (Forced IPv4)**:
   - Downloads the latest community catalog JSON defined by `catalog_url` in `mpc_plugins.conf` (default: `https://sd88me.github.io/mpc-vst-plugins/catalog.json`) exclusively using `libcurl` with `CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4`.
   - If `libcurl` does not respond or encounters an error, no fallback is executed and a clear error message is reported.
3. **Automatic Missing Thumbnail Download (`thumb.png`)**:
   - For every plugin in the catalog, checks whether `plugins/<plugin-id>/thumb.png` exists on disk. If the file is missing, it is automatically downloaded from `screenshotUrl` via `libcurl` (IPv4), verified against the 8-byte PNG signature, and linked in `mpc_plugins.conf`.
4. **Verification of Previously Installed Plugins in `MPC.settings`**:
   - For every plugin registered or previously detected as installed, scans `/media/az01-internal/Settings/MPC/MPC.settings` inside `<VALUE name="pluginList-arm"><KNOWNPLUGINS><PLUGIN ... file="..."/>` (supporting both single-line and JUCE multi-line `<PLUGIN>` tags) and verifies that the referenced `.so` binary exists on disk so the plugin and its skin load properly.
   - **Auto-Relocation**: If the `.so` file is missing at the registered path, the manager searches all directories listed in `<SynthContentLocations>` (requiring both subdir and binary filename to match when `targetSubdir` is specified). If found in another `SynthContentLocations` path, `MPC.settings` is backed up (`MPC.settings.bak-YYYYMMDDHHMMSS`), the `file="..."` attribute is automatically updated to the valid path, and a `[WARNING]` is logged.
   - **Broken Installation Detection**: If the `.so` file cannot be found in any `SynthContentLocations` directory, the plugin installation is marked as **`BROKEN`** (`! BROKEN (.SO MISSING)`).

---

## 5. Plugin Installation, Uninstall, Reinstall & Repair Workflows

1. **Target Drive Selection**:
   Scans `SynthContentLocations` from `MPC.settings`, mounted drives under `/media/*/Synths`, and `<PLG_MAN_DIR>/Synths`, displaying available free space for each location.
2. **Step 1/4 — Safety Backup (Gold Rule — `YYYYMMDDHHMMSS`)**:
   Creates a human-readable timestamped backup `MPC.settings.bak-YYYYMMDDHHMMSS` before any modification, and validates the XML document structure before atomically replacing `MPC.settings`.
3. **Step 2/4 — `SynthContentLocations` Registration**:
   Ensures the selected `Synths` directory is registered inside `<VALUE name="SynthContentLocations">` in `MPC.settings`.
4. **Step 3/4 — Download via `libcurl` (IPv4) & Archive Extraction**:
   - Verifies Internet connectivity via `ping_host`, then downloads the release archive via `libcurl` (IPv4) with automatic parent directory creation.
   - If `libcurl` fails or does not respond, download is aborted with an error message (no fallback).
   - Extracts `portable/<Author> - VST - <Plugin>/` into the target `Synths` directory.
5. **Step 4/4 — Registration in `pluginList-arm` (`INSTALL` / `REINSTALL` / `REPAIR`)**:
   - **Healthy Installed Plugins**: Opening the Detail View displays **`UNINSTALL`** right next to **`REINSTALL`**.
   - **Broken Installations (`REPAIR`)**: When a plugin installation is marked **`BROKEN`**, the Detail View proposes **`REPAIR`**. Repairing performs a full installation while purging all broken, stale, or invalid `<PLUGIN .../>` entries from `MPC.settings` before inserting a single clean `<PLUGIN .../>` registration.

---

## 6. Hardware Controls (Touchscreen & MPC Internal ALSA MIDI)

| Action | Touchscreen (`/dev/input/event*`) | MPC Physical Controls (ALSA Private RawMIDI) |
| :--- | :--- | :--- |
| **Browse / Select** | Tap a plugin card or list row | **Data Wheel** (`CC 0x64`) |
| **Open Detail / Confirm** | Tap card or **`[INSTALL]`** / **`[REPAIR]`** | **Push Data Wheel** (`Note 0x6F`) or **`[+]`** (`Note 0x36`) |
| **Uninstall Plugin** | Tap **`[UNINSTALL]`** in Detail View | — |
| **Back / Cancel** | Tap **`[< BACK]`** / **`[RETURN]`** / **`[CANCEL]`** | **`[-]`** (`Note 0x37`), **`[MENU]`** (`0x7B`) or **`[MAIN]`** (`0x34`) |
| **Refresh Catalog** | Tap **`[REFRESH]`** in Top Header | — |
| **Quit Application** | Tap **`[EXIT]`** in Bottom Bar | Press **`[-]`** or **`[MENU]`** from root List View |

### Automatic Standby / Screensaver
After **60 seconds of inactivity**, an animated anti-burn-in screensaver activates automatically. Any touchscreen tap or MIDI input (pads, Q-Links, buttons, or Data Wheel) immediately wakes the display without triggering unintended actions.
