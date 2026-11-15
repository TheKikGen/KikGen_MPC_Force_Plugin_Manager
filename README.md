# The KikGen Labs — MPC & Force Standalone Plugin Manager (Release V1.2.15)
<img width="948" height="594" alt="image" src="https://github.com/user-attachments/assets/e8c59a7f-fb9e-4e55-8245-bfd1eda6ea7a" />

Standalone VST2 / Addin / Patch / Companion App manager for **Akai MPC Live / Live II / One / One+ / X / Key 37 / Key 61 / Force** written in native **C++17** (Linux DRM/KMS & `/dev/fb0` Framebuffer, ALSA RawMIDI & `evdev` Touchscreen).

- **Key Highlights
  - **Multi-Drive `"VST2 Plugins"` Architecture**: Uses `VST2 Plugins` as the standard third-party plugin folder across the Plugin Manager root (`<PLG_MAN_DIR>/VST2 Plugins`) and all writable external/internal drives (`/media/<Drive>/VST2 Plugins` on USB sticks, internal SATA SSD 1 / SATA SSD 2, SD cards), while preserving Akai's native `<Location>` entries in `MPC.settings` untouched.
  - **RAM Session Staging (`/tmp/MPC.settings.new`) & Single Commit on Exit**: All `MPC.settings` modifications during a session (install, reinstall, uninstall, repair, sanity check fixes, `<SynthContentLocations>` registration) are staged and validated in RAM (`/tmp/MPC.settings.new`) and committed once upon clean exit.
  - **UBIFS-Safe Atomic Commit & Strict Backup Integrity**: Pre-truncates and `fsync()`s active `/media/az01-internal/Settings/MPC/MPC.settings` before streaming the validated XML buffer to reclaim UBIFS Logical Erase Blocks (LEBs) immediately on saturated internal flash. Backups are exclusively stored in `<PLG_MAN_DIR>/backups/MPC.settings.bak-YYYYMMDDHHMMSS` after passing full XML integrity validation (`validateMpcSettingsXml`), and manual files inside `/media/az01-internal/Settings/MPC/` are never touched.
  - **Sanity Checks & Presets (`.xpl`) Path Synchronizer**: Built-in diagnostic and repair suite scanning all writable `VST2 Plugins` locations for orphan plugins, relinking or purging broken `<PLUGIN>` references in `MPC.settings`, and updating `<PLUGIN file="..."/>` inside `Presets/*.xpl` files while preserving `<version file="1"/>` and binary `<state>` payloads.
  - **Offline Binary Package Cache (`.zip`)**: Supports caching individual or all plugin release archives locally in `plugins/<plugin-id>/<plugin-id>.zip` for 100% offline installation and reinstallation, with one-tap cache purge.
  - **Advanced Settings Suite**: Dedicated **Plugin Manager** and **MPC Settings** tabs including **ROLL BACK** (backup restore/delete with plugin count inspection), **LIMITS** (audio track/plugin instance unlocked limits), **SSH** (session or permanent Dropbear/OpenSSH activation on Port 22), **BOOT LOG** (`syslog`/`dmesg`/`plgman_log.txt` viewer), and **SANITY CHECKS**.
  - **3-Level Hierarchical Filtering & Sorting**: Level 1 (`DISCOVER` / `INSTALLED` / `UPDATES`), Level 2 (`ALL` / `INSTRUMENTS` / `MIDI` / `EFFECTS` / `ADDINS` / `DEVICE PATCHES` / `COMPANION APPS`), and Level 3 (dynamic Sub-Category filter `[ ▽ ALL ▼ ]` + multi-criteria `Sort` by Updated, Name, Size, or Downloads).

---

## 1. Package Architecture & Directory Tree

