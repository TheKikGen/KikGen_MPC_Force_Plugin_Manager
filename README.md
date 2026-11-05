# The KikGen Labs — MPC & Force Standalone Plugin Manager

Standalone VST2 / Addin / Patch manager for **Akai MPC Live / Live II / One / One+ / X / Key 37 / Key 61 / Force** written in native **C++17** (Linux Framebuffer `/dev/fb0`, ALSA RawMIDI & `evdev` Touchscreen, zero external build dependencies).

---

## 1. Package Architecture & Directory Tree

```text
KikGen_MPC_Force_Plugin_Manager/
├── README.md                     # Complete documentation (this file)
├── Makefile                      # Cross-compilation Makefile (ARMv7hf, AArch64, x86_64, Single-File)
├── mpc_plugins.conf              # Configuration & local catalog file (JSON + code blocks)
├── mpc_plugin_manager_fb.cpp     # Amalgamated single-file C++17 source build
├── include/
│   ├── tkgl_gfx.hpp              # 2D Framebuffer graphics engine (anti-aliased SDF, EPX font, PNG/PPM decoder)
│   ├── tkgl_config.hpp           # mpc_plugins.conf parser, online catalog client, verifier & installer
│   └── tkgl_ui_toolkit.hpp       # Touch & MIDI UI toolkit (List view, Detail view, popups, screensaver)
├── src/
│   ├── tkgl_gfx.cpp              # /dev/fb0 Framebuffer rendering implementation
│   ├── tkgl_config.cpp           # Path resolution (PLG_MAN_DIR), dynamic libcurl.so.4 loader & MPC.settings engine
│   ├── tkgl_ui_toolkit.cpp       # Plugin cards, Detail view (Install/Reinstall/Uninstall/Repair) & live logs
│   └── main.cpp                  # Entry point, ALSA Private MIDI & evdev touchscreen threads
├── plugins/                      # Local catalog cache (tkgl_catalog.json) and per-plugin assets
│   └── <plugin-id>/
│       ├── thumb.ppm / thumb.png # Downloaded or pre-rendered plugin thumbnail
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

---

## 4. Online Catalog Refresh (`REFRESH`) & `MPC.settings` Integrity Verification

Pressing the **`[REFRESH]`** button in the top bar performs a complete online synchronization and integrity audit:
1. **Catalog Download via `libcurl.so.4`**:
   - Downloads the latest community catalog JSON defined by `catalog_url` in `mpc_plugins.conf` (default: `https://sd88me.github.io/mpc-vst-plugins/catalog.json`) using dynamic `dlopen("libcurl.so.4")` / `libcurl.so.4.8.0`.
2. **Download All Thumbnails**:
   - Automatically downloads all plugin thumbnails into `plugins/<plugin-id>/thumb.png` during the refresh operation so every card and detail view renders its real screenshot offline.
3. **Verification of Previously Installed Plugins in `MPC.settings`**:
   - For every plugin registered or previously detected as installed, scans `/media/az01-internal/Settings/MPC/MPC.settings` inside `<VALUE name="pluginList-arm"><KNOWNPLUGINS><PLUGIN ... file="..."/>` and verifies that the referenced `.so` binary exists on disk so the plugin and its skin load properly.
   - **Auto-Relocation**: If the `.so` file is missing at the registered path, the manager searches all directories listed in `<SynthContentLocations>`. If the `.so` file is found in another `SynthContentLocations` path, `MPC.settings` is backed up (`Gold Rule`), the `file="..."` attribute is automatically updated to the valid path, and a `[WARNING]` is logged.
   - **Broken Installation Detection**: If the `.so` file cannot be found in any `SynthContentLocations` directory, the plugin installation is marked as **`BROKEN`** (`! BROKEN (.SO MISSING)`).

---

## 5. Plugin Installation, Uninstall, Reinstall & Repair Workflows

1. **Target Drive Selection**:
   Scans `SynthContentLocations` from `MPC.settings`, mounted drives under `/media/*/Synths`, and `<PLG_MAN_DIR>/Synths`, displaying available free space for each location.
2. **Step 1/4 — Safety Backup (Gold Rule)**:
   Creates a timestamped backup `MPC.settings.bak-<timestamp>` before any modification.
3. **Step 2/4 — `SynthContentLocations` Registration**:
   Ensures the selected `Synths` directory is registered inside `<VALUE name="SynthContentLocations">` in `MPC.settings`.
4. **Step 3/4 — Native Download via `libcurl.so.4` & Archive Extraction**:
   - Uses the dynamically loaded `libcurl.so.4` / `libcurl.so.4.8.0` library (and CLI helper mode `mpc_plugin_manager_fb --fetch <url> <dest>`) with full HTTPS/TLS and HTTP 302 redirect support.
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
