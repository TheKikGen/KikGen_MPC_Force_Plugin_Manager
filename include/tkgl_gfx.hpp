/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: include/tkgl_gfx.hpp
 * Description: High-Definition Anti-Aliased Linux Framebuffer (/dev/fb0) Engine
 *              - Sub-pixel SDF Rounded Panels & Borders (CSS-grade border-radius)
 *              - Smooth Anti-Aliased EPX/Subpixel Monospace Typography
 *              - Zero-dependency PNG (DEFLATE) + P6 PPM Image Decoder & Cache
 *              - Vector UI Icons (Checkmark, Download, Refresh, Chevrons, Link)
 * ============================================================================
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <linux/fb.h>

namespace tkgl::gfx {

constexpr int LOGICAL_WIDTH  = 1280;
constexpr int LOGICAL_HEIGHT = 800;

struct Color {
    uint8_t r, g, b, a;
    constexpr Color() : r(0), g(0), b(0), a(255) {}
    constexpr Color(uint8_t _r, uint8_t _g, uint8_t _b, uint8_t _a = 255)
        : r(_r), g(_g), b(_b), a(_a) {}
};

namespace Palette {
    constexpr Color BgDarkest      = Color(18, 18, 21);    // #121215
    constexpr Color BgHeader       = Color(24, 24, 28);    // #18181c
    constexpr Color BgFilterBar    = Color(28, 28, 33);    // #1c1c21
    constexpr Color BgCard         = Color(29, 29, 35);    // #1d1d23
    constexpr Color BgCardSelected = Color(37, 37, 45);    // #25252d
    constexpr Color BgDescCard     = Color(24, 24, 31);    // #18181f
    constexpr Color BgSpecsCard    = Color(27, 27, 33);    // #1b1b21
    constexpr Color BgPayloadBar   = Color(21, 21, 27);    // #15151b
    constexpr Color BgTagPill      = Color(37, 37, 48);    // #252530
    constexpr Color BgButton       = Color(35, 35, 42);    // #23232a
    constexpr Color AkaiRed        = Color(217, 43, 43);   // #d92b2b
    constexpr Color AkaiCrimson    = Color(225, 29, 72);   // #e11d48
    constexpr Color AkaiAmber      = Color(245, 158, 11);  // #f59e0b
    constexpr Color AmberGold      = Color(251, 191, 36);  // #fbbf24
    constexpr Color StatusGreen    = Color(52, 211, 153);  // #34d399
    constexpr Color StatusGreenBdr = Color(16, 185, 129);  // #10b981
    constexpr Color StatusGreenBg  = Color(6, 46, 34);     // #062e22
    constexpr Color StatusCyan     = Color(45, 212, 191);  // #2dd4bf
    constexpr Color TextPrimary    = Color(244, 244, 246); // #f4f4f6
    constexpr Color TextSecondary  = Color(161, 161, 170); // #a1a1aa
    constexpr Color TextMuted      = Color(113, 113, 122); // #71717a
    constexpr Color BorderLight    = Color(50, 50, 62);    // #32323e
    constexpr Color BorderTag      = Color(61, 61, 78);    // #3d3d4e
    constexpr Color BorderDark     = Color(36, 36, 46);    // #24242e
}

struct Rect {
    int x, y, w, h;
    constexpr bool contains(int px, int py) const {
        return px >= x && px < (x + w) && py >= y && py < (y + h);
    }
};

struct DecodedImage {
    int width  = 0;
    int height = 0;
    std::vector<uint8_t> rgb; // 3 bytes per pixel (R, G, B)
    bool valid = false;
};

class DrmFramebufferCanvas {
private:
    int fb_fd_ = -1;
    uint8_t* fb_ptr_ = nullptr;
    size_t fb_mmap_size_ = 0;

    struct fb_var_screeninfo vinfo_;
    struct fb_fix_screeninfo finfo_;

    int phys_width_  = 800;
    int phys_height_ = 1280;
    int log_width_   = LOGICAL_WIDTH;
    int log_height_  = LOGICAL_HEIGHT;
    int rotation_    = 270;

    std::vector<uint32_t> backbuffer_;
    std::vector<uint8_t>  rgb_shadow_; // Fast RGB readback buffer for smooth alpha blending
    std::map<std::string, DecodedImage> image_cache_;

public:
    DrmFramebufferCanvas();
    ~DrmFramebufferCanvas();

    bool init(const char* fb_device = "/dev/fb0", int rotation_deg = 270);
    bool initHardware(const char* fb_device = "/dev/fb0") { return init(fb_device, rotation_); }
    void shutdown();

