/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: src/tkgl_ui_toolkit.cpp
 * Description: High-Definition Framebuffer UI matching image_22ce5693.png:
 *              - SDF anti-aliased rounded cards, buttons, and #tag pills
 *              - Real PNG/PPM plugin screenshots with top-left version pill
 *                and bottom-right FX/INST/MIDI badge
 *              - Verified Gen1 / Gen2 emerald badges with vector checkmarks
 *              - Multi-line smooth description + release notes + #tags card
 *              - Automatic Standby / Screensaver after inactivity (wakes on
 *                any MIDI or Touch input)
 * ============================================================================
 */

#include "tkgl_ui_toolkit.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sys/statvfs.h>

namespace tkgl::ui {

using namespace tkgl::gfx;

PluginManagerView::PluginManagerView(DrmFramebufferCanvas& canvas, config::LauncherConfig& config)
    : canvas_(canvas), config_(config) {
    lastActivityTime_ = std::time(nullptr);
    isOnline_ = config::ConfigParser::checkInternetReachable(config_.ping_host);
    rebuildFilteredItems();
    if (config::ConfigParser::isMpcNativeVst2Supported()) {
        v310Warnings_ = config::ConfigParser::checkMpcV310StartupWarnings();
        if (v310Warnings_.needsAttention()) {
            uiState_ = UIState::V310_WARNING_POPUP;
        }
    }
}

bool PluginManagerView::wakeIfScreensaver() {
    lastActivityTime_ = std::time(nullptr);
    if (isScreensaver_) {
        isScreensaver_ = false;
        render();
        return true;
    }
    return false;
}

void PluginManagerView::onAnyInput() {
    wakeIfScreensaver();
}

void PluginManagerView::checkIdleTick() {
    if (uiState_ == UIState::EXEC_SCREEN && !execFinished_) {
        lastActivityTime_ = std::time(nullptr);
        return;
    }
    std::time_t now = std::time(nullptr);
    if (!isScreensaver_) {
        if (std::difftime(now, lastActivityTime_) >= IDLE_TIMEOUT_SEC) {
            isScreensaver_ = true;
            screensaverTick_ = 0;
            render();
        }
    } else {
        screensaverTick_++;
        render();
    }
}

std::vector<std::string> PluginManagerView::getAvailableCategories() const {
    std::vector<std::string> cats;
    cats.push_back("ALL");
    std::set<std::string> seen;
    for (int s = 0; s < static_cast<int>(config_.sections.size()); ++s) {
        if (sectionFilter_ >= 0 && sectionFilter_ != s) continue;
        for (const auto& it : config_.sections[s].items) {
            if (!it.category.empty() && seen.find(it.category) == seen.end()) {
                seen.insert(it.category);
                cats.push_back(it.category);
            }
        }
    }
    return cats;
}

void PluginManagerView::cycleCategoryFilter() {
    auto cats = getAvailableCategories();
    if (cats.empty()) {
        categoryFilter_ = "ALL";
        return;
    }
    int curIdx = 0;
    for (size_t i = 0; i < cats.size(); ++i) {
        if (cats[i] == categoryFilter_) {
            curIdx = static_cast<int>(i);
            break;
        }
    }
    int nextIdx = (curIdx + 1) % static_cast<int>(cats.size());
    categoryFilter_ = cats[nextIdx];
    selectedIndex_ = 0;
    rebuildFilteredItems();
    render();
}

void PluginManagerView::cycleSortOption() {
    sortBy_ = (sortBy_ + 1) % 4;
    selectedIndex_ = 0;
    rebuildFilteredItems();
    render();
}

void PluginManagerView::rebuildFilteredItems() {
    filteredItems_.clear();
    for (int s = 0; s < static_cast<int>(config_.sections.size()); ++s) {
        if (sectionFilter_ >= 0 && sectionFilter_ != s) continue;
        const auto& sec = config_.sections[s];
        for (int i = 0; i < static_cast<int>(sec.items.size()); ++i) {
            const auto& item = sec.items[i];
            if (activeTab_ == FilterTab::INSTALLED && (item.installed == 0)) continue;
            if (activeTab_ == FilterTab::UPDATES && !item.updateAvail) continue;
            if (categoryFilter_ != "ALL" && item.category != categoryFilter_) continue;
            filteredItems_.push_back({s, i});
        }
    }

    if (sortBy_ > 0) {
        std::stable_sort(filteredItems_.begin(), filteredItems_.end(), [&](const FilteredItemRef& a, const FilteredItemRef& b) {
            const auto& itemA = config_.sections[a.sectionIdx].items[a.itemIdx];
            const auto& itemB = config_.sections[b.sectionIdx].items[b.itemIdx];
            if (sortBy_ == 1) { // NAME
                return itemA.label < itemB.label;
            }
            if (sortBy_ == 2) { // SIZE
                double szA = std::atof(itemA.sizeMb.c_str());
                double szB = std::atof(itemB.sizeMb.c_str());
                return szA > szB;
            }
            if (sortBy_ == 3) { // DOWNLOADS
                long dlA = std::atol(itemA.downloads.c_str());
                long dlB = std::atol(itemB.downloads.c_str());
                return dlA > dlB;
            }
            return false;
        });
    }

    if (selectedIndex_ >= static_cast<int>(filteredItems_.size())) {
        selectedIndex_ = std::max(0, static_cast<int>(filteredItems_.size()) - 1);
    }
}

void PluginManagerView::triggerCatalogRefresh() {
    uiState_ = UIState::EXEC_SCREEN;
    isRefreshing_ = true;
    animFrame_ = 0;
    execLogs_.clear();
    execLogs_.push_back("=== The KikGen Labs: Online Catalog Sync ===");
    execFinished_ = false;
    render();

    std::string pingTarget = config_.ping_host.empty() ? "github.com" : config_.ping_host;
    isOnline_ = config::ConfigParser::checkInternetReachable(pingTarget, &execLogs_);
    if (!isOnline_) {
        execLogs_.push_back("[ERROR] No internet access (ping " + pingTarget + " failed).");
        execLogs_.push_back("[ERROR] Refresh aborted: internet connection is required.");
        isRefreshing_ = false;
        execFinished_ = true;
        lastActivityTime_ = std::time(nullptr);
        render();
        return;
    }

    canvas_.clearImageCache();
    auto progressCb = [this](const std::string& msg) {
        animFrame_++;
        lastActivityTime_ = std::time(nullptr);
        execLogs_.push_back(msg);
        if (execLogs_.size() > 18) {
            execLogs_.erase(execLogs_.begin() + 1);
        }
        render();
    };

    config::ConfigParser::syncFromOnlineCatalog(config_, execLogs_, progressCb);
    rebuildFilteredItems();
    canvas_.clearImageCache();
    isRefreshing_ = false;
    execFinished_ = true;
    lastActivityTime_ = std::time(nullptr);
    render();
}

void PluginManagerView::openRefreshPopup() {
    if (uiState_ != UIState::REFRESH_POPUP) {
        prevUiState_ = uiState_;
        uiState_ = UIState::REFRESH_POPUP;
        render();
    }
}

void PluginManagerView::closeRefreshPopup() {
    if (uiState_ == UIState::REFRESH_POPUP) {
        uiState_ = (prevUiState_ == UIState::REFRESH_POPUP) ? UIState::MENU : prevUiState_;
        render();
    }
}

void PluginManagerView::triggerCacheAllBinaries() {
    uiState_ = UIState::EXEC_SCREEN;
    isRefreshing_ = true;
    animFrame_ = 0;
    execLogs_.clear();
    execLogs_.push_back("=== The KikGen Labs: Full Online Sync & Binary Mirroring ===");
    execFinished_ = false;
    render();

    std::string pingTarget = config_.ping_host.empty() ? "github.com" : config_.ping_host;
    isOnline_ = config::ConfigParser::checkInternetReachable(pingTarget, &execLogs_);
    if (!isOnline_) {
        execLogs_.push_back("[ERROR] No internet access (ping " + pingTarget + " failed).");
        execLogs_.push_back("[ERROR] Caching aborted: internet connection is required to mirror binaries.");
        isRefreshing_ = false;
        execFinished_ = true;
        lastActivityTime_ = std::time(nullptr);
        render();
        return;
    }

    auto progressCb = [this](const std::string& msg) {
        animFrame_++;
        lastActivityTime_ = std::time(nullptr);
        if (msg.rfind("[CACHE", 0) == 0 && !execLogs_.empty() && execLogs_.back().rfind("[CACHE", 0) == 0) {
            execLogs_.back() = msg;
        } else {
            execLogs_.push_back(msg);
        }
        if (execLogs_.size() > 18) {
            execLogs_.erase(execLogs_.begin() + 1);
        }
        render();
    };

    config::ConfigParser::syncFromOnlineCatalog(config_, execLogs_, progressCb);
    config::ConfigParser::cacheAllCatalogBinaries(config_, execLogs_, progressCb);
    config::ConfigParser::refreshInstallStates(config_, &execLogs_);
    rebuildFilteredItems();
    canvas_.clearImageCache();
    isRefreshing_ = false;
    execFinished_ = true;
    lastActivityTime_ = std::time(nullptr);
    render();
}

void PluginManagerView::triggerCacheCurrentItem() {
    if (filteredItems_.empty()) return;
    const auto& ref = filteredItems_[selectedIndex_];
    auto& item = config_.sections[ref.sectionIdx].items[ref.itemIdx];

    uiState_ = UIState::EXEC_SCREEN;
    animFrame_ = 0;
    execLogs_.clear();
    execLogs_.push_back("=== The KikGen Labs: Download & Cache " + item.label + " ===");
    execFinished_ = false;
    render();

    std::string pingTarget = config_.ping_host.empty() ? "github.com" : config_.ping_host;
    isOnline_ = config::ConfigParser::checkInternetReachable(pingTarget, &execLogs_);
    if (!isOnline_) {
        execLogs_.push_back("[ERROR] No internet access (ping " + pingTarget + " failed).");
        execLogs_.push_back("[ERROR] Download aborted: internet connection is required.");
        isRefreshing_ = false;
        execFinished_ = true;
        lastActivityTime_ = std::time(nullptr);
        render();
        return;
    }

    auto stepCb = [this](const std::string& line) {
        animFrame_++;
        lastActivityTime_ = std::time(nullptr);
        if (line.rfind("[CACHE]", 0) == 0 && !execLogs_.empty() && execLogs_.back().rfind("[CACHE]", 0) == 0) {
            execLogs_.back() = line;
        } else {
            execLogs_.push_back(line);
        }
        while (execLogs_.size() > 18) {
            execLogs_.erase(execLogs_.begin() + 1);
        }
        render();
    };

    if (config::ConfigParser::cacheItemBinary(item, execLogs_, stepCb)) {
        item.isCached = true;
        config::ConfigParser::isItemBinaryCached(item, &item.cachedZipSize);
        execLogs_.push_back("[TKGL-CACHE] " + item.label + " is now available for offline install.");
    }
    execFinished_ = true;
    lastActivityTime_ = std::time(nullptr);
    render();
}

void PluginManagerView::triggerDeleteCurrentItemBinary() {
    if (filteredItems_.empty()) return;
    const auto& ref = filteredItems_[selectedIndex_];
    auto& item = config_.sections[ref.sectionIdx].items[ref.itemIdx];

    uiState_ = UIState::EXEC_SCREEN;
    animFrame_ = 0;
    execLogs_.clear();
    execLogs_.push_back("=== The KikGen Labs: Purging Cached Binary for " + item.label + " ===");
    execFinished_ = false;
    render();

    if (config::ConfigParser::deleteItemBinary(item, execLogs_)) {
        item.isCached = false;
        item.cachedZipSize.clear();
        execLogs_.push_back("[TKGL-CACHE] Local archive removed. (Installed plugin files in /VST2 Plugins remain untouched).");
    }
    execFinished_ = true;
    lastActivityTime_ = std::time(nullptr);
    render();
}

void PluginManagerView::openInstallPopup() {
    synthLocations_ = config::ConfigParser::getSynthContentLocations();
    selectedSynthLocIdx_ = 0;
    uiState_ = UIState::INSTALL_POPUP;
    render();
}

void PluginManagerView::closeInstallPopup() {
    uiState_ = UIState::LAUNCH_SCREEN;
    render();
}

void PluginManagerView::confirmInstall() {
    if (filteredItems_.empty()) return;
    const auto& ref = filteredItems_[selectedIndex_];
    const auto& sec = config_.sections[ref.sectionIdx];
    const auto& item = sec.items[ref.itemIdx];
    std::string targetDir = (!synthLocations_.empty() && selectedSynthLocIdx_ >= 0 && selectedSynthLocIdx_ < static_cast<int>(synthLocations_.size()))
        ? synthLocations_[selectedSynthLocIdx_].path
        : "./VST2 Plugins";

    uiState_ = UIState::EXEC_SCREEN;
    animFrame_ = 0;
    execLogs_.clear();
    execLogs_.push_back("=== The KikGen Labs: Deploying " + item.label + " " + item.version + " ===");
    execFinished_ = false;
    render();

    auto stepCb = [this](const std::string& line) {
        animFrame_++;
        lastActivityTime_ = std::time(nullptr);
        if (line.rfind("[TKGL-NET] Downloading", 0) == 0) {
            if (!execLogs_.empty() && execLogs_.back().rfind("[TKGL-NET] Downloading", 0) == 0) {
                execLogs_.back() = line;
            } else {
                execLogs_.push_back(line);
            }
        }
        while (execLogs_.size() > 18) {
            execLogs_.erase(execLogs_.begin() + 1);
        }
        render();
    };

    int instRc = config::ConfigParser::executeItemInstall(config_, item, targetDir, execLogs_, stepCb);
    if (instRc != 0) {
        execLogs_.push_back("[ERROR] Installation ABORTED (status " + std::to_string(instRc) + ").");
    }
    config::ConfigParser::refreshInstallStates(config_, &execLogs_);
    config::ConfigParser::saveToFile(config_.config_path, config_);
    rebuildFilteredItems();
    execFinished_ = true;
    lastActivityTime_ = std::time(nullptr);
    render();
}

void PluginManagerView::triggerUninstall() {
    if (filteredItems_.empty()) return;
    const auto& ref = filteredItems_[selectedIndex_];
    const auto& sec = config_.sections[ref.sectionIdx];
    const auto& item = sec.items[ref.itemIdx];

    uiState_ = UIState::EXEC_SCREEN;
    animFrame_ = 0;
    execLogs_.clear();
    execLogs_.push_back("=== The KikGen Labs: Uninstalling " + item.label + " ===");
    execFinished_ = false;
    render();

    auto stepCb = [this](const std::string& line) {
        (void)line;
        animFrame_++;
        lastActivityTime_ = std::time(nullptr);
        if (execLogs_.size() > 18) {
            execLogs_.erase(execLogs_.begin() + 1);
        }
        render();
    };

    config::ConfigParser::executeItemUninstall(config_, item, execLogs_, stepCb);
    config::ConfigParser::refreshInstallStates(config_, &execLogs_);
    config::ConfigParser::saveToFile(config_.config_path, config_);
    rebuildFilteredItems();
    execFinished_ = true;
    lastActivityTime_ = std::time(nullptr);
    render();
}

void PluginManagerView::openExitPopup() {
    if (uiState_ != UIState::EXIT_POPUP) {
        prevUiState_ = uiState_;
        uiState_ = UIState::EXIT_POPUP;
        render();
    }
}

void PluginManagerView::closeExitPopup() {
    if (uiState_ == UIState::EXIT_POPUP) {
        uiState_ = (prevUiState_ == UIState::EXIT_POPUP) ? UIState::MENU : prevUiState_;
        render();
    }
}

void PluginManagerView::openPurgePopup() {
    if (uiState_ != UIState::PURGE_POPUP) {
        prevUiState_ = uiState_;
        uiState_ = UIState::PURGE_POPUP;
        render();
    }
}

void PluginManagerView::closePurgePopup() {
    if (uiState_ == UIState::PURGE_POPUP) {
        uiState_ = (prevUiState_ == UIState::PURGE_POPUP) ? UIState::LAUNCH_SCREEN : prevUiState_;
        render();
    }
}

void PluginManagerView::openSettingsScreen() {
    prevUiState_ = uiState_;
    uiState_ = UIState::SETTINGS_SCREEN;
    settingsStatusMsg_.clear();
    settingsStatusColor_ = 0;
    refreshMpcBackups();
    if (bootLogLines_.empty()) {
        refreshBootLogs();
    }
    render();
}

void PluginManagerView::closeSettingsScreen() {
    uiState_ = (prevUiState_ == UIState::SETTINGS_SCREEN || prevUiState_ == UIState::V310_WARNING_POPUP)
        ? UIState::MENU
        : prevUiState_;
    render();
}

void PluginManagerView::openSanityChecksFromWarning() {
    prevUiState_ = UIState::MENU;
    settingsTab_ = 1;
    mpcSettingsSubTab_ = 2;
    uiState_ = UIState::SETTINGS_SCREEN;
    settingsStatusMsg_.clear();
    settingsStatusColor_ = 0;
    refreshMpcBackups();
    refreshSanityChecks();
    render();
}

void PluginManagerView::closeV310WarningPopup() {
    if (uiState_ == UIState::V310_WARNING_POPUP) {
        uiState_ = UIState::MENU;
        render();
    }
}

void PluginManagerView::refreshMpcBackups() {
    mpcBackups_ = config::ConfigParser::listMpcSettingsBackups();
    if (selectedBackupIdx_ >= static_cast<int>(mpcBackups_.size())) {
        selectedBackupIdx_ = mpcBackups_.empty() ? 0 : static_cast<int>(mpcBackups_.size()) - 1;
    }
}

void PluginManagerView::refreshBootLogs() {
    bootLogLines_ = config::ConfigParser::readSystemBootLogs(300);
    int maxVisible = 16;
    bootLogScrollOffset_ = std::max(0, static_cast<int>(bootLogLines_.size()) - maxVisible);
}

void PluginManagerView::refreshSanityChecks() {
    sanityIssues_ = config::ConfigParser::runSanityChecks(config_);
    if (selectedSanityIdx_ >= static_cast<int>(sanityIssues_.size())) {
        selectedSanityIdx_ = sanityIssues_.empty() ? 0 : static_cast<int>(sanityIssues_.size()) - 1;
    }
    sanityScrollOffset_ = 0;
}

void PluginManagerView::triggerRepair() {
    // Repair corresponds to a clean reinstallation per user specification (allows selecting destination SynthLocation)
    openInstallPopup();
}

void PluginManagerView::openRestoreBackupPopup() {
    if (mpcBackups_.empty() || selectedBackupIdx_ < 0 || selectedBackupIdx_ >= static_cast<int>(mpcBackups_.size())) {
        settingsStatusMsg_ = "No backup selected to restore.";
        settingsStatusColor_ = 2; // red
        render();
        return;
    }
    prevUiState_ = uiState_;
    uiState_ = UIState::RESTORE_BACKUP_POPUP;
    render();
}

void PluginManagerView::closeRestoreBackupPopup() {
    uiState_ = UIState::SETTINGS_SCREEN;
    render();
}

void PluginManagerView::openDeleteBackupPopup() {
    if (mpcBackups_.empty() || selectedBackupIdx_ < 0 || selectedBackupIdx_ >= static_cast<int>(mpcBackups_.size())) {
        settingsStatusMsg_ = "No backup selected to delete.";
        settingsStatusColor_ = 2; // red
        render();
        return;
    }
    prevUiState_ = uiState_;
    uiState_ = UIState::DELETE_BACKUP_POPUP;
    render();
}

void PluginManagerView::closeDeleteBackupPopup() {
    uiState_ = UIState::SETTINGS_SCREEN;
    render();
}

void PluginManagerView::executeRestoreSelectedBackup() {
    if (mpcBackups_.empty() || selectedBackupIdx_ < 0 || selectedBackupIdx_ >= static_cast<int>(mpcBackups_.size())) {
        settingsStatusMsg_ = "No backup selected to restore.";
        settingsStatusColor_ = 2;
        uiState_ = UIState::SETTINGS_SCREEN;
        render();
        return;
    }
    const auto& backup = mpcBackups_[selectedBackupIdx_];
    std::vector<std::string> logs;
    bool ok = config::ConfigParser::restoreMpcSettingsBackup(backup.fullPath, logs);
    if (ok) {
        settingsStatusMsg_ = "Restored: " + backup.filename;
        settingsStatusColor_ = 1; // green
        config::ConfigParser::refreshInstallStates(config_, &logs);
        config::ConfigParser::saveToFile(config_.config_path, config_);
        rebuildFilteredItems();
        refreshMpcBackups();
    } else {
        settingsStatusMsg_ = "Restore failed! See logs.";
        settingsStatusColor_ = 2; // red
    }
    uiState_ = UIState::SETTINGS_SCREEN;
    render();
}

void PluginManagerView::executeDeleteSelectedBackup() {
    if (mpcBackups_.empty() || selectedBackupIdx_ < 0 || selectedBackupIdx_ >= static_cast<int>(mpcBackups_.size())) {
        settingsStatusMsg_ = "No backup selected to delete.";
        settingsStatusColor_ = 2;
        uiState_ = UIState::SETTINGS_SCREEN;
        render();
        return;
    }
    std::string bName = mpcBackups_[selectedBackupIdx_].filename;
    std::string bPath = mpcBackups_[selectedBackupIdx_].fullPath;
    std::vector<std::string> logs;
    bool ok = config::ConfigParser::deleteMpcSettingsBackup(bPath, logs);
    if (ok) {
        settingsStatusMsg_ = "Deleted: " + bName;
        settingsStatusColor_ = 1; // green
        refreshMpcBackups();
        if (selectedBackupIdx_ >= static_cast<int>(mpcBackups_.size())) {
            selectedBackupIdx_ = mpcBackups_.empty() ? 0 : static_cast<int>(mpcBackups_.size()) - 1;
        }
        int maxOffset = std::max(0, static_cast<int>(mpcBackups_.size()) - 4);
        if (backupScrollOffset_ > maxOffset) backupScrollOffset_ = maxOffset;
    } else {
        settingsStatusMsg_ = "Delete failed!";
        settingsStatusColor_ = 2; // red
    }
    uiState_ = UIState::SETTINGS_SCREEN;
    render();
}

void PluginManagerView::render() {
    if (isScreensaver_) {
        drawScreensaver();
        canvas_.blitToFramebuffer();
        return;
    }
    if (uiState_ == UIState::SETTINGS_SCREEN) {
        drawSettingsScreen();
        canvas_.blitToFramebuffer();
        return;
    }
    if (uiState_ == UIState::RESTORE_BACKUP_POPUP) {
        drawSettingsScreen();
        drawRestoreBackupPopup();
        canvas_.blitToFramebuffer();
        return;
    }
    if (uiState_ == UIState::DELETE_BACKUP_POPUP) {
        drawSettingsScreen();
        drawDeleteBackupPopup();
        canvas_.blitToFramebuffer();
        return;
    }
    canvas_.clear(Palette::BgDarkest);
    drawTopHeader();
    if (uiState_ == UIState::MENU) {
        drawFilterBar();
        drawListView();
        drawBottomPaginationBar();
    } else if (uiState_ == UIState::LAUNCH_SCREEN) {
        drawDetailView();
        drawBottomPaginationBar();
    } else if (uiState_ == UIState::INSTALL_POPUP) {
        drawDetailView();
        drawInstallPopup();
    } else if (uiState_ == UIState::EXIT_POPUP) {
        if (prevUiState_ == UIState::LAUNCH_SCREEN) {
            drawDetailView();
        } else {
            drawFilterBar();
            drawListView();
        }
        drawBottomPaginationBar();
        drawExitPopup();
    } else if (uiState_ == UIState::REFRESH_POPUP) {
        if (prevUiState_ == UIState::LAUNCH_SCREEN) {
            drawDetailView();
        } else {
            drawFilterBar();
            drawListView();
        }
        drawBottomPaginationBar();
        drawRefreshPopup();
    } else if (uiState_ == UIState::PURGE_POPUP) {
        drawDetailView();
        drawBottomPaginationBar();
        drawPurgePopup();
    } else if (uiState_ == UIState::V310_WARNING_POPUP) {
        drawFilterBar();
        drawListView();
        drawBottomPaginationBar();
        drawV310WarningPopup();
    } else {
        drawExecutionScreen();
    }
    canvas_.blitToFramebuffer();
}

void PluginManagerView::drawTopHeader() {
    int W = canvas_.getLogicalWidth();
    canvas_.fillRect({0, 0, W, 62}, Palette::BgHeader);
    canvas_.drawHLine(0, 61, W, Palette::BorderLight);

    // 1. Poloq 4-Square Rounded Icon (3 grey rounded squares + 1 Akai Red square)
    Rect iconBox = {22, 11, 40, 40};
    canvas_.drawPanel(iconBox, 8, Color(35, 35, 41), Color(58, 58, 68), 1.0f);
    canvas_.fillRoundedRect({29, 18, 11, 11}, 3, Color(161, 161, 170));
    canvas_.fillRoundedRect({44, 18, 11, 11}, 3, Color(161, 161, 170));
    canvas_.fillRoundedRect({29, 33, 11, 11}, 3, Color(161, 161, 170));
    canvas_.fillRoundedRect({44, 33, 11, 11}, 3, Palette::AkaiCrimson);

    // 2. Main Title + "By The KikGen Labs - V1.2.23" on same baseline (matching image_22ce5693.png)
    int titleX = 76;
    canvas_.drawTextClipped({titleX, 21, 380, 22}, config_.app_title, Palette::TextPrimary, 2, true);
    int titleW = std::min(380, canvas_.measureText(config_.app_title, 2) + 14);
    canvas_.drawText(titleX + titleW, 26, "By The KikGen Labs - V1.2.23", Palette::TextSecondary, 1, false);

    // 3. REFRESH Button with Circular Sync Arrow Icon
    Rect refreshBtn = {W - 442, 13, 128, 36};
    canvas_.drawPanel(refreshBtn, 8, Color(35, 35, 44), Color(62, 62, 76), 1.0f);
    canvas_.drawRefreshIcon(refreshBtn.x + 14, refreshBtn.y + 10, 16, isRefreshing_ ? Palette::AmberGold : Palette::StatusGreen);
    if (isRefreshing_) {
        int dotCount = (animFrame_ % 5) + 1;
        std::string dotPill;
        for (int d = 0; d < 5; ++d) dotPill += (d < dotCount) ? "." : " ";
        canvas_.drawText(refreshBtn.x + 36, refreshBtn.y + 12, "SYNC " + dotPill, Palette::AmberGold, 1, true);
    } else {
        canvas_.drawText(refreshBtn.x + 38, refreshBtn.y + 12, "REFRESH", Palette::TextPrimary, 1, true);
    }

    // 4. Glowing ONLINE / OFFLINE Status Dot + Label (non-interactive indicator)
    if (isOnline_) {
        canvas_.drawGlowDot(W - 292, 31, 5, Palette::StatusGreen);
        canvas_.drawText(W - 278, 23, "ONLINE", Palette::TextPrimary, 2, true);
    } else {
        canvas_.drawGlowDot(W - 292, 31, 5, Palette::AkaiCrimson);
        canvas_.drawText(W - 278, 23, "OFFLINE", Palette::TextSecondary, 2, true);
    }

    // 5. USB /Synth Free Space Progress Pill + GB Free Label
    double freeGB = 202.8;
    int usedPct = 24;
    struct statvfs vfs;
    std::string statDir = config::ConfigParser::getManagerRootDir();
    if (statDir.empty()) statDir = ".";
    if (statvfs(statDir.c_str(), &vfs) == 0 && vfs.f_blocks > 0) {
        double totalBytes = static_cast<double>(vfs.f_blocks) * vfs.f_frsize;
        double freeBytes  = static_cast<double>(vfs.f_bavail) * vfs.f_frsize;
        freeGB = freeBytes / (1024.0 * 1024.0 * 1024.0);
        usedPct = std::max(6, std::min(98, static_cast<int>(((totalBytes - freeBytes) / totalBytes) * 100.0)));
    }

    Rect trackR = {W - 184, 16, 160, 8};
    canvas_.fillRoundedRect(trackR, 4, Color(38, 38, 46));
    int fillW = std::max(12, (trackR.w * usedPct) / 100);
    canvas_.fillRoundedRect({trackR.x, trackR.y, fillW, trackR.h}, 4, Palette::StatusGreen);

    char freeBuf[64];
    std::snprintf(freeBuf, sizeof(freeBuf), "%.1f GB free", freeGB);
    canvas_.drawTextRight(W - 24, 32, freeBuf, Palette::TextSecondary, 1, false);
}

void PluginManagerView::drawFilterBar() {
    int W = canvas_.getLogicalWidth();
    // Row 1: DISCOVER | INSTALLED | UPDATES + Sort Pill (y = 62..108)
    canvas_.fillRect({0, 62, W, 46}, Palette::BgFilterBar);
    canvas_.drawHLine(0, 107, W, Palette::BorderDark);

    Rect segGroup = {22, 67, 432, 36};
    canvas_.drawPanel(segGroup, 8, Color(18, 18, 22), Palette::BorderLight, 1.0f);

    int installedCount = 0;
    int updatesCount = 0;
    for (const auto& s : config_.sections) {
        for (const auto& it : s.items) {
            if (it.installed) installedCount++;
            if (it.updateAvail) updatesCount++;
        }
    }

    std::string tabs[3] = {
        "DISCOVER",
        installedCount > 0 ? ("INSTALLED (" + std::to_string(installedCount) + ")") : "INSTALLED",
        updatesCount > 0 ? ("UPDATES (" + std::to_string(updatesCount) + ")") : "UPDATES"
    };

    for (int t = 0; t < 3; ++t) {
        bool isAct = (static_cast<int>(activeTab_) == t);
        Rect tr = {26 + t * 142, 70, 138, 30};
        if (isAct) {
            canvas_.fillRoundedRect(tr, 6, Palette::AkaiRed);
        }
        canvas_.drawTextCentered(tr, tabs[t], isAct ? Palette::TextPrimary : Palette::TextSecondary, 1, isAct);
    }

    // Level 3 Category Filter Pill: [ ▽ <Category> ▼ ] (matching image_54d88661.png)
    Rect catRect = {W - 430, 68, 210, 34};
    canvas_.drawPanel(catRect, 7, Color(37, 37, 44), Color(58, 58, 68), 1.0f);
    canvas_.drawFilterIcon(catRect.x + 12, catRect.y + 10, 14, Color(161, 161, 170));
    std::string catText = categoryFilter_;
    canvas_.drawTextClipped({catRect.x + 32, catRect.y + 10, catRect.w - 52, 16}, catText, Palette::TextPrimary, 1, true);
    canvas_.drawDownTriangle(catRect.x + catRect.w - 16, catRect.y + 14, 8, Color(140, 140, 150));

    // Sort Button [ ⇅ Sort: <SORT> ▼ ]
    Rect sortRect = {W - 210, 68, 188, 34};
    canvas_.drawPanel(sortRect, 7, Color(37, 37, 44), Color(58, 58, 68), 1.0f);
    canvas_.drawSortIcon(sortRect.x + 12, sortRect.y + 10, 14, Color(161, 161, 170));
    const char* sortNames[4] = {"UPDATED", "NAME", "SIZE", "POPULAR"};
    std::string sortLabel = std::string("Sort: ") + sortNames[sortBy_];
    canvas_.drawTextClipped({sortRect.x + 32, sortRect.y + 10, sortRect.w - 52, 16}, sortLabel, Palette::TextSecondary, 1, true);
    canvas_.drawDownTriangle(sortRect.x + sortRect.w - 16, sortRect.y + 14, 8, Color(140, 140, 150));

    // Row 2: SECTION CLASSIFICATION TAB BAR matching image_6572f34f.png (y = 108..148)
    // Tabs: ALL | INSTRUMENTS | MIDI | EFFECTS | ADDINS | DEVICE PATCHES | COMPANION APPS
    canvas_.fillRect({0, 108, W, 40}, Color(20, 20, 24));
    canvas_.drawHLine(0, 147, W, Palette::BorderLight);

    int totalAll = 0;
    for (const auto& s : config_.sections) {
        for (const auto& it : s.items) {
            if (activeTab_ == FilterTab::INSTALLED && (it.installed == 0)) continue;
            if (activeTab_ == FilterTab::UPDATES && !it.updateAvail) continue;
            totalAll++;
        }
    }

    int tabX = 22;
    int numSec = static_cast<int>(config_.sections.size());
    for (int idx = -1; idx < numSec; ++idx) {
        bool isSelected = (sectionFilter_ == idx);
        std::string title = (idx < 0) ? "ALL" : config_.sections[idx].title;
        int cnt = 0;
        if (idx < 0) {
            cnt = totalAll;
        } else {
            for (const auto& it : config_.sections[idx].items) {
                if (activeTab_ == FilterTab::INSTALLED && (it.installed == 0)) continue;
                if (activeTab_ == FilterTab::UPDATES && !it.updateAvail) continue;
                cnt++;
            }
        }
        std::string cntStr = std::to_string(cnt);

        int textW = canvas_.measureText(title, 1);
        int pillW = std::max(24, canvas_.measureText(cntStr, 1) + 12);
        int tabW = textW + pillW + 26;
        if (tabX + tabW > W - 16) break;

        Rect tabR = {tabX, 112, tabW, 35};
        if (isSelected) {
            canvas_.drawPanel(tabR, 6, Color(32, 32, 40), Color(56, 56, 70), 1.0f);
            // Red Top Indicator Bar (matching image_6572f34f.png)
            canvas_.fillRoundedRect({tabR.x + 2, tabR.y, tabR.w - 4, 3}, 1, Palette::AkaiCrimson);
        }

        canvas_.drawText(
            tabR.x + 10,
            tabR.y + 12,
            title,
            isSelected ? Palette::TextPrimary : Palette::TextSecondary,
            1,
            isSelected
        );

        Rect cntPill = {tabR.x + 14 + textW, tabR.y + 8, pillW, 19};
        canvas_.drawPanel(
            cntPill,
            9,
            isSelected ? Color(50, 50, 64) : Color(31, 31, 39),
            isSelected ? Color(78, 78, 96) : Color(46, 46, 58),
            1.0f
        );
        canvas_.drawTextCentered(
            cntPill,
            cntStr,
            isSelected ? Palette::TextPrimary : Palette::TextSecondary,
            1,
            isSelected
        );

        tabX += tabW + 6;
    }
}

void PluginManagerView::drawCardThumbnail(Rect r, const config::MenuItem& item, int index, bool large) {
    // 1. Try loading bundled or online-cached PNG / PPM screenshot with rounded corners
    bool loadedImg = canvas_.drawImageRounded(r, 9, item.thumbPpmPath, item.screenshotUrl, item.id);

    if (!loadedImg) {
        // 2. High-detail procedural dark VST synth skin (mimics RMXXXL / Poloq screenshots)
        canvas_.drawPanel(r, 9, Color(14, 15, 20), Color(58, 58, 70), 1.0f);

        Color accent = (index % 3 == 0) ? Color(234, 88, 12) :
                       (index % 3 == 1) ? Palette::StatusCyan : Palette::AkaiCrimson;

        // Top dark header strip inside synth UI
        canvas_.fillRoundedRect({r.x + 6, r.y + 6, r.w - 12, 22}, 5, Color(24, 26, 34));
        canvas_.drawHLine(r.x + 8, r.y + 28, r.w - 16, Color(42, 44, 56));

        // 6 Anti-Aliased Rotary Knobs with Metallic Caps & Indicator Ticks
        int numKnobs = large ? 6 : 4;
        int knobStep = (r.w - 24) / numKnobs;
        int knobY = r.y + (large ? 58 : 54);
        int knobR = large ? 14 : 11;

        for (int k = 0; k < numKnobs; ++k) {
            int kcx = r.x + 12 + knobStep / 2 + k * knobStep;
            // Outer dark ring + tick marks
            for (int deg = -135; deg <= 135; deg += 30) {
                float rad = (deg - 90) * 3.14159265f / 180.0f;
                int tx = kcx + static_cast<int>(std::cos(rad) * (knobR + 4));
                int ty = knobY + static_cast<int>(std::sin(rad) * (knobR + 4));
                canvas_.putPixel(tx, ty, Color(140, 140, 155));
            }
            // Knob cap
            Rect capR = {kcx - knobR, knobY - knobR, knobR * 2, knobR * 2};
            canvas_.drawPanel(capR, knobR, Color(38, 40, 48), Color(180, 180, 195), 1.2f);
            // Indicator line
            float valDeg = (-110 + ((k * 47 + index * 29) % 220) - 90) * 3.14159265f / 180.0f;
            canvas_.drawLineAA(kcx, knobY,
                               kcx + std::cos(valDeg) * (knobR - 2),
                               knobY + std::sin(valDeg) * (knobR - 2),
                               Color(255, 255, 255), 1.8f);
            // Tiny parameter underline bar
            canvas_.fillRoundedRect({kcx - knobR, knobY + knobR + 8, knobR * 2, 4}, 2, Color(45, 50, 64));
            canvas_.fillRoundedRect({kcx - knobR, knobY + knobR + 8, knobR + (k % 2) * 5, 4}, 2, Color(56, 189, 248));
        }

        // Bottom performance pads / step sequencer buttons inside synth preview
        if (large) {
            int padW = (r.w - 36) / 2;
            Rect pad1 = {r.x + 12, r.y + r.h - 34, padW, 24};
            Rect pad2 = {r.x + 18 + padW, r.y + r.h - 34, padW, 24};
            canvas_.drawPanel(pad1, 5, Color(146, 64, 14), Color(217, 119, 6), 1.0f);
            canvas_.drawTextCentered(pad1, "RELEASE FX", Palette::TextPrimary, 1, true);
            canvas_.drawPanel(pad2, 5, Color(127, 29, 29), Palette::AkaiRed, 1.0f);
            canvas_.drawTextCentered(pad2, "PANIC", Palette::TextPrimary, 1, true);
        } else {
            for (int b = 0; b < 8; ++b) {
                int bw = (r.w - 86) / 8;
                Rect sb = {r.x + 10 + b * (bw + 2), r.y + r.h - 24, bw, 14};
                canvas_.drawPanel(sb, 3, (b % 3 == 0) ? accent : Color(32, 34, 42), Color(62, 64, 76), 1.0f);
            }
        }
    }

    // Top-Left Translucent Version Pill (e.g. [v1.2.2] matching image_22ce5693.png)
    int verW = std::max(48, canvas_.measureText(item.version, 1) + 14);
    Rect verPill = {r.x + 8, r.y + 8, verW, 20};
    canvas_.drawPanel(verPill, 5, Color(10, 10, 14, 220), Color(68, 68, 82), 1.0f);
    canvas_.drawTextCentered(verPill, item.version, Palette::TextPrimary, 1, false);

    // Bottom-Right Translucent Type Badge (e.g. [FX] / [INST] / [MIDI] matching image_22ce5693.png)
    int badgeW = std::max(36, canvas_.measureText(item.shortType, 1) + 16);
    Rect badgeR = {r.x + r.w - badgeW - 8, r.y + r.h - 28, badgeW, 20};
    canvas_.drawPanel(badgeR, 5, Color(12, 12, 16, 235), Color(82, 82, 98), 1.0f);
    canvas_.drawTextCentered(badgeR, item.shortType, Palette::TextPrimary, 1, true);
}

void PluginManagerView::drawListView() {
    int W = canvas_.getLogicalWidth();
    if (filteredItems_.empty()) {
        canvas_.drawTextCentered({0, 220, W, 200}, "No plugins match the current filter.", Palette::TextSecondary, 2, false);
        return;
    }

    int currentPage = selectedIndex_ / itemsPerPage_;
    int startIdx = currentPage * itemsPerPage_;
    int endIdx = std::min(static_cast<int>(filteredItems_.size()), startIdx + itemsPerPage_);

    int cardY = 158;
    int cardH = 180;

    for (int idx = startIdx; idx < endIdx; ++idx) {
        bool isSelected = (idx == selectedIndex_);
        const auto& ref = filteredItems_[idx];
        const auto& item = config_.sections[ref.sectionIdx].items[ref.itemIdx];

        Rect cardR = {22, cardY, W - 44, cardH};
        canvas_.drawPanel(
            cardR,
            12,
            isSelected ? Palette::BgCardSelected : Palette::BgCard,
            isSelected ? Palette::AkaiCrimson : Palette::BorderLight,
            isSelected ? 2.0f : 1.0f
        );

        // 1. Left Thumbnail (212 x 152)
        drawCardThumbnail({cardR.x + 14, cardR.y + 14, 212, cardH - 28}, item, idx, false);

        // 2. Center Column -> strictly confined to x: 244..990
        int infoX = cardR.x + 244;
        int infoW = 735;

        canvas_.drawTextClipped({infoX, cardR.y + 14, infoW, 22}, item.label, Palette::TextPrimary, 2, true);

        std::string subline = "by " + item.author + "  .  [" + config_.sections[ref.sectionIdx].title + "]  .  " + item.category;
        canvas_.drawTextClipped({infoX, cardR.y + 42, infoW, 16}, subline, Palette::TextSecondary, 1, false);

        // 2-line wrapped description inside the card
        canvas_.drawTextWrapped({infoX, cardR.y + 66, infoW, 54}, item.description, Palette::TextPrimary, 2, 5);

        // Bottom badges: [Stable] + [✓ Tested: ...] clamped to infoW
        Rect chanR = {infoX, cardR.y + cardH - 40, 76, 26};
        canvas_.drawPanel(chanR, 6, Color(20, 20, 24), Palette::BorderLight, 1.0f);
        canvas_.drawTextCentered(chanR, item.channel, Palette::TextSecondary, 1, false);

        if (item.installed == 2) {
            Rect brokenR = {infoX + 86, cardR.y + cardH - 40, 232, 26};
            canvas_.drawPanel(brokenR, 6, Color(68, 22, 22), Palette::AkaiCrimson, 1.5f);
            canvas_.drawTextCentered(brokenR, "! BROKEN INSTALL (.SO MISSING)", Palette::AmberGold, 1, true);
        } else {
            std::string testedStr = "Tested: " + item.testedDevice;
            int desiredTestedW = canvas_.measureText(testedStr, 1) + 34;
            int clampedTestedW = std::min(infoW - 88, std::max(150, desiredTestedW));
            Rect testR = {infoX + 86, cardR.y + cardH - 40, clampedTestedW, 26};
            canvas_.drawPanel(testR, 6, Palette::StatusGreenBg, Palette::StatusGreenBdr, 1.0f);
            canvas_.drawCheckmarkIcon(testR.x + 8, testR.y + 6, 13, Palette::StatusGreen);
            canvas_.drawTextClipped({testR.x + 25, testR.y + 7, testR.w - 31, 14}, testedStr, Palette::StatusGreen, 1, true);
        }

        // 3. Right Metadata & Action Button Column -> x: 1000..1240
        int rightEdge = cardR.x + cardR.w - 18;
        int rightMinX = cardR.x + 985;
        canvas_.drawTextRightClipped(rightMinX, rightEdge, cardR.y + 16, item.version + "  " + item.sizeMb, Palette::TextPrimary, 2, true);
        canvas_.drawTextRightClipped(rightMinX, rightEdge, cardR.y + 42, "Updated " + item.updatedDate, Palette::TextSecondary, 1, false);

        Rect btnR = {rightEdge - 168, cardR.y + cardH - 56, 168, 42};
        if (item.installed == 2) {
            canvas_.drawPanel(btnR, 8, Color(64, 34, 12), Palette::AmberGold, 1.5f);
            canvas_.drawRefreshIcon(btnR.x + 20, btnR.y + 13, 15, Palette::AmberGold);
            canvas_.drawText(btnR.x + 44, btnR.y + 12, "REPAIR", Palette::AmberGold, 2, true);
        } else if (item.installed == 1) {
            canvas_.drawPanel(btnR, 8, Palette::StatusGreenBg, Palette::StatusGreenBdr, 1.5f);
            canvas_.drawCheckmarkIcon(btnR.x + 18, btnR.y + 13, 15, Palette::StatusGreen);
            canvas_.drawText(btnR.x + 42, btnR.y + 12, "INSTALLED", Palette::StatusGreen, 2, true);
        } else {
            canvas_.fillRoundedRect(btnR, 8, Palette::AkaiRed);
            canvas_.drawDownloadIcon(btnR.x + 24, btnR.y + 12, 16, Palette::TextPrimary);
            canvas_.drawText(btnR.x + 48, btnR.y + 12, "INSTALL", Palette::TextPrimary, 2, true);
        }

        cardY += cardH + 10;
    }
}

// ============================================================================
// High-Definition Detail View (1:1 Match with image_22ce5693.png)
// ============================================================================
void PluginManagerView::drawDetailView() {
    int W = canvas_.getLogicalWidth();
    if (filteredItems_.empty()) return;
    const auto& ref = filteredItems_[selectedIndex_];
    const auto& sec = config_.sections[ref.sectionIdx];
    const auto& item = sec.items[ref.itemIdx];

    // 1. Top Detail Sub-Header Row (y = 74..134, divider at y = 142)
    Rect backBtn = {24, 80, 106, 42};
    canvas_.drawPanel(backBtn, 8, Palette::BgButton, Color(58, 58, 70), 1.0f);
    canvas_.drawChevronIcon(backBtn.x + 14, backBtn.y + 13, 15, true, Palette::TextPrimary);
    canvas_.drawText(backBtn.x + 36, backBtn.y + 12, "BACK", Palette::TextPrimary, 2, true);

    // Bold Plugin Title + Subtitle ("by Author · Category")
    // With redundant badges removed, title area gains significant horizontal space
    int maxTitleW = (item.installed > 0) ? (W - 480) : (W - 320);
    canvas_.drawTextClipped({146, 76, maxTitleW, 26}, item.label, Palette::TextPrimary, 3, true);
    std::string subTitle = "by " + item.author + "  .  " + item.category;
    canvas_.drawTextClipped({146, 107, maxTitleW, 16}, subTitle, Palette::TextSecondary, 2, false);

    // Top-Right Action Buttons:
    // User Specification:
    // - If ZIP is present locally: Display a button "ZIP: <size in MB>". Pressing it opens confirmation to purge it.
    // - If ZIP is absent locally: Do NOT display "NO LOCAL ZIP", display interactive "CACHE ZIP" button.
    // - If broken (installed == 2): [ BROKEN INSTALL ] badge + Amber [ REPAIR ] button
    // - If installed (installed == 1): [ UNINSTALL ] button close to [ REINSTALL ] button
    // - If not installed (installed == 0): Red [ INSTALL ] button
    std::string cachedZipSize;
    bool isCached = config::ConfigParser::isItemBinaryCached(item, &cachedZipSize);

    // Cache / Purge ZIP Button (Consolidated Single Button: "ZIP: <size>" or "CACHE ZIP")
    Rect zipBtn = {W - 496, 79, 130, 44};
    if (isCached) {
        canvas_.drawPanel(zipBtn, 8, Color(16, 38, 48), Palette::StatusCyan, 1.2f);
        canvas_.drawTextCentered(zipBtn, "ZIP: " + cachedZipSize, Palette::StatusCyan, 1, true);
    } else {
        canvas_.drawPanel(zipBtn, 8, Color(24, 38, 52), Palette::StatusCyan, 1.2f);
        canvas_.drawDownloadIcon(zipBtn.x + 10, zipBtn.y + 14, 15, Palette::StatusCyan);
        canvas_.drawText(zipBtn.x + 30, zipBtn.y + 13, "CACHE ZIP", Palette::StatusCyan, 1, true);
    }

    if (item.installed == 2) {
        // User specification: broken installs can be cleanly uninstalled or repaired
        Rect uninstBtn = {W - 356, 79, 154, 44};
        canvas_.drawPanel(uninstBtn, 8, Color(42, 24, 28), Palette::AkaiCrimson, 1.5f);
        canvas_.drawTextCentered(uninstBtn, "UNINSTALL", Palette::TextPrimary, 2, true);

        Rect repBtn = {W - 186, 79, 162, 44};
        canvas_.drawPanel(repBtn, 8, Color(180, 83, 9), Palette::AmberGold, 1.5f);
        canvas_.drawRefreshIcon(repBtn.x + 22, repBtn.y + 13, 17, Palette::TextPrimary);
        canvas_.drawText(repBtn.x + 48, repBtn.y + 13, "REPAIR", Palette::TextPrimary, 2, true);
    } else if (item.installed == 1) {
        Rect uninstBtn = {W - 356, 79, 154, 44};
        canvas_.drawPanel(uninstBtn, 8, Color(42, 24, 28), Palette::AkaiCrimson, 1.5f);
        canvas_.drawTextCentered(uninstBtn, "UNINSTALL", Palette::TextPrimary, 2, true);

        Rect instBtn = {W - 186, 79, 162, 44};
        canvas_.fillRoundedRect(instBtn, 8, Palette::AkaiRed);
        canvas_.drawDownloadIcon(instBtn.x + 18, instBtn.y + 13, 17, Palette::TextPrimary);
        canvas_.drawText(instBtn.x + 42, instBtn.y + 13, "REINSTALL", Palette::TextPrimary, 2, true);
    } else {
        Rect instBtn = {W - 186, 79, 162, 44};
        canvas_.fillRoundedRect(instBtn, 8, Palette::AkaiRed);
        canvas_.drawDownloadIcon(instBtn.x + 24, instBtn.y + 13, 17, Palette::TextPrimary);
        canvas_.drawText(instBtn.x + 50, instBtn.y + 13, "INSTALL", Palette::TextPrimary, 2, true);
    }

    canvas_.drawHLine(24, 138, W - 48, Palette::BorderLight);

    // ========================================================================
    // LEFT COLUMN (x = 24, w = 382): Large Screenshot + Rounded Specs Table
    // ========================================================================
    int leftX = 24;
    int leftW = 382;

    drawCardThumbnail({leftX, 154, leftW, 208}, item, selectedIndex_, true);

    Rect specsBox = {leftX, 376, leftW, 332};
    canvas_.drawPanel(specsBox, 12, Palette::BgSpecsCard, Palette::BorderLight, 1.0f);

    const char* keys[6] = {"Version", "Size", "Updated", "MPC OS", "Tested On", "Target"};
    std::string vals[6] = {
        item.version,
        item.sizeMb,
        item.updatedDate,
        item.osCompat.empty() ? "MPC OS 3.x" : item.osCompat,
        item.testedDevice,
        "/VST2 Plugins/" + item.targetSubdir
    };

    for (int r = 0; r < 6; ++r) {
        int ry = specsBox.y + 18 + r * 52;
        canvas_.drawText(specsBox.x + 18, ry, keys[r], Palette::TextSecondary, 2, false);

        if (r == 4) {
            // Tested On row in Emerald Green with Checkmark
            std::string tVal = vals[r];
            int maxChars = (specsBox.w - 148) / canvas_.charAdvance(2);
            if (static_cast<int>(tVal.size()) > maxChars && maxChars > 3) {
                tVal = tVal.substr(0, maxChars - 3) + "...";
            }
            int tw = canvas_.measureText(tVal, 2);
            int rx = specsBox.x + specsBox.w - 18 - tw;
            canvas_.drawCheckmarkIcon(rx - 18, ry + 2, 14, Palette::StatusGreen);
            canvas_.drawText(rx, ry, tVal, Palette::StatusGreen, 2, true);
        } else {
            Color valColor = (r == 5) ? Palette::AmberGold : Palette::TextPrimary;
            canvas_.drawTextRightClipped(
                specsBox.x + 124,
                specsBox.x + specsBox.w - 18,
                ry,
                vals[r],
                valColor,
                2,
                true
            );
        }

        if (r < 5) {
            canvas_.drawHLine(specsBox.x + 16, ry + 34, specsBox.w - 32, Palette::BorderDark);
        }
    }

    // ========================================================================
    // RIGHT COLUMN (x = 424, w = 832):
    // 1. Verification & Category Badges + Local ZIP Presence Info
    // 2. Full-Height Rounded Description, Release Notes & #Tags Card
    // ========================================================================
    int rightX = 424;
    int rightW = W - rightX - 24;

    // 1. Top Verification & Category Badges (y = 154)
    Rect vGen1 = {rightX, 154, 142, 28};
    canvas_.drawPanel(vGen1, 6, Palette::StatusGreenBg, Palette::StatusGreenBdr, 1.0f);
    canvas_.drawCheckmarkIcon(vGen1.x + 9, vGen1.y + 7, 13, Palette::StatusGreen);
    canvas_.drawText(vGen1.x + 26, vGen1.y + 8, "VERIFIED GEN1", Palette::StatusGreen, 1, true);

    Rect vGen2 = {rightX + 150, 154, 142, 28};
    canvas_.drawPanel(vGen2, 6, Palette::StatusGreenBg, Palette::StatusGreenBdr, 1.0f);
    canvas_.drawCheckmarkIcon(vGen2.x + 9, vGen2.y + 7, 13, Palette::StatusGreen);
    canvas_.drawText(vGen2.x + 26, vGen2.y + 8, "VERIFIED GEN2", Palette::StatusGreen, 1, true);

    std::string catUpper = item.category;
    for (char& ch : catUpper) if (ch >= 'a' && ch <= 'z') ch -= 32;
    int catW = std::min(190, std::max(100, canvas_.measureText(catUpper, 1) + 24));
    Rect catBadge = {rightX + 300, 154, catW, 28};
    canvas_.drawPanel(catBadge, 6, Color(34, 34, 43), Color(58, 58, 72), 1.0f);
    canvas_.drawTextCentered(catBadge, catUpper, Palette::TextPrimary, 1, true);

    int binW = std::min(160, std::max(84, canvas_.measureText(item.targetBinary, 1) + 24));
    Rect binBadge = {catBadge.x + catBadge.w + 8, 154, binW, 28};
    canvas_.drawPanel(binBadge, 6, Color(34, 34, 43), Color(58, 58, 72), 1.0f);
    canvas_.drawTextCentered(binBadge, item.targetBinary, Palette::TextSecondary, 1, false);

    // 2. LARGE ROUNDED DESCRIPTION, RELEASE NOTES & TAGS CARD (y = 194, h = 514 -> aligns with specsBox bottom at 708)
    Rect descCard = {rightX, 194, rightW, 514};
    canvas_.drawPanel(descCard, 12, Palette::BgDescCard, Color(50, 50, 64), 1.0f);

    // Header inside Description Card
    canvas_.drawText(descCard.x + 18, descCard.y + 16, "PLUGIN DESCRIPTION & ARCHITECTURE", Palette::StatusGreen, 2, true);
    canvas_.drawTextRight(descCard.x + descCard.w - 18, descCard.y + 18, item.downloads + " downloads", Palette::TextSecondary, 1, false);
    canvas_.drawHLine(descCard.x + 18, descCard.y + 42, descCard.w - 36, Palette::BorderDark);

    // Multi-line Smooth Wrapped Description (Primary Focus!)
    canvas_.drawTextWrapped(
        {descCard.x + 18, descCard.y + 56, descCard.w - 36, 250},
        item.description,
        Palette::TextPrimary,
        2,
        7
    );

    // Release Notes & Hardware Mapping Section inside Card
    int relY = descCard.y + 324;
    canvas_.drawHLine(descCard.x + 18, relY, descCard.w - 36, Palette::BorderDark);
    canvas_.drawText(descCard.x + 18, relY + 14, "RELEASE NOTES & HARDWARE MAPPING", Palette::TextSecondary, 1, true);

    std::string skinPathDisplay = !item.skinFolderPath.empty()
        ? item.skinFolderPath
        : ("/media/MPC/VST2 Plugins/" + (!item.targetSubdir.empty() ? item.targetSubdir : item.label));

    std::string releaseLine = "Release " + item.version + " (" + item.updatedDate + ")  .  Binary: " + item.targetBinary;
    std::string skinLine    = "Skin folder: " + skinPathDisplay;

    canvas_.drawText(descCard.x + 18, relY + 34, releaseLine, Color(212, 212, 218), 1, false);
    canvas_.drawText(descCard.x + 18, relY + 54, skinLine, Palette::AmberGold, 1, true);

    // Bottom #TAGS Bar inside Description Card
    int tagsY = descCard.y + descCard.h - 54;
    canvas_.drawHLine(descCard.x + 18, tagsY, descCard.w - 36, Palette::BorderDark);
    canvas_.drawText(descCard.x + 18, tagsY + 20, "TAGS:", Palette::TextSecondary, 1, false);

    int tagX = descCard.x + 68;
    const auto& tagList = item.tags;
    for (size_t t = 0; t < tagList.size(); ++t) {
        std::string label = "#" + tagList[t];
        int pillW = canvas_.measureText(label, 1) + 20;
        if (tagX + pillW > descCard.x + descCard.w - 18) break;
        Rect pillR = {tagX, tagsY + 13, pillW, 26};
        canvas_.drawPanel(pillR, 6, Palette::BgTagPill, Palette::BorderTag, 1.0f);
        canvas_.drawTextCentered(pillR, label, Palette::TextPrimary, 1, false);
        tagX += pillW + 8;
    }
}

void PluginManagerView::drawInstallPopup() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();
    if (filteredItems_.empty()) return;
    const auto& ref = filteredItems_[selectedIndex_];
    const auto& sec = config_.sections[ref.sectionIdx];
    const auto& item = sec.items[ref.itemIdx];

