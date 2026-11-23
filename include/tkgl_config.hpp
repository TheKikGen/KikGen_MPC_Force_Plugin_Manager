/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: include/tkgl_config.hpp
 * Description: Configuration & Online Catalog Parser
 *              Supports "global" (including "catalog_url"), "sections", "items",
 *              tags, osCompat, and live online catalog refresh.
 * ============================================================================
 */

#pragma once

#include <string>
#include <vector>
#include <set>
#include <functional>

namespace tkgl::config {

constexpr const char* TKGL_MANAGER_VERSION = "V1.2.23 (v1.2.23)";

struct MenuItem {
    std::string id;
    std::string label;
    std::string author        = "The KikGen Labs";
    std::string version       = "v1.1.4";
    std::string category      = "Synthesizer";
    std::string shortType     = "INST";
    std::string channel       = "Stable";
    std::string testedDevice  = "MPC Live II / Force";
    std::string osCompat      = "MPC OS 2.10 / 3.x";
    std::string updatedDate   = "2026-10-16";
    std::string downloads     = "127";
    std::string sizeMb        = "2.4 MB";
    std::string cpuLoad       = "14%";
    std::string targetSubdir  = "";
    std::string targetBinary  = "";
    std::string screenshotUrl = "";
    std::string downloadUrl   = "";
    std::string thumbPpmPath  = "";
    std::string description;
    std::string cmd;
    int         is_inline     = 0;
    int         installed     = 0; // 0 = uninstalled / not installed, 1 = installed & accessible, 2 = broken (.so path missing)
    bool        updateAvail   = false;
    bool        isCached      = false;
    std::string cachedZipSize = "";
    std::vector<std::string> tags;
    std::string skinFolderPath = ""; // Complete resolved path from root (/)
};

struct SynthLocation {
    std::string path;
    std::string label;
    std::string freeSpace;
    uint64_t    freeBytes = 0;
    bool        isManagerDefault = false;
};

struct MpcSettingsBackup {
    std::string filename;
    std::string fullPath;
    std::string timestampFormatted;
    size_t      fileSize = 0;
    int         pluginCount = 0;
};

struct SanityCheckIssue {
    enum Type {
        ORPHAN_SYNTH_DIR          = 1, // Present in writable SynthLocation on disk, absent from MPC.settings KNOWNPLUGINS
        MISSING_SO_PATH           = 2, // Declared in MPC.settings KNOWNPLUGINS, but .so file not found on disk
        LEGACY_KNOWNPLUGINS       = 3, // MPC OS >= 3.10: Legacy KNOWNPLUGINS entries still present in MPC.settings (can be purged)
        OUTSIDE_VST2_LOCATION     = 4, // MPC OS >= 3.10: VST2 plugin found in a SynthContentLocation other than mount-root VST2 Plugins
        LEGACY_SYNTH_VST2_LOCATIONS = 5 // MPC OS >= 3.10: SynthContentLocations still contains VST2 Plugins <Location> entries (shown once Checks 1 & 2 are cleared)
    };
    int         type = ORPHAN_SYNTH_DIR;
    std::string pluginName;
    std::string dirPath;
    std::string soPath;
    std::string synthRoot;
};

struct MpcV310StartupWarnings {
    bool hasSynthVst2Locations = false;
    std::vector<std::string> synthVst2Locations;
    bool hasLegacyKnownPlugins = false;
    int  knownPluginsCount     = 0;
    bool needsAttention() const { return hasSynthVst2Locations || hasLegacyKnownPlugins; }
};

struct MpcBinaryVersionInfo {
    bool        found         = false;
    std::string binaryPath    = "/usr/bin/MPC";
    int         major         = 0;
    int         minor         = 0;
    int         minorIndex    = 0;
    int         patch         = 0;
    bool        hasMinorIndex = false;
    bool        hasPatch      = false;
    std::string versionNumberStr; // e.g. "3.10.0.1"
    std::string fullVersionStr;   // e.g. "3.10.0.1 (build)"
    bool        isNativeVst2  = false; // true when major > 3 || (major == 3 && minor >= 10)
};

struct Section {
    std::string title;
    std::vector<MenuItem> items;
};

struct LauncherConfig {
    std::string app_title       = "AKAI MPCs PLUGINS MANAGER";
    int         screen_rotation = 270;
    std::string catalog_url     = "https://sd88me.github.io/mpc-vst-plugins/catalog.json";
    std::string ping_host       = "github.com";
    std::string config_path     = "mpc_plugins.conf";
    std::vector<Section> sections;

