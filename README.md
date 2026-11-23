# The KikGen Labs — MPC & Force Standalone Plugin Manager (Release V1.2.23 — v1.2.23)

Standalone VST2 / Addin / Patch / Companion App manager for **Akai MPC Live / Live II / One / One+ / X / Key 37 / Key 61 / Force** written in native **C++17** (Linux DRM/KMS & `/dev/fb0` Framebuffer, ALSA RawMIDI & `evdev` Touchscreen).

- **Release Tag**: `V1.2.23` (`v1.2.23`)
- **Key Highlights in V1.2.23**:
  - **External Hotplug-Only Drive Scanning**: VST2 Plugins root discovery (`getSynthContentLocations()`, `getExternalHotplugMounts()`, install/uninstall/sanity scans) now inspects `/proc/mounts` + `/sys/block/` and strictly scans **external hotplug drives only** (USB drives and external SD card slots), excluding internal flash/eMMC (`az01-internal*`, `mmcblk0`), fixed internal partitions, virtual filesystems, and unmounted `/media/*` directories.
  - **Broken Installation Uninstall Support**: Broken plugins (`installed == 2`) can now be cleanly uninstalled both from the **Detail View** `[UNINSTALL]` button and directly inside the **`REPAIR POPUP`**, purging residual folders and any legacy `MPC.settings` entries.
  - **RAM-Safe Two-Phase Install & Automatic `plugin.json` Reconstitution**: Extracts only `.so` files to `/tmp` for 32/64-bit ELF architecture verification (preventing `/tmp` RAMFS exhaustion on large bundles), then unpacks directly onto the target storage drive; automatically reconstitutes missing `plugin.json` manifests from `mpc-plugin.json` / `plugin-meta.xml` and logs the reconstitution in `plgman_log.txt`.
  - **MPC Binary Version Detection (`/usr/bin/MPC`) & Native VST2 Support (`>= 3.10`)**: Automatically parses the `"MPC" + "V.X.Y.Z (xxx)"` header from `/usr/bin/MPC` and displays it first inside **MPC Settings**. Starting with MPC OS **3.10**, native VST2 loading requires zero edits to `MPC.settings`; the manager leaves `MPC.settings` untouched during install/uninstall/init and detects plugins directly from disk `VST2 Plugins` folders at mount roots.
  - **Mount-Root `"VST2 Plugins"` Architecture (`>= 3.10`)**: Since MPC OS 3.10 scans `<MountPoint>/VST2 Plugins` at the root of every writable mounted drive (excluding RO volumes and internal MPC flash), the Plugin Manager resolves the mount point on which it resides (`/media/<Mount>/VST2 Plugins`) rather than using a nested subfolder, alongside all writable `/media/<Drive>/VST2 Plugins` targets.
  - **Startup Migration Alert & Sequenced Sanity Checks (`>= 3.10`)**: On launch under MPC OS `>= 3.10`, warns if `MPC.settings` still contains `VST2 Plugins` entries in `<SynthContentLocations>` or entries in `<KNOWNPLUGINS>` and directs the user to **MPC Settings -> Sanity Checks**. Once `<KNOWNPLUGINS>` are purged and outside-`VST2 Plugins` plugins are relocated, Sanity Checks proposes purging `VST2 Plugins` entries from `<SynthContentLocations>` with a pre-edit safety backup.
  - **Sleep-Safe Dual-Port ALSA MIDI Session Listener**: Opens and keeps **both** Public and Private ALSA RawMIDI ports open across the entire Plugin Manager session (closing cleanly at session exit and tracing all port open/activity/close events in `plgman_log.txt`), ensuring zero dropped Jog Wheel or button inputs if either controller port enters sleep.
  - **RAM Session Staging (`/tmp/MPC.settings.new`) & Single Commit on Exit**: When `MPC.settings` edits or Sanity Check purges occur, modifications are staged and validated in RAM (`/tmp/MPC.settings.new`) and committed once upon clean exit with UBIFS-safe pre-truncate + `fsync()`.
  - **Sanity Checks & Presets (`.xpl`) Path Synchronizer**: Adapts automatically to MPC OS version: on `>= 3.10`, purges legacy `<KNOWNPLUGINS>`, relocates outside-`VST2 Plugins` installs to mount-root `VST2 Plugins`, then purges legacy `VST2 Plugins` `<SynthContentLocations>` entries; on `< 3.10`, repairs orphan plugins and broken `.so` paths, plus synchronizes `Presets/*.xpl` on all OS versions.
  - **Offline Binary Package Cache (`.zip`)**: Supports caching individual or all plugin release archives locally in `plugins/<plugin-id>/<plugin-id>.zip` for 100% offline installation and reinstallation, with one-tap cache purge.
  - **Advanced Settings Suite**: Dedicated **Plugin Manager** tab (including interactive **Screen Rotation** `No rotation` / `90` / `180` / `270` saved to `mpc_plugins.conf`) and **MPC Settings** tab (**ROLL BACK**, **BOOT LOG**, and **SANITY CHECKS**).
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
└── VST2 Plugins/                 # Legacy (< 3.10) subfolder; on >= 3.10 created at <MountPoint>/VST2 Plugins/
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
- **Patched build for older target GLIBC (links against `target_libs/` from target device)**:
  ```bash
  make patched
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
  Version: V1.2.23 (v1.2.23) - Compiled on <Date> <Time>
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

## 4. Multi-Drive `"VST2 Plugins"` Discovery & `<SynthContentLocations>`

1. **Mount-Root `"VST2 Plugins"` Discovery (`MPC OS >= 3.10` Native VST2)**:
   - Starting with MPC OS **3.10**, the Akai MPC binary natively scans `<MountPoint>/VST2 Plugins` at the **root** of every writable mounted volume (excluding read-only volumes and `/media/az01-internal`). Subfolders such as `<ManagerRootDir>/VST2 Plugins` are **not** scanned by MPC 3.10+.
   - Accordingly, `getManagerMountRoot()` parses `/proc/mounts` to resolve the writable mount point hosting the manager (e.g., `/media/PLGMAN`) and creates `<MountPoint>/VST2 Plugins` (`/media/PLGMAN/VST2 Plugins`) as the manager's default VST2 folder.
   - `MPC.settings` is **never modified** during startup, catalog refresh, installation, or uninstallation on `>= 3.10`; installed plugins and `Presets/*.xpl` paths are managed directly on disk.
2. **Legacy Mode (`MPC OS < 3.10`) & `<SynthContentLocations>` Preservation**:
   - `MPC.settings` contains native Akai synth paths inside `<VALUE name="SynthContentLocations"><SynthContentLocations>` (such as `/usr/share/Akai/Content/Synths` or `/media/<Drive>/Synths`). The manager **never alters or removes** native Akai `<Location>` entries.
   - On `< 3.10`, third-party VST2 plugins target `<ManagerRootDir>/VST2 Plugins` or `/media/<DriveName>/VST2 Plugins`, and the manager registers those `VST2 Plugins` folders in `<SynthContentLocations>` via `ensureManagerSynthsInMpcSettings()` / `executeItemInstall()`.
3. **External Hotplug-Only Enumeration (`getExternalHotplugMounts()` & `getSynthContentLocations()`)**:
   - Inspects `/proc/mounts` and `/sys/block/<dev>` to scan **exclusively active writable external hotplug drives** (USB flash/SSD on `/usb` bus or removable `sd*`, and external SD cards `mmcblk1+` with non-`MMC` card type), while strictly excluding internal flash/eMMC (`az01-internal*`, `mmcblk0*`), virtual/system filesystems, read-only mounts, and unmounted `/media/*` directories.
   - Each detected external hotplug drive is offered as `/media/<HotplugDrive>/VST2 Plugins`, queried via `statvfs()` and sorted largest-first.

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

### Installation States, ELF 32/64-bit Check, `plugin.json` & Detail View Actions
- **RAM-Safe Pre-Install 32-bit / 64-bit Check (`getSystemElfBitness()` & `getElfFileBitness()`)**: Only the `.so` binary file(s) are extracted to `/tmp` for ELF class verification (`ELF32` on 32-bit OS, `ELF64` on 64-bit OS), and `/tmp` is cleaned immediately. Any mismatch aborts cleanly; only verified bundles are unzipped directly onto the target storage drive (preventing `/tmp` RAM exhaustion on large plugins).
- **Automatic `plugin.json` Reconstitution**: If `plugin.json` is missing in the deployed plugin folder, it is automatically reconstituted from `mpc-plugin.json`, `plugin-meta.xml`, and catalog metadata (using `file32` / `file64` matching the verified ELF binary) and traced in `plgman_log.txt`.
- **Not Installed (`installed == 0`)**: Displays **`[INSTALL]`**, opening the target `VST2 Plugins` drive selector popup.
- **Installed & Healthy (`installed == 1`)**: Registered in `MPC.settings` `KNOWNPLUGINS` and its `.so` binary exists on disk. Displays **`[UNINSTALL]`** and **`[REINSTALL]`** (allowing seamless migration to another drive's `VST2 Plugins` folder while cleaning the old location).
- **Broken (`installed == 2`)**: Registered in `MPC.settings` `KNOWNPLUGINS`, but the referenced `.so` binary is missing or inaccessible on storage. Displays both **`[UNINSTALL]`** (clean removal of broken XML tags and residual files) and **`[REPAIR]`** (reinstallation and clean registration).

---

## 7. Advanced Settings & Sanity Checks (`[SETTINGS]`)

Tap the **Gear icon (`[SETTINGS]`)** in the top header to access two main tabs:

### Tab 1: `Plugin Manager`
- Displays active manager paths (`PLG_MAN_DIR`, `mpc_plugins.conf`, `catalog_url`, `ping_host`, active `VST2 Plugins` mount locations) and allows changing **Screen Rotation** (**`No rotation`**, **`90`**, **`180`**, **`270`**) immediately with persistent saving to `mpc_plugins.conf`.

### Tab 2: `MPC Settings` (MPC Version Header + 3 Sub-Tabs)
- **MPC Binary Version Banner (Displayed First)**: Scans `/usr/bin/MPC` for the `"MPC"` signature followed immediately by `"V.X.Y.Z (xxx)"` (`Y`, `Z`, and build tag optional) and displays whether the device runs **Native VST2 mode (`>= 3.10`, `MPC.settings` untouched)** or **Legacy mode (`< 3.10`)**.
1. **`ROLL BACK`**:
   - Lists all healthy backups in `<ManagerRootDir>/backups/` with timestamp, file size, and registered plugin count (e.g., `18 plugins`).
   - Scrollable via the physical **Data Wheel** or touchscreen; supports **`[RESTORE]`** (with automatic pre-restore safety backup) and **`[DELETE]`**.
2. **`BOOT LOG`**:
   - Live scrollable viewer aggregating `/var/log/messages` (`syslog`), `/var/log/boot.log`, `dmesg`, and `plgman_log.txt`.
3. **`SANITY CHECKS`**:
   - **On MPC OS `>= 3.10` (Native VST2)**:
     - **Obsolete `<KNOWNPLUGINS>` Check (`LEGACY_KNOWNPLUGINS`)**: Detects legacy `<KNOWNPLUGINS>` entries still present in `MPC.settings` and offers **`[PURGE ALL]`** (with pre-edit safety backup).
     - **Outside Mount-Root `VST2 Plugins` Relocation (`OUTSIDE_VST2_LOCATION`)**: Detects third-party VST2 plugins placed in `SynthContentLocations`, `/media/*/Synths`, or legacy manager subfolders outside `<MountPoint>/VST2 Plugins` and offers **`[REINSTALL]`** to move them into mount-root `VST2 Plugins` or **`[DELETE]`**.
     - **Sequenced `<SynthContentLocations>` Cleanup (`LEGACY_SYNTH_VST2_LOCATIONS`)**: Once `<KNOWNPLUGINS>` and outside-`VST2 Plugins` issues are resolved, detects `VST2 Plugins` `<Location>` entries still remaining in `<SynthContentLocations>` and offers **`[PURGE ALL]`** (with pre-edit safety backup) while keeping Akai's native locations intact.
   - **On MPC OS `< 3.10` (Legacy)**:
     - **Orphan Plugin Detection (`ORPHAN_SYNTH_DIR`)** & **Broken `.so` Reference Detection (`MISSING_SO_PATH`)**: Registers or relinks orphan/broken plugins in staged `/tmp/MPC.settings.new`.
   - **Global Presets Path Synchronizer (`[APPLY PRESETS PATHS (.XPL)]` / `applyPresetsPathsForAllInstalled()`)**:
     - Scans `Presets/*.xpl` for every installed plugin (directly from `VST2 Plugins` on `>= 3.10` or staged `MPC.settings` on `< 3.10`) and updates `<PLUGIN file="..."/>` with the active target `.so` path while strictly preserving `<version file="1"/>` and the binary `<state>` preset payload.

---

## 8. Hardware Controls (Touchscreen & Persistent Dual-Port ALSA MIDI)

- **Dual-Port Persistent Monitoring**: Both **Public** and **Private** ALSA RawMIDI ports (`hw:X,0,0` & `hw:X,0,1`) are opened at launch, continuously scanned together for the entire Plugin Manager session (protecting against unexpected port sleep on MPC/Force controllers), and closed cleanly on exit. All port open, first-activity, and close events are logged to `plgman_log.txt`.

| Action | Touchscreen (`/dev/input/event*`) | MPC & Akai Force Physical Controls (ALSA Public + Private RawMIDI `hw:X,0,0` & `hw:X,0,1`) |
| :--- | :--- | :--- |
| **Browse / Scroll List** | Tap a plugin card, backup, or log row | **Jog Wheel** (`B0 64 01` CW / `B0 64 7F` CCW) or **Cursor Up/Down/Left/Right** (`0x21`/`0x22`/`0x3D`/`0x3E`) |
| **Open Detail / Confirm** | Tap card or **`[INSTALL]`** / **`[REPAIR]`** | **Push Jog Wheel** (`90 6F 7F` press / `90 6F 00` release) or **`[+]`** (`Note 0x36`) |
| **Uninstall / Delete** | Tap **`[UNINSTALL]`** / **`[DELETE]`** | — |
| **Cache / Purge ZIP** | Tap **`[CACHE]`** / **`[PURGE]`** in Detail View | — |
| **Back / Cancel** | Tap **`[< BACK]`** / **`[RETURN]`** / **`[CANCEL]`** | **Force**: **`[MENU]`** (`Note 0x02`) or **`[NAVIGATE]`** (`Note 0x00`) • **MPC**: **`[-]`** (`Note 0x37`), **`[MENU]`** (`Note 0x7B`) or **`[MAIN]`** (`Note 0x34`) |
| **Refresh Catalog** | Tap **`[REFRESH]`** in Top Header | — |
| **Open Settings** | Tap **Gear Icon** in Top Header | — |
| **Quit & Commit Session** | Tap **`[EXIT]`** in Bottom Bar | Press **`[MENU]`** / **`[NAVIGATE]`** (Force) or **`[-]`** / **`[MENU]`** / **`[MAIN]`** (MPC) from root List View |

### Automatic Standby / Screensaver
After **60 seconds of inactivity**, an animated anti-burn-in screensaver activates automatically. Any touchscreen tap or MIDI input (pads, Q-Links, buttons, or Data Wheel) immediately wakes the display without triggering unintended actions.