    // Darkened modal backdrop
    canvas_.fillRect({0, 0, W, H}, Color(0, 0, 0, 210));

    // Modal Card (w = 760, h = 640 centered)
    int mw = 760;
    int mh = 640;
    int mx = (W - mw) / 2;
    int my = (H - mh) / 2;
    Rect modalR = {mx, my, mw, mh};
    canvas_.drawPanel(modalR, 16, Color(22, 22, 29), Color(54, 54, 70), 2.0f);

    // Modal Header
    Rect tagR = {mx + 20, my + 18, 128, 26};
    canvas_.fillRoundedRect(tagR, 6, item.installed == 2 ? Color(180, 83, 9) : Palette::AkaiRed);
    canvas_.drawTextCentered(tagR, item.installed == 2 ? "REPAIR POPUP" : "INSTALL POPUP", Palette::TextPrimary, 1, true);

    canvas_.drawTextClipped({mx + 158, my + 18, mw - 300, 26}, item.label + " " + item.version, Palette::TextPrimary, 3, true);
    canvas_.drawTextRight(mx + mw - 20, my + 24, item.shortType + "  .  " + item.channel, Palette::StatusGreen, 1, true);

    std::string subH = "Destination directory from /media/az01-internal/Settings/MPC/MPC.settings";
    canvas_.drawTextClipped({mx + 20, my + 54, mw - 40, 18}, subH, Palette::TextSecondary, 1, false);
    canvas_.drawHLine(mx + 20, my + 78, mw - 40, Palette::BorderDark);

