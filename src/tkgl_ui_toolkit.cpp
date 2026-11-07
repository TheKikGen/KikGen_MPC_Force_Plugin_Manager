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

void PluginManagerView::rebuildFilteredItems() {
    filteredItems_.clear();
    for (int s = 0; s < static_cast<int>(config_.sections.size()); ++s) {
        if (sectionFilter_ >= 0 && sectionFilter_ != s) continue;
        const auto& sec = config_.sections[s];
        for (int i = 0; i < static_cast<int>(sec.items.size()); ++i) {
            const auto& item = sec.items[i];
            if (activeTab_ == FilterTab::INSTALLED && !item.installed) continue;
            if (activeTab_ == FilterTab::UPDATES && !item.updateAvail) continue;
            filteredItems_.push_back({s, i});
        }
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
        : "./Synths";

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

    config::ConfigParser::executeItemInstall(config_, item, targetDir, execLogs_, stepCb);
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

void PluginManagerView::render() {
    if (isScreensaver_) {
        drawScreensaver();
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

    // 2. Main Title + "By The KikGen Labs - V1.1.2" on same baseline (matching image_22ce5693.png)
    int titleX = 76;
    canvas_.drawTextClipped({titleX, 21, 430, 22}, config_.app_title, Palette::TextPrimary, 2, true);
    int titleW = std::min(430, canvas_.measureText(config_.app_title, 2) + 14);
    canvas_.drawText(titleX + titleW, 26, "By The KikGen Labs - V1.1.2", Palette::TextSecondary, 1, false);

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

    // 4. Glowing ONLINE / OFFLINE Dot + Status Label (verified via ping_host)
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

    std::string secLabel = (sectionFilter_ < 0)
        ? "SECTION: ALL"
        : ("SECTION: " + config_.sections[sectionFilter_].title);

    Rect catRect = {W - 430, 68, 210, 34};
    canvas_.drawPanel(catRect, 7, Color(37, 37, 44), Color(58, 58, 68), 1.0f);
    canvas_.drawTextCentered(catRect, secLabel, Palette::TextPrimary, 1, true);

    Rect sortRect = {W - 210, 68, 188, 34};
    canvas_.drawPanel(sortRect, 7, Color(37, 37, 44), Color(58, 58, 68), 1.0f);
    canvas_.drawTextCentered(sortRect, "Sort: UPDATED", Palette::TextSecondary, 1, true);

    // Row 2: SECTION CLASSIFICATION TAB BAR matching image_6572f34f.png (y = 108..148)
    // Tabs: ALL | INSTRUMENTS | MIDI | EFFECTS | ADDINS | DEVICE PATCHES | COMPANION APPS
    canvas_.fillRect({0, 108, W, 40}, Color(20, 20, 24));
    canvas_.drawHLine(0, 147, W, Palette::BorderLight);

    int totalAll = 0;
    for (const auto& s : config_.sections) {
        totalAll += static_cast<int>(s.items.size());
    }

    int tabX = 22;
    int numSec = static_cast<int>(config_.sections.size());
    for (int idx = -1; idx < numSec; ++idx) {
        bool isSelected = (sectionFilter_ == idx);
        std::string title = (idx < 0) ? "ALL" : config_.sections[idx].title;
        int cnt = (idx < 0) ? totalAll : static_cast<int>(config_.sections[idx].items.size());
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

        if (item.broken) {
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
        if (item.broken) {
            canvas_.drawPanel(btnR, 8, Color(64, 34, 12), Palette::AmberGold, 1.5f);
            canvas_.drawRefreshIcon(btnR.x + 20, btnR.y + 13, 15, Palette::AmberGold);
            canvas_.drawText(btnR.x + 44, btnR.y + 12, "REPAIR", Palette::AmberGold, 2, true);
        } else if (item.installed) {
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
    int maxTitleW = (item.installed || item.broken) ? (W - 530) : (W - 360);
    canvas_.drawTextClipped({146, 76, maxTitleW, 26}, item.label, Palette::TextPrimary, 3, true);
    std::string subTitle = "by " + item.author + "  .  " + item.category;
    canvas_.drawTextClipped({146, 107, maxTitleW, 16}, subTitle, Palette::TextSecondary, 2, false);

    // Top-Right Action Buttons:
    // - If broken: [ ! BROKEN ] badge + Amber [ REPAIR ] button
    // - If installed (and not broken): [ UNINSTALL ] button close to [ REINSTALL ] button
    // - If not installed: Red [ INSTALL ] button
    if (item.broken) {
        Rect warnBadge = {W - 418, 84, 222, 34};
        canvas_.drawPanel(warnBadge, 7, Color(64, 20, 20), Palette::AkaiCrimson, 1.2f);
        canvas_.drawTextCentered(warnBadge, "BROKEN (.SO MISSING)", Palette::AmberGold, 1, true);

        Rect repBtn = {W - 186, 79, 162, 44};
        canvas_.drawPanel(repBtn, 8, Color(180, 83, 9), Palette::AmberGold, 1.5f);
        canvas_.drawRefreshIcon(repBtn.x + 22, repBtn.y + 13, 17, Palette::TextPrimary);
        canvas_.drawText(repBtn.x + 48, repBtn.y + 13, "REPAIR", Palette::TextPrimary, 2, true);
    } else if (item.installed) {
        Rect uninstBtn = {W - 356, 79, 160, 44};
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
        "/Synth/" + item.targetSubdir
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
    // 1. Verification & Category Badges + GitHub Source Link
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

    // Top-right GitHub Source link + icon
    canvas_.drawTextRight(rightX + rightW - 20, 162, "GitHub Source", Palette::TextSecondary, 1, false);
    canvas_.drawExternalLinkIcon(rightX + rightW - 15, 161, 13, Palette::TextSecondary);

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

    std::string releaseLine = "Release " + item.version + " (" + item.updatedDate + ")  .  Binary: " +
                              item.targetBinary + "  .  Skin folder: /Synth/" + item.targetSubdir;
    canvas_.drawTextWrapped(
        {descCard.x + 18, relY + 34, descCard.w - 36, 84},
        releaseLine,
        Color(212, 212, 218),
        2,
        6
    );

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
    canvas_.fillRoundedRect(tagR, 6, item.broken ? Color(180, 83, 9) : Palette::AkaiRed);
    canvas_.drawTextCentered(tagR, item.broken ? "REPAIR POPUP" : "INSTALL POPUP", Palette::TextPrimary, 1, true);

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
        canvas_.drawText(rowR.x + 44, rowR.y + 10, loc.path, Palette::TextPrimary, 2, true);
        if (loc.isManagerDefault) {
            int pw = canvas_.measureText(loc.path, 2);
            Rect defPill = {rowR.x + 44 + pw + 10, rowR.y + 8, 124, 20};
            canvas_.drawPanel(defPill, 4, Palette::StatusGreenBg, Palette::StatusGreenBdr, 1.0f);
            canvas_.drawTextCentered(defPill, "Manager Default", Palette::StatusGreen, 1, true);
        }

        canvas_.drawTextClipped({rowR.x + 44, rowR.y + 32, rowR.w - 200, 16}, loc.label, Palette::TextSecondary, 1, false);

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
    if (item.broken) {
        canvas_.drawText(infoBox.x + 14, infoBox.y + 76, "3. Repair: Cleans all bad entries in MPC.settings & adds a new clean <PLUGIN> entry", Palette::AmberGold, 1, false);
    } else {
        canvas_.drawText(infoBox.x + 14, infoBox.y + 76, "3. Registration: Injects <pluginList-arm><KNOWNPLUGINS> with %payload-path% replaced", Palette::TextSecondary, 1, false);
    }

    // Bottom Action Buttons: RETURN on Left, INSTALL / REPAIR on Right
    canvas_.drawHLine(mx + 20, my + 566, mw - 40, Palette::BorderDark);

    Rect retBtn = {mx + 20, my + 578, 180, 48};
    canvas_.drawPanel(retBtn, 10, Palette::BgButton, Color(62, 62, 78), 1.0f);
    canvas_.drawChevronIcon(retBtn.x + 16, retBtn.y + 15, 17, true, Palette::TextSecondary);
    canvas_.drawText(retBtn.x + 42, retBtn.y + 15, "RETURN", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(retBtn.x + retBtn.w - 14, retBtn.y + 18, "[-] MINUS", Palette::TextSecondary, 1, false);

    Rect instBtn = {mx + mw - 20 - 220, my + 578, 220, 48};
    canvas_.fillRoundedRect(instBtn, 10, item.broken ? Color(180, 83, 9) : Palette::AkaiRed);
    canvas_.drawDownloadIcon(instBtn.x + 18, instBtn.y + 15, 18, Palette::TextPrimary);
    canvas_.drawText(instBtn.x + 46, instBtn.y + 15, item.broken ? "REPAIR" : "INSTALL", Palette::TextPrimary, 2, true);
    canvas_.drawTextRight(instBtn.x + instBtn.w - 14, instBtn.y + 18, "[+] PLUS", Palette::TextPrimary, 1, false);
}

void PluginManagerView::drawExecutionScreen() {
    int W = canvas_.getLogicalWidth();
    int H = canvas_.getLogicalHeight();
    Rect termBox = {24, 88, W - 48, H - 176};
    canvas_.drawPanel(termBox, 12, Color(10, 12, 16), Palette::BorderLight, 1.5f);

    int ly = termBox.y + 20;
    for (size_t i = 0; i < execLogs_.size() && ly < termBox.y + termBox.h - 28; ++i) {
        canvas_.drawTextClipped({termBox.x + 20, ly, termBox.w - 40, 20}, execLogs_[i], Palette::StatusGreen, 2, false);
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

void PluginManagerView::onWheelDelta(int delta) {
    if (wakeIfScreensaver()) return;
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
    if (uiState_ == UIState::EXIT_POPUP) {
        shouldExit_ = true;
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
        openInstallPopup();
    } else if (uiState_ == UIState::EXEC_SCREEN && execFinished_) {
        rebuildFilteredItems();
        uiState_ = UIState::MENU;
        render();
    }
}

void PluginManagerView::onMenuButton() {
    if (wakeIfScreensaver()) return;
    // Hardware Minus (-) button (Note 0x37 / 90 37 7F) or MENU button (Note 0x7B):
    // Identical behaviour to the "< BACK" touch button or Return button.
    // When already at the root MENU view, pressing [-] opens the EXIT confirmation popup.
    if (uiState_ == UIState::EXIT_POPUP) {
        closeExitPopup();
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

    if (uiState_ == UIState::EXIT_POPUP) {
        int mw = 560;
        int mh = 270;
        int mx = (W - mw) / 2;
        int my = (H - mh) / 2;

        // CANCEL button: mx + 24, my + 196, w = 236, h = 50
        if (lx >= mx + 20 && lx <= mx + 265 && ly >= my + 190 && ly <= my + 255) {
            closeExitPopup();
            return;
        }
        // QUIT button: mx + mw - 260, my + 196, w = 236, h = 50
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

        // Check pick list entries
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

        // RETURN button: mx + 20, my + 578, w = 180, h = 48
        if (lx >= mx + 20 && lx <= mx + 200 && ly >= my + 570 && ly <= my + 635) {
            closeInstallPopup();
            return;
        }

        // INSTALL button: mx + mw - 240, my + 578, w = 220, h = 48
        if (lx >= mx + mw - 240 && lx <= mx + mw - 20 && ly >= my + 570 && ly <= my + 635) {
            confirmInstall();
            return;
        }
        return;
    }

    // Top Header REFRESH Button
    if (ly >= 8 && ly <= 55 && lx >= W - 448 && lx <= W - 310) {
        triggerCatalogRefresh();
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
            // UNINSTALL button (close to REINSTALL when installed and not broken)
            if (item.installed && !item.broken && lx >= W - 360 && lx <= W - 192 && ly >= 72 && ly <= 132) {
                triggerUninstall();
                return;
            }
        }
        // INSTALL / REINSTALL / REPAIR button
        if (lx >= W - 190 && ly >= 72 && ly <= 132) {
            openInstallPopup();
            return;
        }
        return;
    }

    if (uiState_ == UIState::MENU) {
        // Row 1: DISCOVER | INSTALLED | UPDATES + Cycle Section Button (y = 62..107)
        if (ly >= 62 && ly < 108) {
            if (lx >= 22 && lx < 166) { activeTab_ = FilterTab::DISCOVER; rebuildFilteredItems(); render(); return; }
            if (lx >= 166 && lx < 310) { activeTab_ = FilterTab::INSTALLED; rebuildFilteredItems(); render(); return; }
            if (lx >= 310 && lx < 454) { activeTab_ = FilterTab::UPDATES; rebuildFilteredItems(); render(); return; }
            if (lx >= W - 435 && lx <= W - 215) {
                sectionFilter_++;
                if (sectionFilter_ >= static_cast<int>(config_.sections.size())) sectionFilter_ = -1;
                rebuildFilteredItems();
                render();
                return;
            }
            return;
        }

        // Row 2: SECTION CLASSIFICATION TAB BAR (y = 108..148)
        if (ly >= 108 && ly <= 148) {
            int totalAll = 0;
            for (const auto& s : config_.sections) totalAll += static_cast<int>(s.items.size());
            int tabX = 22;
            int numSec = static_cast<int>(config_.sections.size());
            for (int idx = -1; idx < numSec; ++idx) {
                std::string title = (idx < 0) ? "ALL" : config_.sections[idx].title;
                int cnt = (idx < 0) ? totalAll : static_cast<int>(config_.sections[idx].items.size());
                std::string cntStr = std::to_string(cnt);
                int textW = canvas_.measureText(title, 1);
                int pillW = std::max(24, canvas_.measureText(cntStr, 1) + 12);
                int tabW = textW + pillW + 26;
                if (lx >= tabX && lx <= tabX + tabW) {
                    sectionFilter_ = idx;
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