```text
KikGen_MPC_Force_Plugin_Manager/
├── README.md                     # Complete documentation (this file)
├── Makefile                      # Cross-compilation Makefile (ARMv7hf, Patched GLIBC, AArch64, x86_64, Single-File)
├── mpc_plugins.conf              # Configuration & local catalog file (JSON + code blocks, catalog_url, ping_host)
├── plgman_log.txt                # Persistent session execution & diagnostic log (regenerated on launch)
├── mpc_plugin_manager_fb.cpp     # Amalgamated single-file C++17 source build
├── include/
│   ├── tkgl_gfx.hpp              # 2D DRM/Framebuffer graphics engine (anti-aliased SDF, EPX font, PNG/PPM decoder)
│   ├── tkgl_config.hpp           # mpc_plugins.conf parser, online catalog client, XML staging & sanity engine
│   └── tkgl_ui_toolkit.hpp       # Touch & MIDI UI toolkit (List view, Detail view, Settings, popups, screensaver)
├── src/
│   ├── tkgl_gfx.cpp              # DRM/KMS & /dev/fb0 Framebuffer rendering implementation
│   ├── tkgl_config.cpp           # Path resolution (PLG_MAN_DIR), libcurl IPv4 downloader, MPC.settings staging & backups
│   ├── tkgl_ui_toolkit.cpp       # UI views, 3-level filters, Detail view, Advanced Settings & Sanity Checks
│   └── main.cpp                  # Entry point, CLI flags (-v, --fetch), ALSA RawMIDI & evdev touchscreen threads
├── backups/                      # Validated safety backups (MPC.settings.bak-YYYYMMDDHHMMSS, max 10 retained)
├── plugins/                      # Local catalog cache (tkgl_catalog.json), thumbnails & cached binary archives
│   └── <plugin-id>/
│       ├── thumb.png             # Downloaded plugin screenshot thumbnail (verified 8-byte PNG signature)
│       ├── <plugin-id>.zip       # Optional cached release archive for offline installation
│       ├── manifest.json         # Plugin metadata
│       ├── install.sh            # Standalone installation script
│       └── uninstall.sh          # Standalone uninstallation script
└── VST2 Plugins/                 # Default local directory for third-party VST2 plugin installations
```

---

## 2. Compilation (ARMv7hf Cross-Compilation)

### Prerequisites
On your Linux development machine (Debian/Ubuntu/WSL):
```bash
sudo apt-get update
sudo apt-get install -y g++-arm-linux-gnueabihf libasound2-dev libcurl4-openssl-dev
```

### `make` Targets
- **Standard modular build for MPC / Force (ARMv7hf + NEON)**:
  ```bash
  make
  # or: make armv7
  ```
 
  *(Generates `csu_stubs.o` to satisfy `__libc_csu_init`/`__libc_csu_fini`, strips binary, and verifies max GLIBC/GLIBCXX symbols)*
- **Inspect GLIBC version dependencies**:
  ```bash
  make check-glibc BIN=mpc_plugin_manager_fb
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

## 3. Running on MPC / Force, CLI Options & `PLG_MAN_DIR` Support

### Command-Line Options
- **Standard Interactive UI Launch**:
  ```bash
  ./mpc_plugin_manager_fb [path/to/mpc_plugins.conf]
  ```
- **Version Banner (`-v` / `--version`)**:
  ```bash
  ./mpc_plugin_manager_fb --version
  ```
  Outputs:
  ```text
  The KikGen Labs - Plugins Manager for Akai MPC and Force Devices
  Version: V1.2.15 (v1.2.15) - Compiled on <Date> <Time>
  ```
- **Direct File Downloader Helper (`--fetch <url> <dest_path>`)**:
  ```bash
  ./mpc_plugin_manager_fb --fetch "https://example.com/script.sh" /tmp/script.sh
  ```
  Used by inline Device Patches and Companion Apps to download remote assets over IPv4 via `libcurl`.

### Running from `noexec` USB Drives / SD Cards (`PLG_MAN_DIR`)
On MPC OS, USB flash drives and SD cards formatted as `vfat` / `exfat` are mounted with `noexec`. To execute the binary from `/tmp` while keeping all configuration, backups, thumbnails, cached ZIPs, and `VST2 Plugins` on your USB drive, export **`PLG_MAN_DIR`**:

```sh
#!/bin/sh
export PLG_MAN_DIR="/media/PLGMAN/autorun/mpc_plugin_manager"