    // Pick List Header
    canvas_.drawText(mx + 20, my + 92, "SynthContentLocations", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(mx + mw - 20, my + 94, "Rotate Wheel to Select", Palette::TextSecondary, 1, false);

    // Pick List Entries (up to 5 entries)
    int rowY = my + 120;
    int rowH = 54;
    for (size_t i = 0; i < synthLocations_.size() && i < 5; ++i) {
        const auto& loc = synthLocations_[i];
        bool isSel = (static_cast<int>(i) == selectedSynthLocIdx_);
        Rect rowR = {mx + 20, rowY, mw - 40, rowH};

        Color bg = isSel ? Color(35, 35, 48) : Color(26, 26, 34);
        Color bdr = isSel ? Palette::AkaiRed : Palette::BorderDark;
        canvas_.drawPanel(rowR, 10, bg, bdr, isSel ? 2.0f : 1.0f);

        // Radio indicator
        int rx = rowR.x + 16;
        int ry = rowR.y + 19;
        canvas_.drawCircleAA(rx + 8, ry + 8, 8.0f, isSel ? Palette::AkaiRed : Palette::TextSecondary, 1.5f);
        if (isSel) {
            canvas_.fillCircleAA(rx + 8, ry + 8, 4.0f, Palette::AkaiRed);
        }

        // Path and Label
        canvas_.drawTextClipped({rowR.x + 44, rowR.y + 10, rowR.w - 180, 20}, loc.path, Palette::TextPrimary, 2, true);
        canvas_.drawTextClipped({rowR.x + 44, rowR.y + 32, rowR.w - 180, 16}, loc.label, Palette::TextSecondary, 1, false);

        // Free Space
        Color fsCol = isSel ? Palette::StatusCyan : Palette::TextSecondary;
        canvas_.drawTextRight(rowR.x + rowR.w - 18, rowR.y + 18, loc.freeSpace, fsCol, 2, true);

        rowY += rowH + 8;
    }

    // Rules Info Box
    Rect infoBox = {mx + 20, my + 442, mw - 40, 108};
    canvas_.drawPanel(infoBox, 10, Color(16, 16, 21), Palette::BorderDark, 1.0f);
    canvas_.drawText(infoBox.x + 14, infoBox.y + 10, "MPC.SETTINGS RULES APPLIED", Palette::AmberGold, 1, true);
    canvas_.drawTextRight(infoBox.x + infoBox.w - 14, infoBox.y + 10, "RULE 1 (GOLD RULE) ENFORCED", Palette::StatusGreen, 1, true);
    canvas_.drawHLine(infoBox.x + 14, infoBox.y + 28, infoBox.w - 28, Palette::BorderDark);

    canvas_.drawText(infoBox.x + 14, infoBox.y + 36, "1. Gold Rule: Auto-snapshot MPC.settings.bak-YYYYMMDDHHMMSS before modification", Palette::TextSecondary, 1, false);
    std::string r2 = "2. Target: Extracts portable/" + item.author + " - VST - " + item.label + " to selected location";
    canvas_.drawTextClipped({infoBox.x + 14, infoBox.y + 56, infoBox.w - 28, 16}, r2, Palette::TextSecondary, 1, false);
    if (item.installed == 2) {
        canvas_.drawText(infoBox.x + 14, infoBox.y + 76, "3. Repair: Cleans all bad entries in MPC.settings & adds a new clean <PLUGIN> entry", Palette::AmberGold, 1, false);
    } else {
        canvas_.drawText(infoBox.x + 14, infoBox.y + 76, "3. Registration: Injects <pluginList-arm><KNOWNPLUGINS> with %payload-path% replaced", Palette::TextSecondary, 1, false);
    }

    // Bottom Action Buttons: RETURN on Left, optional UNINSTALL for broken installs in Middle, INSTALL / REPAIR on Right
    canvas_.drawHLine(mx + 20, my + 566, mw - 40, Palette::BorderDark);

    Rect retBtn = {mx + 20, my + 578, 180, 48};
    canvas_.drawPanel(retBtn, 10, Palette::BgButton, Color(62, 62, 78), 1.0f);
    canvas_.drawChevronIcon(retBtn.x + 16, retBtn.y + 15, 17, true, Palette::TextSecondary);
    canvas_.drawText(retBtn.x + 42, retBtn.y + 15, "RETURN", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(retBtn.x + retBtn.w - 14, retBtn.y + 18, "[-] MINUS", Palette::TextSecondary, 1, false);

    if (item.installed == 2) {
        Rect uninstBtn = {mx + 220, my + 578, 180, 48};
        canvas_.drawPanel(uninstBtn, 10, Color(42, 24, 28), Palette::AkaiCrimson, 1.5f);
        canvas_.drawTextCentered(uninstBtn, "UNINSTALL", Palette::TextPrimary, 2, true);
    }

    Rect instBtn = {mx + mw - 20 - 220, my + 578, 220, 48};
    canvas_.fillRoundedRect(instBtn, 10, item.installed == 2 ? Color(180, 83, 9) : Palette::AkaiRed);
    canvas_.drawDownloadIcon(instBtn.x + 18, instBtn.y + 15, 18, Palette::TextPrimary);
    canvas_.drawText(instBtn.x + 46, instBtn.y + 15, item.installed == 2 ? "REPAIR" : "INSTALL", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(instBtn.x + instBtn.w - 14, instBtn.y + 18, "[+] PLUS", Palette::TextPrimary, 1, false);
}

void PluginManagerView::drawExecutionScreen() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();
    Rect termBox = {24, 88, W - 48, H - 176};
    canvas_.drawPanel(termBox, 12, Color(10, 12, 16), Palette::BorderLight, 1.5f);

    int ly = termBox.y + 20;
    for (size_t i = 0; i < execLogs_.size() && ly < termBox.y + termBox.h - 28; ++i) {
        const std::string& logLine = execLogs_[i];
        Color col = Palette::StatusGreen;
        if (logLine.find("[ERROR]") != std::string::npos || logLine.find("ABORTED") != std::string::npos) {
            col = Color(255, 100, 110);
        } else if (logLine.find("[WARN]") != std::string::npos || logLine.find("[TKGL-ARCH]") != std::string::npos) {
            col = Palette::AmberGold;
        }
        canvas_.drawTextClipped({termBox.x + 20, ly, termBox.w - 40, 20}, logLine, col, 2, false);
        ly += 26;
    }

    if (!execFinished_) {
        int dotCount = (animFrame_ % 5) + 1;
        std::string dotPill;
        for (int d = 0; d < 5; ++d) {
            dotPill += (d < dotCount) ? " * " : " . ";
        }
        Rect actBox = {(W - 560) / 2, H - 72, 560, 50};
        canvas_.drawPanel(actBox, 8, Color(24, 24, 30), Palette::AmberGold, 1.2f);
        std::string actTitle = isRefreshing_ ? "REFRESHING CATALOG [ " : "DOWNLOADING & INSTALLING [ ";
        canvas_.drawTextCentered(actBox, actTitle + dotPill + " ]", Palette::AmberGold, 2, true);
    } else {
        Rect backBtn = {(W - 460) / 2, H - 72, 460, 50};
        canvas_.fillRoundedRect(backBtn, 8, Palette::AkaiRed);
        canvas_.drawTextCentered(backBtn, "TAP, PRESS WHEEL OR [-] TO RETURN", Palette::TextPrimary, 2, true);
    }
}

void PluginManagerView::drawBottomPaginationBar() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();
    int barY = H - 64;

    canvas_.fillRect({0, barY, W, 64}, Palette::BgHeader);
    canvas_.drawHLine(0, barY, W, Palette::BorderLight);

    int totalPages = std::max(1, (static_cast<int>(filteredItems_.size()) + itemsPerPage_ - 1) / itemsPerPage_);
    int currentPage = (selectedIndex_ / itemsPerPage_) + 1;

    Rect prevBtn = {24, barY + 13, 38, 38};
    canvas_.drawPanel(prevBtn, 8, Color(36, 36, 44), Color(58, 58, 70), 1.0f);
    canvas_.drawChevronIcon(prevBtn.x + 11, prevBtn.y + 11, 16, true, Palette::TextPrimary);

    std::string pageStr = std::to_string(currentPage) + " / " + std::to_string(totalPages);
    canvas_.drawTextCentered({66, barY + 13, 88, 38}, pageStr, Palette::TextPrimary, 2, true);

    Rect nextBtn = {158, barY + 13, 38, 38};
    canvas_.drawPanel(nextBtn, 8, Color(36, 36, 44), Color(58, 58, 70), 1.0f);
    canvas_.drawChevronIcon(nextBtn.x + 11, nextBtn.y + 11, 16, false, Palette::TextPrimary);

    int installedCount = 0;
    int updatesCount = 0;
    for (const auto& s : config_.sections) {
        for (const auto& it : s.items) {
            if (it.installed) installedCount++;
            if (it.updateAvail) updatesCount++;
        }
    }
    std::string countStr = std::to_string(filteredItems_.size()) + " in view  .  " +
                           std::to_string(installedCount) + " installed  .  " +
                           std::to_string(updatesCount) + " updates";
    canvas_.drawTextClipped({216, barY + 23, W - 560, 20}, countStr, Palette::TextSecondary, 2, false);

    Rect updBtn = {W - 284, barY + 13, 146, 38};
    canvas_.drawPanel(updBtn, 8, Color(35, 32, 28), Palette::AkaiAmber, 1.5f);
    canvas_.drawTextCentered(updBtn, "UPDATE ALL", Palette::AmberGold, 2, true);

    Rect exitBtn = {W - 126, barY + 13, 102, 38};
    canvas_.drawPanel(exitBtn, 8, Color(46, 18, 24), Palette::AkaiCrimson, 1.5f);
    canvas_.drawTextCentered(exitBtn, "EXIT", Palette::TextPrimary, 2, true);
}

void PluginManagerView::drawExitPopup() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();

