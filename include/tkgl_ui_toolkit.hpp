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
    EXIT_POPUP    = 4  // Confirmation Popup to quit the Plugin Manager
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

    bool                               isScreensaver_       = false;
    std::time_t                        lastActivityTime_    = 0;
    int                                screensaverTick_     = 0;
    static constexpr int               IDLE_TIMEOUT_SEC     = 60;
    bool                               shouldExit_          = false;

    bool wakeIfScreensaver();
    void rebuildFilteredItems();
    void triggerCatalogRefresh();
    void openInstallPopup();
    void closeInstallPopup();
    void confirmInstall();
    void triggerUninstall();
    void openExitPopup();
    void closeExitPopup();
    void drawTopHeader();
    void drawFilterBar();
    void drawCardThumbnail(gfx::Rect r, const config::MenuItem& item, int index, bool large = false);
    void drawListView();
    void drawDetailView();
    void drawInstallPopup();
    void drawExitPopup();
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