cp -f "$PLG_MAN_DIR/mpc_plugin_manager_fb" /tmp/mpc_plugin_manager_fb
chmod 755 /tmp/mpc_plugin_manager_fb
/tmp/mpc_plugin_manager_fb
```

#### Root Directory Resolution Priority (`ConfigParser::getManagerRootDir()`):
1. **`PLG_MAN_DIR`**: If set and non-empty, defines the root directory (`mpc_plugins.conf`, `plugins/`, `VST2 Plugins/`, `backups/`, `plgman_log.txt`).
2. **`/proc/self/exe`**: Otherwise, the directory containing the running executable is used.
3. **`getcwd()`**: Fallback to the current working directory.

---

## 4. Multi-Drive `"VST2 Plugins"` Discovery & `<SynthContentLocations>` Management

1. **Preservation of Existing Akai `<Location>` Entries**:
   - `MPC.settings` contains native Akai synth paths inside `<VALUE name="SynthContentLocations"><SynthContentLocations>` (such as `/usr/share/Akai/Content/Synths` or `/media/<Drive>/Synths`).
   - The Plugin Manager **never deletes or alters** existing `<Location>` entries in `MPC.settings`.
2. **Multi-Drive Installation Targets (`getSynthContentLocations()`)**:
   - Third-party VST2 plugins always target a **`VST2 Plugins`** folder (as in the MPC 3.10) :
     1. The Plugin Manager's own directory: `<ManagerRootDir>/VST2 Plugins` (e.g., on the USB stick).
     2. Every writable mounted volume under `/media/<DriveName>` (internal SATA SSD 1, SATA SSD 2, SD card, USB drives — strictly excluding the internal system flash `/media/az01-internal`) is offered as **`/media/<DriveName>/VST2 Plugins`**, **even if the `VST2 Plugins` folder does not exist yet on that drive**.
   - Free space is queried directly via `statvfs()` on the folder (or its parent mount point if the `VST2 Plugins` subfolder has not been created yet), and targets are sorted from largest to smallest available space.
3. **Automatic Registration in `<SynthContentLocations>`**:
   - **On Startup & Refresh (`ensureManagerSynthsInMpcSettings()`)**: Automatically verifies that `<ManagerRootDir>/VST2 Plugins`, any existing `/media/*/VST2 Plugins` folders on mounted drives, and any `.../VST2 Plugins` parent folders referenced in `KNOWNPLUGINS` are registered inside `<SynthContentLocations>` in `/tmp/MPC.settings.new`.
   - **On Plugin Installation (`executeItemInstall()`)**: Creates `/media/<DriveName>/VST2 Plugins` on the chosen target drive if not yet present, and appends `<Location>/media/<DriveName>/VST2 Plugins</Location>` into `<SynthContentLocations>` if missing.

---

## 5. `MPC.settings` Session Staging, Commit & Backup Rules

To protect the Akai internal flash (`/media/az01-internal/Settings/MPC/MPC.settings`) from wear, corruption, or `ENOSPC` errors on UBIFS:

1. **Session Staging in RAM (`/tmp/MPC.settings.new`)**:
   - On launch, `initSessionMpcSettings()` copies the active `MPC.settings` to `/tmp/MPC.settings.new`.
   - Every operation during the session (Install, Reinstall, Uninstall, Repair, Sanity Check Repair/Delete, `<SynthContentLocations>` updates) reads and writes exclusively to `/tmp/MPC.settings.new` after verifying XML well-formedness via `validateMpcSettingsXml()`.
2. **Single Atomic Commit on Exit (`commitSessionMpcSettingsOnExit()`)**:
   - When exiting the Plugin Manager, if `/tmp/MPC.settings.new` differs from active `MPC.settings`, the staged file is validated and committed once.
   - `posixCopyFileWithSync()` pre-truncates active `MPC.settings`, calls `fsync()` + `sync()` so UBIFS immediately reclaims dirty Logical Erase Blocks (LEBs), and writes the new buffer with `fsync()`.
3. **Strict Backup Rules (`$PLG_MAN_DIR/backups`)**:
   - **Dedicated Backup Location**: All backups are stored exclusively in `<ManagerRootDir>/backups/MPC.settings.bak-YYYYMMDDHHMMSS` (fallback `/tmp/mpc_plugin_manager_backups` if running from internal flash).
   - **Zero Interference with Internal Flash**: The manager **only** touches `/media/az01-internal/Settings/MPC/MPC.settings`. Any manual backup files created by the user inside `/media/az01-internal/Settings/MPC/` are never moved, modified, or deleted.
   - **Integrity Gate**: A backup is created **only** if `validateMpcSettingsXml()` confirms the source `MPC.settings` is non-empty and structurally valid XML. Corrupted files are never backed up.
   - **Retention Policy (Max 10 Healthy Backups)**: Any corrupted backup file in `<ManagerRootDir>/backups/` is purged automatically, and only the **10 most recent healthy backups** are retained.

---

## 6. Online Synchronization (`REFRESH`), Offline Package Caching & Detail View

### Online Catalog Refresh (`[REFRESH]`)
1. **Internet Reachability Check (`ping_host`)**: Pings `ping_host` from `mpc_plugins.conf` (default: `github.com`) and updates the `ONLINE` / `OFFLINE` indicator.
2. **Catalog Sync via `libcurl` (Forced IPv4)**: Downloads `catalog_url` using `CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4`, merging online metadata while preserving local installation and cache states.
3. **Thumbnail Sync (`thumb.png`)**: Downloads missing plugin screenshots, validates their 8-byte PNG header, and stores them in `plugins/<plugin-id>/thumb.png`.
4. **Prompt to Cache All Packages**: Offers to download and cache all plugin release `.zip` archives locally for offline use.

### Offline Binary Cache (`CACHE` / `PURGE`)
- In the **Detail View**, tap **`[CACHE]`** to download the plugin's release archive into `plugins/<plugin-id>/<plugin-id>.zip`.
- When cached (`CACHED (<size>)`), `executeItemInstall()` installs directly from the local `.zip` archive without requiring an Internet connection.
- Tap **`[PURGE]`** to delete the cached `.zip` archive from storage and free up USB/SD space (without affecting the installed plugin).

### Installation States & Detail View Actions
- **Not Installed (`installed == 0`)**: Displays **`[INSTALL]`**, opening the target `VST2 Plugins` drive selector popup.
- **Installed & Healthy (`installed == 1`)**: Registered in `MPC.settings` `KNOWNPLUGINS` and its `.so` binary exists on disk. Displays **`[UNINSTALL]`** and **`[REINSTALL]`** (allowing seamless migration to another drive's `VST2 Plugins` folder while cleaning the old location).
- **Broken (`installed == 2`)**: Registered in `MPC.settings` `KNOWNPLUGINS`, but the referenced `.so` binary is missing or inaccessible on storage. Displays both **`[UNINSTALL]`** (clean removal of broken XML tags and residual files) and **`[REPAIR]`** (reinstallation and clean registration).

---

## 7. Advanced Settings & Sanity Checks (`[SETTINGS]`)

Tap the **Gear icon (`[SETTINGS]`)** in the top header to access two main tabs:

### Tab 1: `Plugin Manager`
- Displays active manager paths (`PLG_MAN_DIR`, `mpc_plugins.conf`, `catalog_url`, `ping_host`, active `VST2 Plugins` mount locations, and free storage meters).

### Tab 2: `MPC Settings` (5 Sub-Tabs)
1. **`ROLL BACK`**:
   - Lists all healthy backups in `<ManagerRootDir>/backups/` with timestamp, file size, and registered plugin count (e.g., `18 plugins`).
   - Scrollable via the physical **Data Wheel** or touchscreen; supports **`[RESTORE]`** (with automatic pre-restore safety backup) and **`[DELETE]`**.
2. **`LIMITS`**:
   - Inspects and configures unlocked audio track and plugin instance limits in `MPC.settings`.
3. **`SSH`**:
   - Enables root SSH access (Port 22 via Dropbear/sshd) either for the **Current Session** or **Permanently** across reboots.
4. **`BOOT LOG`**:
   - Live scrollable viewer aggregating `/var/log/messages` (`syslog`), `/var/log/boot.log`, `dmesg`, and `plgman_log.txt`.
5. **`SANITY CHECKS`**:
   - **Orphan Plugin Detection (`ORPHAN_SYNTH_DIR`)**: Scans all writable `VST2 Plugins` directories for plugin folders containing a `.so` or `plugin-meta.xml` that are missing from `MPC.settings` `KNOWNPLUGINS`.
     - **`[REPAIR]`**: Registers the orphan plugin (and its `VST2 Plugins` location) into staged `/tmp/MPC.settings.new`, deduplicating any stale entries and updating `Presets/*.xpl`.
     - **`[DELETE]`**: Permanently deletes the orphan plugin folder from disk.
   - **Broken `.so` Reference Detection (`MISSING_SO_PATH`)**: Scans all `<PLUGIN .../>` entries in `MPC.settings` `KNOWNPLUGINS` whose `file="..."` binary no longer exists.
     - **`[REPAIR]`**: Searches all writable `VST2 Plugins` locations across mounted drives to relink the moved `.so` (and updates `Presets/*.xpl`), or falls back to reinstalling from the catalog/cache into `/tmp/MPC.settings.new`.
     - **`[DELETE]`**: Cleanly removes the broken `<PLUGIN .../>` line from staged `/tmp/MPC.settings.new`.
   - **Global Presets Path Synchronizer (`[APPLY PRESETS PATHS (.XPL)]` / `applyPresetsPathsForAllInstalled()`)**:
     - Scans `Presets/*.xpl` for every installed plugin in `/tmp/MPC.settings.new` and updates `<PLUGIN file="..."/>` with the active target `.so` path while strictly preserving `<version file="1"/>` and the binary `<state>` preset payload.

---

## 8. Hardware Controls (Touchscreen & MPC Internal ALSA MIDI)

| Action | Touchscreen (`/dev/input/event*`) | MPC Physical Controls (ALSA Private RawMIDI) |
| :--- | :--- | :--- |
| **Browse / Scroll List** | Tap a plugin card, backup, or log row | **Data Wheel** (`CC 0x64`) |
| **Open Detail / Confirm** | Tap card or **`[INSTALL]`** / **`[REPAIR]`** | **Push Data Wheel** (`Note 0x6F`) or **`[+]`** (`Note 0x36`) |
| **Uninstall / Delete** | Tap **`[UNINSTALL]`** / **`[DELETE]`** | — |
| **Cache / Purge ZIP** | Tap **`[CACHE]`** / **`[PURGE]`** in Detail View | — |
| **Back / Cancel** | Tap **`[< BACK]`** / **`[RETURN]`** / **`[CANCEL]`** | **`[-]`** (`Note 0x37`), **`[MENU]`** (`0x7B`) or **`[MAIN]`** (`0x34`) |
| **Refresh Catalog** | Tap **`[REFRESH]`** in Top Header | — |
| **Open Settings** | Tap **Gear Icon** in Top Header | — |
| **Quit & Commit Session** | Tap **`[EXIT]`** in Bottom Bar | Press **`[-]`** or **`[MENU]`** from root List View |

### Automatic Standby / Screensaver
After **60 seconds of inactivity**, an animated anti-burn-in screensaver activates automatically. Any touchscreen tap or MIDI input (pads, Q-Links, buttons, or Data Wheel) immediately wakes the display without triggering unintended actions.