    void setRotation(int rotation_deg);
    int  getRotation() const { return rotation_; }
    int  getLogicalWidth() const { return log_width_; }
    int  getLogicalHeight() const { return log_height_; }
    int  getPhysicalWidth() const { return phys_width_; }
    int  getPhysicalHeight() const { return phys_height_; }

    inline uint32_t packColor(uint8_t r, uint8_t g, uint8_t b) const {
        uint32_t rlen = vinfo_.red.length   ? vinfo_.red.length   : 8;
        uint32_t glen = vinfo_.green.length ? vinfo_.green.length : 8;
        uint32_t blen = vinfo_.blue.length  ? vinfo_.blue.length  : 8;
        uint32_t a = (vinfo_.transp.length > 0) ? ((1u << vinfo_.transp.length) - 1u) : 0xFFu;
        uint32_t pr = (static_cast<uint32_t>(r) >> (8 - rlen)) << vinfo_.red.offset;
        uint32_t pg = (static_cast<uint32_t>(g) >> (8 - glen)) << vinfo_.green.offset;
        uint32_t pb = (static_cast<uint32_t>(b) >> (8 - blen)) << vinfo_.blue.offset;
        uint32_t pa = (vinfo_.transp.length > 0) ? (a << vinfo_.transp.offset) : 0u;
        return pr | pg | pb | pa;
    }

    void mapTouchToLogical(int rawX, int rawY, int maxRawX, int maxRawY, int& outLx, int& outLy) const;

    void clear(Color c = Palette::BgDarkest);
    void clearScreenBlack();
    void putPixel(int x, int y, Color c);
    void blendPixel(int x, int y, Color c, uint8_t alpha);
    void drawHLine(int x, int y, int w, Color c);
    void drawVLine(int x, int y, int h, Color c);
    void drawLineAA(float x0, float y0, float x1, float y1, Color c, float thickness = 1.5f);
    void drawRect(Rect r, Color c, int thickness = 1);
    void fillRect(Rect r, Color c);

    // Anti-Aliased SDF Rounded Rectangles & Panels (matches web simulator CSS border-radius)
    void fillRoundedRect(Rect r, int radius, Color c);
    void drawRoundedRect(Rect r, int radius, Color borderColor, float thickness = 1.0f);
    void drawPanel(Rect r, int radius, Color fillColor, Color borderColor, float borderThickness = 1.0f);
    void drawGlowDot(int cx, int cy, int radius, Color c);
    void fillCircleAA(int cx, int cy, float radius, Color c);
    void drawCircleAA(int cx, int cy, float radius, Color borderColor, float thickness = 1.0f);

    // Vector Icons matching Hardware Simulator (image_22ce5693.png)
    void drawCheckmarkIcon(int x, int y, int size, Color c);
    void drawDownloadIcon(int x, int y, int size, Color c);
    void drawRefreshIcon(int x, int y, int size, Color c);
    void drawChevronIcon(int x, int y, int size, bool pointLeft, Color c);
    void drawExternalLinkIcon(int x, int y, int size, Color c);

    // Smooth Anti-Aliased Typography with EPX Diagonal Smoothing & Tall Monospace Proportions
    int  charAdvance(int scale) const;
    int  charHeight(int scale) const;
    int  measureText(const std::string& text, int scale = 2) const;
    void drawCharSmooth(int x, int y, char c, Color color, int scale = 2, bool bold = false);
    void drawText(int x, int y, const std::string& text, Color color, int scale = 2, bool bold = false);
    void drawTextCentered(Rect bounds, const std::string& text, Color color, int scale = 2, bool bold = false);
    void drawTextRight(int rightX, int y, const std::string& text, Color color, int scale = 2, bool bold = false);
    void drawTextRightClipped(int minX, int rightX, int y, const std::string& text, Color color, int scale = 2, bool bold = false);
    void drawTextClipped(Rect clipBounds, const std::string& text, Color color, int scale = 2, bool bold = false);
    void drawTextWrapped(Rect bounds, const std::string& text, Color color, int scale = 2, int lineGap = 6);

    // Zero-dependency PNG + P6 PPM Image Loader, Bilinear Resampler & Rounded Corner Mask
    bool loadImageFile(const std::string& path, DecodedImage& outImg);
    bool drawImageRounded(Rect r, int radius, const std::string& path, const std::string& fallbackUrl = "", const std::string& pluginId = "");
    bool drawImagePPM(Rect r, const std::string& ppmPath) { return drawImageRounded(r, 8, ppmPath); }
    void clearImageCache() { image_cache_.clear(); }

    void blitToFramebuffer();
};

using FramebufferCanvas = DrmFramebufferCanvas;

} // namespace tkgl::gfx
