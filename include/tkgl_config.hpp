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

constexpr const char* TKGL_MANAGER_VERSION = "V1.2.15 (v1.2.15)";

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
        ORPHAN_SYNTH_DIR = 1, // Present in writable SynthLocation on disk, absent from MPC.settings KNOWNPLUGINS
        MISSING_SO_PATH  = 2  // Declared in MPC.settings KNOWNPLUGINS, but .so file not found on disk
    };
    int         type = ORPHAN_SYNTH_DIR;
    std::string pluginName;
    std::string dirPath;
    std::string soPath;
    std::string synthRoot;
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
    static bool enableSshAccess(bool permanent, std::string& outStatusMsg);
    static std::vector<SanityCheckIssue> runSanityChecks(const LauncherConfig& config, std::vector<std::string>* outLogs = nullptr);
    static bool repairSanityIssue(LauncherConfig& config, const SanityCheckIssue& issue, std::string& outStatusMsg, std::vector<std::string>* outLogs = nullptr);
    static bool deleteSanityIssue(LauncherConfig& config, const SanityCheckIssue& issue, std::string& outStatusMsg, std::vector<std::string>* outLogs = nullptr);
    static int  applyPresetsPathsForAllInstalled(const LauncherConfig& config, int& outPluginsCount, std::string& outStatusMsg, std::vector<std::string>* outLogs = nullptr);
};

using PluginCatalogConfig = LauncherConfig;

} // namespace tkgl::config