    // Dimmed full-screen backdrop
    canvas_.fillRect({0, 62, W, H - 62}, Color(6, 6, 10, 215));

    int mw = 560;
    int mh = 270;
    int mx = (W - mw) / 2;
    int my = (H - mh) / 2;

    Rect modalR = {mx, my, mw, mh};
    canvas_.drawPanel(modalR, 14, Color(22, 22, 29), Palette::AkaiCrimson, 2.0f);

    // Top Header
    Rect badgeR = {mx + 24, my + 22, 92, 26};
    canvas_.fillRoundedRect(badgeR, 6, Palette::AkaiRed);
    canvas_.drawTextCentered(badgeR, "EXIT APP", Palette::TextPrimary, 1, true);

    canvas_.drawText(mx + 128, my + 24, "QUIT PLUGIN MANAGER?", Palette::TextPrimary, 2, true);
    canvas_.drawHLine(mx + 24, my + 62, mw - 48, Palette::BorderLight);

    canvas_.drawTextCentered({mx + 24, my + 86, mw - 48, 24}, "Close The KikGen Labs Plugin Manager and", Palette::TextPrimary, 2, false);
    canvas_.drawTextCentered({mx + 24, my + 116, mw - 48, 24}, "return to the Akai MPC / Force system?", Palette::TextSecondary, 2, false);
    if (config::ConfigParser::hasStagedMpcSettings()) {
        canvas_.drawTextCentered({mx + 24, my + 148, mw - 48, 20}, "* Session changes will be committed to MPC.settings", Palette::StatusGreen, 1, true);
    }

    canvas_.drawHLine(mx + 24, my + 178, mw - 48, Palette::BorderDark);

