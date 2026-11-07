/**
 * ============================================================================
 * THE KIKGEN LABS - SINGLE-FILE AMALGAMATED C++17 BUILD
 * File: mpc_plugin_manager_fb.cpp
 * Compile with:
 *   arm-linux-gnueabihf-g++ -std=c++17 -O2 -Wall -Wextra -Wno-format-truncation \
 *       -pthread -o mpc_plugin_manager_fb mpc_plugin_manager_fb.cpp -lasound -lpthread -ldl
 * ============================================================================
 */

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

/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: src/tkgl_gfx.cpp
 * Description: High-Definition Anti-Aliased Framebuffer Renderer
 *              - SDF Anti-Aliased Rounded Corners & Borders
 *              - EPX/Scale2x Anti-Aliased Smooth Monospace Font Engine
 *              - Pure C++17 PNG (RFC-1951 DEFLATE) + P6 PPM Image Decoder
 *              - Bilinear Image Scaling with Rounded Corner Clipping
 * ============================================================================
 */


#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <algorithm>

namespace tkgl::gfx {

// Standard C++17 contiguous 8x8 bitmap font for ASCII 32 (' ') to 126 ('~')
static const uint8_t font_8x8_basic[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 33 '!'
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // 34 '"'
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // 35 '#'
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // 36 '$'
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // 37 '%'
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // 38 '&'
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // 39 '\''
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // 40 '('
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // 41 ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 42 '*'
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // 43 '+'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // 44 ','
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // 45 '-'
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // 46 '.'
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // 47 '/'
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 48 '0'
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 49 '1'
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 50 '2'
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 51 '3'
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 52 '4'
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 53 '5'
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 54 '6'
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 55 '7'
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 56 '8'
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 57 '9'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // 58 ':'
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // 59 ';'
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // 60 '<'
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // 61 '='
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 62 '>'
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // 63 '?'
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // 64 '@'
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 65 'A'
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 66 'B'
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 67 'C'
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 68 'D'
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 69 'E'
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 70 'F'
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 71 'G'
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 72 'H'
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 73 'I'
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 74 'J'
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 75 'K'
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 76 'L'
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 77 'M'
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 78 'N'
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 79 'O'
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 80 'P'
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 81 'Q'
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 82 'R'
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 83 'S'
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 84 'T'
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 85 'U'
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 86 'V'
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 87 'W'
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 88 'X'
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 89 'Y'
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 90 'Z'
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // 91 '['
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // 92 '\\'
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // 93 ']'
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // 94 '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 95 '_'
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // 96 backtick
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // 97 'a'
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // 98 'b'
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // 99 'c'
    {0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00}, // 100 'd'
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00}, // 101 'e'
    {0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00}, // 102 'f'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // 103 'g'
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // 104 'h'
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // 105 'i'
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // 106 'j'
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // 107 'k'
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 108 'l'
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // 109 'm'
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // 110 'n'
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // 111 'o'
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // 112 'p'
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // 113 'q'
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // 114 'r'
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // 115 's'
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // 116 't'
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // 117 'u'
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 118 'v'
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // 119 'w'
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // 120 'x'
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // 121 'y'
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, // 122 'z'
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // 123 '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // 124 '|'
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // 125 '}'
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}  // 126 '~'
};

DrmFramebufferCanvas::DrmFramebufferCanvas() {
    std::memset(&vinfo_, 0, sizeof(vinfo_));
    std::memset(&finfo_, 0, sizeof(finfo_));
    vinfo_.bits_per_pixel = 32;
    vinfo_.red.offset = 16;   vinfo_.red.length = 8;
    vinfo_.green.offset = 8;  vinfo_.green.length = 8;
    vinfo_.blue.offset = 0;   vinfo_.blue.length = 8;
    vinfo_.transp.offset = 24; vinfo_.transp.length = 8;
    backbuffer_.resize(log_width_ * log_height_, 0);
    rgb_shadow_.resize(log_width_ * log_height_ * 3, 0);
}

DrmFramebufferCanvas::~DrmFramebufferCanvas() {
    shutdown();
}

void DrmFramebufferCanvas::setRotation(int rotation_deg) {
    if (rotation_deg != 0 && rotation_deg != 90 && rotation_deg != 180 && rotation_deg != 270) {
        rotation_deg = 270;
    }
    rotation_ = rotation_deg;

    if (rotation_ == 90 || rotation_ == 270) {
        log_width_  = phys_height_;
        log_height_ = phys_width_;
    } else {
        log_width_  = phys_width_;
        log_height_ = phys_height_;
    }
    backbuffer_.assign(log_width_ * log_height_, 0);
    rgb_shadow_.assign(log_width_ * log_height_ * 3, 0);
}

bool DrmFramebufferCanvas::init(const char* fb_device, int rotation_deg) {
    const char* dev = (fb_device && fb_device[0] && std::strcmp(fb_device, "undefined") != 0)
        ? fb_device
        : "/dev/fb0";

    fb_fd_ = open(dev, O_RDWR);
    if (fb_fd_ < 0) {
        std::perror("[TKGL-GFX] Error opening framebuffer device");
        phys_width_  = 800;
        phys_height_ = 1280;
        setRotation(rotation_deg);
        return false;
    }

    if (ioctl(fb_fd_, FBIOGET_FSCREENINFO, &finfo_) == -1 ||
        ioctl(fb_fd_, FBIOGET_VSCREENINFO, &vinfo_) == -1) {
        std::perror("[TKGL-GFX] Error reading fb_fix/var_screeninfo");
        close(fb_fd_);
        fb_fd_ = -1;
        return false;
    }

    phys_width_  = static_cast<int>(vinfo_.xres);
    phys_height_ = static_cast<int>(vinfo_.yres);
    setRotation(rotation_deg);

    fb_mmap_size_ = (finfo_.smem_len > 0)
        ? static_cast<size_t>(finfo_.smem_len)
        : static_cast<size_t>(finfo_.line_length) * vinfo_.yres;

    fb_ptr_ = static_cast<uint8_t*>(
        mmap(nullptr, fb_mmap_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd_, 0)
    );

    if (fb_ptr_ == MAP_FAILED) {
        std::perror("[TKGL-GFX] Error mmap /dev/fb0");
        fb_ptr_ = nullptr;
        close(fb_fd_);
        fb_fd_ = -1;
        return false;
    }

    std::printf("[TKGL-GFX] Opened %s: phys=%dx%d (%dbpp, line_length=%u, smem=%zu) -> logical=%dx%d (rot=%d)\n",
                dev, phys_width_, phys_height_, vinfo_.bits_per_pixel,
                finfo_.line_length, fb_mmap_size_, log_width_, log_height_, rotation_);
    return true;
}

void DrmFramebufferCanvas::shutdown() {
    if (fb_ptr_ && fb_ptr_ != MAP_FAILED) {
        munmap(fb_ptr_, fb_mmap_size_);
        fb_ptr_ = nullptr;
    }
    if (fb_fd_ >= 0) {
        close(fb_fd_);
        fb_fd_ = -1;
    }
}

void DrmFramebufferCanvas::mapTouchToLogical(
    int rawX, int rawY, int maxRawX, int maxRawY, int& outLx, int& outLy
) const {
    int px = (maxRawX > 0) ? (rawX * phys_width_) / maxRawX : rawX;
    int py = (maxRawY > 0) ? (rawY * phys_height_) / maxRawY : rawY;

    if (rotation_ == 90) {
        outLx = py;
        outLy = phys_width_ - 1 - px;
    } else if (rotation_ == 180) {
        outLx = phys_width_ - 1 - px;
        outLy = phys_height_ - 1 - py;
    } else if (rotation_ == 270) {
        outLx = phys_height_ - 1 - py;
        outLy = px;
    } else {
        outLx = px;
        outLy = py;
    }
}

void DrmFramebufferCanvas::clear(Color c) {
    uint32_t packed = packColor(c.r, c.g, c.b);
    std::fill(backbuffer_.begin(), backbuffer_.end(), packed);
    size_t total = static_cast<size_t>(log_width_) * log_height_;
    uint8_t* ptr = rgb_shadow_.data();
    for (size_t i = 0; i < total; ++i) {
        ptr[0] = c.r;
        ptr[1] = c.g;
        ptr[2] = c.b;
        ptr += 3;
    }
}

void DrmFramebufferCanvas::clearScreenBlack() {
    clear(Color(0, 0, 0));
    blitToFramebuffer();
}

void DrmFramebufferCanvas::putPixel(int x, int y, Color c) {
    if (x < 0 || x >= log_width_ || y < 0 || y >= log_height_) return;
    if (c.a < 255) {
        blendPixel(x, y, c, c.a);
        return;
    }
    size_t idx = static_cast<size_t>(y) * log_width_ + x;
    backbuffer_[idx] = packColor(c.r, c.g, c.b);
    size_t sidx = idx * 3;
    rgb_shadow_[sidx]     = c.r;
    rgb_shadow_[sidx + 1] = c.g;
    rgb_shadow_[sidx + 2] = c.b;
}

void DrmFramebufferCanvas::blendPixel(int x, int y, Color c, uint8_t alpha) {
    if (x < 0 || x >= log_width_ || y < 0 || y >= log_height_ || alpha == 0) return;
    if (alpha == 255) {
        size_t idx = static_cast<size_t>(y) * log_width_ + x;
        backbuffer_[idx] = packColor(c.r, c.g, c.b);
        size_t sidx = idx * 3;
        rgb_shadow_[sidx]     = c.r;
        rgb_shadow_[sidx + 1] = c.g;
        rgb_shadow_[sidx + 2] = c.b;
        return;
    }
    size_t idx = static_cast<size_t>(y) * log_width_ + x;
    size_t sidx = idx * 3;
    uint32_t a = alpha;
    uint32_t invA = 255 - a;
    uint8_t nr = static_cast<uint8_t>((c.r * a + rgb_shadow_[sidx]     * invA) / 255);
    uint8_t ng = static_cast<uint8_t>((c.g * a + rgb_shadow_[sidx + 1] * invA) / 255);
    uint8_t nb = static_cast<uint8_t>((c.b * a + rgb_shadow_[sidx + 2] * invA) / 255);
    rgb_shadow_[sidx]     = nr;
    rgb_shadow_[sidx + 1] = ng;
    rgb_shadow_[sidx + 2] = nb;
    backbuffer_[idx] = packColor(nr, ng, nb);
}

void DrmFramebufferCanvas::drawHLine(int x, int y, int w, Color c) {
    if (y < 0 || y >= log_height_ || w <= 0) return;
    int x0 = std::max(0, x);
    int x1 = std::min(log_width_ - 1, x + w - 1);
    for (int px = x0; px <= x1; ++px) putPixel(px, y, c);
}

void DrmFramebufferCanvas::drawVLine(int x, int y, int h, Color c) {
    if (x < 0 || x >= log_width_ || h <= 0) return;
    int y0 = std::max(0, y);
    int y1 = std::min(log_height_ - 1, y + h - 1);
    for (int py = y0; py <= y1; ++py) putPixel(x, py, c);
}

void DrmFramebufferCanvas::drawLineAA(float x0, float y0, float x1, float y1, Color c, float thickness) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f) {
        putPixel(static_cast<int>(x0), static_cast<int>(y0), c);
        return;
    }
    int minX = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - thickness - 1)));
    int maxX = std::min(log_width_ - 1, static_cast<int>(std::ceil(std::max(x0, x1) + thickness + 1)));
    int minY = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - thickness - 1)));
    int maxY = std::min(log_height_ - 1, static_cast<int>(std::ceil(std::max(y0, y1) + thickness + 1)));

    float halfT = thickness * 0.5f;
    for (int py = minY; py <= maxY; ++py) {
        for (int px = minX; px <= maxX; ++px) {
            float vx = px - x0;
            float vy = py - y0;
            float t = std::max(0.0f, std::min(1.0f, (vx * dx + vy * dy) / (len * len)));
            float projX = x0 + t * dx;
            float projY = y0 + t * dy;
            float dist = std::sqrt((px - projX) * (px - projX) + (py - projY) * (py - projY));
            float cov = std::max(0.0f, std::min(1.0f, halfT + 0.5f - dist));
            if (cov > 0.0f) {
                blendPixel(px, py, c, static_cast<uint8_t>(cov * c.a));
            }
        }
    }
}

void DrmFramebufferCanvas::fillRect(Rect r, Color c) {
    int x0 = std::max(0, r.x);
    int y0 = std::max(0, r.y);
    int x1 = std::min(log_width_, r.x + r.w);
    int y1 = std::min(log_height_, r.y + r.h);
    if (x0 >= x1 || y0 >= y1) return;

    if (c.a < 255) {
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) blendPixel(px, py, c, c.a);
        }
        return;
    }

    uint32_t packed = packColor(c.r, c.g, c.b);
    for (int py = y0; py < y1; ++py) {
        uint32_t* row = &backbuffer_[py * log_width_];
        uint8_t* srow = &rgb_shadow_[(py * log_width_ + x0) * 3];
        for (int px = x0; px < x1; ++px) {
            row[px] = packed;
            srow[0] = c.r;
            srow[1] = c.g;
            srow[2] = c.b;
            srow += 3;
        }
    }
}

void DrmFramebufferCanvas::drawRect(Rect r, Color c, int thickness) {
    for (int i = 0; i < thickness; ++i) {
        drawHLine(r.x + i, r.y + i, r.w - 2 * i, c);
        drawHLine(r.x + i, r.y + r.h - 1 - i, r.w - 2 * i, c);
        drawVLine(r.x + i, r.y + i, r.h - 2 * i, c);
        drawVLine(r.x + r.w - 1 - i, r.y + i, r.h - 2 * i, c);
    }
}

// Signed-Distance-Field (SDF) Anti-Aliased Rounded Rectangle Fill
void DrmFramebufferCanvas::fillRoundedRect(Rect r, int radius, Color c) {
    if (r.w <= 0 || r.h <= 0) return;
    int rad = std::min({radius, r.w / 2, r.h / 2});
    if (rad <= 1) {
        fillRect(r, c);
        return;
    }

    // Fast interior rectangles
    fillRect({r.x + rad, r.y, r.w - 2 * rad, r.h}, c);
    fillRect({r.x, r.y + rad, rad, r.h - 2 * rad}, c);
    fillRect({r.x + r.w - rad, r.y + rad, rad, r.h - 2 * rad}, c);

    // Anti-aliased SDF corners
    float radF = static_cast<float>(rad);
    for (int cy = 0; cy < rad; ++cy) {
        for (int cx = 0; cx < rad; ++cx) {
            float dx = (radF - 0.5f) - cx;
            float dy = (radF - 0.5f) - cy;
            float dist = std::sqrt(dx * dx + dy * dy);
            float cov = std::max(0.0f, std::min(1.0f, radF + 0.5f - dist));
            if (cov > 0.0f) {
                uint8_t a = static_cast<uint8_t>(cov * c.a);
                blendPixel(r.x + cx, r.y + cy, c, a);
                blendPixel(r.x + r.w - 1 - cx, r.y + cy, c, a);
                blendPixel(r.x + cx, r.y + r.h - 1 - cy, c, a);
                blendPixel(r.x + r.w - 1 - cx, r.y + r.h - 1 - cy, c, a);
            }
        }
    }
}

// Signed-Distance-Field (SDF) Anti-Aliased Rounded Border
void DrmFramebufferCanvas::drawRoundedRect(Rect r, int radius, Color borderColor, float thickness) {
    if (r.w <= 0 || r.h <= 0) return;
    int rad = std::min({radius, r.w / 2, r.h / 2});
    if (rad <= 1) {
        drawRect(r, borderColor, std::max(1, static_cast<int>(thickness)));
        return;
    }

    int tInt = std::max(1, static_cast<int>(std::round(thickness)));
    for (int i = 0; i < tInt; ++i) {
        drawHLine(r.x + rad, r.y + i, r.w - 2 * rad, borderColor);
        drawHLine(r.x + rad, r.y + r.h - 1 - i, r.w - 2 * rad, borderColor);
        drawVLine(r.x + i, r.y + rad, r.h - 2 * rad, borderColor);
        drawVLine(r.x + r.w - 1 - i, r.y + rad, r.h - 2 * rad, borderColor);
    }

    float outerR = static_cast<float>(rad);
    float innerR = std::max(0.0f, outerR - thickness);
    for (int cy = 0; cy < rad; ++cy) {
        for (int cx = 0; cx < rad; ++cx) {
            float dx = (outerR - 0.5f) - cx;
            float dy = (outerR - 0.5f) - cy;
            float dist = std::sqrt(dx * dx + dy * dy);
            float covOuter = std::max(0.0f, std::min(1.0f, outerR + 0.5f - dist));
            float covInner = std::max(0.0f, std::min(1.0f, dist - (innerR - 0.5f)));
            float cov = covOuter * covInner;
            if (cov > 0.0f) {
                uint8_t a = static_cast<uint8_t>(cov * borderColor.a);
                blendPixel(r.x + cx, r.y + cy, borderColor, a);
                blendPixel(r.x + r.w - 1 - cx, r.y + cy, borderColor, a);
                blendPixel(r.x + cx, r.y + r.h - 1 - cy, borderColor, a);
                blendPixel(r.x + r.w - 1 - cx, r.y + r.h - 1 - cy, borderColor, a);
            }
        }
    }
}

void DrmFramebufferCanvas::drawPanel(Rect r, int radius, Color fillColor, Color borderColor, float borderThickness) {
    fillRoundedRect(r, radius, fillColor);
    drawRoundedRect(r, radius, borderColor, borderThickness);
}

void DrmFramebufferCanvas::drawGlowDot(int cx, int cy, int radius, Color c) {
    int glowR = radius + 5;
    for (int dy = -glowR; dy <= glowR; ++dy) {
        for (int dx = -glowR; dx <= glowR; ++dx) {
            float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            if (dist <= radius) {
                float edge = std::max(0.0f, std::min(1.0f, radius + 0.5f - dist));
                blendPixel(cx + dx, cy + dy, c, static_cast<uint8_t>(edge * 255));
            } else if (dist <= glowR) {
                float t = 1.0f - (dist - radius) / static_cast<float>(glowR - radius);
                blendPixel(cx + dx, cy + dy, c, static_cast<uint8_t>(t * t * 95));
            }
        }
    }
}

void DrmFramebufferCanvas::fillCircleAA(int cx, int cy, float radius, Color c) {
    int rInt = static_cast<int>(std::ceil(radius + 1.0f));
    for (int dy = -rInt; dy <= rInt; ++dy) {
        for (int dx = -rInt; dx <= rInt; ++dx) {
            float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            float cov = std::max(0.0f, std::min(1.0f, radius + 0.5f - dist));
            if (cov > 0.0f) {
                blendPixel(cx + dx, cy + dy, c, static_cast<uint8_t>(cov * c.a));
            }
        }
    }
}

void DrmFramebufferCanvas::drawCircleAA(int cx, int cy, float radius, Color borderColor, float thickness) {
    int rInt = static_cast<int>(std::ceil(radius + thickness + 1.0f));
    float outerR = radius;
    float innerR = std::max(0.0f, radius - thickness);
    for (int dy = -rInt; dy <= rInt; ++dy) {
        for (int dx = -rInt; dx <= rInt; ++dx) {
            float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
            float covOuter = std::max(0.0f, std::min(1.0f, outerR + 0.5f - dist));
            float covInner = std::max(0.0f, std::min(1.0f, dist - (innerR - 0.5f)));
            float cov = covOuter * covInner;
            if (cov > 0.0f) {
                blendPixel(cx + dx, cy + dy, borderColor, static_cast<uint8_t>(cov * borderColor.a));
            }
        }
    }
}

// Vector Checkmark Icon (✓)
void DrmFramebufferCanvas::drawCheckmarkIcon(int x, int y, int size, Color c) {
    float s = static_cast<float>(size);
    drawLineAA(x + s * 0.15f, y + s * 0.52f, x + s * 0.42f, y + s * 0.80f, c, 2.0f);
    drawLineAA(x + s * 0.42f, y + s * 0.80f, x + s * 0.88f, y + s * 0.22f, c, 2.0f);
}

// Vector Download Icon (↓ into tray)
void DrmFramebufferCanvas::drawDownloadIcon(int x, int y, int size, Color c) {
    float s = static_cast<float>(size);
    float cx = x + s * 0.5f;
    // Vertical shaft
    drawLineAA(cx, y + s * 0.12f, cx, y + s * 0.65f, c, 1.8f);
    // Arrow head
    drawLineAA(cx - s * 0.25f, y + s * 0.42f, cx, y + s * 0.68f, c, 1.8f);
    drawLineAA(cx + s * 0.25f, y + s * 0.42f, cx, y + s * 0.68f, c, 1.8f);
    // Bottom tray
    drawLineAA(x + s * 0.15f, y + s * 0.68f, x + s * 0.15f, y + s * 0.88f, c, 1.6f);
    drawLineAA(x + s * 0.15f, y + s * 0.88f, x + s * 0.85f, y + s * 0.88f, c, 1.6f);
    drawLineAA(x + s * 0.85f, y + s * 0.68f, x + s * 0.85f, y + s * 0.88f, c, 1.6f);
}

// Vector Refresh / Sync Icon (↻)
void DrmFramebufferCanvas::drawRefreshIcon(int x, int y, int size, Color c) {
    float cx = x + size * 0.5f;
    float cy = y + size * 0.5f;
    float r  = size * 0.36f;
    for (int deg = 30; deg < 330; deg += 8) {
        if (deg > 165 && deg < 205) continue;
        float a0 = deg * 3.14159265f / 180.0f;
        float a1 = (deg + 8) * 3.14159265f / 180.0f;
        drawLineAA(cx + std::cos(a0) * r, cy + std::sin(a0) * r,
                   cx + std::cos(a1) * r, cy + std::sin(a1) * r, c, 1.6f);
    }
    // Arrow tip at top-right
    drawLineAA(cx + r * 0.5f, cy - r * 0.95f, cx + r * 0.98f, cy - r * 0.45f, c, 1.6f);
    drawLineAA(cx + r * 0.35f, cy - r * 0.45f, cx + r * 0.98f, cy - r * 0.45f, c, 1.6f);
}

// Vector Chevron (< or >)
void DrmFramebufferCanvas::drawChevronIcon(int x, int y, int size, bool pointLeft, Color c) {
    float s = static_cast<float>(size);
    if (pointLeft) {
        drawLineAA(x + s * 0.68f, y + s * 0.18f, x + s * 0.30f, y + s * 0.50f, c, 1.9f);
        drawLineAA(x + s * 0.30f, y + s * 0.50f, x + s * 0.68f, y + s * 0.82f, c, 1.9f);
    } else {
        drawLineAA(x + s * 0.32f, y + s * 0.18f, x + s * 0.70f, y + s * 0.50f, c, 1.9f);
        drawLineAA(x + s * 0.70f, y + s * 0.50f, x + s * 0.32f, y + s * 0.82f, c, 1.9f);
    }
}

// Vector External Link Icon (↗)
void DrmFramebufferCanvas::drawExternalLinkIcon(int x, int y, int size, Color c) {
    float s = static_cast<float>(size);
    drawLineAA(x + s * 0.15f, y + s * 0.25f, x + s * 0.55f, y + s * 0.25f, c, 1.3f);
    drawLineAA(x + s * 0.15f, y + s * 0.25f, x + s * 0.15f, y + s * 0.85f, c, 1.3f);
    drawLineAA(x + s * 0.15f, y + s * 0.85f, x + s * 0.75f, y + s * 0.85f, c, 1.3f);
    drawLineAA(x + s * 0.75f, y + s * 0.45f, x + s * 0.75f, y + s * 0.85f, c, 1.3f);
    // Diagonal arrow
    drawLineAA(x + s * 0.42f, y + s * 0.58f, x + s * 0.88f, y + s * 0.12f, c, 1.5f);
    drawLineAA(x + s * 0.58f, y + s * 0.12f, x + s * 0.88f, y + s * 0.12f, c, 1.5f);
    drawLineAA(x + s * 0.88f, y + s * 0.12f, x + s * 0.88f, y + s * 0.42f, c, 1.5f);
}

// ============================================================================
// Smooth Anti-Aliased EPX / Subpixel Monospace Font Engine
// ============================================================================
int DrmFramebufferCanvas::charAdvance(int scale) const {
    if (scale <= 1) return 8;
    if (scale == 2) return 10;
    return 14;
}

int DrmFramebufferCanvas::charHeight(int scale) const {
    if (scale <= 1) return 12;
    if (scale == 2) return 18;
    return 24;
}

int DrmFramebufferCanvas::measureText(const std::string& text, int scale) const {
    return static_cast<int>(text.size()) * charAdvance(scale);
}

// Reconstructs a high-resolution 16x24 anti-aliased glyph using Scale2x/EPX
// diagonal smoothing so letters have crisp, modern curves & diagonals.
void DrmFramebufferCanvas::drawCharSmooth(int x, int y, char c, Color color, int scale, bool bold) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc < 32 || uc > 126) uc = ' ';
    if (uc == ' ') return;
    const uint8_t* glyph = font_8x8_basic[uc - 32];

    auto bit = [&](int r, int col) -> int {
        if (r < 0 || r >= 8 || col < 0 || col >= 8) return 0;
        return (glyph[r] & (1 << col)) ? 1 : 0;
    };

    // Step 1: EPX 2x magnification into 16x16 with anti-aliased diagonal smoothing
    uint8_t epx[16][16];
    for (int r = 0; r < 8; ++r) {
        for (int col = 0; col < 8; ++col) {
            int P = bit(r, col);
            int A = bit(r - 1, col);
            int B = bit(r, col + 1);
            int C = bit(r, col - 1);
            int D = bit(r + 1, col);

            if (P) {
                epx[r * 2][col * 2]         = 255;
                epx[r * 2][col * 2 + 1]     = 255;
                epx[r * 2 + 1][col * 2]     = 255;
                epx[r * 2 + 1][col * 2 + 1] = 255;
            } else {
                // Smooth interior diagonal corners (anti-aliasing)
                epx[r * 2][col * 2]         = (C && A && (!bit(r - 1, col - 1))) ? 145 : 0;
                epx[r * 2][col * 2 + 1]     = (A && B && (!bit(r - 1, col + 1))) ? 145 : 0;
                epx[r * 2 + 1][col * 2]     = (D && C && (!bit(r + 1, col - 1))) ? 145 : 0;
                epx[r * 2 + 1][col * 2 + 1] = (B && D && (!bit(r + 1, col + 1))) ? 145 : 0;
            }
        }
    }

    int outW = (scale <= 1) ? 7 : (scale == 2 ? 9 : 13);
    int outH = charHeight(scale);

    for (int dy = 0; dy < outH; ++dy) {
        float sy = (dy * 15.0f) / std::max(1, outH - 1);
        int y0 = static_cast<int>(sy);
        int y1 = std::min(15, y0 + 1);
        float fy = sy - y0;

        for (int dx = 0; dx < outW; ++dx) {
            float sx = (dx * 14.0f) / std::max(1, outW - 1);
            int x0 = static_cast<int>(sx);
            int x1 = std::min(15, x0 + 1);
            float fx = sx - x0;

            float v00 = epx[y0][x0];
            float v10 = epx[y0][x1];
            float v01 = epx[y1][x0];
            float v11 = epx[y1][x1];

            float val = (v00 * (1.0f - fx) + v10 * fx) * (1.0f - fy) +
                        (v01 * (1.0f - fx) + v11 * fx) * fy;

            if (val > 24.0f) {
                // Sharpen core while preserving smooth anti-aliased edge
                float norm = std::min(1.0f, (val - 24.0f) / 170.0f);
                uint8_t alpha = static_cast<uint8_t>((norm * color.a));
                blendPixel(x + dx, y + dy, color, alpha);
                if (bold && alpha > 90) {
                    blendPixel(x + dx + 1, y + dy, color, static_cast<uint8_t>(alpha * 0.72f));
                }
            }
        }
    }
}

void DrmFramebufferCanvas::drawText(int x, int y, const std::string& text, Color color, int scale, bool bold) {
    int cx = x;
    int adv = charAdvance(scale);
    for (char c : text) {
        drawCharSmooth(cx, y, c, color, scale, bold);
        cx += adv;
    }
}

void DrmFramebufferCanvas::drawTextCentered(Rect bounds, const std::string& text, Color color, int scale, bool bold) {
    if (bounds.w <= 8) return;
    int adv = charAdvance(scale);
    int maxChars = std::max(1, (bounds.w - 8) / adv);
    std::string clipped = text;
    if (static_cast<int>(clipped.size()) > maxChars) {
        clipped = (maxChars > 2) ? (clipped.substr(0, maxChars - 2) + "..") : clipped.substr(0, maxChars);
    }
    int textW = static_cast<int>(clipped.size()) * adv;
    int textH = charHeight(scale);
    int tx = bounds.x + (bounds.w - textW) / 2;
    int ty = bounds.y + (bounds.h - textH) / 2;
    drawText(tx, ty, clipped, color, scale, bold);
}

void DrmFramebufferCanvas::drawTextRight(int rightX, int y, const std::string& text, Color color, int scale, bool bold) {
    int textW = measureText(text, scale);
    drawText(rightX - textW, y, text, color, scale, bold);
}

void DrmFramebufferCanvas::drawTextRightClipped(int minX, int rightX, int y, const std::string& text, Color color, int scale, bool bold) {
    int adv = charAdvance(scale);
    int availW = std::max(adv, rightX - minX);
    int maxChars = availW / adv;
    std::string clipped = text;
    if (static_cast<int>(clipped.size()) > maxChars) {
        clipped = (maxChars > 2) ? (clipped.substr(0, maxChars - 2) + "..") : clipped.substr(0, maxChars);
    }
    int textW = static_cast<int>(clipped.size()) * adv;
    drawText(rightX - textW, y, clipped, color, scale, bold);
}

void DrmFramebufferCanvas::drawTextClipped(Rect clipBounds, const std::string& text, Color color, int scale, bool bold) {
    int adv = charAdvance(scale);
    int maxChars = std::max(0, clipBounds.w / adv);
    if (maxChars <= 0) return;

    std::string clipped = text;
    if (static_cast<int>(clipped.size()) > maxChars) {
        clipped = (maxChars > 2) ? (clipped.substr(0, maxChars - 2) + "..") : clipped.substr(0, maxChars);
    }
    drawText(clipBounds.x, clipBounds.y, clipped, color, scale, bold);
}

void DrmFramebufferCanvas::drawTextWrapped(Rect bounds, const std::string& text, Color color, int scale, int lineGap) {
    int adv = charAdvance(scale);
    int chH = charHeight(scale);
    int lineH = chH + lineGap;
    int maxCols = std::max(1, bounds.w / adv);

    int curY = bounds.y;
    std::string currentLine;
    std::string word;

    auto flushLine = [&]() {
        if (curY + chH <= bounds.y + bounds.h) {
            drawTextClipped({bounds.x, curY, bounds.w, chH}, currentLine, color, scale, false);
        }
        curY += lineH;
        currentLine.clear();
    };

    for (size_t i = 0; i <= text.size(); ++i) {
        char c = (i < text.size()) ? text[i] : ' ';
        if (c == '\r') continue;
        if (c == ' ' || c == '\n') {
            if (!word.empty()) {
                if (static_cast<int>(word.size()) > maxCols) {
                    word = word.substr(0, maxCols);
                }
                int needed = static_cast<int>(currentLine.empty() ? word.size() : currentLine.size() + 1 + word.size());
                if (needed > maxCols) {
                    flushLine();
                    if (curY + chH > bounds.y + bounds.h) return;
                    currentLine = word;
                } else {
                    if (!currentLine.empty()) currentLine += ' ';
                    currentLine += word;
                }
                word.clear();
            }
            if (c == '\n') {
                flushLine();
                if (curY + chH > bounds.y + bounds.h) return;
            }
        } else {
            word += c;
        }
    }
    if (!currentLine.empty() && curY + chH <= bounds.y + bounds.h) {
        drawTextClipped({bounds.x, curY, bounds.w, chH}, currentLine, color, scale, false);
    }
}

// ============================================================================
// Pure C++17 Zero-Dependency PNG (RFC-1951 DEFLATE) + P6 PPM Image Decoder
// ============================================================================
namespace {

struct BitStream {
    const uint8_t* data;
    size_t size;
    size_t bytePos = 0;
    int bitPos = 0;

    uint32_t readBits(int count) {
        uint32_t val = 0;
        for (int i = 0; i < count; ++i) {
            if (bytePos >= size) return val;
            uint32_t b = (data[bytePos] >> bitPos) & 1u;
            val |= (b << i);
            bitPos++;
            if (bitPos == 8) {
                bitPos = 0;
                bytePos++;
            }
        }
        return val;
    }

    void alignByte() {
        if (bitPos != 0) {
            bitPos = 0;
            bytePos++;
        }
    }
};

struct HuffTree {
    uint16_t counts[16] = {0};
    uint16_t symbols[288] = {0};

    void build(const uint8_t* lengths, int numSyms) {
        std::memset(counts, 0, sizeof(counts));
        for (int i = 0; i < numSyms; ++i) {
            if (lengths[i] > 0 && lengths[i] < 16) counts[lengths[i]]++;
        }
        uint16_t offsets[16] = {0};
        uint16_t sum = 0;
        for (int i = 1; i < 16; ++i) {
            offsets[i] = sum;
            sum += counts[i];
        }
        for (int i = 0; i < numSyms; ++i) {
            int len = lengths[i];
            if (len > 0 && len < 16) {
                symbols[offsets[len]++] = static_cast<uint16_t>(i);
            }
        }
    }

    int decode(BitStream& bs) const {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(bs.readBits(1));
            int count = counts[len];
            if (code - count < first) {
                return symbols[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

static bool inflateZlib(const std::vector<uint8_t>& compressed, std::vector<uint8_t>& out) {
    if (compressed.size() < 6) return false;
    // Skip 2-byte zlib header (+4 bytes if FDICT flag is set)
    size_t hdrLen = ((compressed[1] & 0x20) != 0) ? 6 : 2;
    if (compressed.size() <= hdrLen) return false;
    BitStream bs{compressed.data() + hdrLen, compressed.size() - hdrLen, 0, 0};

    static const int lenBase[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const int lenExtra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const int distBase[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static const int distExtra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    static const int clOrder[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };

    int bfinal = 0;
    do {
        bfinal = static_cast<int>(bs.readBits(1));
        int btype = static_cast<int>(bs.readBits(2));
        if (btype == 0) {
            bs.alignByte();
            if (bs.bytePos + 4 > bs.size) return false;
            uint16_t len = static_cast<uint16_t>(bs.data[bs.bytePos] | (bs.data[bs.bytePos + 1] << 8));
            bs.bytePos += 4;
            if (bs.bytePos + len > bs.size) return false;
            out.insert(out.end(), bs.data + bs.bytePos, bs.data + bs.bytePos + len);
            bs.bytePos += len;
        } else if (btype == 1 || btype == 2) {
            HuffTree litTree, distTree;
            if (btype == 1) {
                uint8_t litLens[288];
                for (int i = 0; i <= 143; ++i) litLens[i] = 8;
                for (int i = 144; i <= 255; ++i) litLens[i] = 9;
                for (int i = 256; i <= 279; ++i) litLens[i] = 7;
                for (int i = 280; i <= 287; ++i) litLens[i] = 8;
                litTree.build(litLens, 288);
                uint8_t distLens[32];
                for (int i = 0; i < 32; ++i) distLens[i] = 5;
                distTree.build(distLens, 32);
            } else {
                int hlit  = static_cast<int>(bs.readBits(5)) + 257;
                int hdist = static_cast<int>(bs.readBits(5)) + 1;
                int hclen = static_cast<int>(bs.readBits(4)) + 4;
                uint8_t clLens[19] = {0};
                for (int i = 0; i < hclen; ++i) {
                    clLens[clOrder[i]] = static_cast<uint8_t>(bs.readBits(3));
                }
                HuffTree clTree;
                clTree.build(clLens, 19);

                uint8_t allLens[320] = {0};
                int idx = 0;
                while (idx < hlit + hdist) {
                    int sym = clTree.decode(bs);
                    if (sym < 0) return false;
                    if (sym < 16) {
                        allLens[idx++] = static_cast<uint8_t>(sym);
                    } else if (sym == 16) {
                        int rep = static_cast<int>(bs.readBits(2)) + 3;
                        uint8_t prev = (idx > 0) ? allLens[idx - 1] : 0;
                        while (rep-- > 0 && idx < hlit + hdist) allLens[idx++] = prev;
                    } else if (sym == 17) {
                        int rep = static_cast<int>(bs.readBits(3)) + 3;
                        while (rep-- > 0 && idx < hlit + hdist) allLens[idx++] = 0;
                    } else if (sym == 18) {
                        int rep = static_cast<int>(bs.readBits(7)) + 11;
                        while (rep-- > 0 && idx < hlit + hdist) allLens[idx++] = 0;
                    }
                }
                litTree.build(allLens, hlit);
                distTree.build(allLens + hlit, hdist);
            }

            while (true) {
                int sym = litTree.decode(bs);
                if (sym < 0) return false;
                if (sym < 256) {
                    out.push_back(static_cast<uint8_t>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    int lidx = sym - 257;
                    if (lidx < 0 || lidx >= 29) return false;
                    int length = lenBase[lidx] + static_cast<int>(bs.readBits(lenExtra[lidx]));
                    int dsym = distTree.decode(bs);
                    if (dsym < 0 || dsym >= 30) return false;
                    int dist = distBase[dsym] + static_cast<int>(bs.readBits(distExtra[dsym]));
                    if (dist <= 0 || static_cast<size_t>(dist) > out.size()) return false;
                    for (int k = 0; k < length; ++k) {
                        out.push_back(out[out.size() - dist]);
                    }
                }
            }
        } else {
            return false;
        }
    } while (!bfinal);

    return true;
}

static uint8_t paethPredictor(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a);
    int pb = std::abs(p - b);
    int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return static_cast<uint8_t>(a);
    if (pb <= pc) return static_cast<uint8_t>(b);
    return static_cast<uint8_t>(c);
}

static bool decodePNG(const std::string& path, DecodedImage& outImg) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 33) return false;
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (std::memcmp(bytes.data(), sig, 8) != 0) return false;

    auto readU32 = [&](size_t off) -> uint32_t {
        return (static_cast<uint32_t>(bytes[off]) << 24) |
               (static_cast<uint32_t>(bytes[off + 1]) << 16) |
               (static_cast<uint32_t>(bytes[off + 2]) << 8) |
               static_cast<uint32_t>(bytes[off + 3]);
    };

    int width = 0, height = 0, bitDepth = 0, colorType = 0, interlace = 0;
    std::vector<uint8_t> idat;
    std::vector<uint8_t> palette;

    size_t pos = 8;
    while (pos + 12 <= bytes.size()) {
        uint32_t len = readU32(pos);
        const uint8_t* type = &bytes[pos + 4];
        const uint8_t* cdata = &bytes[pos + 8];
        if (pos + 12 + len > bytes.size()) break;

        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            width     = static_cast<int>(readU32(pos + 8));
            height    = static_cast<int>(readU32(pos + 12));
            bitDepth  = cdata[8];
            colorType = cdata[9];
            interlace = cdata[12];
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            palette.assign(cdata, cdata + len);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), cdata, cdata + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len;
    }

    if (width <= 0 || height <= 0 || idat.empty() || interlace != 0) return false;
    if (bitDepth != 1 && bitDepth != 2 && bitDepth != 4 && bitDepth != 8 && bitDepth != 16) return false;
    int channels = (colorType == 6) ? 4 : (colorType == 2 ? 3 : (colorType == 4 ? 2 : (colorType == 0 || colorType == 3 ? 1 : 0)));
    if (channels == 0) return false;

    size_t bitsPerPixel = static_cast<size_t>(channels) * static_cast<size_t>(bitDepth);
    size_t filterBpp = (bitsPerPixel + 7) / 8;
    size_t stride = (static_cast<size_t>(width) * bitsPerPixel + 7) / 8;

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (stride + 1));
    if (!inflateZlib(idat, raw)) return false;
    if (raw.size() < static_cast<size_t>(height) * (stride + 1)) return false;

    std::vector<uint8_t> recon(static_cast<size_t>(height) * stride, 0);
    for (int y = 0; y < height; ++y) {
        uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        uint8_t* dst = &recon[y * stride];
        const uint8_t* prev = (y > 0) ? &recon[(y - 1) * stride] : nullptr;

        for (size_t i = 0; i < stride; ++i) {
            uint8_t a = (i >= filterBpp) ? dst[i - filterBpp] : 0;
            uint8_t b = prev ? prev[i] : 0;
            uint8_t c = (prev && i >= filterBpp) ? prev[i - filterBpp] : 0;
            switch (filter) {
                case 0: dst[i] = src[i]; break;
                case 1: dst[i] = static_cast<uint8_t>(src[i] + a); break;
                case 2: dst[i] = static_cast<uint8_t>(src[i] + b); break;
                case 3: dst[i] = static_cast<uint8_t>(src[i] + ((a + b) >> 1)); break;
                case 4: dst[i] = static_cast<uint8_t>(src[i] + paethPredictor(a, b, c)); break;
                default: dst[i] = src[i]; break;
            }
        }
    }

    outImg.width = width;
    outImg.height = height;
    outImg.rgb.resize(static_cast<size_t>(width) * height * 3);

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = &recon[y * stride];
        for (int x = 0; x < width; ++x) {
            uint8_t r = 18, g = 18, b = 22;
            if (bitDepth == 8) {
                if (colorType == 6) {
                    uint8_t a = row[x * 4 + 3];
                    r = static_cast<uint8_t>((row[x * 4]     * a + 18 * (255 - a)) / 255);
                    g = static_cast<uint8_t>((row[x * 4 + 1] * a + 18 * (255 - a)) / 255);
                    b = static_cast<uint8_t>((row[x * 4 + 2] * a + 22 * (255 - a)) / 255);
                } else if (colorType == 2) {
                    r = row[x * 3];
                    g = row[x * 3 + 1];
                    b = row[x * 3 + 2];
                } else if (colorType == 3) {
                    int pidx = row[x] * 3;
                    if (pidx + 2 < static_cast<int>(palette.size())) {
                        r = palette[pidx];
                        g = palette[pidx + 1];
                        b = palette[pidx + 2];
                    }
                } else if (colorType == 0 || colorType == 4) {
                    r = g = b = row[x * channels];
                }
            } else if (bitDepth == 16) {
                if (colorType == 6) {
                    uint8_t a = row[(x * 4 + 3) * 2];
                    r = static_cast<uint8_t>((row[(x * 4)     * 2] * a + 18 * (255 - a)) / 255);
                    g = static_cast<uint8_t>((row[(x * 4 + 1) * 2] * a + 18 * (255 - a)) / 255);
                    b = static_cast<uint8_t>((row[(x * 4 + 2) * 2] * a + 22 * (255 - a)) / 255);
                } else if (colorType == 2) {
                    r = row[(x * 3)     * 2];
                    g = row[(x * 3 + 1) * 2];
                    b = row[(x * 3 + 2) * 2];
                } else {
                    r = g = b = row[(x * channels) * 2];
                }
            } else {
                // Sub-byte bitDepth (1, 2, 4) for colorType 3 (indexed palette) or 0 (grayscale)
                uint8_t v = 0;
                if (bitDepth == 4) v = (row[x >> 1] >> ((1 - (x & 1)) * 4)) & 0x0F;
                else if (bitDepth == 2) v = (row[x >> 2] >> ((3 - (x & 3)) * 2)) & 0x03;
                else if (bitDepth == 1) v = (row[x >> 3] >> (7 - (x & 7))) & 0x01;
                if (colorType == 3) {
                    int pidx = static_cast<int>(v) * 3;
                    if (pidx + 2 < static_cast<int>(palette.size())) {
                        r = palette[pidx];
                        g = palette[pidx + 1];
                        b = palette[pidx + 2];
                    }
                } else {
                    int scale = (bitDepth == 4) ? 17 : (bitDepth == 2 ? 85 : 255);
                    r = g = b = static_cast<uint8_t>(v * scale);
                }
            }
            size_t outIdx = (static_cast<size_t>(y) * width + x) * 3;
            outImg.rgb[outIdx]     = r;
            outImg.rgb[outIdx + 1] = g;
            outImg.rgb[outIdx + 2] = b;
        }
    }
    outImg.valid = true;
    return true;
}

static bool decodePPM(const std::string& path, DecodedImage& outImg) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    char magic[16] = {0};
    if (!std::fgets(magic, sizeof(magic), fp) || std::strncmp(magic, "P6", 2) != 0) {
        std::fclose(fp);
        return false;
    }
    int ch = std::fgetc(fp);
    while (ch == '#') {
        while (ch != '\n' && ch != EOF) ch = std::fgetc(fp);
        ch = std::fgetc(fp);
    }
    std::ungetc(ch, fp);

    int imgW = 0, imgH = 0, maxVal = 0;
    if (std::fscanf(fp, "%d %d %d", &imgW, &imgH, &maxVal) != 3 || imgW <= 0 || imgH <= 0) {
        std::fclose(fp);
        return false;
    }
    std::fgetc(fp);

    size_t numBytes = static_cast<size_t>(imgW) * imgH * 3;
    outImg.rgb.resize(numBytes);
    if (std::fread(outImg.rgb.data(), 1, numBytes, fp) != numBytes) {
        std::fclose(fp);
        return false;
    }
    std::fclose(fp);
    outImg.width = imgW;
    outImg.height = imgH;
    outImg.valid = true;
    return true;
}

} // anonymous namespace

bool DrmFramebufferCanvas::loadImageFile(const std::string& path, DecodedImage& outImg) {
    if (path.empty()) return false;
    if (decodePNG(path, outImg)) return true;
    if (decodePPM(path, outImg)) return true;
    return false;
}

// Renders a PNG or PPM image with Bilinear Filtering and SDF Rounded Corner Masking
bool DrmFramebufferCanvas::drawImageRounded(
    Rect r, int radius, const std::string& path, const std::string& fallbackUrl, const std::string& pluginId
) {
    if (r.w <= 0 || r.h <= 0) return false;
    std::string cacheKey = !pluginId.empty() ? pluginId : path;

    auto it = image_cache_.find(cacheKey);
    if (it == image_cache_.end()) {
        (void)fallbackUrl;
        DecodedImage decoded;
        // 1. Always try real downloaded PNG screenshot (plugins/<id>/thumb.png) first
        if (!pluginId.empty()) {
            std::string pngLocal = "plugins/" + pluginId + "/thumb.png";
            struct stat stPng;
            if (stat(pngLocal.c_str(), &stPng) == 0 && stPng.st_size == 0) {
                unlink(pngLocal.c_str());
            }
            loadImageFile(pngLocal, decoded);
        }
        // 2. Fallback to explicit path or local thumb.ppm if thumb.png is not yet downloaded
        if (!decoded.valid && !path.empty()) {
            loadImageFile(path, decoded);
        }
        if (!decoded.valid && !pluginId.empty()) {
            std::string ppmLocal = "plugins/" + pluginId + "/thumb.ppm";
            loadImageFile(ppmLocal, decoded);
        }
        if (decoded.valid) {
            image_cache_[cacheKey] = decoded;
            it = image_cache_.find(cacheKey);
        } else {
            return false;
        }
    }

    const DecodedImage& img = it->second;
    if (!img.valid || img.width <= 0 || img.height <= 0) return false;

    int rad = std::min({radius, r.w / 2, r.h / 2});
    float radF = static_cast<float>(rad);

    // Aspect-cover crop & bilinear scaling
    float scaleX = static_cast<float>(img.width) / r.w;
    float scaleY = static_cast<float>(img.height) / r.h;
    float scale = std::min(scaleX, scaleY);
    float cropW = r.w * scale;
    float offX = (img.width - cropW) * 0.5f;
    float offY = 0.0f; // Top-aligned like object-top in CSS

    for (int dy = 0; dy < r.h; ++dy) {
        float sy = std::max(0.0f, std::min(static_cast<float>(img.height - 1), offY + dy * scale));
        int y0 = static_cast<int>(sy);
        int y1 = std::min(img.height - 1, y0 + 1);
        float fy = sy - y0;

        for (int dx = 0; dx < r.w; ++dx) {
            // Check rounded corner SDF coverage
            float cov = 1.0f;
            if (rad > 1) {
                int cx = (dx < rad) ? dx : (dx >= r.w - rad ? (r.w - 1 - dx) : -1);
                int cy = (dy < rad) ? dy : (dy >= r.h - rad ? (r.h - 1 - dy) : -1);
                if (cx >= 0 && cy >= 0) {
                    float ddx = (radF - 0.5f) - cx;
                    float ddy = (radF - 0.5f) - cy;
                    float dist = std::sqrt(ddx * ddx + ddy * ddy);
                    cov = std::max(0.0f, std::min(1.0f, radF + 0.5f - dist));
                }
            }
            if (cov <= 0.0f) continue;

            float sx = std::max(0.0f, std::min(static_cast<float>(img.width - 1), offX + dx * scale));
            int x0 = static_cast<int>(sx);
            int x1 = std::min(img.width - 1, x0 + 1);
            float fx = sx - x0;

            const uint8_t* p00 = &img.rgb[(y0 * img.width + x0) * 3];
            const uint8_t* p10 = &img.rgb[(y0 * img.width + x1) * 3];
            const uint8_t* p01 = &img.rgb[(y1 * img.width + x0) * 3];
            const uint8_t* p11 = &img.rgb[(y1 * img.width + x1) * 3];

            uint8_t cr = static_cast<uint8_t>((p00[0] * (1 - fx) + p10[0] * fx) * (1 - fy) + (p01[0] * (1 - fx) + p11[0] * fx) * fy);
            uint8_t cg = static_cast<uint8_t>((p00[1] * (1 - fx) + p10[1] * fx) * (1 - fy) + (p01[1] * (1 - fx) + p11[1] * fx) * fy);
            uint8_t cb = static_cast<uint8_t>((p00[2] * (1 - fx) + p10[2] * fx) * (1 - fy) + (p01[2] * (1 - fx) + p11[2] * fx) * fy);

            // Subtle dark vignette at bottom 28px so bottom-right badge is always legible
            if (dy > r.h - 28) {
                float vig = 1.0f - 0.45f * static_cast<float>(dy - (r.h - 28)) / 28.0f;
                cr = static_cast<uint8_t>(cr * vig);
                cg = static_cast<uint8_t>(cg * vig);
                cb = static_cast<uint8_t>(cb * vig);
            }

            blendPixel(r.x + dx, r.y + dy, Color(cr, cg, cb), static_cast<uint8_t>(cov * 255));
        }
    }

    drawRoundedRect(r, rad, Palette::BorderLight, 1.0f);
    return true;
}

void DrmFramebufferCanvas::blitToFramebuffer() {
    if (!fb_ptr_) return;
    int bpp = static_cast<int>(vinfo_.bits_per_pixel);
    int bytes_per_pixel = bpp / 8;

    for (int ly = 0; ly < log_height_; ++ly) {
        for (int lx = 0; lx < log_width_; ++lx) {
            int px = lx;
            int py = ly;
            if (rotation_ == 90) {
                px = phys_width_ - 1 - ly;
                py = lx;
            } else if (rotation_ == 180) {
                px = phys_width_ - 1 - lx;
                py = phys_height_ - 1 - ly;
            } else if (rotation_ == 270) {
                px = ly;
                py = phys_height_ - 1 - lx;
            }

            if (px >= 0 && px < phys_width_ && py >= 0 && py < phys_height_) {
                uint32_t color = backbuffer_[ly * log_width_ + lx];
                size_t location = static_cast<size_t>(px + vinfo_.xoffset) * bytes_per_pixel +
                                  static_cast<size_t>(py + vinfo_.yoffset) * finfo_.line_length;
                if (location + bytes_per_pixel <= fb_mmap_size_) {
                    if (bpp == 32) {
                        *reinterpret_cast<uint32_t*>(fb_ptr_ + location) = color;
                    } else if (bpp == 16) {
                        *reinterpret_cast<uint16_t*>(fb_ptr_ + location) = static_cast<uint16_t>(color);
                    }
                }
            }
        }
    }

    if (fb_fd_ >= 0) {
        ioctl(fb_fd_, FBIOPAN_DISPLAY, &vinfo_);
    }
}

} // namespace tkgl::gfx

/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: include/tkgl_config.hpp
 * Description: Configuration & Online Catalog Parser
 *              Supports "global" (including "catalog_url"), "sections", "items",
 *              tags, osCompat, and live online catalog refresh.
 * ============================================================================
 */



#include <string>
#include <vector>
#include <functional>

namespace tkgl::config {

constexpr const char* TKGL_MANAGER_VERSION = "V1.1.2 (v1.1.2)";

struct MenuItem {
    std::string id;
    std::string label;
    std::string author        = "The KikGen Labs";
    std::string version       = "v1.1.2";
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
};

using PluginCatalogConfig = LauncherConfig;

} // namespace tkgl::config

/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: src/tkgl_config.cpp
 * Description: Block-delimited parser for mpc_plugins.conf, internet reachability
 *              check via ping, and libcurl IPv4 downloader for catalog.json,
 *              plugin thumbnails (thumb.png), and plugin archives.
 * ============================================================================
 */


#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <csignal>
#include <ctime>
#include <chrono>
#include <dirent.h>
#include <libgen.h>
#include <dlfcn.h>
#include <unordered_map>
#include <algorithm>

namespace tkgl::config {

namespace {

// Reload glibc DNS resolver configuration (handles autorun starting before Wi-Fi DHCP updates /etc/resolv.conf)
static void reloadSystemDnsResolver() {
    using ResInitFn = int(*)(void);
    static ResInitFn fn = nullptr;
    static bool lookedUp = false;
    if (!lookedUp) {
        lookedUp = true;
        fn = reinterpret_cast<ResInitFn>(dlsym(RTLD_DEFAULT, "__res_init"));
        if (!fn) fn = reinterpret_cast<ResInitFn>(dlsym(RTLD_DEFAULT, "res_init"));
    }
    if (fn) fn();
}

static std::string sanitizePingHost(const std::string& rawHost) {
    std::string s = rawHost;
    if (s.rfind("https://", 0) == 0) s = s.substr(8);
    else if (s.rfind("http://", 0) == 0) s = s.substr(7);
    size_t slash = s.find_first_of("/:");
    if (slash != std::string::npos) s = s.substr(0, slash);

    std::string clean;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') {
            clean += c;
        }
    }
    return clean.empty() ? "github.com" : clean;
}

} // anonymous namespace

bool ConfigParser::checkInternetReachable(
    const std::string& pingHost,
    std::vector<std::string>* outLogs
) {
    reloadSystemDnsResolver();
    std::string host = sanitizePingHost(pingHost);

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            if (devNull > 2) close(devNull);
        }
        execlp("ping", "ping", "-c", "1", "-W", "2", host.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    } else if (pid > 0) {
        auto startWait = std::chrono::steady_clock::now();
        int status = 0;
        while (true) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                bool ok = (WIFEXITED(status) && WEXITSTATUS(status) == 0);
                if (ok) {
                    std::string msg = "[TKGL-NET] Online check OK (ping " + host + " succeeded).";
                    std::printf("%s\n", msg.c_str());
                    if (outLogs) outLogs->push_back(msg);
                } else {
                    std::string msg = "[TKGL-NET] Offline: ping " + host + " failed (no internet access).";
                    std::fprintf(stderr, "%s\n", msg.c_str());
                    if (outLogs) outLogs->push_back(msg);
                }
                return ok;
            }
            if (w < 0) break;
            usleep(80000);
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startWait
            ).count();
            if (elapsedMs >= 3000) {
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                std::string msg = "[TKGL-NET] Offline: ping " + host + " timed out.";
                std::fprintf(stderr, "%s\n", msg.c_str());
                if (outLogs) outLogs->push_back(msg);
                return false;
            }
        }
    }
    return false;
}

bool ConfigParser::downloadFile(
    const std::string& url,
    const std::string& destPath,
    long timeoutSec,
    std::function<void(const std::string&)> onProgress,
    const std::string& progressPrefix
) {
    if (url.empty() || destPath.empty()) {
        std::fprintf(stderr, "[ERROR] downloadFile: Empty URL or destination path.\n");
        return false;
    }

    reloadSystemDnsResolver();

    // Sanitize URL (trim whitespace and encode spaces)
    std::string cleanUrl;
    cleanUrl.reserve(url.size() + 16);
    for (char c : url) {
        if (c == '\r' || c == '\n' || c == '\t') continue;
        if (c == ' ') cleanUrl += "%20";
        else cleanUrl += c;
    }
    if (cleanUrl.empty()) return false;

    // Ensure parent directory of destPath exists
    size_t lastSlash = destPath.find_last_of('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
        std::string parentDir = destPath.substr(0, lastSlash);
        mkdir(parentDir.c_str(), 0755);
    }

    std::string tmpPath = destPath + ".part";
    unlink(tmpPath.c_str());

    FILE* fp = fopen(tmpPath.c_str(), "wb");
    if (!fp) {
        char errBuf[256];
        std::snprintf(errBuf, sizeof(errBuf), "[ERROR] Could not open %s for writing: %s", tmpPath.c_str(), strerror(errno));
        std::fprintf(stderr, "%s\n", errBuf);
        if (onProgress) onProgress(errBuf);
        return false;
    }

    // Dynamic loader for libcurl present across all MPC firmware generations
    struct LibCurlLoader {
        void* handle = nullptr;
        int   (*global_init)(long) = nullptr;
        void  (*global_cleanup)() = nullptr;
        void* (*easy_init)() = nullptr;
        void  (*easy_cleanup)(void*) = nullptr;
        int   (*easy_setopt)(void*, int, ...) = nullptr;
        int   (*easy_perform)(void*) = nullptr;
        const char* (*easy_strerror)(int) = nullptr;
        int   (*easy_getinfo)(void*, int, ...) = nullptr;
        bool  loaded = false;
        std::string loadError;

        LibCurlLoader() {
            const char* candidates[] = {
                "libcurl.so.4",
                "libcurl.so.4.8.0",
                "libcurl.so.3",
                "libcurl.so",
                "/usr/lib/libcurl.so.4",
                "/lib/libcurl.so.4",
                "/usr/lib/arm-linux-gnueabihf/libcurl.so.4",
                "/usr/lib/aarch64-linux-gnu/libcurl.so.4",
                "/usr/lib/x86_64-linux-gnu/libcurl.so.4"
            };
            for (const char* path : candidates) {
                handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
                if (handle) break;
            }
            if (!handle) {
                const char* dlErr = dlerror();
                loadError = dlErr ? dlErr : "libcurl.so.4 not found on system";
                return;
            }

            *(void**)(&global_init)   = dlsym(handle, "curl_global_init");
            *(void**)(&global_cleanup)= dlsym(handle, "curl_global_cleanup");
            *(void**)(&easy_init)     = dlsym(handle, "curl_easy_init");
            *(void**)(&easy_cleanup)  = dlsym(handle, "curl_easy_cleanup");
            *(void**)(&easy_setopt)   = dlsym(handle, "curl_easy_setopt");
            *(void**)(&easy_perform)  = dlsym(handle, "curl_easy_perform");
            *(void**)(&easy_strerror) = dlsym(handle, "curl_easy_strerror");
            *(void**)(&easy_getinfo)  = dlsym(handle, "curl_easy_getinfo");

            if (!easy_init || !easy_cleanup || !easy_setopt || !easy_perform) {
                loadError = "Required libcurl function symbols missing";
                dlclose(handle);
                handle = nullptr;
                return;
            }
            if (global_init) global_init(3L /* CURL_GLOBAL_DEFAULT */);
            loaded = true;
        }

        ~LibCurlLoader() {
            if (global_cleanup && loaded) global_cleanup();
            if (handle) dlclose(handle);
        }
    };

    static LibCurlLoader s_curl;
    if (!s_curl.loaded) {
        fclose(fp);
        unlink(tmpPath.c_str());
        std::string errMsg = "[ERROR] libcurl is not available: " + s_curl.loadError + ". Download aborted (no fallback).";
        std::fprintf(stderr, "%s\n", errMsg.c_str());
        if (onProgress) onProgress(errMsg);
        return false;
    }

    void* curl = s_curl.easy_init();
    if (!curl) {
        fclose(fp);
        unlink(tmpPath.c_str());
        std::string errMsg = "[ERROR] curl_easy_init failed. Download aborted.";
        std::fprintf(stderr, "%s\n", errMsg.c_str());
        if (onProgress) onProgress(errMsg);
        return false;
    }

    auto writeCb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        FILE* outF = static_cast<FILE*>(userdata);
        return (outF && ptr) ? fwrite(ptr, size, nmemb, outF) : 0;
    };

    struct ProgressData {
        std::function<void(const std::string&)> onProgress;
        std::string prefix;
        std::chrono::steady_clock::time_point lastTick;
        int dotCount = 0;
    };

    ProgressData pdata;
    pdata.onProgress = onProgress;
    pdata.prefix = progressPrefix;
    pdata.lastTick = std::chrono::steady_clock::now();

    auto progCb = [](void* clientp, double dltotal, double dlnow, double /*ultotal*/, double /*ulnow*/) -> int {
        ProgressData* p = static_cast<ProgressData*>(clientp);
        if (!p || !p->onProgress || p->prefix.empty()) return 0;
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - p->lastTick).count();
        if (ms >= 250) {
            p->lastTick = now;
            p->dotCount = (p->dotCount % 5) + 1;
            std::string dots(static_cast<size_t>(p->dotCount), '.');
            double mbNow = dlnow / 1048576.0;
            char buf[64];
            if (dltotal > 0) {
                double mbTotal = dltotal / 1048576.0;
                std::snprintf(buf, sizeof(buf), "%.2f / %.2f MB", mbNow, mbTotal);
            } else {
                std::snprintf(buf, sizeof(buf), "%.2f MB", mbNow);
            }
            p->onProgress(p->prefix + " (libcurl IPv4) [ " + dots + " ] " + buf + " received");
        }
        return 0;
    };

    long maxWaitSec = (timeoutSec > 0) ? timeoutSec : 60L;

    // Standard libcurl option constants (RFC & libcurl ABI compliant)
    constexpr int OPT_WRITEDATA        = 10001;
    constexpr int OPT_URL              = 10002;
    constexpr int OPT_TIMEOUT          = 13;
    constexpr int OPT_NOPROGRESS       = 43;
    constexpr int OPT_FAILONERROR      = 45;
    constexpr int OPT_FOLLOWLOCATION   = 52;
    constexpr int OPT_PROGRESSFUNCTION = 20056;
    constexpr int OPT_PROGRESSDATA     = 10057;
    constexpr int OPT_WRITEFUNCTION    = 20011;
    constexpr int OPT_SSL_VERIFYPEER   = 64;
    constexpr int OPT_SSL_VERIFYHOST   = 81;
    constexpr int OPT_CONNECTTIMEOUT   = 78;
    constexpr int OPT_NOSIGNAL         = 99;
    constexpr int OPT_USERAGENT        = 10018;
    constexpr int OPT_IPRESOLVE        = 113;
    constexpr int OPT_MAXREDIRS        = 68;
    constexpr long VAL_IPRESOLVE_V4    = 1L; // Mandatory IPv4 configuration per user requirement

    s_curl.easy_setopt(curl, OPT_URL, cleanUrl.c_str());
    s_curl.easy_setopt(curl, OPT_WRITEFUNCTION, static_cast<size_t(*)(char*,size_t,size_t,void*)>(writeCb));
    s_curl.easy_setopt(curl, OPT_WRITEDATA, fp);
    s_curl.easy_setopt(curl, OPT_IPRESOLVE, VAL_IPRESOLVE_V4); // Strictly IPv4
    s_curl.easy_setopt(curl, OPT_FOLLOWLOCATION, 1L);
    s_curl.easy_setopt(curl, OPT_MAXREDIRS, 10L);
    s_curl.easy_setopt(curl, OPT_TIMEOUT, maxWaitSec);
    s_curl.easy_setopt(curl, OPT_CONNECTTIMEOUT, 15L);
    s_curl.easy_setopt(curl, OPT_FAILONERROR, 1L);
    s_curl.easy_setopt(curl, OPT_SSL_VERIFYPEER, 0L);
    s_curl.easy_setopt(curl, OPT_SSL_VERIFYHOST, 0L);
    s_curl.easy_setopt(curl, OPT_NOSIGNAL, 1L);
    s_curl.easy_setopt(curl, OPT_USERAGENT, "TheKikGenLabs-MPC-PluginManager/1.0");

    if (onProgress && !progressPrefix.empty()) {
        s_curl.easy_setopt(curl, OPT_NOPROGRESS, 0L);
        s_curl.easy_setopt(curl, OPT_PROGRESSFUNCTION, static_cast<int(*)(void*,double,double,double,double)>(progCb));
        s_curl.easy_setopt(curl, OPT_PROGRESSDATA, &pdata);
    } else {
        s_curl.easy_setopt(curl, OPT_NOPROGRESS, 1L);
    }

    int res = s_curl.easy_perform(curl);
    long httpCode = 0;
    if (s_curl.easy_getinfo) {
        s_curl.easy_getinfo(curl, 0x200002 /* CURLINFO_RESPONSE_CODE */, &httpCode);
    }

    fclose(fp);
    s_curl.easy_cleanup(curl);

    // If libcurl does not respond, transfer fails, or HTTP code indicates error: DO NOT FALLBACK!
    if (res != 0 /* CURLE_OK */ || (httpCode >= 400)) {
        unlink(tmpPath.c_str());
        const char* errStr = s_curl.easy_strerror ? s_curl.easy_strerror(res) : "transfer failed";
        char errMsg[256];
        if (httpCode > 0) {
            std::snprintf(errMsg, sizeof(errMsg), "[ERROR] libcurl IPv4 transfer failed (HTTP %ld, res=%d: %s) for %s", httpCode, res, errStr, cleanUrl.c_str());
        } else {
            std::snprintf(errMsg, sizeof(errMsg), "[ERROR] libcurl IPv4 transfer failed (res=%d: %s) for %s", res, errStr, cleanUrl.c_str());
        }
        std::fprintf(stderr, "%s\n", errMsg);
        if (onProgress) onProgress(errMsg);
        return false; // Strictly no fallback
    }

    struct stat st;
    if (stat(tmpPath.c_str(), &st) != 0 || st.st_size <= 0) {
        unlink(tmpPath.c_str());
        std::string errMsg = "[ERROR] libcurl downloaded file is empty: " + cleanUrl;
        std::fprintf(stderr, "%s\n", errMsg.c_str());
        if (onProgress) onProgress(errMsg);
        return false;
    }

    // If destination is a .png file, verify the 8-byte PNG signature so a 404 HTML page is never saved as a thumbnail
    if (destPath.size() >= 4 && destPath.compare(destPath.size() - 4, 4, ".png") == 0) {
        std::ifstream pngCheck(tmpPath, std::ios::binary);
        uint8_t sig[8] = {0};
        pngCheck.read(reinterpret_cast<char*>(sig), 8);
        pngCheck.close();
        static const uint8_t kPngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        if (std::memcmp(sig, kPngSig, 8) != 0) {
            unlink(tmpPath.c_str());
            std::string errMsg = "[ERROR] Downloaded thumbnail has invalid PNG signature: " + cleanUrl;
            std::fprintf(stderr, "%s\n", errMsg.c_str());
            if (onProgress) onProgress(errMsg);
            return false;
        }
    }

    unlink(destPath.c_str());
    if (std::rename(tmpPath.c_str(), destPath.c_str()) == 0) {
        return true;
    }

    std::ifstream srcF(tmpPath, std::ios::binary);
    std::ofstream dstF(destPath, std::ios::binary | std::ios::trunc);
    if (srcF.is_open() && dstF.is_open()) {
        dstF << srcF.rdbuf();
        dstF.close();
        unlink(tmpPath.c_str());
        return (stat(destPath.c_str(), &st) == 0 && st.st_size > 0);
    }

    unlink(tmpPath.c_str());
    return false;
}

std::string ConfigParser::getManagerRootDir() {
    // Capture initial working directory before any chdir() happens in main()
    static std::string s_initialCwd;
    if (s_initialCwd.empty()) {
        char cwdBuf[1024];
        if (getcwd(cwdBuf, sizeof(cwdBuf)) != nullptr) {
            s_initialCwd = cwdBuf;
            while (s_initialCwd.size() > 1 && s_initialCwd.back() == '/') s_initialCwd.pop_back();
        }
    }

    // 1. Check PLG_MAN_DIR environment variable first (handles /tmp execution when USB vfat is mounted noexec)
    const char* envDir = std::getenv("PLG_MAN_DIR");
    if (envDir && envDir[0] != '\0') {
        std::string dir(envDir);
        char resolved[1024];
        if (dir[0] != '/' && realpath(dir.c_str(), resolved) != nullptr) {
            dir = resolved;
        } else if (dir == "." && !s_initialCwd.empty()) {
            dir = s_initialCwd;
        }
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        return dir;
    }

    // 2. Fallback to executable directory via /proc/self/exe, UNLESS running from /tmp without config
    char exePath[1024];
    ssize_t exeLen = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (exeLen > 0) {
        exePath[exeLen] = '\0';
        char* exeDir = dirname(exePath);
        if (exeDir && std::strlen(exeDir) > 0) {
            std::string dir(exeDir);
            while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
            bool isTmpExec = (dir == "/tmp" || dir.rfind("/tmp/", 0) == 0 || dir == "/var/tmp");
            struct stat stConf;
            std::string confInExe = dir + "/mpc_plugins.conf";
            if (!isTmpExec || stat(confInExe.c_str(), &stConf) == 0) {
                return dir;
            }
        }
    }

    // 3. Fallback to initial working directory (e.g. when launched as /tmp/mpc_plugin_manager_fb from USB folder)
    if (!s_initialCwd.empty() && s_initialCwd != "/tmp") {
        return s_initialCwd;
    }

    char cwdBuf[1024];
    if (getcwd(cwdBuf, sizeof(cwdBuf)) != nullptr) {
        std::string dir(cwdBuf);
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        return dir;
    }

    return "/media/az01-internal-sd/KikGen_MPC_Force_Plugin_Manager";
}

void LauncherConfig::loadEmbeddedDefaults() {
    sections.clear();
    {
        Section sec;
        sec.title = "INSTRUMENTS";
        sec.items = {
        {
            "tr-6w6", "6W6", "athousanddetails", "v1.2.0", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 2.x & 3.x", "2026-11-02",
            "2606", "7.6 MB", "31%",
            "athousanddetails - VST - 6W6", "tr6w6.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/6w6.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/6w6-vst-v1.2.0/6W6-1.2.0-mpc-armv7.zip", "plugins/tr-6w6/thumb.png",
            "MPC/Force port of athousanddetails' schwung-6W6 TR-606 drum machine: voices fitted against real hardware recordings, with measured-partial hats and cymbal. Touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - 6W6\" && if [ -f \"./plugins/tr-6w6/tr6w6.so\" ]; then cp -rf ./plugins/tr-6w6/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - 6W6/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/6w6-vst-v1.2.0/6W6-1.2.0-mpc-armv7.zip\" \"/tmp/tr-6w6.zip\") && unzip -o /tmp/tr-6w6.zip -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - 6W6/\" && rm -f /tmp/tr-6w6.zip; fi && sync", 1,
            false,
            false,
            false,
            { "tr-606" }
        },
        {
            "tr-8w8", "8W8", "athousanddetails", "v1.2.0", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 2.x & 3.x", "2026-11-02",
            "2706", "5.9 MB", "13%",
            "athousanddetails - VST - 8W8", "tr8w8.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/8w8.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/8w8-vst-v1.2.0/8W8-1.2.0-mpc-armv7.zip", "plugins/tr-8w8/thumb.png",
            "MPC/Force port of athousanddetails' schwung-8W8 TR-808 drum machine: 15 of 16 voices circuit-modelled from the service notes. Touchscreen skin and Q-Links.", "./plugins/tr-8w8/install.sh", 0,
            false,
            false,
            false,
            { "tr-808" }
        },
        {
            "tr-9w9", "9W9", "athousanddetails", "v1.2.0", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 2.x & 3.x", "2026-11-02",
            "2643", "5.1 MB", "21%",
            "athousanddetails - VST - 9W9", "tr9w9.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/9w9.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.2.0/9W9-1.2.0-mpc-armv7.zip", "plugins/tr-9w9/thumb.png",
            "MPC/Force port of athousanddetails' schwung-9W9 TR-909 drum machine: circuit-modelled kick, snare, toms, rim and clap, with sampled hats, ride and crash (ER-99 derived), as on the original. Touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9\" && if [ -f \"./plugins/tr-9w9/tr9w9.so\" ]; then cp -rf ./plugins/tr-9w9/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.2.0/9W9-1.2.0-mpc-armv7.zip\" \"/tmp/tr-9w9.zip\") && unzip -o /tmp/tr-9w9.zip -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9/\" && rm -f /tmp/tr-9w9.zip; fi && sync", 1,
            false,
            false,
            false,
            { "tr-909" }
        },
        {
            "acid-303", "Acid-303", "sd88me", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "823", "23.6 MB", "28%",
            "sd88me - VST - Acid-303", "acid303.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-acid303/main/docs/device-303.png", "https://github.com/sd88me/mpc-vst-acid303/releases/download/acid303-vst-v1.0.2/Acid-303-1.0.2-mpc-armv7.zip", "plugins/acid-303/thumb.png",
            "An accurate, circuit-modelled 303 voice, matched to the hardware: the voice (from Lemus) solves the original schematic transistor by transistor and is fitted to 400 notes from a real unit. Switchable Devil Fish mods; Gen1 and Gen2 (WARN for CPU on Gen1: one or two instances, ECO).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Acid-303\" && if [ -f \"./plugins/acid-303/acid303.so\" ]; then cp -rf ./plugins/acid-303/* \"$TARGET_SYNTH_DIR/sd88me - VST - Acid-303/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-acid303/releases/download/acid303-vst-v1.0.2/Acid-303-1.0.2-mpc-armv7.zip\" \"/tmp/acid-303.zip\") && unzip -o /tmp/acid-303.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Acid-303/\" && rm -f /tmp/acid-303.zip; fi && sync", 1,
            false,
            false,
            false,
            { "303", "acid", "mono", "bass", "circuit-model", "devil-fish" }
        },
        {
            "aphex", "Aphex", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1559", "7.4 MB", "14%",
            "Filliformes - VST - [SYN] Aphex", "aphex.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-aphex/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-aphex/releases/download/v1.1.1/SYN-Aphex-1.1.1-mpc-armv7.zip", "plugins/aphex/thumb.png",
            "Korg MS-10/MS-20 style mono synth with a patch bay, an ESP section and both MS filters.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Aphex\" && if [ -f \"./plugins/aphex/aphex.so\" ]; then cp -rf ./plugins/aphex/* \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Aphex/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-aphex/releases/download/v1.1.1/SYN-Aphex-1.1.1-mpc-armv7.zip\" \"/tmp/aphex.zip\") && unzip -o /tmp/aphex.zip -d \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Aphex/\" && rm -f /tmp/aphex.zip; fi && sync", 1,
            false,
            false,
            false,
            { "ms-20", "monophonic", "patch-bay", "analog-modelling" }
        },
        {
            "chordism", "Chordism", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1570", "2.3 MB", "14%",
            "Charles Vestal - VST - [SYN] Chordism", "chordism.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-chordism/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-chordism/releases/download/v1.0.2/SYN-Chordism-1.0.2-mpc-armv7.zip", "plugins/chordism/thumb.png",
            "One key in, a four-voice chord out: morphing oscillators, FM, filter, lo-fi, delay, reverb and an arpeggiator.", "./plugins/chordism/install.sh", 0,
            false,
            false,
            false,
            { "chords", "arpeggiator", "fm", "lo-fi" }
        },
        {
            "clementine-xt", "Clementine-XT", "sd88me", "v0.9.7", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-11-01",
            "4225", "2.5 MB", "14%",
            "sd88me - VST - Clementine-XT", "clementine_xt.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-clementineXT/main/docs/img/clementine-xt.png", "https://github.com/sd88me/mpc-vst-clementineXT/releases/download/clementine-xt-v0.9.7/Clementine-XT-0.9.7-mpc-armv7.zip", "plugins/clementine-xt/thumb.png",
            "Ten-voice wavetable synthesizer modelled exactly on the Microwave II/XT with a ten-page touchscreen skin and Q-Links. Needs your own ROM files in its ROMS folder.", "./plugins/clementine-xt/install.sh", 0,
            false,
            false,
            false,
            { "wavetable", "polyphonic", "arpeggiator" }
        },
        {
            "op1fx-cluster", "Cluster", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "161", "3.5 MB", "14%",
            "bombest - VST - Cluster", "cluster.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/cluster.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cluster-0.4.1-mpc-armv7.zip", "plugins/op1fx-cluster/thumb.png",
            "Eight self-feedback sine oscillators that drift apart over a note. OP-1 inspired.", "./plugins/op1fx-cluster/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "feedback", "drone" }
        },
        {
            "crate-digger", "Crate Digger", "sd88me", "v1.3.1", "Sample Browser",
            "INST", "Stable", "MPC Live III (Gen2) (az01 base 5.0.17), Akai Force (az01 base 5.0.17)", "MPC OS 2.x & 3.x", "2026-11-06",
            "9407", "67.8 MB", "3%",
            "sd88me - VST - Crate Digger", "cratedigger.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-cratedigger/main/docs/img/crate-digger.png", "https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v1.3.1/Crate-Digger-1.3.1-mpc-armv7.zip", "plugins/crate-digger/thumb.png",
            "Dig for records by genre, style, decade, region and country on Discogs, then stream and play them into an MPC track, with its own screen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger\" && if [ -f \"./plugins/crate-digger/cratedigger.so\" ]; then cp -rf ./plugins/crate-digger/* \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v1.3.1/Crate-Digger-1.3.1-mpc-armv7.zip\" \"/tmp/crate-digger.zip\") && unzip -o /tmp/crate-digger.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger/\" && rm -f /tmp/crate-digger.zip; fi && sync", 1,
            false,
            false,
            false,
            { "discogs", "streaming" }
        },
        {
            "tr-cw78", "CW-78", "athousanddetails", "v1.2.0", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 2.x & 3.x", "2026-11-02",
            "2495", "6.9 MB", "8%",
            "athousanddetails - VST - CW-78", "trcw78.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/cw78.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/cw78-vst-v1.2.0/CW-78-1.2.0-mpc-armv7.zip", "plugins/tr-cw78/thumb.png",
            "MPC/Force port of athousanddetails' schwung-CW-78 CR-78 drum machine: voices modelled from the 1979 service notes, with the preset rhythms. Touchscreen skin and Q-Links.", "./plugins/tr-cw78/install.sh", 0,
            false,
            false,
            false,
            { "cr-78" }
        },
        {
            "op1fx-dbox", "DBox", "Bombest", "v0.4.1", "Drum Machine",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "147", "8.6 MB", "14%",
            "bombest - VST - DBox", "dbox.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dbox.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DBox-0.4.1-mpc-armv7.zip", "plugins/op1fx-dbox/thumb.png",
            "A 24-key synthesized drum kit: two oscillators and noise per hit, 12 voices. OP-1 inspired.", "./plugins/op1fx-dbox/install.sh", 0,
            false,
            false,
            false,
            { "op1", "drums", "synth", "kit" }
        },
        {
            "denis", "Denis", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1475", "5.4 MB", "14%",
            "Filliformes - VST - [SYN] Denis", "denis.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-denis/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-denis/releases/download/v1.1.1/SYN-Denis-1.1.1-mpc-armv7.zip", "plugins/denis/thumb.png",
            "West Coast / Serge-inspired mono synth: complex oscillators, a wavefolder and a modulation matrix.", "./plugins/denis/install.sh", 0,
            false,
            false,
            false,
            { "west-coast", "serge", "wavefolder", "monophonic" }
        },
        {
            "dexed-dx7", "Dexed", "sd88me", "v1.0.7", "Fm Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "6479", "3.7 MB", "14%",
            "sd88me - VST - Dexed (DX7)", "dx7_dexed.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-dx7/main/docs/img/dexed-dx7.png", "https://github.com/sd88me/mpc-vst-dx7/releases/download/dx7-v1.0.7/Dexed-DX7-1.0.7-mpc-armv7.zip", "plugins/dexed-dx7/thumb.png",
            "DX7-style 6-operator FM synth (Dexed/MSFA engine) as a native instrument, with .syx bank browsing, all 32 algorithm diagrams and a touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Dexed (DX7)\" && if [ -f \"./plugins/dexed-dx7/dx7_dexed.so\" ]; then cp -rf ./plugins/dexed-dx7/* \"$TARGET_SYNTH_DIR/sd88me - VST - Dexed (DX7)/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-dx7/releases/download/dx7-v1.0.7/Dexed-DX7-1.0.7-mpc-armv7.zip\" \"/tmp/dexed-dx7.zip\") && unzip -o /tmp/dexed-dx7.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Dexed (DX7)/\" && rm -f /tmp/dexed-dx7.zip; fi && sync", 1,
            false,
            false,
            false,
            { "dx7", "emulation" }
        },
        {
            "op1fx-digital", "Digital", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "144", "5.7 MB", "14%",
            "bombest - VST - Digital", "digital.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/digital.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Digital-0.4.1-mpc-armv7.zip", "plugins/op1fx-digital/thumb.png",
            "Feedback sine oscillator, a second sine with sync, and a bit-flipping XOR stage. Eight voices. OP-1 inspired.", "./plugins/op1fx-digital/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "feedback", "xor" }
        },
        {
            "dinxxl", "DINXXL", "L'Cronx", "v1.0.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "224", "4.5 MB", "14%",
            "L'Cronx - VST - DINXXL", "dinxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/dinxxl/v1.0.0/docs/img/keys.png", "https://github.com/sunskiefer/DINXXL/releases/download/v1.0.0/DINXXL-1.0.0-mpc-armv7.zip", "plugins/dinxxl/thumb.png",
            "BlastBeats-style OPL3 FM groovebox: the published v7.1 firmware by Twisted Electrons runs unmodified on a software model of its board (YMF262 via ymfm). 6 drums, 4 four-op synths, 64-step sequencer with p-locks and songs, factory card inside. By L'Cronx; not made or endorsed by Twisted Electrons.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - DINXXL\" && if [ -f \"./plugins/dinxxl/dinxxl.so\" ]; then cp -rf ./plugins/dinxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - DINXXL/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/DINXXL/releases/download/v1.0.0/DINXXL-1.0.0-mpc-armv7.zip\" \"/tmp/dinxxl.zip\") && unzip -o /tmp/dinxxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - DINXXL/\" && rm -f /tmp/dinxxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "blastbeats", "opl3", "fm", "drum-machine", "groovebox", "step-sequencer", "chiptune", "adlib" }
        },
        {
            "op1fx-dna", "DNA", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "161", "4.7 MB", "14%",
            "bombest - VST - DNA", "dna.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dna.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DNA-0.4.1-mpc-armv7.zip", "plugins/op1fx-dna/thumb.png",
            "A 128-bit genome played as a one-bit wave through an anti-aliased reader. Eight voices. OP-1 inspired.", "./plugins/op1fx-dna/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "1-bit", "wavetable" }
        },
        {
            "drloguexxl", "DrLogueXXL", "L'Cronx", "v0.2.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "546", "7.8 MB", "14%",
            "L'Cronx - VST - DrLogueXXL", "drloguexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/DrLogueXXL/v0.2.0/docs/img/perform.png", "https://github.com/sunskiefer/DrLogueXXL/releases/download/v0.2.1/DrLogueXXL-0.2.1-mpc-armv7.zip", "plugins/drloguexxl/thumb.png",
            "Hybrid drum machine modelled on the KORG drumlogue (not affiliated with KORG): analogue-style kick, snare and toms, six sample parts with 192 synthesised sounds and your own WAVs, a MULTI part with VPM, noise and six open logue SDK units, delay, reverb and master effects, choke groups, a mixer and four macros, on a panel in the machine's look.", "./plugins/drloguexxl/install.sh", 0,
            false,
            false,
            false,
            { "drums", "analog", "hybrid", "samples", "fm", "logue", "drumlogue", "mixer" }
        },
        {
            "dronexxl", "DroneXXL", "L'Cronx", "v0.1.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-25",
            "417", "83.7 MB", "14%",
            "L'Cronx - VST - DroneXXL", "dronexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/DroneXXL/v0.1.0/docs/img/overview.png", "https://github.com/sunskiefer/DroneXXL/releases/download/v0.1.0/DroneXXL-0.1.0-mpc-armv7.zip", "plugins/dronexxl/thumb.png",
            "Drone and experimental synth, after the Solar 42, built on Neil Baumberger's AmbientModules: a 15-module rack (4 drones, mixer, filter, pads, 2 PapaSrapa, 2 VCOs, LFOs, joystick, sequencer, detector) patched by touch, with 8 macros, an XY pad, a randomizer that never repeats and a brickwall limiter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - DroneXXL\" && if [ -f \"./plugins/dronexxl/dronexxl.so\" ]; then cp -rf ./plugins/dronexxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - DroneXXL/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/DroneXXL/releases/download/v0.1.0/DroneXXL-0.1.0-mpc-armv7.zip\" \"/tmp/dronexxl.zip\") && unzip -o /tmp/dronexxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - DroneXXL/\" && rm -f /tmp/dronexxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drone", "experimental", "modular", "patch-panel", "ambient", "randomizer", "vcv-rack" }
        },
        {
            "op1fx-drwave", "DrWave", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "155", "6.7 MB", "14%",
            "bombest - VST - DrWave", "drwave.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/drwave.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DrWave-0.4.1-mpc-armv7.zip", "plugins/op1fx-drwave/thumb.png",
            "Wave, blend, motion and fold controls over eight voices. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - DrWave\" && if [ -f \"./plugins/op1fx-drwave/drwave.so\" ]; then cp -rf ./plugins/op1fx-drwave/* \"$TARGET_SYNTH_DIR/bombest - VST - DrWave/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DrWave-0.4.1-mpc-armv7.zip\" \"/tmp/op1fx-drwave.zip\") && unzip -o /tmp/op1fx-drwave.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - DrWave/\" && rm -f /tmp/op1fx-drwave.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "synth", "wavetable", "fold" }
        },
        {
            "op1fx-dsynth", "DSynth", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "149", "4.8 MB", "14%",
            "bombest - VST - DSynth", "dsynth.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dsynth.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DSynth-0.4.1-mpc-armv7.zip", "plugins/op1fx-dsynth/thumb.png",
            "Two oscillators with FM, a pitch drop for drum thump and an eight-mode filter. OP-1 inspired.", "./plugins/op1fx-dsynth/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "fm", "drums" }
        },
        {
            "dub-force-siren", "Dub Force Siren", "no3z", "v2.1.0", "Synth",
            "INST", "Stable", "Akai Force (3.9.1.2)", "MPC OS 3.x", "2026-10-26",
            "1157", "117 KB", "14%",
            "Dub Force - VST - Dub Force Siren", "dub_force_siren.so",
            "https://raw.githubusercontent.com/no3z/mpc-dubsiren/v2.0.0/resources/preview.png", "https://github.com/no3z/mpc-dubsiren/releases/download/v2.1.0/Dub-Force-Siren-2.1.0-mpc-armv7.zip", "plugins/dub-force-siren/thumb.png",
            "Dub siren with tape echo: siren and zap modes, LFO, filter and ping-pong delay on one page mirroring two banks of eight Q-Links.", "./plugins/dub-force-siren/install.sh", 0,
            false,
            false,
            false,
            { "dub", "siren", "echo", "sfx" }
        },
        {
            "evil-tuner-xxl", "Evil Tuner XXL", "L'Cronx", "v0.2.0", "Radio Granular",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "1166", "12.8 MB", "14%",
            "L'Cronx - VST - Evil Tuner XXL", "evil_tuner_xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Evil-Tuner-XXL/main/docs/img/RADIO.png", "https://github.com/sunskiefer/Evil-Tuner-XXL/releases/download/v0.2.0/Evil-Tuner-XXL-0.2.0-mpc-armv7.zip", "plugins/evil-tuner-xxl/thumb.png",
            "Internet radio (30,000+ stations), or a phone over Wi-Fi or AirPlay, played into an MPC track through a granular engine, filter, character stages, delay, reverb, trance gate, stutter, tempo-synced looper and three LFOs. Inspired by the Endorphin.es Evil Pet (no affiliation, no code). Needs a network.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - Evil Tuner XXL\" && if [ -f \"./plugins/evil-tuner-xxl/evil_tuner_xxl.so\" ]; then cp -rf ./plugins/evil-tuner-xxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - Evil Tuner XXL/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/Evil-Tuner-XXL/releases/download/v0.2.0/Evil-Tuner-XXL-0.2.0-mpc-armv7.zip\" \"/tmp/evil-tuner-xxl.zip\") && unzip -o /tmp/evil-tuner-xxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - Evil Tuner XXL/\" && rm -f /tmp/evil-tuner-xxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "radio", "internet-radio", "granular", "airplay", "looper", "stutter", "reverb", "sampling" }
        },
        {
            "fizzik", "Fizzik", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1461", "1.8 MB", "14%",
            "Filliformes - VST - [SYN] Fizzik", "fizzik.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-fizzik/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-fizzik/releases/download/v1.0.2/SYN-Fizzik-1.0.2-mpc-armv7.zip", "plugins/fizzik/thumb.png",
            "Physical modelling: an exciter and two coupled resonators (string, beam, plate, membrane).", "./plugins/fizzik/install.sh", 0,
            false,
            false,
            false,
            { "physical-modelling", "resonator", "percussion" }
        },
        {
            "op1fx-fm", "Fm", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "154", "8.3 MB", "14%",
            "bombest - VST - Fm", "fm.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/fm.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Fm-0.4.1-mpc-armv7.zip", "plugins/op1fx-fm/thumb.png",
            "Four-operator FM synth with eight voices and four screen knobs. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Fm\" && if [ -f \"./plugins/op1fx-fm/fm.so\" ]; then cp -rf ./plugins/op1fx-fm/* \"$TARGET_SYNTH_DIR/bombest - VST - Fm/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Fm-0.4.1-mpc-armv7.zip\" \"/tmp/op1fx-fm.zip\") && unzip -o /tmp/op1fx-fm.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Fm/\" && rm -f /tmp/op1fx-fm.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "synth", "fm", "operators" }
        },
        {
            "fretty", "Fretty", "3dthc", "v0.8.5", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "1297", "69.8 MB", "14%",
            "3dthc - VST - Fretty", "fretty.so",
            "https://raw.githubusercontent.com/3dthc/fretty-mpc/main/docs/images/play_sunburst.png", "https://github.com/3dthc/fretty-mpc/releases/download/v0.8.5/Fretty-0.8.5-mpc-armv7.zip", "plugins/fretty/thumb.png",
            "A guitar instrument for Akai MPC OS standalone devices (native VST2), featuring refined selector typography, 110 presets, and a synth layer.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - Fretty\" && if [ -f \"./plugins/fretty/fretty.so\" ]; then cp -rf ./plugins/fretty/* \"$TARGET_SYNTH_DIR/3dthc - VST - Fretty/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/fretty-mpc/releases/download/v0.8.5/Fretty-0.8.5-mpc-armv7.zip\" \"/tmp/fretty.zip\") && unzip -o /tmp/fretty.zip -d \"$TARGET_SYNTH_DIR/3dthc - VST - Fretty/\" && rm -f /tmp/fretty.zip; fi && sync", 1,
            false,
            false,
            false,
            {  }
        },
        {
            "gaiaxxl", "GAIAXXL", "L'Cronx", "v0.1.3", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "967", "7.7 MB", "14%",
            "L'Cronx - VST - GAIAXXL", "gaiaxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/GAIAXXL/v0.1.2/docs/img/tone.png", "https://github.com/sunskiefer/GAIAXXL/releases/download/v0.1.3/GAIAXXL-0.1.3-mpc-armv7.zip", "plugins/gaiaxxl/thumb.png",
            "Three-tone virtual-analog synth modelled on the discontinued Roland GAIA SH-01 (not affiliated with Roland): the editor's parameters over six full-screen pages, 21 oscillator variations (with KORG logue-sdk wave tables), SYNC/RING, the GAIA's effects chain and arpeggiator, two beams, a musical randomizer and 64 patches.", "./plugins/gaiaxxl/install.sh", 0,
            false,
            false,
            false,
            { "synth", "virtual-analog", "supersaw", "gaia", "arpeggiator", "pad", "lead", "randomizer" }
        },
        {
            "hank", "Hank", "saustin2010", "v1.1.1", "Fm Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1179", "2.1 MB", "14%",
            "Charles Vestal - VST - [SYN] Hank", "hank.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-hank/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-hank/releases/download/v1.1.1/SYN-Hank-1.1.1-mpc-armv7.zip", "plugins/hank/thumb.png",
            "Small, immediate two-operator FM synth on one page: ratio, brightness, bite, envelopes, noise; 32 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Hank\" && if [ -f \"./plugins/hank/hank.so\" ]; then cp -rf ./plugins/hank/* \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Hank/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-hank/releases/download/v1.1.1/SYN-Hank-1.1.1-mpc-armv7.zip\" \"/tmp/hank.zip\") && unzip -o /tmp/hank.zip -d \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Hank/\" && rm -f /tmp/hank.zip; fi && sync", 1,
            false,
            false,
            false,
            { "fm", "two-operator", "polyphonic", "bass" }
        },
        {
            "helm", "Helm", "Lewinator56", "v1.0.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-04",
            "1143", "12.3 MB", "14%",
            "Matt Tytel - VST - Helm", "helm.so",
            "https://github.com/user-attachments/assets/b537127a-22e6-4e21-b9f1-48e473c892b8", "https://github.com/Lewinator56/mpc-vst-helm/releases/download/1.0.0/Helm-1.0.0-mpc-armv7.zip", "plugins/helm/thumb.png",
            "32-voice polyphonic synthesizer (Helm engine by Matt Tytel) with 2 cross-mod oscillators, sub, noise, multi-mode filter, 3 LFOs, 3 envelopes, 32-step sequencer & arpeggiator, built-in effects and touchscreen skin with Q-Links.", "./plugins/helm/install.sh", 0,
            false,
            false,
            false,
            { "polyphonic", "subtractive", "arpeggiator", "step-sequencer", "mod-matrix" }
        },
        {
            "hera", "Hera", "saustin2010", "v1.1.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "2361", "3.3 MB", "14%",
            "jpcima - VST - [SYN] Hera", "hera.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-hera/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-hera/releases/download/v1.1.2/SYN-Hera-1.1.2-mpc-armv7.zip", "plugins/hera/thumb.png",
            "Roland Juno-60: jpcima's Hera engine with its 56 presets, the BBD chorus and a touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jpcima - VST - [SYN] Hera\" && if [ -f \"./plugins/hera/hera.so\" ]; then cp -rf ./plugins/hera/* \"$TARGET_SYNTH_DIR/jpcima - VST - [SYN] Hera/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-hera/releases/download/v1.1.2/SYN-Hera-1.1.2-mpc-armv7.zip\" \"/tmp/hera.zip\") && unzip -o /tmp/hera.zip -d \"$TARGET_SYNTH_DIR/jpcima - VST - [SYN] Hera/\" && rm -f /tmp/hera.zip; fi && sync", 1,
            false,
            false,
            false,
            { "juno-60", "polyphonic", "chorus", "analog-modelling" }
        },
        {
            "hexachord-73", "HexChord 73", "Eurikon", "v1.0.1", "Synth",
            "INST", "Stable", "MPC Live II Retro (3.10)", "MPC OS 3.x", "2026-11-06",
            "309", "2.9 MB", "23%",
            "Twisted Cable - VST - HexChord 73", "hexachord-73.so",
            "https://raw.githubusercontent.com/Eurikon/mpc-vst-plugins/9eed16e63f97e5ad87621a21cead85b6a7fee35b/media/hexachord-73.png", "https://github.com/Eurikon/mpc-vst-plugins/releases/download/hexachord-73-v1.0.1/HexChord-73-1.0.1-mpc-armv7.zip", "plugins/hexachord-73/thumb.png",
            "Stereo chord synthesizer with four engines, eight chords, four voicings and a touchscreen chord diagram.", "./plugins/hexachord-73/install.sh", 0,
            false,
            false,
            false,
            { "chord", "polyphonic", "stereo" }
        },
        {
            "incipience", "Incipience", "saustin2010", "v1.1.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "872", "7.0 MB", "14%",
            "Steve A - VST - [DRUM] Incipience", "incipience.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-incipience/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-incipience/releases/download/v1.1.1/DRUM-Incipience-1.1.1-mpc-armv7.zip", "plugins/incipience/thumb.png",
            "Eleven-instrument drum synth laid out like the Jomox AlphaBase Mk II (kick, MBrane, hats, clap, rim, cymbals, two sample players, 4-op FM, reverb, delay), with RANDOMISE per instrument and a mod matrix; each instrument chromatic on its own MIDI channel and on its own track with Incipience OUT; plays the AlphaBase's free samples and your own WAVs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Incipience\" && if [ -f \"./plugins/incipience/incipience.so\" ]; then cp -rf ./plugins/incipience/* \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Incipience/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-incipience/releases/download/v1.1.1/DRUM-Incipience-1.1.1-mpc-armv7.zip\" \"/tmp/incipience.zip\") && unzip -o /tmp/incipience.zip -d \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Incipience/\" && rm -f /tmp/incipience.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drums", "alphabase", "fm", "samples", "multi-out", "mod-matrix" }
        },
        {
            "interzonexxl", "InterzoneXXL", "L'Cronx", "v0.1.3", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "1752", "13.6 MB", "14%",
            "L'Cronx - VST - InterzoneXXL", "interzonexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/InterzoneXXL/v0.1.0/docs/img/vco.png", "https://github.com/sunskiefer/InterzoneXXL/releases/download/v0.1.3/InterzoneXXL-0.1.3-mpc-armv7.zip", "plugins/interzonexxl/thumb.png",
            "Valley Audio's Interzone synth voice from VCV Rack, played from MIDI in mono or up to 16-voice poly on the module's own panel. Every input jack is patched to a built-in source (LFOs, sequencers, random) on the MPC tempo, with a brickwall limiter and 16 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - InterzoneXXL\" && if [ -f \"./plugins/interzonexxl/interzonexxl.so\" ]; then cp -rf ./plugins/interzonexxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - InterzoneXXL/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/InterzoneXXL/releases/download/v0.1.3/InterzoneXXL-0.1.3-mpc-armv7.zip\" \"/tmp/interzonexxl.zip\") && unzip -o /tmp/interzonexxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - InterzoneXXL/\" && rm -f /tmp/interzonexxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "synth", "monosynth", "polysynth", "analog", "vcv", "valley", "lfo", "modulation" }
        },
        {
            "j106", "JN-106", "sd88me", "v0.2.0", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), Akai MPC Live III (aarch64) (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-28",
            "805", "25.3 MB", "14%",
            "sd88me - VST - JN-106", "kr106.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-jn106/9053529f679d2a5f07781f002004e27cc12959e0/docs/img/screenshot-lfo-dco.png", "https://github.com/sd88me/mpc-vst-jn106/releases/download/jn-106-vst-v0.2.0/JN-106-0.2.0-mpc-armv7.zip", "plugins/j106/thumb.png",
            "Faithful, circuit-modelled and hardware-profiled six-voice Juno emulation (Ultramaster KR-106 DSP): IR3109 filter, BA662 VCA and MN3009 BBD chorus modelled from the circuits, the 106 CPU firmware reimplemented from its disassembly, curves fitted to real hardware and checked against the service notes. 256 factory presets, realistic panel skin.", "./plugins/j106/install.sh", 0,
            false,
            false,
            false,
            { "analog", "poly", "vintage", "chorus" }
        },
        {
            "kutilxxl", "KutilXXL", "L'Cronx", "v0.2.1", "Groovebox",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "317", "85.3 MB", "14%",
            "L'Cronx - VST - KutilXXL", "kutilxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/BUSTEDXXL/v0.2.1/docs/img/screen1.png", "https://github.com/sunskiefer/BUSTEDXXL/releases/download/v0.2.1/KutilXXL-0.2.1-mpc-armv7.zip", "plugins/kutilxxl/thumb.png",
            "Groovebox of Bastl-style modules patched with touch cables across three screens: Wave Bard on Bastl's open-source Kastle 2 DSP plus Kompas, Crust 2, Pizza, Alchemist, Basil and ABC. Tap a module to open a window with just its jacks, big and named, and patch between modules.", "./plugins/kutilxxl/install.sh", 0,
            false,
            false,
            false,
            { "bastl", "kastle", "modular", "patch-panel", "touch-cables", "sampler", "fm", "groovebox" }
        },
        {
            "lucky-dip", "Lucky Dip", "sd88me", "v0.9.2", "Sampler",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 2.x & 3.x", "2026-10-21",
            "1865", "2.0 MB", "14%",
            "sd88me - VST - Lucky Dip", "lucky_dip.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-luckydip/main/docs/img/lucky-dip.png", "https://github.com/sd88me/mpc-vst-luckydip/releases/download/v0.9.2/Lucky-Dip-0.9.2-mpc-armv7.zip", "plugins/lucky-dip/thumb.png",
            "A random 16-pad drum-kit builder and inspiration machine: dips into your sample library, fills the pads at random, plays the kit and exports it as an MPC drum program.", "./plugins/lucky-dip/install.sh", 0,
            false,
            false,
            false,
            { "sampler", "utility", "kit-builder", "random", "drum-kit" }
        },
        {
            "lxrxxl", "LXRXXL", "L'Cronx", "v0.6.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3713", "10.2 MB", "14%",
            "L'Cronx - VST - LXRXXL", "lxrxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/LXR02XXL/v0.6.0/docs/img/page.png", "https://github.com/sunskiefer/LXR02XXL/releases/download/v0.6.1/LXRXXL-0.6.1-mpc-armv7.zip", "plugins/lxrxxl/thumb.png",
            "Inspired by the LXR-02: its open-source sound engine (GPL-3.0) with the real FX unit and the plate reverb, the whole panel on one page, 64-step sequencer with songs, rolls and a euclidean generator, Force pads playing the voices, randomizers and the Hrtl LXR-02 DLC presets old and new. Artwork by Suns Kiefer.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL\" && if [ -f \"./plugins/lxrxxl/lxrxxl.so\" ]; then cp -rf ./plugins/lxrxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/LXR02XXL/releases/download/v0.6.1/LXRXXL-0.6.1-mpc-armv7.zip\" \"/tmp/lxrxxl.zip\") && unzip -o /tmp/lxrxxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL/\" && rm -f /tmp/lxrxxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drums", "drum-machine", "lxr", "lxr-02", "erica-synths", "sequencer", "reverb", "randomizer" }
        },
        {
            "lxrxxl-bazzline", "LXRXXL BAZZLINE", "L'Cronx", "v0.2.10", "Bass Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "3365", "16.2 MB", "14%",
            "L'Cronx - VST - LXRXXL BAZZLINE", "lxrxxl_bazzline.so",
            "https://raw.githubusercontent.com/sunskiefer/LXRXXL-BAZZLINE/v0.2.2/docs/img/panel.png", "https://github.com/sunskiefer/LXRXXL-BAZZLINE/releases/download/v0.2.10/LXRXXL-BAZZLINE-0.2.10-mpc-armv7.zip", "plugins/lxrxxl-bazzline/thumb.png",
            "An 8-track bass synth: a variation of LXRXXL with bass voices inspired by the Erica Synths Bassline DB-01 on the LXR's oscillators, the LXR-02 style sequencer with the DB-01's step functions, a tape/digital delay, a MIDI gate/duck, an 8-track mixer and sound / performance presets saved on the device.", "./plugins/lxrxxl-bazzline/install.sh", 0,
            false,
            false,
            false,
            { "bass", "acid", "303", "bassline", "db-01", "lxr", "sequencer", "delay" }
        },
        {
            "lxrxxl-zynth", "LXRXXL ZYNTH", "L'Cronx", "v0.2.2", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "1332", "22.4 MB", "14%",
            "L'Cronx - VST - LXRXXL ZYNTH", "lxrxxl_zynth.so",
            "https://raw.githubusercontent.com/sunskiefer/LXRXXL-ZYNTH/v0.2.0/docs/img/panel.png", "https://github.com/sunskiefer/LXRXXL-ZYNTH/releases/download/v0.2.2/LXRXXL-ZYNTH-0.2.2-mpc-armv7.zip", "plugins/lxrxxl-zynth/thumb.png",
            "An experimental 8-track synth with 19 voices on unconventional maths (Lorenz chaos, Hilbert frequency shifter, Euclidean rhythms) and voices coded after the DX7, CZ, Minimoog, Hammond, NES chips, Braids and Plaits. LXR-02 style sequencer with random functions, tape/digital delay, mixer and presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL ZYNTH\" && if [ -f \"./plugins/lxrxxl-zynth/lxrxxl_zynth.so\" ]; then cp -rf ./plugins/lxrxxl-zynth/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL ZYNTH/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/LXRXXL-ZYNTH/releases/download/v0.2.2/LXRXXL-ZYNTH-0.2.2-mpc-armv7.zip\" \"/tmp/lxrxxl-zynth.zip\") && unzip -o /tmp/lxrxxl-zynth.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL ZYNTH/\" && rm -f /tmp/lxrxxl-zynth.zip; fi && sync", 1,
            false,
            false,
            false,
            { "synth", "groovebox", "fm", "wavetable", "sequencer", "experimental", "chaos", "lxr" }
        },
        {
            "machinemodule", "Machinemodule", "sd88me", "v1.0.3", "Drum Machine",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "100", "2.0 MB", "14%",
            "sd88me - VST - Machinemodule", "machinemodule.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule.png", "", "plugins/machinemodule/thumb.png",
            "The Elektron Machinedrum SPS-1 UW sound engine as a native instrument: all 16 tracks from one plugin instance, with its own touchscreen skin and Q-Links.", "./plugins/machinemodule/install.sh", 0,
            false,
            false,
            false,
            { "elektron", "machinedrum", "firmware-derived" }
        },
        {
            "machinemodule-gen2", "Machinemodule (Gen2)", "sd88me", "v1.0.3", "Drum Machine",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-04",
            "549", "7.7 MB", "14%",
            "sd88me - VST - Machinemodule", "machinedrum_one.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule-gen2.png", "https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.3/Machinemodule-1.0.3-mpc-aarch64.zip", "plugins/machinemodule-gen2/thumb.png",
            "The Elektron Machinedrum UW sound engine as an instrument for Gen2 MPC devices: 16 tracks in one plugin, touchscreen skin, bank and kit lists, Q-Links. A ready-made download with nothing of Elektron's in it: you add your own OS file (and optionally your flash image) in the plugin folder. Gen2 only.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule\" && if [ -f \"./plugins/machinemodule-gen2/machinedrum_one.so\" ]; then cp -rf ./plugins/machinemodule-gen2/* \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.3/Machinemodule-1.0.3-mpc-aarch64.zip\" \"/tmp/machinemodule-gen2.zip\") && unzip -o /tmp/machinemodule-gen2.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule/\" && rm -f /tmp/machinemodule-gen2.zip; fi && sync", 1,
            false,
            false,
            false,
            { "elektron", "machinedrum", "gen2" }
        },
        {
            "braids", "Macro Osc", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1974", "8.0 MB", "14%",
            "Émilie Gillet - VST - [SYN] Macro Osc", "braids.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-macro-osc/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-macro-osc/releases/download/v1.1.1/SYN-Macro-Osc-1.1.1-mpc-armv7.zip", "plugins/braids/thumb.png",
            "Macro Osc: a macro oscillator with 47 synthesis models, from Émilie Gillet's open-source Braids.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Macro Osc\" && if [ -f \"./plugins/braids/braids.so\" ]; then cp -rf ./plugins/braids/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Macro Osc/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-macro-osc/releases/download/v1.1.1/SYN-Macro-Osc-1.1.1-mpc-armv7.zip\" \"/tmp/braids.zip\") && unzip -o /tmp/braids.zip -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Macro Osc/\" && rm -f /tmp/braids.zip; fi && sync", 1,
            false,
            false,
            false,
            { "macro-oscillator", "eurorack" }
        },
        {
            "maschine-group", "Maschine Group", "mmiroshnikov", "v1.0.0", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-13",
            "547", "169 KB", "14%",
            "mmiroshnikov - VST - Maschine Group", "maschine_group.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-maschine-group/main/docs/group.png", "https://github.com/mmiroshnikov/mpc-maschine-group/releases/download/maschine-group-v1.0.0/Maschine-Group-1.0.0-mpc-armv7.zip", "plugins/maschine-group/thumb.png",
            "Opens a Native Instruments Maschine 2 drum group (.mxgrp) and plays its pads and stored patterns, with a three-page touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/mmiroshnikov - VST - Maschine Group\" && if [ -f \"./plugins/maschine-group/maschine_group.so\" ]; then cp -rf ./plugins/maschine-group/* \"$TARGET_SYNTH_DIR/mmiroshnikov - VST - Maschine Group/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-maschine-group/releases/download/maschine-group-v1.0.0/Maschine-Group-1.0.0-mpc-armv7.zip\" \"/tmp/maschine-group.zip\") && unzip -o /tmp/maschine-group.zip -d \"$TARGET_SYNTH_DIR/mmiroshnikov - VST - Maschine Group/\" && rm -f /tmp/maschine-group.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drums", "maschine", "sampler", "patterns" }
        },
        {
            "maze-voice", "Maze Voice", "sd88me", "v1.0.5", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "3693", "4.2 MB", "14%",
            "sd88me - VST - Maze Voice", "maze_voice.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-maze/main/docs/img/maze-voice.png", "https://github.com/sd88me/mpc-vst-maze/releases/download/maze-voice-vst-v1.0.5/Maze-Voice-1.0.5-mpc-armv7.zip", "plugins/maze-voice/thumb.png",
            "Monophonic Moog Labyrinth-style thru-zero oscillator, wavefolder and state-variable filter voice with two LFOs, a page randomiser and a three-page touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Voice\" && if [ -f \"./plugins/maze-voice/maze_voice.so\" ]; then cp -rf ./plugins/maze-voice/* \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Voice/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-maze/releases/download/maze-voice-vst-v1.0.5/Maze-Voice-1.0.5-mpc-armv7.zip\" \"/tmp/maze-voice.zip\") && unzip -o /tmp/maze-voice.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Voice/\" && rm -f /tmp/maze-voice.zip; fi && sync", 1,
            false,
            false,
            false,
            { "monophonic", "wavefolder", "moog", "analog-modelling" }
        },
        {
            "meowsynth", "MeowSynth", "mcardonaserrano", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "1340", "4.5 MB", "14%",
            "knobster - VST - MeowSynth", "meowsynth.so",
            "https://raw.githubusercontent.com/mcardonaserrano/meowsynth-mpc/main/previews/meow_page_meowing.png", "https://github.com/mcardonaserrano/meowsynth-mpc/releases/download/v1.0.2/MeowSynth-1.0.2-mpc-armv7.zip", "plugins/meowsynth/thumb.png",
            "knobster's MeowSynth rebuilt natively from the original's SynthEdit patch: a ramp oscillator through an envelope-following resonant filter that meows, five cats, vibrato and chorus, with the original's look and a meowing cat on the touchscreen and Q-Links.", "./plugins/meowsynth/install.sh", 0,
            false,
            false,
            false,
            { "cat", "meow", "novelty", "synthedit" }
        },
        {
            "meta4mpcxxl", "Meta4MPCXXL", "L'Cronx", "v0.1.2", "Modular",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "449", "11.2 MB", "14%",
            "L'Cronx - VST - Meta4MPCXXL", "meta4mpcxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Meta4MPCXXL/v0.1.1/docs/img/meta4mpcxxl.png", "https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-0.1.2-mpc-armv7.zip", "plugins/meta4mpcxxl/thumb.png",
            "Plays 4ms MetaModule-format patches (VCV Rack patches exported with the MetaModule hub) with 4ms's own open-source patch player and 215 built-in modules: every parameter on Q-Link pages, MIDI and the Force's clock into the patch, pads and keys to the hub's gate/CV inputs, patch outs 3-8 as MIDI for a CV track, and Wi-Fi patch upload from VCV Rack.", "./plugins/meta4mpcxxl/install.sh", 0,
            false,
            false,
            false,
            { "modular", "vcv", "metamodule", "patch-player", "eurorack", "generative", "cv" }
        },
        {
            "jv-880", "Mini-JV", "sd88me", "v1.0.9", "Rompler",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 2.x & 3.x", "2026-11-01",
            "6412", "1.7 MB", "14%",
            "sd88me - VST - JV-880", "jv880.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-jv880/f24bd1c922f2e8b2cf56eb497c6e13465ef8dad3/docs/screenshots/jv880.png", "https://github.com/sd88me/mpc-vst-jv880/releases/download/v1.0.9/JV-880-1.0.9-mpc-armv7.zip", "plugins/jv-880/thumb.png",
            "An emulation of the Roland JV-880 sample-based sound engine as a native instrument, with the original reverb and chorus, a Play page with eight macros and a touchscreen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - JV-880\" && if [ -f \"./plugins/jv-880/jv880.so\" ]; then cp -rf ./plugins/jv-880/* \"$TARGET_SYNTH_DIR/sd88me - VST - JV-880/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-jv880/releases/download/v1.0.9/JV-880-1.0.9-mpc-armv7.zip\" \"/tmp/jv-880.zip\") && unzip -o /tmp/jv-880.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - JV-880/\" && rm -f /tmp/jv-880.zip; fi && sync", 1,
            false,
            false,
            false,
            { "roland", "emulation" }
        },
        {
            "elements", "Modal Voice", "saustin2010", "v1.2.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1830", "1.2 MB", "14%",
            "Émilie Gillet - VST - [SYN] Modal Voice", "elements.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-modal-voice/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-modal-voice/releases/download/v1.2.1/SYN-Modal-Voice-1.2.1-mpc-armv7.zip", "plugins/elements/thumb.png",
            "Modal Voice: a modal synthesis voice you bow, blow and strike, with 12 presets, from Émilie Gillet's open-source Elements.", "./plugins/elements/install.sh", 0,
            false,
            false,
            false,
            { "modal", "physical-modelling", "eurorack" }
        },
        {
            "model-72", "Model-72", "sd88me", "v1.0.1", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "940", "31.6 MB", "14%",
            "sd88me - VST - Model-72", "model72.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-model72/main/docs/img/mixer_filter.png", "https://github.com/sd88me/mpc-vst-model72/releases/download/model72-v1.0.1/Model-72-1.0.1-mpc-armv7.zip", "plugins/model-72/thumb.png",
            "A port of Idle Foundry's CA-72, a circuit-modelled 1970s Minimoog Model-D: three oscillators, the ladder filter and contours, mono, poly or unison with stereo and 19 presets. LO is the fitted light model; HI runs the circuit's own filter in real time.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Model-72\" && if [ -f \"./plugins/model-72/model72.so\" ]; then cp -rf ./plugins/model-72/* \"$TARGET_SYNTH_DIR/sd88me - VST - Model-72/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-model72/releases/download/model72-v1.0.1/Model-72-1.0.1-mpc-armv7.zip\" \"/tmp/model-72.zip\") && unzip -o /tmp/model-72.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Model-72/\" && rm -f /tmp/model-72.zip; fi && sync", 1,
            false,
            false,
            false,
            { "analog", "mono", "bass", "circuit-model", "stereo", "presets" }
        },
        {
            "monksynth", "MonkSynth", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1313", "1.6 MB", "14%",
            "Jonathan Taylor - VST - [SYN] MonkSynth", "monksynth.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-monksynth/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-monksynth/releases/download/v1.1.1/SYN-MonkSynth-1.1.1-mpc-armv7.zip", "plugins/monksynth/thumb.png",
            "A formant (FOF) singing voice with 12 characters and a choir.", "./plugins/monksynth/install.sh", 0,
            false,
            false,
            false,
            { "formant", "voice", "choir" }
        },
        {
            "mono-voice", "Mono Voice", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1481", "250 KB", "14%",
            "timncox - VST - [SYN] Mono Voice", "monovoice.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-monovoice/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-monovoice/releases/download/v1.0.2/SYN-Mono-Voice-1.0.2-mpc-armv7.zip", "plugins/mono-voice/thumb.png",
            "Elektron Monomachine-style digital voice: SuperWave, SID, DigiPRO, FM and more machines; 12 factory patches.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/timncox - VST - [SYN] Mono Voice\" && if [ -f \"./plugins/mono-voice/monovoice.so\" ]; then cp -rf ./plugins/mono-voice/* \"$TARGET_SYNTH_DIR/timncox - VST - [SYN] Mono Voice/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-monovoice/releases/download/v1.0.2/SYN-Mono-Voice-1.0.2-mpc-armv7.zip\" \"/tmp/mono-voice.zip\") && unzip -o /tmp/mono-voice.zip -d \"$TARGET_SYNTH_DIR/timncox - VST - [SYN] Mono Voice/\" && rm -f /tmp/mono-voice.zip; fi && sync", 1,
            false,
            false,
            false,
            { "monomachine", "sid", "fm", "digital" }
        },
        {
            "monomodule-one-gen2", "Monomodule One (Gen2)", "sd88me", "v1.0.4", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-04",
            "391", "4.2 MB", "14%",
            "sd88me - VST - Monomodule One", "monomodule_one.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-monomodule/main/docs/img/monomodule-gen2.png", "https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-One-1.0.4-mpc-aarch64.zip", "plugins/monomodule-one-gen2/thumb.png",
            "The Elektron Monomachine's synth engine (15 machines) as an instrument for Gen2 MPC devices, with a touchscreen skin, banks and preset lists, and Q-Links. A ready-made download: nothing of Elektron's is in the zip; you put your own Monomachine OS file in the plugin folder and it makes its skin and sound from that. Gen2 (aarch64) only.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule One\" && if [ -f \"./plugins/monomodule-one-gen2/monomodule_one.so\" ]; then cp -rf ./plugins/monomodule-one-gen2/* \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule One/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-One-1.0.4-mpc-aarch64.zip\" \"/tmp/monomodule-one-gen2.zip\") && unzip -o /tmp/monomodule-one-gen2.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule One/\" && rm -f /tmp/monomodule-one-gen2.zip; fi && sync", 1,
            false,
            false,
            false,
            { "elektron", "monomachine", "gen2" }
        },
        {
            "monomodule", "Monomodule One + FX", "sd88me", "v1.0.4", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "100", "2.0 MB", "14%",
            "sd88me - VST - Monomodule One + FX", "monomodule.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-monomodule/main/docs/img/monomodule.png", "", "plugins/monomodule/thumb.png",
            "The Elektron Monomachine's synth engine (15 machines) as an instrument, plus its 7 effect machines as an audio effect, each with a touchscreen skin and Q-Links.", "./plugins/monomodule/install.sh", 0,
            false,
            false,
            false,
            { "elektron", "monomachine", "firmware-derived" }
        },
        {
            "monoslide", "MonoSlide", "Eurikon", "v1.0.4", "Synth",
            "INST", "Stable", "MPC Live II Retro (3.10)", "MPC OS 3.x", "2026-11-06",
            "301", "4.3 MB", "11%",
            "Twisted Cable - VST - MonoSlide", "monoslide.so",
            "https://raw.githubusercontent.com/Eurikon/mpc-vst-plugins/0b9a58444c7d84a78e38d95818eeb53172195782/media/monoslide.png", "https://github.com/Eurikon/mpc-vst-plugins/releases/download/monoslide-v1.0.4/MonoSlide-1.0.4-mpc-armv7.zip", "plugins/monoslide/thumb.png",
            "Monophonic acid bass voice with overlapping-note slide, velocity accents and an MPC touchscreen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Twisted Cable - VST - MonoSlide\" && if [ -f \"./plugins/monoslide/monoslide.so\" ]; then cp -rf ./plugins/monoslide/* \"$TARGET_SYNTH_DIR/Twisted Cable - VST - MonoSlide/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Eurikon/mpc-vst-plugins/releases/download/monoslide-v1.0.4/MonoSlide-1.0.4-mpc-armv7.zip\" \"/tmp/monoslide.zip\") && unzip -o /tmp/monoslide.zip -d \"$TARGET_SYNTH_DIR/Twisted Cable - VST - MonoSlide/\" && rm -f /tmp/monoslide.zip; fi && sync", 1,
            false,
            false,
            false,
            { "303", "acid", "bass", "monophonic" }
        },
        {
            "morpho-pe", "Morpho-PE", "sd88me", "v1.0.2", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "1676", "2.2 MB", "14%",
            "sd88me - VST - Morpho-PE", "morpho_pe.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-morpho-PE/main/docs/img/morpho-pe.png", "https://github.com/sd88me/mpc-vst-morpho-PE/releases/download/morpho-pe-v1.0.2/Morpho-PE-1.0.2-mpc-armv7.zip", "plugins/morpho-pe/thumb.png",
            "Four-voice synthesizer modelled on the Poly Evolver's voice: analog-style and digital waveshape oscillators, 2/4-pole lowpass, delay, feedback, distortion, 4 x 16 step sequencer, a bank browser and a touchscreen skin with Q-Links. Loads Poly Evolver program dumps and, from your own Prophet VS ROM chip images, the 95 factory waves.", "./plugins/morpho-pe/install.sh", 0,
            false,
            false,
            false,
            { "analog-modelling", "polyphonic", "sequencer", "evolver" }
        },
        {
            "mpc-plaits", "MPC Plaits", "poloq", "v1.0.1", "Synth",
            "INST", "Stable", "Akai MPC One (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-05",
            "2176", "5.5 MB", "24%",
            "poloq - VST - MPC Plaits", "plaits.so",
            "https://raw.githubusercontent.com/poloq-instruments/mpc-vst-plaits/a30c24b83663a5efead73bbaca674ba4502a3eb5/docs/screenshots/card.png", "https://github.com/poloq-instruments/mpc-vst-plaits/releases/download/v1.0.1/MPC-Plaits-1.0.1-mpc-armv7.zip", "plugins/mpc-plaits/thumb.png",
            "Mutable Instruments Plaits, all 24 models, with LFOs, envelopes, two mod matrices, tempo sync and a touchscreen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/poloq - VST - MPC Plaits\" && if [ -f \"./plugins/mpc-plaits/plaits.so\" ]; then cp -rf ./plugins/mpc-plaits/* \"$TARGET_SYNTH_DIR/poloq - VST - MPC Plaits/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/poloq-instruments/mpc-vst-plaits/releases/download/v1.0.1/MPC-Plaits-1.0.1-mpc-armv7.zip\" \"/tmp/mpc-plaits.zip\") && unzip -o /tmp/mpc-plaits.zip -d \"$TARGET_SYNTH_DIR/poloq - VST - MPC Plaits/\" && rm -f /tmp/mpc-plaits.zip; fi && sync", 1,
            false,
            false,
            false,
            { "mutable-instruments", "plaits", "macro-oscillator", "eurorack" }
        },
        {
            "mr-drums", "Mr Drums", "saustin2010", "v1.1.1", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1214", "464 KB", "14%",
            "Move Everything - VST - [DRUM] Mr Drums", "mrdrums.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-mrdrums/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-mrdrums/releases/download/v1.1.1/DRUM-Mr-Drums-1.1.1-mpc-armv7.zip", "plugins/mr-drums/thumb.png",
            "A 16-pad drum sampler: kits of samples on pads 36-51.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Move Everything - VST - [DRUM] Mr Drums\" && if [ -f \"./plugins/mr-drums/mrdrums.so\" ]; then cp -rf ./plugins/mr-drums/* \"$TARGET_SYNTH_DIR/Move Everything - VST - [DRUM] Mr Drums/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-mrdrums/releases/download/v1.1.1/DRUM-Mr-Drums-1.1.1-mpc-armv7.zip\" \"/tmp/mr-drums.zip\") && unzip -o /tmp/mr-drums.zip -d \"$TARGET_SYNTH_DIR/Move Everything - VST - [DRUM] Mr Drums/\" && rm -f /tmp/mr-drums.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drums", "kits", "pads" }
        },
        {
            "mr-hyde", "Mr Hyde", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1725", "1.9 MB", "14%",
            "Move Everything - VST - [SYN] Mr Hyde", "mrhyde.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-mrhyde/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-mrhyde/releases/download/v1.1.1/SYN-Mr-Hyde-1.1.1-mpc-armv7.zip", "plugins/mr-hyde/thumb.png",
            "MicroFreak-style voice on Plaits' 17 models, with a low-pass gate, filter, cycling envelope and a 6x6 mod matrix; 19 presets.", "./plugins/mr-hyde/install.sh", 0,
            false,
            false,
            false,
            { "microfreak", "macro-oscillator", "mod-matrix", "low-pass-gate" }
        },
        {
            "ms20xxl", "MS20XXL", "L'Cronx", "v0.1.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-27",
            "748", "19.3 MB", "14%",
            "L'Cronx - VST - MS20XXL", "ms20xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/MS20XXL/v0.1.1/docs/img/patch.png", "https://github.com/sunskiefer/MS20XXL/releases/download/v0.1.2/MS20XXL-0.1.2-mpc-armv7.zip", "plugins/ms20xxl/thumb.png",
            "Korg MS-20-style semi-modular mono synth, circuit-modelled from the MS-20 schematics: the KLM-307 (MK2) and KORG35 (MK1) filter boards solved from their circuit equations, Hz/V sockets, the panel's voltage laws. Every MS-20 patch jack in its place, normalled like the hardware, patched by touch; an SQ-10-style sequencer; 63 patches with cables.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - MS20XXL\" && if [ -f \"./plugins/ms20xxl/ms20xxl.so\" ]; then cp -rf ./plugins/ms20xxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - MS20XXL/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/MS20XXL/releases/download/v0.1.2/MS20XXL-0.1.2-mpc-armv7.zip\" \"/tmp/ms20xxl.zip\") && unzip -o /tmp/ms20xxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - MS20XXL/\" && rm -f /tmp/ms20xxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "ms-20", "semi-modular", "mono", "patch-panel", "sequencer", "sq-10", "korg35", "analog" }
        },
        {
            "mutable-vibe", "Mutable Vibe MPC", "nachtaktiv303", "v0.9.3", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2506", "1.1 MB", "14%",
            "nachtaktiv303 - VST - Mutable Vibe MPC", "mutable_vibe_mpc.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-mutable-vibe/main/preview_0.png", "https://github.com/nachtaktiv303/mpc-vst-mutable-vibe/releases/download/mutable-vibe-vst-v0.9.3/Mutable-Vibe-MPC-0.9.3-mpc-armv7.zip", "plugins/mutable-vibe/thumb.png",
            "Mutable Instruments Rings + Plaits in one synth: 6 resonator models and 14 melodic macro-oscillator engines, with filter, FX, 4 envelopes, 4 LFOs, a mod matrix and tempo sync, plus a touchscreen skin and Q-Links.", "./plugins/mutable-vibe/install.sh", 0,
            false,
            false,
            false,
            { "mutable-instruments", "rings", "plaits", "resonator", "macro-oscillator", "eurorack" }
        },
        {
            "noisemaker", "Noisemaker", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1270", "5.3 MB", "14%",
            "TAL - VST - [SYN] Noisemaker", "noisemaker.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-noisemaker/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-noisemaker/releases/download/v1.1.1/SYN-Noisemaker-1.1.1-mpc-armv7.zip", "plugins/noisemaker/thumb.png",
            "TAL-NoiseMaker: a virtual-analog polysynth with 12 filter types, three envelopes and effects; all 256 factory presets built in, and folders of .noisemakerpreset files load as banks (the README links a free collection of 442).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/TAL - VST - [SYN] Noisemaker\" && if [ -f \"./plugins/noisemaker/noisemaker.so\" ]; then cp -rf ./plugins/noisemaker/* \"$TARGET_SYNTH_DIR/TAL - VST - [SYN] Noisemaker/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-noisemaker/releases/download/v1.1.1/SYN-Noisemaker-1.1.1-mpc-armv7.zip\" \"/tmp/noisemaker.zip\") && unzip -o /tmp/noisemaker.zip -d \"$TARGET_SYNTH_DIR/TAL - VST - [SYN] Noisemaker/\" && rm -f /tmp/noisemaker.zip; fi && sync", 1,
            false,
            false,
            false,
            { "tal", "virtual-analog", "polyphonic", "subtractive" }
        },
        {
            "nova-editor", "Nova Editor", "3dthc", "v0.7.0", "Editor",
            "INST", "Stable", "MPC Live II (3.10)", "MPC OS 2.x & 3.x", "2026-11-03",
            "30", "1.7 MB", "14%",
            "3dthc - VST - Nova Editor", "nova_editor.so",
            "https://raw.githubusercontent.com/3dthc/nova-editor/main/nova-editor-main.png", "https://github.com/3dthc/Nova-Editor/releases/download/nova-editor-v0.7.0/Nova-Editor-0.7.0-mpc-armv7.zip", "plugins/nova-editor/thumb.png",
            "An editor and librarian for the Novation MiniNova as a native plugin, with its own MPC screen skin: every control maps to a MiniNova CC/NRPN, controls follow the synth, banks A-C and imported .syx files live on the SD card. Also a SysEx librarian for any other synth. With nova-audio loaded, the MiniNova plays on the plugin's own track.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - Nova Editor\" && if [ -f \"./plugins/nova-editor/nova_editor.so\" ]; then cp -rf ./plugins/nova-editor/* \"$TARGET_SYNTH_DIR/3dthc - VST - Nova Editor/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/Nova-Editor/releases/download/nova-editor-v0.7.0/Nova-Editor-0.7.0-mpc-armv7.zip\" \"/tmp/nova-editor.zip\") && unzip -o /tmp/nova-editor.zip -d \"$TARGET_SYNTH_DIR/3dthc - VST - Nova Editor/\" && rm -f /tmp/nova-editor.zip; fi && sync", 1,
            false,
            false,
            false,
            { "librarian", "sysex", "mininova", "novation", "midi" }
        },
        {
            "nusaw", "NuSaw", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1393", "2.2 MB", "14%",
            "Charles Vestal - VST - [SYN] NuSaw", "nusaw.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-nusaw/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-nusaw/releases/download/v1.0.2/SYN-NuSaw-1.0.2-mpc-armv7.zip", "plugins/nusaw/thumb.png",
            "Detuned supersaw polysynth with a sub, resonant filter, chorus and delay; 27 presets for pads, stabs and leads.", "./plugins/nusaw/install.sh", 0,
            false,
            false,
            false,
            { "supersaw", "trance", "polyphonic", "pads" }
        },
        {
            "obxd", "OB-Xd", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1627", "1.5 MB", "14%",
            "reales - VST - [SYN] OB-Xd", "obxd.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-obxd/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-obxd/releases/download/v1.1.1/SYN-OB-Xd-1.1.1-mpc-armv7.zip", "plugins/obxd/thumb.png",
            "OB-Xd, the Oberheim OB-X emulation: two oscillators with sync and cross-mod, a 12/24 dB filter, voice variation; 128 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/reales - VST - [SYN] OB-Xd\" && if [ -f \"./plugins/obxd/obxd.so\" ]; then cp -rf ./plugins/obxd/* \"$TARGET_SYNTH_DIR/reales - VST - [SYN] OB-Xd/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-obxd/releases/download/v1.1.1/SYN-OB-Xd-1.1.1-mpc-armv7.zip\" \"/tmp/obxd.zip\") && unzip -o /tmp/obxd.zip -d \"$TARGET_SYNTH_DIR/reales - VST - [SYN] OB-Xd/\" && rm -f /tmp/obxd.zip; fi && sync", 1,
            false,
            false,
            false,
            { "ob-x", "analog-modelling", "polyphonic", "subtractive" }
        },
        {
            "obxdmpc", "OB-Xd MPC", "3dthc", "v0.4.5", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "1630", "10.7 MB", "14%",
            "sd88me - VST - OB-Xd", "obxd.so",
            "https://raw.githubusercontent.com/3dthc/OB-XD-MPC/master/docs/screenshots/obxd-mpc-black.png", "https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.5/OB-Xd-0.4.5-mpc-armv7.zip", "plugins/obxdmpc/thumb.png",
            "OB-Xd, the Oberheim OB-X emulation: two oscillators with sync and cross-mod, a 12/24 dB filter, voice variation; 128 presets.", "./plugins/obxdmpc/install.sh", 0,
            false,
            false,
            false,
            { "ob-x", "analog-modelling", "polyphonic", "subtractive" }
        },
        {
            "odin2", "Odin 2", "3dthc", "v1.4.6", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "1542", "17.6 MB", "14%",
            "3dthc - VST - Odin 2", "odin2.so",
            "https://raw.githubusercontent.com/3dthc/Odin2-MPC/main/odin2-mpc-main.png", "https://github.com/3dthc/Odin2-MPC/releases/download/odin2-v1.4.6/Odin-2-1.4.6-mpc-armv7.zip", "plugins/odin2/thumb.png",
            "Odin 2 synthesizer ported to standalone Akai MPC devices as a native VST2 plugin. Features Odin's original engine, 261 factory patches, touchscreen pages, automatic voice management and a CPU meter. Tested on an Akai MPC Live II; unofficial port.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - Odin 2\" && if [ -f \"./plugins/odin2/odin2.so\" ]; then cp -rf ./plugins/odin2/* \"$TARGET_SYNTH_DIR/3dthc - VST - Odin 2/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/Odin2-MPC/releases/download/odin2-v1.4.6/Odin-2-1.4.6-mpc-armv7.zip\" \"/tmp/odin2.zip\") && unzip -o /tmp/odin2.zip -d \"$TARGET_SYNTH_DIR/3dthc - VST - Odin 2/\" && rm -f /tmp/odin2.zip; fi && sync", 1,
            false,
            false,
            false,
            {  }
        },
        {
            "omni-sampler", "Omni Sampler", "MrHighMan", "v1.8.1", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-02",
            "3014", "5.0 MB", "20%",
            "MR HighMan - VST - Omni Sampler", "omni_sampler.so",
            "https://raw.githubusercontent.com/B0ss-M/MrHighMan/main/docs/screenshot.png", "https://github.com/B0ss-M/MrHighMan/releases/download/v1.8.1/Omni-Sampler-1.8.1-mpc-armv7.zip", "plugins/omni-sampler/thumb.png",
            "Multi-format sampler: plays Akai, E-mu, Roland, Kontakt/Maschine, SF2 and SFZ instruments straight from their files or sampler disk images, with layers, keyswitching, a mod matrix and its own screen skin.", "./plugins/omni-sampler/install.sh", 0,
            false,
            false,
            false,
            { "akai", "emu", "roland", "kontakt", "sf2", "sfz", "disk-images" }
        },
        {
            "plugin-manager-g2", "Open MPC Plugin Manager G2", "sd88me", "v0.2.5", "Utility",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-31",
            "1050", "18.6 MB", "14%",
            "OpenMPC - VST - Plugin Manager G2", "openmpc_pm_g2.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/588466d/tools/plugin_manager_g2/docs/screenshots/manager-discover-light.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/plugin-manager-g2-v0.2.5/Plugin-Manager-G2-0.2.5-mpc-armv7.zip", "plugins/plugin-manager-g2/thumb.png",
            "The official OpenMPC plugin manager for Gen1 and Gen2 MPCs (32-bit build for Force, Live, One, X, Key; 64-bit for the Live III and kin). Browse this catalog and install, update and remove plugins on the MPC itself, with rollback if an install fails. Built on poloq's mpc-vst-manager.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/OpenMPC - VST - Plugin Manager G2\" && if [ -f \"./plugins/plugin-manager-g2/openmpc_pm_g2.so\" ]; then cp -rf ./plugins/plugin-manager-g2/* \"$TARGET_SYNTH_DIR/OpenMPC - VST - Plugin Manager G2/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/plugin-manager-g2-v0.2.5/Plugin-Manager-G2-0.2.5-mpc-armv7.zip\" \"/tmp/plugin-manager-g2.zip\") && unzip -o /tmp/plugin-manager-g2.zip -d \"$TARGET_SYNTH_DIR/OpenMPC - VST - Plugin Manager G2/\" && rm -f /tmp/plugin-manager-g2.zip; fi && sync", 1,
            false,
            false,
            false,
            { "installer", "catalog", "manager", "gen1", "gen2", "utility" }
        },
        {
            "open303", "Open303", "saustin2010", "v1.2.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2148", "2.0 MB", "14%",
            "Robin Schmidt - VST - [SYN] Open303", "acid303.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-open303/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-open303/releases/download/v1.2.1/SYN-Open303-1.2.1-mpc-armv7.zip", "plugins/open303/thumb.png",
            "TB-303 bass line: Open303 with the Devilfish mods, a drive stage and 13 presets.", "./plugins/open303/install.sh", 0,
            false,
            false,
            false,
            { "tb-303", "acid", "bass", "monophonic" }
        },
        {
            "osirus", "Osirus", "sd88me", "v0.2.10", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-03",
            "854", "4.3 MB", "14%",
            "sd88me - VST - Osirus", "osirus.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-osirus/e39d03c538f4fde79d558cd4c423b18964cc5ffc/docs/screenshots/osirus-osc.png", "https://github.com/sd88me/mpc-vst-osirus/releases/download/v0.2.10/Osirus-0.2.10-mpc-aarch64.zip", "plugins/osirus/thumb.png",
            "An emulation of the Access Virus A, B and C synthesizers' original firmware (gearmulator's DSP56300 emulator) as a native instrument for Gen2 MPC devices, with a touchscreen skin, banks and patch browser, Q-Link control and knob values on every page. Gen2 (aarch64) only; you need your own Virus ROM.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Osirus\" && if [ -f \"./plugins/osirus/osirus.so\" ]; then cp -rf ./plugins/osirus/* \"$TARGET_SYNTH_DIR/sd88me - VST - Osirus/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-osirus/releases/download/v0.2.10/Osirus-0.2.10-mpc-aarch64.zip\" \"/tmp/osirus.zip\") && unzip -o /tmp/osirus.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Osirus/\" && rm -f /tmp/osirus.zip; fi && sync", 1,
            false,
            false,
            false,
            { "emulation", "virtual-analog", "rompler-free", "gen2" }
        },
        {
            "percolator", "Percolator", "saustin2010", "v2.1.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2570", "6.6 MB", "14%",
            "Steve A - VST - [DRUM] Percolator", "percolator.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-percolator/v2.1.0/screenshots/page_1.png", "https://github.com/saustin2010/mpc-vst-percolator/releases/download/v2.1.0/DRUM-Percolator-2.1.0-mpc-armv7.zip", "plugins/percolator/thumb.png",
            "Four-voice percussion synth laid out like the Erica Synths Pērkons HD-01 (three algorithms per voice, BBD delay, LFO to every knob), with envelope displays; each voice on its own MIDI channel with the Pērkons' CCs, and on its own track with Percolator OUT; plays the Pērkons' free kit packs.", "./plugins/percolator/install.sh", 0,
            false,
            false,
            false,
            { "drums", "percussion", "kits", "drone", "envelope-display" }
        },
        {
            "op1fx-phase", "Phase", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "146", "7.5 MB", "14%",
            "bombest - VST - Phase", "phase.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/phase.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phase-0.4.1-mpc-armv7.zip", "plugins/op1fx-phase/thumb.png",
            "Eight-voice phase-distortion synth, designed from the OP-1 screen.", "./plugins/op1fx-phase/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "phase-distortion" }
        },
        {
            "polywerk", "PolyWerk", "Devko", "v0.0.6", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "245", "6.3 MB", "14%",
            "Devko - VST - PolyWerk", "polywerk.so",
            "https://raw.githubusercontent.com/Devko/PolyWerk/main/docs/img/waves.png", "https://github.com/Devko/PolyWerk/releases/download/v0.0.6/PolyWerk-0.0.6-mpc-armv7.zip", "plugins/polywerk/thumb.png",
            "Eight-voice wavetable synth: two wavetable oscillators with 8x unison and subs, 30 built-in tables plus your own Serum-format WAVs, two filters with comb and vowel, two LFOs, a 12-slot mod matrix, arpeggiator and sequencers, 205 presets and a seven-tab touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Devko - VST - PolyWerk\" && if [ -f \"./plugins/polywerk/polywerk.so\" ]; then cp -rf ./plugins/polywerk/* \"$TARGET_SYNTH_DIR/Devko - VST - PolyWerk/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Devko/PolyWerk/releases/download/v0.0.6/PolyWerk-0.0.6-mpc-armv7.zip\" \"/tmp/polywerk.zip\") && unzip -o /tmp/polywerk.zip -d \"$TARGET_SYNTH_DIR/Devko - VST - PolyWerk/\" && rm -f /tmp/polywerk.zip; fi && sync", 1,
            false,
            false,
            false,
            { "wavetable", "polyphonic", "arpeggiator", "sequencer", "mod-matrix" }
        },
        {
            "potentialbreaks", "PotentialBreaks", "PotentialBreaks", "v1.1.4", "Sampler",
            "INST", "Stable", "MPC One (HAKAI 3.10)", "MPC OS 3.x", "2026-11-06",
            "135", "1.6 MB", "14%",
            "PotentialBreaks - VST - PotentialBreaks", "potentialbreaks.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialbreaks/708f8f156ff4aae9ac35a56a27ca4bce1a81a76d/docs/images/potentialbreaks.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialbreaks/releases/download/v1.1.4/PotentialBreaks-1.1.4-mpc-armv7.zip", "plugins/potentialbreaks/thumb.png",
            "Drum-break slicer: picks a random break from your own break folders (WAV or FLAC), chops it to 8 or 16 slices or by hits across the pads and matches it to the project tempo, by re-pitching or stretching. Shuffle the slices and save the groove as MIDI.", "./plugins/potentialbreaks/install.sh", 0,
            false,
            false,
            false,
            { "breaks", "slicer", "drums", "chop" }
        },
        {
            "potentialnoise", "PotentialNoise", "PotentialBreaks", "v2.0.2", "Utility",
            "INST", "Stable", "MPC One (HAKAI 3.10)", "MPC OS 3.x", "2026-11-06",
            "61", "191 KB", "14%",
            "PotentialBreaks - VST - PotentialNoise", "potentialnoise.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialnoise/60512395e7a359614485d3029583f5e290984dda/docs/images/potentialnoise.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialnoise/releases/download/v2.0.2/PotentialNoise-2.0.2-mpc-armv7.zip", "plugins/potentialnoise/thumb.png",
            "Calibrated pink noise reference for the pink-noise mixing method: one-tap RMS levels from -24 to -12 dB, mono or stereo, full or 40 Hz-16 kHz range, live RMS and peak reading and a level meter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialNoise\" && if [ -f \"./plugins/potentialnoise/potentialnoise.so\" ]; then cp -rf ./plugins/potentialnoise/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialNoise/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialnoise/releases/download/v2.0.2/PotentialNoise-2.0.2-mpc-armv7.zip\" \"/tmp/potentialnoise.zip\") && unzip -o /tmp/potentialnoise.zip -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialNoise/\" && rm -f /tmp/potentialnoise.zip; fi && sync", 1,
            false,
            false,
            false,
            { "pink-noise", "mixing", "reference", "meter" }
        },
        {
            "potentialwax", "PotentialWax", "PotentialBreaks", "v1.2.2", "Sampler",
            "INST", "Stable", "MPC One (HAKAI 3.10)", "MPC OS 3.x", "2026-11-06",
            "95", "1.9 MB", "14%",
            "PotentialBreaks - VST - PotentialWax", "potentialwax.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialwax/edbcaf4d8ca332e3cb05a0603aa63d4124d6c220/docs/images/potentialwax.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialwax/releases/download/v1.2.2/PotentialWax-1.2.2-mpc-armv7.zip", "plugins/potentialwax/thumb.png",
            "Melodic sampler in the spirit of Serato Sample: picks or browses songs from your own folders (MP3, WAV or FLAC), detects tempo and key, sets 16 cue points (phrases, bars or hits) on the pads and matches them to your tempo and key, with time-stretch or vinyl-style pitching. Save the chops as WAVs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialWax\" && if [ -f \"./plugins/potentialwax/potentialwax.so\" ]; then cp -rf ./plugins/potentialwax/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialWax/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialwax/releases/download/v1.2.2/PotentialWax-1.2.2-mpc-armv7.zip\" \"/tmp/potentialwax.zip\") && unzip -o /tmp/potentialwax.zip -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialWax/\" && rm -f /tmp/potentialwax.zip; fi && sync", 1,
            false,
            false,
            false,
            { "chop", "melodic", "sampler", "time-stretch", "key-detection" }
        },
        {
            "prloguexxl", "PrLogueXXL", "L'Cronx", "v0.1.1", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-23",
            "551", "18.0 MB", "14%",
            "L'Cronx - VST - PrLogueXXL", "prloguexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PrLogueXXL/v0.1.1/docs/img/osc.png", "https://github.com/sunskiefer/PrLogueXXL/releases/download/v0.1.1/PrLogueXXL-0.1.1-mpc-armv7.zip", "plugins/prloguexxl/thumb.png",
            "16-voice poly synth modelled on the KORG prologue-16 (not affiliated with KORG): two VCOs, a multi engine (noise, 16 VPM types, KORG's logue-sdk waves oscillator), Poly/Duo/Mono/Unison/Chord, Main and Sub timbres, arpeggiator on MPC's grid, mod FX, delay/reverb and a low-frequency compressor with a live meter, over four pages.", "./plugins/prloguexxl/install.sh", 0,
            false,
            false,
            false,
            { "synth", "poly", "analog-modelled", "vpm", "fm", "arpeggiator", "pad", "lead" }
        },
        {
            "profit-08", "Profit-08", "sd88me", "v0.1.3", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "2099", "4.5 MB", "14%",
            "sd88me - VST - Profit-08", "profit_08.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-profit8/main/docs/img/screenshot-program.png", "https://github.com/sd88me/mpc-vst-profit8/releases/download/profit-08-v0.1.3/Profit-08-0.1.3-mpc-armv7.zip", "plugins/profit-08/thumb.png",
            "Eight-voice, two-layer analog-style synthesizer: stack and split layers, a gated sequencer and arpeggiator, a resonant 2/4-pole filter and a banks browser for your own program and bank SysEx dumps.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-08\" && if [ -f \"./plugins/profit-08/profit_08.so\" ]; then cp -rf ./plugins/profit-08/* \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-08/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-profit8/releases/download/profit-08-v0.1.3/Profit-08-0.1.3-mpc-armv7.zip\" \"/tmp/profit-08.zip\") && unzip -o /tmp/profit-08.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-08/\" && rm -f /tmp/profit-08.zip; fi && sync", 1,
            false,
            false,
            false,
            { "analog", "poly", "layers", "sequencer" }
        },
        {
            "profit-5", "Profit-5", "sd88me", "v0.2.1", "Synth",
            "INST", "Stable", "Akai MPC Live III (Gen2) (3.10)", "MPC OS 3.x", "2026-10-28",
            "1017", "20.4 MB", "31%",
            "sd88me - VST - Profit-5", "profit_5.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-profit5/5724d1fe8fdb0aab2c72837228a7fd77f1980ec1/docs/img/screenshot-osc.png", "https://github.com/sd88me/mpc-vst-profit5/releases/download/profit-5-vst-v0.2.1/Profit-5-0.2.1-mpc-armv7.zip", "plugins/profit-5/thumb.png",
            "Five-voice analog polysynth modelled on an early SSM-chip classic: two oscillators, a four-pole resonant filter, Poly-Mod, wheel-mod, glide and unison, 38 factory programs, a Rev 2 / Rev 4 voicing switch, and an importer for Repro-5 .h2p patches.", "./plugins/profit-5/install.sh", 0,
            false,
            false,
            false,
            { "analog", "poly", "vintage", "poly-mod" }
        },
        {
            "ps3100xxl", "PS3100XXL", "L'Cronx", "v0.1.5", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-23",
            "649", "15.9 MB", "14%",
            "L'Cronx - VST - PS3100XXL", "ps3100xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PS3100XXL/v0.1.5/docs/img/overview.png", "https://github.com/sunskiefer/PS3100XXL/releases/download/v0.1.5/PS3100XXL-0.1.5-mpc-armv7.zip", "plugins/ps3100xxl/thumb.png",
            "Semi-modular poly synth modelled on the 1977 Korg PS-3100 (not affiliated with Korg): one oscillator, filter and envelope per key up to 48 voices, three-peak resonators, ensemble, two modulation generators, sample & hold, twelve temperament knobs and a patch bay you patch by touch, on one zoomable page.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - PS3100XXL\" && if [ -f \"./plugins/ps3100xxl/ps3100xxl.so\" ]; then cp -rf ./plugins/ps3100xxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - PS3100XXL/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/PS3100XXL/releases/download/v0.1.5/PS3100XXL-0.1.5-mpc-armv7.zip\" \"/tmp/ps3100xxl.zip\") && unzip -o /tmp/ps3100xxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - PS3100XXL/\" && rm -f /tmp/ps3100xxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "synth", "poly", "semi-modular", "analog-modelled", "patch-bay", "ensemble", "resonator" }
        },
        {
            "op1fx-pulse", "Pulse", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "137", "5.1 MB", "14%",
            "bombest - VST - Pulse", "pulse.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/pulse.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Pulse-0.4.1-mpc-armv7.zip", "plugins/op1fx-pulse/thumb.png",
            "Two PWM pulse oscillators per voice, eight voices. OP-1 inspired.", "./plugins/op1fx-pulse/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "pwm", "pulse" }
        },
        {
            "raffosynth", "RaffoSynth", "saustin2010", "v1.3.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2004", "6.4 MB", "14%",
            "Raffo - VST - [SYN] RaffoSynth", "moog.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-raffosynth/v1.3.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-raffosynth/releases/download/v1.3.1/SYN-RaffoSynth-1.3.1-mpc-armv7.zip", "plugins/raffosynth/thumb.png",
            "RaffoSynth: a Minimoog-style mono synth with four oscillators and a ladder filter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Raffo - VST - [SYN] RaffoSynth\" && if [ -f \"./plugins/raffosynth/moog.so\" ]; then cp -rf ./plugins/raffosynth/* \"$TARGET_SYNTH_DIR/Raffo - VST - [SYN] RaffoSynth/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-raffosynth/releases/download/v1.3.1/SYN-RaffoSynth-1.3.1-mpc-armv7.zip\" \"/tmp/raffosynth.zip\") && unzip -o /tmp/raffosynth.zip -d \"$TARGET_SYNTH_DIR/Raffo - VST - [SYN] RaffoSynth/\" && rm -f /tmp/raffosynth.zip; fi && sync", 1,
            false,
            false,
            false,
            { "minimoog", "monophonic", "ladder-filter", "analog-modelling" }
        },
        {
            "rings", "Resonator", "saustin2010", "v1.2.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2006", "4.1 MB", "14%",
            "Émilie Gillet - VST - [SYN] Resonator", "rings.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-resonator/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-resonator/releases/download/v1.2.1/SYN-Resonator-1.2.1-mpc-armv7.zip", "plugins/rings/thumb.png",
            "Resonator: seven resonator models (strings, modal, FM, chords) strummed by the notes you play; 14 presets, from Émilie Gillet's open-source Rings.", "./plugins/rings/install.sh", 0,
            false,
            false,
            false,
            { "resonator", "physical-modelling", "eurorack" }
        },
        {
            "setbfree-organ", "setBfree Organ", "SamplingHigh", "v0.3.0", "Organ",
            "INST", "Stable", "Akai Force (3.9.1.2)", "MPC OS 3.x", "2026-10-24",
            "1123", "2.2 MB", "8120%",
            "Force Runtime - VST - setBfree Organ", "setbfree.so",
            "https://raw.githubusercontent.com/SamplingHigh/mpc-vst-setbfree/v0.3.0/docs/images/setbfree-organ.png", "https://github.com/SamplingHigh/mpc-vst-setbfree/releases/download/v0.3.0/setBfree-Organ-0.3.0-mpc-armv7.zip", "plugins/setbfree-organ/thumb.png",
            "Tonewheel organ with drawbars, percussion, scanner, editable Leslie and keyboard splits.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Force Runtime - VST - setBfree Organ\" && if [ -f \"./plugins/setbfree-organ/setbfree.so\" ]; then cp -rf ./plugins/setbfree-organ/* \"$TARGET_SYNTH_DIR/Force Runtime - VST - setBfree Organ/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/SamplingHigh/mpc-vst-setbfree/releases/download/v0.3.0/setBfree-Organ-0.3.0-mpc-armv7.zip\" \"/tmp/setbfree-organ.zip\") && unzip -o /tmp/setbfree-organ.zip -d \"$TARGET_SYNTH_DIR/Force Runtime - VST - setBfree Organ/\" && rm -f /tmp/setbfree-organ.zip; fi && sync", 1,
            false,
            false,
            false,
            { "tonewheel", "drawbar", "rotary" }
        },
        {
            "sfizz", "sfizz", "gmorb (MPC port); sfizz by sfztools", "v1.1.1", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "709", "1.4 MB", "14%",
            "sfztools - VST - sfizz", "sfizz_mpc.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-sfizz/6d9e627281779f087898521cd27caabf9b7a75f2/docs/screen.png", "https://github.com/gmorb/mpc-vst-sfizz/releases/download/v1.1.1/sfizz-1.1.1-mpc-armv7.zip", "plugins/sfizz/thumb.png",
            "The sfizz 1.2.3 sampler: plays SFZ libraries and SoundFont 2 (.sf2) files from SD cards and USB drives, streamed from disk, with key switches, the instrument's own CC controls, Scala tuning, mono/legato with glide and up to Sinc 72 interpolation, on a one-page screen modelled on sfizz's editor. Gen1 and Gen2 builds.", "./plugins/sfizz/install.sh", 0,
            false,
            false,
            false,
            { "sfz", "sf2", "soundfont", "sampler", "piano", "keyswitch", "streaming", "scala" }
        },
        {
            "sonix", "Sonix", "3dthc", "v0.3.12", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "1238", "124.9 MB", "14%",
            "3dthc - VST - Sonix", "sonix.so",
            "https://raw.githubusercontent.com/3dthc/Sonix/main/docs/images/main_layers.jpg", "https://github.com/3dthc/Sonix/releases/download/v0.3.12/Sonix-0.3.12-mpc-armv7.zip", "plugins/sonix/thumb.png",
            "A melodic ROMpler instrument for standalone Akai MPC devices, built as a native ARM VST2 plugin. Includes pianos, keys, pads, strings, brass, choirs, leads, plucks, bass, bells, synth chords and effects, using redistributable sound libraries.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - Sonix\" && if [ -f \"./plugins/sonix/sonix.so\" ]; then cp -rf ./plugins/sonix/* \"$TARGET_SYNTH_DIR/3dthc - VST - Sonix/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/Sonix/releases/download/v0.3.12/Sonix-0.3.12-mpc-armv7.zip\" \"/tmp/sonix.zip\") && unzip -o /tmp/sonix.zip -d \"$TARGET_SYNTH_DIR/3dthc - VST - Sonix/\" && rm -f /tmp/sonix.zip; fi && sync", 1,
            false,
            false,
            false,
            {  }
        },
        {
            "op1fx-string", "String", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "150", "3.7 MB", "14%",
            "bombest - VST - String", "string.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/string.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/String-0.4.1-mpc-armv7.zip", "plugins/op1fx-string/thumb.png",
            "Plucked string with eight voices and an amp envelope. OP-1 inspired.", "./plugins/op1fx-string/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "pluck", "string" }
        },
        {
            "sturm-tp", "Sturm-TP", "sd88me", "v0.1.2", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "1363", "4.5 MB", "14%",
            "sd88me - VST - Sturm-TP", "sturm.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-sturm/sturm-tp-v0.1.1/docs/img/sturm.png", "https://github.com/sd88me/mpc-vst-sturm/releases/download/sturm-tp-v0.1.2/Sturm-TP-0.1.2-mpc-armv7.zip", "plugins/sturm-tp/thumb.png",
            "Six-voice analog-style drum and synth voice modelled on the Tempest drum machine: two analog and two sample oscillators, lowpass and highpass filters, five envelopes, two LFOs, eight mod paths, kit mode, distortion, compressor and delay. Bank browser with Q-Links; loads its SysEx dumps.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Sturm-TP\" && if [ -f \"./plugins/sturm-tp/sturm.so\" ]; then cp -rf ./plugins/sturm-tp/* \"$TARGET_SYNTH_DIR/sd88me - VST - Sturm-TP/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-sturm/releases/download/sturm-tp-v0.1.2/Sturm-TP-0.1.2-mpc-armv7.zip\" \"/tmp/sturm-tp.zip\") && unzip -o /tmp/sturm-tp.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Sturm-TP/\" && rm -f /tmp/sturm-tp.zip; fi && sync", 1,
            false,
            false,
            false,
            { "analog-modelling", "drums", "sampler", "kit", "tempest", "dsi" }
        },
        {
            "subwerk", "SubWerk", "Devko", "v0.0.10", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "222", "1.0 MB", "14%",
            "Devko - VST - SubWerk", "subwerk.so",
            "https://raw.githubusercontent.com/Devko/SubWerk/main/docs/img/filter.png", "https://github.com/Devko/SubWerk/releases/download/v0.0.10/SubWerk-0.0.10-mpc-armv7.zip", "plugins/subwerk/thumb.png",
            "Analog-style monosynth in the spirit of a 37-key American paraphonic classic: two morphing oscillators with hard sync, sub and noise, a feedback mixer, a 4-pole ladder filter with Multidrive, two looping envelopes, glide and Duo. 57 presets and a six-tab touchscreen skin with Q-Links.", "./plugins/subwerk/install.sh", 0,
            false,
            false,
            false,
            { "monosynth", "analog", "bass", "ladder-filter", "paraphonic" }
        },
        {
            "surgexxl", "SurgeXXL", "L'Cronx", "v0.2.0", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-24",
            "932", "294.4 MB", "14%",
            "L'Cronx - VST - SurgeXXL", "surgexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/SurgeXXL/08c3c3eed8a3ad6792211fb1dc2351ded2383995/docs/img/osc.png", "https://github.com/sunskiefer/SurgeXXL/releases/download/v0.2.0/SurgeXXL-0.2.0-mpc-armv7.zip", "plugins/surgexxl/thumb.png",
            "The open-source Surge XT synth engine, headless, for Gen1: 2 scenes of 3 oscillators, dual filters, 6 LFOs per scene, mod matrix, 16 FX slots and 8 macros on eight pages. Loads every factory and third-party patch (3008) and wavetable (761), plus your own. Install it on its own (about 450 MB). MPC OS 3.x.", "./plugins/surgexxl/install.sh", 0,
            false,
            false,
            false,
            { "synth", "polysynth", "wavetable", "fm", "surge", "lfo", "presets", "airwindows" }
        },
        {
            "tablor", "Tablor", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1081", "20.3 MB", "14%",
            "athousanddetails - VST - [SYN] Tablor", "tablor.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-tablor/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-tablor/releases/download/v1.1.1/SYN-Tablor-1.1.1-mpc-armv7.zip", "plugins/tablor/thumb.png",
            "Two-oscillator wavetable synth with unison, filter and mod envelopes; ships its wavetable library and 9 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - [SYN] Tablor\" && if [ -f \"./plugins/tablor/tablor.so\" ]; then cp -rf ./plugins/tablor/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - [SYN] Tablor/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-tablor/releases/download/v1.1.1/SYN-Tablor-1.1.1-mpc-armv7.zip\" \"/tmp/tablor.zip\") && unzip -o /tmp/tablor.zip -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - [SYN] Tablor/\" && rm -f /tmp/tablor.zip; fi && sync", 1,
            false,
            false,
            false,
            { "wavetable", "polyphonic", "unison" }
        },
        {
            "tinyfreaky", "TinyFreaky", "MagicStino", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "835", "5.0 MB", "14%",
            "MagicStino - VST - Tinyfreaky", "tinyfreaky.so",
            "https://raw.githubusercontent.com/MagicStino/TinyFreaky/v0.4.0/assets/force-readable-preview.png", "https://github.com/MagicStino/TinyFreaky/releases/download/v0.4.1/Tinyfreaky-0.4.1-mpc-armv7.zip", "plugins/tinyfreaky/thumb.png",
            "Eight-voice MicroFreak-inspired interpretation with 16 synthesis engines, a 5x7 modulation matrix, arpeggiator/swing, 64-step sequencer and sound-informed preset names. 342 playable factory slots; six engine families remain unavailable.", "./plugins/tinyfreaky/install.sh", 0,
            false,
            false,
            false,
            { "synth", "polyphonic", "modulation", "sequencer" }
        },
        {
            "tr-mpc", "TR-MPC", "sd88me", "v1.1.0", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 3.x", "2026-11-02",
            "1612", "29.1 MB", "25%",
            "sd88me - VST - TR-MPC", "trmpc.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/7e704c6f3be14b661c887631e203494d8cc3f74f/docs/img/tr-mpc-slots.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-vst-v1.1.0/TR-MPC-1.1.0-mpc-armv7.zip", "plugins/tr-mpc/thumb.png",
            "A modular 16-slot drum kit: each slot plays any of the 49 voices of athousanddetails' 6W6, 8W8, CW-78 and 9W9 (a 606 kick beside an 808 snare and 909 hats), with per-slot level, tune, decay, drive, pan and sends, one shared reverb, delay and master chain, a randomise module, and a touchscreen skin that reskins each slot to its voice's kit.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC\" && if [ -f \"./plugins/tr-mpc/trmpc.so\" ]; then cp -rf ./plugins/tr-mpc/* \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-vst-v1.1.0/TR-MPC-1.1.0-mpc-armv7.zip\" \"/tmp/tr-mpc.zip\") && unzip -o /tmp/tr-mpc.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC/\" && rm -f /tmp/tr-mpc.zip; fi && sync", 1,
            false,
            false,
            false,
            { "tr-606", "tr-808", "cr-78", "tr-909", "modular" }
        },
        {
            "tributary", "Tributary", "johnny", "v1.1.0", "Utility",
            "INST", "Stable", "MPC One (3.9.1.2)", "MPC OS 3.x", "2026-10-10",
            "894", "556 KB", "3%",
            "johnny - VST - Tributary", "tributary.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/mpc-tributary/v1.1.0/screenshots/tributary.png", "https://github.com/stampman3000-jpg/mpc-tributary/releases/download/v1.1.0/Tributary-1.1.0-mpc-armv7.zip", "plugins/tributary/thumb.png",
            "Sums chosen Elektron Overbridge outputs onto one MPC track over USB. Based on Overwitch.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/johnny - VST - Tributary\" && if [ -f \"./plugins/tributary/tributary.so\" ]; then cp -rf ./plugins/tributary/* \"$TARGET_SYNTH_DIR/johnny - VST - Tributary/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/stampman3000-jpg/mpc-tributary/releases/download/v1.1.0/Tributary-1.1.0-mpc-armv7.zip\" \"/tmp/tributary.zip\") && unzip -o /tmp/tributary.zip -d \"$TARGET_SYNTH_DIR/johnny - VST - Tributary/\" && rm -f /tmp/tributary.zip; fi && sync", 1,
            false,
            false,
            false,
            { "utility" }
        },
        {
            "tropix", "Tropix", "3dthc", "v0.1.2", "Sampler",
            "INST", "Stable", "MPC Live II (3.10)", "MPC OS 2.x & 3.x", "2026-11-06",
            "377", "215.2 MB", "14%",
            "3dthc - VST - Tropix", "tropix.so",
            "https://raw.githubusercontent.com/3dthc/Tropix/1005f9c4dc719f58bd17b02fac750457e6c8cb1e/docs/images/browse.jpg", "https://github.com/3dthc/Tropix/releases/download/v0.1.2/Tropix-0.1.2-mpc-armv7.zip", "plugins/tropix/thumb.png",
            "Island and afro ROMpler: 128 presets and 65 instruments for afrobeat, amapiano, kompa, soca, reggae and Hawaiian music - steel pans, log drums, saxes and trumpets, kompa and slack-key guitars, steels, keys, mallets, basses and percussion, from redistributable recordings and original synthesis. Two layers, glide, horn scoop, vibrato, nine colorways.", "./plugins/tropix/install.sh", 0,
            false,
            false,
            false,
            { "rompler", "afrobeat", "amapiano", "kompa", "soca", "reggae", "steel-pan", "horns" }
        },
        {
            "vader", "VADER", "L'Cronx", "v0.9.0", "Synth",
            "INST", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-30",
            "1889", "42.2 MB", "14%",
            "L'Cronx - VST - VADER", "vader.so",
            "https://raw.githubusercontent.com/sunskiefer/VADER/v0.8.0/docs/img/main_star.png", "https://github.com/sunskiefer/VADER/releases/download/v0.9.0/VADER-0.9.0-mpc-armv7.zip", "plugins/vader/thumb.png",
            "An orbit-based vector synth: an orbiter travels a shape (15 built in, 35 maths shapes, or your own SVG, OBJ, PNG or JPG) or one of 27 modes across four corners. Each corner plays overtone generators, one of 14 voices, or a WAV sample. Two timbres, arpeggiator, effects. Its screen runs on your phone: scan the QR code, load shapes from the phone too.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - VADER\" && if [ -f \"./plugins/vader/vader.so\" ]; then cp -rf ./plugins/vader/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - VADER/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/VADER/releases/download/v0.9.0/VADER-0.9.0-mpc-armv7.zip\" \"/tmp/vader.zip\") && unzip -o /tmp/vader.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - VADER/\" && rm -f /tmp/vader.zip; fi && sync", 1,
            false,
            false,
            false,
            { "synth", "vector", "orbit", "additive", "morphing", "arpeggiator", "svg", "3d" }
        },
        {
            "op1fx-voltage", "Voltage", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "135", "4.6 MB", "14%",
            "bombest - VST - Voltage", "voltage.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/voltage.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Voltage-0.4.1-mpc-armv7.zip", "plugins/op1fx-voltage/thumb.png",
            "Four-control synth with eight voices and an amp envelope. OP-1 inspired.", "./plugins/op1fx-voltage/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "analog" }
        },
        {
            "wurl", "Wurl", "saustin2010", "v1.1.1", "Electric Piano",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1299", "827 KB", "14%",
            "Filliformes - VST - [SYN] Wurl", "wurl.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-wurl/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-wurl/releases/download/v1.1.1/SYN-Wurl-1.1.1-mpc-armv7.zip", "plugins/wurl/thumb.png",
            "A physically modelled Wurlitzer 200A electric piano (OpenWurli) that barks with velocity, with tremolo and 10 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Wurl\" && if [ -f \"./plugins/wurl/wurl.so\" ]; then cp -rf ./plugins/wurl/* \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Wurl/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-wurl/releases/download/v1.1.1/SYN-Wurl-1.1.1-mpc-armv7.zip\" \"/tmp/wurl.zip\") && unzip -o /tmp/wurl.zip -d \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Wurl/\" && rm -f /tmp/wurl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "wurlitzer", "physical-modelling", "keys", "tremolo" }
        },
        {
            "xenia", "Xenia", "sd88me", "v0.1.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-03",
            "317", "8.4 MB", "14%",
            "sd88me - VST - Xenia", "xenia.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-xenia/aeb6a25cfe8e5e14fc1df592526b2c2a32eb99be/docs/screenshots/xenia-play.png", "https://github.com/sd88me/mpc-vst-xenia/releases/download/v0.1.2/Xenia-0.1.2-mpc-aarch64.zip", "plugins/xenia/thumb.png",
            "An emulation of the Microwave XT wavetable synthesizer's original firmware (gearmulator's 68k and DSP56300 emulators) as a native instrument for Gen2 MPC devices: ten touchscreen pages, ROM banks and patch browser, assignable play knobs and Q-Link control on every page. Gen2 (aarch64) only; you need your own Microwave XT ROM.", "./plugins/xenia/install.sh", 0,
            false,
            false,
            false,
            { "emulation", "wavetable", "rompler-free", "gen2" }
        },
        {
            "pitonic", "πToniC", "Timo", "v1.0.0", "Drum Machine",
            "INST", "Stable", "Akai Force (Gen1) (3.10.0.49)", "MPC OS 3.x", "2026-11-06",
            "32", "2.1 MB", "14%",
            "Timo - VST - πToniC", "pitonic.so",
            "https://raw.githubusercontent.com/timomacquis/piToniC-MPC/v1.0.0/Cover.png", "https://github.com/timomacquis/piToniC-MPC/releases/download/v1.0.0/piToniC-1.0.0-Patternarium-BEST150-mpc-armv7.zip", "plugins/pitonic/thumb.png",
            "Eight-voice percussion synthesizer with oscillator and noise sound design, 150 Patternarium kits, per-voice sends, shared reverb and delay.", "./plugins/pitonic/install.sh", 0,
            false,
            false,
            false,
            { "drum-machine", "synth", "percussion" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "MIDI";
        sec.items = {
        {
            "acid", "Acid", "sd88me", "v2.1.0", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-10-22",
            "5921", "3.2 MB", "14%",
            "sd88me - VST - Acid", "acid.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-acid/master/docs/img/acid.png", "https://github.com/sd88me/mpc-vst-acid/releases/download/acid-vst-v2.1.0/Acid-2.1.0-mpc-armv7.zip", "plugins/acid/thumb.png",
            "TB-303-style acid bassline sequencer: two blendable lines with generate/mutate, driving a synth track over MIDI, with a touchscreen skin and Q-Links.", "./plugins/acid/install.sh", 0,
            false,
            false,
            false,
            { "303", "acid", "midi", "bassline" }
        },
        {
            "cadenza", "Cadenza", "Shrekage", "v0.25.0", "Sequencer",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-29",
            "195", "1.1 MB", "40%",
            "Cadenza - VST - Cadenza", "cadenza.so",
            "https://raw.githubusercontent.com/Shrekage2024/cadenza-force/main/docs/screenshot.png", "https://github.com/Shrekage2024/cadenza-force/releases/download/v0.25.0/Cadenza-0.25.0-mpc-armv7.zip", "plugins/cadenza/thumb.png",
            "Chord-driven generative MIDI sequencer: pick a key and scale, shape chords, bass and melody with a few knobs per page, and it generates and follows along as you play.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Cadenza - VST - Cadenza\" && if [ -f \"./plugins/cadenza/cadenza.so\" ]; then cp -rf ./plugins/cadenza/* \"$TARGET_SYNTH_DIR/Cadenza - VST - Cadenza/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Shrekage2024/cadenza-force/releases/download/v0.25.0/Cadenza-0.25.0-mpc-armv7.zip\" \"/tmp/cadenza.zip\") && unzip -o /tmp/cadenza.zip -d \"$TARGET_SYNTH_DIR/Cadenza - VST - Cadenza/\" && rm -f /tmp/cadenza.zip; fi && sync", 1,
            false,
            false,
            false,
            { "chords", "generative", "midi", "bassline" }
        },
        {
            "chordsmith", "Chordsmith", "jacob-sabella", "v1.0.4", "Utility",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "3070", "4.2 MB", "8%",
            "jacob-sabella - VST - Chordsmith", "chordsmith.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-chordsmith/188d34ca0d9482d2d1ba840edb12ccd8c3773f76/docs/screenshots/chordsmith.png", "https://github.com/jacob-sabella/mpc-vst-chordsmith/releases/download/chordsmith-vst-v1.0.4/Chordsmith-1.0.4-mpc-armv7.zip", "plugins/chordsmith/thumb.png",
            "Chord tool: pads, keys or chord buttons play scale, set or custom chords with voicing, strum, arp and harp modes, MIDI out to other tracks and a built-in preview synth.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Chordsmith\" && if [ -f \"./plugins/chordsmith/chordsmith.so\" ]; then cp -rf ./plugins/chordsmith/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Chordsmith/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-vst-chordsmith/releases/download/chordsmith-vst-v1.0.4/Chordsmith-1.0.4-mpc-armv7.zip\" \"/tmp/chordsmith.zip\") && unzip -o /tmp/chordsmith.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Chordsmith/\" && rm -f /tmp/chordsmith.zip; fi && sync", 1,
            false,
            false,
            false,
            { "chords", "midi", "arpeggiator", "strum", "chord-builder" }
        },
        {
            "daemon", "Daemon", "sd88me", "v1.0.1", "Sequencer",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "559", "727 KB", "14%",
            "sd88me - VST - Daemon", "daemon.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-daemon/v1.0.0/docs/screenshots/daemon.png", "https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-1.0.1-mpc-armv7.zip", "plugins/daemon/thumb.png",
            "AI generative assistant: type or say what you hear and Daemon writes chords, basslines, melodies and beats in your key and plays them in time on its MIDI port. Agent mode builds the session too (tracks, instruments, presets, effects, clips) as a plan you RUN. Gemini or Claude; needs network and an API key.", "./plugins/daemon/install.sh", 0,
            false,
            false,
            false,
            { "generative", "ai", "midi", "chords", "melody", "beats", "agent", "voice" }
        },
        {
            "grids", "Drum Map", "saustin2010", "v1.2.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1582", "337 KB", "14%",
            "Émilie Gillet - VST - [SEQ] Drum Map", "grids.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-drum-map/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-drum-map/releases/download/v1.2.1/SEQ-Drum-Map-1.2.1-mpc-armv7.zip", "plugins/grids/thumb.png",
            "Drum Map: a topographic drum sequencer that plays kick, snare and hats from MPC's clock through its own MIDI port, from Émilie Gillet's open-source Grids.", "./plugins/grids/install.sh", 0,
            false,
            false,
            false,
            { "drum", "midi", "euclidean", "eurorack" }
        },
        {
            "rampage", "Dual Slope", "saustin2010", "v1.2.1", "Modulation",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1337", "3.1 MB", "14%",
            "Andrew Belt - VST - [SEQ] Dual Slope", "rampage.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-dual-slope/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-dual-slope/releases/download/v1.2.1/SEQ-Dual-Slope-1.2.1-mpc-armv7.zip", "plugins/rampage/thumb.png",
            "Dual Slope, based on Befaco's Rampage VCV Rack module: two rise-and-fall generators (envelopes, LFOs, slew) whose outputs go out of its own MIDI port as CCs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Andrew Belt - VST - [SEQ] Dual Slope\" && if [ -f \"./plugins/rampage/rampage.so\" ]; then cp -rf ./plugins/rampage/* \"$TARGET_SYNTH_DIR/Andrew Belt - VST - [SEQ] Dual Slope/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-dual-slope/releases/download/v1.2.1/SEQ-Dual-Slope-1.2.1-mpc-armv7.zip\" \"/tmp/rampage.zip\") && unzip -o /tmp/rampage.zip -d \"$TARGET_SYNTH_DIR/Andrew Belt - VST - [SEQ] Dual Slope/\" && rm -f /tmp/rampage.zip; fi && sync", 1,
            false,
            false,
            false,
            { "vcv", "envelope", "lfo", "slew", "midi-cc", "eurorack" }
        },
        {
            "eucalypso", "Eucalypso", "saustin2010", "v1.0.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "887", "546 KB", "14%",
            "handcraftedcc - VST - [SEQ] Eucalypso", "eucalypso.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-eucalypso/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-eucalypso/releases/download/v1.0.1/SEQ-Eucalypso-1.0.1-mpc-armv7.zip", "plugins/eucalypso/thumb.png",
            "Four-lane Euclidean sequencer that turns the notes you hold into interlocking rhythms for other tracks.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Eucalypso\" && if [ -f \"./plugins/eucalypso/eucalypso.so\" ]; then cp -rf ./plugins/eucalypso/* \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Eucalypso/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-eucalypso/releases/download/v1.0.1/SEQ-Eucalypso-1.0.1-mpc-armv7.zip\" \"/tmp/eucalypso.zip\") && unzip -o /tmp/eucalypso.zip -d \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Eucalypso/\" && rm -f /tmp/eucalypso.zip; fi && sync", 1,
            false,
            false,
            false,
            { "euclidean", "midi", "polyrhythm", "generative" }
        },
        {
            "euclidier", "Euclidier", "sd88me", "v1.0.8", "Sequencer",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 2.x & 3.x", "2026-10-14",
            "4361", "1.6 MB", "14%",
            "sd88me - VST - Euclidier", "euclidier.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-euclidier/master/docs/img/euclidier.png", "https://github.com/sd88me/mpc-vst-euclidier/releases/download/euclidier-vst-v1.0.8/Euclidier-1.0.8-mpc-armv7.zip", "plugins/euclidier/thumb.png",
            "8-lane Euclidean MIDI sequencer: live pattern circle and lane rows with a play-head, per-lane and global randomise, a drum-mode switch, and a touchscreen skin with Q-Links.", "./plugins/euclidier/install.sh", 0,
            false,
            false,
            false,
            { "euclidean", "midi", "drum", "polyrhythm" }
        },
        {
            "glass-spheres", "Glass Spheres", "Padbangers", "v1.0.0", "Sequencer",
            "MIDI", "Stable", "Akai MPC X (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-04",
            "968", "6.6 MB", "4%",
            "Padbangers - VST - Glass Spheres", "glass_spheres.so",
            "https://raw.githubusercontent.com/FullPace/glass-spheres/glass-spheres-v1.0.0/docs/glass-spheres-main.png", "https://github.com/padbangers/glass-spheres/releases/download/glass-spheres-v1.0.0/Glass-Spheres-1.0.0-mpc-armv7.zip", "plugins/glass-spheres/thumb.png",
            "A random sampler after Mutable Instruments Marbles: random gates, melodies and deja vu loops that play other tracks over MIDI or analog gear over the MPC X's CV jacks, with envelopes, LFOs and a modulation matrix.", "./plugins/glass-spheres/install.sh", 0,
            false,
            false,
            false,
            { "mutable-instruments", "marbles", "random", "generative", "sequencer", "midi", "cv", "eurorack" }
        },
        {
            "groove-bank", "Groove Bank", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1606", "852 KB", "14%",
            "Mission Minnow - VST - [SEQ] Groove Bank", "groovebank.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-groovebank/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-groovebank/releases/download/v1.1.1/SEQ-Groove-Bank-1.1.1-mpc-armv7.zip", "plugins/groove-bank/thumb.png",
            "Replays the chord you hold in genre grooves from its library, with swing, strum and latch, through its own MIDI port.", "./plugins/groove-bank/install.sh", 0,
            false,
            false,
            false,
            { "midi", "chords", "rhythm", "grooves" }
        },
        {
            "maze-sequencer", "Maze Sequencer", "sd88me", "v1.0.7", "Sequencer",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (Gen2) (MPC OS 3.9.1)", "MPC OS 2.x & 3.x", "2026-10-13",
            "4153", "3.0 MB", "1%",
            "sd88me - VST - Maze Sequencer", "maze_seq.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-maze/main/docs/img/maze-sequencer.png", "https://github.com/sd88me/mpc-vst-maze/releases/download/maze-sequencer-vst-v1.0.7/Maze-Sequencer-1.0.7-mpc-armv7.zip", "plugins/maze-sequencer/thumb.png",
            "Dual 8-step generative sequencer (Moog Labyrinth style) with corrupt, scale quantising, two LFOs and per-line MIDI channels, driving other tracks over MIDI.", "./plugins/maze-sequencer/install.sh", 0,
            false,
            false,
            false,
            { "generative", "midi", "moog", "labyrinth" }
        },
        {
            "midi-player", "MIDI Player", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1000", "144 KB", "14%",
            "Charles Vestal - VST - [SEQ] MIDI Player", "midiplayer.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-midiplayer/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-midiplayer/releases/download/v1.1.1/SEQ-MIDI-Player-1.1.1-mpc-armv7.zip", "plugins/midi-player/thumb.png",
            "Plays Standard MIDI Files in time with MPC's tempo and transport, one track or all, through its own MIDI port.", "./plugins/midi-player/install.sh", 0,
            false,
            false,
            false,
            { "midi", "midi-file", "player", "transport-sync" }
        },
        {
            "pixel-walkers", "Pixel Walkers", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1016", "375 KB", "14%",
            "mestela - VST - [SEQ] Pixel Walkers", "pixelwalkers.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-pixelwalkers/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-pixelwalkers/releases/download/v1.1.1/SEQ-Pixel-Walkers-1.1.1-mpc-armv7.zip", "plugins/pixel-walkers/thumb.png",
            "Generative gravity sequencer: each note you play becomes a walker that bounces and retriggers its note as it lands.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/mestela - VST - [SEQ] Pixel Walkers\" && if [ -f \"./plugins/pixel-walkers/pixelwalkers.so\" ]; then cp -rf ./plugins/pixel-walkers/* \"$TARGET_SYNTH_DIR/mestela - VST - [SEQ] Pixel Walkers/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-pixelwalkers/releases/download/v1.1.1/SEQ-Pixel-Walkers-1.1.1-mpc-armv7.zip\" \"/tmp/pixel-walkers.zip\") && unzip -o /tmp/pixel-walkers.zip -d \"$TARGET_SYNTH_DIR/mestela - VST - [SEQ] Pixel Walkers/\" && rm -f /tmp/pixel-walkers.zip; fi && sync", 1,
            false,
            false,
            false,
            { "generative", "midi", "physics", "bouncing" }
        },
        {
            "steprunner-8", "StepRunner-8", "djokenzi", "v1.2.0", "Sequencer",
            "MIDI", "Stable", "MPC Live (3.9.1.2)", "MPC OS 3.x", "2026-10-19",
            "1483", "3.4 MB", "14%",
            "djokenzi - VST - StepRunner-8", "steprunner8.so",
            "https://raw.githubusercontent.com/djokenzi/steprunner-8/76285a3aa34dcdc85bf8e72e3aa18baa7f6428cd/docs/screenshots/steprunner-8.png", "https://github.com/djokenzi/steprunner-8/releases/download/v1.2.0/StepRunner-8-1.2.0-mpc-armv7.zip", "plugins/steprunner-8/thumb.png",
            "8-track, 64-step sequencer with step conditions, step and CC locks, patterns, states and performance effects, driving a drum program over MIDI.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/djokenzi - VST - StepRunner-8\" && if [ -f \"./plugins/steprunner-8/steprunner8.so\" ]; then cp -rf ./plugins/steprunner-8/* \"$TARGET_SYNTH_DIR/djokenzi - VST - StepRunner-8/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/djokenzi/steprunner-8/releases/download/v1.2.0/StepRunner-8-1.2.0-mpc-armv7.zip\" \"/tmp/steprunner-8.zip\") && unzip -o /tmp/steprunner-8.zip -d \"$TARGET_SYNTH_DIR/djokenzi - VST - StepRunner-8/\" && rm -f /tmp/steprunner-8.zip; fi && sync", 1,
            false,
            false,
            false,
            { "midi", "step-sequencer", "drums", "step-locks" }
        },
        {
            "stevequencer", "Stevequencer", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "1114", "1.2 MB", "14%",
            "Steve A - VST - [SEQ] Stevequencer", "stevequencer.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-stevequencer/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-stevequencer/releases/download/v1.1.1/SEQ-Stevequencer-1.1.1-mpc-armv7.zip", "plugins/stevequencer/thumb.png",
            "Melodic step sequencer edited from the Q-Links: 16 steps on a 4x4 grid, up to 64, with chance, ratchets and MOD lanes.", "./plugins/stevequencer/install.sh", 0,
            false,
            false,
            false,
            { "step-sequencer", "midi", "q-links", "melodic" }
        },
        {
            "stevequencer-16", "Stevequencer 16", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "1083", "628 KB", "14%",
            "Steve A - VST - [SEQ] Stevequencer 16", "stevequencer16.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-stevequencer16/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-stevequencer16/releases/download/v1.1.1/SEQ-Stevequencer-16-1.1.1-mpc-armv7.zip", "plugins/stevequencer-16/thumb.png",
            "16-step sequencer with eight modulation lanes that move any parameter of the instrument (CC or NRPN), on any page.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer 16\" && if [ -f \"./plugins/stevequencer-16/stevequencer16.so\" ]; then cp -rf ./plugins/stevequencer-16/* \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer 16/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-stevequencer16/releases/download/v1.1.1/SEQ-Stevequencer-16-1.1.1-mpc-armv7.zip\" \"/tmp/stevequencer-16.zip\") && unzip -o /tmp/stevequencer-16.zip -d \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer 16/\" && rm -f /tmp/stevequencer-16.zip; fi && sync", 1,
            false,
            false,
            false,
            { "step-sequencer", "midi", "modulation", "nrpn", "q-links" }
        },
        {
            "super-arp", "Super Arp", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1121", "2.3 MB", "14%",
            "handcraftedcc - VST - [SEQ] Super Arp", "superarp.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-superarp/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-superarp/releases/download/v1.1.1/SEQ-Super-Arp-1.1.1-mpc-armv7.zip", "plugins/super-arp/thumb.png",
            "Arpeggiator that splits note order from rhythm: 40 patterns, 40 rhythms and seeded random modifiers.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Super Arp\" && if [ -f \"./plugins/super-arp/superarp.so\" ]; then cp -rf ./plugins/super-arp/* \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Super Arp/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-superarp/releases/download/v1.1.1/SEQ-Super-Arp-1.1.1-mpc-armv7.zip\" \"/tmp/super-arp.zip\") && unzip -o /tmp/super-arp.zip -d \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Super Arp/\" && rm -f /tmp/super-arp.zip; fi && sync", 1,
            false,
            false,
            false,
            { "arpeggiator", "midi", "patterns", "rhythm" }
        },
        {
            "piclidean-mpc", "πClideaN MPC", "Timo", "v1.3.2", "Sequencer",
            "MIDI", "Stable", "Akai Force (Gen1) (3.10.0.49)", "MPC OS 3.x", "2026-11-06",
            "162", "5.4 MB", "14%",
            "Timo - VST - πClideaN MPC", "piclidean_mpc.so",
            "https://raw.githubusercontent.com/timomacquis/piClideaN16-MPC/48c2560cb46bcec86b8f473844d38b3b37a21709/Cover.png", "https://github.com/timomacquis/piClideaN16-MPC/releases/download/v1.3.2/ClideaN-MPC-1.3.2-mpc-armv7.zip", "plugins/piclidean-mpc/thumb.png",
            "Sixteen-lane hybrid MIDI sequencer with Euclidean patterns, per-step expression, scales, selective randomization and dual-LFO modulation.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Timo - VST - πClideaN MPC\" && if [ -f \"./plugins/piclidean-mpc/piclidean_mpc.so\" ]; then cp -rf ./plugins/piclidean-mpc/* \"$TARGET_SYNTH_DIR/Timo - VST - πClideaN MPC/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/timomacquis/piClideaN16-MPC/releases/download/v1.3.2/ClideaN-MPC-1.3.2-mpc-armv7.zip\" \"/tmp/piclidean-mpc.zip\") && unzip -o /tmp/piclidean-mpc.zip -d \"$TARGET_SYNTH_DIR/Timo - VST - πClideaN MPC/\" && rm -f /tmp/piclidean-mpc.zip; fi && sync", 1,
            false,
            false,
            false,
            { "midi", "sequencer", "euclidean", "generative" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "EFFECTS";
        sec.items = {
        {
            "airwindows-air3", "Airwindows Air3", "mmiroshnikov", "v1.0.0", "Eq",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1226", "745 KB", "14%",
            "Airwindows - VST - Air3", "air3.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Air3.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Air3-1.0.0-mpc-armv7.zip", "plugins/airwindows-air3/thumb.png",
            "Air-band EQ that lifts or tames the very top and the low ground of a mix.", "./plugins/airwindows-air3/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "eq", "air", "treble" }
        },
        {
            "airwindows-buttercomp2", "Airwindows ButterComp2", "mmiroshnikov", "v1.0.0", "Compressor",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1198", "757 KB", "14%",
            "Airwindows - VST - ButterComp2", "buttercomp2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ButterComp2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ButterComp2-1.0.0-mpc-armv7.zip", "plugins/airwindows-buttercomp2/thumb.png",
            "Smooth, buttery bipolar compressor that stays transparent even when pushed.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ButterComp2\" && if [ -f \"./plugins/airwindows-buttercomp2/buttercomp2.so\" ]; then cp -rf ./plugins/airwindows-buttercomp2/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ButterComp2/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ButterComp2-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-buttercomp2.zip\") && unzip -o /tmp/airwindows-buttercomp2.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ButterComp2/\" && rm -f /tmp/airwindows-buttercomp2.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "compressor", "dynamics" }
        },
        {
            "airwindows-chamber2", "Airwindows Chamber2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1146", "702 KB", "14%",
            "Airwindows - VST - Chamber2", "chamber2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Chamber2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Chamber2-1.0.0-mpc-armv7.zip", "plugins/airwindows-chamber2/thumb.png",
            "Chamber reverb with delay, regeneration and thickness controls.", "./plugins/airwindows-chamber2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "chamber" }
        },
        {
            "airwindows-chorusensemble", "Airwindows ChorusEnsemble", "mmiroshnikov", "v1.0.0", "Chorus",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1151", "647 KB", "14%",
            "Airwindows - VST - ChorusEnsemble", "chorusensemble.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ChorusEnsemble.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ChorusEnsemble-1.0.0-mpc-armv7.zip", "plugins/airwindows-chorusensemble/thumb.png",
            "Lush multi-tap ensemble chorus.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ChorusEnsemble\" && if [ -f \"./plugins/airwindows-chorusensemble/chorusensemble.so\" ]; then cp -rf ./plugins/airwindows-chorusensemble/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ChorusEnsemble/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ChorusEnsemble-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-chorusensemble.zip\") && unzip -o /tmp/airwindows-chorusensemble.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ChorusEnsemble/\" && rm -f /tmp/airwindows-chorusensemble.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "chorus", "ensemble", "modulation" }
        },
        {
            "aw-ambience", "Airwindows Collection: Ambience", "sd88me", "v0.1.0", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "177", "10.6 MB", "14%",
            "Airwindows - VST - AW Ambience", "aw_ambience.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/ambience.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Ambience-0.1.0-mpc-armv7.zip", "plugins/aw-ambience/thumb.png",
            "The Airwindows Ambience collection: all 26 ambience, delay, chorus and echo effects in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-ambience/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "delay", "collection" }
        },
        {
            "aw-console-strips", "Airwindows Collection: Console Strips", "sd88me", "v0.1.0", "Mixing",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "182", "15.2 MB", "14%",
            "Airwindows - VST - AW Console Strips", "aw_consolestrips.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/consolestrips.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Console-Strips-0.1.0-mpc-armv7.zip", "plugins/aw-console-strips/thumb.png",
            "The Airwindows Console Strips collection: all 8 big console channel strips (ConsoleX and ConsoleH), spread over four pages in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Console Strips\" && if [ -f \"./plugins/aw-console-strips/aw_consolestrips.so\" ]; then cp -rf ./plugins/aw-console-strips/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Console Strips/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Console-Strips-0.1.0-mpc-armv7.zip\" \"/tmp/aw-console-strips.zip\") && unzip -o /tmp/aw-console-strips.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Console Strips/\" && rm -f /tmp/aw-console-strips.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "channel-strip", "collection" }
        },
        {
            "aw-consoles", "Airwindows Collection: Consoles", "sd88me", "v0.1.0", "Mixing",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "170", "10.5 MB", "14%",
            "Airwindows - VST - AW Consoles", "aw_consoles.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/consoles.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Consoles-0.1.0-mpc-armv7.zip", "plugins/aw-consoles/thumb.png",
            "The Airwindows Consoles collection: all 50 console and mix-buss systems (Console 0 to 9, ConsoleLA, MC, MD and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-consoles/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "console", "collection" }
        },
        {
            "aw-distortion", "Airwindows Collection: Distortion", "sd88me", "v0.1.0", "Distortion",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "175", "10.0 MB", "14%",
            "Airwindows - VST - AW Distortion", "aw_distortion.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/distortion.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Distortion-0.1.0-mpc-armv7.zip", "plugins/aw-distortion/thumb.png",
            "The Airwindows Distortion collection: all 15 distortions (Density, Drive, Edge, Mackity, HardVacuum and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Distortion\" && if [ -f \"./plugins/aw-distortion/aw_distortion.so\" ]; then cp -rf ./plugins/aw-distortion/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Distortion/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Distortion-0.1.0-mpc-armv7.zip\" \"/tmp/aw-distortion.zip\") && unzip -o /tmp/aw-distortion.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Distortion/\" && rm -f /tmp/aw-distortion.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "distortion", "collection" }
        },
        {
            "aw-dynamics", "Airwindows Collection: Dynamics", "sd88me", "v0.1.0", "Dynamics",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "173", "13.8 MB", "14%",
            "Airwindows - VST - AW Dynamics", "aw_dynamics.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/dynamics.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Dynamics-0.1.0-mpc-armv7.zip", "plugins/aw-dynamics/thumb.png",
            "The Airwindows Dynamics collection: all 35 compressors, gates and dynamics tools in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-dynamics/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "compressor", "collection" }
        },
        {
            "aw-effects", "Airwindows Collection: Effects", "sd88me", "v0.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "183", "12.2 MB", "14%",
            "Airwindows - VST - AW Effects", "aw_effects.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/effects.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Effects-0.1.0-mpc-armv7.zip", "plugins/aw-effects/thumb.png",
            "The Airwindows Effects collection: all 29 effects (Pafnuty, tremolo, pitch shifters, wavefolders, exciters and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Effects\" && if [ -f \"./plugins/aw-effects/aw_effects.so\" ]; then cp -rf ./plugins/aw-effects/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Effects/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Effects-0.1.0-mpc-armv7.zip\" \"/tmp/aw-effects.zip\") && unzip -o /tmp/aw-effects.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Effects/\" && rm -f /tmp/aw-effects.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "effects", "collection" }
        },
        {
            "aw-filter", "Airwindows Collection: Filter", "sd88me", "v0.1.0", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "169", "13.7 MB", "14%",
            "Airwindows - VST - AW Filter", "aw_filter.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/filter.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Filter-0.1.0-mpc-armv7.zip", "plugins/aw-filter/thumb.png",
            "The Airwindows Filter collection: all 54 filters and EQs (Baxandall, Isolator, Holt, SmoothEQ and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-filter/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "eq", "collection" }
        },
        {
            "aw-reverb", "Airwindows Collection: Reverb", "sd88me", "v0.1.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "177", "10.7 MB", "14%",
            "Airwindows - VST - AW Reverb", "aw_reverb.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/reverb.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Reverb-0.1.0-mpc-armv7.zip", "plugins/aw-reverb/thumb.png",
            "The Airwindows Reverb collection: all 42 reverbs (the k-series, plates, Galactic, Verbity and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Reverb\" && if [ -f \"./plugins/aw-reverb/aw_reverb.so\" ]; then cp -rf ./plugins/aw-reverb/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Reverb/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Reverb-0.1.0-mpc-armv7.zip\" \"/tmp/aw-reverb.zip\") && unzip -o /tmp/aw-reverb.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Reverb/\" && rm -f /tmp/aw-reverb.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "reverb", "collection" }
        },
        {
            "aw-saturation", "Airwindows Collection: Saturation", "sd88me", "v0.1.0", "Saturation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "177", "10.9 MB", "14%",
            "Airwindows - VST - AW Saturation", "aw_saturation.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/saturation.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Saturation-0.1.0-mpc-armv7.zip", "plugins/aw-saturation/thumb.png",
            "The Airwindows Saturation collection: all 20 saturation models (Tube, Mojo, Focus, Spiral2 and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-saturation/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "saturation", "collection" }
        },
        {
            "aw-tape", "Airwindows Collection: Tape", "sd88me", "v0.1.0", "Tape",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "178", "10.6 MB", "14%",
            "Airwindows - VST - AW Tape", "aw_tape.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/tape.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Tape-0.1.0-mpc-armv7.zip", "plugins/aw-tape/thumb.png",
            "The Airwindows Tape collection: all 12 tape emulations (ToTape, TapeHack, IronOxide and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Tape\" && if [ -f \"./plugins/aw-tape/aw_tape.so\" ]; then cp -rf ./plugins/aw-tape/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Tape/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Tape-0.1.0-mpc-armv7.zip\" \"/tmp/aw-tape.zip\") && unzip -o /tmp/aw-tape.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Tape/\" && rm -f /tmp/aw-tape.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "tape", "collection" }
        },
        {
            "aw-utility", "Airwindows Collection: Utility", "sd88me", "v0.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "160", "13.5 MB", "14%",
            "Airwindows - VST - AW Utility", "aw_utility.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/utility.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Utility-0.1.0-mpc-armv7.zip", "plugins/aw-utility/thumb.png",
            "The Airwindows Utility collection: all 38 utilities (gain, pan, trims, mid/side, monitoring, mix checks and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-utility/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "utility", "collection" }
        },
        {
            "airwindows-console7channel", "Airwindows Console7Channel", "mmiroshnikov", "v1.0.0", "Saturation",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1145", "737 KB", "14%",
            "Airwindows - VST - Console7Channel", "console7channel.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Console7Channel.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Console7Channel-1.0.0-mpc-armv7.zip", "plugins/airwindows-console7channel/thumb.png",
            "Console7 channel stage: analog-style console saturation behind a fader.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Console7Channel\" && if [ -f \"./plugins/airwindows-console7channel/console7channel.so\" ]; then cp -rf ./plugins/airwindows-console7channel/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Console7Channel/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Console7Channel-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-console7channel.zip\") && unzip -o /tmp/airwindows-console7channel.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Console7Channel/\" && rm -f /tmp/airwindows-console7channel.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "console", "saturation", "summing" }
        },
        {
            "airwindows-density2", "Airwindows Density2", "mmiroshnikov", "v1.0.0", "Saturation",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1158", "648 KB", "14%",
            "Airwindows - VST - Density2", "density2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Density2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Density2-1.0.0-mpc-armv7.zip", "plugins/airwindows-density2/thumb.png",
            "Density saturation with a highpass, for thickening and fattening sounds.", "./plugins/airwindows-density2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "saturation", "distortion", "density" }
        },
        {
            "airwindows-galactic", "Airwindows Galactic", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1216", "835 KB", "14%",
            "Airwindows - VST - Galactic", "galactic.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Galactic.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic-1.0.0-mpc-armv7.zip", "plugins/airwindows-galactic/thumb.png",
            "Huge, dark, shimmering ambient space reverb.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic\" && if [ -f \"./plugins/airwindows-galactic/galactic.so\" ]; then cp -rf ./plugins/airwindows-galactic/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-galactic.zip\") && unzip -o /tmp/airwindows-galactic.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic/\" && rm -f /tmp/airwindows-galactic.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "reverb", "ambient", "shimmer" }
        },
        {
            "airwindows-galactic2", "Airwindows Galactic2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1143", "816 KB", "14%",
            "Airwindows - VST - Galactic2", "galactic2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Galactic2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic2-1.0.0-mpc-armv7.zip", "plugins/airwindows-galactic2/thumb.png",
            "Galactic reverb with drive, sustain and darkening for vast ambient tails.", "./plugins/airwindows-galactic2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "ambient" }
        },
        {
            "airwindows-galactic3", "Airwindows Galactic3", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1161", "692 KB", "14%",
            "Airwindows - VST - Galactic3", "galactic3.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Galactic3.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic3-1.0.0-mpc-armv7.zip", "plugins/airwindows-galactic3/thumb.png",
            "Galactic reverb with brightness, detune, derez and size controls.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic3\" && if [ -f \"./plugins/airwindows-galactic3/galactic3.so\" ]; then cp -rf ./plugins/airwindows-galactic3/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic3/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic3-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-galactic3.zip\") && unzip -o /tmp/airwindows-galactic3.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic3/\" && rm -f /tmp/airwindows-galactic3.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "reverb", "ambient" }
        },
        {
            "airwindows-galacticvibe", "Airwindows GalacticVibe", "mmiroshnikov", "v1.0.0", "Chorus",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1113", "702 KB", "14%",
            "Airwindows - VST - GalacticVibe", "galacticvibe.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/GalacticVibe.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/GalacticVibe-1.0.0-mpc-armv7.zip", "plugins/airwindows-galacticvibe/thumb.png",
            "The pitch-wobble vibrato stage of the Galactic reverbs on its own.", "./plugins/airwindows-galacticvibe/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "vibrato", "modulation", "chorus" }
        },
        {
            "airwindows-infinity2", "Airwindows Infinity2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1153", "622 KB", "14%",
            "Airwindows - VST - Infinity2", "infinity2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Infinity2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Infinity2-1.0.0-mpc-armv7.zip", "plugins/airwindows-infinity2/thumb.png",
            "Infinite-sustain reverb with filtering, damping and allpass diffusion.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Infinity2\" && if [ -f \"./plugins/airwindows-infinity2/infinity2.so\" ]; then cp -rf ./plugins/airwindows-infinity2/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Infinity2/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Infinity2-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-infinity2.zip\") && unzip -o /tmp/airwindows-infinity2.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Infinity2/\" && rm -f /tmp/airwindows-infinity2.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "reverb", "ambient", "freeze" }
        },
        {
            "airwindows-kalienspaceship", "Airwindows kAlienSpaceship", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1135", "721 KB", "14%",
            "Airwindows - VST - kAlienSpaceship", "kalienspaceship.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/kAlienSpaceship.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kAlienSpaceship-1.0.0-mpc-armv7.zip", "plugins/airwindows-kalienspaceship/thumb.png",
            "Enormous sci-fi space reverb from the kCathedral family.", "./plugins/airwindows-kalienspaceship/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "sci-fi", "ambient" }
        },
        {
            "airwindows-kcathedral5", "Airwindows kCathedral5", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1097", "599 KB", "14%",
            "Airwindows - VST - kCathedral5", "kcathedral5.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/kCathedral5.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCathedral5-1.0.0-mpc-armv7.zip", "plugins/airwindows-kcathedral5/thumb.png",
            "Cathedral-sized reverb with early reflections and position control.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - kCathedral5\" && if [ -f \"./plugins/airwindows-kcathedral5/kcathedral5.so\" ]; then cp -rf ./plugins/airwindows-kcathedral5/* \"$TARGET_SYNTH_DIR/Airwindows - VST - kCathedral5/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCathedral5-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-kcathedral5.zip\") && unzip -o /tmp/airwindows-kcathedral5.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - kCathedral5/\" && rm -f /tmp/airwindows-kcathedral5.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "reverb", "cathedral" }
        },
        {
            "airwindows-kcosmos", "Airwindows kCosmos", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1134", "715 KB", "14%",
            "Airwindows - VST - kCosmos", "kcosmos.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/kCosmos.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCosmos-1.0.0-mpc-armv7.zip", "plugins/airwindows-kcosmos/thumb.png",
            "Vast cosmic reverb with predelay and early reflections.", "./plugins/airwindows-kcosmos/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "ambient", "cosmic" }
        },
        {
            "airwindows-mackity", "Airwindows Mackity", "mmiroshnikov", "v1.0.0", "Saturation",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-05",
            "994", "587 KB", "14%",
            "Airwindows - VST - Mackity", "mackity.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Mackity.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.1.0/Mackity-1.0.0-mpc-armv7.zip", "plugins/airwindows-mackity/thumb.png",
            "Mackie-style console distortion: drive it with the input trim, tame it with the output pad.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Mackity\" && if [ -f \"./plugins/airwindows-mackity/mackity.so\" ]; then cp -rf ./plugins/airwindows-mackity/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Mackity/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.1.0/Mackity-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-mackity.zip\") && unzip -o /tmp/airwindows-mackity.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Mackity/\" && rm -f /tmp/airwindows-mackity.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "saturation", "distortion", "console" }
        },
        {
            "airwindows-matrixverb", "Airwindows MatrixVerb", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1109", "730 KB", "14%",
            "Airwindows - VST - MatrixVerb", "matrixverb.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/MatrixVerb.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/MatrixVerb-1.0.0-mpc-armv7.zip", "plugins/airwindows-matrixverb/thumb.png",
            "Classic algorithmic reverb with filter, damping, modulation and room size.", "./plugins/airwindows-matrixverb/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "algorithmic" }
        },
        {
            "airwindows-pitchdelay", "Airwindows PitchDelay", "mmiroshnikov", "v1.0.0", "Delay",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1140", "618 KB", "14%",
            "Airwindows - VST - PitchDelay", "pitchdelay.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/PitchDelay.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/PitchDelay-1.0.0-mpc-armv7.zip", "plugins/airwindows-pitchdelay/thumb.png",
            "Delay with pitch shifting in the feedback path and a resonant filter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - PitchDelay\" && if [ -f \"./plugins/airwindows-pitchdelay/pitchdelay.so\" ]; then cp -rf ./plugins/airwindows-pitchdelay/* \"$TARGET_SYNTH_DIR/Airwindows - VST - PitchDelay/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/PitchDelay-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-pitchdelay.zip\") && unzip -o /tmp/airwindows-pitchdelay.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - PitchDelay/\" && rm -f /tmp/airwindows-pitchdelay.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "delay", "pitch", "filter" }
        },
        {
            "airwindows-tapedelay2", "Airwindows TapeDelay2", "mmiroshnikov", "v1.0.0", "Delay",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1189", "548 KB", "14%",
            "Airwindows - VST - TapeDelay2", "tapedelay2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/TapeDelay2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/TapeDelay2-1.0.0-mpc-armv7.zip", "plugins/airwindows-tapedelay2/thumb.png",
            "Tape-style delay with flutter and a resonant filter in the repeats.", "./plugins/airwindows-tapedelay2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "delay", "tape", "flutter" }
        },
        {
            "airwindows-tovinyl4", "Airwindows ToVinyl4", "mmiroshnikov", "v1.0.0", "Utility",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1095", "738 KB", "14%",
            "Airwindows - VST - ToVinyl4", "tovinyl4.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ToVinyl4.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ToVinyl4-1.0.0-mpc-armv7.zip", "plugins/airwindows-tovinyl4/thumb.png",
            "Vinyl mastering stage: mid and side highpass, high limiter and groove wear.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ToVinyl4\" && if [ -f \"./plugins/airwindows-tovinyl4/tovinyl4.so\" ]; then cp -rf ./plugins/airwindows-tovinyl4/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ToVinyl4/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ToVinyl4-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-tovinyl4.zip\") && unzip -o /tmp/airwindows-tovinyl4.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ToVinyl4/\" && rm -f /tmp/airwindows-tovinyl4.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "vinyl", "mastering", "lo-fi" }
        },
        {
            "airwindows-verbity2", "Airwindows Verbity2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1090", "779 KB", "14%",
            "Airwindows - VST - Verbity2", "verbity2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Verbity2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Verbity2-1.0.0-mpc-armv7.zip", "plugins/airwindows-verbity2/thumb.png",
            "Dense, smooth reverb with size, sustain and mulch controls.", "./plugins/airwindows-verbity2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "dense" }
        },
        {
            "airwindows-zacidlowpass", "Airwindows ZAcidLowpass", "mmiroshnikov", "v1.0.0", "Filter",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1064", "685 KB", "14%",
            "Airwindows - VST - ZAcidLowpass", "zacidlowpass.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ZAcidLowpass.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZAcidLowpass-1.0.0-mpc-armv7.zip", "plugins/airwindows-zacidlowpass/thumb.png",
            "Acid-style overdriven resonant lowpass filter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ZAcidLowpass\" && if [ -f \"./plugins/airwindows-zacidlowpass/zacidlowpass.so\" ]; then cp -rf ./plugins/airwindows-zacidlowpass/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ZAcidLowpass/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZAcidLowpass-1.0.0-mpc-armv7.zip\" \"/tmp/airwindows-zacidlowpass.zip\") && unzip -o /tmp/airwindows-zacidlowpass.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ZAcidLowpass/\" && rm -f /tmp/airwindows-zacidlowpass.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "filter", "lowpass", "acid" }
        },
        {
            "airwindows-zlowpass2", "Airwindows ZLowpass2", "mmiroshnikov", "v1.0.0", "Filter",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1061", "694 KB", "14%",
            "Airwindows - VST - ZLowpass2", "zlowpass2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ZLowpass2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZLowpass2-1.0.0-mpc-armv7.zip", "plugins/airwindows-zlowpass2/thumb.png",
            "Smooth lowpass filter with a choice of slopes.", "./plugins/airwindows-zlowpass2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "filter", "lowpass" }
        },
        {
            "boris-granular", "Boris Granular", "sd88me", "v1.0.1", "Granular",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "1346", "10.3 MB", "14%",
            "sd88me - VST - Boris Granular", "boris_granular.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-boris/2d7b1360c68ccc91b22b0c5030152e08dd3682b8/docs/screenshot-force.png", "https://github.com/sd88me/mpc-vst-boris/releases/download/v1.0.1/Boris-Granular-1.0.1-mpc-armv7.zip", "plugins/boris-granular/thumb.png",
            "Real-time granular insert effect: incoming audio fills a 10 s buffer and up to 24 overlapping grains replay it with random size, delay, pitch, direction and pan. MPC tempo sync, freeze, feedback, 22 presets and a one-page touchscreen skin with Q-Links. A port of Boris Granular Station by Alessandro Gaiba.", "./plugins/boris-granular/install.sh", 0,
            false,
            false,
            false,
            { "granular", "freeze", "tempo-sync", "texture", "pads", "shimmer", "stutter" }
        },
        {
            "clif-tape", "Clif Tape", "MagicStino", "v0.2.0", "Effect",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "222", "1.6 MB", "14%",
            "MagicStino - VST - Clif Tape", "clif_tape.so",
            "https://raw.githubusercontent.com/MagicStino/Clif-Tape/v0.2.0/catalog-thumbnail.png", "https://github.com/MagicStino/Clif-Tape/releases/download/v0.2.0/Clif-Tape-0.2.0-mpc-armv7.zip", "plugins/clif-tape/thumb.png",
            "Stereo tape colour and digital degradation, expressive Wow/Flutter, twelve tone profiles, crackle, stereo spread, independent noise textures and Silence Gate. Fifteen presets and touchscreen Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MagicStino - VST - Clif Tape\" && if [ -f \"./plugins/clif-tape/clif_tape.so\" ]; then cp -rf ./plugins/clif-tape/* \"$TARGET_SYNTH_DIR/MagicStino - VST - Clif Tape/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/MagicStino/Clif-Tape/releases/download/v0.2.0/Clif-Tape-0.2.0-mpc-armv7.zip\" \"/tmp/clif-tape.zip\") && unzip -o /tmp/clif-tape.zip -d \"$TARGET_SYNTH_DIR/MagicStino - VST - Clif Tape/\" && rm -f /tmp/clif-tape.zip; fi && sync", 1,
            false,
            false,
            false,
            { "tape", "lo-fi", "stereo", "effect" }
        },
        {
            "op1fx-cwo", "Cwo", "Bombest", "v0.4.1", "Delay",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "124", "4.2 MB", "14%",
            "bombest - VST - Cwo", "cwo.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/cwo.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cwo-0.4.1-mpc-armv7.zip", "plugins/op1fx-cwo/thumb.png",
            "Stereo delay with a frequency shifter in the feedback loop, plus a MIX knob. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Cwo\" && if [ -f \"./plugins/op1fx-cwo/cwo.so\" ]; then cp -rf ./plugins/op1fx-cwo/* \"$TARGET_SYNTH_DIR/bombest - VST - Cwo/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cwo-0.4.1-mpc-armv7.zip\" \"/tmp/op1fx-cwo.zip\") && unzip -o /tmp/op1fx-cwo.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Cwo/\" && rm -f /tmp/op1fx-cwo.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "delay", "frequency-shifter", "feedback" }
        },
        {
            "daemon-listen", "Daemon Listen", "sd88me", "v1.0.1", "Utility",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "505", "64 KB", "14%",
            "sd88me - VST - Daemon Listen", "daemonlisten.so",
            "", "https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-Listen-1.0.1-mpc-armv7.zip", "plugins/daemon-listen/thumb.png",
            "Companion to Daemon: insert it on the audio track that carries your mic, and Daemon's TALK button hears you. Passes audio through unchanged. Needs Daemon in the same project.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon Listen\" && if [ -f \"./plugins/daemon-listen/daemonlisten.so\" ]; then cp -rf ./plugins/daemon-listen/* \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon Listen/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-Listen-1.0.1-mpc-armv7.zip\" \"/tmp/daemon-listen.zip\") && unzip -o /tmp/daemon-listen.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon Listen/\" && rm -f /tmp/daemon-listen.zip; fi && sync", 1,
            false,
            false,
            false,
            { "voice", "mic", "ai" }
        },
        {
            "op1fx-delay", "Delay", "Bombest", "v0.4.1", "Delay",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "126", "4.2 MB", "14%",
            "bombest - VST - Delay", "delay.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/delay.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Delay-0.4.1-mpc-armv7.zip", "plugins/op1fx-delay/thumb.png",
            "Four-knob delay with a 12-preset bank. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Delay\" && if [ -f \"./plugins/op1fx-delay/delay.so\" ]; then cp -rf ./plugins/op1fx-delay/* \"$TARGET_SYNTH_DIR/bombest - VST - Delay/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Delay-0.4.1-mpc-armv7.zip\" \"/tmp/op1fx-delay.zip\") && unzip -o /tmp/op1fx-delay.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Delay/\" && rm -f /tmp/op1fx-delay.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "delay", "echo", "tape" }
        },
        {
            "dragonfly-early-reflections", "Dragonfly Early Reflections", "gmorb", "v1.3.1", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "205", "766 KB", "14%",
            "Dragonfly - VST - Early Reflections", "dragonfly_early.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-early-reflections/2044beed9f510a3963df7b61da6a4c19c6c7e7b3/docs/banners/early.png", "https://github.com/gmorb/mpc-vst-dragonfly-early-reflections/releases/download/v1.3.1/Early-Reflections-1.3.1-mpc-armv7.zip", "plugins/dragonfly-early-reflections/thumb.png",
            "Dragonfly Early Reflections as a native insert effect, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Dragonfly - VST - Early Reflections\" && if [ -f \"./plugins/dragonfly-early-reflections/dragonfly_early.so\" ]; then cp -rf ./plugins/dragonfly-early-reflections/* \"$TARGET_SYNTH_DIR/Dragonfly - VST - Early Reflections/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/gmorb/mpc-vst-dragonfly-early-reflections/releases/download/v1.3.1/Early-Reflections-1.3.1-mpc-armv7.zip\" \"/tmp/dragonfly-early-reflections.zip\") && unzip -o /tmp/dragonfly-early-reflections.zip -d \"$TARGET_SYNTH_DIR/Dragonfly - VST - Early Reflections/\" && rm -f /tmp/dragonfly-early-reflections.zip; fi && sync", 1,
            false,
            false,
            false,
            { "reverb", "early-reflections", "dragonfly" }
        },
        {
            "dragonfly-hall", "Dragonfly Hall", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "1087", "2.4 MB", "14%",
            "Dragonfly - VST - Hall", "dragonfly_hall.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-hall/eb9f41f22ea93414776db40760c2387b36cf3144/docs/banners/hall.png", "https://github.com/gmorb/mpc-vst-dragonfly-hall/releases/download/v1.3.0/Hall-1.3.0-mpc-armv7.zip", "plugins/dragonfly-hall/thumb.png",
            "Dragonfly Hall reverb as a native insert effect, with a touchscreen skin and Q-Links.", "./plugins/dragonfly-hall/install.sh", 0,
            false,
            false,
            false,
            { "reverb", "hall", "dragonfly" }
        },
        {
            "dragonfly-plate", "Dragonfly Plate", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "1051", "1.2 MB", "14%",
            "Dragonfly - VST - Plate", "dragonfly_plate.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-plate/a9087385948d18ae38739c44ddf42ab826d1a567/docs/banners/plate.png", "https://github.com/gmorb/mpc-vst-dragonfly-plate/releases/download/v1.3.0/Plate-1.3.0-mpc-armv7.zip", "plugins/dragonfly-plate/thumb.png",
            "Dragonfly Plate reverb as a native insert effect, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Dragonfly - VST - Plate\" && if [ -f \"./plugins/dragonfly-plate/dragonfly_plate.so\" ]; then cp -rf ./plugins/dragonfly-plate/* \"$TARGET_SYNTH_DIR/Dragonfly - VST - Plate/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/gmorb/mpc-vst-dragonfly-plate/releases/download/v1.3.0/Plate-1.3.0-mpc-armv7.zip\" \"/tmp/dragonfly-plate.zip\") && unzip -o /tmp/dragonfly-plate.zip -d \"$TARGET_SYNTH_DIR/Dragonfly - VST - Plate/\" && rm -f /tmp/dragonfly-plate.zip; fi && sync", 1,
            false,
            false,
            false,
            { "reverb", "plate", "dragonfly" }
        },
        {
            "dragonfly-room", "Dragonfly Room", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "1043", "2.0 MB", "14%",
            "Dragonfly - VST - Room", "dragonfly_room.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-room/6c62e1a5c9ef57f0c31897afa0faf07d7a446585/docs/banners/room.png", "https://github.com/gmorb/mpc-vst-dragonfly-room/releases/download/v1.3.0/Room-1.3.0-mpc-armv7.zip", "plugins/dragonfly-room/thumb.png",
            "Dragonfly Room reverb as a native insert effect, with a touchscreen skin and Q-Links.", "./plugins/dragonfly-room/install.sh", 0,
            false,
            false,
            false,
            { "reverb", "room", "dragonfly" }
        },
        {
            "drips", "Drips", "Padbangers", "v1.0.0", "Granular",
            "FX", "Stable", "Akai MPC X (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-13",
            "664", "3.2 MB", "10%",
            "Padbangers - VST - Drips", "drips.so",
            "https://raw.githubusercontent.com/padbangers/drips/85683cebf7b4fcf1716ed6f32faa89041768b24e/docs/drips-main.png", "https://github.com/padbangers/drips/releases/download/drips-v1.0.0/Drips-1.0.0-mpc-armv7.zip", "plugins/drips/thumb.png",
            "Drips: a granular texture synthesizer with four recording qualities, a delay and beat slicer, tempo-synced seeding, wavetables and a modulation page, from Émilie Gillet's open-source Beads.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Padbangers - VST - Drips\" && if [ -f \"./plugins/drips/drips.so\" ]; then cp -rf ./plugins/drips/* \"$TARGET_SYNTH_DIR/Padbangers - VST - Drips/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/padbangers/drips/releases/download/drips-v1.0.0/Drips-1.0.0-mpc-armv7.zip\" \"/tmp/drips.zip\") && unzip -o /tmp/drips.zip -d \"$TARGET_SYNTH_DIR/Padbangers - VST - Drips/\" && rm -f /tmp/drips.zip; fi && sync", 1,
            false,
            false,
            false,
            { "granular", "texture", "delay", "beat-slicer", "wavetable", "reverb", "tempo-sync" }
        },
        {
            "dub-station", "Dub Station", "nachtaktiv303", "v1.0.2", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1393", "2.1 MB", "14%",
            "Lukas - VST - Dub Station", "dub_station.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-dub-station/main/preview_0.png", "https://github.com/nachtaktiv303/mpc-vst-dub-station/releases/download/dub-station-vst-v1.0.2/Dub-Station-1.0.2-mpc-armv7.zip", "plugins/dub-station/thumb.png",
            "Dub-techno multi-effect: two independent tape delays, a huge modulated reverb, tape grit, a noise/crackle ambience bed, phaser, ducking and a panic kill.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Lukas - VST - Dub Station\" && if [ -f \"./plugins/dub-station/dub_station.so\" ]; then cp -rf ./plugins/dub-station/* \"$TARGET_SYNTH_DIR/Lukas - VST - Dub Station/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/nachtaktiv303/mpc-vst-dub-station/releases/download/dub-station-vst-v1.0.2/Dub-Station-1.0.2-mpc-armv7.zip\" \"/tmp/dub-station.zip\") && unzip -o /tmp/dub-station.zip -d \"$TARGET_SYNTH_DIR/Lukas - VST - Dub Station/\" && rm -f /tmp/dub-station.zip; fi && sync", 1,
            false,
            false,
            false,
            { "dub", "delay", "tape", "reverb", "space-echo", "multi-effect" }
        },
        {
            "effectwerk", "EffectWerk", "Devko", "v0.0.5", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "184", "3.0 MB", "14%",
            "Devko - VST - EffectWerk", "effectwerk.so",
            "https://raw.githubusercontent.com/Devko/EffectWerk/v0.0.1/docs/img/looper.png", "https://github.com/Devko/EffectWerk/releases/download/v0.0.5/EffectWerk-0.0.5-mpc-armv7.zip", "plugins/effectwerk/thumb.png",
            "A live looper and performance mixer in one insert slot: record bars on MPC's grid, then roll, slow, reverse and layer them, and crossfade two scenes from 64 one-tap effects. Plus a ten-module effect rack with macros, LFOs and a mod matrix, 62 presets and a 14-page touchscreen skin with Q-Links.", "./plugins/effectwerk/install.sh", 0,
            false,
            false,
            false,
            { "looper", "performance-mixer", "octatrack-style", "crossfader", "scenes", "effect-rack", "granular", "shimmer-reverb" }
        },
        {
            "ghost", "GHOST", "MPC-EURO", "v2.4.2", "Glitch",
            "FX", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "78", "812 KB", "14%",
            "MPC-EURO - VST - GHOST", "ghost.so",
            "", "https://github.com/carballocba/MPC-EURO-/releases/download/v2.4.2-installer2-rc3/GHOST-2.4.2-installer2-rc3-mpc-armv7.zip", "plugins/ghost/thumb.png",
            "Live buffer manipulation, rhythmic stutter and generative glitch with independent and combined Random Repeat modes.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MPC-EURO - VST - GHOST\" && if [ -f \"./plugins/ghost/ghost.so\" ]; then cp -rf ./plugins/ghost/* \"$TARGET_SYNTH_DIR/MPC-EURO - VST - GHOST/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/carballocba/MPC-EURO-/releases/download/v2.4.2-installer2-rc3/GHOST-2.4.2-installer2-rc3-mpc-armv7.zip\" \"/tmp/ghost.zip\") && unzip -o /tmp/ghost.zip -d \"$TARGET_SYNTH_DIR/MPC-EURO - VST - GHOST/\" && rm -f /tmp/ghost.zip; fi && sync", 1,
            false,
            false,
            false,
            { "buffer", "stutter", "reverse", "generative", "performance" }
        },
        {
            "op1fx-grid", "Grid", "Bombest", "v0.4.1", "Reverb",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "118", "9.5 MB", "14%",
            "bombest - VST - Grid", "grid.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/grid.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Grid-0.4.1-mpc-armv7.zip", "plugins/op1fx-grid/thumb.png",
            "A 2D waveguide mesh you feed audio and read back. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Grid\" && if [ -f \"./plugins/op1fx-grid/grid.so\" ]; then cp -rf ./plugins/op1fx-grid/* \"$TARGET_SYNTH_DIR/bombest - VST - Grid/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Grid-0.4.1-mpc-armv7.zip\" \"/tmp/op1fx-grid.zip\") && unzip -o /tmp/op1fx-grid.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Grid/\" && rm -f /tmp/op1fx-grid.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "reverb", "waveguide", "resonator" }
        },
        {
            "incipience-out", "Incipience OUT", "saustin2010", "v1.1.1", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "683", "104 KB", "14%",
            "Steve A - VST - [FX] Incipience OUT", "incipienceout.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-incipience/v1.1.1/incipienceout/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-incipience/releases/download/incipience-out-v1.1.1/FX-Incipience-OUT-1.1.1-mpc-armv7.zip", "plugins/incipience-out/thumb.png",
            "Companion to Incipience: plays one of its OUT 1-11 on its own MPC track (an audio track will do), so an instrument gets that track's fader, inserts and outputs, in time with the kit. Needs Incipience 1.0.0 or later in the same project.", "./plugins/incipience-out/install.sh", 0,
            false,
            false,
            false,
            { "routing", "multi-out", "drum" }
        },
        {
            "keyscope", "Keyscope", "jacob-sabella", "v1.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-06",
            "2193", "3.2 MB", "4%",
            "jacob-sabella - VST - Keyscope", "keyscope.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-keyscope/88cb99ac5e0d682e1e6d9c81836bf4096a9cde3c/docs/screenshots/keyscope.png", "https://github.com/jacob-sabella/mpc-vst-keyscope/releases/download/keyscope-vst-v1.1.0/Keyscope-1.1.0-mpc-armv7.zip", "plugins/keyscope/thumb.png",
            "Listens to any track and names its key, chord, bass note and tuning, from the spectrum of the whole mix; the audio passes through untouched.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Keyscope\" && if [ -f \"./plugins/keyscope/keyscope.so\" ]; then cp -rf ./plugins/keyscope/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Keyscope/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-vst-keyscope/releases/download/keyscope-vst-v1.1.0/Keyscope-1.1.0-mpc-armv7.zip\" \"/tmp/keyscope.zip\") && unzip -o /tmp/keyscope.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Keyscope/\" && rm -f /tmp/keyscope.zip; fi && sync", 1,
            false,
            false,
            false,
            { "key-detection", "chord-detection", "tuner", "analyzer" }
        },
        {
            "liminal-amp", "Liminal Amp", "gmorb", "v1.0.2", "Amp Sim",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1149", "5.5 MB", "14%",
            "Gm0rb - VST - Liminal Amp", "liminal_amp.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-liminal-amp/v1.0.0/docs/images/page.png", "https://github.com/gmorb/mpc-vst-liminal-amp/releases/download/v1.0.2/Liminal-Amp-1.0.2-mpc-armv7.zip", "plugins/liminal-amp/thumb.png",
            "Lightweight, official-sound Neural Amp Modeler: A2 captures with cab IR, gate and tone stack, resampled like the official plugin; browses models/ and irs/ folders on any drive.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Gm0rb - VST - Liminal Amp\" && if [ -f \"./plugins/liminal-amp/liminal_amp.so\" ]; then cp -rf ./plugins/liminal-amp/* \"$TARGET_SYNTH_DIR/Gm0rb - VST - Liminal Amp/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/gmorb/mpc-vst-liminal-amp/releases/download/v1.0.2/Liminal-Amp-1.0.2-mpc-armv7.zip\" \"/tmp/liminal-amp.zip\") && unzip -o /tmp/liminal-amp.zip -d \"$TARGET_SYNTH_DIR/Gm0rb - VST - Liminal Amp/\" && rm -f /tmp/liminal-amp.zip; fi && sync", 1,
            false,
            false,
            false,
            { "nam", "guitar", "amp", "cab-ir", "lightweight" }
        },
        {
            "liminal-hz", "Liminal Hz", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-24",
            "1976", "22.4 MB", "14%",
            "Gm0rb - VST - Liminal Hz", "liminal_hz.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-liminal-hz/main/docs/images/banner.png", "https://github.com/gmorb/mpc-vst-liminal-hz/releases/download/v1.3.0/Liminal-Hz-1.3.0-mpc-armv7.zip", "plugins/liminal-hz/thumb.png",
            "Real rooms, halls and echoes from impulse responses (up to 5 s, mono or stereo), the space for your NAM amp; ~3.5% of an audio block on a Force; browses reverbs/ folders and TONE3000.", "./plugins/liminal-hz/install.sh", 0,
            false,
            false,
            false,
            { "reverb", "delay", "echo", "convolution", "ir", "tone3000" }
        },
        {
            "loop-saboteurxxl", "Loop-SaboteurXXL", "L'Cronx", "v0.1.3", "Glitch",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "1193", "8.2 MB", "14%",
            "L'Cronx - VST - Loop-SaboteurXXL", "loop_saboteurxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Loop-SaboteurXXL/v0.1.0/docs/img/3-acts-seq.png", "https://github.com/sunskiefer/Loop-SaboteurXXL/releases/download/v0.1.3/Loop-SaboteurXXL-0.1.3-mpc-armv7.zip", "plugins/loop-saboteurxxl/thumb.png",
            "A port of Loop Saboteur by Stephen McLeod Blythe (clickysteve): a glitch / chop / beat-slicer that records the last bars and replays them on the MPC tempo as retriggered, pitched, reversed, stretched and crushed slices. Eight Acts, a 64-step sequencer with trig conditions, ratchets and p-locks, and 19 factory presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - Loop-SaboteurXXL\" && if [ -f \"./plugins/loop-saboteurxxl/loop_saboteurxxl.so\" ]; then cp -rf ./plugins/loop-saboteurxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - Loop-SaboteurXXL/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/Loop-SaboteurXXL/releases/download/v0.1.3/Loop-SaboteurXXL-0.1.3-mpc-armv7.zip\" \"/tmp/loop-saboteurxxl.zip\") && unzip -o /tmp/loop-saboteurxxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - Loop-SaboteurXXL/\" && rm -f /tmp/loop-saboteurxxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "glitch", "slicer", "stutter", "beat-repeat", "sequencer", "p-lock", "lo-fi", "tape" }
        },
        {
            "machinemodule-tap-gen2", "Machinemodule Tap (Gen2)", "sd88me", "v1.0.3", "Utility",
            "FX", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-04",
            "404", "95 KB", "14%",
            "sd88me - VST - Machinemodule Tap", "machinedrum_tapfx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule-tap-gen2.png", "https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.3/Machinemodule-Tap-1.0.3-mpc-aarch64.zip", "plugins/machinemodule-tap-gen2/thumb.png",
            "Puts any Machinemodule tracks, and its reverb and delay sends, on their own MPC return or FX track, so MPC's mixer and effects can process them. Needs Machinemodule in the same project. Gen2 (aarch64) only.", "./plugins/machinemodule-tap-gen2/install.sh", 0,
            false,
            false,
            false,
            { "elektron", "machinedrum", "gen2" }
        },
        {
            "magic-tape-echo", "Magic Tape Echo", "MagicStino", "v0.1.2", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "503", "2.6 MB", "14%",
            "MagicStino - VST - Magic Tape Echo", "magic_tape_echo.so",
            "https://raw.githubusercontent.com/MagicStino/Magic-Tape-Echo/a616cc2fad6bd7eb920111627bb1fc81631ff09a/magic-tape-echo-three-heads.png", "https://github.com/MagicStino/Magic-Tape-Echo/releases/download/v0.1.2/Magic-Tape-Echo-0.1.2-mpc-armv7.zip", "plugins/magic-tape-echo/thumb.png",
            "Three-head stereo tape echo with tape saturation, wow/flutter, tempo sync, optional spring reverb and a slow VU meter.", "./plugins/magic-tape-echo/install.sh", 0,
            false,
            false,
            false,
            { "delay", "tape", "echo", "spring" }
        },
        {
            "warps", "Meta Mod", "saustin2010", "v1.2.1", "Modulation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1628", "1.0 MB", "14%",
            "Émilie Gillet - VST - [FX] Meta Mod", "warps.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-meta-mod/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-meta-mod/releases/download/v1.2.1/FX-Meta-Mod-1.2.1-mpc-armv7.zip", "plugins/warps/thumb.png",
            "Meta Mod: ring mod, folding, XOR, vocoder and frequency shifting as an insert effect; 12 presets, from Émilie Gillet's open-source Warps.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Meta Mod\" && if [ -f \"./plugins/warps/warps.so\" ]; then cp -rf ./plugins/warps/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Meta Mod/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-meta-mod/releases/download/v1.2.1/FX-Meta-Mod-1.2.1-mpc-armv7.zip\" \"/tmp/warps.zip\") && unzip -o /tmp/warps.zip -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Meta Mod/\" && rm -f /tmp/warps.zip; fi && sync", 1,
            false,
            false,
            false,
            { "ring-mod", "vocoder", "frequency-shifter", "eurorack" }
        },
        {
            "meta4mpcxxl-fx", "Meta4MPCXXL FX", "L'Cronx", "v0.1.2", "Modular",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "392", "11.2 MB", "14%",
            "L'Cronx - VST - Meta4MPCXXL FX", "meta4mpcxxl_fx.so",
            "https://raw.githubusercontent.com/sunskiefer/Meta4MPCXXL/v0.1.1/docs/img/meta4mpcxxl.png", "https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-FX-0.1.2-mpc-armv7.zip", "plugins/meta4mpcxxl-fx/thumb.png",
            "Meta4MPCXXL as an effect: the track's audio goes into a 4ms MetaModule-format (VCV Rack) patch's In 1 / In 2, with every parameter on Q-Links, MIDI and clock into the patch, and Wi-Fi patch upload from VCV Rack.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL FX\" && if [ -f \"./plugins/meta4mpcxxl-fx/meta4mpcxxl_fx.so\" ]; then cp -rf ./plugins/meta4mpcxxl-fx/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL FX/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-FX-0.1.2-mpc-armv7.zip\" \"/tmp/meta4mpcxxl-fx.zip\") && unzip -o /tmp/meta4mpcxxl-fx.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL FX/\" && rm -f /tmp/meta4mpcxxl-fx.zip; fi && sync", 1,
            false,
            false,
            false,
            { "modular", "vcv", "metamodule", "patch-player", "eurorack", "multi-fx" }
        },
        {
            "monomodule-fx-gen2", "Monomodule FX (Gen2)", "sd88me", "v1.0.4", "Multi Effect",
            "FX", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-04",
            "341", "4.2 MB", "14%",
            "sd88me - VST - Monomodule FX", "monomodule_fx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-monomodule/main/docs/img/monomodule-fx-gen2.png", "https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-FX-1.0.4-mpc-aarch64.zip", "plugins/monomodule-fx-gen2/thumb.png",
            "The Elektron Monomachine's seven effect machines (thru, reverb, chorus, dynamix, ring mod, phaser, flanger) as an audio effect for Gen2 MPC devices, with a touchscreen skin and Q-Links. A ready-made download with nothing of Elektron's in it: it reads your own Monomachine OS file from the plugin folder. Gen2 only.", "./plugins/monomodule-fx-gen2/install.sh", 0,
            false,
            false,
            false,
            { "elektron", "monomachine", "gen2" }
        },
        {
            "mpc-dj-fx-2", "MPC DJ FX 2", "mitchoster", "v3.1.6", "Multi Fx",
            "FX", "Stable", "MPC Live II (MPC OS 3.x (ACVS-MPC3 build))", "MPC OS 3.x", "2026-11-03",
            "1063", "5.1 MB", "30%",
            "LarryDaimond - VST - MPC DJ FX 2", "mpc_djfx2.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-djfx2/0f20d92f/docs/ui-preview/page_1.png", "https://github.com/mitchoster/mpc-vst-djfx2/releases/download/v3.1.6/MPC-DJ-FX-2-3.1.6-mpc-armv7.zip", "plugins/mpc-dj-fx-2/thumb.png",
            "One-slot live DJ insert for MPC/Force: 3-band isolator, DJ filter, beat repeat, delay, reverb, through-zero flanger, grain delay, limiter, and eight performance macros measured from Lenny Kiser's DJ-FX rack.", "./plugins/mpc-dj-fx-2/install.sh", 0,
            false,
            false,
            false,
            { "dj", "isolator", "filter", "beat-repeat", "delay", "reverb", "performance", "macro" }
        },
        {
            "mpc-multihead-echo", "MPC Multihead Echo", "mitchoster", "v1.0.2", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-02",
            "180", "1.7 MB", "14%",
            "LarryDaimond - VST - MPC Multihead Echo", "mpc_multihead_echo.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-multihead-echo/c31c3ef479ce87083ab87fbef14c2fe92a6bb83b/docs/companion-panel-echo.png", "https://github.com/mitchoster/mpc-vst-multihead-echo/releases/download/v1.0.2/MPC-Multihead-Echo-1.0.2-mpc-armv7.zip", "plugins/mpc-multihead-echo/thumb.png",
            "Three-head tape echo with three preamp characters, chorus, tape wear, tempo sync with drift, a spring reverb and a companion screen in the browser.", "./plugins/mpc-multihead-echo/install.sh", 0,
            false,
            false,
            false,
            { "tape-echo", "delay", "saturation", "spring-reverb", "tempo-sync", "companion-app" }
        },
        {
            "mpc-tape-echo", "MPC Tape Echo", "mitchoster", "v1.2.3", "Delay",
            "FX", "Stable", "MPC Live II (MPC OS 3.x (ACVS-MPC3 build))", "MPC OS 3.x", "2026-11-02",
            "637", "11.6 MB", "14%",
            "LarryDaimond - VST - MPC Tape Echo", "mpc_tape_echo.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-tape-echo/main/docs/img/panel.png", "https://github.com/mitchoster/mpc-vst-tape-echo/releases/download/v1.2.3/MPC-Tape-Echo-1.2.3-mpc-armv7.zip", "plugins/mpc-tape-echo/thumb.png",
            "Three-head tape echo with a spring tank: 12 head and reverb modes, tape saturation, wow and flutter, tempo sync, live VU.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/LarryDaimond - VST - MPC Tape Echo\" && if [ -f \"./plugins/mpc-tape-echo/mpc_tape_echo.so\" ]; then cp -rf ./plugins/mpc-tape-echo/* \"$TARGET_SYNTH_DIR/LarryDaimond - VST - MPC Tape Echo/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mitchoster/mpc-vst-tape-echo/releases/download/v1.2.3/MPC-Tape-Echo-1.2.3-mpc-armv7.zip\" \"/tmp/mpc-tape-echo.zip\") && unzip -o /tmp/mpc-tape-echo.zip -d \"$TARGET_SYNTH_DIR/LarryDaimond - VST - MPC Tape Echo/\" && rm -f /tmp/mpc-tape-echo.zip; fi && sync", 1,
            false,
            false,
            false,
            { "delay", "tape", "echo", "spring" }
        },
        {
            "nam", "NAM", "jacob-sabella", "v1.0.10", "Amp Sim",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "3802", "2.8 MB", "14%",
            "jacob-sabella - VST - NAM", "nam_vst.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-nam/f2fa28c8f4086606e929732a4824e72579bdcf80/docs/screenshots/nam.png", "https://github.com/jacob-sabella/mpc-vst-nam/releases/download/v1.0.10/NAM-1.0.10-mpc-armv7.zip", "plugins/nam/thumb.png",
            "Neural Amp Modeler captures as an insert effect, with cab IR, tone, gate, pitch, delay, reverb and an on-device TONE3000 downloader.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - NAM\" && if [ -f \"./plugins/nam/nam_vst.so\" ]; then cp -rf ./plugins/nam/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - NAM/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-vst-nam/releases/download/v1.0.10/NAM-1.0.10-mpc-armv7.zip\" \"/tmp/nam.zip\") && unzip -o /tmp/nam.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - NAM/\" && rm -f /tmp/nam.zip; fi && sync", 1,
            false,
            false,
            false,
            { "nam", "guitar", "amp", "cab-ir", "tone3000" }
        },
        {
            "op1fx-nitro", "Nitro", "Bombest", "v0.4.1", "Filter",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "118", "4.9 MB", "14%",
            "bombest - VST - Nitro", "nitro.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/nitro.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Nitro-0.4.1-mpc-armv7.zip", "plugins/op1fx-nitro/thumb.png",
            "Filter with a drive stage and a MIX knob, 12 presets. OP-1 inspired.", "./plugins/op1fx-nitro/install.sh", 0,
            false,
            false,
            false,
            { "op1", "filter", "drive", "dj" }
        },
        {
            "ottmpc", "OTTmpc", "sd88me", "v1.0.1", "Compressor",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "1222", "1.7 MB", "14%",
            "sd88me - VST - OTTmpc", "ottmpc.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-OTT/claude/friendly-wozniak-m9ak1l/docs/screenshot.png", "https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v1.0.1/OTTmpc-1.0.1-mpc-armv7.zip", "plugins/ottmpc/thumb.png",
            "Vital's OTT multiband up/down compressor as an insert effect: live band meters with dB numbers, 12 Q-Link controls and 10 presets.", "./plugins/ottmpc/install.sh", 0,
            false,
            false,
            false,
            { "ott", "multiband", "compressor", "dynamics" }
        },
        {
            "overcast", "Overcast", "Padbangers", "v1.0.6", "Granular",
            "FX", "Stable", "Akai MPC X (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-04",
            "3986", "3.8 MB", "9%",
            "Padbangers - VST - Overcast", "overcast.so",
            "https://raw.githubusercontent.com/FullPace/overcast/overcast-v1.0.6/docs/overcast-main.png", "https://github.com/padbangers/overcast/releases/download/overcast-v1.0.6/Overcast-1.0.6-mpc-armv7.zip", "plugins/overcast/thumb.png",
            "Mutable Instruments Clouds (Parasites firmware, six modes) with a modulation page: two envelopes, two LFOs and an 8-slot matrix onto Clouds' CV inputs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Padbangers - VST - Overcast\" && if [ -f \"./plugins/overcast/overcast.so\" ]; then cp -rf ./plugins/overcast/* \"$TARGET_SYNTH_DIR/Padbangers - VST - Overcast/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/padbangers/overcast/releases/download/overcast-v1.0.6/Overcast-1.0.6-mpc-armv7.zip\" \"/tmp/overcast.zip\") && unzip -o /tmp/overcast.zip -d \"$TARGET_SYNTH_DIR/Padbangers - VST - Overcast/\" && rm -f /tmp/overcast.zip; fi && sync", 1,
            false,
            false,
            false,
            { "mutable-instruments", "clouds", "parasites", "granular", "texture", "reverb", "delay", "eurorack" }
        },
        {
            "percolator-out", "Percolator OUT", "saustin2010", "v2.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "952", "147 KB", "14%",
            "Steve A - VST - [FX] Percolator OUT", "percolatorout.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-percolator/v2.1.0/percolatorout/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-percolator/releases/download/percolator-out-v2.1.0/FX-Percolator-OUT-2.1.0-mpc-armv7.zip", "plugins/percolator-out/thumb.png",
            "Companion to Percolator: plays one of its OUT 1-4 on its own MPC track (an audio track will do), so a voice gets that track's fader, inserts and outputs, in time with the kit. Needs Percolator 2.0.0 or later in the same project.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [FX] Percolator OUT\" && if [ -f \"./plugins/percolator-out/percolatorout.so\" ]; then cp -rf ./plugins/percolator-out/* \"$TARGET_SYNTH_DIR/Steve A - VST - [FX] Percolator OUT/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-percolator/releases/download/percolator-out-v2.1.0/FX-Percolator-OUT-2.1.0-mpc-armv7.zip\" \"/tmp/percolator-out.zip\") && unzip -o /tmp/percolator-out.zip -d \"$TARGET_SYNTH_DIR/Steve A - VST - [FX] Percolator OUT/\" && rm -f /tmp/percolator-out.zip; fi && sync", 1,
            false,
            false,
            false,
            { "routing", "multi-out", "drum" }
        },
        {
            "op1fx-phone", "Phone", "Bombest", "v0.4.1", "Filter",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "119", "5.1 MB", "14%",
            "bombest - VST - Phone", "phone.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/phone.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phone-0.4.1-mpc-armv7.zip", "plugins/op1fx-phone/thumb.png",
            "Telephone-band squash with a 12-preset bank. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Phone\" && if [ -f \"./plugins/op1fx-phone/phone.so\" ]; then cp -rf ./plugins/op1fx-phone/* \"$TARGET_SYNTH_DIR/bombest - VST - Phone/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phone-0.4.1-mpc-armv7.zip\" \"/tmp/op1fx-phone.zip\") && unzip -o /tmp/op1fx-phone.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Phone/\" && rm -f /tmp/op1fx-phone.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "telephone", "band-limit", "lo-fi" }
        },
        {
            "phyxxl", "PhyXXL", "L'Cronx", "v0.1.6", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-25",
            "805", "18.3 MB", "14%",
            "L'Cronx - VST - PhyXXL", "phyxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PhyXXL/v0.1.2/docs/img/overview.png", "https://github.com/sunskiefer/PhyXXL/releases/download/v0.1.6/PhyXXL-0.1.6-mpc-armv7.zip", "plugins/phyxxl/thumb.png",
            "Eight filters in the style of classic synths (MS-20, SEM, Minimoog, Matrix-12, 2600, Wasp, prologue, Polivoks) in one effect. Route audio from any filter into any other; modulate each from ten sources by touch patch bay or matrix. 28 patches. By L'Cronx, GPL-3. No third-party code; not endorsed by the makers of the originals.", "./plugins/phyxxl/install.sh", 0,
            false,
            false,
            false,
            { "filter", "multi-filter", "ms-20", "sem", "moog", "matrix-12", "arp-2600", "polivoks" }
        },
        {
            "plateauxxl", "PlateauXXL", "L'Cronx", "v0.4.3", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "1580", "12.9 MB", "14%",
            "L'Cronx - VST - PlateauXXL", "plateauxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PlateauXXL/v0.4.1/docs/img/plateau.png", "https://github.com/sunskiefer/PlateauXXL/releases/download/v0.4.3/PlateauXXL-0.4.3-mpc-armv7.zip", "plugins/plateauxxl/thumb.png",
            "Valley Audio's Plateau reverb from VCV Rack, patched: every CV input on a built-in source (four Bogaudio LFOs, Tidal Modulator 2, Random Sampler, step and gate sequencers) synced to the MPC tempo, a brickwall limiter, Panic and 16 presets, each page drawn like its module.", "./plugins/plateauxxl/install.sh", 0,
            false,
            false,
            false,
            { "reverb", "plate", "dattorro", "valley", "vcv", "freeze", "lfo", "modulation" }
        },
        {
            "potentialpatina", "PotentialPatina", "PotentialBreaks", "v1.0.4", "Saturation",
            "FX", "Stable", "MPC One (HAKAI 3.10)", "MPC OS 3.x", "2026-11-05",
            "105", "5.6 MB", "14%",
            "PotentialBreaks - VST - PotentialPatina", "potentialpatina.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialpatina/7e1f7be4526c33f6b88da8238943086a89ec977f/docs/images/potentialpatina.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialpatina/releases/download/v1.0.4/PotentialPatina-1.0.4-mpc-armv7.zip", "plugins/potentialpatina/thumb.png",
            "Retro-colour insert effect: vinyl and tape noise, wow and flutter, tape/tube/cassette drive, bit and sample-rate crush, a tone tilt and a small room, with presets from subtle dust to old radio.", "./plugins/potentialpatina/install.sh", 0,
            false,
            false,
            false,
            { "lo-fi", "vinyl", "tape", "bitcrusher", "wobble" }
        },
        {
            "pulytek", "PulyTek", "MagicStino", "v1.0.2", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-10-23",
            "2278", "14.9 MB", "14%",
            "MagicStino - VST - PulyTek", "pulytek.so",
            "https://raw.githubusercontent.com/MagicStino/pulytek/v1.0.1/docs/preview_0.png", "https://github.com/MagicStino/pulytek/releases/download/v1.0.2/PulyTek-1.0.2-mpc-armv7.zip", "plugins/pulytek/thumb.png",
            "Pultec-style WDF EQ with tube warmth, AIR, stereo VU meters and four-band side EQ.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MagicStino - VST - PulyTek\" && if [ -f \"./plugins/pulytek/pulytek.so\" ]; then cp -rf ./plugins/pulytek/* \"$TARGET_SYNTH_DIR/MagicStino - VST - PulyTek/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/MagicStino/pulytek/releases/download/v1.0.2/PulyTek-1.0.2-mpc-armv7.zip\" \"/tmp/pulytek.zip\") && unzip -o /tmp/pulytek.zip -d \"$TARGET_SYNTH_DIR/MagicStino - VST - PulyTek/\" && rm -f /tmp/pulytek.zip; fi && sync", 1,
            false,
            false,
            false,
            { "eq", "pultec", "wdf", "tube", "stereo" }
        },
        {
            "op1fx-punch", "Punch", "Bombest", "v0.4.1", "Distortion",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "126", "6.0 MB", "14%",
            "bombest - VST - Punch", "punch.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/punch.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Punch-0.4.1-mpc-armv7.zip", "plugins/op1fx-punch/thumb.png",
            "Bit-crush style punch for drums and buses, 12 presets, with a Detroit boxer skin.", "./plugins/op1fx-punch/install.sh", 0,
            false,
            false,
            false,
            { "op1", "compressor", "drive", "drums" }
        },
        {
            "rings-fx", "Resonator FX", "saustin2010", "v1.2.1", "Resonator",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1894", "1.2 MB", "14%",
            "Émilie Gillet - VST - [FX] Resonator FX", "ringsfx.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-resonator-fx/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-resonator-fx/releases/download/v1.2.1/FX-Resonator-FX-1.2.1-mpc-armv7.zip", "plugins/rings-fx/thumb.png",
            "Resonator FX: Resonator as an insert effect, the track's audio strums it, turning drums and loops into tuned strings; 12 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Resonator FX\" && if [ -f \"./plugins/rings-fx/ringsfx.so\" ]; then cp -rf ./plugins/rings-fx/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Resonator FX/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-resonator-fx/releases/download/v1.2.1/FX-Resonator-FX-1.2.1-mpc-armv7.zip\" \"/tmp/rings-fx.zip\") && unzip -o /tmp/rings-fx.zip -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Resonator FX/\" && rm -f /tmp/rings-fx.zip; fi && sync", 1,
            false,
            false,
            false,
            { "rings", "resonator", "eurorack" }
        },
        {
            "rmxxxl", "RMXXXL", "L'Cronx", "v1.3.0", "Multi Fx",
            "FX", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "2446", "3.3 MB", "14%",
            "L'Cronx - VST - RMXXXL", "rmxxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/RMXXXL/0ef9fe195525cada860205f1370a4b9f6896bc60/docs/img/remix.png", "https://github.com/sunskiefer/RMXXXL/releases/download/v1.3.0/RMXXXL-1.3.0-mpc-armv7.zip", "plugins/rmxxxl/thumb.png",
            "RMX-1000-style remix effect: isolator, DJ filter, Build Up / Break Down scenes, synced echo, Release FX and a Release kill switch, EQ Kill buttons, a clean Panic, Clouds, Dragonfly reverbs, sample pads with ADSR, 16 user presets and a brickwall limiter, with a big-button touchscreen skin and Q-Links.", "./plugins/rmxxxl/install.sh", 0,
            false,
            false,
            false,
            { "remix", "dj", "rmx", "isolator", "filter", "clouds", "reverb", "pads" }
        },
        {
            "shapey-5-band-eq", "Shapey 5-Band EQ", "3dthc", "v1.0.1", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "333", "3.4 MB", "14%",
            "3DTHC - VST - Shapey 5-Band EQ", "shapey_5band_eq.so",
            "https://raw.githubusercontent.com/3dthc/Shapey-MPC/ddbf9bbc9f66f2af0802070b38bb3d113140131c/docs/shapey-5-band-eq.png", "https://github.com/3dthc/Shapey-MPC/releases/download/shapey-5-band-eq-vst-v1.0.1/Shapey-5-Band-EQ-1.0.1-mpc-armv7.zip", "plugins/shapey-5-band-eq/thumb.png",
            "The 5-band version of Shapey EQ, made for touch: every band has its own lane under a taller curve, so any dot drags directly. Any band can be a bell, shelf, cut, notch or band pass. Live analyzer, zero latency.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey 5-Band EQ\" && if [ -f \"./plugins/shapey-5-band-eq/shapey_5band_eq.so\" ]; then cp -rf ./plugins/shapey-5-band-eq/* \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey 5-Band EQ/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/Shapey-MPC/releases/download/shapey-5-band-eq-vst-v1.0.1/Shapey-5-Band-EQ-1.0.1-mpc-armv7.zip\" \"/tmp/shapey-5-band-eq.zip\") && unzip -o /tmp/shapey-5-band-eq.zip -d \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey 5-Band EQ/\" && rm -f /tmp/shapey-5-band-eq.zip; fi && sync", 1,
            false,
            false,
            false,
            { "eq", "parametric-eq", "analyzer", "mixing" }
        },
        {
            "shapey-eq", "Shapey EQ", "3dthc", "v1.6.2", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "380", "4.3 MB", "14%",
            "3DTHC - VST - Shapey EQ", "shapey_eq.so",
            "https://raw.githubusercontent.com/3dthc/Shapey-MPC/ddbf9bbc9f66f2af0802070b38bb3d113140131c/docs/shapey-eq.png", "https://github.com/3dthc/Shapey-MPC/releases/download/shapey-eq-vst-v1.6.2/Shapey-EQ-1.6.2-mpc-armv7.zip", "plugins/shapey-eq/thumb.png",
            "An 8-band parametric EQ you shape by touch: tap or drag on the curve, drag numbered dots, and make any band a bell, shelf, cut, notch or band pass. Live analyzer, zero latency, presets and four themes.", "./plugins/shapey-eq/install.sh", 0,
            false,
            false,
            false,
            { "eq", "parametric-eq", "analyzer", "mixing" }
        },
        {
            "op1fx-spring", "Spring", "Bombest", "v0.4.1", "Reverb",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "120", "3.4 MB", "14%",
            "bombest - VST - Spring", "spring.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/spring.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Spring-0.4.1-mpc-armv7.zip", "plugins/op1fx-spring/thumb.png",
            "Spring reverb with a 12-preset bank. OP-1 inspired.", "./plugins/op1fx-spring/install.sh", 0,
            false,
            false,
            false,
            { "op1", "reverb", "spring" }
        },
        {
            "stompbox", "Stompbox", "jacob-sabella", "v1.1.0", "Amp Sim",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-06",
            "222", "6.8 MB", "21%",
            "jacob-sabella - VST - Stompbox", "stompbox.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-stompbox/bc70be3dd01103bc792db0c2b0e54a589fc37d36/docs/screenshots/stompbox.png", "https://github.com/jacob-sabella/mpc-vst-stompbox/releases/download/stompbox-vst-v1.1.0/Stompbox-1.1.0-mpc-armv7.zip", "plugins/stompbox/thumb.png",
            "A whole guitar rig from guitarix's DSP: gate, comp, wah, any number of pedals, amp, tone stack, power amp, cabinet with an IR loader, post FX, mod, delay, reverb and EQ. 219 models and guitarix's 40 factory presets in the PRESET menu. Not affiliated with guitarix.", "./plugins/stompbox/install.sh", 0,
            false,
            false,
            false,
            { "guitar", "amp", "pedals", "cabinet", "cab-ir", "guitarix", "presets" }
        },
        {
            "stream", "Stream", "johnny", "v1.0.0", "Utility",
            "FX", "Stable", "MPC One (3.9.1.2)", "MPC OS 3.x", "2026-10-03",
            "531", "2.7 MB", "1%",
            "johnny - VST - Stream", "stream.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/mpc-stream/764605ba1a08594e2dc5268e4f12bd6e25edb184/screenshots/stream.png", "https://github.com/stampman3000-jpg/mpc-stream/releases/download/v1.0.0/Stream-1.0.0-mpc-armv7.zip", "plugins/stream/thumb.png",
            "Multichannel USB audio effect send from an MPC to a Mac. Needs BlackHole 64ch and a Mac helper app.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/johnny - VST - Stream\" && if [ -f \"./plugins/stream/stream.so\" ]; then cp -rf ./plugins/stream/* \"$TARGET_SYNTH_DIR/johnny - VST - Stream/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/stampman3000-jpg/mpc-stream/releases/download/v1.0.0/Stream-1.0.0-mpc-armv7.zip\" \"/tmp/stream.zip\") && unzip -o /tmp/stream.zip -d \"$TARGET_SYNTH_DIR/johnny - VST - Stream/\" && rm -f /tmp/stream.zip; fi && sync", 1,
            false,
            false,
            false,
            { "utility" }
        },
        {
            "tr-mpc-tap-fx", "TR-MPC Tap FX", "sd88me", "v1.1.0", "Utility",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 3.x", "2026-11-02",
            "1070", "515 KB", "14%",
            "sd88me - VST - TR-MPC Tap FX", "trmpc_tapfx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/7e704c6f3be14b661c887631e203494d8cc3f74f/docs/img/tr-mpc-tap-fx.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-tap-fx-vst-v1.1.0/TR-MPC-Tap-FX-1.1.0-mpc-armv7.zip", "plugins/tr-mpc-tap-fx/thumb.png",
            "Companion to TR-MPC: sends any slot, or the kit's reverb or delay send, out on its own MPC track, sample-aligned with the kit, so MPC's mixer, submixes and insert effects can process it. Needs TR-MPC in the same project.", "./plugins/tr-mpc-tap-fx/install.sh", 0,
            false,
            false,
            false,
            { "routing", "multi-out", "drum" }
        },
        {
            "tuner", "Tuner", "jacob-sabella", "v1.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-06",
            "180", "669 KB", "1%",
            "jacob-sabella - VST - Tuner", "tuner.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-tuner/a3e4c099eacce3cdd440d54eb3ff059c96baafa5/docs/screenshots/needle.png", "https://github.com/jacob-sabella/mpc-vst-tuner/releases/download/tuner-vst-v1.1.0/Tuner-1.1.0-mpc-armv7.zip", "plugins/tuner/thumb.png",
            "A chromatic tuner insert with a live needle drawn straight onto the MPC's screen at 20-24 fps: note, cents and Hz, A4 from 430 to 450 Hz, and a mute for silent tuning. It writes into MPC's display memory, which MPC OS doesn't offer plugins, so a firmware update could break it. Not affiliated with Akai.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Tuner\" && if [ -f \"./plugins/tuner/tuner.so\" ]; then cp -rf ./plugins/tuner/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Tuner/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-vst-tuner/releases/download/tuner-vst-v1.1.0/Tuner-1.1.0-mpc-armv7.zip\" \"/tmp/tuner.zip\") && unzip -o /tmp/tuner.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Tuner/\" && rm -f /tmp/tuner.zip; fi && sync", 1,
            false,
            false,
            false,
            { "tuner", "guitar", "bass", "pitch", "screen-draw" }
        },
        {
            "tuney", "Tuney", "3dthc", "v1.4.3", "Pitch",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "300", "9.3 MB", "14%",
            "3DTHC - VST - Tuney", "tuney.so",
            "https://raw.githubusercontent.com/3dthc/Tuney-MPC/55862ef83ab6938b5f1e3e20f5fa1ead7449e9b2/docs/tuney.png", "https://github.com/3dthc/Tuney-MPC/releases/download/tuney-vst-v1.4.3/Tuney-1.4.3-mpc-armv7.zip", "plugins/tuney/thumb.png",
            "Real-time vocal pitch correction: key and scale or your own notes on a touch keyboard, a pitch dial, Retune, Transition, Amount, Vibrato and Humanize, voice ranges and a reference pitch. Presets from hard tune to subtle.", "./plugins/tuney/install.sh", 0,
            false,
            false,
            false,
            { "pitch-correction", "vocal", "tuner", "effect" }
        },
        {
            "verglas", "Verglas", "saustin2010", "v1.2.1", "Granular",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1486", "642 KB", "14%",
            "Émilie Gillet - VST - [FX] Verglas", "verglas.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-verglas/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-verglas/releases/download/v1.2.1/FX-Verglas-1.2.1-mpc-armv7.zip", "plugins/verglas/thumb.png",
            "Verglas: granular, stretch, looper and spectral modes and freeze as an insert effect; 12 presets, from Émilie Gillet's open-source Clouds.", "./plugins/verglas/install.sh", 0,
            false,
            false,
            false,
            { "clouds", "granular", "texture", "eurorack" }
        },
        {
            "vibe-fx", "Vibe FX", "nachtaktiv303", "v1.0.0", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1146", "906 KB", "14%",
            "nachtaktiv303 - VST - Vibe FX", "mpc_fx.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-vibe-fx/main/preview_1.png", "https://github.com/nachtaktiv303/mpc-vst-vibe-fx/releases/download/vibe-fx-vst-v1.0.0/Vibe-FX-1.0.0-mpc-armv7.zip", "plugins/vibe-fx/thumb.png",
            "Drive, chorus, FDN reverb and tape delay in one native MPC OS insert effect, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Vibe FX\" && if [ -f \"./plugins/vibe-fx/mpc_fx.so\" ]; then cp -rf ./plugins/vibe-fx/* \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Vibe FX/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/nachtaktiv303/mpc-vst-vibe-fx/releases/download/vibe-fx-vst-v1.0.0/Vibe-FX-1.0.0-mpc-armv7.zip\" \"/tmp/vibe-fx.zip\") && unzip -o /tmp/vibe-fx.zip -d \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Vibe FX/\" && rm -f /tmp/vibe-fx.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drive", "chorus", "reverb", "delay", "multi-fx", "insert" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "ADDINS";
        sec.items = {
        {
            "force-link-audio", "Force Link Audio", "Mac Digi / Rare Data", "v1.1.0", "Network Audio",
            "ADDIN", "Stable", "Akai Force (3.9.1.2)", "MPC OS 2.x & 3.x", "2026-10-14",
            "356", "257 KB", "14%",
            "Mac Digi / Rare Data - VST - Force Link Audio", "forceLinkAudioAddin.so",
            "https://github.com/macdigi/force-link-audio/releases/download/v1.1.0/force-link-audio-card.png", "https://github.com/macdigi/force-link-audio/releases/download/v1.1.0/Force-Link-Audio-1.1.0-mpc-armv7.zip", "plugins/force-link-audio/thumb.png",
            "Publish the Force stereo main mix to Link Audio receivers on your trusted LAN while staying standalone. Disable the legacy Force Link Audio addon first. Off until enabled in settings; no remote-control server. Other MPC hardware is unverified.", "./plugins/force-link-audio/install.sh", 0,
            false,
            false,
            false,
            { "link-audio", "network-audio", "recording", "compa" }
        },
        {
            "commander", "MPC Commander", "jacob-sabella", "v1.2.0", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-06",
            "3052", "252 KB", "14%",
            "jacob-sabella - VST - MPC Commander", "mpc_commander_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-commander/763c959e1855fd3bcec7dfd6832b4be77a502e4f/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-commander/releases/download/commander-addin-v1.2.0/MPC-Commander-addin-1.2.0-mpc-armv7.zip", "plugins/commander/thumb.png",
            "The plugins MPC loads, with their own skins, and MPC's transport, on a desktop app. Listens on the device only: reach it through an SSH tunnel (no login; bind=0.0.0.0 opens it to your network).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Commander\" && if [ -f \"./plugins/commander/mpc_commander_addin.so\" ]; then cp -rf ./plugins/commander/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Commander/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-commander/releases/download/commander-addin-v1.2.0/MPC-Commander-addin-1.2.0-mpc-armv7.zip\" \"/tmp/commander.zip\") && unzip -o /tmp/commander.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Commander/\" && rm -f /tmp/commander.zip; fi && sync", 1,
            false,
            false,
            false,
            { "desktop", "plugins", "skins", "transport", "midi" }
        },
        {
            "filebrowser", "MPC File Browser", "jacob-sabella", "v1.0.1", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "573", "19.2 MB", "14%",
            "jacob-sabella - VST - MPC File Browser", "mpc_filebrowser_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-filebrowser/88b86eb39f907485995497cf0eaeeb5ddd7aa3e4/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-filebrowser/releases/download/filebrowser-addin-v1.0.1/MPC-File-Browser-1.0.1-mpc-armv7.zip", "plugins/filebrowser/thumb.png",
            "FileBrowser Quantum beside MPC: browse the device's drives in a web browser, upload and download samples and projects, play audio, edit text files. Listens on the device only: reach it through an SSH tunnel (login admin / mpcadmin; change it). bind=0.0.0.0 opens it to your network.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC File Browser\" && if [ -f \"./plugins/filebrowser/mpc_filebrowser_addin.so\" ]; then cp -rf ./plugins/filebrowser/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC File Browser/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-filebrowser/releases/download/filebrowser-addin-v1.0.1/MPC-File-Browser-1.0.1-mpc-armv7.zip\" \"/tmp/filebrowser.zip\") && unzip -o /tmp/filebrowser.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC File Browser/\" && rm -f /tmp/filebrowser.zip; fi && sync", 1,
            false,
            false,
            false,
            { "files", "browser", "upload", "samples", "editor" }
        },
        {
            "remote", "MPC Remote", "jacob-sabella", "v1.2.0", "Utility",
            "ADDIN", "Stable", "MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-06",
            "3511", "69 KB", "14%",
            "jacob-sabella - VST - MPC Remote", "mpc_remote_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-remote/c9cc05422873025693cf5527b485ef44da51c5a0/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-remote/releases/download/remote-addin-v1.2.0/MPC-Remote-addin-1.2.0-mpc-armv7.zip", "plugins/remote/thumb.png",
            "The MPC screen in a browser, and an MCP server so an AI model can see and touch it, play MIDI and read files. Listens on the device only: reach it through an SSH tunnel (no login; bind=0.0.0.0 opens it to your network).", "./plugins/remote/install.sh", 0,
            false,
            false,
            false,
            { "remote", "screen", "touch", "mcp", "midi" }
        },
        {
            "sample-draw-handler", "MPC Sample Draw Handler", "jacob-sabella", "v1.0.1", "Utility",
            "ADDIN", "Stable", "MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-06",
            "39", "64 KB", "14%",
            "jacob-sabella - VST - MPC Sample Draw Handler", "mpc_sample_draw_handler.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-sample-draw-handler/3db0a761ac2e8ac74d52fef433e576b6c966fee1/docs/screenshots/stompbox-page.gif", "https://github.com/jacob-sabella/mpc-addin-sample-draw-handler/releases/download/sample-draw-handler-addin-v1.0.1/MPC-Sample-draw-handler-addin-1.0.1-mpc-aarch64.zip", "plugins/sample-draw-handler/thumb.png",
            "Lets plugins and addins draw their own page on the MPC Sample's screen. Needed by MPC Sample Rack.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Draw Handler\" && if [ -f \"./plugins/sample-draw-handler/mpc_sample_draw_handler.so\" ]; then cp -rf ./plugins/sample-draw-handler/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Draw Handler/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-sample-draw-handler/releases/download/sample-draw-handler-addin-v1.0.1/MPC-Sample-draw-handler-addin-1.0.1-mpc-aarch64.zip\" \"/tmp/sample-draw-handler.zip\") && unzip -o /tmp/sample-draw-handler.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Draw Handler/\" && rm -f /tmp/sample-draw-handler.zip; fi && sync", 1,
            false,
            false,
            false,
            { "screen", "plugin-pages" }
        },
        {
            "sample-rack", "MPC Sample Rack", "jacob-sabella", "v1.0.0", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-06",
            "28", "68 KB", "14%",
            "jacob-sabella - VST - MPC Sample Rack", "mpc_sample_rack.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-sample-rack/c2a988ddc52bf92dc3bf83f5f4283cadc78eeb47/docs/screenshots/rack-page.png", "https://github.com/jacob-sabella/mpc-addin-sample-rack/releases/download/sample-rack-addin-v1.0.0/MPC-Sample-rack-addin-1.0.0-mpc-aarch64.zip", "plugins/sample-rack/thumb.png",
            "Add, remove and turn on or off the plugins in a project's inserts from a RACK page on the Sample. APPLY saves and restarts MPC: save first.", "./plugins/sample-rack/install.sh", 0,
            false,
            false,
            false,
            { "plugins", "inserts" }
        },
        {
            "sample-scalemode", "MPC Sample Scale Mode", "jacob-sabella", "v1.1.0", "Utility",
            "ADDIN", "Stable", "MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-06",
            "16", "320 KB", "14%",
            "jacob-sabella - VST - MPC Sample Scale Mode", "mpc_sample_scalemode.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-sample-scalemode/7f5c90324dbc6f3458eb17466a265d5992d2fade/docs/screenshots/scale-page.png", "https://github.com/jacob-sabella/mpc-addin-sample-scalemode/releases/download/v1.1.0/MPC-Sample-Scale-Mode-1.1.0-mpc-aarch64.zip", "plugins/sample-scalemode/thumb.png",
            "Hold 16 Levels on the MPC Sample to play its 16 slots in a scale (8 scales, any root), starting from the sample's own note (MPC's, or heard in the audio when untagged). A SCALE page shows the notes and lights the one you play. Recorded hits keep their notes. Needs the MPC Sample Draw Handler for the page.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Scale Mode\" && if [ -f \"./plugins/sample-scalemode/mpc_sample_scalemode.so\" ]; then cp -rf ./plugins/sample-scalemode/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Scale Mode/\"; else (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-sample-scalemode/releases/download/v1.1.0/MPC-Sample-Scale-Mode-1.1.0-mpc-aarch64.zip\" \"/tmp/sample-scalemode.zip\") && unzip -o /tmp/sample-scalemode.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Scale Mode/\" && rm -f /tmp/sample-scalemode.zip; fi && sync", 1,
            false,
            false,
            false,
            { "scale", "16-levels", "notes", "pads" }
        },
        {
            "skipback", "MPC Skipback", "sd88me", "v0.6.2", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "1196", "64 KB", "14%",
            "sd88me - VST - MPC Skipback", "libmpc_skipback.so",
            "https://raw.githubusercontent.com/sd88me/mpc-addin-skipback/dc82602b2a05354d42445c8a330fb92a864892fb/docs/img/skipback.png", "https://github.com/sd88me/mpc-addin-skipback/releases/download/v0.6.2/MPC-Skipback-0.6.2-mpc-armv7.zip", "plugins/skipback/thumb.png",
            "Records the main output all the time and saves the last 30 to 60 seconds as a WAV when you double-press a hardware button (Rec Arm on a Force; Overdub, or a button you teach it, on other models). Tested on a Gen1 Force; other models are unverified.", "./plugins/skipback/install.sh", 0,
            false,
            false,
            false,
            { "skipback", "recording", "sampling", "wav", "buffer" }
        },
        {
            "usb-audio", "MPC USB audio", "jacob-sabella", "v1.1.1", "Audio Interface",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "3472", "64 KB", "14%",
            "jacob-sabella - VST - MPC USB audio", "libmpc_usb_audio.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-usb-audio/f73cf3a1de3c3d005f30b6d38caddf587376e6d4/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-usb-audio/releases/download/usb-audio-addin-v1.1.1/MPC-USB-audio-addin-1.1.1-mpc-armv7.zip", "plugins/usb-audio/thumb.png",
            "The MPC as a USB audio interface: its outputs and inputs as channels on the computer, next to its MIDI ports.", "./plugins/usb-audio/install.sh", 0,
            false,
            false,
            false,
            { "usb", "audio-interface", "recording", "inputs" }
        },
        {
            "nova-audio", "Nova Audio", "3dthc", "v0.2.0", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "25", "64 KB", "14%",
            "3dthc - VST - Nova Audio", "libnova_audio.so",
            "", "https://github.com/3dthc/mpc-addin-nova-audio/releases/download/nova-audio-v0.2.0/Nova-Audio-for-Nova-Editor-0.2.0-mpc-armv7.zip", "plugins/nova-audio/thumb.png",
            "Lets a plugin play the MPC's audio inputs as its own sound, so a hardware synth becomes one ordinary MPC track: played, monitored, recorded, mixed and resampled on the plugin's own track, with no separate audio track and no Monitor switching. Written for Nova Editor but not MiniNova-specific; any plugin can read the tap.", "./plugins/nova-audio/install.sh", 0,
            false,
            false,
            false,
            { "audio-input", "hardware-synth", "mininova", "ld-preload" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "DEVICE PATCHES";
        sec.items = {
        {
            "patch-auto-exec", "Auto exec: run plugins from any noexec drive", "sd88me (mount logic after timomacquis)", "v1.0.0", "Device Patch",
            "PATCH", "Stable", "MPC OS Gen1 (Akai Force 3.9.1)", "MPC OS 2.x & 3.x (armv7l)", "2026-10-14",
            "412", "18 KB", "1%",
            "patches/auto-exec", "auto-exec-patch.sh",
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/1a6251e5dc90386a010179cb2173a51831cbbf4b/tools/mpc_patch/auto_exec/auto-exec-patch.sh", "plugins/patch-auto-exec/thumb.png",
            "Recommended for a noexec drive. Makes the Synths folder of every noexec drive (SSD, USB stick, SD card) executable through a small boot-time service, with nothing to choose; the drives stay noexec. No MPC restart required and undo is built in.", "(\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/1a6251e5dc90386a010179cb2173a51831cbbf4b/tools/mpc_patch/auto_exec/auto-exec-patch.sh\" /tmp/auto-exec-patch.sh) && sh /tmp/auto-exec-patch.sh", 1,
            false,
            false,
            false,
            { "device-patch", "noexec", "systemd", "mount", "undo-built-in" }
        },
        {
            "patch-16-pad-drum-layout", "16-pad drum layout for selected plugins", "sd88me", "v1.0.0", "Device Patch",
            "PATCH", "Stable", "MPC OS 3.9.1.2 (armv7l)", "MPC OS 3.9.1.2", "2026-10-12",
            "318", "24 KB", "1%",
            "patches/drum-pad-layout", "mpc-drum-pad-patch.sh",
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/6ddf68cf92b3f1c0bdd322231ed133c0cc2eed48/tools/mpc_patch/mpc-drum-pad-patch.sh", "plugins/patch-16-pad-drum-layout/thumb.png",
            "Machinedrum Module / Machinemodule, 6W6, 8W8, CW-78, 9W9, TR-MPC and Lucky Dip get MPC's drum layout with all 16 pads lit. Backs up /usr/bin/MPC to /sdcard/MPC-backup with built-in undo.", "(\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/6ddf68cf92b3f1c0bdd322231ed133c0cc2eed48/tools/mpc_patch/mpc-drum-pad-patch.sh\" /tmp/mpc-drum-pad-patch.sh) && sh /tmp/mpc-drum-pad-patch.sh", 1,
            false,
            false,
            false,
            { "device-patch", "16-pad", "drum-layout", "tr-mpc", "6w6", "8w8", "9w9" }
        },
        {
            "patch-button-remap", "Button remap: open screens from hardware buttons", "mmiroshnikov", "v1.0.0", "Device Patch",
            "PATCH", "Stable", "MPC Live (Hakai 3.9.1), Force (3.9.0)", "MPC OS 3.9 (armv7l)", "2026-10-10",
            "275", "42 KB", "1%",
            "patches/hwremap", "hwremap-patch.sh",
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/a9bb8c71ba382fa6c3902fbbdead6d5f620222a5/tools/mpc_patch/hwremap/hwremap-patch.sh", "plugins/patch-button-remap/thumb.png",
            "Remap hardware buttons on a standalone MPC or Force via an LD_PRELOAD shim (/usr/lib/hwremap.so): a button can become other buttons, a pad, or a screen tap. Pick which remaps you want at install (Mixer, Edit, Clip, Menu, and Knobs options).", "(\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/a9bb8c71ba382fa6c3902fbbdead6d5f620222a5/tools/mpc_patch/hwremap/hwremap-patch.sh\" /tmp/hwremap-patch.sh) && sh /tmp/hwremap-patch.sh", 1,
            false,
            false,
            false,
            { "device-patch", "hwremap", "buttons", "ld-preload", "force", "mpc-live" }
        },
        {
            "patch-drive-exec", "Drive exec: run plugins from a noexec drive (SSD)", "timomacquis (adapted by sd88me)", "v1.0.0", "Device Patch",
            "PATCH", "Beta", "MPC OS Gen1 (Akai Force 3.9.1)", "MPC OS Gen1 (armv7l)", "2026-10-08",
            "164", "16 KB", "1%",
            "patches/drive-exec", "drive-exec-patch.sh",
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/f036a8791daff1ccf426c3953e84a8942c979564/tools/mpc_patch/drive_exec/drive-exec-patch.sh", "plugins/patch-drive-exec/thumb.png",
            "Makes one selected drive Synths folder executable through a boot-time systemd bind-mount service while keeping the drive itself noexec. (Superseded by Auto exec, retained for uninstall compatibility.)", "(\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/f036a8791daff1ccf426c3953e84a8942c979564/tools/mpc_patch/drive_exec/drive-exec-patch.sh\" /tmp/drive-exec-patch.sh) && sh /tmp/drive-exec-patch.sh", 1,
            false,
            false,
            false,
            { "device-patch", "ssd", "noexec", "bind-mount" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "COMPANION APPS";
        sec.items = {
        {
            "companion-mpc-link", "MPC Link", "Padbangers", "v1.0.2", "Companion App",
            "APP", "Stable", "MPC X, MPC OS 3.9.1 (macOS & Windows)", "MPC OS 2.x & 3.x (SSH)", "2026-10-15",
            "690", "4.1 MB", "2%",
            "companion/mpc-link", "mpc-link-sshfs",
            "https://raw.githubusercontent.com/padbangers/mpc-link/main/assets/catalog.png", "https://github.com/padbangers/mpc-link/releases/download/v1.0.2/MPC-Link-1.0.2-macOS.zip", "plugins/companion-mpc-link/thumb.png",
            "Menu bar / tray app that puts folders from your computer into the MPC's browser over the network: load samples straight from the computer, save projects back to it. Connects over SSH, copies a static sshfs to the device and mounts the shared folders on a drive MPC already shows.", "echo \"[MPC Link] Companion app runs on macOS/Windows and connects to root@<mpc-ip> via SSHFS.\" && mkdir -p /media/AKAI_FORCE_SSD/MPC_Link_Shares", 1,
            false,
            false,
            false,
            { "companion-app", "sshfs", "network-mount", "macos", "windows", "samples" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
}

static const char* find_matching_delim(const char* start, char open_c, char close_c) {
    if (!start || *start != open_c) return nullptr;
    int depth = 1;
    bool in_quotes = false;
    const char* p = start + 1;
    while (*p && depth > 0) {
        if (*p == '\\' && in_quotes && *(p + 1)) {
            p += 2;
            continue;
        }
        if (*p == '"') {
            in_quotes = !in_quotes;
        } else if (!in_quotes) {
            if (*p == open_c) depth++;
            else if (*p == close_c) {
                depth--;
                if (depth == 0) return p;
            }
        }
        p++;
    }
    return nullptr;
}

static bool extract_string_field(const char* start, const char* end, const char* key, std::string& out) {
    std::string pattern = std::string("\"") + key + "\"";
    const char* p = start;
    while (p < end) {
        const char* found = std::strstr(p, pattern.c_str());
        if (!found || found >= end) return false;
        const char* colon = found + pattern.size();
        while (colon < end && (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n')) colon++;
        if (colon < end && *colon == ':') {
            const char* val = colon + 1;
            while (val < end && (*val == ' ' || *val == '\t' || *val == '\r' || *val == '\n')) val++;
            if (val < end && *val == '"') {
                val++;
                out.clear();
                while (val < end && *val != '"') {
                    if (*val == '\\' && (val + 1) < end) {
                        val++;
                        if (*val == 'n') out += '\n';
                        else if (*val == 't') out += '\t';
                        else out += *val;
                    } else {
                        out += *val;
                    }
                    val++;
                }
                return true;
            }
        }
        p = found + pattern.size();
    }
    return false;
}

static void extract_string_array(const char* start, const char* end, const char* key, std::vector<std::string>& out) {
    std::string pattern = std::string("\"") + key + "\"";
    const char* found = std::strstr(start, pattern.c_str());
    if (!found || found >= end) return;
    const char* arr_open = std::strchr(found + pattern.size(), '[');
    if (!arr_open || arr_open >= end) return;
    const char* arr_close = find_matching_delim(arr_open, '[', ']');
    if (!arr_close || arr_close > end) return;

    out.clear();
    const char* p = arr_open + 1;
    while (p < arr_close) {
        const char* q1 = std::strchr(p, '"');
        if (!q1 || q1 >= arr_close) break;
        const char* q2 = q1 + 1;
        std::string s;
        while (q2 < arr_close && *q2 != '"') {
            if (*q2 == '\\' && (q2 + 1) < arr_close) q2++;
            s += *q2++;
        }
        if (!s.empty()) out.push_back(s);
        p = q2 + 1;
    }
}

static bool extract_int_field(const char* start, const char* end, const char* key, int& out) {
    std::string pattern = std::string("\"") + key + "\"";
    const char* found = std::strstr(start, pattern.c_str());
    if (!found || found >= end) return false;
    const char* colon = found + pattern.size();
    while (colon < end && (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n')) colon++;
    if (colon < end && *colon == ':') {
        const char* val = colon + 1;
        while (val < end && (*val == ' ' || *val == '\t' || *val == '\r' || *val == '\n')) val++;
        if (val < end && (*val == '-' || (*val >= '0' && *val <= '9'))) {
            out = std::atoi(val);
            return true;
        }
    }
    return false;
}

static bool extract_raw_code_field(const char* start, const char* end, std::string& out) {
    const char* found = std::strstr(start, "\"code\"");
    if (!found || found >= end) return false;
    const char* p = found + 6;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end || *p != ':') return false;
    p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end || *p != '{') return false;

    const char* close_brace = find_matching_delim(p, '{', '}');
    if (!close_brace || close_brace > end) return false;

    out.assign(p + 1, close_brace - (p + 1));
    while (!out.empty() && (out.front() == '\n' || out.front() == '\r' || out.front() == ' ')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return true;
}

bool ConfigParser::loadFromFile(const std::string& configPath, LauncherConfig& outConfig) {
    outConfig.config_path = configPath;
    std::ifstream file(configPath, std::ios::binary);
    if (!file.is_open()) {
        std::printf("[TKGL-CFG] %s not found, loading embedded catalog.\n", configPath.c_str());
        outConfig.loadEmbeddedDefaults();
        refreshInstallStates(outConfig);
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string buf = ss.str();
    const char* raw = buf.c_str();

    // 1. Parse "global": { ... }
    const char* global_key = std::strstr(raw, "\"global\"");
    if (global_key) {
        const char* brace = std::strchr(global_key, '{');
        const char* end_brace = find_matching_delim(brace, '{', '}');
        if (brace && end_brace) {
            extract_string_field(brace, end_brace, "title", outConfig.app_title);
            extract_int_field(brace, end_brace, "rotation", outConfig.screen_rotation);
            extract_string_field(brace, end_brace, "catalog_url", outConfig.catalog_url);
            extract_string_field(brace, end_brace, "ping_host", outConfig.ping_host);
        }
    }
    if (outConfig.ping_host.empty()) {
        outConfig.ping_host = "github.com";
    }

    // 2. Parse "sections": [ ... ]
    outConfig.sections.clear();
    const char* sections_key = std::strstr(raw, "\"sections\"");
    if (sections_key) {
        const char* arr_start = std::strchr(sections_key, '[');
        const char* arr_end = find_matching_delim(arr_start, '[', ']');
        if (arr_start && arr_end) {
            const char* p = arr_start + 1;
            while (p < arr_end) {
                const char* sec_open = std::strchr(p, '{');
                if (!sec_open || sec_open >= arr_end) break;
                const char* sec_close = find_matching_delim(sec_open, '{', '}');
                if (!sec_close || sec_close > arr_end) break;

                Section sec;
                extract_string_field(sec_open, sec_close, "section", sec.title);
                if (sec.title.empty()) sec.title = "Plugins";

                const char* items_key = std::strstr(sec_open, "\"items\"");
                if (items_key && items_key < sec_close) {
                    const char* items_arr_open = std::strchr(items_key, '[');
                    const char* items_arr_close = find_matching_delim(items_arr_open, '[', ']');
                    if (items_arr_open && items_arr_close && items_arr_close <= sec_close) {
                        const char* ip = items_arr_open + 1;
                        while (ip < items_arr_close) {
                            const char* item_open = std::strchr(ip, '{');
                            if (!item_open || item_open >= items_arr_close) break;
                            const char* item_close = find_matching_delim(item_open, '{', '}');
                            if (!item_close || item_close > items_arr_close) break;

                            MenuItem item;
                            item.category = sec.title;
                            extract_string_field(item_open, item_close, "id", item.id);
                            extract_string_field(item_open, item_close, "item", item.label);
                            extract_string_field(item_open, item_close, "author", item.author);
                            extract_string_field(item_open, item_close, "version", item.version);
                            extract_string_field(item_open, item_close, "size", item.sizeMb);
                            extract_string_field(item_open, item_close, "updated", item.updatedDate);
                            extract_string_field(item_open, item_close, "downloads", item.downloads);
                            extract_string_field(item_open, item_close, "tested", item.testedDevice);
                            extract_string_field(item_open, item_close, "screenshot", item.screenshotUrl);
                            extract_string_field(item_open, item_close, "url", item.downloadUrl);
                            extract_string_field(item_open, item_close, "description", item.description);
                            extract_string_field(item_open, item_close, "target_subdir", item.targetSubdir);
                            extract_string_field(item_open, item_close, "binary", item.targetBinary);
                            extract_string_array(item_open, item_close, "tags", item.tags);

                            int instFlag = 0, brkFlag = 0;
                            if (extract_int_field(item_open, item_close, "installed", instFlag)) {
                                item.installed = (instFlag != 0);
                            }
                            if (extract_int_field(item_open, item_close, "broken", brkFlag)) {
                                item.broken = (brkFlag != 0);
                            }

                            if (item.tags.empty()) {
                                item.tags = {"mpc", "standalone", "vst2", "armv7"};
                            }
                            if (!item.id.empty()) {
                                item.thumbPpmPath = "plugins/" + item.id + "/thumb.png";
                            }

                            if (extract_raw_code_field(item_open, item_close, item.cmd)) {
                                item.is_inline = 1;
                            } else if (extract_string_field(item_open, item_close, "script", item.cmd)) {
                                item.is_inline = 0;
                            }

                            if (item.downloadUrl.empty() && !item.cmd.empty()) {
                                size_t hpos = item.cmd.find("https://");
                                if (hpos != std::string::npos) {
                                    size_t hend = item.cmd.find_first_of(" \"'\t\r\n", hpos);
                                    item.downloadUrl = (hend != std::string::npos)
                                        ? item.cmd.substr(hpos, hend - hpos)
                                        : item.cmd.substr(hpos);
                                }
                            }

                            if (!item.label.empty()) {
                                sec.items.push_back(item);
                            }
                            ip = item_close + 1;
                        }
                    }
                }

                if (!sec.items.empty()) {
                    outConfig.sections.push_back(sec);
                }
                p = sec_close + 1;
            }
        }
    }

    if (outConfig.sections.empty()) {
        outConfig.loadEmbeddedDefaults();
    }
    refreshInstallStates(outConfig);
    return true;
}

static std::string escapeJsonString(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 16);
    for (char c : raw) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

bool ConfigParser::saveToFile(const std::string& configPath, const LauncherConfig& config) {
    std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    std::string pingHost = config.ping_host.empty() ? "github.com" : config.ping_host;
    out << "{\n";
    out << "  \"global\": {\n";
    out << "    \"title\": \"" << escapeJsonString(config.app_title) << "\",\n";
    out << "    \"rotation\": " << config.screen_rotation << ",\n";
    out << "    \"catalog_url\": \"" << escapeJsonString(config.catalog_url) << "\",\n";
    out << "    \"ping_host\": \"" << escapeJsonString(pingHost) << "\"\n";
    out << "  },\n";
    out << "  \"sections\": [\n";

    for (size_t s = 0; s < config.sections.size(); ++s) {
        const auto& sec = config.sections[s];
        out << "    {\n";
        out << "      \"section\": \"" << escapeJsonString(sec.title) << "\",\n";
        out << "      \"items\": [\n";
        for (size_t i = 0; i < sec.items.size(); ++i) {
            const auto& it = sec.items[i];
            out << "        {\n";
            out << "          \"id\": \"" << escapeJsonString(it.id) << "\",\n";
            out << "          \"item\": \"" << escapeJsonString(it.label) << "\",\n";
            out << "          \"author\": \"" << escapeJsonString(it.author) << "\",\n";
            out << "          \"version\": \"" << escapeJsonString(it.version) << "\",\n";
            out << "          \"size\": \"" << escapeJsonString(it.sizeMb) << "\",\n";
            out << "          \"updated\": \"" << escapeJsonString(it.updatedDate) << "\",\n";
            out << "          \"downloads\": \"" << escapeJsonString(it.downloads) << "\",\n";
            out << "          \"tested\": \"" << escapeJsonString(it.testedDevice) << "\",\n";
            out << "          \"screenshot\": \"" << escapeJsonString(it.screenshotUrl) << "\",\n";
            out << "          \"url\": \"" << escapeJsonString(it.downloadUrl) << "\",\n";
            out << "          \"description\": \"" << escapeJsonString(it.description) << "\",\n";
            out << "          \"target_subdir\": \"" << escapeJsonString(it.targetSubdir) << "\",\n";
            out << "          \"binary\": \"" << escapeJsonString(it.targetBinary) << "\",\n";
            out << "          \"installed\": " << (it.installed ? 1 : 0) << ",\n";
            out << "          \"broken\": " << (it.broken ? 1 : 0) << ",\n";
            if (it.is_inline) {
                out << "          \"code\": {\n            " << it.cmd << "\n          }\n";
            } else {
                out << "          \"script\": \"" << escapeJsonString(it.cmd) << "\"\n";
            }
            out << "        }" << (i + 1 < sec.items.size() ? "," : "") << "\n";
        }
        out << "      ]\n";
        out << "    }" << (s + 1 < config.sections.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    return true;
}

bool ConfigParser::syncFromOnlineCatalog(
    LauncherConfig& config,
    std::vector<std::string>& outLogs,
    std::function<void(const std::string&)> onProgress
) {
    std::string mgrDir = getManagerRootDir();
    if (!mgrDir.empty()) {
        chdir(mgrDir.c_str());
    }

    // 1. Mandatory internet reachability check before any refresh operation
    std::string pingHost = config.ping_host.empty() ? "github.com" : config.ping_host;
    outLogs.push_back("[TKGL-SYNC] Checking internet access (ping " + pingHost + ")...");
    if (onProgress) onProgress("[TKGL-SYNC] Pinging " + pingHost + "...");
    if (!checkInternetReachable(pingHost, &outLogs)) {
        outLogs.push_back("[ERROR] No internet access (ping " + pingHost + " failed). Refresh aborted.");
        if (onProgress) onProgress("[ERROR] No internet (" + pingHost + " unreachable). Refresh impossible.");
        return false;
    }

    std::string url = config.catalog_url;
    if (url.empty()) url = "https://sd88me.github.io/mpc-vst-plugins/catalog.json";
    if (url.back() == '/') url += "catalog.json";
    std::string fallbackUrl = "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/gh-pages/catalog.json";

    mkdir("plugins", 0755);
    outLogs.push_back("[TKGL-SYNC] Downloading online catalog via libcurl (IPv4): " + url);
    if (onProgress) onProgress("[TKGL-SYNC] Fetching catalog JSON via libcurl (IPv4)...");

    std::string tmpCat = "/tmp/tkgl_catalog_dl.json";
    unlink(tmpCat.c_str());
    bool dlOk = downloadFile(url, tmpCat, 20, onProgress, "[TKGL-SYNC] Downloading catalog");
    if (!dlOk && url != fallbackUrl) {
        outLogs.push_back("[TKGL-SYNC] Trying raw GitHub mirror: " + fallbackUrl);
        dlOk = downloadFile(fallbackUrl, tmpCat, 20, onProgress, "[TKGL-SYNC] Downloading mirror catalog");
    }
    if (!dlOk) {
        outLogs.push_back("[ERROR] Failed to download online catalog via libcurl. Refresh aborted.");
        if (onProgress) onProgress("[ERROR] Catalog download failed via libcurl.");
        return false;
    }

    {
        std::ifstream src(tmpCat, std::ios::binary);
        std::ofstream dst("plugins/tkgl_catalog.json", std::ios::binary | std::ios::trunc);
        if (src.is_open() && dst.is_open()) {
            dst << src.rdbuf();
            dst.close();
        }
        unlink(tmpCat.c_str());
    }

    struct stat stCat;
    if (stat("plugins/tkgl_catalog.json", &stCat) != 0 || stCat.st_size < 64) {
        outLogs.push_back("[ERROR] Invalid or empty plugins/tkgl_catalog.json.");
        return false;
    }

    std::ifstream file("plugins/tkgl_catalog.json", std::ios::binary);
    if (!file.is_open()) {
        outLogs.push_back("[ERROR] Could not open plugins/tkgl_catalog.json.");
        return false;
    }
    outLogs.push_back(std::string("[TKGL-SYNC] Loaded catalog plugins/tkgl_catalog.json (") + std::to_string(stCat.st_size / 1024) + " KB)");
    if (onProgress) onProgress("[TKGL-SYNC] Parsing catalog entries...");

    std::stringstream ss;
    ss << file.rdbuf();
    std::string buf = ss.str();
    const char* raw = buf.c_str();

    const char* plugins_key = std::strstr(raw, "\"plugins\"");
    if (!plugins_key) {
        outLogs.push_back("[ERROR] Invalid catalog.json: missing plugins array.");
        return false;
    }

    const char* arr_open = std::strchr(plugins_key, '[');
    const char* arr_close = find_matching_delim(arr_open, '[', ']');
    if (!arr_open || !arr_close) return false;

    Section secInst;    secInst.title    = "INSTRUMENTS";
    Section secMidi;    secMidi.title    = "MIDI";
    Section secFx;      secFx.title      = "EFFECTS";
    Section secAddins;  secAddins.title  = "ADDINS";
    Section secPatches; secPatches.title = "DEVICE PATCHES";
    Section secApps;    secApps.title    = "COMPANION APPS";

    // Preserve existing DEVICE PATCHES and COMPANION APPS items, and remember previously installed plugins
    std::unordered_map<std::string, bool> prevInstalledMap;
    for (const auto& existingSec : config.sections) {
        if (existingSec.title == "DEVICE PATCHES") secPatches.items = existingSec.items;
        if (existingSec.title == "COMPANION APPS") secApps.items = existingSec.items;
        for (const auto& existingIt : existingSec.items) {
            if (existingIt.installed) {
                if (!existingIt.id.empty()) prevInstalledMap[existingIt.id] = true;
                if (!existingIt.label.empty()) prevInstalledMap[existingIt.label] = true;
            }
        }
    }

    auto ensureThumbDownloaded = [&](MenuItem& it, int itemNumber) {
        if (it.id.empty()) return;
        std::string plugDir = "plugins/" + it.id;
        mkdir("plugins", 0755);
        mkdir(plugDir.c_str(), 0755);
        std::string localPng = plugDir + "/thumb.png";
        std::string localPpm = plugDir + "/thumb.ppm";

        // Normalize relative screenshot URL if needed
        if (!it.screenshotUrl.empty() &&
            it.screenshotUrl.rfind("http://", 0) != 0 &&
            it.screenshotUrl.rfind("https://", 0) != 0) {
            std::string baseUrl = url;
            size_t slashPos = baseUrl.find_last_of('/');
            if (slashPos != std::string::npos) {
                baseUrl = baseUrl.substr(0, slashPos + 1);
            } else {
                baseUrl += "/";
            }
            if (!it.screenshotUrl.empty() && it.screenshotUrl[0] == '/') {
                it.screenshotUrl.erase(0, 1);
            }
            it.screenshotUrl = baseUrl + it.screenshotUrl;
        }

        struct stat stPng;
        if (stat(localPng.c_str(), &stPng) == 0 && stPng.st_size < 64) {
            unlink(localPng.c_str());
        }
        bool hasPng = (stat(localPng.c_str(), &stPng) == 0 && stPng.st_size >= 64);

        // If thumb.png does not exist on disk, download it via libcurl (IPv4)
        if (!hasPng && !it.screenshotUrl.empty()) {
            if (onProgress) {
                int dotCount = (itemNumber % 5) + 1;
                std::string dots;
                for (int d = 0; d < 5; ++d) dots += (d < dotCount) ? " * " : " . ";
                onProgress("[SYNC " + dots + "] Downloading thumb: " + it.label);
            }
            if (downloadFile(it.screenshotUrl, localPng, 15)) {
                hasPng = (stat(localPng.c_str(), &stPng) == 0 && stPng.st_size >= 64);
                if (hasPng) {
                    unlink(localPpm.c_str());
                    outLogs.push_back("[TKGL-SYNC] Downloaded thumb: " + localPng);
                }
            } else {
                outLogs.push_back("[WARNING] Failed to download thumb for " + it.label + ": " + it.screenshotUrl);
            }
        }
        it.thumbPpmPath = localPng;
    };

    const char* p = arr_open + 1;
    int count = 0;
    while (p < arr_close) {
        const char* p_open = std::strchr(p, '{');
        if (!p_open || p_open >= arr_close) break;
        const char* p_close = find_matching_delim(p_open, '{', '}');
        if (!p_close || p_close > arr_close) break;

        MenuItem item;
        extract_string_field(p_open, p_close, "id", item.id);
        extract_string_field(p_open, p_close, "name", item.label);
        extract_string_field(p_open, p_close, "author", item.author);
        extract_string_field(p_open, p_close, "style", item.category);
        extract_string_field(p_open, p_close, "summary", item.description);
        extract_string_field(p_open, p_close, "screenshot", item.screenshotUrl);
        if (item.screenshotUrl.empty()) extract_string_field(p_open, p_close, "image", item.screenshotUrl);
        if (item.screenshotUrl.empty()) extract_string_field(p_open, p_close, "thumbnail", item.screenshotUrl);
        extract_string_array(p_open, p_close, "tags", item.tags);
        if (item.tags.empty()) item.tags = {"synth", "mpc", "vst2"};

        std::string kind, role;
        extract_string_field(p_open, p_close, "kind", kind);
        extract_string_field(p_open, p_close, "role", role);

        // Extract latest version entry from versions[0]
        const char* vers_key = std::strstr(p_open, "\"versions\"");
        if (vers_key && vers_key < p_close) {
            const char* v_arr = std::strchr(vers_key, '[');
            const char* v_obj = v_arr ? std::strchr(v_arr, '{') : nullptr;
            const char* v_close = v_obj ? find_matching_delim(v_obj, '{', '}') : nullptr;
            if (v_obj && v_close && v_close <= p_close) {
                std::string ver;
                if (extract_string_field(v_obj, v_close, "version", ver)) item.version = "v" + ver;
                extract_string_field(v_obj, v_close, "date", item.updatedDate);
                if (item.screenshotUrl.empty()) {
                    extract_string_field(v_obj, v_close, "screenshot", item.screenshotUrl);
                }

                // Extract download URL: check assets.armv7.url first, then download_url/package_url, then scan all "url" fields for .zip/release asset
                const char* armv7_key = std::strstr(v_obj, "\"armv7\"");
                if (armv7_key && armv7_key < v_close) {
                    const char* arm_open = std::strchr(armv7_key, '{');
                    const char* arm_close = arm_open ? find_matching_delim(arm_open, '{', '}') : nullptr;
                    if (arm_open && arm_close && arm_close <= v_close) {
                        extract_string_field(arm_open, arm_close, "url", item.downloadUrl);
                    }
                }
                if (item.downloadUrl.empty()) {
                    extract_string_field(v_obj, v_close, "download_url", item.downloadUrl);
                }
                if (item.downloadUrl.empty()) {
                    extract_string_field(v_obj, v_close, "package_url", item.downloadUrl);
                }
                if (item.downloadUrl.empty()) {
                    const char* scanP = v_obj;
                    std::string firstUrl;
                    while (scanP < v_close) {
                        const char* uFound = std::strstr(scanP, "\"url\"");
                        if (!uFound || uFound >= v_close) break;
                        std::string candidate;
                        if (extract_string_field(uFound, v_close, "url", candidate) && !candidate.empty()) {
                            if (firstUrl.empty()) firstUrl = candidate;
                            if (candidate.find(".zip") != std::string::npos ||
                                candidate.find("/releases/download/") != std::string::npos ||
                                candidate.find("raw.githubusercontent.com") != std::string::npos) {
                                item.downloadUrl = candidate;
                                break;
                            }
                        }
                        scanP = uFound + 5;
                    }
                    if (item.downloadUrl.empty()) item.downloadUrl = firstUrl;
                }

                int sizeB = 0, dl = 0;
                if (extract_int_field(v_obj, v_close, "size", sizeB) && sizeB > 0) {
                    char mbBuf[32];
                    std::snprintf(mbBuf, sizeof(mbBuf), "%.1f MB", static_cast<double>(sizeB) / (1024.0 * 1024.0));
                    item.sizeMb = mbBuf;
                }
                if (extract_int_field(v_obj, v_close, "downloads", dl)) {
                    item.downloads = std::to_string(dl);
                }
                extract_string_field(v_obj, v_close, "so", item.targetBinary);
                extract_string_field(v_obj, v_close, "skin", item.targetSubdir);
                extract_string_field(v_obj, v_close, "os_compat", item.osCompat);
                std::string dev;
                if (extract_string_field(v_obj, v_close, "device", dev)) item.testedDevice = dev;
            }
        }
        if (item.downloadUrl.empty()) {
            extract_string_field(p_open, p_close, "url", item.downloadUrl);
        }

        if (item.targetSubdir.empty()) item.targetSubdir = item.author + " - VST - " + item.label;
        if (item.targetBinary.empty()) item.targetBinary = item.id + ".so";

        // Preserve previous installed detection state so refreshInstallStates verifies MPC.settings & .so
        if ((!item.id.empty() && prevInstalledMap[item.id]) || (!item.label.empty() && prevInstalledMap[item.label])) {
            item.installed = true;
        }

        // Download thumb.png if it does not exist on disk
        ensureThumbDownloaded(item, count + 1);

        item.is_inline = 1;
        item.cmd = "mkdir -p \"$SYNTH_INSTALL_DIR/" + item.targetSubdir + "\" && (\"$PLG_MAN_DIR/mpc_plugin_manager_fb\" --fetch \"" + item.downloadUrl + "\" /tmp/" + item.id + ".zip) && unzip -o /tmp/" + item.id + ".zip -d \"$SYNTH_INSTALL_DIR/" + item.targetSubdir + "/\" && rm -f /tmp/" + item.id + ".zip && sync";

        if (!item.label.empty()) {
            if (kind == "addin") {
                item.shortType = "ADDIN";
                secAddins.items.push_back(item);
            } else if (role == "midi" || item.category.find("MIDI") != std::string::npos || item.category.find("Sequencer") != std::string::npos) {
                item.shortType = "MIDI";
                secMidi.items.push_back(item);
            } else if (kind == "effect" || item.category.find("Effect") != std::string::npos) {
                item.shortType = "FX";
                secFx.items.push_back(item);
            } else {
                item.shortType = "INST";
                secInst.items.push_back(item);
            }
            count++;
            if (onProgress) {
                int dotCount = (count % 5) + 1;
                std::string dots;
                for (int d = 0; d < 5; ++d) dots += (d < dotCount) ? " * " : " . ";
                onProgress("[SYNC " + dots + "] (" + std::to_string(count) + ") " + item.label + " (" + item.category + ")");
            }
        }
        p = p_close + 1;
    }

    // Also ensure thumbnails are downloaded for any preserved DEVICE PATCHES and COMPANION APPS items
    for (auto& patchIt : secPatches.items) {
        ensureThumbDownloaded(patchIt, ++count);
    }
    for (auto& appIt : secApps.items) {
        ensureThumbDownloaded(appIt, ++count);
    }

    if (count > 0) {
        config.sections.clear();
        if (!secInst.items.empty())    config.sections.push_back(secInst);
        if (!secMidi.items.empty())    config.sections.push_back(secMidi);
        if (!secFx.items.empty())      config.sections.push_back(secFx);
        if (!secAddins.items.empty())  config.sections.push_back(secAddins);
        if (!secPatches.items.empty()) config.sections.push_back(secPatches);
        if (!secApps.items.empty())    config.sections.push_back(secApps);
        refreshInstallStates(config, &outLogs);
        saveToFile(config.config_path, config);
        outLogs.push_back("[TKGL-SYNC] Classified " + std::to_string(count) + " plugins into " + std::to_string(config.sections.size()) + " sections in " + config.config_path);
        return true;
    }
    return false;
}

// ============================================================================
// Safe MPC.settings XML Manipulation & Integrity Engine
// ============================================================================

static std::string getBackupTimestampYYYYMMDDHHMMSS() {
    std::time_t now = std::time(nullptr);
    struct tm tmBuf;
    struct tm* lt = localtime_r(&now, &tmBuf);
    char buf[32];
    if (lt && std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", lt) > 0) {
        return std::string(buf);
    }
    return "20260101000000";
}

static std::string xmlUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 5; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out += '"';  i += 5; continue; }
            if (s.compare(i, 4, "&lt;")   == 0) { out += '<';  i += 3; continue; }
            if (s.compare(i, 4, "&gt;")   == 0) { out += '>';  i += 3; continue; }
            if (s.compare(i, 5, "&amp;")  == 0) { out += '&';  i += 4; continue; }
        }
        out += s[i];
    }
    return out;
}

static std::string xmlEscapeAttr(const std::string& raw) {
    std::string unescaped = xmlUnescape(raw);
    std::string out;
    out.reserve(unescaped.size() + 16);
    for (char c : unescaped) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            default:   out += c;        break;
        }
    }
    return out;
}

static std::string xmlEscapeText(const std::string& raw) {
    std::string unescaped = xmlUnescape(raw);
    std::string out;
    out.reserve(unescaped.size() + 16);
    for (char c : unescaped) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            default:   out += c;        break;
        }
    }
    return out;
}

static std::string resolveMpcSettingsPath() {
    const char* candMpc[] = {
        "/media/az01-internal/Settings/MPC/MPC.settings",
        "/media/internal/Settings/MPC/MPC.settings",
        "/var/lib/MPC/MPC.settings",
        "/root/.config/Akai/MPC/MPC.settings"
    };
    for (const char* cp : candMpc) {
        struct stat st;
        if (stat(cp, &st) == 0 && S_ISREG(st.st_mode)) {
            return std::string(cp);
        }
    }
    return "/media/az01-internal/Settings/MPC/MPC.settings";
}

// Finds the closing '>' of an XML tag starting at tagStart (quote-aware across multiple lines).
// Sets outIsSelfClosing = true if the tag ends with "/>". Returns index right after '>', or npos.
static size_t findXmlTagEnd(const std::string& xml, size_t tagStart, bool& outIsSelfClosing) {
    outIsSelfClosing = false;
    if (tagStart >= xml.size() || xml[tagStart] != '<') return std::string::npos;
    bool inDoubleQuote = false;
    bool inSingleQuote = false;
    char lastNonSpace = 0;
    for (size_t i = tagStart + 1; i < xml.size(); ++i) {
        char c = xml[i];
        if (c == '"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
        } else if (c == '\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
        } else if (!inDoubleQuote && !inSingleQuote) {
            if (c == '>') {
                outIsSelfClosing = (lastNonSpace == '/');
                return i + 1;
            }
            if (c == '<') {
                // Unexpected '<' before closing '>' -> malformed tag
                return std::string::npos;
            }
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                lastNonSpace = c;
            }
        }
    }
    return std::string::npos;
}

static std::string getXmlTagAttribute(const std::string& tag, const std::string& attrName) {
    std::string needle = attrName + "=";
    size_t pos = 0;
    while ((pos = tag.find(needle, pos)) != std::string::npos) {
        if (pos > 0) {
            char prev = tag[pos - 1];
            if (prev != ' ' && prev != '\t' && prev != '\r' && prev != '\n') {
                pos += needle.size();
                continue;
            }
        }
        size_t valStart = pos + needle.size();
        if (valStart < tag.size() && (tag[valStart] == '"' || tag[valStart] == '\'')) {
            char quote = tag[valStart];
            size_t valEnd = tag.find(quote, valStart + 1);
            if (valEnd != std::string::npos) {
                return xmlUnescape(tag.substr(valStart + 1, valEnd - (valStart + 1)));
            }
        }
        pos += needle.size();
    }
    return "";
}

static std::string setXmlTagAttribute(const std::string& tag, const std::string& attrName, const std::string& newRawValue) {
    std::string escapedVal = xmlEscapeAttr(newRawValue);
    std::string needle = attrName + "=";
    size_t pos = 0;
    while ((pos = tag.find(needle, pos)) != std::string::npos) {
        if (pos > 0) {
            char prev = tag[pos - 1];
            if (prev != ' ' && prev != '\t' && prev != '\r' && prev != '\n') {
                pos += needle.size();
                continue;
            }
        }
        size_t valStart = pos + needle.size();
        if (valStart < tag.size() && (tag[valStart] == '"' || tag[valStart] == '\'')) {
            char quote = tag[valStart];
            size_t valEnd = tag.find(quote, valStart + 1);
            if (valEnd != std::string::npos) {
                std::string updated = tag;
                updated.replace(valStart, (valEnd - valStart) + 1, std::string("\"") + escapedVal + "\"");
                return updated;
            }
        }
        pos += needle.size();
    }
    return tag;
}

// Locates <VALUE name="valueName"...> block in MPC.settings.
// Properly distinguishes self-closing <VALUE name="..."/> from <VALUE name="...">...</VALUE>.
static bool findValueBlock(
    const std::string& xml,
    const std::string& valueName,
    size_t& outBlockStart,
    size_t& outOpenTagEnd,
    size_t& outBlockEnd,
    bool& outIsSelfClosing
) {
    size_t pos = 0;
    while ((pos = xml.find("<VALUE", pos)) != std::string::npos) {
        if (pos + 6 < xml.size()) {
            char nextC = xml[pos + 6];
            if (nextC != ' ' && nextC != '\t' && nextC != '\r' && nextC != '\n' && nextC != '/' && nextC != '>') {
                pos += 6;
                continue;
            }
        }
        bool selfClosing = false;
        size_t openEnd = findXmlTagEnd(xml, pos, selfClosing);
        if (openEnd == std::string::npos) return false;
        std::string openTag = xml.substr(pos, openEnd - pos);
        if (getXmlTagAttribute(openTag, "name") == valueName) {
            outBlockStart = pos;
            outOpenTagEnd = openEnd;
            outIsSelfClosing = selfClosing;
            if (selfClosing) {
                outBlockEnd = openEnd;
                return true;
            }
            size_t closeVal = xml.find("</VALUE>", openEnd);
            if (closeVal == std::string::npos) return false;
            outBlockEnd = closeVal + 8;
            return true;
        }
        pos = openEnd;
    }
    return false;
}

struct MpcPluginXmlEntry {
    size_t startPos = 0;
    size_t endPos   = 0;
    std::string rawTag;
    std::string name;
    std::string descName;
    std::string file;
};

// Extracts all <PLUGIN .../> (or <PLUGIN ...>...</PLUGIN>) elements within [rangeStart, rangeEnd),
// including multi-line <PLUGIN> elements wrapped by JUCE XmlElement!
static std::vector<MpcPluginXmlEntry> extractPluginXmlEntries(const std::string& xml, size_t rangeStart, size_t rangeEnd) {
    std::vector<MpcPluginXmlEntry> entries;
    size_t pos = rangeStart;
    while (pos < rangeEnd && (pos = xml.find("<PLUGIN", pos)) != std::string::npos && pos < rangeEnd) {
        if (pos + 7 < xml.size()) {
            char c = xml[pos + 7];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '/' && c != '>') {
                pos += 7;
                continue;
            }
        }
        bool selfClosing = false;
        size_t tagEnd = findXmlTagEnd(xml, pos, selfClosing);
        if (tagEnd == std::string::npos || tagEnd > rangeEnd) break;
        size_t elemEnd = tagEnd;
        if (!selfClosing) {
            size_t closeTag = xml.find("</PLUGIN>", tagEnd);
            if (closeTag != std::string::npos && closeTag + 9 <= rangeEnd) {
                elemEnd = closeTag + 9;
            }
        }
        MpcPluginXmlEntry e;
        e.startPos = pos;
        e.endPos   = elemEnd;
        e.rawTag   = xml.substr(pos, elemEnd - pos);
        e.name     = getXmlTagAttribute(e.rawTag, "name");
        e.descName = getXmlTagAttribute(e.rawTag, "descriptiveName");
        e.file     = getXmlTagAttribute(e.rawTag, "file");
        entries.push_back(e);
        pos = elemEnd;
    }
    return entries;
}

// Collapses multi-line whitespace inside an XML tag (outside quotes) into single spaces
[[maybe_unused]] static inline std::string normalizeSingleLineXmlTag(const std::string& tag) {
    std::string out;
    out.reserve(tag.size());
    bool inDoubleQuote = false;
    bool inSingleQuote = false;
    bool lastWasSpace  = false;
    for (size_t i = 0; i < tag.size(); ++i) {
        char c = tag[i];
        if (c == '"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
            out += c;
            lastWasSpace = false;
        } else if (c == '\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
            out += c;
            lastWasSpace = false;
        } else if (!inDoubleQuote && !inSingleQuote && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            if (!lastWasSpace && !out.empty() && out.back() != '<') {
                out += ' ';
                lastWasSpace = true;
            }
        } else {
            if (!inDoubleQuote && !inSingleQuote && (c == '>' || (c == '/' && i + 1 < tag.size() && tag[i + 1] == '>'))) {
                if (!out.empty() && out.back() == ' ') out.pop_back();
            }
            out += c;
            lastWasSpace = false;
        }
    }
    return out;
}

// Strict structural XML validator for MPC.settings before committing any changes to disk
static bool validateMpcSettingsXml(const std::string& xml, std::string& outErrorReason) {
    if (xml.size() < 32) {
        outErrorReason = "XML content is too short or empty";
        return false;
    }
    size_t propOpen = xml.find("<PROPERTIES");
    size_t propClose = xml.rfind("</PROPERTIES>");
    if (propOpen == std::string::npos || propClose == std::string::npos || propClose <= propOpen) {
        outErrorReason = "Missing or mismatched <PROPERTIES> root element";
        return false;
    }

    // Verify all '<' and '>' tags and quotes are balanced across the entire document
    int openValueCount = 0;
    int closeValueCount = 0;
    int openSynthLocCount = 0;
    int closeSynthLocCount = 0;
    int openKnownPluginsCount = 0;
    int closeKnownPluginsCount = 0;

    size_t i = 0;
    while (i < xml.size()) {
        size_t lt = xml.find('<', i);
        if (lt == std::string::npos) break;

        // Skip XML declaration <?...?> or comments <!--...--> or CDATA
        if (lt + 1 < xml.size() && xml[lt + 1] == '?') {
            size_t qEnd = xml.find("?>", lt + 2);
            if (qEnd == std::string::npos) {
                outErrorReason = "Unclosed XML declaration <?xml ...?>";
                return false;
            }
            i = qEnd + 2;
            continue;
        }
        if (xml.compare(lt, 4, "<!--") == 0) {
            size_t cEnd = xml.find("-->", lt + 4);
            if (cEnd == std::string::npos) {
                outErrorReason = "Unclosed XML comment";
                return false;
            }
            i = cEnd + 3;
            continue;
        }

        bool isSelfClosing = false;
        size_t gtEnd = findXmlTagEnd(xml, lt, isSelfClosing);
        if (gtEnd == std::string::npos) {
            outErrorReason = "Malformed or unclosed XML tag near offset " + std::to_string(lt);
            return false;
        }

        std::string tag = xml.substr(lt, gtEnd - lt);
        auto tagMatchesElement = [](const std::string& t, const char* elemName, bool isClosing) -> bool {
            std::string prefix = std::string(isClosing ? "</" : "<") + elemName;
            if (t.rfind(prefix, 0) != 0) return false;
            if (t.size() == prefix.size()) return true;
            char next = t[prefix.size()];
            return (next == '>' || next == '/' || next == ' ' || next == '\t' || next == '\r' || next == '\n');
        };

        if (tagMatchesElement(tag, "VALUE", true)) {
            closeValueCount++;
        } else if (tagMatchesElement(tag, "VALUE", false)) {
            if (!isSelfClosing) openValueCount++;
        } else if (tagMatchesElement(tag, "SynthContentLocations", true)) {
            closeSynthLocCount++;
        } else if (tagMatchesElement(tag, "SynthContentLocations", false)) {
            if (!isSelfClosing) openSynthLocCount++;
        } else if (tagMatchesElement(tag, "KNOWNPLUGINS", true)) {
            closeKnownPluginsCount++;
        } else if (tagMatchesElement(tag, "KNOWNPLUGINS", false)) {
            if (!isSelfClosing) openKnownPluginsCount++;
        }

        i = gtEnd;
    }

    if (openValueCount != closeValueCount) {
        outErrorReason = "Mismatched <VALUE> (" + std::to_string(openValueCount) + ") and </VALUE> (" + std::to_string(closeValueCount) + ") tags";
        return false;
    }
    if (openSynthLocCount != closeSynthLocCount) {
        outErrorReason = "Mismatched <SynthContentLocations> (" + std::to_string(openSynthLocCount) + ") and </SynthContentLocations> (" + std::to_string(closeSynthLocCount) + ") tags";
        return false;
    }
    if (openKnownPluginsCount != closeKnownPluginsCount) {
        outErrorReason = "Mismatched <KNOWNPLUGINS> (" + std::to_string(openKnownPluginsCount) + ") and </KNOWNPLUGINS> (" + std::to_string(closeKnownPluginsCount) + ") tags";
        return false;
    }
    return true;
}

static bool backupAndWriteMpcSettings(
    const std::string& mpcSettingsPath,
    const std::string& newContent,
    std::vector<std::string>* outLogs,
    std::string* outBackupPath = nullptr
) {
    std::string errReason;
    if (!validateMpcSettingsXml(newContent, errReason)) {
        std::string errMsg = "[ERROR] MPC.settings integrity check failed (" + errReason + "). Aborting write to preserve MPC.settings!";
        std::fprintf(stderr, "%s\n", errMsg.c_str());
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }

    // 1. Gold Rule: Create human-readable backup MPC.settings.bak-YYYYMMDDHHMMSS
    std::string ts = getBackupTimestampYYYYMMDDHHMMSS();
    std::string bakPath = mpcSettingsPath + ".bak-" + ts;
    {
        std::ifstream src(mpcSettingsPath, std::ios::binary);
        if (src.is_open()) {
            std::ofstream bak(bakPath, std::ios::binary | std::ios::trunc);
            if (bak.is_open()) {
                bak << src.rdbuf();
                bak.flush();
                bak.close();
                if (outBackupPath) *outBackupPath = bakPath;
            }
            src.close();
        }
    }

    // 2. Write to temporary file first, verify size, then atomically rename over MPC.settings
    std::string tmpPath = mpcSettingsPath + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            if (outLogs) outLogs->push_back("[ERROR] Could not open " + tmpPath + " for writing.");
            return false;
        }
        out.write(newContent.data(), static_cast<std::streamsize>(newContent.size()));
        out.flush();
        if (out.fail()) {
            out.close();
            unlink(tmpPath.c_str());
            if (outLogs) outLogs->push_back("[ERROR] Failed to write full XML buffer to " + tmpPath);
            return false;
        }
        out.close();
    }

    struct stat stTmp;
    if (stat(tmpPath.c_str(), &stTmp) != 0 || static_cast<size_t>(stTmp.st_size) != newContent.size()) {
        unlink(tmpPath.c_str());
        if (outLogs) outLogs->push_back("[ERROR] Temporary MPC.settings size mismatch. Aborting update.");
        return false;
    }

    if (std::rename(tmpPath.c_str(), mpcSettingsPath.c_str()) != 0) {
        unlink(tmpPath.c_str());
        if (outLogs) outLogs->push_back("[ERROR] Failed to rename " + tmpPath + " to " + mpcSettingsPath);
        return false;
    }
    sync();
    return true;
}

// Safely ensures that targetSynthDir is present inside <VALUE name="SynthContentLocations"> in an in-memory XML string.
// Returns true if the XML string was modified.
// NOTE: This function NEVER deletes or purges any pre-existing <Location> elements, preserving
// any manual edits or custom paths added by the user. It ONLY verifies and appends targetSynthDir
// if it is located on an external mounted media drive (/media/...).
static bool ensureSynthLocationInXmlString(std::string& content, const std::string& targetSynthDir) {
    if (targetSynthDir.empty() || targetSynthDir[0] != '/') return false;
    std::string cleanDir = targetSynthDir;
    while (cleanDir.size() > 1 && cleanDir.back() == '/') cleanDir.pop_back();

    // RULE: Only add to <SynthContentLocations> if the path is on an external mounted drive (/media/...).
    // Never automatically register paths from /root, /tmp, or internal root filesystem.
    // If not on /media/, do not modify anything and return false.
    if (cleanDir.rfind("/media/", 0) != 0) {
        return false;
    }

    bool modified = false;

    std::string escapedDir = xmlEscapeText(cleanDir);
    std::string expectedTag = "<Location>" + escapedDir + "</Location>";
    std::string rawExpectedTag = "<Location>" + cleanDir + "</Location>";

    size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
    bool valSelfClosing = false;
    if (findValueBlock(content, "SynthContentLocations", valStart, valOpenEnd, valEnd, valSelfClosing)) {
        if (valSelfClosing) {
            std::string replacement =
                std::string("<VALUE name=\"SynthContentLocations\">\n") +
                "    <SynthContentLocations>\n" +
                "      " + expectedTag + "\n" +
                "    </SynthContentLocations>\n" +
                "  </VALUE>";
            content.replace(valStart, valEnd - valStart, replacement);
            modified = true;
        } else {
            std::string block = content.substr(valStart, valEnd - valStart);
            if (block.find(expectedTag) == std::string::npos && block.find(rawExpectedTag) == std::string::npos) {
                size_t closeLocs = block.find("</SynthContentLocations>");
                if (closeLocs != std::string::npos) {
                    size_t absCloseLocs = valStart + closeLocs;
                    size_t lineStart = absCloseLocs;
                    while (lineStart > valStart && (content[lineStart - 1] == ' ' || content[lineStart - 1] == '\t')) {
                        lineStart--;
                    }
                    std::string indent = content.substr(lineStart, absCloseLocs - lineStart);
                    std::string inject = "  " + expectedTag + "\n" + indent;
                    content.insert(absCloseLocs, inject);
                    modified = true;
                } else {
                    // Check for self-closing <SynthContentLocations/>
                    size_t innerPos = block.find("<SynthContentLocations");
                    if (innerPos != std::string::npos) {
                        size_t absInner = valStart + innerPos;
                        bool innerSelf = false;
                        size_t innerEnd = findXmlTagEnd(content, absInner, innerSelf);
                        if (innerEnd != std::string::npos && innerSelf) {
                            std::string replacement =
                                std::string("<SynthContentLocations>\n") +
                                "      " + expectedTag + "\n" +
                                "    </SynthContentLocations>";
                            content.replace(absInner, innerEnd - absInner, replacement);
                            modified = true;
                        }
                    } else {
                        size_t closeValPos = valEnd - 8; // before </VALUE>
                        std::string inject =
                            std::string("  <SynthContentLocations>\n") +
                            "      " + expectedTag + "\n" +
                            "    </SynthContentLocations>\n  ";
                        content.insert(closeValPos, inject);
                        modified = true;
                    }
                }
            }
        }
    } else {
        size_t propEnd = content.rfind("</PROPERTIES>");
        if (propEnd != std::string::npos) {
            std::string block =
                std::string("  <VALUE name=\"SynthContentLocations\">\n") +
                "    <SynthContentLocations>\n" +
                "      " + expectedTag + "\n" +
                "    </SynthContentLocations>\n" +
                "  </VALUE>\n";
            content.insert(propEnd, block);
            modified = true;
        }
    }
    return modified;
}

void ConfigParser::ensureManagerSynthsInMpcSettings(std::vector<std::string>* outLogs) {
    std::string mgrDir = getManagerRootDir();
    while (mgrDir.size() > 1 && mgrDir.back() == '/') mgrDir.pop_back();

    // RULE: Only add the Synths directory to MPC.settings <SynthContentLocations>
    // if the Plugin Manager is running from an external mounted drive (/media/...).
    // When launched from a working/root directory like /root, /tmp, /home, etc., NEVER add it.
    if (mgrDir.rfind("/media/", 0) != 0) {
        return;
    }

    std::string defaultMgrSynths = mgrDir + "/Synths";
    mkdir(defaultMgrSynths.c_str(), 0755);

    std::string mpcSettingsPath = resolveMpcSettingsPath();
    struct stat st;
    if (stat(mpcSettingsPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return;

    std::ifstream inFile(mpcSettingsPath, std::ios::binary);
    if (!inFile.is_open()) return;
    std::stringstream ss;
    ss << inFile.rdbuf();
    std::string content = ss.str();
    inFile.close();
    if (content.empty()) return;

    if (ensureSynthLocationInXmlString(content, defaultMgrSynths)) {
        std::string bakPath;
        if (backupAndWriteMpcSettings(mpcSettingsPath, content, outLogs, &bakPath)) {
            std::string msg = "[TKGL-INIT] Added <Location>" + defaultMgrSynths + "</Location> to MPC.settings (backup: " + bakPath + ")";
            std::printf("%s\n", msg.c_str());
            if (outLogs) outLogs->push_back(msg);
        }
    }
}

void ConfigParser::refreshInstallStates(LauncherConfig& config, std::vector<std::string>* outLogs) {
    // 0. Ensure the Manager's Synths subdirectory is present in <SynthContentLocations> of MPC.settings
    ensureManagerSynthsInMpcSettings(outLogs);

    // 1. Determine MPC.settings file path
    std::string mpcSettingsPath = resolveMpcSettingsPath();

    // 2. Gather all SynthContentLocations from MPC.settings + Manager's Synths + mounted media
    std::vector<SynthLocation> synthLocs = getSynthContentLocations(mpcSettingsPath);
    std::vector<std::string> searchPaths;
    for (const auto& sl : synthLocs) {
        std::string p = sl.path;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (!p.empty()) searchPaths.push_back(p);
    }

    // Auto-discover /media/*/Synths and /media/*/Expansions
    DIR* mediaDir = opendir("/media");
    if (mediaDir) {
        struct dirent* ent;
        while ((ent = readdir(mediaDir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string drive = std::string("/media/") + ent->d_name;
            struct stat st;
            if (stat(drive.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                std::string s1 = drive + "/Synths";
                if (stat(s1.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) searchPaths.push_back(s1);
                std::string s2 = drive + "/Expansions";
                if (stat(s2.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) searchPaths.push_back(s2);
            }
        }
        closedir(mediaDir);
    }

    std::sort(searchPaths.begin(), searchPaths.end());
    searchPaths.erase(std::unique(searchPaths.begin(), searchPaths.end()), searchPaths.end());

    // 3. Read MPC.settings and extract <PLUGIN .../> entries inside <VALUE name="pluginList-arm">
    std::string mpcSettingsContent;
    std::ifstream fSett(mpcSettingsPath, std::ios::binary);
    if (fSett.is_open()) {
        std::stringstream ss;
        ss << fSett.rdbuf();
        mpcSettingsContent = ss.str();
        fSett.close();
    }

    std::vector<MpcPluginXmlEntry> regPlugins;
    if (!mpcSettingsContent.empty()) {
        size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
        bool valSelfClosing = false;
        if (findValueBlock(mpcSettingsContent, "pluginList-arm", valStart, valOpenEnd, valEnd, valSelfClosing) && !valSelfClosing) {
            regPlugins = extractPluginXmlEntries(mpcSettingsContent, valOpenEnd, valEnd);
        }
    }

    // Helper: search all SynthContentLocations for the plugin's .so file and skin directory
    auto findSoInSynthLocations = [&](const MenuItem& item, const std::string& subDir, const std::string& binName,
                                      std::string& outFoundSo, std::string& outFoundSubdir, std::string& outFoundBin) -> bool {
        outFoundSo.clear();
        outFoundSubdir = subDir;
        outFoundBin = binName;
        std::vector<std::string> candSubdirs = {
            subDir,
            item.author + " - VST - " + item.label,
            item.label,
            item.id
        };
        for (const auto& basePath : searchPaths) {
            struct stat st;
            for (const auto& sd : candSubdirs) {
                if (sd.empty()) continue;
                std::string dirPath = basePath + "/" + sd;
                if (stat(dirPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    std::string binPath = dirPath + "/" + binName;
                    if (stat(binPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                        outFoundSo = binPath;
                        outFoundSubdir = sd;
                        outFoundBin = binName;
                        return true;
                    }
                    DIR* pd = opendir(dirPath.c_str());
                    if (pd) {
                        struct dirent* de;
                        while ((de = readdir(pd)) != nullptr) {
                            size_t len = std::strlen(de->d_name);
                            if (len > 3 && std::strcmp(de->d_name + len - 3, ".so") == 0) {
                                std::string candSo = dirPath + "/" + de->d_name;
                                if (stat(candSo.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                                    outFoundSo = candSo;
                                    outFoundSubdir = sd;
                                    outFoundBin = de->d_name;
                                    closedir(pd);
                                    return true;
                                }
                            }
                        }
                        closedir(pd);
                    }
                }
            }
            std::string directSo = basePath + "/" + binName;
            if (stat(directSo.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                outFoundSo = directSo;
                return true;
            }
        }
        return false;
    };

    bool mpcSettingsModified = false;

    // 4. Match each catalog item to at most one <PLUGIN> entry in MPC.settings (2-pass priority:
    //    Pass 1 matches exact targetSubdir in file="...", Pass 2 matches exact plugin name/id)
    //    so distinct plugins sharing a binary name (e.g. "OB-Xd" by reales vs "OB-Xd MPC" by sd88me)
    //    never collide on the same <PLUGIN> entry.
    std::vector<bool> regClaimed(regPlugins.size(), false);
    std::unordered_map<std::string, int> itemMatchedRegIdx;

    // Pass 1: Exact subdirectory match in rp.file
    for (const auto& sec : config.sections) {
        for (const auto& item : sec.items) {
            std::string subDir = !item.targetSubdir.empty() ? item.targetSubdir : (item.author + " - VST - " + item.label);
            std::string altSubDir = item.author + " - VST - " + item.label;
            for (size_t r = 0; r < regPlugins.size(); ++r) {
                if (regClaimed[r]) continue;
                const auto& rp = regPlugins[r];
                bool subMatch = (!subDir.empty() && rp.file.find("/" + subDir + "/") != std::string::npos) ||
                                (!altSubDir.empty() && rp.file.find("/" + altSubDir + "/") != std::string::npos);
                if (subMatch) {
                    regClaimed[r] = true;
                    itemMatchedRegIdx[item.id + "::" + item.label] = static_cast<int>(r);
                    break;
                }
            }
        }
    }

    // Pass 2: Name / ID match for items not yet matched in Pass 1
    for (const auto& sec : config.sections) {
        for (const auto& item : sec.items) {
            std::string key = item.id + "::" + item.label;
            if (itemMatchedRegIdx.find(key) != itemMatchedRegIdx.end()) continue;
            for (size_t r = 0; r < regPlugins.size(); ++r) {
                if (regClaimed[r]) continue;
                const auto& rp = regPlugins[r];
                bool nameMatch = (!item.label.empty() && (rp.name == item.label || rp.descName == item.label));
                bool idMatch = (!item.id.empty() && (rp.name == item.id || rp.descName == item.id));
                if (nameMatch || idMatch) {
                    regClaimed[r] = true;
                    itemMatchedRegIdx[key] = static_cast<int>(r);
                    break;
                }
            }
        }
    }

    // Verify each plugin's installation state and MPC.settings .so path integrity
    for (auto& sec : config.sections) {
        for (auto& item : sec.items) {
            std::string subDir = !item.targetSubdir.empty() ? item.targetSubdir : (item.author + " - VST - " + item.label);
            std::string binName = !item.targetBinary.empty() ? item.targetBinary : (item.id + ".so");
            std::string key = item.id + "::" + item.label;

            int matchedIdx = -1;
            auto itMatch = itemMatchedRegIdx.find(key);
            if (itMatch != itemMatchedRegIdx.end()) {
                matchedIdx = itMatch->second;
            }

            if (matchedIdx >= 0) {
                // Plugin IS registered in MPC.settings <pluginList-arm><KNOWNPLUGINS>
                std::string regFilePath = regPlugins[matchedIdx].file;
                struct stat stSo;
                if (!regFilePath.empty() && stat(regFilePath.c_str(), &stSo) == 0 && S_ISREG(stSo.st_mode)) {
                    size_t lastSlash = regFilePath.find_last_of('/');
                    if (lastSlash != std::string::npos) {
                        item.targetBinary = regFilePath.substr(lastSlash + 1);
                        size_t prevSlash = regFilePath.find_last_of('/', lastSlash - 1);
                        if (prevSlash != std::string::npos) {
                            item.targetSubdir = regFilePath.substr(prevSlash + 1, lastSlash - prevSlash - 1);
                        }
                    }
                    item.installed = true;
                    item.broken = false;
                } else {
                    // Registered .so path is missing; check if the plugin's .so was relocated to another SynthContentLocations dir
                    std::string foundSo, foundSubdir, foundBin;
                    if (findSoInSynthLocations(item, subDir, binName, foundSo, foundSubdir, foundBin)) {
                        item.targetSubdir = foundSubdir;
                        item.targetBinary = foundBin;
                        item.installed = true;
                        item.broken = false;

                        if (!mpcSettingsContent.empty()) {
                            std::string oldTag = regPlugins[matchedIdx].rawTag;
                            std::string newTag = setXmlTagAttribute(oldTag, "file", foundSo);
                            size_t tagPos = mpcSettingsContent.find(oldTag);
                            if (tagPos != std::string::npos && newTag != oldTag) {
                                mpcSettingsContent.replace(tagPos, oldTag.size(), newTag);
                                regPlugins[matchedIdx].rawTag = newTag;
                                regPlugins[matchedIdx].file = foundSo;
                                mpcSettingsModified = true;
                            }
                        }
                        std::string warnMsg = "[WARNING] " + item.label + ": missing .so at '" +
                            (regFilePath.empty() ? binName : regFilePath) +
                            "', replaced MPC.settings file property with '" + foundSo + "'";
                        std::printf("%s\n", warnMsg.c_str());
                        if (outLogs) outLogs->push_back(warnMsg);
                    } else {
                        // Registered in MPC.settings, but .so does not exist anywhere on disk -> BROKEN
                        item.installed = true;
                        item.broken = true;
                        std::string brkMsg = "[WARNING] " + item.label + ": .so file '" +
                            (regFilePath.empty() ? binName : regFilePath) +
                            "' not found in MPC.settings or SynthContentLocations -> installation is BROKEN.";
                        std::printf("%s\n", brkMsg.c_str());
                        if (outLogs) outLogs->push_back(brkMsg);
                    }
                }
            } else {
                // Plugin is NOT registered in MPC.settings: check if its folder/.so exists on disk
                std::string foundSo, foundSubdir, foundBin;
                if (findSoInSynthLocations(item, subDir, binName, foundSo, foundSubdir, foundBin)) {
                    item.targetSubdir = foundSubdir;
                    item.targetBinary = foundBin;
                    item.installed = true;
                    item.broken = false;
                } else {
                    item.installed = false;
                    item.broken = false;
                }
            }
        }
    }

    // 5. Save updated MPC.settings with XML validation and YYYYMMDDHHMMSS backup if modified
    if (mpcSettingsModified && !mpcSettingsContent.empty()) {
        std::string bakPath;
        if (backupAndWriteMpcSettings(mpcSettingsPath, mpcSettingsContent, outLogs, &bakPath)) {
            if (outLogs) outLogs->push_back("[TKGL-SYNC] Updated relocated plugin file paths in " + mpcSettingsPath + " (backup: " + bakPath + ")");
        }
    }
}

std::vector<SynthLocation> ConfigParser::getSynthContentLocations(const std::string& mpcSettingsPath) {
    std::vector<SynthLocation> locs;

    std::string mgrDir = getManagerRootDir();
    std::string defaultMgrSynths = mgrDir + "/Synths";
    mkdir(defaultMgrSynths.c_str(), 0755);

    auto getFreeSpaceStr = [](const std::string& dirPath) -> std::string {
        struct statvfs sv;
        std::string checkPath = dirPath;
        if (statvfs(checkPath.c_str(), &sv) != 0) {
            checkPath = "/media";
            if (statvfs(checkPath.c_str(), &sv) != 0) {
                checkPath = "/";
                if (statvfs(checkPath.c_str(), &sv) != 0) {
                    return "58.4 GB free";
                }
            }
        }
        double freeGb = (static_cast<double>(sv.f_bavail) * sv.f_frsize) / (1024.0 * 1024.0 * 1024.0);
        char buf[32];
        if (freeGb >= 1.0) {
            std::snprintf(buf, sizeof(buf), "%.1f GB free", freeGb);
        } else {
            std::snprintf(buf, sizeof(buf), "%.0f MB free", freeGb * 1024.0);
        }
        return std::string(buf);
    };

    SynthLocation mgrLoc;
    mgrLoc.path = defaultMgrSynths;
    mgrLoc.label = "Manager Synths (" + defaultMgrSynths + ")";
    mgrLoc.freeSpace = getFreeSpaceStr(defaultMgrSynths);
    mgrLoc.isManagerDefault = true;
    locs.push_back(mgrLoc);

    // Read MPC.settings for registered SynthContentLocations using quote-safe findValueBlock
    std::ifstream f(mpcSettingsPath, std::ios::binary);
    if (f.is_open()) {
        std::stringstream ss;
        ss << f.rdbuf();
        std::string content = ss.str();
        size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
        bool valSelfClosing = false;
        if (findValueBlock(content, "SynthContentLocations", valStart, valOpenEnd, valEnd, valSelfClosing) && !valSelfClosing) {
            std::string sub = content.substr(valOpenEnd, valEnd - valOpenEnd);
            size_t tag = 0;
            while ((tag = sub.find("<Location>", tag)) != std::string::npos) {
                size_t closeTag = sub.find("</Location>", tag);
                if (closeTag == std::string::npos) break;
                std::string path = xmlUnescape(sub.substr(tag + 10, closeTag - (tag + 10)));
                tag = closeTag + 11;
                while (path.size() > 1 && path.back() == '/') path.pop_back();
                if (!path.empty() && path[0] == '/' && path != defaultMgrSynths && path != "./Synths") {
                    bool alreadyIn = false;
                    for (const auto& l : locs) { if (l.path == path) { alreadyIn = true; break; } }
                    if (!alreadyIn) {
                        SynthLocation l;
                        l.path = path;
                        l.label = path;
                        l.freeSpace = getFreeSpaceStr(path);
                        l.isManagerDefault = false;
                        locs.push_back(l);
                    }
                }
            }
        }
    }

    // Auto-discover /media/*/Synths for plugged USB drives or SATA SSDs
    DIR* mediaDir = opendir("/media");
    if (mediaDir) {
        struct dirent* ent;
        while ((ent = readdir(mediaDir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string drive = std::string("/media/") + ent->d_name;
            struct stat st;
            if (stat(drive.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                std::string s1 = drive + "/Synths";
                if (stat(s1.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    bool alreadyIn = false;
                    for (const auto& l : locs) { if (l.path == s1) { alreadyIn = true; break; } }
                    if (!alreadyIn) {
                        SynthLocation l;
                        l.path = s1;
                        l.label = s1;
                        l.freeSpace = getFreeSpaceStr(s1);
                        l.isManagerDefault = false;
                        locs.push_back(l);
                    }
                }
            }
        }
        closedir(mediaDir);
    }

    if (locs.size() <= 1) {
        SynthLocation l1; l1.path = "/media/MPCLIVE500/Synths"; l1.label = "/media/MPCLIVE500/Synths"; l1.freeSpace = getFreeSpaceStr(l1.path); locs.push_back(l1);
        SynthLocation l2; l2.path = "/media/az01-internal-sd/Expansions"; l2.label = "/media/az01-internal-sd/Expansions"; l2.freeSpace = getFreeSpaceStr(l2.path); locs.push_back(l2);
        SynthLocation l3; l3.path = "/sdcard/Expansions"; l3.label = "/sdcard/Expansions"; l3.freeSpace = getFreeSpaceStr(l3.path); locs.push_back(l3);
        SynthLocation l4; l4.path = "/usr/share/Akai/Content/Synths"; l4.label = "/usr/share/Akai/Content/Synths"; l4.freeSpace = getFreeSpaceStr(l4.path); locs.push_back(l4);
    }

    return locs;
}

// Helper: Removes a span [elemStart, elemEnd) from an XML string along with its line indentation and trailing newline
static void eraseXmlElementLineCleanly(std::string& xml, size_t elemStart, size_t elemEnd) {
    size_t lineStart = elemStart;
    while (lineStart > 0 && (xml[lineStart - 1] == ' ' || xml[lineStart - 1] == '\t')) {
        lineStart--;
    }
    size_t lineEnd = elemEnd;
    while (lineEnd < xml.size() && (xml[lineEnd] == ' ' || xml[lineEnd] == '\t')) {
        lineEnd++;
    }
    if (lineEnd < xml.size() && xml[lineEnd] == '\r') lineEnd++;
    if (lineEnd < xml.size() && xml[lineEnd] == '\n') lineEnd++;
    xml.erase(lineStart, lineEnd - lineStart);
}

int ConfigParser::executeItemInstall(
    const LauncherConfig& cfg,
    const MenuItem& item,
    const std::string& targetSynthDir,
    std::vector<std::string>& outLogs,
    std::function<void(const std::string&)> onProgress
) {
    (void)cfg;
    std::string mgrDir = getManagerRootDir();
    std::string defaultMgrSynths = mgrDir + "/Synths";
    std::string synthDir = (targetSynthDir.empty() || targetSynthDir == "./Synths") ? defaultMgrSynths : targetSynthDir;
    while (synthDir.size() > 1 && synthDir.back() == '/') synthDir.pop_back();

    std::string tsStr = getBackupTimestampYYYYMMDDHHMMSS();
    std::string mpcSettings = resolveMpcSettingsPath();
    std::string defaultSubdir = !item.targetSubdir.empty() ? item.targetSubdir : (item.author + " - VST - " + item.label);
    std::string defaultBin    = !item.targetBinary.empty() ? item.targetBinary : (item.id + ".so");

    auto emitLog = [&](const std::string& line) {
        outLogs.push_back(line);
        if (onProgress) onProgress(line);
    };

    emitLog("[TKGL-INSTALL] Step 1/4 (Gold Rule): Preparing MPC.settings backup (" + tsStr + ")...");

    // Step 2: Ensure target Synth directory exists on disk
    mkdir(synthDir.c_str(), 0755);
    emitLog("[TKGL-INSTALL] Step 2/4: Target Synth location: " + synthDir);

    // Step 3A: Download package via libcurl (IPv4)
    std::string pkgUrl = item.downloadUrl;
    if (pkgUrl.empty() && !item.cmd.empty()) {
        size_t hpos = item.cmd.find("https://");
        if (hpos != std::string::npos) {
            size_t hend = item.cmd.find_first_of(" \"'\t\r\n", hpos);
            pkgUrl = (hend != std::string::npos)
                ? item.cmd.substr(hpos, hend - hpos)
                : item.cmd.substr(hpos);
        }
    }

    std::string tmpZipPath = "/tmp/" + item.id + "_" + tsStr + ".zip";
    if (!pkgUrl.empty()) {
        std::string pingHost = cfg.ping_host.empty() ? "github.com" : cfg.ping_host;
        if (!checkInternetReachable(pingHost, &outLogs)) {
            emitLog("[ERROR] No internet connection (ping " + pingHost + " failed). Download aborted.");
            return -1;
        }

        std::string sizeLabel = item.sizeMb.empty() ? "ZIP" : item.sizeMb;
        emitLog("[TKGL-INSTALL] Step 3/4: Downloading " + item.label + " (" + sizeLabel + ") via libcurl (IPv4)...");
        emitLog("[TKGL-NET] Source: " + pkgUrl);

        bool preDownloaded = downloadFile(pkgUrl, tmpZipPath, 180, [&](const std::string& progLine) {
            if (onProgress) onProgress(progLine);
        }, "[TKGL-NET] Downloading");

        if (preDownloaded) {
            struct stat stZip;
            double mb = (stat(tmpZipPath.c_str(), &stZip) == 0) ? (static_cast<double>(stZip.st_size) / 1048576.0) : 0.0;
            char doneBuf[128];
            std::snprintf(doneBuf, sizeof(doneBuf), "[TKGL-NET] Download complete (%.2f MB).", mb);
            emitLog(doneBuf);
        } else {
            emitLog("[ERROR] libcurl IPv4 download failed for: " + pkgUrl);
            unlink(tmpZipPath.c_str());
            return -1;
        }
    } else {
        emitLog("[TKGL-NET] Warning: No download URL configured for " + item.label + " (" + item.id + ").");
    }

    // Step 3B: Extract package to $SYNTH_DIR and record the extracted directory path to /tmp/tkgl_target_dir_<ts>
    char scriptPath[] = "/tmp/tkgl_inst_XXXXXX";
    int fd = mkstemp(scriptPath);
    if (fd < 0) {
        outLogs.push_back("[ERROR] Could not create temp installer script.");
        return -1;
    }
    std::string outTargetDirFile = std::string(scriptPath) + ".dir";

    std::ostringstream ss;
    ss << "#!/bin/sh\n";
    ss << "export PLG_MAN_DIR=\"" << mgrDir << "\"\n";
    ss << "cd \"$PLG_MAN_DIR\" 2>/dev/null || true\n";
    ss << "SYNTH_DIR=\"" << synthDir << "\"\n";
    ss << "TS=\"" << tsStr << "\"\n";
    ss << "DEFAULT_SUBDIR=\"" << defaultSubdir << "\"\n";
    ss << "mkdir -p \"$SYNTH_DIR\"\n";
    ss << "echo \"[TKGL-INSTALL] Extracting portable/ plugin bundle...\"\n";

    if (!pkgUrl.empty()) {
        ss << "TMP_ZIP=\"" << tmpZipPath << "\"\n";
        ss << "TMP_EXT=\"/tmp/" << item.id << "_${TS}_pkg\"\n";
        ss << "rm -rf \"$TMP_EXT\"\n";
        ss << "mkdir -p \"$TMP_EXT\"\n";
        ss << "if [ ! -s \"$TMP_ZIP\" ]; then\n";
        ss << "  echo \"[ERROR] Downloaded archive missing or empty: $TMP_ZIP\"\n";
        ss << "  rm -rf \"$TMP_ZIP\" \"$TMP_EXT\"\n";
        ss << "  exit 1\n";
        ss << "fi\n";
        ss << "if ! unzip -q -o \"$TMP_ZIP\" -d \"$TMP_EXT\"; then\n";
        ss << "  echo \"[ERROR] Failed to unzip $TMP_ZIP\"\n";
        ss << "  rm -rf \"$TMP_ZIP\" \"$TMP_EXT\"\n";
        ss << "  exit 1\n";
        ss << "fi\n";
        ss << "SRC_DIR=\"\"\n";
        ss << "if [ -d \"$TMP_EXT/portable\" ]; then\n";
        ss << "  SRC_DIR=$(find \"$TMP_EXT/portable\" -mindepth 1 -maxdepth 1 -type d | head -n 1)\n";
        ss << "fi\n";
        ss << "if [ -z \"$SRC_DIR\" ]; then\n";
        ss << "  META_FOUND=$(find \"$TMP_EXT\" -name \"plugin-meta.xml\" | head -n 1)\n";
        ss << "  if [ -n \"$META_FOUND\" ]; then SRC_DIR=$(dirname \"$META_FOUND\"); fi\n";
        ss << "fi\n";
        ss << "if [ -z \"$SRC_DIR\" ]; then\n";
        ss << "  SO_FOUND=$(find \"$TMP_EXT\" -name \"*.so\" | head -n 1)\n";
        ss << "  if [ -n \"$SO_FOUND\" ]; then SRC_DIR=$(dirname \"$SO_FOUND\"); fi\n";
        ss << "fi\n";
        ss << "if [ -z \"$SRC_DIR\" ] || [ \"$SRC_DIR\" = \"$TMP_EXT\" ]; then\n";
        ss << "  PLUGIN_DIR_NAME=\"$DEFAULT_SUBDIR\"\n";
        ss << "  TARGET_DIR=\"$SYNTH_DIR/$PLUGIN_DIR_NAME\"\n";
        ss << "  mkdir -p \"$TARGET_DIR\"\n";
        ss << "  cp -rf \"$TMP_EXT/\"* \"$TARGET_DIR/\"\n";
        ss << "else\n";
        ss << "  PLUGIN_DIR_NAME=$(basename \"$SRC_DIR\")\n";
        ss << "  TARGET_DIR=\"$SYNTH_DIR/$PLUGIN_DIR_NAME\"\n";
        ss << "  mkdir -p \"$SYNTH_DIR\"\n";
        ss << "  cp -rf \"$SRC_DIR\" \"$SYNTH_DIR/\"\n";
        ss << "fi\n";
        ss << "rm -rf \"$TMP_ZIP\" \"$TMP_EXT\"\n";
    } else {
        ss << "PLUGIN_DIR_NAME=\"$DEFAULT_SUBDIR\"\n";
        ss << "TARGET_DIR=\"$SYNTH_DIR/$PLUGIN_DIR_NAME\"\n";
        ss << "mkdir -p \"$TARGET_DIR\"\n";
        ss << "if [ -d \"./plugins/" << item.id << "\" ]; then\n";
        ss << "  cp -rf \"./plugins/" << item.id << "/\"* \"$TARGET_DIR/\"\n";
        ss << "fi\n";
    }
    ss << "chmod -R 755 \"$TARGET_DIR\" 2>/dev/null || true\n";
    ss << "printf '%s' \"$TARGET_DIR\" > \"" << outTargetDirFile << "\"\n";
    ss << "sync\n";

    std::string sBody = ss.str();
    ssize_t written = write(fd, sBody.c_str(), sBody.size());
    (void)written;
    close(fd);
    chmod(scriptPath, 0755);

    std::string runCmd = std::string("/bin/sh ") + scriptPath + " 2>&1; rm -f " + scriptPath;
    FILE* pipe = popen(runCmd.c_str(), "r");
    if (!pipe) {
        unlink(outTargetDirFile.c_str());
        outLogs.push_back("[ERROR] Failed to run extraction script.");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;
        emitLog(line);
    }
    int extractRc = pclose(pipe);

    std::string targetDir = synthDir + "/" + defaultSubdir;
    {
        std::ifstream dirFile(outTargetDirFile, std::ios::binary);
        if (dirFile.is_open()) {
            std::stringstream dss;
            dss << dirFile.rdbuf();
            if (!dss.str().empty()) targetDir = dss.str();
            dirFile.close();
        }
        unlink(outTargetDirFile.c_str());
    }
    if (extractRc != 0) {
        return extractRc;
    }

    std::string pluginDirName = defaultSubdir;
    size_t lastSlash = targetDir.find_last_of('/');
    if (lastSlash != std::string::npos && lastSlash + 1 < targetDir.size()) {
        pluginDirName = targetDir.substr(lastSlash + 1);
    }

    // Step 4: Register in MPC.settings using our multi-line-safe C++ XML parser & validator
    emitLog("[TKGL-INSTALL] Step 4/4: Registering in MPC.settings <pluginList-arm><KNOWNPLUGINS>...");
    struct stat stSett;
    if (stat(mpcSettings.c_str(), &stSett) == 0 && S_ISREG(stSett.st_mode)) {
        std::ifstream inSett(mpcSettings, std::ios::binary);
        std::stringstream sss;
        sss << inSett.rdbuf();
        std::string settXml = sss.str();
        inSett.close();

        if (!settXml.empty()) {
            // 4A. Ensure synthDir is in <SynthContentLocations>
            ensureSynthLocationInXmlString(settXml, synthDir);

            // 4B. Build clean single-line <PLUGIN .../> tag(s) from plugin-meta.xml or .so scan
            std::vector<std::string> newPluginTags;
            std::string metaXmlPath = targetDir + "/plugin-meta.xml";
            std::ifstream metaFile(metaXmlPath, std::ios::binary);
            if (metaFile.is_open()) {
                std::stringstream mss;
                mss << metaFile.rdbuf();
                std::string metaXml = mss.str();
                metaFile.close();
                auto metaEntries = extractPluginXmlEntries(metaXml, 0, metaXml.size());
                for (const auto& me : metaEntries) {
                    std::string singleTag = normalizeSingleLineXmlTag(me.rawTag);
                    std::string rawFileAttr = getXmlTagAttribute(singleTag, "file");
                    if (!rawFileAttr.empty()) {
                        std::string resolvedFile = rawFileAttr;
                        const std::string token = "%payload-path%";
                        size_t tpos = resolvedFile.find(token);
                        if (tpos != std::string::npos) {
                            std::string cand1 = resolvedFile;
                            cand1.replace(tpos, token.size(), synthDir);
                            std::string cand2 = resolvedFile;
                            cand2.replace(tpos, token.size(), targetDir);
                            struct stat stSo;
                            if (stat(cand1.c_str(), &stSo) == 0) {
                                resolvedFile = cand1;
                            } else if (stat(cand2.c_str(), &stSo) == 0) {
                                resolvedFile = cand2;
                            } else {
                                resolvedFile = (rawFileAttr.find(token + "/" + pluginDirName) != std::string::npos) ? cand1 : cand2;
                            }
                        }
                        singleTag = setXmlTagAttribute(singleTag, "file", resolvedFile);
                    }
                    newPluginTags.push_back(singleTag);
                }
            }

            if (newPluginTags.empty()) {
                // Fallback: scan targetDir for *.so
                std::string foundSo;
                std::string prefSo = targetDir + "/" + defaultBin;
                struct stat stSo;
                if (stat(prefSo.c_str(), &stSo) == 0 && S_ISREG(stSo.st_mode)) {
                    foundSo = prefSo;
                } else {
                    DIR* d = opendir(targetDir.c_str());
                    if (d) {
                        struct dirent* de;
                        while ((de = readdir(d)) != nullptr) {
                            size_t len = std::strlen(de->d_name);
                            if (len > 3 && std::strcmp(de->d_name + len - 3, ".so") == 0) {
                                foundSo = targetDir + "/" + de->d_name;
                                break;
                            }
                        }
                        closedir(d);
                    }
                }
                if (!foundSo.empty()) {
                    std::string isInst = (item.shortType == "FX") ? "0" : "1";
                    std::string nIn    = (item.shortType == "FX") ? "2" : "0";
                    std::string genTag =
                        "<PLUGIN name=\"" + xmlEscapeAttr(item.label) +
                        "\" descriptiveName=\"" + xmlEscapeAttr(item.label) +
                        "\" format=\"VST\" category=\"" + xmlEscapeAttr(item.category) +
                        "\" manufacturer=\"" + xmlEscapeAttr(item.author) +
                        "\" version=\"" + xmlEscapeAttr(item.version) +
                        "\" file=\"" + xmlEscapeAttr(foundSo) +
                        "\" isInstrument=\"" + isInst +
                        "\" fileTime=\"0\" infoUpdateTime=\"0\" numInputs=\"" + nIn +
                        "\" numOutputs=\"2\" isShell=\"0\"/>";
                    newPluginTags.push_back(genTag);
                }
            }

            if (!newPluginTags.empty()) {
                // 4C. Ensure <VALUE name="pluginList-arm"> exists and is not self-closing
                size_t armStart = 0, armOpenEnd = 0, armEnd = 0;
                bool armSelfClosing = false;
                if (!findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing)) {
                    size_t propEnd = settXml.rfind("</PROPERTIES>");
                    if (propEnd != std::string::npos) {
                        std::string armBlock =
                            "  <VALUE name=\"pluginList-arm\">\n"
                            "    <KNOWNPLUGINS>\n"
                            "    </KNOWNPLUGINS>\n"
                            "  </VALUE>\n";
                        settXml.insert(propEnd, armBlock);
                    }
                } else if (armSelfClosing) {
                    std::string armBlock =
                        "<VALUE name=\"pluginList-arm\">\n"
                        "    <KNOWNPLUGINS>\n"
                        "    </KNOWNPLUGINS>\n"
                        "  </VALUE>";
                    settXml.replace(armStart, armEnd - armStart, armBlock);
                }

                // 4D. Re-locate pluginList-arm block and purge any previous/duplicate/broken <PLUGIN .../> entries
                if (findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing) && !armSelfClosing) {
                    auto existing = extractPluginXmlEntries(settXml, armOpenEnd, armEnd);
                    for (int idx = static_cast<int>(existing.size()) - 1; idx >= 0; --idx) {
                        const auto& ep = existing[idx];
                        bool matchesThisPlugin = false;
                        if (!item.label.empty() && (ep.name == item.label || ep.descName == item.label)) matchesThisPlugin = true;
                        if (!item.id.empty() && (ep.name == item.id || ep.descName == item.id)) matchesThisPlugin = true;
                        if (!pluginDirName.empty() && ep.file.find("/" + pluginDirName + "/") != std::string::npos) matchesThisPlugin = true;
                        if (!defaultSubdir.empty() && ep.file.find("/" + defaultSubdir + "/") != std::string::npos) matchesThisPlugin = true;
                        if (!defaultBin.empty() && ep.file.find("/" + defaultBin) != std::string::npos) matchesThisPlugin = true;
                        for (const auto& nt : newPluginTags) {
                            std::string ntName = getXmlTagAttribute(nt, "name");
                            std::string ntFile = getXmlTagAttribute(nt, "file");
                            if (!ntName.empty() && ep.name == ntName) matchesThisPlugin = true;
                            if (!ntFile.empty() && ep.file == ntFile) matchesThisPlugin = true;
                        }

                        bool isBrokenEntry = false;
                        if (item.broken && !ep.file.empty()) {
                            struct stat stCheck;
                            if (stat(ep.file.c_str(), &stCheck) != 0) {
                                isBrokenEntry = true;
                            }
                        }

                        if (matchesThisPlugin || isBrokenEntry) {
                            eraseXmlElementLineCleanly(settXml, ep.startPos, ep.endPos);
                        }
                    }
                }

                // 4E. Insert the new clean <PLUGIN .../> entry/entries into <KNOWNPLUGINS>
                if (findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing) && !armSelfClosing) {
                    std::string block = settXml.substr(armStart, armEnd - armStart);
                    std::string allNewLines;
                    for (const auto& nt : newPluginTags) {
                        allNewLines += "      " + nt + "\n";
                    }

                    size_t closeKp = block.find("</KNOWNPLUGINS>");
                    if (closeKp != std::string::npos) {
                        size_t absCloseKp = armStart + closeKp;
                        size_t lineStart = absCloseKp;
                        while (lineStart > armStart && (settXml[lineStart - 1] == ' ' || settXml[lineStart - 1] == '\t')) {
                            lineStart--;
                        }
                        settXml.insert(lineStart, allNewLines);
                    } else {
                        size_t kpPos = block.find("<KNOWNPLUGINS");
                        if (kpPos != std::string::npos) {
                            size_t absKp = armStart + kpPos;
                            bool kpSelf = false;
                            size_t kpEnd = findXmlTagEnd(settXml, absKp, kpSelf);
                            if (kpEnd != std::string::npos && kpSelf) {
                                std::string replacement = "<KNOWNPLUGINS>\n" + allNewLines + "    </KNOWNPLUGINS>";
                                settXml.replace(absKp, kpEnd - absKp, replacement);
                            }
                        } else {
                            size_t closeValPos = armEnd - 8;
                            std::string inject = "  <KNOWNPLUGINS>\n" + allNewLines + "    </KNOWNPLUGINS>\n  ";
                            settXml.insert(closeValPos, inject);
                        }
                    }
                }

                // 4F. Validate XML integrity, create MPC.settings.bak-YYYYMMDDHHMMSS backup, and write atomically
                std::string bakPath;
                if (backupAndWriteMpcSettings(mpcSettings, settXml, &outLogs, &bakPath)) {
                    emitLog("[TKGL-INSTALL] Backup saved: " + bakPath);
                    emitLog("[TKGL-INSTALL] Clean plugin entry registered in MPC.settings KNOWNPLUGINS.");
                } else {
                    return -1;
                }
            }
        }
    }

    emitLog("[TKGL-INSTALL] SUCCESS: " + pluginDirName + " " + (item.broken ? "repaired" : "installed") + " in " + synthDir);
    return 0;
}

int ConfigParser::executeItemUninstall(
    const LauncherConfig& cfg,
    const MenuItem& item,
    std::vector<std::string>& outLogs,
    std::function<void(const std::string&)> onProgress
) {
    (void)cfg;
    std::string mgrDir = getManagerRootDir();
    std::string mpcSettings = resolveMpcSettingsPath();
    std::string defaultSubdir = item.targetSubdir.empty()
        ? (item.author + " - VST - " + item.label)
        : item.targetSubdir;
    std::string defaultBin = item.targetBinary.empty()
        ? (item.id + ".so")
        : item.targetBinary;

    auto emitLog = [&](const std::string& line) {
        outLogs.push_back(line);
        if (onProgress) onProgress(line);
    };

    emitLog("[TKGL-REMOVE] Step 1/3 (Gold Rule): Backing up MPC.settings & unregistering plugin...");

    // Step 1 & 2: Safely remove <PLUGIN .../> (including multi-line entries) from MPC.settings in C++
    struct stat stSett;
    if (stat(mpcSettings.c_str(), &stSett) == 0 && S_ISREG(stSett.st_mode)) {
        std::ifstream inSett(mpcSettings, std::ios::binary);
        std::stringstream sss;
        sss << inSett.rdbuf();
        std::string settXml = sss.str();
        inSett.close();

        if (!settXml.empty()) {
            size_t armStart = 0, armOpenEnd = 0, armEnd = 0;
            bool armSelfClosing = false;
            bool removedAny = false;
            if (findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing) && !armSelfClosing) {
                auto existing = extractPluginXmlEntries(settXml, armOpenEnd, armEnd);
                for (int idx = static_cast<int>(existing.size()) - 1; idx >= 0; --idx) {
                    const auto& ep = existing[idx];
                    bool matches = false;
                    if (!item.label.empty() && (ep.name == item.label || ep.descName == item.label)) matches = true;
                    if (!item.id.empty() && (ep.name == item.id || ep.descName == item.id)) matches = true;
                    if (!defaultSubdir.empty() && ep.file.find("/" + defaultSubdir + "/") != std::string::npos) matches = true;
                    if (!defaultBin.empty() && ep.file.find("/" + defaultBin) != std::string::npos) matches = true;
                    if (matches) {
                        eraseXmlElementLineCleanly(settXml, ep.startPos, ep.endPos);
                        removedAny = true;
                    }
                }
            }
            if (removedAny) {
                std::string bakPath;
                if (backupAndWriteMpcSettings(mpcSettings, settXml, &outLogs, &bakPath)) {
                    emitLog("[TKGL-REMOVE] Backup saved: " + bakPath);
                    emitLog("[TKGL-REMOVE] Unregistered " + item.label + " from MPC.settings.");
                }
            }
        }
    }

    // Step 3: Remove plugin directories across all SynthContentLocations
    emitLog("[TKGL-REMOVE] Step 3/3: Removing plugin files from SynthContentLocations...");
    char scriptPath[] = "/tmp/tkgl_uninst_XXXXXX";
    int fd = mkstemp(scriptPath);
    if (fd < 0) {
        outLogs.push_back("[ERROR] Failed to create temporary uninstall script in /tmp.");
        return -1;
    }

    std::ostringstream ss;
    ss << "#!/bin/sh\n";
    ss << "DEFAULT_SUBDIR=\"" << defaultSubdir << "\"\n";
    auto locs = getSynthContentLocations(mpcSettings);
    for (const auto& loc : locs) {
        if (loc.path.empty()) continue;
        ss << "if [ -d \"" << loc.path << "/$DEFAULT_SUBDIR\" ]; then\n";
        ss << "  rm -rf \"" << loc.path << "/$DEFAULT_SUBDIR\"\n";
        ss << "  echo \"[TKGL-REMOVE] Deleted: " << loc.path << "/$DEFAULT_SUBDIR\"\n";
        ss << "fi\n";
    }
    ss << "if [ -d \"" << mgrDir << "/Synths/$DEFAULT_SUBDIR\" ]; then\n";
    ss << "  rm -rf \"" << mgrDir << "/Synths/$DEFAULT_SUBDIR\"\n";
    ss << "  echo \"[TKGL-REMOVE] Deleted: " << mgrDir << "/Synths/$DEFAULT_SUBDIR\"\n";
    ss << "fi\n";
    ss << "sync\n";
    ss << "echo \"[TKGL-REMOVE] SUCCESS: " << item.label << " cleanly uninstalled.\"\n";

    std::string sBody = ss.str();
    ssize_t written = write(fd, sBody.c_str(), sBody.size());
    (void)written;
    close(fd);
    chmod(scriptPath, 0755);

    std::string runCmd = std::string("/bin/sh ") + scriptPath + " 2>&1; rm -f " + scriptPath;
    FILE* pipe = popen(runCmd.c_str(), "r");
    if (!pipe) {
        outLogs.push_back("[ERROR] Failed to run uninstaller script.");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;
        emitLog(line);
    }
    return pclose(pipe);
}

int ConfigParser::executeItemCommand(
    const LauncherConfig& cfg,
    const MenuItem& item,
    std::vector<std::string>& outLogs
) {
    (void)cfg;
    if (item.cmd.empty()) {
        outLogs.push_back("[ERROR] Empty command.");
        return -1;
    }

    std::string mgrDir = getManagerRootDir();
    std::string finalCmd;
    if (item.is_inline) {
        char tmpPath[] = "/tmp/tkgl_exec_XXXXXX";
        int fd = mkstemp(tmpPath);
        if (fd >= 0) {
            std::string defSynthDir = mgrDir + "/Synths";
            std::string scriptBody = "#!/bin/sh\nexport PLG_MAN_DIR=\"" + mgrDir + "\"\ncd \"$PLG_MAN_DIR\" 2>/dev/null || true\nexport SYNTH_INSTALL_DIR=\"${SYNTH_INSTALL_DIR:-" + defSynthDir + "}\"\n" + item.cmd + "\n";
            ssize_t written = write(fd, scriptBody.c_str(), scriptBody.size());
            (void)written;
            close(fd);
            chmod(tmpPath, 0755);
            finalCmd = std::string("/bin/sh ") + tmpPath + " 2>&1; rm -f " + tmpPath;
        } else {
            std::string defSynthDir = mgrDir + "/Synths";
            finalCmd = "PLG_MAN_DIR=\"" + mgrDir + "\" SYNTH_INSTALL_DIR=\"" + defSynthDir + "\" /bin/sh -c '" + item.cmd + "' 2>&1";
        }
    } else {
        std::string defSynthDir = mgrDir + "/Synths";
        finalCmd = "PLG_MAN_DIR=\"" + mgrDir + "\" SYNTH_INSTALL_DIR=\"" + defSynthDir + "\" /bin/sh " + item.cmd + " 2>&1";
    }

    FILE* pipe = popen(finalCmd.c_str(), "r");
    if (!pipe) {
        outLogs.push_back("[ERROR] Failed to spawn /bin/sh.");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = 0;
        outLogs.emplace_back(line);
    }
    return pclose(pipe);
}

} // namespace tkgl::config

/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: include/tkgl_ui_toolkit.hpp
 * Description: High-Definition Poloq & KikGen Hardware UI Toolkit
 *              1:1 Visual Match with the Web Hardware Simulator (image_22ce5693.png)
 * ============================================================================
 */





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

/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: src/main.cpp
 * Description: Standalone DRM/Framebuffer application entry point for
 *              Akai MPC Live / Live II / One / X / Force (Linux ARMv7hf).
 * ============================================================================
 */





#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <alsa/asoundlib.h>
#include <libgen.h>

static std::atomic<bool> g_running{true};
static std::mutex        g_ui_mutex;

static void sig_handler(int) {
    g_running = false;
}

static bool find_mpc_private_midi(char* out_device, size_t max_len) {
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        snd_ctl_t* ctl = nullptr;
        char ctl_name[32];
        std::snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);
        if (snd_ctl_open(&ctl, ctl_name, 0) < 0) continue;

        int dev = -1;
        while (snd_ctl_rawmidi_next_device(ctl, &dev) >= 0 && dev >= 0) {
            snd_rawmidi_info_t* info;
            snd_rawmidi_info_alloca(&info);
            snd_rawmidi_info_set_device(info, dev);
            snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
            snd_ctl_rawmidi_info(ctl, info);

            int subs = snd_rawmidi_info_get_subdevices_count(info);
            for (int sub = 0; sub < subs; ++sub) {
                snd_rawmidi_info_set_subdevice(info, sub);
                if (snd_ctl_rawmidi_info(ctl, info) == 0) {
                    const char* sub_name = snd_rawmidi_info_get_subdevice_name(info);
                    if (sub_name && std::strstr(sub_name, "Private") != nullptr) {
                        std::snprintf(out_device, max_len, "hw:%d,%d,%d", card, dev, sub);
                        snd_ctl_close(ctl);
                        return true;
                    }
                }
            }
        }
        snd_ctl_close(ctl);
    }
    return false;
}

static void midi_thread_func(tkgl::ui::PluginManagerView* ui) {
    char midi_dev[64] = {0};
    if (!find_mpc_private_midi(midi_dev, sizeof(midi_dev))) {
        std::snprintf(midi_dev, sizeof(midi_dev), "hw:0,0,1");
    }

    snd_rawmidi_t* midi_in = nullptr;
    if (snd_rawmidi_open(&midi_in, nullptr, midi_dev, SND_RAWMIDI_NONBLOCK) < 0) {
        std::snprintf(midi_dev, sizeof(midi_dev), "hw:1,0,1");
        if (snd_rawmidi_open(&midi_in, nullptr, midi_dev, SND_RAWMIDI_NONBLOCK) < 0) {
            std::fprintf(stderr, "[TKGL-MIDI] Could not open ALSA Private MIDI port.\n");
            return;
        }
    }
    std::printf("[TKGL-MIDI] Listening on ALSA Private port: %s\n", midi_dev);

    uint8_t status = 0;
    uint8_t data[2] = {0, 0};
    int data_idx = 0;
    uint8_t raw_buf[32];

    while (g_running) {
        ssize_t n = snd_rawmidi_read(midi_in, raw_buf, sizeof(raw_buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                uint8_t b = raw_buf[i];
                if (b >= 0xF8) continue;
                if (b & 0x80) {
                    status = b & 0xF0;
                    data_idx = 0;
                } else if (status != 0) {
                    data[data_idx++] = b;
                    int expected = (status == 0xC0 || status == 0xD0) ? 1 : 2;
                    if (data_idx >= expected) {
                        uint8_t d1 = data[0];
                        uint8_t d2 = (expected == 2) ? data[1] : 0;
                        data_idx = 0;

                        std::lock_guard<std::mutex> lock(g_ui_mutex);
                        // Wake from screensaver on ANY MIDI message (pads, Q-Links, buttons, wheel)
                        if (ui->isScreensaverActive()) {
                            ui->onAnyInput();
                            continue;
                        }
                        ui->onAnyInput();
                        if (status == 0xB0 && d1 == 0x64) {
                            // Data Wheel rotation: B0 64 01 (CW) / B0 64 7F (CCW)
                            int delta = (d2 < 64) ? static_cast<int>(d2) : (static_cast<int>(d2) - 128);
                            ui->onWheelDelta(delta);
                        } else if (status == 0x90 && d2 > 0) {
                            // MPC Internal MIDI Controller Note Mapping (TheKikGen MPC-LiveXplore):
                            // - 0x6F (90 6F 7F): Data Wheel Push -> Enter / Install
                            // - 0x37 (90 37 7F): Minus (-) Button -> Back (same as < BACK touch button)
                            // - 0x36 (90 36 7F): Plus (+) Button  -> Next / Enter
                            // - 0x7B (90 7B 7F): MENU Button      -> Back to Menu
                            // - 0x34 (90 34 7F): MAIN Button      -> Back to Menu
                            if (d1 == 0x37 || d1 == 0x7B || d1 == 0x34) {
                                ui->onMenuButton();
                            } else if (d1 == 0x6F || d1 == 0x36) {
                                ui->onWheelPress();
                            }
                            if (ui->shouldExit()) {
                                g_running = false;
                            }
                        }
                    }
                }
            }
        } else {
            usleep(4000);
        }
    }
    snd_rawmidi_close(midi_in);
}

static void touch_thread_func(tkgl::gfx::DrmFramebufferCanvas* canvas, tkgl::ui::PluginManagerView* ui) {
    int touch_fd = -1;
    int max_x = 0, max_y = 0;
    std::string opened_dev;

    DIR* dir = opendir("/dev/input");
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strncmp(ent->d_name, "event", 5) == 0) {
            std::string devPath = std::string("/dev/input/") + ent->d_name;
            int fd = open(devPath.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                unsigned long evbit = 0;
                ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit);
                if (evbit & (1 << EV_ABS)) {
                    struct input_absinfo abs_x, abs_y;
                    std::memset(&abs_x, 0, sizeof(abs_x));
                    std::memset(&abs_y, 0, sizeof(abs_y));
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0 && abs_x.maximum > 0) {
                        max_x = abs_x.maximum;
                    } else if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0) {
                        max_x = abs_x.maximum;
                    }
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0 && abs_y.maximum > 0) {
                        max_y = abs_y.maximum;
                    } else if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0) {
                        max_y = abs_y.maximum;
                    }
                    if (max_x > 0 && max_y > 0) {
                        touch_fd = fd;
                        opened_dev = devPath;
                        break;
                    }
                }
                close(fd);
            }
        }
    }
    closedir(dir);

    if (touch_fd < 0) return;
    std::printf("[TKGL-TOUCH] Opened %s (max_x=%d, max_y=%d)\n", opened_dev.c_str(), max_x, max_y);

    int cur_x = 0, cur_y = 0;
    bool down = false;
    struct input_event ev;

    while (g_running) {
        ssize_t r = read(touch_fd, &ev, sizeof(ev));
        if (r == sizeof(ev)) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) cur_x = ev.value;
                else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) cur_y = ev.value;
                else if (ev.code == ABS_MT_TRACKING_ID) {
                    if (ev.value >= 0 && !down) {
                        down = true;
                    } else if (ev.value == -1 && down) {
                        down = false;
                        int lx = 0, ly = 0;
                        canvas->mapTouchToLogical(cur_x, cur_y, max_x, max_y, lx, ly);
                        std::lock_guard<std::mutex> lock(g_ui_mutex);
                        ui->onTouchDown(lx, ly);
                        if (ui->shouldExit()) g_running = false;
                    }
                }
            } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                if (ev.value == 1 && !down) {
                    down = true;
                } else if (ev.value == 0 && down) {
                    down = false;
                    int lx = 0, ly = 0;
                    canvas->mapTouchToLogical(cur_x, cur_y, max_x, max_y, lx, ly);
                    std::lock_guard<std::mutex> lock(g_ui_mutex);
                    ui->onTouchDown(lx, ly);
                    if (ui->shouldExit()) g_running = false;
                }
            }
        } else {
            usleep(4000);
        }
    }
    close(touch_fd);
}

int main(int argc, char* argv[]) {
    // 1. Startup Prompt & Version identification per specification
    std::printf("The KikGen Labs - Plugins Manager for Akai MPC and Force Devices\n");
    std::printf("Version: %s - Compiled on %s %s\n", tkgl::config::TKGL_MANAGER_VERSION, __DATE__, __TIME__);

    if (argc >= 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0)) {
        return 0;
    }

    // Headless helper mode: mpc_plugin_manager_fb --fetch <url> <dest> (uses libcurl IPv4)
    if (argc >= 4 && std::strcmp(argv[1], "--fetch") == 0) {
        return tkgl::config::ConfigParser::downloadFile(argv[2], argv[3], 60) ? 0 : 1;
    }

    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    // Resolve root working directory:
    // Priority 1: PLG_MAN_DIR environment variable (when binary is executed from /tmp due to USB vfat noexec)
    // Priority 2: Executable directory resolved from /proc/self/exe (unless /tmp without config)
    std::string rootDir = tkgl::config::ConfigParser::getManagerRootDir();
    if (!rootDir.empty()) {
        if (chdir(rootDir.c_str()) == 0) {
            std::printf("[TKGL-MAIN] Working directory set to: %s\n", rootDir.c_str());
        } else {
            std::fprintf(stderr, "[TKGL-MAIN] Warning: Could not chdir to %s\n", rootDir.c_str());
        }
    }

    std::string cfgPath = (argc > 1) ? argv[1] : "mpc_plugins.conf";
    const char* fbDev   = (argc > 2) ? argv[2] : "/dev/fb0";

    tkgl::config::LauncherConfig config;
    tkgl::config::ConfigParser::loadFromFile(cfgPath, config);

    tkgl::gfx::DrmFramebufferCanvas canvas;
    if (!canvas.init(fbDev, config.screen_rotation)) {
        std::fprintf(stderr, "[TKGL-MAIN] Warning: /dev/fb0 unavailable, running in offscreen mode.\n");
    }

    tkgl::ui::PluginManagerView ui(canvas, config);
    {
        std::lock_guard<std::mutex> lock(g_ui_mutex);
        ui.render();
    }

    std::thread midiThread(midi_thread_func, &ui);
    std::thread touchThread(touch_thread_func, &canvas, &ui);

    int idleTickCounter = 0;
    while (g_running) {
        usleep(50000);
        {
            std::lock_guard<std::mutex> lock(g_ui_mutex);
            if (ui.shouldExit()) {
                g_running = false;
                break;
            }
            if (++idleTickCounter >= 4) { // Every 200ms
                idleTickCounter = 0;
                ui.checkIdleTick();
            }
        }
    }

    if (midiThread.joinable()) midiThread.join();
    if (touchThread.joinable()) touchThread.join();
    canvas.clearScreenBlack();
    canvas.shutdown();
    return 0;
}

