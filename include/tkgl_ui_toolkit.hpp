/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: include/tkgl_ui_toolkit.hpp
 * Description: High-Definition Poloq & KikGen Hardware UI Toolkit
 *              1:1 Visual Match with the Web Hardware Simulator (image_22ce5693.png)
 * ============================================================================
 */

#pragma once

#include "tkgl_gfx.hpp"
#include "tkgl_config.hpp"
#include <string>
#include <vector>
#include <ctime>

namespace tkgl::ui {

enum class UIState {
    MENU          = 0, // Poloq-style 3-card paginated List View
    LAUNCH_SCREEN = 1, // High-definition Detail View matching image_22ce5693.png
    EXEC_SCREEN   = 2, // Live Shell Script / Online Catalog Sync Console
    INSTALL_POPUP = 3, // Installation Popup with SynthContentLocations pick list
    EXIT_POPUP    = 4, // Confirmation Popup to quit the Plugin Manager
    REFRESH_POPUP = 5, // Prompt: CACHE ALL PLUGIN PACKAGES? (YES / NO / CANCEL)
    PURGE_POPUP   = 6, // Confirmation Popup to delete/purge local cached ZIP package
    SETTINGS_SCREEN = 7, // Advanced Settings (Tab 1: Plugin Manager / Tab 2: MPC Settings & Rollback)
    RESTORE_BACKUP_POPUP = 8, // Confirmation Modal before restoring selected MPC.settings backup
    DELETE_BACKUP_POPUP  = 9  // Confirmation Modal before permanently deleting selected backup
};

enum class FilterTab {
    DISCOVER  = 0,
    INSTALLED = 1,
    UPDATES   = 2
};

struct FilteredItemRef {
    int sectionIdx;
    int itemIdx;
};

class PluginManagerView {
private:
    gfx::DrmFramebufferCanvas& canvas_;
    config::LauncherConfig&    config_;

    UIState   uiState_       = UIState::MENU;
    UIState   prevUiState_   = UIState::MENU;
    FilterTab activeTab_     = FilterTab::DISCOVER;
    int       sectionFilter_ = -1;
    std::string categoryFilter_ = "ALL";
    int       sortBy_        = 0; // 0 = UPDATED, 1 = NAME, 2 = SIZE, 3 = DOWNLOADS
    int       selectedIndex_ = 0;
    int       itemsPerPage_  = 3;

    std::vector<FilteredItemRef>       filteredItems_;
    std::vector<std::string>           execLogs_;
    bool                               execFinished_        = false;
    int                                animFrame_           = 0;
    bool                               isRefreshing_        = false;
    bool                               isOnline_            = false;
    std::vector<config::SynthLocation> synthLocations_;
    int                                selectedSynthLocIdx_ = 0;

    int                                settingsTab_          = 0; // 0 = Plugin Manager, 1 = MPC Settings
    int                                mpcSettingsSubTab_    = 0; // 0 = ROLL BACK, 1 = LIMITS, 2 = SSH, 3 = BOOT LOG, 4 = SANITY CHECKS
    std::vector<config::MpcSettingsBackup> mpcBackups_;
    int                                selectedBackupIdx_    = 0;
    int                                backupScrollOffset_   = 0;
    std::vector<std::string>           bootLogLines_;
    int                                bootLogScrollOffset_  = 0;
    int                                sshEnabledMode_       = 0; // 0=inactive, 1=session, 2=permanent
    std::vector<config::SanityCheckIssue> sanityIssues_;
    int                                selectedSanityIdx_    = 0;
    int                                sanityScrollOffset_   = 0;
    std::string                        settingsStatusMsg_;
    int                                settingsStatusColor_  = 0; // 0=none, 1=green, 2=red

    bool                               isScreensaver_       = false;
    std::time_t                        lastActivityTime_    = 0;
    int                                screensaverTick_     = 0;
    static constexpr int               IDLE_TIMEOUT_SEC     = 60;
    bool                               shouldExit_          = false;

    bool wakeIfScreensaver();
    std::vector<std::string> getAvailableCategories() const;
    void cycleCategoryFilter();
    void cycleSortOption();
    void rebuildFilteredItems();
    void triggerCatalogRefresh();
    void triggerCacheAllBinaries();
    void triggerCacheCurrentItem();
    void triggerDeleteCurrentItemBinary();
    void openInstallPopup();
    void closeInstallPopup();
    void confirmInstall();
    void triggerUninstall();
    void triggerRepair();
    void openExitPopup();
    void closeExitPopup();
    void openRefreshPopup();
    void closeRefreshPopup();
    void openPurgePopup();
    void closePurgePopup();
    void openSettingsScreen();
    void closeSettingsScreen();
    void refreshMpcBackups();
    void refreshBootLogs();
    void refreshSanityChecks();
    void openRestoreBackupPopup();
    void closeRestoreBackupPopup();
    void openDeleteBackupPopup();
    void closeDeleteBackupPopup();
    void executeRestoreSelectedBackup();
    void executeDeleteSelectedBackup();
    void drawTopHeader();
    void drawFilterBar();
    void drawCardThumbnail(gfx::Rect r, const config::MenuItem& item, int index, bool large = false);
    void drawListView();
    void drawDetailView();
    void drawInstallPopup();
    void drawExitPopup();
    void drawRefreshPopup();
    void drawPurgePopup();
    void drawRestoreBackupPopup();
    void drawDeleteBackupPopup();
    void drawSettingsScreen();
    void drawPluginManagerSettingsTab();
    void drawMpcSettingsTab();
    void drawMpcRollbackSubTab();
    void drawMpcLimitsSubTab();
    void drawMpcSshSubTab();
    void drawMpcBootLogSubTab();
    void drawMpcSanitySubTab();
    void drawExecutionScreen();
    void drawBottomPaginationBar();
    void drawScreensaver();

public:
    PluginManagerView(gfx::DrmFramebufferCanvas& canvas, config::LauncherConfig& config);
    void render();
    void onWheelDelta(int delta);
    void onWheelPress();
    void onMenuButton();
    void onTouchDown(int lx, int ly);
    void onAnyInput();
    void checkIdleTick();
    bool isScreensaverActive() const { return isScreensaver_; }
    bool shouldExit() const { return shouldExit_; }
};

} // namespace tkgl::ui