    // Left Button: CANCEL ([-] MINUS)
    Rect cancelBtn = {mx + 24, my + 196, 236, 50};
    canvas_.drawPanel(cancelBtn, 10, Palette::BgButton, Color(62, 62, 78), 1.0f);
    canvas_.drawText(cancelBtn.x + 22, cancelBtn.y + 16, "CANCEL", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(cancelBtn.x + cancelBtn.w - 18, cancelBtn.y + 19, "[-] MINUS", Palette::TextSecondary, 1, false);

    // Right Button: QUIT ([+] PLUS / WHEEL)
    Rect quitBtn = {mx + mw - 24 - 236, my + 196, 236, 50};
    canvas_.fillRoundedRect(quitBtn, 10, Palette::AkaiRed);
    canvas_.drawText(quitBtn.x + 22, quitBtn.y + 16, "QUIT", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(quitBtn.x + quitBtn.w - 18, quitBtn.y + 19, "[+] / ENTER", Palette::TextPrimary, 1, false);
}

void PluginManagerView::drawRefreshPopup() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();

    // Dimmed full-screen backdrop
    canvas_.fillRect({0, 62, W, H - 62}, Color(6, 6, 10, 215));

    int mw = 680;
    int mh = 310;
    int mx = (W - mw) / 2;
    int my = (H - mh) / 2;

    Rect modalR = {mx, my, mw, mh};
    canvas_.drawPanel(modalR, 14, Color(22, 22, 29), Palette::BorderLight, 2.0f);

    // Top Header Pill Badge
    Rect badgeR = {mx + 24, my + 22, 148, 26};
    canvas_.fillRoundedRect(badgeR, 6, Palette::BorderLight);
    canvas_.drawTextCentered(badgeR, "CATALOG SYNC", Palette::TextPrimary, 1, true);

    canvas_.drawText(mx + 184, my + 24, "CACHE ALL PLUGIN PACKAGES?", Palette::AmberGold, 2, true);
    canvas_.drawHLine(mx + 24, my + 62, mw - 48, Palette::BorderLight);

    canvas_.drawTextCentered({mx + 24, my + 82, mw - 48, 24}, "Would you like to mirror all plugin zip archives locally?", Palette::TextPrimary, 2, false);
    canvas_.drawTextCentered({mx + 24, my + 112, mw - 48, 22}, "- YES: Mirrors all missing .zip packages to disk for full OFFLINE use.", Palette::TextSecondary, 1, false);
    canvas_.drawTextCentered({mx + 24, my + 138, mw - 48, 22}, "- NO: Refreshes catalog metadata, updates & thumbnails only (FAST).", Palette::TextSecondary, 1, false);

    canvas_.drawHLine(mx + 24, my + 196, mw - 48, Palette::BorderDark);

    // 3 Action Buttons: CANCEL | NO (METADATA ONLY) | YES (CACHE ALL)
    int btnW = 196;
    int btnH = 50;
    int btnY = my + 218;

    // Button 1: CANCEL ([-] MINUS)
    Rect cancelBtn = {mx + 24, btnY, btnW, btnH};
    canvas_.drawPanel(cancelBtn, 10, Palette::BgButton, Color(62, 62, 78), 1.0f);
    canvas_.drawText(cancelBtn.x + 18, cancelBtn.y + 16, "CANCEL", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(cancelBtn.x + cancelBtn.w - 14, cancelBtn.y + 19, "[-]", Palette::TextSecondary, 1, false);

    // Button 2: NO (FAST SYNC)
    Rect noBtn = {mx + 24 + btnW + 18, btnY, btnW, btnH};
    canvas_.drawPanel(noBtn, 10, Color(28, 38, 48), Color(54, 82, 108), 1.2f);
    canvas_.drawText(noBtn.x + 18, noBtn.y + 16, "NO (FAST)", Palette::StatusCyan, 2, true);
    canvas_.drawTextRight(noBtn.x + noBtn.w - 14, noBtn.y + 19, "META ONLY", Palette::StatusCyan, 1, false);

    // Button 3: YES (CACHE ALL)
    Rect yesBtn = {mx + mw - 24 - btnW, btnY, btnW, btnH};
    canvas_.fillRoundedRect(yesBtn, 10, Palette::AkaiRed);
    canvas_.drawText(yesBtn.x + 18, yesBtn.y + 16, "YES (ALL)", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(yesBtn.x + yesBtn.w - 14, yesBtn.y + 19, "DOWNLOAD", Palette::TextPrimary, 1, false);
}

void PluginManagerView::drawPurgePopup() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();

    if (filteredItems_.empty()) return;
    const auto& ref = filteredItems_[selectedIndex_];
    const auto& item = config_.sections[ref.sectionIdx].items[ref.itemIdx];
    std::string cachedZipSize;
    config::ConfigParser::isItemBinaryCached(item, &cachedZipSize);

    // Dimmed full-screen backdrop
    canvas_.fillRect({0, 62, W, H - 62}, Color(6, 6, 10, 215));

    int mw = 580;
    int mh = 280;
    int mx = (W - mw) / 2;
    int my = (H - mh) / 2;

    Rect modalR = {mx, my, mw, mh};
    canvas_.drawPanel(modalR, 14, Color(22, 22, 29), Palette::AkaiCrimson, 2.0f);

    // Top Header Badge
    Rect badgeR = {mx + 24, my + 22, 110, 26};
    canvas_.fillRoundedRect(badgeR, 6, Palette::AkaiRed);
    canvas_.drawTextCentered(badgeR, "PURGE ZIP", Palette::TextPrimary, 1, true);

    canvas_.drawText(mx + 146, my + 24, "DELETE LOCAL ARCHIVE?", Palette::AmberGold, 2, true);
    canvas_.drawHLine(mx + 24, my + 62, mw - 48, Palette::BorderLight);

    std::string qLine = "Delete local package for " + item.label + "?";
    canvas_.drawTextCentered({mx + 24, my + 84, mw - 48, 24}, qLine, Palette::TextPrimary, 2, true);

    std::string sizeLine = "Frees " + (cachedZipSize.empty() ? item.sizeMb : cachedZipSize) + " on storage drive.";
    canvas_.drawTextCentered({mx + 24, my + 114, mw - 48, 22}, sizeLine, Palette::TextSecondary, 1, false);
    canvas_.drawTextCentered({mx + 24, my + 138, mw - 48, 22}, "(Installed plugin files in /VST2 Plugins remain untouched and active)", Palette::AmberGold, 1, false);

    canvas_.drawHLine(mx + 24, my + 186, mw - 48, Palette::BorderDark);

    // 2 Buttons: CANCEL ([-] MINUS) | PURGE ZIP ([+] PLUS / WHEEL)
    int btnW = 240;
    int btnH = 50;
    int btnY = my + 206;

    Rect cancelBtn = {mx + 24, btnY, btnW, btnH};
    canvas_.drawPanel(cancelBtn, 10, Palette::BgButton, Color(62, 62, 78), 1.0f);
    canvas_.drawText(cancelBtn.x + 20, cancelBtn.y + 16, "CANCEL", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(cancelBtn.x + cancelBtn.w - 16, cancelBtn.y + 19, "[-] MINUS", Palette::TextSecondary, 1, false);

    Rect purgeBtn = {mx + mw - 24 - btnW, btnY, btnW, btnH};
    canvas_.fillRoundedRect(purgeBtn, 10, Palette::AkaiRed);
    canvas_.drawText(purgeBtn.x + 20, purgeBtn.y + 16, "PURGE ZIP", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(purgeBtn.x + purgeBtn.w - 16, purgeBtn.y + 19, "[+] / ENTER", Palette::TextPrimary, 1, false);
}

void PluginManagerView::drawRestoreBackupPopup() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();
    canvas_.fillRect({0, 62, W, H - 62}, Color(6, 6, 10, 215));

    int mw = 640;
    int mh = 320;
    int mx = (W - mw) / 2;
    int my = (H - mh) / 2;

    Rect modal = {mx, my, mw, mh};
    canvas_.drawPanel(modal, 12, Color(22, 22, 28), Palette::AkaiCrimson, 2.0f);

    canvas_.drawText(modal.x + 30, modal.y + 26, "CONFIRM RESTORE BACKUP?", Palette::TextPrimary, 2, true);
    canvas_.drawHLine(modal.x + 30, modal.y + 60, modal.w - 60, Color(50, 50, 62));

    std::string bName = (!mpcBackups_.empty() && selectedBackupIdx_ >= 0 && selectedBackupIdx_ < static_cast<int>(mpcBackups_.size()))
        ? mpcBackups_[selectedBackupIdx_].filename : "";

    canvas_.drawText(modal.x + 30, modal.y + 80, "Restore the following settings file to active MPC.settings?", Palette::TextSecondary, 1, false);
    Rect fileBox = {modal.x + 30, modal.y + 108, modal.w - 60, 44};
    canvas_.drawPanel(fileBox, 6, Color(16, 16, 22), Color(40, 40, 52), 1.0f);
    canvas_.drawText(fileBox.x + 18, fileBox.y + 14, bName, Palette::AmberGold, 1, true);

    canvas_.drawText(modal.x + 30, modal.y + 170, "* A safety backup (MPC.settings.bak-*) will be created first.", Palette::TextMuted, 1, false);
    canvas_.drawText(modal.x + 30, modal.y + 192, "* Active MPC application must be restarted to apply changes.", Palette::TextMuted, 1, false);

    // Cancel Button
    Rect cancelBtn = {modal.x + 40, modal.y + 240, 240, 50};
    canvas_.drawPanel(cancelBtn, 8, Color(35, 35, 44), Color(62, 62, 76), 1.0f);
    canvas_.drawTextCentered(cancelBtn, "CANCEL", Palette::TextPrimary, 1, true);

    // Confirm Restore Button
    Rect confBtn = {modal.x + modal.w - 280, modal.y + 240, 240, 50};
    canvas_.fillRoundedRect(confBtn, 8, Palette::AkaiRed);
    canvas_.drawTextCentered(confBtn, "CONFIRM RESTORE", Palette::TextPrimary, 1, true);
}

void PluginManagerView::drawDeleteBackupPopup() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();
    canvas_.fillRect({0, 62, W, H - 62}, Color(6, 6, 10, 215));

    int mw = 640;
    int mh = 320;
    int mx = (W - mw) / 2;
    int my = (H - mh) / 2;

    Rect modal = {mx, my, mw, mh};
    canvas_.drawPanel(modal, 12, Color(22, 22, 28), Color(220, 38, 38), 2.0f);

    canvas_.drawText(modal.x + 30, modal.y + 26, "WARNING: DELETE BACKUP?", Color(255, 90, 100), 2, true);
    canvas_.drawHLine(modal.x + 30, modal.y + 60, modal.w - 60, Color(50, 50, 62));

    std::string bName = (!mpcBackups_.empty() && selectedBackupIdx_ >= 0 && selectedBackupIdx_ < static_cast<int>(mpcBackups_.size()))
        ? mpcBackups_[selectedBackupIdx_].filename : "";

    canvas_.drawText(modal.x + 30, modal.y + 80, "Permanently delete the selected MPC.settings backup?", Palette::TextSecondary, 1, false);
    Rect fileBox = {modal.x + 30, modal.y + 108, modal.w - 60, 44};
    canvas_.drawPanel(fileBox, 6, Color(16, 16, 22), Color(40, 40, 52), 1.0f);
    canvas_.drawText(fileBox.x + 18, fileBox.y + 14, bName, Palette::TextPrimary, 1, true);

    canvas_.drawText(modal.x + 30, modal.y + 172, "Warning: This file will be permanently removed from disk.", Color(255, 120, 120), 1, false);
    canvas_.drawText(modal.x + 30, modal.y + 194, "This operation cannot be undone.", Palette::TextMuted, 1, false);

    // Cancel Button
    Rect cancelBtn = {modal.x + 40, modal.y + 240, 240, 50};
    canvas_.drawPanel(cancelBtn, 8, Color(35, 35, 44), Color(62, 62, 76), 1.0f);
    canvas_.drawTextCentered(cancelBtn, "CANCEL", Palette::TextPrimary, 1, true);

    // Confirm Delete Button
    Rect confBtn = {modal.x + modal.w - 280, modal.y + 240, 240, 50};
    canvas_.drawPanel(confBtn, 8, Color(180, 20, 30), Color(255, 70, 80), 1.0f);
    canvas_.drawTextCentered(confBtn, "PERMANENT DELETE", Palette::TextPrimary, 1, true);
}

void PluginManagerView::drawV310WarningPopup() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();
    canvas_.fillRect({0, 62, W, H - 62}, Color(6, 6, 10, 222));

    int mw = 820;
    int mh = 430;
    int mx = (W - mw) / 2;
    int my = (H - mh) / 2;

    Rect modal = {mx, my, mw, mh};
    canvas_.drawPanel(modal, 14, Color(22, 22, 29), Palette::AmberGold, 2.0f);

    Rect badgeR = {mx + 26, my + 22, 130, 28};
    canvas_.fillRoundedRect(badgeR, 6, Color(180, 83, 9));
    canvas_.drawTextCentered(badgeR, "MPC >= 3.10", Palette::TextPrimary, 1, true);

    canvas_.drawText(mx + 170, my + 25, "LEGACY MPC.SETTINGS ENTRIES DETECTED", Palette::AmberGold, 2, true);
    canvas_.drawHLine(mx + 26, my + 62, mw - 52, Palette::BorderLight);

    canvas_.drawText(mx + 28, my + 78, "To prevent plugin conflicts or disappearance on MPC OS >= 3.10, please review:", Palette::TextPrimary, 1, false);

    int cy = my + 112;
    if (v310Warnings_.hasSynthVst2Locations) {
        Rect r1 = {mx + 28, cy, mw - 56, 78};
        canvas_.drawPanel(r1, 8, Color(30, 24, 18), Color(180, 120, 30), 1.0f);
        std::string sTitle = ". SynthContentLocations in MPC.settings points to VST2 Plugins folder(s) (" +
                             std::to_string(v310Warnings_.synthVst2Locations.size()) + " entry(ies))";
        canvas_.drawTextClipped({r1.x + 14, r1.y + 12, r1.w - 28, 22}, sTitle, Palette::AmberGold, 1, true);
        std::string sDetail = v310Warnings_.synthVst2Locations[0];
        if (v310Warnings_.synthVst2Locations.size() > 1) {
            sDetail += "  (+ " + std::to_string(v310Warnings_.synthVst2Locations.size() - 1) + " more)";
        }
        canvas_.drawTextClipped({r1.x + 18, r1.y + 42, r1.w - 36, 20}, sDetail, Palette::TextSecondary, 1, false);
        cy += 88;
    }

    if (v310Warnings_.hasLegacyKnownPlugins) {
        Rect r2 = {mx + 28, cy, mw - 56, 78};
        canvas_.drawPanel(r2, 8, Color(34, 20, 24), Palette::AkaiCrimson, 1.0f);
        std::string kTitle = ". VST2 Plugins still declared inside <KNOWNPLUGINS> of MPC.settings (" +
                             std::to_string(v310Warnings_.knownPluginsCount) + " entry(ies))";
        canvas_.drawTextClipped({r2.x + 14, r2.y + 12, r2.w - 28, 22}, kTitle, Color(255, 130, 140), 1, true);
        canvas_.drawTextClipped({r2.x + 18, r2.y + 42, r2.w - 36, 20},
                                "MPC >= 3.10 scans <MountPoint>/VST2 Plugins natively and no longer uses KNOWNPLUGINS.",
                                Palette::TextSecondary, 1, false);
        cy += 88;
    }

    Rect tipBox = {mx + 28, my + 294, mw - 56, 46};
    canvas_.drawPanel(tipBox, 8, Color(16, 34, 26), Palette::StatusGreen, 1.0f);
    canvas_.drawGlowDot(tipBox.x + 20, tipBox.y + 23, 4, Palette::StatusGreen);
    canvas_.drawText(tipBox.x + 38, tipBox.y + 15, "=> Go to MPC Settings and select SANITY CHECKS to migrate & purge safely.", Palette::StatusGreen, 1, true);

    canvas_.drawHLine(mx + 26, my + 350, mw - 52, Palette::BorderDark);

    int btnY = my + 362;
    Rect dismissBtn = {mx + 28, btnY, 240, 48};
    canvas_.drawPanel(dismissBtn, 10, Palette::BgButton, Color(62, 62, 78), 1.0f);
    canvas_.drawText(dismissBtn.x + 18, dismissBtn.y + 15, "DISMISS", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(dismissBtn.x + dismissBtn.w - 14, dismissBtn.y + 18, "[-] MINUS", Palette::TextSecondary, 1, false);

    Rect sanityBtn = {mx + mw - 28 - 350, btnY, 350, 48};
    canvas_.fillRoundedRect(sanityBtn, 10, Palette::AkaiRed);
    canvas_.drawText(sanityBtn.x + 18, sanityBtn.y + 15, "OPEN SANITY CHECKS", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(sanityBtn.x + sanityBtn.w - 14, sanityBtn.y + 18, "[+] / ENTER", Palette::TextPrimary, 1, false);
}

void PluginManagerView::drawScreensaver() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();
    canvas_.clear(Color(6, 6, 9));

    // Smooth Lissajous drift to prevent display burn-in
    int ox = static_cast<int>(std::sin(screensaverTick_ * 0.08f) * 140.0f);
    int oy = static_cast<int>(std::cos(screensaverTick_ * 0.055f) * 85.0f);

    int cw = 640;
    int ch = 290;
    int cx = (W - cw) / 2 + ox;
    int cy = (H - ch) / 2 + oy;

    Rect cardR = {cx, cy, cw, ch};
    canvas_.drawPanel(cardR, 18, Color(14, 15, 20), Palette::AkaiCrimson, 2.0f);

    // Top Poloq / KikGen 4-Square Emblem centered
    int iconX = cx + (cw - 48) / 2;
    int iconY = cy + 24;
    Rect iconBox = {iconX, iconY, 48, 48};
    canvas_.drawPanel(iconBox, 10, Color(28, 28, 36), Color(58, 58, 72), 1.0f);
    canvas_.fillRoundedRect({iconX + 9,  iconY + 9,  13, 13}, 3, Color(161, 161, 170));
    canvas_.fillRoundedRect({iconX + 26, iconY + 9,  13, 13}, 3, Color(161, 161, 170));
    canvas_.fillRoundedRect({iconX + 9,  iconY + 26, 13, 13}, 3, Color(161, 161, 170));
    canvas_.fillRoundedRect({iconX + 26, iconY + 26, 13, 13}, 3, Palette::AkaiCrimson);

    // Title & Subtitle
    canvas_.drawTextCentered({cx + 20, cy + 86, cw - 40, 28}, "THE KIKGEN LABS", Palette::TextPrimary, 3, true);
    canvas_.drawTextCentered({cx + 20, cy + 120, cw - 40, 20}, "PLUGINS MANAGER FOR AKAI MPC & FORCE", Palette::AmberGold, 2, true);

    canvas_.drawHLine(cx + 48, cy + 154, cw - 96, Palette::BorderDark);

    // Catalog & Installed status
    int totalCatalog = 0;
    int installedCount = 0;
    for (const auto& s : config_.sections) {
        totalCatalog += static_cast<int>(s.items.size());
        for (const auto& it : s.items) {
            if (it.installed) installedCount++;
        }
    }
    std::string statLine = std::to_string(totalCatalog) + " Plugins in Catalog  .  " +
                           std::to_string(installedCount) + " Installed on Device";
    canvas_.drawTextCentered({cx + 20, cy + 170, cw - 40, 20}, statLine, Palette::StatusGreen, 2, true);

    // Animated pulse dots
    int dotCount = (screensaverTick_ % 5) + 1;
    std::string dots = "[ ";
    for (int d = 0; d < 5; ++d) {
        dots += (d < dotCount) ? " * " : " . ";
    }
    dots += " ]";
    canvas_.drawTextCentered({cx + 20, cy + 200, cw - 40, 18}, dots, Palette::TextSecondary, 1, true);

    // Bottom wake prompt pill
    Rect wakePill = {cx + 40, cy + ch - 52, cw - 80, 34};
    canvas_.drawPanel(wakePill, 8, Color(22, 22, 29), Palette::BorderLight, 1.0f);
    canvas_.drawGlowDot(wakePill.x + 22, wakePill.y + 17, 4, Palette::StatusGreen);
    canvas_.drawTextCentered(wakePill, "TOUCH SCREEN OR USE ANY MIDI CONTROL TO WAKE", Palette::TextPrimary, 1, true);
}

void PluginManagerView::drawSettingsScreen() {
    int W = canvas_.getLogicalWidth();
    canvas_.clear(Palette::BgDarkest);

    // Top Navigation Header
    canvas_.fillRect({0, 0, W, 62}, Palette::BgHeader);
    canvas_.drawHLine(0, 61, W, Palette::BorderLight);

    // 1. < BACK Button
    Rect backBtn = {20, 11, 110, 40};
    canvas_.drawPanel(backBtn, 8, Color(35, 35, 44), Color(62, 62, 76), 1.0f);
    canvas_.drawText(backBtn.x + 18, backBtn.y + 12, "< BACK", Palette::TextPrimary, 1, true);

    // 2. Title
    canvas_.drawText(148, 22, "ADVANCED SETTINGS", Palette::TextPrimary, 2, true);

    // 3. Tab Selectors (Tab 0: PLUGIN MANAGER, Tab 1: MPC SETTINGS)
    Rect tab0 = {560, 11, 220, 40};
    Rect tab1 = {800, 11, 220, 40};

    if (settingsTab_ == 0) {
        canvas_.drawPanel(tab0, 8, Color(48, 48, 60), Palette::AkaiCrimson, 2.0f);
        canvas_.drawText(tab0.x + 32, tab0.y + 12, "PLUGIN MANAGER", Palette::TextPrimary, 1, true);

        canvas_.drawPanel(tab1, 8, Color(28, 28, 35), Color(50, 50, 60), 1.0f);
        canvas_.drawText(tab1.x + 42, tab1.y + 12, "MPC SETTINGS", Palette::TextSecondary, 1, false);
    } else {
        canvas_.drawPanel(tab0, 8, Color(28, 28, 35), Color(50, 50, 60), 1.0f);
        canvas_.drawText(tab0.x + 32, tab0.y + 12, "PLUGIN MANAGER", Palette::TextSecondary, 1, false);

        canvas_.drawPanel(tab1, 8, Color(48, 48, 60), Palette::AkaiCrimson, 2.0f);
        canvas_.drawText(tab1.x + 42, tab1.y + 12, "MPC SETTINGS", Palette::TextPrimary, 1, true);
    }

    // Top Header Version badge
    canvas_.drawText(W - 190, 26, "V1.2.23", Palette::TextSecondary, 1, true);

    // Body
    if (settingsTab_ == 0) {
        drawPluginManagerSettingsTab();
    } else {
        drawMpcSettingsTab();
    }
}

void PluginManagerView::drawPluginManagerSettingsTab() {
    int W = canvas_.getLogicalWidth();

    // Card 1: Manager Configuration (mpc_plugins.conf)
    Rect card1 = {30, 80, W - 60, 372};
    canvas_.drawPanel(card1, 10, Color(24, 24, 30), Color(46, 46, 58), 1.0f);
    canvas_.drawText(card1.x + 20, card1.y + 18, "GENERAL CONFIGURATION (mpc_plugins.conf)", Palette::AmberGold, 1, true);
    if (!settingsStatusMsg_.empty()) {
        Color stCol = (settingsStatusColor_ == 2) ? Color(255, 110, 120) : Palette::StatusGreen;
        canvas_.drawTextClipped({card1.x + 560, card1.y + 18, card1.w - 580, 22}, settingsStatusMsg_, stCol, 1, true);
    }
    canvas_.drawHLine(card1.x + 20, card1.y + 42, card1.w - 40, Color(46, 46, 58));

    int y = card1.y + 58;
    auto drawParamRow = [&](const std::string& key, const std::string& val, int stepY = 34) {
        canvas_.drawText(card1.x + 24, y, key, Palette::TextSecondary, 1, false);
        canvas_.drawTextClipped({card1.x + 330, y, card1.w - 360, 22}, val, Palette::TextPrimary, 1, true);
        y += stepY;
    };

    drawParamRow("Application Title", config_.app_title);
    drawParamRow("Online Catalog URL", config_.catalog_url);
    drawParamRow("Ping Connectivity Host", config_.ping_host);

    // Interactive Screen Rotation row (y = 240): No rotation (0), 90, 180, 270
    canvas_.drawText(card1.x + 24, y, "Screen Rotation", Palette::TextSecondary, 1, false);
    const int rotVals[4] = {0, 90, 180, 270};
    const char* rotLabels[4] = {"No rotation", "90", "180", "270"};
    const int rotBtnX[4] = {360, 507, 654, 801};
    for (int i = 0; i < 4; ++i) {
        bool isAct = (config_.screen_rotation == rotVals[i]);
        Rect rBtn = {rotBtnX[i], y - 7, 135, 28};
        canvas_.drawPanel(
            rBtn, 6,
            isAct ? Palette::AkaiCrimson : Color(32, 32, 42),
            isAct ? Color(255, 110, 130) : Color(60, 60, 74),
            isAct ? 2.0f : 1.0f
        );
        canvas_.drawTextCentered(rBtn, rotLabels[i], isAct ? Palette::TextPrimary : Palette::TextSecondary, 1, isAct);
    }
    y += 36;

    drawParamRow("Active Manager Root Dir", config::ConfigParser::getManagerRootDir(), 32);
    drawParamRow("Manager VST2 Plugins Dir", config::ConfigParser::getManagerVst2Dir(), 32);
    drawParamRow("Resolved Backups Dir", config::ConfigParser::resolveBackupDirectory(), 32);
    drawParamRow("Resolved MPC.settings", config::ConfigParser::resolveMpcSettingsPath(), 32);
    drawParamRow("Total Catalog Plugins", std::to_string(filteredItems_.size()) + " items currently indexed", 32);

    // Card 2: System & Hardware Environment
    Rect card2 = {30, 466, W - 60, 294};
    canvas_.drawPanel(card2, 10, Color(24, 24, 30), Color(46, 46, 58), 1.0f);
    canvas_.drawText(card2.x + 20, card2.y + 18, "RUNTIME ENVIRONMENT & SAFETY POLICIES", Palette::StatusGreen, 1, true);
    canvas_.drawHLine(card2.x + 20, card2.y + 42, card2.w - 40, Color(46, 46, 58));

    y = card2.y + 58;
    drawParamRow("Target Architecture", "ARMv7-A Cortex-A9 (Akai MPC Live, One, X, Key 61, Force)", 32);
    drawParamRow("Gold Rule Safety Backups", "Automatic backups stored in Manager ($PLG_MAN_DIR/backups)", 32);
    drawParamRow("Mount Root VST2 Policy", "On >= 3.10, VST2 Plugins is created at manager mount root (<Mount>/VST2 Plugins)", 32);
    drawParamRow("Internet Reachability", isOnline_ ? "ONLINE (Verified via ping_host)" : "OFFLINE (Local USB only)", 32);
    drawParamRow("Offline Binary Caching", "Local .zip archives stored in plugins/<id>/<id>.zip", 32);
    drawParamRow("Version Release Tag", "The KikGen Labs - V1.2.23 (v1.2.23)", 32);
}

void PluginManagerView::drawMpcSettingsTab() {
    int W = canvas_.getLogicalWidth();

    // 1. Detected MPC OS Binary Version banner (displayed FIRST in MPC Settings)
    auto mpcVer = config::ConfigParser::detectMpcBinaryVersion();
    Rect verCard = {30, 70, W - 60, 42};
    canvas_.drawPanel(
        verCard, 8,
        mpcVer.isNativeVst2 ? Color(16, 34, 26) : Color(32, 26, 20),
        mpcVer.isNativeVst2 ? Palette::StatusGreen : Palette::AmberGold,
        1.5f
    );
    canvas_.drawGlowDot(verCard.x + 20, verCard.y + 21, 5, mpcVer.isNativeVst2 ? Palette::StatusGreen : Palette::AmberGold);
    std::string verText = "MPC Binary (/usr/bin/MPC): " + (mpcVer.found ? ("MPC " + mpcVer.fullVersionStr) : "Version not detected");
    canvas_.drawTextClipped({verCard.x + 38, verCard.y + 12, 650, 22}, verText, Palette::TextPrimary, 1, true);

    std::string modeBadge = mpcVer.isNativeVst2
        ? ">= 3.10 NATIVE VST2 (MPC.settings untouched)"
        : "< 3.10 LEGACY MODE (MPC.settings sync active)";
    Rect modePill = {verCard.x + verCard.w - 460, verCard.y + 6, 444, 30};
    canvas_.drawPanel(
        modePill, 6,
        mpcVer.isNativeVst2 ? Color(20, 62, 42) : Color(52, 36, 16),
        mpcVer.isNativeVst2 ? Palette::StatusGreen : Palette::AmberGold,
        1.0f
    );
    canvas_.drawTextCentered(modePill, modeBadge, mpcVer.isNativeVst2 ? Palette::StatusGreen : Palette::AmberGold, 1, true);

    // 2. Sub-tab selector bar: 3 buttons (0=ROLL BACK, 1=BOOT LOG, 2=SANITY CHECKS)
    const char* subLabels[3] = {"ROLL BACK", "BOOT LOG", "SANITY CHECKS"};
    int subX[3] = {30, 443, 856};
    int subW = 394;
    int subY = 122;
    int subH = 40;

    for (int i = 0; i < 3; ++i) {
        bool isAct = (mpcSettingsSubTab_ == i);
        Rect rSub = {subX[i], subY, subW, subH};
        canvas_.drawPanel(rSub, 8, isAct ? Color(48, 24, 32) : Color(26, 26, 34), isAct ? Palette::AkaiCrimson : Color(52, 52, 64), isAct ? 2.0f : 1.0f);
        canvas_.drawGlowDot(rSub.x + 18, rSub.y + 20, 4, isAct ? Palette::AkaiCrimson : Color(80, 80, 94));
        canvas_.drawTextCentered(rSub, subLabels[i], isAct ? Palette::TextPrimary : Palette::TextSecondary, 1, isAct);
    }

    if (mpcSettingsSubTab_ == 0) {
        drawMpcRollbackSubTab();
    } else if (mpcSettingsSubTab_ == 1) {
        drawMpcBootLogSubTab();
    } else {
        drawMpcSanitySubTab();
    }
}

void PluginManagerView::drawMpcRollbackSubTab() {
    int W = canvas_.getLogicalWidth();

    Rect b1 = {30, 174, W - 60, 588};
    canvas_.drawPanel(b1, 10, Color(24, 24, 30), Color(46, 46, 58), 1.0f);
    canvas_.drawText(b1.x + 20, b1.y + 16, "MPC.SETTINGS ROLLBACK (RESTORATION)", Palette::TextPrimary, 1, true);
    canvas_.drawText(b1.x + 430, b1.y + 18, "Backups stored in Manager ($PLG_MAN_DIR/backups, max 10)", Palette::TextSecondary, 1, false);
    canvas_.drawHLine(b1.x + 20, b1.y + 38, b1.w - 40, Color(46, 46, 58));

    int rowY = b1.y + 48;
    int rowH = 44;
    int maxVisible = 9;
    if (mpcBackups_.empty()) {
        canvas_.drawText(b1.x + 36, rowY + 30, "No timestamped backup files (MPC.settings.bak-*) found in Manager backups directory.", Palette::TextSecondary, 1, false);
        canvas_.drawText(b1.x + 36, rowY + 60, "Backups are automatically generated before any MPC.settings modification.", Palette::TextMuted, 1, false);
    } else {
        int maxOffset = std::max(0, static_cast<int>(mpcBackups_.size()) - maxVisible);
        if (backupScrollOffset_ > maxOffset) backupScrollOffset_ = maxOffset;
        if (backupScrollOffset_ < 0) backupScrollOffset_ = 0;

        size_t start = static_cast<size_t>(backupScrollOffset_);
        size_t end = std::min(mpcBackups_.size(), start + static_cast<size_t>(maxVisible));

        for (size_t i = start; i < end; ++i) {
            bool isSel = (static_cast<int>(i) == selectedBackupIdx_);
            Rect rRow = {b1.x + 20, rowY, 900, rowH};
            canvas_.drawPanel(rRow, 6, isSel ? Color(42, 24, 30) : Color(30, 30, 38), isSel ? Palette::AkaiCrimson : Color(50, 50, 62), isSel ? 2.0f : 1.0f);

            canvas_.drawGlowDot(rRow.x + 18, rRow.y + 22, 4, isSel ? Palette::AkaiCrimson : Color(80, 80, 92));
            canvas_.drawText(rRow.x + 36, rRow.y + 14, mpcBackups_[i].filename, isSel ? Palette::TextPrimary : Palette::TextSecondary, 1, true);
            canvas_.drawText(rRow.x + 470, rRow.y + 14, mpcBackups_[i].timestampFormatted, Palette::AmberGold, 1, false);

            std::string pCountStr = std::to_string(mpcBackups_[i].pluginCount) + " plugins";
            canvas_.drawText(rRow.x + 740, rRow.y + 14, pCountStr, Palette::StatusGreen, 1, true);

            rowY += rowH + 8;
        }
    }

    // Action Buttons on Right
    Rect refBtn = {b1.x + 940, b1.y + 50, 240, 48};
    canvas_.drawPanel(refBtn, 8, Color(35, 35, 44), Color(62, 62, 76), 1.0f);
    canvas_.drawTextCentered(refBtn, "REFRESH LIST", Palette::TextPrimary, 1, true);

    Rect restBtn = {b1.x + 940, b1.y + 112, 240, 54};
    canvas_.drawPanel(restBtn, 8, Palette::AkaiCrimson, Color(255, 90, 110), 1.0f);
    canvas_.drawTextCentered(restBtn, "RESTORE BACKUP", Palette::TextPrimary, 1, true);

    Rect delBtn = {b1.x + 940, b1.y + 180, 240, 48};
    canvas_.drawPanel(delBtn, 8, Color(42, 22, 26), Color(160, 45, 55), 1.0f);
    canvas_.drawTextCentered(delBtn, "DELETE BACKUP", Color(255, 140, 150), 1, true);

    // Info box on right bottom
    Rect infoBox = {b1.x + 940, b1.y + 244, 240, 160};
    canvas_.drawPanel(infoBox, 8, Color(18, 18, 24), Color(42, 42, 54), 1.0f);
    canvas_.drawText(infoBox.x + 14, infoBox.y + 16, "GOLD RULE SAFETY", Palette::AmberGold, 1, true);
    canvas_.drawText(infoBox.x + 14, infoBox.y + 42, "Max retention: 10 files", Palette::TextSecondary, 1, false);
    canvas_.drawText(infoBox.x + 14, infoBox.y + 66, "Location: $PLG_MAN_DIR/", Palette::TextMuted, 1, false);
    canvas_.drawText(infoBox.x + 14, infoBox.y + 86, "          backups/", Palette::TextMuted, 1, false);
    canvas_.drawText(infoBox.x + 14, infoBox.y + 114, "Wheel: Select backup", Palette::StatusGreen, 1, false);
    canvas_.drawText(infoBox.x + 14, infoBox.y + 134, "Push : Restore selected", Palette::StatusGreen, 1, false);

    if (!settingsStatusMsg_.empty()) {
        Color col = (settingsStatusColor_ == 1) ? Palette::StatusGreen : Palette::AkaiCrimson;
        canvas_.drawTextClipped({b1.x + 940, b1.y + 420, 240, 60}, settingsStatusMsg_, col, 1, true);
    }
}

void PluginManagerView::drawMpcBootLogSubTab() {
    int W = canvas_.getLogicalWidth();

    Rect bLog = {30, 174, W - 60, 588};
    canvas_.drawPanel(bLog, 10, Color(14, 16, 22), Color(46, 46, 58), 1.0f);
    canvas_.drawText(bLog.x + 20, bLog.y + 16, "SYSTEM BOOTLOG / SYSLOG / DMESG CONSOLE", Palette::AmberGold, 1, true);
    std::string countInfo = std::to_string(bootLogLines_.size()) + " lines captured (/var/log/messages, syslog, dmesg, plgman_log)";
    canvas_.drawText(bLog.x + 430, bLog.y + 16, countInfo, Palette::TextSecondary, 1, false);

    // Refresh Button in top-right of log panel
    Rect refLogBtn = {bLog.x + bLog.w - 200, bLog.y + 8, 180, 34};
    canvas_.drawPanel(refLogBtn, 6, Color(35, 35, 46), Palette::StatusGreen, 1.5f);
    canvas_.drawRefreshIcon(refLogBtn.x + 14, refLogBtn.y + 9, 16, Palette::StatusGreen);
    canvas_.drawText(refLogBtn.x + 38, refLogBtn.y + 10, "REFRESH LOG", Palette::TextPrimary, 1, true);

    canvas_.drawHLine(bLog.x + 20, bLog.y + 46, bLog.w - 40, Color(46, 46, 58));

    // Log viewport area
    Rect termRect = {bLog.x + 16, bLog.y + 56, bLog.w - 110, bLog.h - 72};
    canvas_.drawPanel(termRect, 6, Color(8, 10, 14), Color(32, 36, 46), 1.0f);

    int maxVisible = 16;
    int maxOffset = std::max(0, static_cast<int>(bootLogLines_.size()) - maxVisible);
    if (bootLogScrollOffset_ > maxOffset) bootLogScrollOffset_ = maxOffset;
    if (bootLogScrollOffset_ < 0) bootLogScrollOffset_ = 0;

    int ly = termRect.y + 14;
    size_t start = static_cast<size_t>(bootLogScrollOffset_);
    size_t end = std::min(bootLogLines_.size(), start + static_cast<size_t>(maxVisible));

    for (size_t i = start; i < end; ++i) {
        const std::string& line = bootLogLines_[i];
        Color lineCol = Palette::StatusGreen;
        if (line.find("ERROR") != std::string::npos || line.find("error") != std::string::npos || line.find("fail") != std::string::npos) {
            lineCol = Color(255, 110, 120);
        } else if (line.find("WARN") != std::string::npos || line.find("warn") != std::string::npos) {
            lineCol = Palette::AmberGold;
        } else if (line.find("[DMESG]") != std::string::npos) {
            lineCol = Color(140, 200, 255);
        }
        canvas_.drawTextClipped({termRect.x + 12, ly, termRect.w - 24, 22}, line, lineCol, 1, false);
        ly += 31;
    }

    // Up / Down Scroll Buttons on right edge of log window
    Rect upBtn = {bLog.x + bLog.w - 82, bLog.y + 56, 66, 120};
    canvas_.drawPanel(upBtn, 8, Color(30, 30, 40), Color(62, 62, 76), 1.0f);
    canvas_.drawTextCentered(upBtn, "UP", Palette::TextPrimary, 1, true);

    Rect dnBtn = {bLog.x + bLog.w - 82, bLog.y + bLog.h - 136, 66, 120};
    canvas_.drawPanel(dnBtn, 8, Color(30, 30, 40), Color(62, 62, 76), 1.0f);
    canvas_.drawTextCentered(dnBtn, "DOWN", Palette::TextPrimary, 1, true);
}

void PluginManagerView::drawMpcSanitySubTab() {
    int W = canvas_.getLogicalWidth();
    bool isNativeVst2 = config::ConfigParser::isMpcNativeVst2Supported();

    // Top Box: Sanity Checks
    Rect bTop = {30, 174, W - 60, 416};
    canvas_.drawPanel(bTop, 10, Color(24, 24, 30), Color(46, 46, 58), 1.0f);
    std::string sanityTitle = isNativeVst2
        ? "1, 2 & 3. NATIVE VST2 SANITY CHECKS (MPC >= 3.10: KNOWNPLUGINS, RELOCATE, SYNTHLOCATIONS)"
        : "1 & 2. ORPHAN PLUGINS & MISSING .SO (STAGED: /tmp/MPC.settings.new)";
    canvas_.drawText(bTop.x + 20, bTop.y + 16, sanityTitle, Palette::TextPrimary, 1, true);

    if (config::ConfigParser::hasStagedMpcSettings()) {
        Rect stBadge = {bTop.x + bTop.w - 410, bTop.y + 10, 198, 30};
        canvas_.drawPanel(stBadge, 5, Color(16, 44, 30), Palette::StatusGreen, 1.0f);
        canvas_.drawTextCentered(stBadge, "STAGED IN /tmp (.new)", Palette::StatusGreen, 1, true);
    }

    Rect scanBtn = {bTop.x + bTop.w - 200, bTop.y + 8, 180, 34};
    canvas_.drawPanel(scanBtn, 6, Color(35, 35, 46), Palette::AmberGold, 1.5f);
    canvas_.drawRefreshIcon(scanBtn.x + 14, scanBtn.y + 9, 16, Palette::AmberGold);
    canvas_.drawText(scanBtn.x + 42, scanBtn.y + 10, "SCAN NOW", Palette::TextPrimary, 1, true);

    canvas_.drawHLine(bTop.x + 20, bTop.y + 46, bTop.w - 40, Color(46, 46, 58));

    int rowY = bTop.y + 54;
    int rowH = 58;
    int maxVisible = 5;

    if (sanityIssues_.empty()) {
        Rect okBox = {bTop.x + 24, rowY + 26, bTop.w - 48, 86};
        canvas_.drawPanel(okBox, 8, Color(16, 34, 26), Palette::StatusGreen, 1.5f);
        canvas_.drawGlowDot(okBox.x + 24, okBox.y + 43, 5, Palette::StatusGreen);
        canvas_.drawText(
            okBox.x + 46, okBox.y + 22,
            isNativeVst2
                ? "ALL NATIVE VST2 SANITY CHECKS PASSED (MPC OS >= 3.10)"
                : "ALL SANITY CHECKS PASSED (0 ANOMALIES IN STAGED /tmp/MPC.settings.new)",
            Palette::StatusGreen, 1, true
        );
        canvas_.drawText(
            okBox.x + 46, okBox.y + 48,
            isNativeVst2
                ? "No KNOWNPLUGINS or VST2 SynthContentLocations entries in MPC.settings; plugins reside at <Mount>/VST2 Plugins."
                : "Every plugin folder in writable VST2 Plugins locations is registered in KNOWNPLUGINS, and all .so paths exist.",
            Palette::TextSecondary, 1, false
        );
    } else {
        int maxOffset = std::max(0, static_cast<int>(sanityIssues_.size()) - maxVisible);
        if (sanityScrollOffset_ > maxOffset) sanityScrollOffset_ = maxOffset;
        if (sanityScrollOffset_ < 0) sanityScrollOffset_ = 0;

        size_t start = static_cast<size_t>(sanityScrollOffset_);
        size_t end = std::min(sanityIssues_.size(), start + static_cast<size_t>(maxVisible));

        for (size_t i = start; i < end; ++i) {
            bool isSel = (static_cast<int>(i) == selectedSanityIdx_);
            const auto& iss = sanityIssues_[i];

            std::string badgeLabel = "1. ORPHAN SYNTH";
            Color badgeBg = Color(54, 38, 14);
            Color badgeFg = Palette::AmberGold;
            std::string detailPath = "Dir: " + iss.dirPath;
            std::string primaryBtnLabel = "REPAIR";
            bool allowDelete = true;

            if (iss.type == config::SanityCheckIssue::MISSING_SO_PATH) {
                badgeLabel = "2. MISSING .SO";
                badgeBg = Color(58, 18, 24);
                badgeFg = Color(255, 110, 120);
                detailPath = "Missing: " + iss.soPath;
            } else if (iss.type == config::SanityCheckIssue::LEGACY_KNOWNPLUGINS) {
                badgeLabel = "1. KNOWNPLUGINS";
                badgeBg = Color(54, 38, 14);
                badgeFg = Palette::AmberGold;
                detailPath = iss.dirPath;
                primaryBtnLabel = "PURGE ALL";
                allowDelete = false;
            } else if (iss.type == config::SanityCheckIssue::OUTSIDE_VST2_LOCATION) {
                badgeLabel = "2. OUTSIDE VST2";
                badgeBg = Color(24, 42, 62);
                badgeFg = Color(120, 200, 255);
                detailPath = "Outside mount-root VST2 Plugins: " + iss.dirPath;
                primaryBtnLabel = "REINSTALL";
            } else if (iss.type == config::SanityCheckIssue::LEGACY_SYNTH_VST2_LOCATIONS) {
                badgeLabel = "3. SYNTHLOCATIONS";
                badgeBg = Color(48, 28, 56);
                badgeFg = Color(216, 160, 255);
                detailPath = "Purge VST2 Plugins <Location> entries in MPC.settings: " + iss.dirPath;
                primaryBtnLabel = "PURGE ALL";
                allowDelete = false;
            }

            Rect rRow = {bTop.x + 18, rowY, bTop.w - 36, rowH};
            canvas_.drawPanel(rRow, 6, isSel ? Color(40, 24, 32) : Color(18, 18, 24), isSel ? Palette::AkaiCrimson : Color(44, 44, 56), isSel ? 2.0f : 1.0f);

            // Type Badge
            Rect rBadge = {rRow.x + 10, rRow.y + 12, 150, 34};
            canvas_.drawPanel(rBadge, 4, badgeBg, badgeFg, 1.0f);
            canvas_.drawTextCentered(rBadge, badgeLabel, badgeFg, 1, true);

            // Plugin Name & Path
            canvas_.drawTextClipped({rRow.x + 172, rRow.y + 8, 610, 22}, iss.pluginName, Palette::TextPrimary, 1, true);
            canvas_.drawTextClipped({rRow.x + 172, rRow.y + 32, 610, 20}, detailPath, Palette::TextSecondary, 1, false);

            // Action Buttons
            Rect btnRep = {rRow.x + 796, rRow.y + 9, 122, 40};
            canvas_.drawPanel(btnRep, 6, Color(20, 82, 46), Palette::StatusGreen, 1.0f);
            canvas_.drawTextCentered(btnRep, primaryBtnLabel, Palette::TextPrimary, 1, true);

            if (allowDelete) {
                Rect btnDel = {rRow.x + 926, rRow.y + 9, 122, 40};
                canvas_.drawPanel(btnDel, 6, Color(88, 22, 32), Palette::AkaiCrimson, 1.0f);
                canvas_.drawTextCentered(btnDel, "DELETE", Palette::TextPrimary, 1, true);
            }

            Rect btnIgn = {rRow.x + 1056, rRow.y + 9, 118, 40};
            canvas_.drawPanel(btnIgn, 6, Color(36, 36, 46), Color(82, 82, 98), 1.0f);
            canvas_.drawTextCentered(btnIgn, "IGNORE", Palette::TextSecondary, 1, true);

            rowY += rowH + 6;
        }
    }

    // Bottom Box: Check 3 (Apply .so path modifications to all installed plugin presets .xpl)
    Rect bBot = {30, 602, W - 60, 158};
    canvas_.drawPanel(bBot, 10, Color(24, 24, 30), Color(46, 46, 58), 1.0f);
    canvas_.drawText(bBot.x + 20, bBot.y + 16, "3. APPLY .SO PATHS TO INSTALLED PLUGIN PRESETS (.XPL)", Palette::AmberGold, 1, true);
    canvas_.drawHLine(bBot.x + 20, bBot.y + 40, bBot.w - 40, Color(46, 46, 58));

    canvas_.drawText(
        bBot.x + 20, bBot.y + 54,
        isNativeVst2
            ? "Scans Presets/*.xpl for every plugin in VST2 Plugins folders and updates <PLUGIN file='...'/>"
            : "Scans Presets/*.xpl for every plugin in staged /tmp/MPC.settings.new and updates <PLUGIN file='...'/>",
        Palette::TextSecondary, 1, false
    );
    canvas_.drawText(bBot.x + 20, bBot.y + 78, "with the target .so path (preserving <version file='1'/> and binary <state> payload).", Palette::TextMuted, 1, false);

    if (!settingsStatusMsg_.empty()) {
        Color stCol = (settingsStatusColor_ == 2) ? Color(255, 110, 120) : Palette::StatusGreen;
        canvas_.drawTextClipped({bBot.x + 20, bBot.y + 114, 780, 26}, settingsStatusMsg_, stCol, 1, true);
    }

    Rect applyPresetsBtn = {bBot.x + bBot.w - 380, bBot.y + 52, 360, 64};
    canvas_.drawPanel(applyPresetsBtn, 8, Palette::AkaiCrimson, Color(255, 100, 120), 1.5f);
    canvas_.drawTextCentered(applyPresetsBtn, "APPLY TO ALL PRESETS (.XPL)", Palette::TextPrimary, 1, true);
}

void PluginManagerView::onWheelDelta(int delta) {
    if (wakeIfScreensaver()) return;
    if (uiState_ == UIState::SETTINGS_SCREEN) {
        if (settingsTab_ == 1) {
            if (mpcSettingsSubTab_ == 0 && !mpcBackups_.empty()) {
                selectedBackupIdx_ = std::max(0, std::min(static_cast<int>(mpcBackups_.size()) - 1, selectedBackupIdx_ + delta));
                if (selectedBackupIdx_ < backupScrollOffset_) {
                    backupScrollOffset_ = selectedBackupIdx_;
                } else if (selectedBackupIdx_ >= backupScrollOffset_ + 9) {
                    backupScrollOffset_ = selectedBackupIdx_ - 8;
                }
                render();
            } else if (mpcSettingsSubTab_ == 1 && !bootLogLines_.empty()) {
                int maxOffset = std::max(0, static_cast<int>(bootLogLines_.size()) - 16);
                bootLogScrollOffset_ = std::max(0, std::min(maxOffset, bootLogScrollOffset_ + delta * 3));
                render();
            } else if (mpcSettingsSubTab_ == 2 && !sanityIssues_.empty()) {
                selectedSanityIdx_ = std::max(0, std::min(static_cast<int>(sanityIssues_.size()) - 1, selectedSanityIdx_ + delta));
                if (selectedSanityIdx_ < sanityScrollOffset_) {
                    sanityScrollOffset_ = selectedSanityIdx_;
                } else if (selectedSanityIdx_ >= sanityScrollOffset_ + 5) {
                    sanityScrollOffset_ = selectedSanityIdx_ - 4;
                }
                render();
            }
        }
        return;
    }
    if (uiState_ == UIState::V310_WARNING_POPUP) {
        return;
    }
    if (uiState_ == UIState::INSTALL_POPUP) {
        if (!synthLocations_.empty()) {
            selectedSynthLocIdx_ = std::max(0, std::min(static_cast<int>(synthLocations_.size()) - 1, selectedSynthLocIdx_ + delta));
            render();
        }
        return;
    }
    if (!filteredItems_.empty()) {
        selectedIndex_ = std::max(0, std::min(static_cast<int>(filteredItems_.size()) - 1, selectedIndex_ + delta));
        render();
    }
}

void PluginManagerView::onWheelPress() {
    if (wakeIfScreensaver()) return;
    if (uiState_ == UIState::SETTINGS_SCREEN) {
        if (settingsTab_ == 1) {
            if (mpcSettingsSubTab_ == 0) {
                openRestoreBackupPopup();
            } else if (mpcSettingsSubTab_ == 1) {
                refreshBootLogs();
                render();
            } else if (mpcSettingsSubTab_ == 2) {
                if (!sanityIssues_.empty() && selectedSanityIdx_ >= 0 && selectedSanityIdx_ < static_cast<int>(sanityIssues_.size())) {
                    std::string msg;
                    bool ok = config::ConfigParser::repairSanityIssue(config_, sanityIssues_[selectedSanityIdx_], msg);
                    settingsStatusMsg_ = msg;
                    settingsStatusColor_ = ok ? 1 : 2;
                    refreshSanityChecks();
                    rebuildFilteredItems();
                    render();
                } else {
                    int plugCnt = 0;
                    std::string msg;
                    config::ConfigParser::applyPresetsPathsForAllInstalled(config_, plugCnt, msg);
                    settingsStatusMsg_ = msg;
                    settingsStatusColor_ = 1;
                    render();
                }
            }
        }
        return;
    }
    if (uiState_ == UIState::RESTORE_BACKUP_POPUP) {
        executeRestoreSelectedBackup();
        return;
    }
    if (uiState_ == UIState::DELETE_BACKUP_POPUP) {
        executeDeleteSelectedBackup();
        return;
    }
    if (uiState_ == UIState::EXIT_POPUP) {
        shouldExit_ = true;
        return;
    }
    if (uiState_ == UIState::PURGE_POPUP) {
        closePurgePopup();
        triggerDeleteCurrentItemBinary();
        return;
    }
    if (uiState_ == UIState::REFRESH_POPUP) {
        closeRefreshPopup();
        triggerCatalogRefresh();
        return;
    }
    if (uiState_ == UIState::V310_WARNING_POPUP) {
        openSanityChecksFromWarning();
        return;
    }
    if (uiState_ == UIState::INSTALL_POPUP) {
        confirmInstall();
        return;
    }
    if (uiState_ == UIState::MENU && !filteredItems_.empty()) {
        uiState_ = UIState::LAUNCH_SCREEN;
        render();
    } else if (uiState_ == UIState::LAUNCH_SCREEN && !filteredItems_.empty()) {
        const auto& ref = filteredItems_[selectedIndex_];
        const auto& item = config_.sections[ref.sectionIdx].items[ref.itemIdx];
        if (item.installed == 2) {
            triggerRepair();
        } else {
            openInstallPopup();
        }
    } else if (uiState_ == UIState::EXEC_SCREEN && execFinished_) {
        rebuildFilteredItems();
        uiState_ = UIState::MENU;
        render();
    }
}

void PluginManagerView::onMenuButton() {
    if (wakeIfScreensaver()) return;
    if (uiState_ == UIState::V310_WARNING_POPUP) {
        closeV310WarningPopup();
        return;
    }
    if (uiState_ == UIState::RESTORE_BACKUP_POPUP) {
        closeRestoreBackupPopup();
        return;
    }
    if (uiState_ == UIState::DELETE_BACKUP_POPUP) {
        closeDeleteBackupPopup();
        return;
    }
    if (uiState_ == UIState::SETTINGS_SCREEN) {
        closeSettingsScreen();
        return;
    }
    if (uiState_ == UIState::EXIT_POPUP) {
        closeExitPopup();
        return;
    }
    if (uiState_ == UIState::PURGE_POPUP) {
        closePurgePopup();
        return;
    }
    if (uiState_ == UIState::REFRESH_POPUP) {
        closeRefreshPopup();
        return;
    }
    if (uiState_ == UIState::INSTALL_POPUP) {
        closeInstallPopup();
        return;
    }
    if (uiState_ == UIState::LAUNCH_SCREEN) {
        uiState_ = UIState::MENU;
        render();
    } else if (uiState_ == UIState::EXEC_SCREEN && execFinished_) {
        rebuildFilteredItems();
        uiState_ = UIState::MENU;
        render();
    } else if (uiState_ == UIState::MENU) {
        openExitPopup();
    }
}

void PluginManagerView::onTouchDown(int lx, int ly) {
    if (wakeIfScreensaver()) return;
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();

    if (uiState_ == UIState::V310_WARNING_POPUP) {
        int mw = 820;
        int mh = 430;
        int mx = (W - mw) / 2;
        int my = (H - mh) / 2;
        int btnY = my + 362;
        // DISMISS button: {mx + 28, btnY, 240, 48}
        if (lx >= mx + 24 && lx <= mx + 272 && ly >= btnY - 6 && ly <= btnY + 54) {
            closeV310WarningPopup();
            return;
        }
        // OPEN SANITY CHECKS button: {mx + mw - 28 - 350, btnY, 350, 48}
        int sanityX = mx + mw - 28 - 350;
        if (lx >= sanityX - 6 && lx <= sanityX + 356 && ly >= btnY - 6 && ly <= btnY + 54) {
            openSanityChecksFromWarning();
            return;
        }
        return;
    }

    if (uiState_ == UIState::PURGE_POPUP) {
        int mw = 580;
        int mh = 280;
        int mx = (W - mw) / 2;
        int my = (H - mh) / 2;
        int btnW = 240;
        int btnY = my + 206;

        if (lx >= mx + 20 && lx <= mx + 24 + btnW && ly >= btnY - 5 && ly <= btnY + 55) {
            closePurgePopup();
            return;
        }
        int purgeX = mx + mw - 24 - btnW;
        if (lx >= purgeX - 5 && lx <= purgeX + btnW + 5 && ly >= btnY - 5 && ly <= btnY + 55) {
            closePurgePopup();
            triggerDeleteCurrentItemBinary();
            return;
        }
        return;
    }

    if (uiState_ == UIState::REFRESH_POPUP) {
        int mw = 680;
        int mh = 310;
        int mx = (W - mw) / 2;
        int my = (H - mh) / 2;
        int btnW = 196;
        int btnY = my + 218;

        if (lx >= mx + 20 && lx <= mx + 24 + btnW && ly >= btnY - 5 && ly <= btnY + 55) {
            closeRefreshPopup();
            return;
        }
        int noX = mx + 24 + btnW + 18;
        if (lx >= noX - 5 && lx <= noX + btnW + 5 && ly >= btnY - 5 && ly <= btnY + 55) {
            closeRefreshPopup();
            triggerCatalogRefresh();
            return;
        }
        int yesX = mx + mw - 24 - btnW;
        if (lx >= yesX - 5 && lx <= yesX + btnW + 5 && ly >= btnY - 5 && ly <= btnY + 55) {
            closeRefreshPopup();
            triggerCacheAllBinaries();
            return;
        }
        return;
    }

    if (uiState_ == UIState::EXIT_POPUP) {
        int mw = 560;
        int mh = 270;
        int mx = (W - mw) / 2;
        int my = (H - mh) / 2;

        if (lx >= mx + 20 && lx <= mx + 265 && ly >= my + 190 && ly <= my + 255) {
            closeExitPopup();
            return;
        }
        if (lx >= mx + mw - 265 && lx <= mx + mw - 20 && ly >= my + 190 && ly <= my + 255) {
            shouldExit_ = true;
            return;
        }
        return;
    }

    if (uiState_ == UIState::EXEC_SCREEN && execFinished_) {
        rebuildFilteredItems();
        uiState_ = UIState::MENU;
        render();
        return;
    }

    if (uiState_ == UIState::INSTALL_POPUP) {
        int mw = 760;
        int mh = 640;
        int mx = (W - mw) / 2;
        int my = (H - mh) / 2;

        int rowY = my + 120;
        int rowH = 54;
        for (size_t i = 0; i < synthLocations_.size() && i < 5; ++i) {
            if (lx >= mx + 20 && lx <= mx + mw - 20 && ly >= rowY && ly <= rowY + rowH) {
                selectedSynthLocIdx_ = static_cast<int>(i);
                render();
                return;
            }
            rowY += rowH + 8;
        }

        if (lx >= mx + 20 && lx <= mx + 200 && ly >= my + 570 && ly <= my + 635) {
            closeInstallPopup();
            return;
        }
        if (lx >= mx + 215 && lx <= mx + 405 && ly >= my + 570 && ly <= my + 635) {
            if (!filteredItems_.empty()) {
                const auto& ref = filteredItems_[selectedIndex_];
                const auto& item = config_.sections[ref.sectionIdx].items[ref.itemIdx];
                if (item.installed == 2) {
                    triggerUninstall();
                    return;
                }
            }
        }
        if (lx >= mx + mw - 240 && lx <= mx + mw - 20 && ly >= my + 570 && ly <= my + 635) {
            confirmInstall();
            return;
        }
        return;
    }

    if (uiState_ == UIState::RESTORE_BACKUP_POPUP) {
        int mw = 640;
        int mh = 320;
        int mx = (W - mw) / 2;
        int my = (H - mh) / 2;
        if (lx >= mx + 30 && lx <= mx + 290 && ly >= my + 230 && ly <= my + 300) {
            closeRestoreBackupPopup();
            return;
        }
        if (lx >= mx + mw - 290 && lx <= mx + mw - 30 && ly >= my + 230 && ly <= my + 300) {
            executeRestoreSelectedBackup();
            return;
        }
        return;
    }

    if (uiState_ == UIState::DELETE_BACKUP_POPUP) {
        int mw = 640;
        int mh = 320;
        int mx = (W - mw) / 2;
        int my = (H - mh) / 2;
        if (lx >= mx + 30 && lx <= mx + 290 && ly >= my + 230 && ly <= my + 300) {
            closeDeleteBackupPopup();
            return;
        }
        if (lx >= mx + mw - 290 && lx <= mx + mw - 30 && ly >= my + 230 && ly <= my + 300) {
            executeDeleteSelectedBackup();
            return;
        }
        return;
    }

    if (uiState_ == UIState::SETTINGS_SCREEN) {
        // 1. < BACK Button (x: 16..135, y: 8..55)
        if (lx >= 16 && lx <= 135 && ly >= 8 && ly <= 55) {
            closeSettingsScreen();
            return;
        }
        // 2. Tab 0: PLUGIN MANAGER (x: 550..785, y: 8..55)
        if (lx >= 550 && lx <= 785 && ly >= 8 && ly <= 55) {
            settingsTab_ = 0;
            settingsStatusMsg_.clear();
            render();
            return;
        }
        // 3. Tab 1: MPC SETTINGS (x: 795..1030, y: 8..55)
        if (lx >= 795 && lx <= 1030 && ly >= 8 && ly <= 55) {
            settingsTab_ = 1;
            settingsStatusMsg_.clear();
            refreshMpcBackups();
            if (mpcSettingsSubTab_ == 1) refreshBootLogs();
            if (mpcSettingsSubTab_ == 2) refreshSanityChecks();
            render();
            return;
        }
        // 4. Tab 0 interactions (Plugin Manager: Screen Rotation selector)
        if (settingsTab_ == 0) {
            if (ly >= 228 && ly <= 268) {
                const int rotVals[4] = {0, 90, 180, 270};
                const char* rotLabels[4] = {"No rotation", "90", "180", "270"};
                const int rotBtnX[4] = {360, 507, 654, 801};
                for (int i = 0; i < 4; ++i) {
                    if (lx >= rotBtnX[i] - 4 && lx <= rotBtnX[i] + 139) {
                        config_.screen_rotation = rotVals[i];
                        canvas_.setRotation(rotVals[i]);
                        bool saved = config::ConfigParser::saveToFile(config_.config_path, config_);
                        config::ConfigParser::logMessage(
                            "[TKGL-CFG] Screen rotation set to " + std::string(rotLabels[i]) +
                            " (" + std::to_string(rotVals[i]) + " deg)" +
                            (saved ? " and saved to mpc_plugins.conf." : " (warning: saveToFile failed).")
                        );
                        settingsStatusMsg_ = saved
                            ? ("Rotation saved: " + std::string(rotLabels[i]))
                            : "Error saving rotation to mpc_plugins.conf";
                        settingsStatusColor_ = saved ? 1 : 2;
                        render();
                        return;
                    }
                }
            }
            return;
        }
        // 5. Tab 1 interactions (MPC Settings with 3 sub-tabs: ROLL BACK, BOOT LOG, SANITY CHECKS)
        if (settingsTab_ == 1) {
            // Sub-tab selector row (y: 118..166)
            if (ly >= 118 && ly <= 166) {
                int subX[3] = {30, 443, 856};
                for (int i = 0; i < 3; ++i) {
                    if (lx >= subX[i] && lx <= subX[i] + 394) {
                        mpcSettingsSubTab_ = i;
                        settingsStatusMsg_.clear();
                        if (i == 0) refreshMpcBackups();
                        if (i == 1) refreshBootLogs();
                        if (i == 2) refreshSanityChecks();
                        render();
                        return;
                    }
                }
            }

            if (mpcSettingsSubTab_ == 0) {
                // Sub-tab 0: ROLL BACK
                // Backup list item rows (y: 222..690, stride 52, x: 40..940)
                if (lx >= 40 && lx <= 940 && ly >= 222 && ly <= 690) {
                    int row = (ly - 222) / 52;
                    int actualIdx = backupScrollOffset_ + row;
                    if (actualIdx >= 0 && actualIdx < static_cast<int>(mpcBackups_.size())) {
                        selectedBackupIdx_ = actualIdx;
                        render();
                        return;
                    }
                }
                // [ REFRESH LIST ] button (x: 960..1220, y: 220..276)
                if (lx >= 960 && lx <= 1220 && ly >= 220 && ly <= 276) {
                    refreshMpcBackups();
                    settingsStatusMsg_ = "Backup list refreshed.";
                    settingsStatusColor_ = 1;
                    render();
                    return;
                }
                // [ RESTORE BACKUP ] button (x: 960..1220, y: 282..344)
                if (lx >= 960 && lx <= 1220 && ly >= 282 && ly <= 344) {
                    openRestoreBackupPopup();
                    return;
                }
                // [ DELETE BACKUP ] button (x: 960..1220, y: 350..406)
                if (lx >= 960 && lx <= 1220 && ly >= 350 && ly <= 406) {
                    openDeleteBackupPopup();
                    return;
                }
            } else if (mpcSettingsSubTab_ == 1) {
                // Sub-tab 1: BOOT LOG
                // [ REFRESH LOG ] button: x: 1040..1240, y: 178..220
                if (lx >= 1040 && lx <= 1240 && ly >= 178 && ly <= 220) {
                    refreshBootLogs();
                    render();
                    return;
                }
                // [ UP ] button: x: 1158..1244, y: 226..354
                if (lx >= 1158 && lx <= 1244 && ly >= 226 && ly <= 354) {
                    bootLogScrollOffset_ = std::max(0, bootLogScrollOffset_ - 8);
                    render();
                    return;
                }
                // [ DOWN ] button: x: 1158..1244, y: 622..750
                if (lx >= 1158 && lx <= 1244 && ly >= 622 && ly <= 750) {
                    int maxOffset = std::max(0, static_cast<int>(bootLogLines_.size()) - 16);
                    bootLogScrollOffset_ = std::min(maxOffset, bootLogScrollOffset_ + 8);
                    render();
                    return;
                }
            } else if (mpcSettingsSubTab_ == 2) {
                // Sub-tab 2: SANITY CHECKS
                // [ SCAN NOW ] button: x: 1040..1240, y: 178..220
                if (lx >= 1040 && lx <= 1240 && ly >= 178 && ly <= 220) {
                    refreshSanityChecks();
                    settingsStatusMsg_ = "Sanity check scan completed (" + std::to_string(sanityIssues_.size()) + " issue(s) found).";
                    settingsStatusColor_ = 1;
                    render();
                    return;
                }
                // Issue rows (y: 228..548, row stride = 64)
                if (ly >= 228 && ly <= 548 && !sanityIssues_.empty()) {
                    int row = (ly - 228) / 64;
                    int actualIdx = sanityScrollOffset_ + row;
                    if (actualIdx >= 0 && actualIdx < static_cast<int>(sanityIssues_.size())) {
                        selectedSanityIdx_ = actualIdx;
                        const auto issueCopy = sanityIssues_[actualIdx];
                        // [ REPAIR / PURGE ALL / REINSTALL ] button: x: 840..968
                        if (lx >= 840 && lx <= 968) {
                            std::string msg;
                            bool ok = config::ConfigParser::repairSanityIssue(config_, issueCopy, msg);
                            settingsStatusMsg_ = msg;
                            settingsStatusColor_ = ok ? 1 : 2;
                            refreshSanityChecks();
                            rebuildFilteredItems();
                            render();
                            return;
                        }
                        // [ DELETE ] button: x: 970..1098 (not applicable to LEGACY_KNOWNPLUGINS or LEGACY_SYNTH_VST2_LOCATIONS)
                        if (issueCopy.type != config::SanityCheckIssue::LEGACY_KNOWNPLUGINS &&
                            issueCopy.type != config::SanityCheckIssue::LEGACY_SYNTH_VST2_LOCATIONS &&
                            lx >= 970 && lx <= 1098) {
                            std::string msg;
                            bool ok = config::ConfigParser::deleteSanityIssue(config_, issueCopy, msg);
                            settingsStatusMsg_ = msg;
                            settingsStatusColor_ = ok ? 1 : 2;
                            refreshSanityChecks();
                            rebuildFilteredItems();
                            render();
                            return;
                        }
                        // [ IGNORE ] button: x: 1100..1225
                        if (lx >= 1100 && lx <= 1225) {
                            settingsStatusMsg_ = "Ignored sanity issue: " + issueCopy.pluginName;
                            settingsStatusColor_ = 1;
                            sanityIssues_.erase(sanityIssues_.begin() + actualIdx);
                            if (selectedSanityIdx_ >= static_cast<int>(sanityIssues_.size())) {
                                selectedSanityIdx_ = sanityIssues_.empty() ? 0 : static_cast<int>(sanityIssues_.size()) - 1;
                            }
                            render();
                            return;
                        }
                        render();
                        return;
                    }
                }
                // [ APPLY TO ALL PRESETS (.XPL) ] button: x: 860..1240, y: 648..724
                if (lx >= 860 && lx <= 1240 && ly >= 648 && ly <= 724) {
                    int plugCnt = 0;
                    std::string msg;
                    config::ConfigParser::applyPresetsPathsForAllInstalled(config_, plugCnt, msg);
                    settingsStatusMsg_ = msg;
                    settingsStatusColor_ = 1;
                    render();
                    return;
                }
            }
        }
        return;
    }

    // Top Header: Poloq 4-Square Grid Icon (x: 16..68, y: 8..55)
    // Opens Advanced Settings screen at any time (from MENU, LAUNCH_SCREEN, etc.)
    if (lx >= 16 && lx <= 68 && ly >= 8 && ly <= 55) {
        if (uiState_ != UIState::SETTINGS_SCREEN && uiState_ != UIState::EXEC_SCREEN) {
            openSettingsScreen();
            return;
        }
    }

    // Top Header: REFRESH Button (opens CACHE ALL PLUGIN PACKAGES? prompt modal)
    // ONLY available on MENU screen!
    if (uiState_ == UIState::MENU && ly >= 8 && ly <= 55 && lx >= W - 448 && lx <= W - 310) {
        openRefreshPopup();
        return;
    }

    // Bottom Pagination Bar (< and > and EXIT)
    int barY = H - 64;
    if (ly >= barY) {
        if (lx >= 20 && lx <= 66) { onWheelDelta(-itemsPerPage_); return; }
        if (lx >= 154 && lx <= 204) { onWheelDelta(itemsPerPage_); return; }
        if (lx >= W - 130 && lx <= W - 20) { openExitPopup(); return; }
        return;
    }

    if (uiState_ == UIState::LAUNCH_SCREEN) {
        // < BACK button
        if (lx >= 20 && lx <= 140 && ly >= 72 && ly <= 132) {
            uiState_ = UIState::MENU;
            render();
            return;
        }

        if (!filteredItems_.empty()) {
            const auto& ref = filteredItems_[selectedIndex_];
            const auto& item = config_.sections[ref.sectionIdx].items[ref.itemIdx];
            bool isCached = config::ConfigParser::isItemBinaryCached(item);

            // Single ZIP button:
            // - If cached: Button displays "ZIP: X.X MB" -> Tapping opens confirmation popup to purge
            // - If not cached: Button displays "CACHE ZIP" -> Tapping triggers immediate download & cache
            // Positioned at W - 496, w = 130, h = 44 (x: W - 505 .. W - 360, y: 72..132)
            if (lx >= W - 505 && lx <= W - 360 && ly >= 72 && ly <= 132) {
                if (isCached) {
                    openPurgePopup();
                } else {
                    triggerCacheCurrentItem();
                }
                return;
            }

            // UNINSTALL button (available when installed & healthy (1) OR broken (2), W - 356 .. W - 200)
            if ((item.installed == 1 || item.installed == 2) && lx >= W - 360 && lx <= W - 200 && ly >= 72 && ly <= 132) {
                triggerUninstall();
                return;
            }
        }

        // INSTALL / REINSTALL / REPAIR button (W - 190 .. W - 20, y: 72..132)
        if (lx >= W - 190 && ly >= 72 && ly <= 132) {
            if (!filteredItems_.empty()) {
                const auto& ref = filteredItems_[selectedIndex_];
                const auto& item = config_.sections[ref.sectionIdx].items[ref.itemIdx];
                if (item.installed == 2) {
                    triggerRepair();
                    return;
                }
            }
            openInstallPopup();
            return;
        }
        return;
    }

    if (uiState_ == UIState::MENU) {
        // Row 1: DISCOVER | INSTALLED | UPDATES + Level 3 Category Filter + Sort (y = 62..107)
        if (ly >= 62 && ly < 108) {
            if (lx >= 22 && lx < 166) { activeTab_ = FilterTab::DISCOVER; selectedIndex_ = 0; rebuildFilteredItems(); render(); return; }
            if (lx >= 166 && lx < 310) { activeTab_ = FilterTab::INSTALLED; selectedIndex_ = 0; rebuildFilteredItems(); render(); return; }
            if (lx >= 310 && lx < 454) { activeTab_ = FilterTab::UPDATES; selectedIndex_ = 0; rebuildFilteredItems(); render(); return; }
            if (lx >= W - 435 && lx <= W - 215) {
                cycleCategoryFilter();
                return;
            }
            if (lx >= W - 215 && lx <= W - 15) {
                cycleSortOption();
                return;
            }
            return;
        }

        // Row 2: SECTION CLASSIFICATION TAB BAR (y = 108..148)
        if (ly >= 108 && ly <= 148) {
            int totalAll = 0;
            for (const auto& s : config_.sections) {
                for (const auto& it : s.items) {
                    if (activeTab_ == FilterTab::INSTALLED && (it.installed == 0)) continue;
                    if (activeTab_ == FilterTab::UPDATES && !it.updateAvail) continue;
                    totalAll++;
                }
            }
            int tabX = 22;
            int numSec = static_cast<int>(config_.sections.size());
            for (int idx = -1; idx < numSec; ++idx) {
                std::string title = (idx < 0) ? "ALL" : config_.sections[idx].title;
                int cnt = 0;
                if (idx < 0) {
                    cnt = totalAll;
                } else {
                    for (const auto& it : config_.sections[idx].items) {
                        if (activeTab_ == FilterTab::INSTALLED && (it.installed == 0)) continue;
                        if (activeTab_ == FilterTab::UPDATES && !it.updateAvail) continue;
                        cnt++;
                    }
                }
                std::string cntStr = std::to_string(cnt);
                int textW = canvas_.measureText(title, 1);
                int pillW = std::max(24, canvas_.measureText(cntStr, 1) + 12);
                int tabW = textW + pillW + 26;
                if (lx >= tabX && lx <= tabX + tabW) {
                    sectionFilter_ = idx;
                    categoryFilter_ = "ALL";
                    selectedIndex_ = 0;
                    rebuildFilteredItems();
                    render();
                    return;
                }
                tabX += tabW + 6;
            }
            return;
        }

        int currentPage = selectedIndex_ / itemsPerPage_;
        int startIdx = currentPage * itemsPerPage_;
        for (int c = 0; c < itemsPerPage_; ++c) {
            int idx = startIdx + c;
            if (idx >= static_cast<int>(filteredItems_.size())) break;
            int cardY = 158 + c * 190;
            if (ly >= cardY && ly <= cardY + 180) {
                selectedIndex_ = idx;
                if (lx >= W - 210) {
                    openInstallPopup();
                } else {
                    uiState_ = UIState::LAUNCH_SCREEN;
                    render();
                }
                return;
            }
        }
    }
}

} // namespace tkgl::ui
