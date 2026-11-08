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
#include <functional>

namespace tkgl::config {

constexpr const char* TKGL_MANAGER_VERSION = "V1.1.7 (v1.1.7)";

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
    bool        installed     = false;
    bool        updateAvail   = false;
    bool        broken        = false;
    bool        isCached      = false;
    std::string cachedZipSize = "";
    std::vector<std::string> tags;
};

struct SynthLocation {
    std::string path;
    std::string label;
    std::string freeSpace;
    bool        isManagerDefault = false;
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
};

using PluginCatalogConfig = LauncherConfig;

} // namespace tkgl::config