    void loadEmbeddedDefaults();
};

class ConfigParser {
public:
    static std::string getManagerRootDir();
    static std::string getManagerMountRoot();
    static std::string getManagerVst2Dir();
    static bool checkInternetReachable(
        const std::string& pingHost = "github.com",
        std::vector<std::string>* outLogs = nullptr
    );
    static bool downloadFile(
        const std::string& url,
        const std::string& destPath,
        long timeoutSec = 60,
        std::function<void(const std::string&)> onProgress = nullptr,
        const std::string& progressPrefix = "[TKGL-NET] Downloading"
    );
    static bool loadFromFile(const std::string& configPath, LauncherConfig& outConfig);
    static bool saveToFile(const std::string& configPath, const LauncherConfig& config);
    static bool syncFromOnlineCatalog(LauncherConfig& config, std::vector<std::string>& outLogs, std::function<void(const std::string&)> onProgress = nullptr);
    static std::vector<SynthLocation> getSynthContentLocations(const std::string& mpcSettingsPath = "/media/az01-internal/Settings/MPC/MPC.settings");
    static int  executeItemInstall(const LauncherConfig& cfg, const MenuItem& item, const std::string& targetSynthDir, std::vector<std::string>& outLogs, std::function<void(const std::string&)> onProgress = nullptr);
    static int  executeItemUninstall(const LauncherConfig& cfg, const MenuItem& item, std::vector<std::string>& outLogs, std::function<void(const std::string&)> onProgress = nullptr);
    static int  executeItemCommand(const LauncherConfig& cfg, const MenuItem& item, std::vector<std::string>& outLogs);
    static int  getSystemElfBitness(std::string* outDesc = nullptr);
    static int  getElfFileBitness(const std::string& filePath, std::string* outMachineDesc = nullptr);
    static MpcBinaryVersionInfo detectMpcBinaryVersion(bool forceRefresh = false);
    static bool isMpcNativeVst2Supported();
    static MpcV310StartupWarnings checkMpcV310StartupWarnings();
    static void ensureManagerSynthsInMpcSettings(std::vector<std::string>* outLogs = nullptr);
    static void refreshInstallStates(LauncherConfig& config, std::vector<std::string>* outLogs = nullptr);
    static bool isItemBinaryCached(const MenuItem& item, std::string* outSizeStr = nullptr, std::string* outZipPath = nullptr);
    static bool cacheItemBinary(const MenuItem& item, std::vector<std::string>& outLogs, std::function<void(const std::string&)> onProgress = nullptr);
    static bool deleteItemBinary(const MenuItem& item, std::vector<std::string>& outLogs);
    static int  cacheAllCatalogBinaries(LauncherConfig& config, std::vector<std::string>& outLogs, std::function<void(const std::string&)> onProgress = nullptr);
    static std::string resolveMpcSettingsPath();
    static std::string resolveBackupDirectory();
    static std::vector<MpcSettingsBackup> listMpcSettingsBackups();
    static bool restoreMpcSettingsBackup(const std::string& backupPath, std::vector<std::string>& outLogs);
    static bool deleteMpcSettingsBackup(const std::string& backupPath, std::vector<std::string>& outLogs);
    static bool isSynthDirectoryWritable(const std::string& path);
    static void initLogFile();
    static void logMessage(const std::string& msg);
    static void initSession();
    static bool hasStagedMpcSettings();
    static bool commitSessionMpcSettings(std::vector<std::string>* outLogs = nullptr);
    static void enforceBackupRetention(size_t maxKeep = 10, std::vector<std::string>* outLogs = nullptr);
    static std::vector<std::string> readSystemBootLogs(size_t maxLines = 250);
    static bool purgeKnownPluginsInMpcSettings(std::string& outStatusMsg, std::vector<std::string>* outLogs = nullptr);
    static bool purgeVst2LocationsInSynthContentLocations(std::string& outStatusMsg, std::vector<std::string>* outLogs = nullptr);
    static std::vector<SanityCheckIssue> runSanityChecks(const LauncherConfig& config, std::vector<std::string>* outLogs = nullptr);
    static bool repairSanityIssue(LauncherConfig& config, const SanityCheckIssue& issue, std::string& outStatusMsg, std::vector<std::string>* outLogs = nullptr);
    static bool deleteSanityIssue(LauncherConfig& config, const SanityCheckIssue& issue, std::string& outStatusMsg, std::vector<std::string>* outLogs = nullptr);
    static int  applyPresetsPathsForAllInstalled(const LauncherConfig& config, int& outPluginsCount, std::string& outStatusMsg, std::vector<std::string>* outLogs = nullptr);
};

using PluginCatalogConfig = LauncherConfig;

} // namespace tkgl::config
