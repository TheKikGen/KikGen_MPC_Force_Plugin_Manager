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
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // 96 '`'
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
    // Skip 2-byte zlib header
    BitStream bs{compressed.data() + 2, compressed.size() - 6, 0, 0};

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

    if (width <= 0 || height <= 0 || bitDepth != 8 || interlace != 0 || idat.empty()) return false;
    int bpp = (colorType == 6) ? 4 : (colorType == 2 ? 3 : (colorType == 4 ? 2 : (colorType == 0 || colorType == 3 ? 1 : 0)));
    if (bpp == 0) return false;

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (width * bpp + 1));
    if (!inflateZlib(idat, raw)) return false;

    size_t stride = static_cast<size_t>(width) * bpp;
    if (raw.size() < static_cast<size_t>(height) * (stride + 1)) return false;

    std::vector<uint8_t> recon(static_cast<size_t>(height) * stride, 0);
    for (int y = 0; y < height; ++y) {
        uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        uint8_t* dst = &recon[y * stride];
        const uint8_t* prev = (y > 0) ? &recon[(y - 1) * stride] : nullptr;

        for (size_t i = 0; i < stride; ++i) {
            uint8_t a = (i >= static_cast<size_t>(bpp)) ? dst[i - bpp] : 0;
            uint8_t b = prev ? prev[i] : 0;
            uint8_t c = (prev && i >= static_cast<size_t>(bpp)) ? prev[i - bpp] : 0;
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

    for (int i = 0; i < width * height; ++i) {
        uint8_t r = 18, g = 18, b = 22;
        if (colorType == 6) {
            uint8_t a = recon[i * 4 + 3];
            r = static_cast<uint8_t>((recon[i * 4]     * a + 18 * (255 - a)) / 255);
            g = static_cast<uint8_t>((recon[i * 4 + 1] * a + 18 * (255 - a)) / 255);
            b = static_cast<uint8_t>((recon[i * 4 + 2] * a + 22 * (255 - a)) / 255);
        } else if (colorType == 2) {
            r = recon[i * 3];
            g = recon[i * 3 + 1];
            b = recon[i * 3 + 2];
        } else if (colorType == 3) {
            int pidx = recon[i] * 3;
            if (pidx + 2 < static_cast<int>(palette.size())) {
                r = palette[pidx];
                g = palette[pidx + 1];
                b = palette[pidx + 2];
            }
        } else if (colorType == 0 || colorType == 4) {
            r = g = b = recon[i * bpp];
        }
        outImg.rgb[i * 3]     = r;
        outImg.rgb[i * 3 + 1] = g;
        outImg.rgb[i * 3 + 2] = b;
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
        DecodedImage decoded;
        // 1. Try local path (e.g. plugins/<id>/thumb.ppm or thumb.png)
        if (!loadImageFile(path, decoded)) {
            if (!pluginId.empty()) {
                std::string pngLocal = "plugins/" + pluginId + "/thumb.png";
                struct stat stPng;
                if (stat(pngLocal.c_str(), &stPng) == 0 && stPng.st_size == 0) {
                    unlink(pngLocal.c_str());
                }
                if (!loadImageFile(pngLocal, decoded)) {
                    std::string ppmLocal = "plugins/" + pluginId + "/thumb.ppm";
                    loadImageFile(ppmLocal, decoded);
                }
            }
        }
        // 2. If not yet on disk, kick off a non-blocking background fetch into plugins/<id>/thumb.png
        //    using "/proc/self/exe --fetch" (which dynamically loads libcurl.so.4 / libcurl.so.4.8.0).
        if (!decoded.valid && !pluginId.empty() && !fallbackUrl.empty()) {
            static std::unordered_map<std::string, bool> s_bgThumbRequested;
            if (!s_bgThumbRequested[pluginId]) {
                s_bgThumbRequested[pluginId] = true;
                std::string plugDir = "plugins/" + pluginId;
                mkdir("plugins", 0755);
                mkdir(plugDir.c_str(), 0755);
                std::string localPng = plugDir + "/thumb.png";
                std::string tmpPng = "/tmp/tkgl_thumb_" + pluginId + ".png";
                std::string bgCmd = "(rm -f \"" + tmpPng + "\"; "
                    "/proc/self/exe --fetch \"" + fallbackUrl + "\" \"" + tmpPng + "\" 2>/dev/null; "
                    "if [ ! -s \"" + tmpPng + "\" ] && command -v wget >/dev/null 2>&1; then wget --no-check-certificate -q -O \"" + tmpPng + "\" \"" + fallbackUrl + "\" 2>/dev/null || wget -q -O \"" + tmpPng + "\" \"" + fallbackUrl + "\" 2>/dev/null; fi; "
                    "if [ -s \"" + tmpPng + "\" ]; then mv -f \"" + tmpPng + "\" \"" + localPng + "\"; else rm -f \"" + tmpPng + "\"; fi) >/dev/null 2>&1 &";
                int rc = std::system(bgCmd.c_str());
                (void)rc;
            }
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

struct MenuItem {
    std::string id;
    std::string label;
    std::string author        = "The KikGen Labs";
    std::string version       = "v1.0.0";
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
    std::string usb_mount_path  = "/media/AKAI_FORCE_SSD";
    std::string synth_dir_name  = "/media/AKAI_FORCE_SSD/Synth";
    std::string config_path     = "mpc_plugins.conf";
    std::vector<Section> sections;

    void loadEmbeddedDefaults();
};

class ConfigParser {
public:
    static std::string getManagerRootDir();
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
    static void refreshInstallStates(LauncherConfig& config, std::vector<std::string>* outLogs = nullptr);
};

using PluginCatalogConfig = LauncherConfig;

} // namespace tkgl::config

/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: src/tkgl_config.cpp
 * Description: Block-delimited parser for mpc_plugins.conf AND live online
 *              catalog scraper/parser for catalog.json + native libcurl.so.4
 *              dynamic loader (dlopen/dlsym for libcurl.so.4 / libcurl.so.4.8.0).
 * ============================================================================
 */


#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <ctime>
#include <chrono>
#include <dirent.h>
#include <libgen.h>
#include <dlfcn.h>
#include <unordered_map>
#include <algorithm>

namespace tkgl::config {

// ============================================================================
// Dynamic libcurl.so.4 / libcurl.so.4.8.0 Loader (Zero build-time curl headers)
// ============================================================================
namespace {

struct LibCurlRuntime {
    void* handle = nullptr;
    bool  initialized = false;
    bool  available = false;

    int   (*global_init)(long) = nullptr;
    void* (*easy_init)(void) = nullptr;
    int   (*easy_setopt)(void*, int, ...) = nullptr;
    int   (*easy_perform)(void*) = nullptr;
    int   (*easy_getinfo)(void*, int, ...) = nullptr;
    void  (*easy_cleanup)(void*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;

    static LibCurlRuntime& get() {
        static LibCurlRuntime inst;
        if (!inst.initialized) {
            inst.initialized = true;
            const char* soCandidates[] = {
                "libcurl.so.4",
                "libcurl.so.4.8.0",
                "/usr/lib/libcurl.so.4",
                "/usr/lib/libcurl.so.4.8.0",
                "/lib/libcurl.so.4",
                "/lib/libcurl.so.4.8.0",
                "/usr/lib/arm-linux-gnueabihf/libcurl.so.4",
                "libcurl.so"
            };
            for (const char* name : soCandidates) {
                inst.handle = dlopen(name, RTLD_LAZY);
                if (inst.handle) break;
            }
            if (inst.handle) {
                inst.global_init   = reinterpret_cast<int(*)(long)>(dlsym(inst.handle, "curl_global_init"));
                inst.easy_init     = reinterpret_cast<void*(*)(void)>(dlsym(inst.handle, "curl_easy_init"));
                inst.easy_setopt   = reinterpret_cast<int(*)(void*, int, ...)>(dlsym(inst.handle, "curl_easy_setopt"));
                inst.easy_perform  = reinterpret_cast<int(*)(void*)>(dlsym(inst.handle, "curl_easy_perform"));
                inst.easy_getinfo  = reinterpret_cast<int(*)(void*, int, ...)>(dlsym(inst.handle, "curl_easy_getinfo"));
                inst.easy_cleanup  = reinterpret_cast<void(*)(void*)>(dlsym(inst.handle, "curl_easy_cleanup"));
                inst.easy_strerror = reinterpret_cast<const char*(*)(int)>(dlsym(inst.handle, "curl_easy_strerror"));
                if (inst.easy_init && inst.easy_setopt && inst.easy_perform && inst.easy_cleanup) {
                    if (inst.global_init) inst.global_init(3L); // CURL_GLOBAL_DEFAULT
                    inst.available = true;
                }
            }
        }
        return inst;
    }
};

struct CurlStreamCtx {
    FILE* fp = nullptr;
    size_t bytesReceived = 0;
    std::chrono::steady_clock::time_point lastTick;
    int dotCount = 0;
    std::function<void(const std::string&)> onProgress;
    std::string prefix;
};

static size_t tkgl_libcurl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    CurlStreamCtx* ctx = static_cast<CurlStreamCtx*>(userdata);
    if (!ctx || !ctx->fp) return 0;
    size_t written = std::fwrite(ptr, 1, total, ctx->fp);
    ctx->bytesReceived += written;
    if (ctx->onProgress) {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - ctx->lastTick).count();
        if (ms >= 250) {
            ctx->lastTick = now;
            ctx->dotCount = (ctx->dotCount % 5) + 1;
            std::string dots(static_cast<size_t>(ctx->dotCount), '.');
            char mbBuf[64];
            std::snprintf(mbBuf, sizeof(mbBuf), "%.2f MB", static_cast<double>(ctx->bytesReceived) / 1048576.0);
            ctx->onProgress(ctx->prefix + " [ " + dots + " ] " + mbBuf + " received");
        }
    }
    return written;
}

} // anonymous namespace

bool ConfigParser::downloadFile(
    const std::string& url,
    const std::string& destPath,
    long timeoutSec,
    std::function<void(const std::string&)> onProgress,
    const std::string& progressPrefix
) {
    if (url.empty() || destPath.empty()) return false;
    std::string tmpPath = destPath + ".part";
    unlink(tmpPath.c_str());

    // 1. Primary path on MPC OS: Native dynamic libcurl.so.4 / libcurl.so.4.8.0
    LibCurlRuntime& curlApi = LibCurlRuntime::get();
    if (curlApi.available) {
        void* easy = curlApi.easy_init();
        if (easy) {
            FILE* fp = std::fopen(tmpPath.c_str(), "wb");
            if (fp) {
                CurlStreamCtx ctx;
                ctx.fp = fp;
                ctx.bytesReceived = 0;
                ctx.lastTick = std::chrono::steady_clock::now();
                ctx.dotCount = 0;
                ctx.onProgress = onProgress;
                ctx.prefix = progressPrefix;

                // Frozen libcurl ABI option constants (stable across all libcurl.so.4.x releases)
                constexpr int OPT_WRITEDATA      = 10001;
                constexpr int OPT_URL            = 10002;
                constexpr int OPT_USERAGENT      = 10018;
                constexpr int OPT_WRITEFUNCTION  = 20011;
                constexpr int OPT_TIMEOUT        = 13;
                constexpr int OPT_FAILONERROR    = 45;
                constexpr int OPT_FOLLOWLOCATION = 52;
                constexpr int OPT_SSL_VERIFYPEER = 64;
                constexpr int OPT_MAXREDIRS      = 68;
                constexpr int OPT_CONNECTTIMEOUT = 78;
                constexpr int OPT_SSL_VERIFYHOST = 81;
                constexpr int OPT_NOSIGNAL       = 99;
                constexpr int INFO_RESPONSE_CODE = 0x200002;

                curlApi.easy_setopt(easy, OPT_URL, url.c_str());
                curlApi.easy_setopt(easy, OPT_USERAGENT, "KikGen-MPC-PluginManager/2.0 (libcurl.so.4)");
                curlApi.easy_setopt(easy, OPT_WRITEFUNCTION, &tkgl_libcurl_write_cb);
                curlApi.easy_setopt(easy, OPT_WRITEDATA, &ctx);
                curlApi.easy_setopt(easy, OPT_FOLLOWLOCATION, 1L);
                curlApi.easy_setopt(easy, OPT_MAXREDIRS, 10L);
                curlApi.easy_setopt(easy, OPT_SSL_VERIFYPEER, 0L);
                curlApi.easy_setopt(easy, OPT_SSL_VERIFYHOST, 0L);
                curlApi.easy_setopt(easy, OPT_CONNECTTIMEOUT, 15L);
                curlApi.easy_setopt(easy, OPT_TIMEOUT, timeoutSec > 0 ? timeoutSec : 60L);
                curlApi.easy_setopt(easy, OPT_FAILONERROR, 1L);
                curlApi.easy_setopt(easy, OPT_NOSIGNAL, 1L);

                int res = curlApi.easy_perform(easy);
                std::fflush(fp);
                std::fclose(fp);

                long httpCode = 0;
                if (curlApi.easy_getinfo) {
                    curlApi.easy_getinfo(easy, INFO_RESPONSE_CODE, &httpCode);
                }
                curlApi.easy_cleanup(easy);

                struct stat st;
                if (res == 0 && (httpCode == 0 || (httpCode >= 200 && httpCode < 400)) &&
                    stat(tmpPath.c_str(), &st) == 0 && st.st_size > 0) {
                    unlink(destPath.c_str());
                    if ( std::rename(tmpPath.c_str(), destPath.c_str()) == 0 ) {
                        return true;
                    }
                }
                unlink(tmpPath.c_str());
            } else {
                curlApi.easy_cleanup(easy);
            }
        }
    }

    // 2. Fallback if libcurl.so.4 failed or is unavailable in a non-MPC environment
    std::ostringstream fallbackCmd;
    fallbackCmd << "rm -f \"" << tmpPath << "\"; ";
    fallbackCmd << "if command -v wget >/dev/null 2>&1; then ";
    fallbackCmd << "  wget --no-check-certificate -q -O \"" << tmpPath << "\" \"" << url << "\" 2>/dev/null || wget -q -O \"" << tmpPath << "\" \"" << url << "\" 2>/dev/null; ";
    fallbackCmd << "fi; ";
    fallbackCmd << "if [ ! -s \"" << tmpPath << "\" ] && command -v python3 >/dev/null 2>&1; then ";
    fallbackCmd << "  python3 -c 'import urllib.request,ssl,shutil,sys; ctx=ssl._create_unverified_context(); req=urllib.request.Request(sys.argv[1],headers={\"User-Agent\":\"Wget/1.21\"}); r=urllib.request.urlopen(req,context=ctx,timeout=" << (timeoutSec > 0 ? timeoutSec : 60) << "); f=open(sys.argv[2],\"wb\"); shutil.copyfileobj(r,f,65536); f.close()' \"" << url << "\" \"" << tmpPath << "\" 2>/dev/null; ";
    fallbackCmd << "fi; ";
    fallbackCmd << "if [ ! -s \"" << tmpPath << "\" ] && command -v curl >/dev/null 2>&1; then ";
    fallbackCmd << "  curl -k -fsSL --connect-timeout 15 -m " << (timeoutSec > 0 ? timeoutSec : 60) << " \"" << url << "\" -o \"" << tmpPath << "\" 2>/dev/null; ";
    fallbackCmd << "fi; ";
    fallbackCmd << "if [ -s \"" << tmpPath << "\" ]; then mv -f \"" << tmpPath << "\" \"" << destPath << "\"; else rm -f \"" << tmpPath << "\"; exit 1; fi";

    int rc = std::system(fallbackCmd.str().c_str());
    (void)rc;
    struct stat stFinal;
    return (stat(destPath.c_str(), &stFinal) == 0 && stFinal.st_size > 0);
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
            "2421", "7.6 MB", "31%",
            "athousanddetails - VST - 6W6", "tr6w6.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/6w6.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/6w6-vst-v1.2.0/6W6-1.2.0-mpc-armv7.zip", "plugins/tr-6w6/thumb.ppm",
            "MPC/Force port of athousanddetails' schwung-6W6 TR-606 drum machine: voices fitted against real hardware recordings, with measured-partial hats and cymbal. Touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - 6W6\" && if [ -f \"./plugins/tr-6w6/tr6w6.so\" ]; then cp -rf ./plugins/tr-6w6/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - 6W6/\"; else (wget -q -O /tmp/tr-6w6.zip \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/6w6-vst-v1.2.0/6W6-1.2.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/tr-6w6.zip \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/6w6-vst-v1.2.0/6W6-1.2.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/6w6-vst-v1.2.0/6W6-1.2.0-mpc-armv7.zip\" -o /tmp/tr-6w6.zip) && unzip -o /tmp/tr-6w6.zip -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - 6W6/\" && rm -f /tmp/tr-6w6.zip; fi && sync", 1,
            true,
            false,
            false,
            { "tr-606" }
        },
        {
            "tr-8w8", "8W8", "athousanddetails", "v1.2.0", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 2.x & 3.x", "2026-11-02",
            "2518", "5.9 MB", "13%",
            "athousanddetails - VST - 8W8", "tr8w8.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/8w8.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/8w8-vst-v1.2.0/8W8-1.2.0-mpc-armv7.zip", "plugins/tr-8w8/thumb.ppm",
            "MPC/Force port of athousanddetails' schwung-8W8 TR-808 drum machine: 15 of 16 voices circuit-modelled from the service notes. Touchscreen skin and Q-Links.", "./plugins/tr-8w8/install.sh", 0,
            false,
            false,
            false,
            { "tr-808" }
        },
        {
            "tr-9w9", "9W9", "athousanddetails", "v1.2.0", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 2.x & 3.x", "2026-11-02",
            "2457", "5.1 MB", "21%",
            "athousanddetails - VST - 9W9", "tr9w9.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/9w9.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.2.0/9W9-1.2.0-mpc-armv7.zip", "plugins/tr-9w9/thumb.ppm",
            "MPC/Force port of athousanddetails' schwung-9W9 TR-909 drum machine: circuit-modelled kick, snare, toms, rim and clap, with sampled hats, ride and crash (ER-99 derived), as on the original. Touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9\" && if [ -f \"./plugins/tr-9w9/tr9w9.so\" ]; then cp -rf ./plugins/tr-9w9/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9/\"; else (wget -q -O /tmp/tr-9w9.zip \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.2.0/9W9-1.2.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/tr-9w9.zip \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.2.0/9W9-1.2.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.2.0/9W9-1.2.0-mpc-armv7.zip\" -o /tmp/tr-9w9.zip) && unzip -o /tmp/tr-9w9.zip -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9/\" && rm -f /tmp/tr-9w9.zip; fi && sync", 1,
            false,
            false,
            false,
            { "tr-909" }
        },
        {
            "acid-303", "Acid-303", "sd88me", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "726", "23.6 MB", "28%",
            "sd88me - VST - Acid-303", "acid303.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-acid303/main/docs/device-303.png", "https://github.com/sd88me/mpc-vst-acid303/releases/download/acid303-vst-v1.0.2/Acid-303-1.0.2-mpc-armv7.zip", "plugins/acid-303/thumb.ppm",
            "An accurate, circuit-modelled 303 voice, matched to the hardware: the voice (from Lemus) solves the original schematic transistor by transistor and is fitted to 400 notes from a real unit. Switchable Devil Fish mods; Gen1 and Gen2 (WARN for CPU on Gen1: one or two instances, ECO).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Acid-303\" && if [ -f \"./plugins/acid-303/acid303.so\" ]; then cp -rf ./plugins/acid-303/* \"$TARGET_SYNTH_DIR/sd88me - VST - Acid-303/\"; else (wget -q -O /tmp/acid-303.zip \"https://github.com/sd88me/mpc-vst-acid303/releases/download/acid303-vst-v1.0.2/Acid-303-1.0.2-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/acid-303.zip \"https://github.com/sd88me/mpc-vst-acid303/releases/download/acid303-vst-v1.0.2/Acid-303-1.0.2-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-acid303/releases/download/acid303-vst-v1.0.2/Acid-303-1.0.2-mpc-armv7.zip\" -o /tmp/acid-303.zip) && unzip -o /tmp/acid-303.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Acid-303/\" && rm -f /tmp/acid-303.zip; fi && sync", 1,
            false,
            false,
            false,
            { "303", "acid", "mono", "bass", "circuit-model", "devil-fish" }
        },
        {
            "aphex", "Aphex", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1442", "7.4 MB", "14%",
            "Filliformes - VST - [SYN] Aphex", "aphex.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-aphex/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-aphex/releases/download/v1.1.1/SYN-Aphex-1.1.1-mpc-armv7.zip", "plugins/aphex/thumb.ppm",
            "Korg MS-10/MS-20 style mono synth with a patch bay, an ESP section and both MS filters.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Aphex\" && if [ -f \"./plugins/aphex/aphex.so\" ]; then cp -rf ./plugins/aphex/* \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Aphex/\"; else (wget -q -O /tmp/aphex.zip \"https://github.com/saustin2010/mpc-vst-aphex/releases/download/v1.1.1/SYN-Aphex-1.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/aphex.zip \"https://github.com/saustin2010/mpc-vst-aphex/releases/download/v1.1.1/SYN-Aphex-1.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-aphex/releases/download/v1.1.1/SYN-Aphex-1.1.1-mpc-armv7.zip\" -o /tmp/aphex.zip) && unzip -o /tmp/aphex.zip -d \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Aphex/\" && rm -f /tmp/aphex.zip; fi && sync", 1,
            false,
            false,
            false,
            { "ms-20", "monophonic", "patch-bay", "analog-modelling" }
        },
        {
            "chordism", "Chordism", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1460", "2.3 MB", "14%",
            "Charles Vestal - VST - [SYN] Chordism", "chordism.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-chordism/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-chordism/releases/download/v1.0.2/SYN-Chordism-1.0.2-mpc-armv7.zip", "plugins/chordism/thumb.ppm",
            "One key in, a four-voice chord out: morphing oscillators, FM, filter, lo-fi, delay, reverb and an arpeggiator.", "./plugins/chordism/install.sh", 0,
            false,
            false,
            false,
            { "chords", "arpeggiator", "fm", "lo-fi" }
        },
        {
            "clementine-xt", "Clementine-XT", "sd88me", "v0.9.7", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-11-01",
            "3946", "2.5 MB", "14%",
            "sd88me - VST - Clementine-XT", "clementine_xt.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-clementineXT/main/docs/img/clementine-xt.png", "https://github.com/sd88me/mpc-vst-clementineXT/releases/download/clementine-xt-v0.9.7/Clementine-XT-0.9.7-mpc-armv7.zip", "plugins/clementine-xt/thumb.ppm",
            "Ten-voice wavetable synthesizer modelled exactly on the Microwave II/XT with a ten-page touchscreen skin and Q-Links. Needs your own ROM files in its ROMS folder.", "./plugins/clementine-xt/install.sh", 0,
            false,
            false,
            false,
            { "wavetable", "polyphonic", "arpeggiator" }
        },
        {
            "op1fx-cluster", "Cluster", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "90", "3.5 MB", "14%",
            "bombest - VST - Cluster", "cluster.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/cluster.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cluster-0.4.1-mpc-armv7.zip", "plugins/op1fx-cluster/thumb.ppm",
            "Eight self-feedback sine oscillators that drift apart over a note. OP-1 inspired.", "./plugins/op1fx-cluster/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "feedback", "drone" }
        },
        {
            "crate-digger", "Crate Digger", "sd88me", "v1.3.0", "Sample Browser",
            "INST", "Stable", "MPC Live III (Gen2) (az01 base 5.0.17), Akai Force (az01 base 5.0.17)", "MPC OS 2.x & 3.x", "2026-10-28",
            "8876", "67.8 MB", "3%",
            "sd88me - VST - Crate Digger", "cratedigger.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-cratedigger/main/docs/img/crate-digger.png", "https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v1.3.0/Crate-Digger-1.3.0-mpc-armv7.zip", "plugins/crate-digger/thumb.ppm",
            "Dig for records by genre, style, decade, region and country on Discogs, then stream and play them into an MPC track, with its own screen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger\" && if [ -f \"./plugins/crate-digger/cratedigger.so\" ]; then cp -rf ./plugins/crate-digger/* \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger/\"; else (wget -q -O /tmp/crate-digger.zip \"https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v1.3.0/Crate-Digger-1.3.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/crate-digger.zip \"https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v1.3.0/Crate-Digger-1.3.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v1.3.0/Crate-Digger-1.3.0-mpc-armv7.zip\" -o /tmp/crate-digger.zip) && unzip -o /tmp/crate-digger.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger/\" && rm -f /tmp/crate-digger.zip; fi && sync", 1,
            false,
            false,
            false,
            { "discogs", "streaming" }
        },
        {
            "tr-cw78", "CW-78", "athousanddetails", "v1.2.0", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 2.x & 3.x", "2026-11-02",
            "2313", "6.9 MB", "8%",
            "athousanddetails - VST - CW-78", "trcw78.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/cw78.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/cw78-vst-v1.2.0/CW-78-1.2.0-mpc-armv7.zip", "plugins/tr-cw78/thumb.ppm",
            "MPC/Force port of athousanddetails' schwung-CW-78 CR-78 drum machine: voices modelled from the 1979 service notes, with the preset rhythms. Touchscreen skin and Q-Links.", "./plugins/tr-cw78/install.sh", 0,
            false,
            false,
            false,
            { "cr-78" }
        },
        {
            "op1fx-dbox", "DBox", "Bombest", "v0.4.1", "Drum Machine",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "81", "8.6 MB", "14%",
            "bombest - VST - DBox", "dbox.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dbox.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DBox-0.4.1-mpc-armv7.zip", "plugins/op1fx-dbox/thumb.ppm",
            "A 24-key synthesized drum kit: two oscillators and noise per hit, 12 voices. OP-1 inspired.", "./plugins/op1fx-dbox/install.sh", 0,
            false,
            false,
            false,
            { "op1", "drums", "synth", "kit" }
        },
        {
            "denis", "Denis", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1364", "5.4 MB", "14%",
            "Filliformes - VST - [SYN] Denis", "denis.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-denis/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-denis/releases/download/v1.1.1/SYN-Denis-1.1.1-mpc-armv7.zip", "plugins/denis/thumb.ppm",
            "West Coast / Serge-inspired mono synth: complex oscillators, a wavefolder and a modulation matrix.", "./plugins/denis/install.sh", 0,
            false,
            false,
            false,
            { "west-coast", "serge", "wavefolder", "monophonic" }
        },
        {
            "dexed-dx7", "Dexed", "sd88me", "v1.0.7", "Fm Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "6145", "3.7 MB", "14%",
            "sd88me - VST - Dexed (DX7)", "dx7_dexed.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-dx7/main/docs/img/dexed-dx7.png", "https://github.com/sd88me/mpc-vst-dx7/releases/download/dx7-v1.0.7/Dexed-DX7-1.0.7-mpc-armv7.zip", "plugins/dexed-dx7/thumb.ppm",
            "DX7-style 6-operator FM synth (Dexed/MSFA engine) as a native instrument, with .syx bank browsing, all 32 algorithm diagrams and a touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Dexed (DX7)\" && if [ -f \"./plugins/dexed-dx7/dx7_dexed.so\" ]; then cp -rf ./plugins/dexed-dx7/* \"$TARGET_SYNTH_DIR/sd88me - VST - Dexed (DX7)/\"; else (wget -q -O /tmp/dexed-dx7.zip \"https://github.com/sd88me/mpc-vst-dx7/releases/download/dx7-v1.0.7/Dexed-DX7-1.0.7-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/dexed-dx7.zip \"https://github.com/sd88me/mpc-vst-dx7/releases/download/dx7-v1.0.7/Dexed-DX7-1.0.7-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-dx7/releases/download/dx7-v1.0.7/Dexed-DX7-1.0.7-mpc-armv7.zip\" -o /tmp/dexed-dx7.zip) && unzip -o /tmp/dexed-dx7.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Dexed (DX7)/\" && rm -f /tmp/dexed-dx7.zip; fi && sync", 1,
            true,
            false,
            false,
            { "dx7", "emulation" }
        },
        {
            "op1fx-digital", "Digital", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "75", "5.7 MB", "14%",
            "bombest - VST - Digital", "digital.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/digital.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Digital-0.4.1-mpc-armv7.zip", "plugins/op1fx-digital/thumb.ppm",
            "Feedback sine oscillator, a second sine with sync, and a bit-flipping XOR stage. Eight voices. OP-1 inspired.", "./plugins/op1fx-digital/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "feedback", "xor" }
        },
        {
            "dinxxl", "DINXXL", "L'Cronx", "v1.0.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "177", "4.5 MB", "14%",
            "L'Cronx - VST - DINXXL", "dinxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/dinxxl/v1.0.0/docs/img/keys.png", "https://github.com/sunskiefer/DINXXL/releases/download/v1.0.0/DINXXL-1.0.0-mpc-armv7.zip", "plugins/dinxxl/thumb.ppm",
            "BlastBeats-style OPL3 FM groovebox: the published v7.1 firmware by Twisted Electrons runs unmodified on a software model of its board (YMF262 via ymfm). 6 drums, 4 four-op synths, 64-step sequencer with p-locks and songs, factory card inside. By L'Cronx; not made or endorsed by Twisted Electrons.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - DINXXL\" && if [ -f \"./plugins/dinxxl/dinxxl.so\" ]; then cp -rf ./plugins/dinxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - DINXXL/\"; else (wget -q -O /tmp/dinxxl.zip \"https://github.com/sunskiefer/DINXXL/releases/download/v1.0.0/DINXXL-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/dinxxl.zip \"https://github.com/sunskiefer/DINXXL/releases/download/v1.0.0/DINXXL-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/DINXXL/releases/download/v1.0.0/DINXXL-1.0.0-mpc-armv7.zip\" -o /tmp/dinxxl.zip) && unzip -o /tmp/dinxxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - DINXXL/\" && rm -f /tmp/dinxxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "blastbeats", "opl3", "fm", "drum-machine", "groovebox", "step-sequencer", "chiptune", "adlib" }
        },
        {
            "op1fx-dna", "DNA", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "90", "4.7 MB", "14%",
            "bombest - VST - DNA", "dna.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dna.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DNA-0.4.1-mpc-armv7.zip", "plugins/op1fx-dna/thumb.ppm",
            "A 128-bit genome played as a one-bit wave through an anti-aliased reader. Eight voices. OP-1 inspired.", "./plugins/op1fx-dna/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "1-bit", "wavetable" }
        },
        {
            "drloguexxl", "DrLogueXXL", "L'Cronx", "v0.2.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "467", "7.8 MB", "14%",
            "L'Cronx - VST - DrLogueXXL", "drloguexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/DrLogueXXL/v0.2.0/docs/img/perform.png", "https://github.com/sunskiefer/DrLogueXXL/releases/download/v0.2.1/DrLogueXXL-0.2.1-mpc-armv7.zip", "plugins/drloguexxl/thumb.ppm",
            "Hybrid drum machine modelled on the KORG drumlogue (not affiliated with KORG): analogue-style kick, snare and toms, six sample parts with 192 synthesised sounds and your own WAVs, a MULTI part with VPM, noise and six open logue SDK units, delay, reverb and master effects, choke groups, a mixer and four macros, on a panel in the machine's look.", "./plugins/drloguexxl/install.sh", 0,
            false,
            false,
            false,
            { "drums", "analog", "hybrid", "samples", "fm", "logue", "drumlogue", "mixer" }
        },
        {
            "dronexxl", "DroneXXL", "L'Cronx", "v0.1.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-25",
            "358", "83.7 MB", "14%",
            "L'Cronx - VST - DroneXXL", "dronexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/DroneXXL/v0.1.0/docs/img/overview.png", "https://github.com/sunskiefer/DroneXXL/releases/download/v0.1.0/DroneXXL-0.1.0-mpc-armv7.zip", "plugins/dronexxl/thumb.ppm",
            "Drone and experimental synth, after the Solar 42, built on Neil Baumberger's AmbientModules: a 15-module rack (4 drones, mixer, filter, pads, 2 PapaSrapa, 2 VCOs, LFOs, joystick, sequencer, detector) patched by touch, with 8 macros, an XY pad, a randomizer that never repeats and a brickwall limiter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - DroneXXL\" && if [ -f \"./plugins/dronexxl/dronexxl.so\" ]; then cp -rf ./plugins/dronexxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - DroneXXL/\"; else (wget -q -O /tmp/dronexxl.zip \"https://github.com/sunskiefer/DroneXXL/releases/download/v0.1.0/DroneXXL-0.1.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/dronexxl.zip \"https://github.com/sunskiefer/DroneXXL/releases/download/v0.1.0/DroneXXL-0.1.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/DroneXXL/releases/download/v0.1.0/DroneXXL-0.1.0-mpc-armv7.zip\" -o /tmp/dronexxl.zip) && unzip -o /tmp/dronexxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - DroneXXL/\" && rm -f /tmp/dronexxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drone", "experimental", "modular", "patch-panel", "ambient", "randomizer", "vcv-rack" }
        },
        {
            "op1fx-drwave", "DrWave", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "84", "6.7 MB", "14%",
            "bombest - VST - DrWave", "drwave.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/drwave.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DrWave-0.4.1-mpc-armv7.zip", "plugins/op1fx-drwave/thumb.ppm",
            "Wave, blend, motion and fold controls over eight voices. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - DrWave\" && if [ -f \"./plugins/op1fx-drwave/drwave.so\" ]; then cp -rf ./plugins/op1fx-drwave/* \"$TARGET_SYNTH_DIR/bombest - VST - DrWave/\"; else (wget -q -O /tmp/op1fx-drwave.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DrWave-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-drwave.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DrWave-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DrWave-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-drwave.zip) && unzip -o /tmp/op1fx-drwave.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - DrWave/\" && rm -f /tmp/op1fx-drwave.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "synth", "wavetable", "fold" }
        },
        {
            "op1fx-dsynth", "DSynth", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "80", "4.8 MB", "14%",
            "bombest - VST - DSynth", "dsynth.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dsynth.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DSynth-0.4.1-mpc-armv7.zip", "plugins/op1fx-dsynth/thumb.ppm",
            "Two oscillators with FM, a pitch drop for drum thump and an eight-mode filter. OP-1 inspired.", "./plugins/op1fx-dsynth/install.sh", 0,
            false,
            false,
            false,
            { "op1", "synth", "fm", "drums" }
        },
        {
            "dub-force-siren", "Dub Force Siren", "no3z", "v2.1.0", "Synth",
            "INST", "Stable", "Akai Force (3.9.1.2)", "MPC OS 3.x", "2026-10-26",
            "1088", "117 KB", "14%",
            "Dub Force - VST - Dub Force Siren", "dub_force_siren.so",
            "https://raw.githubusercontent.com/no3z/mpc-dubsiren/v2.0.0/resources/preview.png", "https://github.com/no3z/mpc-dubsiren/releases/download/v2.1.0/Dub-Force-Siren-2.1.0-mpc-armv7.zip", "plugins/dub-force-siren/thumb.ppm",
            "Dub siren with tape echo: siren and zap modes, LFO, filter and ping-pong delay on one page mirroring two banks of eight Q-Links.", "./plugins/dub-force-siren/install.sh", 0,
            false,
            false,
            false,
            { "dub", "siren", "echo", "sfx" }
        },
        {
            "evil-tuner-xxl", "Evil Tuner XXL", "L'Cronx", "v0.2.0", "Radio Granular",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "1055", "12.8 MB", "14%",
            "L'Cronx - VST - Evil Tuner XXL", "evil_tuner_xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Evil-Tuner-XXL/main/docs/img/RADIO.png", "https://github.com/sunskiefer/Evil-Tuner-XXL/releases/download/v0.2.0/Evil-Tuner-XXL-0.2.0-mpc-armv7.zip", "plugins/evil-tuner-xxl/thumb.ppm",
            "Internet radio (30,000+ stations), or a phone over Wi-Fi or AirPlay, played into an MPC track through a granular engine, filter, character stages, delay, reverb, trance gate, stutter, tempo-synced looper and three LFOs. Inspired by the Endorphin.es Evil Pet (no affiliation, no code). Needs a network.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - Evil Tuner XXL\" && if [ -f \"./plugins/evil-tuner-xxl/evil_tuner_xxl.so\" ]; then cp -rf ./plugins/evil-tuner-xxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - Evil Tuner XXL/\"; else (wget -q -O /tmp/evil-tuner-xxl.zip \"https://github.com/sunskiefer/Evil-Tuner-XXL/releases/download/v0.2.0/Evil-Tuner-XXL-0.2.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/evil-tuner-xxl.zip \"https://github.com/sunskiefer/Evil-Tuner-XXL/releases/download/v0.2.0/Evil-Tuner-XXL-0.2.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/Evil-Tuner-XXL/releases/download/v0.2.0/Evil-Tuner-XXL-0.2.0-mpc-armv7.zip\" -o /tmp/evil-tuner-xxl.zip) && unzip -o /tmp/evil-tuner-xxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - Evil Tuner XXL/\" && rm -f /tmp/evil-tuner-xxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "radio", "internet-radio", "granular", "airplay", "looper", "stutter", "reverb", "sampling" }
        },
        {
            "fizzik", "Fizzik", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1366", "1.8 MB", "14%",
            "Filliformes - VST - [SYN] Fizzik", "fizzik.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-fizzik/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-fizzik/releases/download/v1.0.2/SYN-Fizzik-1.0.2-mpc-armv7.zip", "plugins/fizzik/thumb.ppm",
            "Physical modelling: an exciter and two coupled resonators (string, beam, plate, membrane).", "./plugins/fizzik/install.sh", 0,
            false,
            false,
            false,
            { "physical-modelling", "resonator", "percussion" }
        },
        {
            "op1fx-fm", "Fm", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "89", "8.3 MB", "14%",
            "bombest - VST - Fm", "fm.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/fm.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Fm-0.4.1-mpc-armv7.zip", "plugins/op1fx-fm/thumb.ppm",
            "Four-operator FM synth with eight voices and four screen knobs. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Fm\" && if [ -f \"./plugins/op1fx-fm/fm.so\" ]; then cp -rf ./plugins/op1fx-fm/* \"$TARGET_SYNTH_DIR/bombest - VST - Fm/\"; else (wget -q -O /tmp/op1fx-fm.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Fm-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-fm.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Fm-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Fm-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-fm.zip) && unzip -o /tmp/op1fx-fm.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Fm/\" && rm -f /tmp/op1fx-fm.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "synth", "fm", "operators" }
        },
        {
            "fretty", "Fretty", "3dthc", "v0.8.5", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "1124", "69.8 MB", "14%",
            "3dthc - VST - Fretty", "fretty.so",
            "https://raw.githubusercontent.com/3dthc/fretty-mpc/main/docs/images/play_sunburst.png", "https://github.com/3dthc/fretty-mpc/releases/download/v0.8.5/Fretty-0.8.5-mpc-armv7.zip", "plugins/fretty/thumb.ppm",
            "A guitar instrument for Akai MPC OS standalone devices (native VST2), featuring refined selector typography, 110 presets, and a synth layer.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - Fretty\" && if [ -f \"./plugins/fretty/fretty.so\" ]; then cp -rf ./plugins/fretty/* \"$TARGET_SYNTH_DIR/3dthc - VST - Fretty/\"; else (wget -q -O /tmp/fretty.zip \"https://github.com/3dthc/fretty-mpc/releases/download/v0.8.5/Fretty-0.8.5-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/fretty.zip \"https://github.com/3dthc/fretty-mpc/releases/download/v0.8.5/Fretty-0.8.5-mpc-armv7.zip\" || curl -k -sL \"https://github.com/3dthc/fretty-mpc/releases/download/v0.8.5/Fretty-0.8.5-mpc-armv7.zip\" -o /tmp/fretty.zip) && unzip -o /tmp/fretty.zip -d \"$TARGET_SYNTH_DIR/3dthc - VST - Fretty/\" && rm -f /tmp/fretty.zip; fi && sync", 1,
            false,
            false,
            false,
            {  }
        },
        {
            "gaiaxxl", "GAIAXXL", "L'Cronx", "v0.1.3", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "832", "7.7 MB", "14%",
            "L'Cronx - VST - GAIAXXL", "gaiaxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/GAIAXXL/v0.1.2/docs/img/tone.png", "https://github.com/sunskiefer/GAIAXXL/releases/download/v0.1.3/GAIAXXL-0.1.3-mpc-armv7.zip", "plugins/gaiaxxl/thumb.ppm",
            "Three-tone virtual-analog synth modelled on the discontinued Roland GAIA SH-01 (not affiliated with Roland): the editor's parameters over six full-screen pages, 21 oscillator variations (with KORG logue-sdk wave tables), SYNC/RING, the GAIA's effects chain and arpeggiator, two beams, a musical randomizer and 64 patches.", "./plugins/gaiaxxl/install.sh", 0,
            false,
            false,
            false,
            { "synth", "virtual-analog", "supersaw", "gaia", "arpeggiator", "pad", "lead", "randomizer" }
        },
        {
            "hank", "Hank", "saustin2010", "v1.1.1", "Fm Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1084", "2.1 MB", "14%",
            "Charles Vestal - VST - [SYN] Hank", "hank.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-hank/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-hank/releases/download/v1.1.1/SYN-Hank-1.1.1-mpc-armv7.zip", "plugins/hank/thumb.ppm",
            "Small, immediate two-operator FM synth on one page: ratio, brightness, bite, envelopes, noise; 32 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Hank\" && if [ -f \"./plugins/hank/hank.so\" ]; then cp -rf ./plugins/hank/* \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Hank/\"; else (wget -q -O /tmp/hank.zip \"https://github.com/saustin2010/mpc-vst-hank/releases/download/v1.1.1/SYN-Hank-1.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/hank.zip \"https://github.com/saustin2010/mpc-vst-hank/releases/download/v1.1.1/SYN-Hank-1.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-hank/releases/download/v1.1.1/SYN-Hank-1.1.1-mpc-armv7.zip\" -o /tmp/hank.zip) && unzip -o /tmp/hank.zip -d \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Hank/\" && rm -f /tmp/hank.zip; fi && sync", 1,
            false,
            false,
            false,
            { "fm", "two-operator", "polyphonic", "bass" }
        },
        {
            "helm", "Helm", "Lewinator56", "v1.0.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-04",
            "1095", "12.3 MB", "14%",
            "Matt Tytel - VST - Helm", "helm.so",
            "https://github.com/user-attachments/assets/b537127a-22e6-4e21-b9f1-48e473c892b8", "https://github.com/Lewinator56/mpc-vst-helm/releases/download/1.0.0/Helm-1.0.0-mpc-armv7.zip", "plugins/helm/thumb.ppm",
            "32-voice polyphonic synthesizer (Helm engine by Matt Tytel) with 2 cross-mod oscillators, sub, noise, multi-mode filter, 3 LFOs, 3 envelopes, 32-step sequencer & arpeggiator, built-in effects and touchscreen skin with Q-Links.", "./plugins/helm/install.sh", 0,
            false,
            false,
            false,
            { "polyphonic", "subtractive", "arpeggiator", "step-sequencer", "mod-matrix" }
        },
        {
            "hera", "Hera", "saustin2010", "v1.1.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "2183", "3.3 MB", "14%",
            "jpcima - VST - [SYN] Hera", "hera.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-hera/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-hera/releases/download/v1.1.2/SYN-Hera-1.1.2-mpc-armv7.zip", "plugins/hera/thumb.ppm",
            "Roland Juno-60: jpcima's Hera engine with its 56 presets, the BBD chorus and a touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jpcima - VST - [SYN] Hera\" && if [ -f \"./plugins/hera/hera.so\" ]; then cp -rf ./plugins/hera/* \"$TARGET_SYNTH_DIR/jpcima - VST - [SYN] Hera/\"; else (wget -q -O /tmp/hera.zip \"https://github.com/saustin2010/mpc-vst-hera/releases/download/v1.1.2/SYN-Hera-1.1.2-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/hera.zip \"https://github.com/saustin2010/mpc-vst-hera/releases/download/v1.1.2/SYN-Hera-1.1.2-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-hera/releases/download/v1.1.2/SYN-Hera-1.1.2-mpc-armv7.zip\" -o /tmp/hera.zip) && unzip -o /tmp/hera.zip -d \"$TARGET_SYNTH_DIR/jpcima - VST - [SYN] Hera/\" && rm -f /tmp/hera.zip; fi && sync", 1,
            false,
            false,
            false,
            { "juno-60", "polyphonic", "chorus", "analog-modelling" }
        },
        {
            "hexachord-73", "HexChord 73", "Eurikon", "v1.0.0", "Synth",
            "INST", "Stable", "MPC Live II Retro (3.9.1), MPC Live II Retro (3.10)", "MPC OS 3.x", "2026-10-27",
            "257", "2.9 MB", "23%",
            "Twisted Cable - VST - HexChord 73", "hexachord-73.so",
            "https://raw.githubusercontent.com/Eurikon/mpc-vst-plugins/9eed16e63f97e5ad87621a21cead85b6a7fee35b/media/hexachord-73.png", "https://github.com/Eurikon/mpc-vst-plugins/releases/download/hexachord-73-v1.0.0/HexChord-73-1.0.0-mpc-armv7.zip", "plugins/hexachord-73/thumb.ppm",
            "Stereo chord synthesizer with four engines, eight chords, four voicings and a touchscreen chord diagram.", "./plugins/hexachord-73/install.sh", 0,
            false,
            false,
            false,
            { "chord", "polyphonic", "stereo" }
        },
        {
            "incipience", "Incipience", "saustin2010", "v1.1.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "777", "7.0 MB", "14%",
            "Steve A - VST - [DRUM] Incipience", "incipience.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-incipience/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-incipience/releases/download/v1.1.1/DRUM-Incipience-1.1.1-mpc-armv7.zip", "plugins/incipience/thumb.ppm",
            "Eleven-instrument drum synth laid out like the Jomox AlphaBase Mk II (kick, MBrane, hats, clap, rim, cymbals, two sample players, 4-op FM, reverb, delay), with RANDOMISE per instrument and a mod matrix; each instrument chromatic on its own MIDI channel and on its own track with Incipience OUT; plays the AlphaBase's free samples and your own WAVs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Incipience\" && if [ -f \"./plugins/incipience/incipience.so\" ]; then cp -rf ./plugins/incipience/* \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Incipience/\"; else (wget -q -O /tmp/incipience.zip \"https://github.com/saustin2010/mpc-vst-incipience/releases/download/v1.1.1/DRUM-Incipience-1.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/incipience.zip \"https://github.com/saustin2010/mpc-vst-incipience/releases/download/v1.1.1/DRUM-Incipience-1.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-incipience/releases/download/v1.1.1/DRUM-Incipience-1.1.1-mpc-armv7.zip\" -o /tmp/incipience.zip) && unzip -o /tmp/incipience.zip -d \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Incipience/\" && rm -f /tmp/incipience.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drums", "alphabase", "fm", "samples", "multi-out", "mod-matrix" }
        },
        {
            "interzonexxl", "InterzoneXXL", "L'Cronx", "v0.1.3", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "1626", "13.6 MB", "14%",
            "L'Cronx - VST - InterzoneXXL", "interzonexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/InterzoneXXL/v0.1.0/docs/img/vco.png", "https://github.com/sunskiefer/InterzoneXXL/releases/download/v0.1.3/InterzoneXXL-0.1.3-mpc-armv7.zip", "plugins/interzonexxl/thumb.ppm",
            "Valley Audio's Interzone synth voice from VCV Rack, played from MIDI in mono or up to 16-voice poly on the module's own panel. Every input jack is patched to a built-in source (LFOs, sequencers, random) on the MPC tempo, with a brickwall limiter and 16 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - InterzoneXXL\" && if [ -f \"./plugins/interzonexxl/interzonexxl.so\" ]; then cp -rf ./plugins/interzonexxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - InterzoneXXL/\"; else (wget -q -O /tmp/interzonexxl.zip \"https://github.com/sunskiefer/InterzoneXXL/releases/download/v0.1.3/InterzoneXXL-0.1.3-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/interzonexxl.zip \"https://github.com/sunskiefer/InterzoneXXL/releases/download/v0.1.3/InterzoneXXL-0.1.3-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/InterzoneXXL/releases/download/v0.1.3/InterzoneXXL-0.1.3-mpc-armv7.zip\" -o /tmp/interzonexxl.zip) && unzip -o /tmp/interzonexxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - InterzoneXXL/\" && rm -f /tmp/interzonexxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "synth", "monosynth", "polysynth", "analog", "vcv", "valley", "lfo", "modulation" }
        },
        {
            "j106", "JN-106", "sd88me", "v0.2.0", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), Akai MPC Live III (aarch64) (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-28",
            "716", "25.3 MB", "14%",
            "sd88me - VST - JN-106", "kr106.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-jn106/9053529f679d2a5f07781f002004e27cc12959e0/docs/img/screenshot-lfo-dco.png", "https://github.com/sd88me/mpc-vst-jn106/releases/download/jn-106-vst-v0.2.0/JN-106-0.2.0-mpc-armv7.zip", "plugins/j106/thumb.ppm",
            "Faithful, circuit-modelled and hardware-profiled six-voice Juno emulation (Ultramaster KR-106 DSP): IR3109 filter, BA662 VCA and MN3009 BBD chorus modelled from the circuits, the 106 CPU firmware reimplemented from its disassembly, curves fitted to real hardware and checked against the service notes. 256 factory presets, realistic panel skin.", "./plugins/j106/install.sh", 0,
            false,
            false,
            false,
            { "analog", "poly", "vintage", "chorus" }
        },
        {
            "kutilxxl", "KutilXXL", "L'Cronx", "v0.2.1", "Groovebox",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "227", "85.3 MB", "14%",
            "L'Cronx - VST - KutilXXL", "kutilxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/BUSTEDXXL/v0.2.1/docs/img/screen1.png", "https://github.com/sunskiefer/BUSTEDXXL/releases/download/v0.2.1/KutilXXL-0.2.1-mpc-armv7.zip", "plugins/kutilxxl/thumb.ppm",
            "Groovebox of Bastl-style modules patched with touch cables across three screens: Wave Bard on Bastl's open-source Kastle 2 DSP plus Kompas, Crust 2, Pizza, Alchemist, Basil and ABC. Tap a module to open a window with just its jacks, big and named, and patch between modules.", "./plugins/kutilxxl/install.sh", 0,
            false,
            false,
            false,
            { "bastl", "kastle", "modular", "patch-panel", "touch-cables", "sampler", "fm", "groovebox" }
        },
        {
            "lucky-dip", "Lucky Dip", "sd88me", "v0.9.2", "Sampler",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 2.x & 3.x", "2026-10-21",
            "1761", "2.0 MB", "14%",
            "sd88me - VST - Lucky Dip", "lucky_dip.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-luckydip/main/docs/img/lucky-dip.png", "https://github.com/sd88me/mpc-vst-luckydip/releases/download/v0.9.2/Lucky-Dip-0.9.2-mpc-armv7.zip", "plugins/lucky-dip/thumb.ppm",
            "A random 16-pad drum-kit builder and inspiration machine: dips into your sample library, fills the pads at random, plays the kit and exports it as an MPC drum program.", "./plugins/lucky-dip/install.sh", 0,
            false,
            false,
            false,
            { "sampler", "utility", "kit-builder", "random", "drum-kit" }
        },
        {
            "lxrxxl", "LXRXXL", "L'Cronx", "v0.6.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3351", "10.2 MB", "14%",
            "L'Cronx - VST - LXRXXL", "lxrxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/LXR02XXL/v0.6.0/docs/img/page.png", "https://github.com/sunskiefer/LXR02XXL/releases/download/v0.6.1/LXRXXL-0.6.1-mpc-armv7.zip", "plugins/lxrxxl/thumb.ppm",
            "Inspired by the LXR-02: its open-source sound engine (GPL-3.0) with the real FX unit and the plate reverb, the whole panel on one page, 64-step sequencer with songs, rolls and a euclidean generator, Force pads playing the voices, randomizers and the Hrtl LXR-02 DLC presets old and new. Artwork by Suns Kiefer.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL\" && if [ -f \"./plugins/lxrxxl/lxrxxl.so\" ]; then cp -rf ./plugins/lxrxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL/\"; else (wget -q -O /tmp/lxrxxl.zip \"https://github.com/sunskiefer/LXR02XXL/releases/download/v0.6.1/LXRXXL-0.6.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/lxrxxl.zip \"https://github.com/sunskiefer/LXR02XXL/releases/download/v0.6.1/LXRXXL-0.6.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/LXR02XXL/releases/download/v0.6.1/LXRXXL-0.6.1-mpc-armv7.zip\" -o /tmp/lxrxxl.zip) && unzip -o /tmp/lxrxxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL/\" && rm -f /tmp/lxrxxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drums", "drum-machine", "lxr", "lxr-02", "erica-synths", "sequencer", "reverb", "randomizer" }
        },
        {
            "lxrxxl-bazzline", "LXRXXL BAZZLINE", "L'Cronx", "v0.2.10", "Bass Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "3025", "16.2 MB", "14%",
            "L'Cronx - VST - LXRXXL BAZZLINE", "lxrxxl_bazzline.so",
            "https://raw.githubusercontent.com/sunskiefer/LXRXXL-BAZZLINE/v0.2.2/docs/img/panel.png", "https://github.com/sunskiefer/LXRXXL-BAZZLINE/releases/download/v0.2.10/LXRXXL-BAZZLINE-0.2.10-mpc-armv7.zip", "plugins/lxrxxl-bazzline/thumb.ppm",
            "An 8-track bass synth: a variation of LXRXXL with bass voices inspired by the Erica Synths Bassline DB-01 on the LXR's oscillators, the LXR-02 style sequencer with the DB-01's step functions, a tape/digital delay, a MIDI gate/duck, an 8-track mixer and sound / performance presets saved on the device.", "./plugins/lxrxxl-bazzline/install.sh", 0,
            false,
            false,
            false,
            { "bass", "acid", "303", "bassline", "db-01", "lxr", "sequencer", "delay" }
        },
        {
            "lxrxxl-zynth", "LXRXXL ZYNTH", "L'Cronx", "v0.2.2", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "1201", "22.4 MB", "14%",
            "L'Cronx - VST - LXRXXL ZYNTH", "lxrxxl_zynth.so",
            "https://raw.githubusercontent.com/sunskiefer/LXRXXL-ZYNTH/v0.2.0/docs/img/panel.png", "https://github.com/sunskiefer/LXRXXL-ZYNTH/releases/download/v0.2.2/LXRXXL-ZYNTH-0.2.2-mpc-armv7.zip", "plugins/lxrxxl-zynth/thumb.ppm",
            "An experimental 8-track synth with 19 voices on unconventional maths (Lorenz chaos, Hilbert frequency shifter, Euclidean rhythms) and voices coded after the DX7, CZ, Minimoog, Hammond, NES chips, Braids and Plaits. LXR-02 style sequencer with random functions, tape/digital delay, mixer and presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL ZYNTH\" && if [ -f \"./plugins/lxrxxl-zynth/lxrxxl_zynth.so\" ]; then cp -rf ./plugins/lxrxxl-zynth/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL ZYNTH/\"; else (wget -q -O /tmp/lxrxxl-zynth.zip \"https://github.com/sunskiefer/LXRXXL-ZYNTH/releases/download/v0.2.2/LXRXXL-ZYNTH-0.2.2-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/lxrxxl-zynth.zip \"https://github.com/sunskiefer/LXRXXL-ZYNTH/releases/download/v0.2.2/LXRXXL-ZYNTH-0.2.2-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/LXRXXL-ZYNTH/releases/download/v0.2.2/LXRXXL-ZYNTH-0.2.2-mpc-armv7.zip\" -o /tmp/lxrxxl-zynth.zip) && unzip -o /tmp/lxrxxl-zynth.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL ZYNTH/\" && rm -f /tmp/lxrxxl-zynth.zip; fi && sync", 1,
            false,
            false,
            false,
            { "synth", "groovebox", "fm", "wavetable", "sequencer", "experimental", "chaos", "lxr" }
        },
        {
            "machinemodule", "Machinemodule", "sd88me", "v1.0.3", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "100", "2.0 MB", "14%",
            "sd88me - VST - Machinemodule", "machinemodule.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule.png", "", "plugins/machinemodule/thumb.ppm",
            "The Elektron Machinedrum SPS-1 UW sound engine as a native instrument: all 16 tracks from one plugin instance, with its own touchscreen skin and Q-Links.", "./plugins/machinemodule/install.sh", 0,
            false,
            false,
            false,
            { "elektron", "machinedrum", "firmware-derived" }
        },
        {
            "machinemodule-gen2", "Machinemodule (Gen2)", "sd88me", "v1.0.3", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-04",
            "406", "7.7 MB", "14%",
            "sd88me - VST - Machinemodule", "machinedrum_one.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule-gen2.png", "https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.3/Machinemodule-1.0.3-mpc-aarch64.zip", "plugins/machinemodule-gen2/thumb.ppm",
            "The Elektron Machinedrum UW sound engine as an instrument for Gen2 MPC devices: 16 tracks in one plugin, touchscreen skin, bank and kit lists, Q-Links. A ready-made download with nothing of Elektron's in it: you add your own OS file (and optionally your flash image) in the plugin folder. Gen2 only.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule\" && if [ -f \"./plugins/machinemodule-gen2/machinedrum_one.so\" ]; then cp -rf ./plugins/machinemodule-gen2/* \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule/\"; else (wget -q -O /tmp/machinemodule-gen2.zip \"https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.3/Machinemodule-1.0.3-mpc-aarch64.zip\" || wget --no-check-certificate -q -O /tmp/machinemodule-gen2.zip \"https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.3/Machinemodule-1.0.3-mpc-aarch64.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.3/Machinemodule-1.0.3-mpc-aarch64.zip\" -o /tmp/machinemodule-gen2.zip) && unzip -o /tmp/machinemodule-gen2.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule/\" && rm -f /tmp/machinemodule-gen2.zip; fi && sync", 1,
            false,
            false,
            false,
            { "elektron", "machinedrum", "gen2" }
        },
        {
            "braids", "Macro Osc", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1849", "8.0 MB", "14%",
            "Émilie Gillet - VST - [SYN] Macro Osc", "braids.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-macro-osc/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-macro-osc/releases/download/v1.1.1/SYN-Macro-Osc-1.1.1-mpc-armv7.zip", "plugins/braids/thumb.ppm",
            "Macro Osc: a macro oscillator with 47 synthesis models, from Émilie Gillet's open-source Braids.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Macro Osc\" && if [ -f \"./plugins/braids/braids.so\" ]; then cp -rf ./plugins/braids/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Macro Osc/\"; else (wget -q -O /tmp/braids.zip \"https://github.com/saustin2010/mpc-vst-macro-osc/releases/download/v1.1.1/SYN-Macro-Osc-1.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/braids.zip \"https://github.com/saustin2010/mpc-vst-macro-osc/releases/download/v1.1.1/SYN-Macro-Osc-1.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-macro-osc/releases/download/v1.1.1/SYN-Macro-Osc-1.1.1-mpc-armv7.zip\" -o /tmp/braids.zip) && unzip -o /tmp/braids.zip -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Macro Osc/\" && rm -f /tmp/braids.zip; fi && sync", 1,
            false,
            false,
            false,
            { "macro-oscillator", "eurorack" }
        },
        {
            "maschine-group", "Maschine Group", "mmiroshnikov", "v1.0.0", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-13",
            "517", "169 KB", "14%",
            "mmiroshnikov - VST - Maschine Group", "maschine_group.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-maschine-group/main/docs/group.png", "https://github.com/mmiroshnikov/mpc-maschine-group/releases/download/maschine-group-v1.0.0/Maschine-Group-1.0.0-mpc-armv7.zip", "plugins/maschine-group/thumb.ppm",
            "Opens a Native Instruments Maschine 2 drum group (.mxgrp) and plays its pads and stored patterns, with a three-page touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/mmiroshnikov - VST - Maschine Group\" && if [ -f \"./plugins/maschine-group/maschine_group.so\" ]; then cp -rf ./plugins/maschine-group/* \"$TARGET_SYNTH_DIR/mmiroshnikov - VST - Maschine Group/\"; else (wget -q -O /tmp/maschine-group.zip \"https://github.com/mmiroshnikov/mpc-maschine-group/releases/download/maschine-group-v1.0.0/Maschine-Group-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/maschine-group.zip \"https://github.com/mmiroshnikov/mpc-maschine-group/releases/download/maschine-group-v1.0.0/Maschine-Group-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-maschine-group/releases/download/maschine-group-v1.0.0/Maschine-Group-1.0.0-mpc-armv7.zip\" -o /tmp/maschine-group.zip) && unzip -o /tmp/maschine-group.zip -d \"$TARGET_SYNTH_DIR/mmiroshnikov - VST - Maschine Group/\" && rm -f /tmp/maschine-group.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drums", "maschine", "sampler", "patterns" }
        },
        {
            "maze-voice", "Maze Voice", "sd88me", "v1.0.5", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "3495", "4.2 MB", "14%",
            "sd88me - VST - Maze Voice", "maze_voice.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-maze/main/docs/img/maze-voice.png", "https://github.com/sd88me/mpc-vst-maze/releases/download/maze-voice-vst-v1.0.5/Maze-Voice-1.0.5-mpc-armv7.zip", "plugins/maze-voice/thumb.ppm",
            "Monophonic Moog Labyrinth-style thru-zero oscillator, wavefolder and state-variable filter voice with two LFOs, a page randomiser and a three-page touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Voice\" && if [ -f \"./plugins/maze-voice/maze_voice.so\" ]; then cp -rf ./plugins/maze-voice/* \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Voice/\"; else (wget -q -O /tmp/maze-voice.zip \"https://github.com/sd88me/mpc-vst-maze/releases/download/maze-voice-vst-v1.0.5/Maze-Voice-1.0.5-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/maze-voice.zip \"https://github.com/sd88me/mpc-vst-maze/releases/download/maze-voice-vst-v1.0.5/Maze-Voice-1.0.5-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-maze/releases/download/maze-voice-vst-v1.0.5/Maze-Voice-1.0.5-mpc-armv7.zip\" -o /tmp/maze-voice.zip) && unzip -o /tmp/maze-voice.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Voice/\" && rm -f /tmp/maze-voice.zip; fi && sync", 1,
            false,
            false,
            false,
            { "monophonic", "wavefolder", "moog", "analog-modelling" }
        },
        {
            "meowsynth", "MeowSynth", "mcardonaserrano", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "1275", "4.5 MB", "14%",
            "knobster - VST - MeowSynth", "meowsynth.so",
            "https://raw.githubusercontent.com/mcardonaserrano/meowsynth-mpc/main/previews/meow_page_meowing.png", "https://github.com/mcardonaserrano/meowsynth-mpc/releases/download/v1.0.2/MeowSynth-1.0.2-mpc-armv7.zip", "plugins/meowsynth/thumb.ppm",
            "knobster's MeowSynth rebuilt natively from the original's SynthEdit patch: a ramp oscillator through an envelope-following resonant filter that meows, five cats, vibrato and chorus, with the original's look and a meowing cat on the touchscreen and Q-Links.", "./plugins/meowsynth/install.sh", 0,
            false,
            false,
            false,
            { "cat", "meow", "novelty", "synthedit" }
        },
        {
            "meta4mpcxxl", "Meta4MPCXXL", "L'Cronx", "v0.1.2", "Modular",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "397", "11.2 MB", "14%",
            "L'Cronx - VST - Meta4MPCXXL", "meta4mpcxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Meta4MPCXXL/v0.1.1/docs/img/meta4mpcxxl.png", "https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-0.1.2-mpc-armv7.zip", "plugins/meta4mpcxxl/thumb.ppm",
            "Plays 4ms MetaModule-format patches (VCV Rack patches exported with the MetaModule hub) with 4ms's own open-source patch player and 215 built-in modules: every parameter on Q-Link pages, MIDI and the Force's clock into the patch, pads and keys to the hub's gate/CV inputs, patch outs 3-8 as MIDI for a CV track, and Wi-Fi patch upload from VCV Rack.", "./plugins/meta4mpcxxl/install.sh", 0,
            false,
            false,
            false,
            { "modular", "vcv", "metamodule", "patch-player", "eurorack", "generative", "cv" }
        },
        {
            "jv-880", "Mini-JV", "sd88me", "v1.0.9", "Rompler",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 2.x & 3.x", "2026-11-01",
            "6058", "1.7 MB", "14%",
            "sd88me - VST - JV-880", "jv880.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-jv880/f24bd1c922f2e8b2cf56eb497c6e13465ef8dad3/docs/screenshots/jv880.png", "https://github.com/sd88me/mpc-vst-jv880/releases/download/v1.0.9/JV-880-1.0.9-mpc-armv7.zip", "plugins/jv-880/thumb.ppm",
            "An emulation of the Roland JV-880 sample-based sound engine as a native instrument, with the original reverb and chorus, a Play page with eight macros and a touchscreen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - JV-880\" && if [ -f \"./plugins/jv-880/jv880.so\" ]; then cp -rf ./plugins/jv-880/* \"$TARGET_SYNTH_DIR/sd88me - VST - JV-880/\"; else (wget -q -O /tmp/jv-880.zip \"https://github.com/sd88me/mpc-vst-jv880/releases/download/v1.0.9/JV-880-1.0.9-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/jv-880.zip \"https://github.com/sd88me/mpc-vst-jv880/releases/download/v1.0.9/JV-880-1.0.9-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-jv880/releases/download/v1.0.9/JV-880-1.0.9-mpc-armv7.zip\" -o /tmp/jv-880.zip) && unzip -o /tmp/jv-880.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - JV-880/\" && rm -f /tmp/jv-880.zip; fi && sync", 1,
            false,
            false,
            false,
            { "roland", "emulation" }
        },
        {
            "elements", "Modal Voice", "saustin2010", "v1.2.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1692", "1.2 MB", "14%",
            "Émilie Gillet - VST - [SYN] Modal Voice", "elements.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-modal-voice/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-modal-voice/releases/download/v1.2.1/SYN-Modal-Voice-1.2.1-mpc-armv7.zip", "plugins/elements/thumb.ppm",
            "Modal Voice: a modal synthesis voice you bow, blow and strike, with 12 presets, from Émilie Gillet's open-source Elements.", "./plugins/elements/install.sh", 0,
            false,
            false,
            false,
            { "modal", "physical-modelling", "eurorack" }
        },
        {
            "model-72", "Model-72", "sd88me", "v1.0.1", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "856", "31.6 MB", "14%",
            "sd88me - VST - Model-72", "model72.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-model72/main/docs/img/mixer_filter.png", "https://github.com/sd88me/mpc-vst-model72/releases/download/model72-v1.0.1/Model-72-1.0.1-mpc-armv7.zip", "plugins/model-72/thumb.ppm",
            "A port of Idle Foundry's CA-72, a circuit-modelled 1970s Minimoog Model-D: three oscillators, the ladder filter and contours, mono, poly or unison with stereo and 19 presets. LO is the fitted light model; HI runs the circuit's own filter in real time.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Model-72\" && if [ -f \"./plugins/model-72/model72.so\" ]; then cp -rf ./plugins/model-72/* \"$TARGET_SYNTH_DIR/sd88me - VST - Model-72/\"; else (wget -q -O /tmp/model-72.zip \"https://github.com/sd88me/mpc-vst-model72/releases/download/model72-v1.0.1/Model-72-1.0.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/model-72.zip \"https://github.com/sd88me/mpc-vst-model72/releases/download/model72-v1.0.1/Model-72-1.0.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-model72/releases/download/model72-v1.0.1/Model-72-1.0.1-mpc-armv7.zip\" -o /tmp/model-72.zip) && unzip -o /tmp/model-72.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Model-72/\" && rm -f /tmp/model-72.zip; fi && sync", 1,
            false,
            false,
            false,
            { "analog", "mono", "bass", "circuit-model", "stereo", "presets" }
        },
        {
            "monksynth", "MonkSynth", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1211", "1.6 MB", "14%",
            "Jonathan Taylor - VST - [SYN] MonkSynth", "monksynth.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-monksynth/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-monksynth/releases/download/v1.1.1/SYN-MonkSynth-1.1.1-mpc-armv7.zip", "plugins/monksynth/thumb.ppm",
            "A formant (FOF) singing voice with 12 characters and a choir.", "./plugins/monksynth/install.sh", 0,
            false,
            false,
            false,
            { "formant", "voice", "choir" }
        },
        {
            "mono-voice", "Mono Voice", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1378", "250 KB", "14%",
            "timncox - VST - [SYN] Mono Voice", "monovoice.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-monovoice/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-monovoice/releases/download/v1.0.2/SYN-Mono-Voice-1.0.2-mpc-armv7.zip", "plugins/mono-voice/thumb.ppm",
            "Elektron Monomachine-style digital voice: SuperWave, SID, DigiPRO, FM and more machines; 12 factory patches.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/timncox - VST - [SYN] Mono Voice\" && if [ -f \"./plugins/mono-voice/monovoice.so\" ]; then cp -rf ./plugins/mono-voice/* \"$TARGET_SYNTH_DIR/timncox - VST - [SYN] Mono Voice/\"; else (wget -q -O /tmp/mono-voice.zip \"https://github.com/saustin2010/mpc-vst-monovoice/releases/download/v1.0.2/SYN-Mono-Voice-1.0.2-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/mono-voice.zip \"https://github.com/saustin2010/mpc-vst-monovoice/releases/download/v1.0.2/SYN-Mono-Voice-1.0.2-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-monovoice/releases/download/v1.0.2/SYN-Mono-Voice-1.0.2-mpc-armv7.zip\" -o /tmp/mono-voice.zip) && unzip -o /tmp/mono-voice.zip -d \"$TARGET_SYNTH_DIR/timncox - VST - [SYN] Mono Voice/\" && rm -f /tmp/mono-voice.zip; fi && sync", 1,
            false,
            false,
            false,
            { "monomachine", "sid", "fm", "digital" }
        },
        {
            "monomodule-one-gen2", "Monomodule One (Gen2)", "sd88me", "v1.0.4", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-04",
            "264", "4.2 MB", "14%",
            "sd88me - VST - Monomodule One", "monomodule_one.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-monomodule/main/docs/img/monomodule-gen2.png", "https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-One-1.0.4-mpc-aarch64.zip", "plugins/monomodule-one-gen2/thumb.ppm",
            "The Elektron Monomachine's synth engine (15 machines) as an instrument for Gen2 MPC devices, with a touchscreen skin, banks and preset lists, and Q-Links. A ready-made download: nothing of Elektron's is in the zip; you put your own Monomachine OS file in the plugin folder and it makes its skin and sound from that. Gen2 (aarch64) only.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule One\" && if [ -f \"./plugins/monomodule-one-gen2/monomodule_one.so\" ]; then cp -rf ./plugins/monomodule-one-gen2/* \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule One/\"; else (wget -q -O /tmp/monomodule-one-gen2.zip \"https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-One-1.0.4-mpc-aarch64.zip\" || wget --no-check-certificate -q -O /tmp/monomodule-one-gen2.zip \"https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-One-1.0.4-mpc-aarch64.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-One-1.0.4-mpc-aarch64.zip\" -o /tmp/monomodule-one-gen2.zip) && unzip -o /tmp/monomodule-one-gen2.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule One/\" && rm -f /tmp/monomodule-one-gen2.zip; fi && sync", 1,
            false,
            false,
            false,
            { "elektron", "monomachine", "gen2" }
        },
        {
            "monomodule", "Monomodule One + FX", "sd88me", "v1.0.4", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "100", "2.0 MB", "14%",
            "sd88me - VST - Monomodule One + FX", "monomodule.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-monomodule/main/docs/img/monomodule.png", "", "plugins/monomodule/thumb.ppm",
            "The Elektron Monomachine's synth engine (15 machines) as an instrument, plus its 7 effect machines as an audio effect, each with a touchscreen skin and Q-Links.", "./plugins/monomodule/install.sh", 0,
            false,
            false,
            false,
            { "elektron", "monomachine", "firmware-derived" }
        },
        {
            "monoslide", "MonoSlide", "Eurikon", "v1.0.3", "Synth",
            "INST", "Stable", "MPC Live II Retro (3.9.1), MPC Live II Retro (3.10)", "MPC OS 3.x", "2026-10-27",
            "245", "4.5 MB", "11%",
            "Twisted Cable - VST - MonoSlide", "monoslide.so",
            "https://raw.githubusercontent.com/Eurikon/mpc-vst-plugins/0b9a58444c7d84a78e38d95818eeb53172195782/media/monoslide.png", "https://github.com/Eurikon/mpc-vst-plugins/releases/download/monoslide-v1.0.3/MonoSlide-1.0.3-mpc-armv7.zip", "plugins/monoslide/thumb.ppm",
            "Monophonic acid bass voice with overlapping-note slide, velocity accents and an MPC touchscreen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Twisted Cable - VST - MonoSlide\" && if [ -f \"./plugins/monoslide/monoslide.so\" ]; then cp -rf ./plugins/monoslide/* \"$TARGET_SYNTH_DIR/Twisted Cable - VST - MonoSlide/\"; else (wget -q -O /tmp/monoslide.zip \"https://github.com/Eurikon/mpc-vst-plugins/releases/download/monoslide-v1.0.3/MonoSlide-1.0.3-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/monoslide.zip \"https://github.com/Eurikon/mpc-vst-plugins/releases/download/monoslide-v1.0.3/MonoSlide-1.0.3-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Eurikon/mpc-vst-plugins/releases/download/monoslide-v1.0.3/MonoSlide-1.0.3-mpc-armv7.zip\" -o /tmp/monoslide.zip) && unzip -o /tmp/monoslide.zip -d \"$TARGET_SYNTH_DIR/Twisted Cable - VST - MonoSlide/\" && rm -f /tmp/monoslide.zip; fi && sync", 1,
            false,
            false,
            false,
            { "303", "acid", "bass", "monophonic" }
        },
        {
            "morpho-pe", "Morpho-PE", "sd88me", "v1.0.2", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "1568", "2.2 MB", "14%",
            "sd88me - VST - Morpho-PE", "morpho_pe.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-morpho-PE/main/docs/img/morpho-pe.png", "https://github.com/sd88me/mpc-vst-morpho-PE/releases/download/morpho-pe-v1.0.2/Morpho-PE-1.0.2-mpc-armv7.zip", "plugins/morpho-pe/thumb.ppm",
            "Four-voice synthesizer modelled on the Poly Evolver's voice: analog-style and digital waveshape oscillators, 2/4-pole lowpass, delay, feedback, distortion, 4 x 16 step sequencer, a bank browser and a touchscreen skin with Q-Links. Loads Poly Evolver program dumps and, from your own Prophet VS ROM chip images, the 95 factory waves.", "./plugins/morpho-pe/install.sh", 0,
            false,
            false,
            false,
            { "analog-modelling", "polyphonic", "sequencer", "evolver" }
        },
        {
            "mpc-plaits", "MPC Plaits", "poloq", "v1.0.1", "Synth",
            "INST", "Stable", "Akai MPC One (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-05",
            "2085", "5.5 MB", "24%",
            "poloq - VST - MPC Plaits", "plaits.so",
            "https://raw.githubusercontent.com/poloq-instruments/mpc-vst-plaits/a30c24b83663a5efead73bbaca674ba4502a3eb5/docs/screenshots/card.png", "https://github.com/poloq-instruments/mpc-vst-plaits/releases/download/v1.0.1/MPC-Plaits-1.0.1-mpc-armv7.zip", "plugins/mpc-plaits/thumb.ppm",
            "Mutable Instruments Plaits, all 24 models, with LFOs, envelopes, two mod matrices, tempo sync and a touchscreen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/poloq - VST - MPC Plaits\" && if [ -f \"./plugins/mpc-plaits/plaits.so\" ]; then cp -rf ./plugins/mpc-plaits/* \"$TARGET_SYNTH_DIR/poloq - VST - MPC Plaits/\"; else (wget -q -O /tmp/mpc-plaits.zip \"https://github.com/poloq-instruments/mpc-vst-plaits/releases/download/v1.0.1/MPC-Plaits-1.0.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/mpc-plaits.zip \"https://github.com/poloq-instruments/mpc-vst-plaits/releases/download/v1.0.1/MPC-Plaits-1.0.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/poloq-instruments/mpc-vst-plaits/releases/download/v1.0.1/MPC-Plaits-1.0.1-mpc-armv7.zip\" -o /tmp/mpc-plaits.zip) && unzip -o /tmp/mpc-plaits.zip -d \"$TARGET_SYNTH_DIR/poloq - VST - MPC Plaits/\" && rm -f /tmp/mpc-plaits.zip; fi && sync", 1,
            false,
            false,
            false,
            { "mutable-instruments", "plaits", "macro-oscillator", "eurorack" }
        },
        {
            "mr-drums", "Mr Drums", "saustin2010", "v1.1.1", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1111", "464 KB", "14%",
            "Move Everything - VST - [DRUM] Mr Drums", "mrdrums.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-mrdrums/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-mrdrums/releases/download/v1.1.1/DRUM-Mr-Drums-1.1.1-mpc-armv7.zip", "plugins/mr-drums/thumb.ppm",
            "A 16-pad drum sampler: kits of samples on pads 36-51.", "./plugins/mr-drums/install.sh", 0,
            false,
            false,
            false,
            { "drums", "kits", "pads" }
        },
        {
            "mr-hyde", "Mr Hyde", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1588", "1.9 MB", "14%",
            "Move Everything - VST - [SYN] Mr Hyde", "mrhyde.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-mrhyde/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-mrhyde/releases/download/v1.1.1/SYN-Mr-Hyde-1.1.1-mpc-armv7.zip", "plugins/mr-hyde/thumb.ppm",
            "MicroFreak-style voice on Plaits' 17 models, with a low-pass gate, filter, cycling envelope and a 6x6 mod matrix; 19 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Move Everything - VST - [SYN] Mr Hyde\" && if [ -f \"./plugins/mr-hyde/mrhyde.so\" ]; then cp -rf ./plugins/mr-hyde/* \"$TARGET_SYNTH_DIR/Move Everything - VST - [SYN] Mr Hyde/\"; else (wget -q -O /tmp/mr-hyde.zip \"https://github.com/saustin2010/mpc-vst-mrhyde/releases/download/v1.1.1/SYN-Mr-Hyde-1.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/mr-hyde.zip \"https://github.com/saustin2010/mpc-vst-mrhyde/releases/download/v1.1.1/SYN-Mr-Hyde-1.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-mrhyde/releases/download/v1.1.1/SYN-Mr-Hyde-1.1.1-mpc-armv7.zip\" -o /tmp/mr-hyde.zip) && unzip -o /tmp/mr-hyde.zip -d \"$TARGET_SYNTH_DIR/Move Everything - VST - [SYN] Mr Hyde/\" && rm -f /tmp/mr-hyde.zip; fi && sync", 1,
            false,
            false,
            false,
            { "microfreak", "macro-oscillator", "mod-matrix", "low-pass-gate" }
        },
        {
            "ms20xxl", "MS20XXL", "L'Cronx", "v0.1.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-27",
            "655", "19.3 MB", "14%",
            "L'Cronx - VST - MS20XXL", "ms20xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/MS20XXL/v0.1.1/docs/img/patch.png", "https://github.com/sunskiefer/MS20XXL/releases/download/v0.1.2/MS20XXL-0.1.2-mpc-armv7.zip", "plugins/ms20xxl/thumb.ppm",
            "Korg MS-20-style semi-modular mono synth, circuit-modelled from the MS-20 schematics: the KLM-307 (MK2) and KORG35 (MK1) filter boards solved from their circuit equations, Hz/V sockets, the panel's voltage laws. Every MS-20 patch jack in its place, normalled like the hardware, patched by touch; an SQ-10-style sequencer; 63 patches with cables.", "./plugins/ms20xxl/install.sh", 0,
            false,
            false,
            false,
            { "ms-20", "semi-modular", "mono", "patch-panel", "sequencer", "sq-10", "korg35", "analog" }
        },
        {
            "mutable-vibe", "Mutable Vibe MPC", "nachtaktiv303", "v0.9.3", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2365", "1.1 MB", "14%",
            "nachtaktiv303 - VST - Mutable Vibe MPC", "mutable_vibe_mpc.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-mutable-vibe/main/preview_0.png", "https://github.com/nachtaktiv303/mpc-vst-mutable-vibe/releases/download/mutable-vibe-vst-v0.9.3/Mutable-Vibe-MPC-0.9.3-mpc-armv7.zip", "plugins/mutable-vibe/thumb.ppm",
            "Mutable Instruments Rings + Plaits in one synth: 6 resonator models and 14 melodic macro-oscillator engines, with filter, FX, 4 envelopes, 4 LFOs, a mod matrix and tempo sync, plus a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Mutable Vibe MPC\" && if [ -f \"./plugins/mutable-vibe/mutable_vibe_mpc.so\" ]; then cp -rf ./plugins/mutable-vibe/* \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Mutable Vibe MPC/\"; else (wget -q -O /tmp/mutable-vibe.zip \"https://github.com/nachtaktiv303/mpc-vst-mutable-vibe/releases/download/mutable-vibe-vst-v0.9.3/Mutable-Vibe-MPC-0.9.3-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/mutable-vibe.zip \"https://github.com/nachtaktiv303/mpc-vst-mutable-vibe/releases/download/mutable-vibe-vst-v0.9.3/Mutable-Vibe-MPC-0.9.3-mpc-armv7.zip\" || curl -k -sL \"https://github.com/nachtaktiv303/mpc-vst-mutable-vibe/releases/download/mutable-vibe-vst-v0.9.3/Mutable-Vibe-MPC-0.9.3-mpc-armv7.zip\" -o /tmp/mutable-vibe.zip) && unzip -o /tmp/mutable-vibe.zip -d \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Mutable Vibe MPC/\" && rm -f /tmp/mutable-vibe.zip; fi && sync", 1,
            false,
            false,
            false,
            { "mutable-instruments", "rings", "plaits", "resonator", "macro-oscillator", "eurorack" }
        },
        {
            "noisemaker", "Noisemaker", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1170", "5.3 MB", "14%",
            "TAL - VST - [SYN] Noisemaker", "noisemaker.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-noisemaker/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-noisemaker/releases/download/v1.1.1/SYN-Noisemaker-1.1.1-mpc-armv7.zip", "plugins/noisemaker/thumb.ppm",
            "TAL-NoiseMaker: a virtual-analog polysynth with 12 filter types, three envelopes and effects; all 256 factory presets built in, and folders of .noisemakerpreset files load as banks (the README links a free collection of 442).", "./plugins/noisemaker/install.sh", 0,
            false,
            false,
            false,
            { "tal", "virtual-analog", "polyphonic", "subtractive" }
        },
        {
            "nusaw", "NuSaw", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1287", "2.2 MB", "14%",
            "Charles Vestal - VST - [SYN] NuSaw", "nusaw.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-nusaw/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-nusaw/releases/download/v1.0.2/SYN-NuSaw-1.0.2-mpc-armv7.zip", "plugins/nusaw/thumb.ppm",
            "Detuned supersaw polysynth with a sub, resonant filter, chorus and delay; 27 presets for pads, stabs and leads.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] NuSaw\" && if [ -f \"./plugins/nusaw/nusaw.so\" ]; then cp -rf ./plugins/nusaw/* \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] NuSaw/\"; else (wget -q -O /tmp/nusaw.zip \"https://github.com/saustin2010/mpc-vst-nusaw/releases/download/v1.0.2/SYN-NuSaw-1.0.2-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/nusaw.zip \"https://github.com/saustin2010/mpc-vst-nusaw/releases/download/v1.0.2/SYN-NuSaw-1.0.2-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-nusaw/releases/download/v1.0.2/SYN-NuSaw-1.0.2-mpc-armv7.zip\" -o /tmp/nusaw.zip) && unzip -o /tmp/nusaw.zip -d \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] NuSaw/\" && rm -f /tmp/nusaw.zip; fi && sync", 1,
            false,
            false,
            false,
            { "supersaw", "trance", "polyphonic", "pads" }
        },
        {
            "obxd", "OB-Xd", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1503", "1.5 MB", "14%",
            "reales - VST - [SYN] OB-Xd", "obxd.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-obxd/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-obxd/releases/download/v1.1.1/SYN-OB-Xd-1.1.1-mpc-armv7.zip", "plugins/obxd/thumb.ppm",
            "OB-Xd, the Oberheim OB-X emulation: two oscillators with sync and cross-mod, a 12/24 dB filter, voice variation; 128 presets.", "./plugins/obxd/install.sh", 0,
            false,
            false,
            false,
            { "ob-x", "analog-modelling", "polyphonic", "subtractive" }
        },
        {
            "obxdmpc", "OB-Xd MPC", "3dthc", "v0.4.5", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "1424", "10.7 MB", "14%",
            "sd88me - VST - OB-Xd", "obxd.so",
            "https://raw.githubusercontent.com/3dthc/OB-XD-MPC/master/docs/screenshots/obxd-mpc-black.png", "https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.5/OB-Xd-0.4.5-mpc-armv7.zip", "plugins/obxdmpc/thumb.ppm",
            "OB-Xd, the Oberheim OB-X emulation: two oscillators with sync and cross-mod, a 12/24 dB filter, voice variation; 128 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - OB-Xd\" && if [ -f \"./plugins/obxdmpc/obxd.so\" ]; then cp -rf ./plugins/obxdmpc/* \"$TARGET_SYNTH_DIR/sd88me - VST - OB-Xd/\"; else (wget -q -O /tmp/obxdmpc.zip \"https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.5/OB-Xd-0.4.5-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/obxdmpc.zip \"https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.5/OB-Xd-0.4.5-mpc-armv7.zip\" || curl -k -sL \"https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.5/OB-Xd-0.4.5-mpc-armv7.zip\" -o /tmp/obxdmpc.zip) && unzip -o /tmp/obxdmpc.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - OB-Xd/\" && rm -f /tmp/obxdmpc.zip; fi && sync", 1,
            false,
            false,
            false,
            { "ob-x", "analog-modelling", "polyphonic", "subtractive" }
        },
        {
            "odin2", "Odin 2", "3dthc", "v1.4.6", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "1326", "17.6 MB", "14%",
            "3dthc - VST - Odin 2", "odin2.so",
            "https://raw.githubusercontent.com/3dthc/Odin2-MPC/main/odin2-mpc-main.png", "https://github.com/3dthc/Odin2-MPC/releases/download/odin2-v1.4.6/Odin-2-1.4.6-mpc-armv7.zip", "plugins/odin2/thumb.ppm",
            "Odin 2 synthesizer ported to standalone Akai MPC devices as a native VST2 plugin. Features Odin's original engine, 261 factory patches, touchscreen pages, automatic voice management and a CPU meter. Tested on an Akai MPC Live II; unofficial port.", "./plugins/odin2/install.sh", 0,
            false,
            false,
            false,
            {  }
        },
        {
            "omni-sampler", "Omni Sampler", "MrHighMan", "v1.8.1", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-02",
            "2796", "5.0 MB", "20%",
            "MR HighMan - VST - Omni Sampler", "omni_sampler.so",
            "https://raw.githubusercontent.com/B0ss-M/MrHighMan/main/docs/screenshot.png", "https://github.com/B0ss-M/MrHighMan/releases/download/v1.8.1/Omni-Sampler-1.8.1-mpc-armv7.zip", "plugins/omni-sampler/thumb.ppm",
            "Multi-format sampler: plays Akai, E-mu, Roland, Kontakt/Maschine, SF2 and SFZ instruments straight from their files or sampler disk images, with layers, keyswitching, a mod matrix and its own screen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MR HighMan - VST - Omni Sampler\" && if [ -f \"./plugins/omni-sampler/omni_sampler.so\" ]; then cp -rf ./plugins/omni-sampler/* \"$TARGET_SYNTH_DIR/MR HighMan - VST - Omni Sampler/\"; else (wget -q -O /tmp/omni-sampler.zip \"https://github.com/B0ss-M/MrHighMan/releases/download/v1.8.1/Omni-Sampler-1.8.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/omni-sampler.zip \"https://github.com/B0ss-M/MrHighMan/releases/download/v1.8.1/Omni-Sampler-1.8.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/B0ss-M/MrHighMan/releases/download/v1.8.1/Omni-Sampler-1.8.1-mpc-armv7.zip\" -o /tmp/omni-sampler.zip) && unzip -o /tmp/omni-sampler.zip -d \"$TARGET_SYNTH_DIR/MR HighMan - VST - Omni Sampler/\" && rm -f /tmp/omni-sampler.zip; fi && sync", 1,
            false,
            false,
            false,
            { "akai", "emu", "roland", "kontakt", "sf2", "sfz", "disk-images" }
        },
        {
            "plugin-manager-g2", "Open MPC Plugin Manager G2", "sd88me", "v0.2.5", "Utility",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-31",
            "843", "18.6 MB", "14%",
            "OpenMPC - VST - Plugin Manager G2", "openmpc_pm_g2.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/588466d/tools/plugin_manager_g2/docs/screenshots/manager-discover-light.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/plugin-manager-g2-v0.2.5/Plugin-Manager-G2-0.2.5-mpc-armv7.zip", "plugins/plugin-manager-g2/thumb.ppm",
            "The official OpenMPC plugin manager for Gen1 and Gen2 MPCs (32-bit build for Force, Live, One, X, Key; 64-bit for the Live III and kin). Browse this catalog and install, update and remove plugins on the MPC itself, with rollback if an install fails. Built on poloq's mpc-vst-manager.", "./plugins/plugin-manager-g2/install.sh", 0,
            false,
            false,
            false,
            { "installer", "catalog", "manager", "gen1", "gen2", "utility" }
        },
        {
            "open303", "Open303", "saustin2010", "v1.2.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1999", "2.0 MB", "14%",
            "Robin Schmidt - VST - [SYN] Open303", "acid303.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-open303/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-open303/releases/download/v1.2.1/SYN-Open303-1.2.1-mpc-armv7.zip", "plugins/open303/thumb.ppm",
            "TB-303 bass line: Open303 with the Devilfish mods, a drive stage and 13 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Robin Schmidt - VST - [SYN] Open303\" && if [ -f \"./plugins/open303/acid303.so\" ]; then cp -rf ./plugins/open303/* \"$TARGET_SYNTH_DIR/Robin Schmidt - VST - [SYN] Open303/\"; else (wget -q -O /tmp/open303.zip \"https://github.com/saustin2010/mpc-vst-open303/releases/download/v1.2.1/SYN-Open303-1.2.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/open303.zip \"https://github.com/saustin2010/mpc-vst-open303/releases/download/v1.2.1/SYN-Open303-1.2.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-open303/releases/download/v1.2.1/SYN-Open303-1.2.1-mpc-armv7.zip\" -o /tmp/open303.zip) && unzip -o /tmp/open303.zip -d \"$TARGET_SYNTH_DIR/Robin Schmidt - VST - [SYN] Open303/\" && rm -f /tmp/open303.zip; fi && sync", 1,
            false,
            false,
            false,
            { "tb-303", "acid", "bass", "monophonic" }
        },
        {
            "osirus", "Osirus", "sd88me", "v0.2.10", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-03",
            "681", "4.3 MB", "14%",
            "sd88me - VST - Osirus", "osirus.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-osirus/e39d03c538f4fde79d558cd4c423b18964cc5ffc/docs/screenshots/osirus-osc.png", "https://github.com/sd88me/mpc-vst-osirus/releases/download/v0.2.10/Osirus-0.2.10-mpc-aarch64.zip", "plugins/osirus/thumb.ppm",
            "An emulation of the Access Virus A, B and C synthesizers' original firmware (gearmulator's DSP56300 emulator) as a native instrument for Gen2 MPC devices, with a touchscreen skin, banks and patch browser, Q-Link control and knob values on every page. Gen2 (aarch64) only; you need your own Virus ROM.", "./plugins/osirus/install.sh", 0,
            false,
            false,
            false,
            { "emulation", "virtual-analog", "rompler-free", "gen2" }
        },
        {
            "percolator", "Percolator", "saustin2010", "v2.1.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2363", "6.6 MB", "14%",
            "Steve A - VST - [DRUM] Percolator", "percolator.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-percolator/v2.1.0/screenshots/page_1.png", "https://github.com/saustin2010/mpc-vst-percolator/releases/download/v2.1.0/DRUM-Percolator-2.1.0-mpc-armv7.zip", "plugins/percolator/thumb.ppm",
            "Four-voice percussion synth laid out like the Erica Synths Pērkons HD-01 (three algorithms per voice, BBD delay, LFO to every knob), with envelope displays; each voice on its own MIDI channel with the Pērkons' CCs, and on its own track with Percolator OUT; plays the Pērkons' free kit packs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Percolator\" && if [ -f \"./plugins/percolator/percolator.so\" ]; then cp -rf ./plugins/percolator/* \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Percolator/\"; else (wget -q -O /tmp/percolator.zip \"https://github.com/saustin2010/mpc-vst-percolator/releases/download/v2.1.0/DRUM-Percolator-2.1.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/percolator.zip \"https://github.com/saustin2010/mpc-vst-percolator/releases/download/v2.1.0/DRUM-Percolator-2.1.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-percolator/releases/download/v2.1.0/DRUM-Percolator-2.1.0-mpc-armv7.zip\" -o /tmp/percolator.zip) && unzip -o /tmp/percolator.zip -d \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Percolator/\" && rm -f /tmp/percolator.zip; fi && sync", 1,
            false,
            false,
            false,
            { "drums", "percussion", "kits", "drone", "envelope-display" }
        },
        {
            "op1fx-phase", "Phase", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "84", "7.5 MB", "14%",
            "bombest - VST - Phase", "phase.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/phase.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phase-0.4.1-mpc-armv7.zip", "plugins/op1fx-phase/thumb.ppm",
            "Eight-voice phase-distortion synth, designed from the OP-1 screen.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Phase\" && if [ -f \"./plugins/op1fx-phase/phase.so\" ]; then cp -rf ./plugins/op1fx-phase/* \"$TARGET_SYNTH_DIR/bombest - VST - Phase/\"; else (wget -q -O /tmp/op1fx-phase.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phase-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-phase.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phase-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phase-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-phase.zip) && unzip -o /tmp/op1fx-phase.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Phase/\" && rm -f /tmp/op1fx-phase.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "synth", "phase-distortion" }
        },
        {
            "polywerk", "PolyWerk", "Devko", "v0.0.6", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "195", "6.3 MB", "14%",
            "Devko - VST - PolyWerk", "polywerk.so",
            "https://raw.githubusercontent.com/Devko/PolyWerk/main/docs/img/waves.png", "https://github.com/Devko/PolyWerk/releases/download/v0.0.6/PolyWerk-0.0.6-mpc-armv7.zip", "plugins/polywerk/thumb.ppm",
            "Eight-voice wavetable synth: two wavetable oscillators with 8x unison and subs, 30 built-in tables plus your own Serum-format WAVs, two filters with comb and vowel, two LFOs, a 12-slot mod matrix, arpeggiator and sequencers, 205 presets and a seven-tab touchscreen skin with Q-Links.", "./plugins/polywerk/install.sh", 0,
            false,
            false,
            false,
            { "wavetable", "polyphonic", "arpeggiator", "sequencer", "mod-matrix" }
        },
        {
            "potentialbreaks", "PotentialBreaks", "PotentialBreaks", "v1.1.1", "Sampler",
            "INST", "Stable", "MPC One (HAKAI 3.10)", "MPC OS 3.x", "2026-11-05",
            "4", "230 KB", "14%",
            "PotentialBreaks - VST - PotentialBreaks", "potentialbreaks.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialbreaks/0a21900bf9b95201f10e0b4106a5303e6ce3d60e/docs/images/potentialbreaks.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialbreaks/releases/download/v1.1.1/PotentialBreaks-1.1.1-mpc-armv7.zip", "plugins/potentialbreaks/thumb.ppm",
            "Drum-break slicer: picks a random break from your own break folders (WAV or FLAC), chops it to 8 or 16 slices or by hits across the pads and matches it to the project tempo, by re-pitching or stretching. Shuffle the slices and save the groove as MIDI.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialBreaks\" && if [ -f \"./plugins/potentialbreaks/potentialbreaks.so\" ]; then cp -rf ./plugins/potentialbreaks/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialBreaks/\"; else (wget -q -O /tmp/potentialbreaks.zip \"https://github.com/PotentialBreakfast/mpc-vst-potentialbreaks/releases/download/v1.1.1/PotentialBreaks-1.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/potentialbreaks.zip \"https://github.com/PotentialBreakfast/mpc-vst-potentialbreaks/releases/download/v1.1.1/PotentialBreaks-1.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/PotentialBreakfast/mpc-vst-potentialbreaks/releases/download/v1.1.1/PotentialBreaks-1.1.1-mpc-armv7.zip\" -o /tmp/potentialbreaks.zip) && unzip -o /tmp/potentialbreaks.zip -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialBreaks/\" && rm -f /tmp/potentialbreaks.zip; fi && sync", 1,
            false,
            false,
            false,
            { "breaks", "slicer", "drums", "chop" }
        },
        {
            "prloguexxl", "PrLogueXXL", "L'Cronx", "v0.1.1", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-23",
            "475", "18.0 MB", "14%",
            "L'Cronx - VST - PrLogueXXL", "prloguexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PrLogueXXL/v0.1.1/docs/img/osc.png", "https://github.com/sunskiefer/PrLogueXXL/releases/download/v0.1.1/PrLogueXXL-0.1.1-mpc-armv7.zip", "plugins/prloguexxl/thumb.ppm",
            "16-voice poly synth modelled on the KORG prologue-16 (not affiliated with KORG): two VCOs, a multi engine (noise, 16 VPM types, KORG's logue-sdk waves oscillator), Poly/Duo/Mono/Unison/Chord, Main and Sub timbres, arpeggiator on MPC's grid, mod FX, delay/reverb and a low-frequency compressor with a live meter, over four pages.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - PrLogueXXL\" && if [ -f \"./plugins/prloguexxl/prloguexxl.so\" ]; then cp -rf ./plugins/prloguexxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - PrLogueXXL/\"; else (wget -q -O /tmp/prloguexxl.zip \"https://github.com/sunskiefer/PrLogueXXL/releases/download/v0.1.1/PrLogueXXL-0.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/prloguexxl.zip \"https://github.com/sunskiefer/PrLogueXXL/releases/download/v0.1.1/PrLogueXXL-0.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/PrLogueXXL/releases/download/v0.1.1/PrLogueXXL-0.1.1-mpc-armv7.zip\" -o /tmp/prloguexxl.zip) && unzip -o /tmp/prloguexxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - PrLogueXXL/\" && rm -f /tmp/prloguexxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "synth", "poly", "analog-modelled", "vpm", "fm", "arpeggiator", "pad", "lead" }
        },
        {
            "profit-08", "Profit-08", "sd88me", "v0.1.3", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "1951", "4.5 MB", "14%",
            "sd88me - VST - Profit-08", "profit_08.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-profit8/main/docs/img/screenshot-program.png", "https://github.com/sd88me/mpc-vst-profit8/releases/download/profit-08-v0.1.3/Profit-08-0.1.3-mpc-armv7.zip", "plugins/profit-08/thumb.ppm",
            "Eight-voice, two-layer analog-style synthesizer: stack and split layers, a gated sequencer and arpeggiator, a resonant 2/4-pole filter and a banks browser for your own program and bank SysEx dumps.", "./plugins/profit-08/install.sh", 0,
            false,
            false,
            false,
            { "analog", "poly", "layers", "sequencer" }
        },
        {
            "profit-5", "Profit-5", "sd88me", "v0.2.1", "Synth",
            "INST", "Stable", "Akai MPC Live III (Gen2) (3.10)", "MPC OS 3.x", "2026-10-28",
            "891", "20.4 MB", "31%",
            "sd88me - VST - Profit-5", "profit_5.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-profit5/5724d1fe8fdb0aab2c72837228a7fd77f1980ec1/docs/img/screenshot-osc.png", "https://github.com/sd88me/mpc-vst-profit5/releases/download/profit-5-vst-v0.2.1/Profit-5-0.2.1-mpc-armv7.zip", "plugins/profit-5/thumb.ppm",
            "Five-voice analog polysynth modelled on an early SSM-chip classic: two oscillators, a four-pole resonant filter, Poly-Mod, wheel-mod, glide and unison, 38 factory programs, a Rev 2 / Rev 4 voicing switch, and an importer for Repro-5 .h2p patches.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-5\" && if [ -f \"./plugins/profit-5/profit_5.so\" ]; then cp -rf ./plugins/profit-5/* \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-5/\"; else (wget -q -O /tmp/profit-5.zip \"https://github.com/sd88me/mpc-vst-profit5/releases/download/profit-5-vst-v0.2.1/Profit-5-0.2.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/profit-5.zip \"https://github.com/sd88me/mpc-vst-profit5/releases/download/profit-5-vst-v0.2.1/Profit-5-0.2.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-profit5/releases/download/profit-5-vst-v0.2.1/Profit-5-0.2.1-mpc-armv7.zip\" -o /tmp/profit-5.zip) && unzip -o /tmp/profit-5.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-5/\" && rm -f /tmp/profit-5.zip; fi && sync", 1,
            false,
            false,
            false,
            { "analog", "poly", "vintage", "poly-mod" }
        },
        {
            "ps3100xxl", "PS3100XXL", "L'Cronx", "v0.1.5", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-23",
            "550", "15.9 MB", "14%",
            "L'Cronx - VST - PS3100XXL", "ps3100xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PS3100XXL/v0.1.5/docs/img/overview.png", "https://github.com/sunskiefer/PS3100XXL/releases/download/v0.1.5/PS3100XXL-0.1.5-mpc-armv7.zip", "plugins/ps3100xxl/thumb.ppm",
            "Semi-modular poly synth modelled on the 1977 Korg PS-3100 (not affiliated with Korg): one oscillator, filter and envelope per key up to 48 voices, three-peak resonators, ensemble, two modulation generators, sample & hold, twelve temperament knobs and a patch bay you patch by touch, on one zoomable page.", "./plugins/ps3100xxl/install.sh", 0,
            false,
            false,
            false,
            { "synth", "poly", "semi-modular", "analog-modelled", "patch-bay", "ensemble", "resonator" }
        },
        {
            "op1fx-pulse", "Pulse", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "80", "5.1 MB", "14%",
            "bombest - VST - Pulse", "pulse.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/pulse.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Pulse-0.4.1-mpc-armv7.zip", "plugins/op1fx-pulse/thumb.ppm",
            "Two PWM pulse oscillators per voice, eight voices. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Pulse\" && if [ -f \"./plugins/op1fx-pulse/pulse.so\" ]; then cp -rf ./plugins/op1fx-pulse/* \"$TARGET_SYNTH_DIR/bombest - VST - Pulse/\"; else (wget -q -O /tmp/op1fx-pulse.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Pulse-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-pulse.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Pulse-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Pulse-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-pulse.zip) && unzip -o /tmp/op1fx-pulse.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Pulse/\" && rm -f /tmp/op1fx-pulse.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "synth", "pwm", "pulse" }
        },
        {
            "raffosynth", "RaffoSynth", "saustin2010", "v1.3.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1845", "6.4 MB", "14%",
            "Raffo - VST - [SYN] RaffoSynth", "moog.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-raffosynth/v1.3.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-raffosynth/releases/download/v1.3.1/SYN-RaffoSynth-1.3.1-mpc-armv7.zip", "plugins/raffosynth/thumb.ppm",
            "RaffoSynth: a Minimoog-style mono synth with four oscillators and a ladder filter.", "./plugins/raffosynth/install.sh", 0,
            false,
            false,
            false,
            { "minimoog", "monophonic", "ladder-filter", "analog-modelling" }
        },
        {
            "rings", "Resonator", "saustin2010", "v1.2.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1853", "4.1 MB", "14%",
            "Émilie Gillet - VST - [SYN] Resonator", "rings.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-resonator/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-resonator/releases/download/v1.2.1/SYN-Resonator-1.2.1-mpc-armv7.zip", "plugins/rings/thumb.ppm",
            "Resonator: seven resonator models (strings, modal, FM, chords) strummed by the notes you play; 14 presets, from Émilie Gillet's open-source Rings.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Resonator\" && if [ -f \"./plugins/rings/rings.so\" ]; then cp -rf ./plugins/rings/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Resonator/\"; else (wget -q -O /tmp/rings.zip \"https://github.com/saustin2010/mpc-vst-resonator/releases/download/v1.2.1/SYN-Resonator-1.2.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/rings.zip \"https://github.com/saustin2010/mpc-vst-resonator/releases/download/v1.2.1/SYN-Resonator-1.2.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-resonator/releases/download/v1.2.1/SYN-Resonator-1.2.1-mpc-armv7.zip\" -o /tmp/rings.zip) && unzip -o /tmp/rings.zip -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Resonator/\" && rm -f /tmp/rings.zip; fi && sync", 1,
            false,
            false,
            false,
            { "resonator", "physical-modelling", "eurorack" }
        },
        {
            "setbfree-organ", "setBfree Organ", "SamplingHigh", "v0.3.0", "Organ",
            "INST", "Stable", "Akai Force (3.9.1.2)", "MPC OS 3.x", "2026-10-24",
            "982", "2.2 MB", "8120%",
            "Force Runtime - VST - setBfree Organ", "setbfree.so",
            "https://raw.githubusercontent.com/SamplingHigh/mpc-vst-setbfree/v0.3.0/docs/images/setbfree-organ.png", "https://github.com/SamplingHigh/mpc-vst-setbfree/releases/download/v0.3.0/setBfree-Organ-0.3.0-mpc-armv7.zip", "plugins/setbfree-organ/thumb.ppm",
            "Tonewheel organ with drawbars, percussion, scanner, editable Leslie and keyboard splits.", "./plugins/setbfree-organ/install.sh", 0,
            false,
            false,
            false,
            { "tonewheel", "drawbar", "rotary" }
        },
        {
            "sfizz", "sfizz", "gmorb (MPC port); sfizz by sfztools", "v1.1.1", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "619", "1.4 MB", "14%",
            "sfztools - VST - sfizz", "sfizz_mpc.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-sfizz/6d9e627281779f087898521cd27caabf9b7a75f2/docs/screen.png", "https://github.com/gmorb/mpc-vst-sfizz/releases/download/v1.1.1/sfizz-1.1.1-mpc-armv7.zip", "plugins/sfizz/thumb.ppm",
            "The sfizz 1.2.3 sampler: plays SFZ libraries and SoundFont 2 (.sf2) files from SD cards and USB drives, streamed from disk, with key switches, the instrument's own CC controls, Scala tuning, mono/legato with glide and up to Sinc 72 interpolation, on a one-page screen modelled on sfizz's editor. Gen1 and Gen2 builds.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sfztools - VST - sfizz\" && if [ -f \"./plugins/sfizz/sfizz_mpc.so\" ]; then cp -rf ./plugins/sfizz/* \"$TARGET_SYNTH_DIR/sfztools - VST - sfizz/\"; else (wget -q -O /tmp/sfizz.zip \"https://github.com/gmorb/mpc-vst-sfizz/releases/download/v1.1.1/sfizz-1.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/sfizz.zip \"https://github.com/gmorb/mpc-vst-sfizz/releases/download/v1.1.1/sfizz-1.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/gmorb/mpc-vst-sfizz/releases/download/v1.1.1/sfizz-1.1.1-mpc-armv7.zip\" -o /tmp/sfizz.zip) && unzip -o /tmp/sfizz.zip -d \"$TARGET_SYNTH_DIR/sfztools - VST - sfizz/\" && rm -f /tmp/sfizz.zip; fi && sync", 1,
            false,
            false,
            false,
            { "sfz", "sf2", "soundfont", "sampler", "piano", "keyswitch", "streaming", "scala" }
        },
        {
            "sonix", "Sonix", "3dthc", "v0.3.12", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "1075", "124.9 MB", "14%",
            "3dthc - VST - Sonix", "sonix.so",
            "https://raw.githubusercontent.com/3dthc/Sonix/main/docs/images/main_layers.jpg", "https://github.com/3dthc/Sonix/releases/download/v0.3.12/Sonix-0.3.12-mpc-armv7.zip", "plugins/sonix/thumb.ppm",
            "A melodic ROMpler instrument for standalone Akai MPC devices, built as a native ARM VST2 plugin. Includes pianos, keys, pads, strings, brass, choirs, leads, plucks, bass, bells, synth chords and effects, using redistributable sound libraries.", "./plugins/sonix/install.sh", 0,
            false,
            false,
            false,
            {  }
        },
        {
            "op1fx-string", "String", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "88", "3.7 MB", "14%",
            "bombest - VST - String", "string.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/string.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/String-0.4.1-mpc-armv7.zip", "plugins/op1fx-string/thumb.ppm",
            "Plucked string with eight voices and an amp envelope. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - String\" && if [ -f \"./plugins/op1fx-string/string.so\" ]; then cp -rf ./plugins/op1fx-string/* \"$TARGET_SYNTH_DIR/bombest - VST - String/\"; else (wget -q -O /tmp/op1fx-string.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/String-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-string.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/String-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/String-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-string.zip) && unzip -o /tmp/op1fx-string.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - String/\" && rm -f /tmp/op1fx-string.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "synth", "pluck", "string" }
        },
        {
            "sturm-tp", "Sturm-TP", "sd88me", "v0.1.2", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "1267", "4.5 MB", "14%",
            "sd88me - VST - Sturm-TP", "sturm.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-sturm/sturm-tp-v0.1.1/docs/img/sturm.png", "https://github.com/sd88me/mpc-vst-sturm/releases/download/sturm-tp-v0.1.2/Sturm-TP-0.1.2-mpc-armv7.zip", "plugins/sturm-tp/thumb.ppm",
            "Six-voice analog-style drum and synth voice modelled on the Tempest drum machine: two analog and two sample oscillators, lowpass and highpass filters, five envelopes, two LFOs, eight mod paths, kit mode, distortion, compressor and delay. Bank browser with Q-Links; loads its SysEx dumps.", "./plugins/sturm-tp/install.sh", 0,
            false,
            false,
            false,
            { "analog-modelling", "drums", "sampler", "kit", "tempest", "dsi" }
        },
        {
            "subwerk", "SubWerk", "Devko", "v0.0.10", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "179", "1.0 MB", "14%",
            "Devko - VST - SubWerk", "subwerk.so",
            "https://raw.githubusercontent.com/Devko/SubWerk/main/docs/img/filter.png", "https://github.com/Devko/SubWerk/releases/download/v0.0.10/SubWerk-0.0.10-mpc-armv7.zip", "plugins/subwerk/thumb.ppm",
            "Analog-style monosynth in the spirit of a 37-key American paraphonic classic: two morphing oscillators with hard sync, sub and noise, a feedback mixer, a 4-pole ladder filter with Multidrive, two looping envelopes, glide and Duo. 57 presets and a six-tab touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Devko - VST - SubWerk\" && if [ -f \"./plugins/subwerk/subwerk.so\" ]; then cp -rf ./plugins/subwerk/* \"$TARGET_SYNTH_DIR/Devko - VST - SubWerk/\"; else (wget -q -O /tmp/subwerk.zip \"https://github.com/Devko/SubWerk/releases/download/v0.0.10/SubWerk-0.0.10-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/subwerk.zip \"https://github.com/Devko/SubWerk/releases/download/v0.0.10/SubWerk-0.0.10-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Devko/SubWerk/releases/download/v0.0.10/SubWerk-0.0.10-mpc-armv7.zip\" -o /tmp/subwerk.zip) && unzip -o /tmp/subwerk.zip -d \"$TARGET_SYNTH_DIR/Devko - VST - SubWerk/\" && rm -f /tmp/subwerk.zip; fi && sync", 1,
            false,
            false,
            false,
            { "monosynth", "analog", "bass", "ladder-filter", "paraphonic" }
        },
        {
            "surgexxl", "SurgeXXL", "L'Cronx", "v0.2.0", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-24",
            "826", "294.4 MB", "14%",
            "L'Cronx - VST - SurgeXXL", "surgexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/SurgeXXL/08c3c3eed8a3ad6792211fb1dc2351ded2383995/docs/img/osc.png", "https://github.com/sunskiefer/SurgeXXL/releases/download/v0.2.0/SurgeXXL-0.2.0-mpc-armv7.zip", "plugins/surgexxl/thumb.ppm",
            "The open-source Surge XT synth engine, headless, for Gen1: 2 scenes of 3 oscillators, dual filters, 6 LFOs per scene, mod matrix, 16 FX slots and 8 macros on eight pages. Loads every factory and third-party patch (3008) and wavetable (761), plus your own. Install it on its own (about 450 MB). MPC OS 3.x.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - SurgeXXL\" && if [ -f \"./plugins/surgexxl/surgexxl.so\" ]; then cp -rf ./plugins/surgexxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - SurgeXXL/\"; else (wget -q -O /tmp/surgexxl.zip \"https://github.com/sunskiefer/SurgeXXL/releases/download/v0.2.0/SurgeXXL-0.2.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/surgexxl.zip \"https://github.com/sunskiefer/SurgeXXL/releases/download/v0.2.0/SurgeXXL-0.2.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/SurgeXXL/releases/download/v0.2.0/SurgeXXL-0.2.0-mpc-armv7.zip\" -o /tmp/surgexxl.zip) && unzip -o /tmp/surgexxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - SurgeXXL/\" && rm -f /tmp/surgexxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "synth", "polysynth", "wavetable", "fm", "surge", "lfo", "presets", "airwindows" }
        },
        {
            "tablor", "Tablor", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "982", "20.3 MB", "14%",
            "athousanddetails - VST - [SYN] Tablor", "tablor.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-tablor/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-tablor/releases/download/v1.1.1/SYN-Tablor-1.1.1-mpc-armv7.zip", "plugins/tablor/thumb.ppm",
            "Two-oscillator wavetable synth with unison, filter and mod envelopes; ships its wavetable library and 9 presets.", "./plugins/tablor/install.sh", 0,
            false,
            false,
            false,
            { "wavetable", "polyphonic", "unison" }
        },
        {
            "tinyfreaky", "TinyFreaky", "MagicStino", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "745", "5.0 MB", "14%",
            "MagicStino - VST - Tinyfreaky", "tinyfreaky.so",
            "https://raw.githubusercontent.com/MagicStino/TinyFreaky/v0.4.0/assets/force-readable-preview.png", "https://github.com/MagicStino/TinyFreaky/releases/download/v0.4.1/Tinyfreaky-0.4.1-mpc-armv7.zip", "plugins/tinyfreaky/thumb.ppm",
            "Eight-voice MicroFreak-inspired interpretation with 16 synthesis engines, a 5x7 modulation matrix, arpeggiator/swing, 64-step sequencer and sound-informed preset names. 342 playable factory slots; six engine families remain unavailable.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MagicStino - VST - Tinyfreaky\" && if [ -f \"./plugins/tinyfreaky/tinyfreaky.so\" ]; then cp -rf ./plugins/tinyfreaky/* \"$TARGET_SYNTH_DIR/MagicStino - VST - Tinyfreaky/\"; else (wget -q -O /tmp/tinyfreaky.zip \"https://github.com/MagicStino/TinyFreaky/releases/download/v0.4.1/Tinyfreaky-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/tinyfreaky.zip \"https://github.com/MagicStino/TinyFreaky/releases/download/v0.4.1/Tinyfreaky-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/MagicStino/TinyFreaky/releases/download/v0.4.1/Tinyfreaky-0.4.1-mpc-armv7.zip\" -o /tmp/tinyfreaky.zip) && unzip -o /tmp/tinyfreaky.zip -d \"$TARGET_SYNTH_DIR/MagicStino - VST - Tinyfreaky/\" && rm -f /tmp/tinyfreaky.zip; fi && sync", 1,
            false,
            false,
            false,
            { "synth", "polyphonic", "modulation", "sequencer" }
        },
        {
            "tr-mpc", "TR-MPC", "sd88me", "v1.1.0", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 3.x", "2026-11-02",
            "1463", "29.1 MB", "25%",
            "sd88me - VST - TR-MPC", "trmpc.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/7e704c6f3be14b661c887631e203494d8cc3f74f/docs/img/tr-mpc-slots.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-vst-v1.1.0/TR-MPC-1.1.0-mpc-armv7.zip", "plugins/tr-mpc/thumb.ppm",
            "A modular 16-slot drum kit: each slot plays any of the 49 voices of athousanddetails' 6W6, 8W8, CW-78 and 9W9 (a 606 kick beside an 808 snare and 909 hats), with per-slot level, tune, decay, drive, pan and sends, one shared reverb, delay and master chain, a randomise module, and a touchscreen skin that reskins each slot to its voice's kit.", "./plugins/tr-mpc/install.sh", 0,
            false,
            false,
            false,
            { "tr-606", "tr-808", "cr-78", "tr-909", "modular" }
        },
        {
            "tributary", "Tributary", "johnny", "v1.1.0", "Utility",
            "INST", "Stable", "MPC One (3.9.1.2)", "MPC OS 3.x", "2026-10-10",
            "838", "556 KB", "3%",
            "johnny - VST - Tributary", "tributary.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/mpc-tributary/v1.1.0/screenshots/tributary.png", "https://github.com/stampman3000-jpg/mpc-tributary/releases/download/v1.1.0/Tributary-1.1.0-mpc-armv7.zip", "plugins/tributary/thumb.ppm",
            "Sums chosen Elektron Overbridge outputs onto one MPC track over USB. Based on Overwitch.", "./plugins/tributary/install.sh", 0,
            false,
            false,
            false,
            { "utility" }
        },
        {
            "tropix", "Tropix", "3dthc", "v0.1.1", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "280", "215.3 MB", "14%",
            "3dthc - VST - Tropix", "tropix.so",
            "https://raw.githubusercontent.com/3dthc/Tropix/1005f9c4dc719f58bd17b02fac750457e6c8cb1e/docs/images/browse.jpg", "https://github.com/3dthc/Tropix/releases/download/v0.1.1/Tropix-0.1.1-mpc-armv7.zip", "plugins/tropix/thumb.ppm",
            "Island and afro ROMpler: 128 presets and 65 instruments for afrobeat, amapiano, kompa, soca, reggae and Hawaiian music - steel pans, log drums, saxes and trumpets, kompa and slack-key guitars, steels, keys, mallets, basses and percussion, from redistributable recordings and original synthesis. Two layers, glide, horn scoop, vibrato, nine colorways.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - Tropix\" && if [ -f \"./plugins/tropix/tropix.so\" ]; then cp -rf ./plugins/tropix/* \"$TARGET_SYNTH_DIR/3dthc - VST - Tropix/\"; else (wget -q -O /tmp/tropix.zip \"https://github.com/3dthc/Tropix/releases/download/v0.1.1/Tropix-0.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/tropix.zip \"https://github.com/3dthc/Tropix/releases/download/v0.1.1/Tropix-0.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/3dthc/Tropix/releases/download/v0.1.1/Tropix-0.1.1-mpc-armv7.zip\" -o /tmp/tropix.zip) && unzip -o /tmp/tropix.zip -d \"$TARGET_SYNTH_DIR/3dthc - VST - Tropix/\" && rm -f /tmp/tropix.zip; fi && sync", 1,
            false,
            false,
            false,
            { "rompler", "afrobeat", "amapiano", "kompa", "soca", "reggae", "steel-pan", "horns" }
        },
        {
            "vader", "VADER", "L'Cronx", "v0.9.0", "Synth",
            "INST", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-30",
            "1528", "42.2 MB", "14%",
            "L'Cronx - VST - VADER", "vader.so",
            "https://raw.githubusercontent.com/sunskiefer/VADER/v0.8.0/docs/img/main_star.png", "https://github.com/sunskiefer/VADER/releases/download/v0.9.0/VADER-0.9.0-mpc-armv7.zip", "plugins/vader/thumb.ppm",
            "An orbit-based vector synth: an orbiter travels a shape (15 built in, 35 maths shapes, or your own SVG, OBJ, PNG or JPG) or one of 27 modes across four corners. Each corner plays overtone generators, one of 14 voices, or a WAV sample. Two timbres, arpeggiator, effects. Its screen runs on your phone: scan the QR code, load shapes from the phone too.", "./plugins/vader/install.sh", 0,
            false,
            false,
            false,
            { "synth", "vector", "orbit", "additive", "morphing", "arpeggiator", "svg", "3d" }
        },
        {
            "op1fx-voltage", "Voltage", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "78", "4.6 MB", "14%",
            "bombest - VST - Voltage", "voltage.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/voltage.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Voltage-0.4.1-mpc-armv7.zip", "plugins/op1fx-voltage/thumb.ppm",
            "Four-control synth with eight voices and an amp envelope. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Voltage\" && if [ -f \"./plugins/op1fx-voltage/voltage.so\" ]; then cp -rf ./plugins/op1fx-voltage/* \"$TARGET_SYNTH_DIR/bombest - VST - Voltage/\"; else (wget -q -O /tmp/op1fx-voltage.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Voltage-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-voltage.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Voltage-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Voltage-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-voltage.zip) && unzip -o /tmp/op1fx-voltage.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Voltage/\" && rm -f /tmp/op1fx-voltage.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "synth", "analog" }
        },
        {
            "wurl", "Wurl", "saustin2010", "v1.1.1", "Electric Piano",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1198", "827 KB", "14%",
            "Filliformes - VST - [SYN] Wurl", "wurl.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-wurl/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-wurl/releases/download/v1.1.1/SYN-Wurl-1.1.1-mpc-armv7.zip", "plugins/wurl/thumb.ppm",
            "A physically modelled Wurlitzer 200A electric piano (OpenWurli) that barks with velocity, with tremolo and 10 presets.", "./plugins/wurl/install.sh", 0,
            false,
            false,
            false,
            { "wurlitzer", "physical-modelling", "keys", "tremolo" }
        },
        {
            "xenia", "Xenia", "sd88me", "v0.1.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-03",
            "244", "8.4 MB", "14%",
            "sd88me - VST - Xenia", "xenia.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-xenia/aeb6a25cfe8e5e14fc1df592526b2c2a32eb99be/docs/screenshots/xenia-play.png", "https://github.com/sd88me/mpc-vst-xenia/releases/download/v0.1.2/Xenia-0.1.2-mpc-aarch64.zip", "plugins/xenia/thumb.ppm",
            "An emulation of the Microwave XT wavetable synthesizer's original firmware (gearmulator's 68k and DSP56300 emulators) as a native instrument for Gen2 MPC devices: ten touchscreen pages, ROM banks and patch browser, assignable play knobs and Q-Link control on every page. Gen2 (aarch64) only; you need your own Microwave XT ROM.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Xenia\" && if [ -f \"./plugins/xenia/xenia.so\" ]; then cp -rf ./plugins/xenia/* \"$TARGET_SYNTH_DIR/sd88me - VST - Xenia/\"; else (wget -q -O /tmp/xenia.zip \"https://github.com/sd88me/mpc-vst-xenia/releases/download/v0.1.2/Xenia-0.1.2-mpc-aarch64.zip\" || wget --no-check-certificate -q -O /tmp/xenia.zip \"https://github.com/sd88me/mpc-vst-xenia/releases/download/v0.1.2/Xenia-0.1.2-mpc-aarch64.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-xenia/releases/download/v0.1.2/Xenia-0.1.2-mpc-aarch64.zip\" -o /tmp/xenia.zip) && unzip -o /tmp/xenia.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Xenia/\" && rm -f /tmp/xenia.zip; fi && sync", 1,
            false,
            false,
            false,
            { "emulation", "wavetable", "rompler-free", "gen2" }
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
            "5573", "3.2 MB", "14%",
            "sd88me - VST - Acid", "acid.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-acid/master/docs/img/acid.png", "https://github.com/sd88me/mpc-vst-acid/releases/download/acid-vst-v2.1.0/Acid-2.1.0-mpc-armv7.zip", "plugins/acid/thumb.ppm",
            "TB-303-style acid bassline sequencer: two blendable lines with generate/mutate, driving a synth track over MIDI, with a touchscreen skin and Q-Links.", "./plugins/acid/install.sh", 0,
            true,
            false,
            false,
            { "303", "acid", "midi", "bassline" }
        },
        {
            "cadenza", "Cadenza", "Shrekage", "v0.25.0", "Sequencer",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-29",
            "159", "1.1 MB", "40%",
            "Cadenza - VST - Cadenza", "cadenza.so",
            "https://raw.githubusercontent.com/Shrekage2024/cadenza-force/main/docs/screenshot.png", "https://github.com/Shrekage2024/cadenza-force/releases/download/v0.25.0/Cadenza-0.25.0-mpc-armv7.zip", "plugins/cadenza/thumb.ppm",
            "Chord-driven generative MIDI sequencer: pick a key and scale, shape chords, bass and melody with a few knobs per page, and it generates and follows along as you play.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Cadenza - VST - Cadenza\" && if [ -f \"./plugins/cadenza/cadenza.so\" ]; then cp -rf ./plugins/cadenza/* \"$TARGET_SYNTH_DIR/Cadenza - VST - Cadenza/\"; else (wget -q -O /tmp/cadenza.zip \"https://github.com/Shrekage2024/cadenza-force/releases/download/v0.25.0/Cadenza-0.25.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/cadenza.zip \"https://github.com/Shrekage2024/cadenza-force/releases/download/v0.25.0/Cadenza-0.25.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Shrekage2024/cadenza-force/releases/download/v0.25.0/Cadenza-0.25.0-mpc-armv7.zip\" -o /tmp/cadenza.zip) && unzip -o /tmp/cadenza.zip -d \"$TARGET_SYNTH_DIR/Cadenza - VST - Cadenza/\" && rm -f /tmp/cadenza.zip; fi && sync", 1,
            false,
            false,
            false,
            { "chords", "generative", "midi", "bassline" }
        },
        {
            "chordsmith", "Chordsmith", "jacob-sabella", "v1.0.4", "Utility",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "2828", "4.2 MB", "8%",
            "jacob-sabella - VST - Chordsmith", "chordsmith.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-chordsmith/188d34ca0d9482d2d1ba840edb12ccd8c3773f76/docs/screenshots/chordsmith.png", "https://github.com/jacob-sabella/mpc-vst-chordsmith/releases/download/chordsmith-vst-v1.0.4/Chordsmith-1.0.4-mpc-armv7.zip", "plugins/chordsmith/thumb.ppm",
            "Chord tool: pads, keys or chord buttons play scale, set or custom chords with voicing, strum, arp and harp modes, MIDI out to other tracks and a built-in preview synth.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Chordsmith\" && if [ -f \"./plugins/chordsmith/chordsmith.so\" ]; then cp -rf ./plugins/chordsmith/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Chordsmith/\"; else (wget -q -O /tmp/chordsmith.zip \"https://github.com/jacob-sabella/mpc-vst-chordsmith/releases/download/chordsmith-vst-v1.0.4/Chordsmith-1.0.4-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/chordsmith.zip \"https://github.com/jacob-sabella/mpc-vst-chordsmith/releases/download/chordsmith-vst-v1.0.4/Chordsmith-1.0.4-mpc-armv7.zip\" || curl -k -sL \"https://github.com/jacob-sabella/mpc-vst-chordsmith/releases/download/chordsmith-vst-v1.0.4/Chordsmith-1.0.4-mpc-armv7.zip\" -o /tmp/chordsmith.zip) && unzip -o /tmp/chordsmith.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Chordsmith/\" && rm -f /tmp/chordsmith.zip; fi && sync", 1,
            false,
            false,
            false,
            { "chords", "midi", "arpeggiator", "strum", "chord-builder" }
        },
        {
            "daemon", "Daemon", "sd88me", "v1.0.1", "Sequencer",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "493", "727 KB", "14%",
            "sd88me - VST - Daemon", "daemon.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-daemon/v1.0.0/docs/screenshots/daemon.png", "https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-1.0.1-mpc-armv7.zip", "plugins/daemon/thumb.ppm",
            "AI generative assistant: type or say what you hear and Daemon writes chords, basslines, melodies and beats in your key and plays them in time on its MIDI port. Agent mode builds the session too (tracks, instruments, presets, effects, clips) as a plan you RUN. Gemini or Claude; needs network and an API key.", "./plugins/daemon/install.sh", 0,
            false,
            false,
            false,
            { "generative", "ai", "midi", "chords", "melody", "beats", "agent", "voice" }
        },
        {
            "grids", "Drum Map", "saustin2010", "v1.2.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1459", "337 KB", "14%",
            "Émilie Gillet - VST - [SEQ] Drum Map", "grids.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-drum-map/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-drum-map/releases/download/v1.2.1/SEQ-Drum-Map-1.2.1-mpc-armv7.zip", "plugins/grids/thumb.ppm",
            "Drum Map: a topographic drum sequencer that plays kick, snare and hats from MPC's clock through its own MIDI port, from Émilie Gillet's open-source Grids.", "./plugins/grids/install.sh", 0,
            false,
            false,
            false,
            { "drum", "midi", "euclidean", "eurorack" }
        },
        {
            "rampage", "Dual Slope", "saustin2010", "v1.2.1", "Modulation",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1221", "3.1 MB", "14%",
            "Andrew Belt - VST - [SEQ] Dual Slope", "rampage.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-dual-slope/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-dual-slope/releases/download/v1.2.1/SEQ-Dual-Slope-1.2.1-mpc-armv7.zip", "plugins/rampage/thumb.ppm",
            "Dual Slope, based on Befaco's Rampage VCV Rack module: two rise-and-fall generators (envelopes, LFOs, slew) whose outputs go out of its own MIDI port as CCs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Andrew Belt - VST - [SEQ] Dual Slope\" && if [ -f \"./plugins/rampage/rampage.so\" ]; then cp -rf ./plugins/rampage/* \"$TARGET_SYNTH_DIR/Andrew Belt - VST - [SEQ] Dual Slope/\"; else (wget -q -O /tmp/rampage.zip \"https://github.com/saustin2010/mpc-vst-dual-slope/releases/download/v1.2.1/SEQ-Dual-Slope-1.2.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/rampage.zip \"https://github.com/saustin2010/mpc-vst-dual-slope/releases/download/v1.2.1/SEQ-Dual-Slope-1.2.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-dual-slope/releases/download/v1.2.1/SEQ-Dual-Slope-1.2.1-mpc-armv7.zip\" -o /tmp/rampage.zip) && unzip -o /tmp/rampage.zip -d \"$TARGET_SYNTH_DIR/Andrew Belt - VST - [SEQ] Dual Slope/\" && rm -f /tmp/rampage.zip; fi && sync", 1,
            false,
            false,
            false,
            { "vcv", "envelope", "lfo", "slew", "midi-cc", "eurorack" }
        },
        {
            "eucalypso", "Eucalypso", "saustin2010", "v1.0.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "820", "546 KB", "14%",
            "handcraftedcc - VST - [SEQ] Eucalypso", "eucalypso.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-eucalypso/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-eucalypso/releases/download/v1.0.1/SEQ-Eucalypso-1.0.1-mpc-armv7.zip", "plugins/eucalypso/thumb.ppm",
            "Four-lane Euclidean sequencer that turns the notes you hold into interlocking rhythms for other tracks.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Eucalypso\" && if [ -f \"./plugins/eucalypso/eucalypso.so\" ]; then cp -rf ./plugins/eucalypso/* \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Eucalypso/\"; else (wget -q -O /tmp/eucalypso.zip \"https://github.com/saustin2010/mpc-vst-eucalypso/releases/download/v1.0.1/SEQ-Eucalypso-1.0.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/eucalypso.zip \"https://github.com/saustin2010/mpc-vst-eucalypso/releases/download/v1.0.1/SEQ-Eucalypso-1.0.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-eucalypso/releases/download/v1.0.1/SEQ-Eucalypso-1.0.1-mpc-armv7.zip\" -o /tmp/eucalypso.zip) && unzip -o /tmp/eucalypso.zip -d \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Eucalypso/\" && rm -f /tmp/eucalypso.zip; fi && sync", 1,
            false,
            false,
            false,
            { "euclidean", "midi", "polyrhythm", "generative" }
        },
        {
            "euclidier", "Euclidier", "sd88me", "v1.0.8", "Sequencer",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 2.x & 3.x", "2026-10-14",
            "4084", "1.6 MB", "14%",
            "sd88me - VST - Euclidier", "euclidier.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-euclidier/master/docs/img/euclidier.png", "https://github.com/sd88me/mpc-vst-euclidier/releases/download/euclidier-vst-v1.0.8/Euclidier-1.0.8-mpc-armv7.zip", "plugins/euclidier/thumb.ppm",
            "8-lane Euclidean MIDI sequencer: live pattern circle and lane rows with a play-head, per-lane and global randomise, a drum-mode switch, and a touchscreen skin with Q-Links.", "./plugins/euclidier/install.sh", 0,
            true,
            true,
            false,
            { "euclidean", "midi", "drum", "polyrhythm" }
        },
        {
            "glass-spheres", "Glass Spheres", "Padbangers", "v1.0.0", "Sequencer",
            "MIDI", "Stable", "Akai MPC X (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-04",
            "927", "6.6 MB", "4%",
            "Padbangers - VST - Glass Spheres", "glass_spheres.so",
            "https://raw.githubusercontent.com/FullPace/glass-spheres/glass-spheres-v1.0.0/docs/glass-spheres-main.png", "https://github.com/padbangers/glass-spheres/releases/download/glass-spheres-v1.0.0/Glass-Spheres-1.0.0-mpc-armv7.zip", "plugins/glass-spheres/thumb.ppm",
            "A random sampler after Mutable Instruments Marbles: random gates, melodies and deja vu loops that play other tracks over MIDI or analog gear over the MPC X's CV jacks, with envelopes, LFOs and a modulation matrix.", "./plugins/glass-spheres/install.sh", 0,
            false,
            false,
            false,
            { "mutable-instruments", "marbles", "random", "generative", "sequencer", "midi", "cv", "eurorack" }
        },
        {
            "groove-bank", "Groove Bank", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1477", "852 KB", "14%",
            "Mission Minnow - VST - [SEQ] Groove Bank", "groovebank.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-groovebank/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-groovebank/releases/download/v1.1.1/SEQ-Groove-Bank-1.1.1-mpc-armv7.zip", "plugins/groove-bank/thumb.ppm",
            "Replays the chord you hold in genre grooves from its library, with swing, strum and latch, through its own MIDI port.", "./plugins/groove-bank/install.sh", 0,
            false,
            false,
            false,
            { "midi", "chords", "rhythm", "grooves" }
        },
        {
            "maze-sequencer", "Maze Sequencer", "sd88me", "v1.0.7", "Sequencer",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (Gen2) (MPC OS 3.9.1)", "MPC OS 2.x & 3.x", "2026-10-13",
            "3903", "3.0 MB", "1%",
            "sd88me - VST - Maze Sequencer", "maze_seq.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-maze/main/docs/img/maze-sequencer.png", "https://github.com/sd88me/mpc-vst-maze/releases/download/maze-sequencer-vst-v1.0.7/Maze-Sequencer-1.0.7-mpc-armv7.zip", "plugins/maze-sequencer/thumb.ppm",
            "Dual 8-step generative sequencer (Moog Labyrinth style) with corrupt, scale quantising, two LFOs and per-line MIDI channels, driving other tracks over MIDI.", "./plugins/maze-sequencer/install.sh", 0,
            true,
            false,
            false,
            { "generative", "midi", "moog", "labyrinth" }
        },
        {
            "midi-player", "MIDI Player", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "909", "144 KB", "14%",
            "Charles Vestal - VST - [SEQ] MIDI Player", "midiplayer.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-midiplayer/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-midiplayer/releases/download/v1.1.1/SEQ-MIDI-Player-1.1.1-mpc-armv7.zip", "plugins/midi-player/thumb.ppm",
            "Plays Standard MIDI Files in time with MPC's tempo and transport, one track or all, through its own MIDI port.", "./plugins/midi-player/install.sh", 0,
            false,
            false,
            false,
            { "midi", "midi-file", "player", "transport-sync" }
        },
        {
            "pixel-walkers", "Pixel Walkers", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "919", "375 KB", "14%",
            "mestela - VST - [SEQ] Pixel Walkers", "pixelwalkers.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-pixelwalkers/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-pixelwalkers/releases/download/v1.1.1/SEQ-Pixel-Walkers-1.1.1-mpc-armv7.zip", "plugins/pixel-walkers/thumb.ppm",
            "Generative gravity sequencer: each note you play becomes a walker that bounces and retriggers its note as it lands.", "./plugins/pixel-walkers/install.sh", 0,
            false,
            false,
            false,
            { "generative", "midi", "physics", "bouncing" }
        },
        {
            "steprunner-8", "StepRunner-8", "djokenzi", "v1.2.0", "Sequencer",
            "MIDI", "Stable", "MPC Live (3.9.1.2)", "MPC OS 3.x", "2026-10-19",
            "1349", "3.4 MB", "14%",
            "djokenzi - VST - StepRunner-8", "steprunner8.so",
            "https://raw.githubusercontent.com/djokenzi/steprunner-8/76285a3aa34dcdc85bf8e72e3aa18baa7f6428cd/docs/screenshots/steprunner-8.png", "https://github.com/djokenzi/steprunner-8/releases/download/v1.2.0/StepRunner-8-1.2.0-mpc-armv7.zip", "plugins/steprunner-8/thumb.ppm",
            "8-track, 64-step sequencer with step conditions, step and CC locks, patterns, states and performance effects, driving a drum program over MIDI.", "./plugins/steprunner-8/install.sh", 0,
            false,
            false,
            false,
            { "midi", "step-sequencer", "drums", "step-locks" }
        },
        {
            "stevequencer", "Stevequencer", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "996", "1.2 MB", "14%",
            "Steve A - VST - [SEQ] Stevequencer", "stevequencer.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-stevequencer/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-stevequencer/releases/download/v1.1.1/SEQ-Stevequencer-1.1.1-mpc-armv7.zip", "plugins/stevequencer/thumb.ppm",
            "Melodic step sequencer edited from the Q-Links: 16 steps on a 4x4 grid, up to 64, with chance, ratchets and MOD lanes.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer\" && if [ -f \"./plugins/stevequencer/stevequencer.so\" ]; then cp -rf ./plugins/stevequencer/* \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer/\"; else (wget -q -O /tmp/stevequencer.zip \"https://github.com/saustin2010/mpc-vst-stevequencer/releases/download/v1.1.1/SEQ-Stevequencer-1.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/stevequencer.zip \"https://github.com/saustin2010/mpc-vst-stevequencer/releases/download/v1.1.1/SEQ-Stevequencer-1.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-stevequencer/releases/download/v1.1.1/SEQ-Stevequencer-1.1.1-mpc-armv7.zip\" -o /tmp/stevequencer.zip) && unzip -o /tmp/stevequencer.zip -d \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer/\" && rm -f /tmp/stevequencer.zip; fi && sync", 1,
            false,
            false,
            false,
            { "step-sequencer", "midi", "q-links", "melodic" }
        },
        {
            "stevequencer-16", "Stevequencer 16", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "968", "628 KB", "14%",
            "Steve A - VST - [SEQ] Stevequencer 16", "stevequencer16.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-stevequencer16/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-stevequencer16/releases/download/v1.1.1/SEQ-Stevequencer-16-1.1.1-mpc-armv7.zip", "plugins/stevequencer-16/thumb.ppm",
            "16-step sequencer with eight modulation lanes that move any parameter of the instrument (CC or NRPN), on any page.", "./plugins/stevequencer-16/install.sh", 0,
            false,
            false,
            false,
            { "step-sequencer", "midi", "modulation", "nrpn", "q-links" }
        },
        {
            "super-arp", "Super Arp", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1025", "2.3 MB", "14%",
            "handcraftedcc - VST - [SEQ] Super Arp", "superarp.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-superarp/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-superarp/releases/download/v1.1.1/SEQ-Super-Arp-1.1.1-mpc-armv7.zip", "plugins/super-arp/thumb.ppm",
            "Arpeggiator that splits note order from rhythm: 40 patterns, 40 rhythms and seeded random modifiers.", "./plugins/super-arp/install.sh", 0,
            false,
            false,
            false,
            { "arpeggiator", "midi", "patterns", "rhythm" }
        },
        {
            "piclidean-mpc", "πClideaN MPC", "Timo", "v1.3.0", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-30",
            "85", "5.1 MB", "14%",
            "Timo - VST - πClideaN MPC", "piclidean_mpc.so",
            "https://raw.githubusercontent.com/timomacquis/piClideaN16-MPC/48c2560cb46bcec86b8f473844d38b3b37a21709/Cover.png", "https://github.com/timomacquis/piClideaN16-MPC/releases/download/v1.3.0/ClideaN-MPC-1.3.0-mpc-armv7.zip", "plugins/piclidean-mpc/thumb.ppm",
            "Sixteen-lane hybrid MIDI sequencer with Euclidean patterns, per-step expression, scales, selective randomization and dual-LFO modulation.", "./plugins/piclidean-mpc/install.sh", 0,
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
            "1181", "745 KB", "14%",
            "Airwindows - VST - Air3", "air3.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Air3.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Air3-1.0.0-mpc-armv7.zip", "plugins/airwindows-air3/thumb.ppm",
            "Air-band EQ that lifts or tames the very top and the low ground of a mix.", "./plugins/airwindows-air3/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "eq", "air", "treble" }
        },
        {
            "airwindows-buttercomp2", "Airwindows ButterComp2", "mmiroshnikov", "v1.0.0", "Compressor",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1155", "757 KB", "14%",
            "Airwindows - VST - ButterComp2", "buttercomp2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ButterComp2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ButterComp2-1.0.0-mpc-armv7.zip", "plugins/airwindows-buttercomp2/thumb.ppm",
            "Smooth, buttery bipolar compressor that stays transparent even when pushed.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ButterComp2\" && if [ -f \"./plugins/airwindows-buttercomp2/buttercomp2.so\" ]; then cp -rf ./plugins/airwindows-buttercomp2/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ButterComp2/\"; else (wget -q -O /tmp/airwindows-buttercomp2.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ButterComp2-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-buttercomp2.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ButterComp2-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ButterComp2-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-buttercomp2.zip) && unzip -o /tmp/airwindows-buttercomp2.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ButterComp2/\" && rm -f /tmp/airwindows-buttercomp2.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "compressor", "dynamics" }
        },
        {
            "airwindows-chamber2", "Airwindows Chamber2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1103", "702 KB", "14%",
            "Airwindows - VST - Chamber2", "chamber2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Chamber2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Chamber2-1.0.0-mpc-armv7.zip", "plugins/airwindows-chamber2/thumb.ppm",
            "Chamber reverb with delay, regeneration and thickness controls.", "./plugins/airwindows-chamber2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "chamber" }
        },
        {
            "airwindows-chorusensemble", "Airwindows ChorusEnsemble", "mmiroshnikov", "v1.0.0", "Chorus",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1106", "647 KB", "14%",
            "Airwindows - VST - ChorusEnsemble", "chorusensemble.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ChorusEnsemble.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ChorusEnsemble-1.0.0-mpc-armv7.zip", "plugins/airwindows-chorusensemble/thumb.ppm",
            "Lush multi-tap ensemble chorus.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ChorusEnsemble\" && if [ -f \"./plugins/airwindows-chorusensemble/chorusensemble.so\" ]; then cp -rf ./plugins/airwindows-chorusensemble/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ChorusEnsemble/\"; else (wget -q -O /tmp/airwindows-chorusensemble.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ChorusEnsemble-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-chorusensemble.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ChorusEnsemble-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ChorusEnsemble-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-chorusensemble.zip) && unzip -o /tmp/airwindows-chorusensemble.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ChorusEnsemble/\" && rm -f /tmp/airwindows-chorusensemble.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "chorus", "ensemble", "modulation" }
        },
        {
            "aw-ambience", "Airwindows Collection: Ambience", "sd88me", "v0.1.0", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "138", "10.6 MB", "14%",
            "Airwindows - VST - AW Ambience", "aw_ambience.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/ambience.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Ambience-0.1.0-mpc-armv7.zip", "plugins/aw-ambience/thumb.ppm",
            "The Airwindows Ambience collection: all 26 ambience, delay, chorus and echo effects in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-ambience/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "delay", "collection" }
        },
        {
            "aw-console-strips", "Airwindows Collection: Console Strips", "sd88me", "v0.1.0", "Mixing",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "146", "15.2 MB", "14%",
            "Airwindows - VST - AW Console Strips", "aw_consolestrips.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/consolestrips.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Console-Strips-0.1.0-mpc-armv7.zip", "plugins/aw-console-strips/thumb.ppm",
            "The Airwindows Console Strips collection: all 8 big console channel strips (ConsoleX and ConsoleH), spread over four pages in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Console Strips\" && if [ -f \"./plugins/aw-console-strips/aw_consolestrips.so\" ]; then cp -rf ./plugins/aw-console-strips/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Console Strips/\"; else (wget -q -O /tmp/aw-console-strips.zip \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Console-Strips-0.1.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/aw-console-strips.zip \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Console-Strips-0.1.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Console-Strips-0.1.0-mpc-armv7.zip\" -o /tmp/aw-console-strips.zip) && unzip -o /tmp/aw-console-strips.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Console Strips/\" && rm -f /tmp/aw-console-strips.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "channel-strip", "collection" }
        },
        {
            "aw-consoles", "Airwindows Collection: Consoles", "sd88me", "v0.1.0", "Mixing",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "137", "10.5 MB", "14%",
            "Airwindows - VST - AW Consoles", "aw_consoles.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/consoles.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Consoles-0.1.0-mpc-armv7.zip", "plugins/aw-consoles/thumb.ppm",
            "The Airwindows Consoles collection: all 50 console and mix-buss systems (Console 0 to 9, ConsoleLA, MC, MD and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-consoles/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "console", "collection" }
        },
        {
            "aw-distortion", "Airwindows Collection: Distortion", "sd88me", "v0.1.0", "Distortion",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "136", "10.0 MB", "14%",
            "Airwindows - VST - AW Distortion", "aw_distortion.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/distortion.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Distortion-0.1.0-mpc-armv7.zip", "plugins/aw-distortion/thumb.ppm",
            "The Airwindows Distortion collection: all 15 distortions (Density, Drive, Edge, Mackity, HardVacuum and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Distortion\" && if [ -f \"./plugins/aw-distortion/aw_distortion.so\" ]; then cp -rf ./plugins/aw-distortion/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Distortion/\"; else (wget -q -O /tmp/aw-distortion.zip \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Distortion-0.1.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/aw-distortion.zip \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Distortion-0.1.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Distortion-0.1.0-mpc-armv7.zip\" -o /tmp/aw-distortion.zip) && unzip -o /tmp/aw-distortion.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Distortion/\" && rm -f /tmp/aw-distortion.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "distortion", "collection" }
        },
        {
            "aw-dynamics", "Airwindows Collection: Dynamics", "sd88me", "v0.1.0", "Dynamics",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "135", "13.8 MB", "14%",
            "Airwindows - VST - AW Dynamics", "aw_dynamics.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/dynamics.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Dynamics-0.1.0-mpc-armv7.zip", "plugins/aw-dynamics/thumb.ppm",
            "The Airwindows Dynamics collection: all 35 compressors, gates and dynamics tools in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-dynamics/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "compressor", "collection" }
        },
        {
            "aw-effects", "Airwindows Collection: Effects", "sd88me", "v0.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "144", "12.2 MB", "14%",
            "Airwindows - VST - AW Effects", "aw_effects.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/effects.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Effects-0.1.0-mpc-armv7.zip", "plugins/aw-effects/thumb.ppm",
            "The Airwindows Effects collection: all 29 effects (Pafnuty, tremolo, pitch shifters, wavefolders, exciters and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Effects\" && if [ -f \"./plugins/aw-effects/aw_effects.so\" ]; then cp -rf ./plugins/aw-effects/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Effects/\"; else (wget -q -O /tmp/aw-effects.zip \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Effects-0.1.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/aw-effects.zip \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Effects-0.1.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Effects-0.1.0-mpc-armv7.zip\" -o /tmp/aw-effects.zip) && unzip -o /tmp/aw-effects.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Effects/\" && rm -f /tmp/aw-effects.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "effects", "collection" }
        },
        {
            "aw-filter", "Airwindows Collection: Filter", "sd88me", "v0.1.0", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "134", "13.7 MB", "14%",
            "Airwindows - VST - AW Filter", "aw_filter.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/filter.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Filter-0.1.0-mpc-armv7.zip", "plugins/aw-filter/thumb.ppm",
            "The Airwindows Filter collection: all 54 filters and EQs (Baxandall, Isolator, Holt, SmoothEQ and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-filter/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "eq", "collection" }
        },
        {
            "aw-reverb", "Airwindows Collection: Reverb", "sd88me", "v0.1.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "141", "10.7 MB", "14%",
            "Airwindows - VST - AW Reverb", "aw_reverb.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/reverb.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Reverb-0.1.0-mpc-armv7.zip", "plugins/aw-reverb/thumb.ppm",
            "The Airwindows Reverb collection: all 42 reverbs (the k-series, plates, Galactic, Verbity and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Reverb\" && if [ -f \"./plugins/aw-reverb/aw_reverb.so\" ]; then cp -rf ./plugins/aw-reverb/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Reverb/\"; else (wget -q -O /tmp/aw-reverb.zip \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Reverb-0.1.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/aw-reverb.zip \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Reverb-0.1.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Reverb-0.1.0-mpc-armv7.zip\" -o /tmp/aw-reverb.zip) && unzip -o /tmp/aw-reverb.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Reverb/\" && rm -f /tmp/aw-reverb.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "reverb", "collection" }
        },
        {
            "aw-saturation", "Airwindows Collection: Saturation", "sd88me", "v0.1.0", "Saturation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "142", "10.9 MB", "14%",
            "Airwindows - VST - AW Saturation", "aw_saturation.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/saturation.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Saturation-0.1.0-mpc-armv7.zip", "plugins/aw-saturation/thumb.ppm",
            "The Airwindows Saturation collection: all 20 saturation models (Tube, Mojo, Focus, Spiral2 and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-saturation/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "saturation", "collection" }
        },
        {
            "aw-tape", "Airwindows Collection: Tape", "sd88me", "v0.1.0", "Tape",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "143", "10.6 MB", "14%",
            "Airwindows - VST - AW Tape", "aw_tape.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/tape.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Tape-0.1.0-mpc-armv7.zip", "plugins/aw-tape/thumb.ppm",
            "The Airwindows Tape collection: all 12 tape emulations (ToTape, TapeHack, IronOxide and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Tape\" && if [ -f \"./plugins/aw-tape/aw_tape.so\" ]; then cp -rf ./plugins/aw-tape/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Tape/\"; else (wget -q -O /tmp/aw-tape.zip \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Tape-0.1.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/aw-tape.zip \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Tape-0.1.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Tape-0.1.0-mpc-armv7.zip\" -o /tmp/aw-tape.zip) && unzip -o /tmp/aw-tape.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Tape/\" && rm -f /tmp/aw-tape.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "tape", "collection" }
        },
        {
            "aw-utility", "Airwindows Collection: Utility", "sd88me", "v0.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "126", "13.5 MB", "14%",
            "Airwindows - VST - AW Utility", "aw_utility.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/utility.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Utility-0.1.0-mpc-armv7.zip", "plugins/aw-utility/thumb.ppm",
            "The Airwindows Utility collection: all 38 utilities (gain, pan, trims, mid/side, monitoring, mix checks and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-utility/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "utility", "collection" }
        },
        {
            "airwindows-console7channel", "Airwindows Console7Channel", "mmiroshnikov", "v1.0.0", "Saturation",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1103", "737 KB", "14%",
            "Airwindows - VST - Console7Channel", "console7channel.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Console7Channel.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Console7Channel-1.0.0-mpc-armv7.zip", "plugins/airwindows-console7channel/thumb.ppm",
            "Console7 channel stage: analog-style console saturation behind a fader.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Console7Channel\" && if [ -f \"./plugins/airwindows-console7channel/console7channel.so\" ]; then cp -rf ./plugins/airwindows-console7channel/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Console7Channel/\"; else (wget -q -O /tmp/airwindows-console7channel.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Console7Channel-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-console7channel.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Console7Channel-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Console7Channel-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-console7channel.zip) && unzip -o /tmp/airwindows-console7channel.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Console7Channel/\" && rm -f /tmp/airwindows-console7channel.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "console", "saturation", "summing" }
        },
        {
            "airwindows-density2", "Airwindows Density2", "mmiroshnikov", "v1.0.0", "Saturation",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1116", "648 KB", "14%",
            "Airwindows - VST - Density2", "density2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Density2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Density2-1.0.0-mpc-armv7.zip", "plugins/airwindows-density2/thumb.ppm",
            "Density saturation with a highpass, for thickening and fattening sounds.", "./plugins/airwindows-density2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "saturation", "distortion", "density" }
        },
        {
            "airwindows-galactic", "Airwindows Galactic", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1174", "835 KB", "14%",
            "Airwindows - VST - Galactic", "galactic.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Galactic.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic-1.0.0-mpc-armv7.zip", "plugins/airwindows-galactic/thumb.ppm",
            "Huge, dark, shimmering ambient space reverb.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic\" && if [ -f \"./plugins/airwindows-galactic/galactic.so\" ]; then cp -rf ./plugins/airwindows-galactic/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic/\"; else (wget -q -O /tmp/airwindows-galactic.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-galactic.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-galactic.zip) && unzip -o /tmp/airwindows-galactic.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic/\" && rm -f /tmp/airwindows-galactic.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "reverb", "ambient", "shimmer" }
        },
        {
            "airwindows-galactic2", "Airwindows Galactic2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1100", "816 KB", "14%",
            "Airwindows - VST - Galactic2", "galactic2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Galactic2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic2-1.0.0-mpc-armv7.zip", "plugins/airwindows-galactic2/thumb.ppm",
            "Galactic reverb with drive, sustain and darkening for vast ambient tails.", "./plugins/airwindows-galactic2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "ambient" }
        },
        {
            "airwindows-galactic3", "Airwindows Galactic3", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1117", "692 KB", "14%",
            "Airwindows - VST - Galactic3", "galactic3.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Galactic3.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic3-1.0.0-mpc-armv7.zip", "plugins/airwindows-galactic3/thumb.ppm",
            "Galactic reverb with brightness, detune, derez and size controls.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic3\" && if [ -f \"./plugins/airwindows-galactic3/galactic3.so\" ]; then cp -rf ./plugins/airwindows-galactic3/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic3/\"; else (wget -q -O /tmp/airwindows-galactic3.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic3-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-galactic3.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic3-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic3-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-galactic3.zip) && unzip -o /tmp/airwindows-galactic3.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic3/\" && rm -f /tmp/airwindows-galactic3.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "reverb", "ambient" }
        },
        {
            "airwindows-galacticvibe", "Airwindows GalacticVibe", "mmiroshnikov", "v1.0.0", "Chorus",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1073", "702 KB", "14%",
            "Airwindows - VST - GalacticVibe", "galacticvibe.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/GalacticVibe.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/GalacticVibe-1.0.0-mpc-armv7.zip", "plugins/airwindows-galacticvibe/thumb.ppm",
            "The pitch-wobble vibrato stage of the Galactic reverbs on its own.", "./plugins/airwindows-galacticvibe/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "vibrato", "modulation", "chorus" }
        },
        {
            "airwindows-infinity2", "Airwindows Infinity2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1111", "622 KB", "14%",
            "Airwindows - VST - Infinity2", "infinity2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Infinity2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Infinity2-1.0.0-mpc-armv7.zip", "plugins/airwindows-infinity2/thumb.ppm",
            "Infinite-sustain reverb with filtering, damping and allpass diffusion.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Infinity2\" && if [ -f \"./plugins/airwindows-infinity2/infinity2.so\" ]; then cp -rf ./plugins/airwindows-infinity2/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Infinity2/\"; else (wget -q -O /tmp/airwindows-infinity2.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Infinity2-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-infinity2.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Infinity2-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Infinity2-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-infinity2.zip) && unzip -o /tmp/airwindows-infinity2.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Infinity2/\" && rm -f /tmp/airwindows-infinity2.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "reverb", "ambient", "freeze" }
        },
        {
            "airwindows-kalienspaceship", "Airwindows kAlienSpaceship", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1092", "721 KB", "14%",
            "Airwindows - VST - kAlienSpaceship", "kalienspaceship.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/kAlienSpaceship.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kAlienSpaceship-1.0.0-mpc-armv7.zip", "plugins/airwindows-kalienspaceship/thumb.ppm",
            "Enormous sci-fi space reverb from the kCathedral family.", "./plugins/airwindows-kalienspaceship/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "sci-fi", "ambient" }
        },
        {
            "airwindows-kcathedral5", "Airwindows kCathedral5", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1057", "599 KB", "14%",
            "Airwindows - VST - kCathedral5", "kcathedral5.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/kCathedral5.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCathedral5-1.0.0-mpc-armv7.zip", "plugins/airwindows-kcathedral5/thumb.ppm",
            "Cathedral-sized reverb with early reflections and position control.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - kCathedral5\" && if [ -f \"./plugins/airwindows-kcathedral5/kcathedral5.so\" ]; then cp -rf ./plugins/airwindows-kcathedral5/* \"$TARGET_SYNTH_DIR/Airwindows - VST - kCathedral5/\"; else (wget -q -O /tmp/airwindows-kcathedral5.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCathedral5-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-kcathedral5.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCathedral5-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCathedral5-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-kcathedral5.zip) && unzip -o /tmp/airwindows-kcathedral5.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - kCathedral5/\" && rm -f /tmp/airwindows-kcathedral5.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "reverb", "cathedral" }
        },
        {
            "airwindows-kcosmos", "Airwindows kCosmos", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1092", "715 KB", "14%",
            "Airwindows - VST - kCosmos", "kcosmos.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/kCosmos.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCosmos-1.0.0-mpc-armv7.zip", "plugins/airwindows-kcosmos/thumb.ppm",
            "Vast cosmic reverb with predelay and early reflections.", "./plugins/airwindows-kcosmos/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "ambient", "cosmic" }
        },
        {
            "airwindows-mackity", "Airwindows Mackity", "mmiroshnikov", "v1.0.0", "Saturation",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-05",
            "951", "587 KB", "14%",
            "Airwindows - VST - Mackity", "mackity.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Mackity.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.1.0/Mackity-1.0.0-mpc-armv7.zip", "plugins/airwindows-mackity/thumb.ppm",
            "Mackie-style console distortion: drive it with the input trim, tame it with the output pad.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Mackity\" && if [ -f \"./plugins/airwindows-mackity/mackity.so\" ]; then cp -rf ./plugins/airwindows-mackity/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Mackity/\"; else (wget -q -O /tmp/airwindows-mackity.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.1.0/Mackity-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-mackity.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.1.0/Mackity-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.1.0/Mackity-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-mackity.zip) && unzip -o /tmp/airwindows-mackity.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Mackity/\" && rm -f /tmp/airwindows-mackity.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "saturation", "distortion", "console" }
        },
        {
            "airwindows-matrixverb", "Airwindows MatrixVerb", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1069", "730 KB", "14%",
            "Airwindows - VST - MatrixVerb", "matrixverb.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/MatrixVerb.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/MatrixVerb-1.0.0-mpc-armv7.zip", "plugins/airwindows-matrixverb/thumb.ppm",
            "Classic algorithmic reverb with filter, damping, modulation and room size.", "./plugins/airwindows-matrixverb/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "algorithmic" }
        },
        {
            "airwindows-pitchdelay", "Airwindows PitchDelay", "mmiroshnikov", "v1.0.0", "Delay",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1096", "618 KB", "14%",
            "Airwindows - VST - PitchDelay", "pitchdelay.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/PitchDelay.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/PitchDelay-1.0.0-mpc-armv7.zip", "plugins/airwindows-pitchdelay/thumb.ppm",
            "Delay with pitch shifting in the feedback path and a resonant filter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - PitchDelay\" && if [ -f \"./plugins/airwindows-pitchdelay/pitchdelay.so\" ]; then cp -rf ./plugins/airwindows-pitchdelay/* \"$TARGET_SYNTH_DIR/Airwindows - VST - PitchDelay/\"; else (wget -q -O /tmp/airwindows-pitchdelay.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/PitchDelay-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-pitchdelay.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/PitchDelay-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/PitchDelay-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-pitchdelay.zip) && unzip -o /tmp/airwindows-pitchdelay.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - PitchDelay/\" && rm -f /tmp/airwindows-pitchdelay.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "delay", "pitch", "filter" }
        },
        {
            "airwindows-tapedelay2", "Airwindows TapeDelay2", "mmiroshnikov", "v1.0.0", "Delay",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1144", "548 KB", "14%",
            "Airwindows - VST - TapeDelay2", "tapedelay2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/TapeDelay2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/TapeDelay2-1.0.0-mpc-armv7.zip", "plugins/airwindows-tapedelay2/thumb.ppm",
            "Tape-style delay with flutter and a resonant filter in the repeats.", "./plugins/airwindows-tapedelay2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "delay", "tape", "flutter" }
        },
        {
            "airwindows-tovinyl4", "Airwindows ToVinyl4", "mmiroshnikov", "v1.0.0", "Utility",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1054", "738 KB", "14%",
            "Airwindows - VST - ToVinyl4", "tovinyl4.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ToVinyl4.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ToVinyl4-1.0.0-mpc-armv7.zip", "plugins/airwindows-tovinyl4/thumb.ppm",
            "Vinyl mastering stage: mid and side highpass, high limiter and groove wear.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ToVinyl4\" && if [ -f \"./plugins/airwindows-tovinyl4/tovinyl4.so\" ]; then cp -rf ./plugins/airwindows-tovinyl4/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ToVinyl4/\"; else (wget -q -O /tmp/airwindows-tovinyl4.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ToVinyl4-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-tovinyl4.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ToVinyl4-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ToVinyl4-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-tovinyl4.zip) && unzip -o /tmp/airwindows-tovinyl4.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ToVinyl4/\" && rm -f /tmp/airwindows-tovinyl4.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "vinyl", "mastering", "lo-fi" }
        },
        {
            "airwindows-verbity2", "Airwindows Verbity2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1050", "779 KB", "14%",
            "Airwindows - VST - Verbity2", "verbity2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Verbity2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Verbity2-1.0.0-mpc-armv7.zip", "plugins/airwindows-verbity2/thumb.ppm",
            "Dense, smooth reverb with size, sustain and mulch controls.", "./plugins/airwindows-verbity2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "reverb", "dense" }
        },
        {
            "airwindows-zacidlowpass", "Airwindows ZAcidLowpass", "mmiroshnikov", "v1.0.0", "Filter",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1025", "685 KB", "14%",
            "Airwindows - VST - ZAcidLowpass", "zacidlowpass.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ZAcidLowpass.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZAcidLowpass-1.0.0-mpc-armv7.zip", "plugins/airwindows-zacidlowpass/thumb.ppm",
            "Acid-style overdriven resonant lowpass filter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ZAcidLowpass\" && if [ -f \"./plugins/airwindows-zacidlowpass/zacidlowpass.so\" ]; then cp -rf ./plugins/airwindows-zacidlowpass/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ZAcidLowpass/\"; else (wget -q -O /tmp/airwindows-zacidlowpass.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZAcidLowpass-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/airwindows-zacidlowpass.zip \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZAcidLowpass-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZAcidLowpass-1.0.0-mpc-armv7.zip\" -o /tmp/airwindows-zacidlowpass.zip) && unzip -o /tmp/airwindows-zacidlowpass.zip -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ZAcidLowpass/\" && rm -f /tmp/airwindows-zacidlowpass.zip; fi && sync", 1,
            false,
            false,
            false,
            { "airwindows", "filter", "lowpass", "acid" }
        },
        {
            "airwindows-zlowpass2", "Airwindows ZLowpass2", "mmiroshnikov", "v1.0.0", "Filter",
            "FX", "Stable", "Akai MPC Live (3.9.0), Akai Force (3.9.0)", "MPC OS 3.x", "2026-10-03",
            "1020", "694 KB", "14%",
            "Airwindows - VST - ZLowpass2", "zlowpass2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ZLowpass2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZLowpass2-1.0.0-mpc-armv7.zip", "plugins/airwindows-zlowpass2/thumb.ppm",
            "Smooth lowpass filter with a choice of slopes.", "./plugins/airwindows-zlowpass2/install.sh", 0,
            false,
            false,
            false,
            { "airwindows", "filter", "lowpass" }
        },
        {
            "boris-granular", "Boris Granular", "sd88me", "v1.0.1", "Granular",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "1264", "10.3 MB", "14%",
            "sd88me - VST - Boris Granular", "boris_granular.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-boris/2d7b1360c68ccc91b22b0c5030152e08dd3682b8/docs/screenshot-force.png", "https://github.com/sd88me/mpc-vst-boris/releases/download/v1.0.1/Boris-Granular-1.0.1-mpc-armv7.zip", "plugins/boris-granular/thumb.ppm",
            "Real-time granular insert effect: incoming audio fills a 10 s buffer and up to 24 overlapping grains replay it with random size, delay, pitch, direction and pan. MPC tempo sync, freeze, feedback, 22 presets and a one-page touchscreen skin with Q-Links. A port of Boris Granular Station by Alessandro Gaiba.", "./plugins/boris-granular/install.sh", 0,
            false,
            false,
            false,
            { "granular", "freeze", "tempo-sync", "texture", "pads", "shimmer", "stutter" }
        },
        {
            "clif-tape", "Clif Tape", "MagicStino", "v0.2.0", "Effect",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "185", "1.6 MB", "14%",
            "MagicStino - VST - Clif Tape", "clif_tape.so",
            "https://raw.githubusercontent.com/MagicStino/Clif-Tape/v0.2.0/catalog-thumbnail.png", "https://github.com/MagicStino/Clif-Tape/releases/download/v0.2.0/Clif-Tape-0.2.0-mpc-armv7.zip", "plugins/clif-tape/thumb.ppm",
            "Stereo tape colour and digital degradation, expressive Wow/Flutter, twelve tone profiles, crackle, stereo spread, independent noise textures and Silence Gate. Fifteen presets and touchscreen Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MagicStino - VST - Clif Tape\" && if [ -f \"./plugins/clif-tape/clif_tape.so\" ]; then cp -rf ./plugins/clif-tape/* \"$TARGET_SYNTH_DIR/MagicStino - VST - Clif Tape/\"; else (wget -q -O /tmp/clif-tape.zip \"https://github.com/MagicStino/Clif-Tape/releases/download/v0.2.0/Clif-Tape-0.2.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/clif-tape.zip \"https://github.com/MagicStino/Clif-Tape/releases/download/v0.2.0/Clif-Tape-0.2.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/MagicStino/Clif-Tape/releases/download/v0.2.0/Clif-Tape-0.2.0-mpc-armv7.zip\" -o /tmp/clif-tape.zip) && unzip -o /tmp/clif-tape.zip -d \"$TARGET_SYNTH_DIR/MagicStino - VST - Clif Tape/\" && rm -f /tmp/clif-tape.zip; fi && sync", 1,
            false,
            false,
            false,
            { "tape", "lo-fi", "stereo", "effect" }
        },
        {
            "op1fx-cwo", "Cwo", "Bombest", "v0.4.1", "Delay",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "74", "4.2 MB", "14%",
            "bombest - VST - Cwo", "cwo.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/cwo.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cwo-0.4.1-mpc-armv7.zip", "plugins/op1fx-cwo/thumb.ppm",
            "Stereo delay with a frequency shifter in the feedback loop, plus a MIX knob. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Cwo\" && if [ -f \"./plugins/op1fx-cwo/cwo.so\" ]; then cp -rf ./plugins/op1fx-cwo/* \"$TARGET_SYNTH_DIR/bombest - VST - Cwo/\"; else (wget -q -O /tmp/op1fx-cwo.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cwo-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-cwo.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cwo-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cwo-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-cwo.zip) && unzip -o /tmp/op1fx-cwo.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Cwo/\" && rm -f /tmp/op1fx-cwo.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "delay", "frequency-shifter", "feedback" }
        },
        {
            "daemon-listen", "Daemon Listen", "sd88me", "v1.0.1", "Utility",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "448", "64 KB", "14%",
            "sd88me - VST - Daemon Listen", "daemonlisten.so",
            "", "https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-Listen-1.0.1-mpc-armv7.zip", "plugins/daemon-listen/thumb.ppm",
            "Companion to Daemon: insert it on the audio track that carries your mic, and Daemon's TALK button hears you. Passes audio through unchanged. Needs Daemon in the same project.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon Listen\" && if [ -f \"./plugins/daemon-listen/daemonlisten.so\" ]; then cp -rf ./plugins/daemon-listen/* \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon Listen/\"; else (wget -q -O /tmp/daemon-listen.zip \"https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-Listen-1.0.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/daemon-listen.zip \"https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-Listen-1.0.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-Listen-1.0.1-mpc-armv7.zip\" -o /tmp/daemon-listen.zip) && unzip -o /tmp/daemon-listen.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon Listen/\" && rm -f /tmp/daemon-listen.zip; fi && sync", 1,
            false,
            false,
            false,
            { "voice", "mic", "ai" }
        },
        {
            "op1fx-delay", "Delay", "Bombest", "v0.4.1", "Delay",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "73", "4.2 MB", "14%",
            "bombest - VST - Delay", "delay.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/delay.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Delay-0.4.1-mpc-armv7.zip", "plugins/op1fx-delay/thumb.ppm",
            "Four-knob delay with a 12-preset bank. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Delay\" && if [ -f \"./plugins/op1fx-delay/delay.so\" ]; then cp -rf ./plugins/op1fx-delay/* \"$TARGET_SYNTH_DIR/bombest - VST - Delay/\"; else (wget -q -O /tmp/op1fx-delay.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Delay-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-delay.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Delay-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Delay-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-delay.zip) && unzip -o /tmp/op1fx-delay.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Delay/\" && rm -f /tmp/op1fx-delay.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "delay", "echo", "tape" }
        },
        {
            "dragonfly-early-reflections", "Dragonfly Early Reflections", "gmorb", "v1.3.1", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "166", "766 KB", "14%",
            "Dragonfly - VST - Early Reflections", "dragonfly_early.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-early-reflections/2044beed9f510a3963df7b61da6a4c19c6c7e7b3/docs/banners/early.png", "https://github.com/gmorb/mpc-vst-dragonfly-early-reflections/releases/download/v1.3.1/Early-Reflections-1.3.1-mpc-armv7.zip", "plugins/dragonfly-early-reflections/thumb.ppm",
            "Dragonfly Early Reflections as a native insert effect, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Dragonfly - VST - Early Reflections\" && if [ -f \"./plugins/dragonfly-early-reflections/dragonfly_early.so\" ]; then cp -rf ./plugins/dragonfly-early-reflections/* \"$TARGET_SYNTH_DIR/Dragonfly - VST - Early Reflections/\"; else (wget -q -O /tmp/dragonfly-early-reflections.zip \"https://github.com/gmorb/mpc-vst-dragonfly-early-reflections/releases/download/v1.3.1/Early-Reflections-1.3.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/dragonfly-early-reflections.zip \"https://github.com/gmorb/mpc-vst-dragonfly-early-reflections/releases/download/v1.3.1/Early-Reflections-1.3.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/gmorb/mpc-vst-dragonfly-early-reflections/releases/download/v1.3.1/Early-Reflections-1.3.1-mpc-armv7.zip\" -o /tmp/dragonfly-early-reflections.zip) && unzip -o /tmp/dragonfly-early-reflections.zip -d \"$TARGET_SYNTH_DIR/Dragonfly - VST - Early Reflections/\" && rm -f /tmp/dragonfly-early-reflections.zip; fi && sync", 1,
            false,
            false,
            false,
            { "reverb", "early-reflections", "dragonfly" }
        },
        {
            "dragonfly-hall", "Dragonfly Hall", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "1002", "2.4 MB", "14%",
            "Dragonfly - VST - Hall", "dragonfly_hall.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-hall/eb9f41f22ea93414776db40760c2387b36cf3144/docs/banners/hall.png", "https://github.com/gmorb/mpc-vst-dragonfly-hall/releases/download/v1.3.0/Hall-1.3.0-mpc-armv7.zip", "plugins/dragonfly-hall/thumb.ppm",
            "Dragonfly Hall reverb as a native insert effect, with a touchscreen skin and Q-Links.", "./plugins/dragonfly-hall/install.sh", 0,
            false,
            false,
            false,
            { "reverb", "hall", "dragonfly" }
        },
        {
            "dragonfly-plate", "Dragonfly Plate", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "973", "1.2 MB", "14%",
            "Dragonfly - VST - Plate", "dragonfly_plate.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-plate/a9087385948d18ae38739c44ddf42ab826d1a567/docs/banners/plate.png", "https://github.com/gmorb/mpc-vst-dragonfly-plate/releases/download/v1.3.0/Plate-1.3.0-mpc-armv7.zip", "plugins/dragonfly-plate/thumb.ppm",
            "Dragonfly Plate reverb as a native insert effect, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Dragonfly - VST - Plate\" && if [ -f \"./plugins/dragonfly-plate/dragonfly_plate.so\" ]; then cp -rf ./plugins/dragonfly-plate/* \"$TARGET_SYNTH_DIR/Dragonfly - VST - Plate/\"; else (wget -q -O /tmp/dragonfly-plate.zip \"https://github.com/gmorb/mpc-vst-dragonfly-plate/releases/download/v1.3.0/Plate-1.3.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/dragonfly-plate.zip \"https://github.com/gmorb/mpc-vst-dragonfly-plate/releases/download/v1.3.0/Plate-1.3.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/gmorb/mpc-vst-dragonfly-plate/releases/download/v1.3.0/Plate-1.3.0-mpc-armv7.zip\" -o /tmp/dragonfly-plate.zip) && unzip -o /tmp/dragonfly-plate.zip -d \"$TARGET_SYNTH_DIR/Dragonfly - VST - Plate/\" && rm -f /tmp/dragonfly-plate.zip; fi && sync", 1,
            false,
            false,
            false,
            { "reverb", "plate", "dragonfly" }
        },
        {
            "dragonfly-room", "Dragonfly Room", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "964", "2.0 MB", "14%",
            "Dragonfly - VST - Room", "dragonfly_room.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-room/6c62e1a5c9ef57f0c31897afa0faf07d7a446585/docs/banners/room.png", "https://github.com/gmorb/mpc-vst-dragonfly-room/releases/download/v1.3.0/Room-1.3.0-mpc-armv7.zip", "plugins/dragonfly-room/thumb.ppm",
            "Dragonfly Room reverb as a native insert effect, with a touchscreen skin and Q-Links.", "./plugins/dragonfly-room/install.sh", 0,
            false,
            false,
            false,
            { "reverb", "room", "dragonfly" }
        },
        {
            "drips", "Drips", "Padbangers", "v1.0.0", "Granular",
            "FX", "Stable", "Akai MPC X (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-13",
            "619", "3.2 MB", "10%",
            "Padbangers - VST - Drips", "drips.so",
            "https://raw.githubusercontent.com/padbangers/drips/85683cebf7b4fcf1716ed6f32faa89041768b24e/docs/drips-main.png", "https://github.com/padbangers/drips/releases/download/drips-v1.0.0/Drips-1.0.0-mpc-armv7.zip", "plugins/drips/thumb.ppm",
            "Drips: a granular texture synthesizer with four recording qualities, a delay and beat slicer, tempo-synced seeding, wavetables and a modulation page, from Émilie Gillet's open-source Beads.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Padbangers - VST - Drips\" && if [ -f \"./plugins/drips/drips.so\" ]; then cp -rf ./plugins/drips/* \"$TARGET_SYNTH_DIR/Padbangers - VST - Drips/\"; else (wget -q -O /tmp/drips.zip \"https://github.com/padbangers/drips/releases/download/drips-v1.0.0/Drips-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/drips.zip \"https://github.com/padbangers/drips/releases/download/drips-v1.0.0/Drips-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/padbangers/drips/releases/download/drips-v1.0.0/Drips-1.0.0-mpc-armv7.zip\" -o /tmp/drips.zip) && unzip -o /tmp/drips.zip -d \"$TARGET_SYNTH_DIR/Padbangers - VST - Drips/\" && rm -f /tmp/drips.zip; fi && sync", 1,
            false,
            false,
            false,
            { "granular", "texture", "delay", "beat-slicer", "wavetable", "reverb", "tempo-sync" }
        },
        {
            "dub-station", "Dub Station", "nachtaktiv303", "v1.0.2", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1275", "2.1 MB", "14%",
            "Lukas - VST - Dub Station", "dub_station.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-dub-station/main/preview_0.png", "https://github.com/nachtaktiv303/mpc-vst-dub-station/releases/download/dub-station-vst-v1.0.2/Dub-Station-1.0.2-mpc-armv7.zip", "plugins/dub-station/thumb.ppm",
            "Dub-techno multi-effect: two independent tape delays, a huge modulated reverb, tape grit, a noise/crackle ambience bed, phaser, ducking and a panic kill.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Lukas - VST - Dub Station\" && if [ -f \"./plugins/dub-station/dub_station.so\" ]; then cp -rf ./plugins/dub-station/* \"$TARGET_SYNTH_DIR/Lukas - VST - Dub Station/\"; else (wget -q -O /tmp/dub-station.zip \"https://github.com/nachtaktiv303/mpc-vst-dub-station/releases/download/dub-station-vst-v1.0.2/Dub-Station-1.0.2-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/dub-station.zip \"https://github.com/nachtaktiv303/mpc-vst-dub-station/releases/download/dub-station-vst-v1.0.2/Dub-Station-1.0.2-mpc-armv7.zip\" || curl -k -sL \"https://github.com/nachtaktiv303/mpc-vst-dub-station/releases/download/dub-station-vst-v1.0.2/Dub-Station-1.0.2-mpc-armv7.zip\" -o /tmp/dub-station.zip) && unzip -o /tmp/dub-station.zip -d \"$TARGET_SYNTH_DIR/Lukas - VST - Dub Station/\" && rm -f /tmp/dub-station.zip; fi && sync", 1,
            false,
            false,
            false,
            { "dub", "delay", "tape", "reverb", "space-echo", "multi-effect" }
        },
        {
            "effectwerk", "EffectWerk", "Devko", "v0.0.5", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "149", "3.0 MB", "14%",
            "Devko - VST - EffectWerk", "effectwerk.so",
            "https://raw.githubusercontent.com/Devko/EffectWerk/v0.0.1/docs/img/looper.png", "https://github.com/Devko/EffectWerk/releases/download/v0.0.5/EffectWerk-0.0.5-mpc-armv7.zip", "plugins/effectwerk/thumb.ppm",
            "A live looper and performance mixer in one insert slot: record bars on MPC's grid, then roll, slow, reverse and layer them, and crossfade two scenes from 64 one-tap effects. Plus a ten-module effect rack with macros, LFOs and a mod matrix, 62 presets and a 14-page touchscreen skin with Q-Links.", "./plugins/effectwerk/install.sh", 0,
            false,
            false,
            false,
            { "looper", "performance-mixer", "octatrack-style", "crossfader", "scenes", "effect-rack", "granular", "shimmer-reverb" }
        },
        {
            "ghost", "GHOST", "MPC-EURO", "v2.4.2", "Glitch",
            "FX", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "45", "812 KB", "14%",
            "MPC-EURO - VST - GHOST", "ghost.so",
            "", "https://github.com/carballocba/MPC-EURO-/releases/download/v2.4.2-installer2-rc3/GHOST-2.4.2-installer2-rc3-mpc-armv7.zip", "plugins/ghost/thumb.ppm",
            "Live buffer manipulation, rhythmic stutter and generative glitch with independent and combined Random Repeat modes.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MPC-EURO - VST - GHOST\" && if [ -f \"./plugins/ghost/ghost.so\" ]; then cp -rf ./plugins/ghost/* \"$TARGET_SYNTH_DIR/MPC-EURO - VST - GHOST/\"; else (wget -q -O /tmp/ghost.zip \"https://github.com/carballocba/MPC-EURO-/releases/download/v2.4.2-installer2-rc3/GHOST-2.4.2-installer2-rc3-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/ghost.zip \"https://github.com/carballocba/MPC-EURO-/releases/download/v2.4.2-installer2-rc3/GHOST-2.4.2-installer2-rc3-mpc-armv7.zip\" || curl -k -sL \"https://github.com/carballocba/MPC-EURO-/releases/download/v2.4.2-installer2-rc3/GHOST-2.4.2-installer2-rc3-mpc-armv7.zip\" -o /tmp/ghost.zip) && unzip -o /tmp/ghost.zip -d \"$TARGET_SYNTH_DIR/MPC-EURO - VST - GHOST/\" && rm -f /tmp/ghost.zip; fi && sync", 1,
            false,
            false,
            false,
            { "buffer", "stutter", "reverse", "generative", "performance" }
        },
        {
            "op1fx-grid", "Grid", "Bombest", "v0.4.1", "Reverb",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "73", "9.5 MB", "14%",
            "bombest - VST - Grid", "grid.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/grid.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Grid-0.4.1-mpc-armv7.zip", "plugins/op1fx-grid/thumb.ppm",
            "A 2D waveguide mesh you feed audio and read back. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Grid\" && if [ -f \"./plugins/op1fx-grid/grid.so\" ]; then cp -rf ./plugins/op1fx-grid/* \"$TARGET_SYNTH_DIR/bombest - VST - Grid/\"; else (wget -q -O /tmp/op1fx-grid.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Grid-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-grid.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Grid-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Grid-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-grid.zip) && unzip -o /tmp/op1fx-grid.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Grid/\" && rm -f /tmp/op1fx-grid.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "reverb", "waveguide", "resonator" }
        },
        {
            "incipience-out", "Incipience OUT", "saustin2010", "v1.1.1", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "600", "104 KB", "14%",
            "Steve A - VST - [FX] Incipience OUT", "incipienceout.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-incipience/v1.1.1/incipienceout/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-incipience/releases/download/incipience-out-v1.1.1/FX-Incipience-OUT-1.1.1-mpc-armv7.zip", "plugins/incipience-out/thumb.ppm",
            "Companion to Incipience: plays one of its OUT 1-11 on its own MPC track (an audio track will do), so an instrument gets that track's fader, inserts and outputs, in time with the kit. Needs Incipience 1.0.0 or later in the same project.", "./plugins/incipience-out/install.sh", 0,
            false,
            false,
            false,
            { "routing", "multi-out", "drum" }
        },
        {
            "keyscope", "Keyscope", "jacob-sabella", "v1.0.4", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "1970", "3.2 MB", "4%",
            "jacob-sabella - VST - Keyscope", "keyscope.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-keyscope/88cb99ac5e0d682e1e6d9c81836bf4096a9cde3c/docs/screenshots/keyscope.png", "https://github.com/jacob-sabella/mpc-vst-keyscope/releases/download/keyscope-vst-v1.0.4/Keyscope-1.0.4-mpc-armv7.zip", "plugins/keyscope/thumb.ppm",
            "Listens to any track and names its key, chord, bass note and tuning, from the spectrum of the whole mix; the audio passes through untouched.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Keyscope\" && if [ -f \"./plugins/keyscope/keyscope.so\" ]; then cp -rf ./plugins/keyscope/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Keyscope/\"; else (wget -q -O /tmp/keyscope.zip \"https://github.com/jacob-sabella/mpc-vst-keyscope/releases/download/keyscope-vst-v1.0.4/Keyscope-1.0.4-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/keyscope.zip \"https://github.com/jacob-sabella/mpc-vst-keyscope/releases/download/keyscope-vst-v1.0.4/Keyscope-1.0.4-mpc-armv7.zip\" || curl -k -sL \"https://github.com/jacob-sabella/mpc-vst-keyscope/releases/download/keyscope-vst-v1.0.4/Keyscope-1.0.4-mpc-armv7.zip\" -o /tmp/keyscope.zip) && unzip -o /tmp/keyscope.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Keyscope/\" && rm -f /tmp/keyscope.zip; fi && sync", 1,
            false,
            false,
            false,
            { "key-detection", "chord-detection", "tuner", "analyzer" }
        },
        {
            "liminal-amp", "Liminal Amp", "gmorb", "v1.0.2", "Amp Sim",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1059", "5.5 MB", "14%",
            "Gm0rb - VST - Liminal Amp", "liminal_amp.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-liminal-amp/v1.0.0/docs/images/page.png", "https://github.com/gmorb/mpc-vst-liminal-amp/releases/download/v1.0.2/Liminal-Amp-1.0.2-mpc-armv7.zip", "plugins/liminal-amp/thumb.ppm",
            "Lightweight, official-sound Neural Amp Modeler: A2 captures with cab IR, gate and tone stack, resampled like the official plugin; browses models/ and irs/ folders on any drive.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Gm0rb - VST - Liminal Amp\" && if [ -f \"./plugins/liminal-amp/liminal_amp.so\" ]; then cp -rf ./plugins/liminal-amp/* \"$TARGET_SYNTH_DIR/Gm0rb - VST - Liminal Amp/\"; else (wget -q -O /tmp/liminal-amp.zip \"https://github.com/gmorb/mpc-vst-liminal-amp/releases/download/v1.0.2/Liminal-Amp-1.0.2-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/liminal-amp.zip \"https://github.com/gmorb/mpc-vst-liminal-amp/releases/download/v1.0.2/Liminal-Amp-1.0.2-mpc-armv7.zip\" || curl -k -sL \"https://github.com/gmorb/mpc-vst-liminal-amp/releases/download/v1.0.2/Liminal-Amp-1.0.2-mpc-armv7.zip\" -o /tmp/liminal-amp.zip) && unzip -o /tmp/liminal-amp.zip -d \"$TARGET_SYNTH_DIR/Gm0rb - VST - Liminal Amp/\" && rm -f /tmp/liminal-amp.zip; fi && sync", 1,
            false,
            false,
            false,
            { "nam", "guitar", "amp", "cab-ir", "lightweight" }
        },
        {
            "liminal-hz", "Liminal Hz", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-24",
            "1831", "22.4 MB", "14%",
            "Gm0rb - VST - Liminal Hz", "liminal_hz.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-liminal-hz/main/docs/images/banner.png", "https://github.com/gmorb/mpc-vst-liminal-hz/releases/download/v1.3.0/Liminal-Hz-1.3.0-mpc-armv7.zip", "plugins/liminal-hz/thumb.ppm",
            "Real rooms, halls and echoes from impulse responses (up to 5 s, mono or stereo), the space for your NAM amp; ~3.5% of an audio block on a Force; browses reverbs/ folders and TONE3000.", "./plugins/liminal-hz/install.sh", 0,
            false,
            false,
            false,
            { "reverb", "delay", "echo", "convolution", "ir", "tone3000" }
        },
        {
            "loop-saboteurxxl", "Loop-SaboteurXXL", "L'Cronx", "v0.1.3", "Glitch",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "1074", "8.2 MB", "14%",
            "L'Cronx - VST - Loop-SaboteurXXL", "loop_saboteurxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Loop-SaboteurXXL/v0.1.0/docs/img/3-acts-seq.png", "https://github.com/sunskiefer/Loop-SaboteurXXL/releases/download/v0.1.3/Loop-SaboteurXXL-0.1.3-mpc-armv7.zip", "plugins/loop-saboteurxxl/thumb.ppm",
            "A port of Loop Saboteur by Stephen McLeod Blythe (clickysteve): a glitch / chop / beat-slicer that records the last bars and replays them on the MPC tempo as retriggered, pitched, reversed, stretched and crushed slices. Eight Acts, a 64-step sequencer with trig conditions, ratchets and p-locks, and 19 factory presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - Loop-SaboteurXXL\" && if [ -f \"./plugins/loop-saboteurxxl/loop_saboteurxxl.so\" ]; then cp -rf ./plugins/loop-saboteurxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - Loop-SaboteurXXL/\"; else (wget -q -O /tmp/loop-saboteurxxl.zip \"https://github.com/sunskiefer/Loop-SaboteurXXL/releases/download/v0.1.3/Loop-SaboteurXXL-0.1.3-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/loop-saboteurxxl.zip \"https://github.com/sunskiefer/Loop-SaboteurXXL/releases/download/v0.1.3/Loop-SaboteurXXL-0.1.3-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/Loop-SaboteurXXL/releases/download/v0.1.3/Loop-SaboteurXXL-0.1.3-mpc-armv7.zip\" -o /tmp/loop-saboteurxxl.zip) && unzip -o /tmp/loop-saboteurxxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - Loop-SaboteurXXL/\" && rm -f /tmp/loop-saboteurxxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "glitch", "slicer", "stutter", "beat-repeat", "sequencer", "p-lock", "lo-fi", "tape" }
        },
        {
            "machinemodule-tap-gen2", "Machinemodule Tap (Gen2)", "sd88me", "v1.0.3", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-04",
            "296", "95 KB", "14%",
            "sd88me - VST - Machinemodule Tap", "machinedrum_tapfx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule-tap-gen2.png", "https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.3/Machinemodule-Tap-1.0.3-mpc-aarch64.zip", "plugins/machinemodule-tap-gen2/thumb.ppm",
            "Puts any Machinemodule tracks, and its reverb and delay sends, on their own MPC return or FX track, so MPC's mixer and effects can process them. Needs Machinemodule in the same project. Gen2 (aarch64) only.", "./plugins/machinemodule-tap-gen2/install.sh", 0,
            false,
            false,
            false,
            { "elektron", "machinedrum", "gen2" }
        },
        {
            "magic-tape-echo", "Magic Tape Echo", "MagicStino", "v0.1.2", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "444", "2.6 MB", "14%",
            "MagicStino - VST - Magic Tape Echo", "magic_tape_echo.so",
            "https://raw.githubusercontent.com/MagicStino/Magic-Tape-Echo/a616cc2fad6bd7eb920111627bb1fc81631ff09a/magic-tape-echo-three-heads.png", "https://github.com/MagicStino/Magic-Tape-Echo/releases/download/v0.1.2/Magic-Tape-Echo-0.1.2-mpc-armv7.zip", "plugins/magic-tape-echo/thumb.ppm",
            "Three-head stereo tape echo with tape saturation, wow/flutter, tempo sync, optional spring reverb and a slow VU meter.", "./plugins/magic-tape-echo/install.sh", 0,
            false,
            false,
            false,
            { "delay", "tape", "echo", "spring" }
        },
        {
            "warps", "Meta Mod", "saustin2010", "v1.2.1", "Modulation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1503", "1.0 MB", "14%",
            "Émilie Gillet - VST - [FX] Meta Mod", "warps.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-meta-mod/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-meta-mod/releases/download/v1.2.1/FX-Meta-Mod-1.2.1-mpc-armv7.zip", "plugins/warps/thumb.ppm",
            "Meta Mod: ring mod, folding, XOR, vocoder and frequency shifting as an insert effect; 12 presets, from Émilie Gillet's open-source Warps.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Meta Mod\" && if [ -f \"./plugins/warps/warps.so\" ]; then cp -rf ./plugins/warps/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Meta Mod/\"; else (wget -q -O /tmp/warps.zip \"https://github.com/saustin2010/mpc-vst-meta-mod/releases/download/v1.2.1/FX-Meta-Mod-1.2.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/warps.zip \"https://github.com/saustin2010/mpc-vst-meta-mod/releases/download/v1.2.1/FX-Meta-Mod-1.2.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-meta-mod/releases/download/v1.2.1/FX-Meta-Mod-1.2.1-mpc-armv7.zip\" -o /tmp/warps.zip) && unzip -o /tmp/warps.zip -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Meta Mod/\" && rm -f /tmp/warps.zip; fi && sync", 1,
            false,
            false,
            false,
            { "ring-mod", "vocoder", "frequency-shifter", "eurorack" }
        },
        {
            "meta4mpcxxl-fx", "Meta4MPCXXL FX", "L'Cronx", "v0.1.2", "Modular",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "340", "11.2 MB", "14%",
            "L'Cronx - VST - Meta4MPCXXL FX", "meta4mpcxxl_fx.so",
            "https://raw.githubusercontent.com/sunskiefer/Meta4MPCXXL/v0.1.1/docs/img/meta4mpcxxl.png", "https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-FX-0.1.2-mpc-armv7.zip", "plugins/meta4mpcxxl-fx/thumb.ppm",
            "Meta4MPCXXL as an effect: the track's audio goes into a 4ms MetaModule-format (VCV Rack) patch's In 1 / In 2, with every parameter on Q-Links, MIDI and clock into the patch, and Wi-Fi patch upload from VCV Rack.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL FX\" && if [ -f \"./plugins/meta4mpcxxl-fx/meta4mpcxxl_fx.so\" ]; then cp -rf ./plugins/meta4mpcxxl-fx/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL FX/\"; else (wget -q -O /tmp/meta4mpcxxl-fx.zip \"https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-FX-0.1.2-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/meta4mpcxxl-fx.zip \"https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-FX-0.1.2-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-FX-0.1.2-mpc-armv7.zip\" -o /tmp/meta4mpcxxl-fx.zip) && unzip -o /tmp/meta4mpcxxl-fx.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL FX/\" && rm -f /tmp/meta4mpcxxl-fx.zip; fi && sync", 1,
            false,
            false,
            false,
            { "modular", "vcv", "metamodule", "patch-player", "eurorack", "multi-fx" }
        },
        {
            "monomodule-fx-gen2", "Monomodule FX (Gen2)", "sd88me", "v1.0.4", "Multi Effect",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-04",
            "218", "4.2 MB", "14%",
            "sd88me - VST - Monomodule FX", "monomodule_fx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-monomodule/main/docs/img/monomodule-fx-gen2.png", "https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-FX-1.0.4-mpc-aarch64.zip", "plugins/monomodule-fx-gen2/thumb.ppm",
            "The Elektron Monomachine's seven effect machines (thru, reverb, chorus, dynamix, ring mod, phaser, flanger) as an audio effect for Gen2 MPC devices, with a touchscreen skin and Q-Links. A ready-made download with nothing of Elektron's in it: it reads your own Monomachine OS file from the plugin folder. Gen2 only.", "./plugins/monomodule-fx-gen2/install.sh", 0,
            false,
            false,
            false,
            { "elektron", "monomachine", "gen2" }
        },
        {
            "mpc-dj-fx-2", "MPC DJ FX 2", "mitchoster", "v3.1.6", "Multi Fx",
            "FX", "Stable", "MPC Live II (MPC OS 3.x (ACVS-MPC3 build))", "MPC OS 3.x", "2026-11-03",
            "731", "5.1 MB", "30%",
            "LarryDaimond - VST - MPC DJ FX 2", "mpc_djfx2.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-djfx2/0f20d92f/docs/ui-preview/page_1.png", "https://github.com/mitchoster/mpc-vst-djfx2/releases/download/v3.1.6/MPC-DJ-FX-2-3.1.6-mpc-armv7.zip", "plugins/mpc-dj-fx-2/thumb.ppm",
            "One-slot live DJ insert for MPC/Force: 3-band isolator, DJ filter, beat repeat, delay, reverb, through-zero flanger, grain delay, limiter, and eight performance macros measured from Lenny Kiser's DJ-FX rack.", "./plugins/mpc-dj-fx-2/install.sh", 0,
            false,
            false,
            false,
            { "dj", "isolator", "filter", "beat-repeat", "delay", "reverb", "performance", "macro" }
        },
        {
            "mpc-multihead-echo", "MPC Multihead Echo", "mitchoster", "v1.0.2", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-02",
            "123", "1.7 MB", "14%",
            "LarryDaimond - VST - MPC Multihead Echo", "mpc_multihead_echo.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-multihead-echo/c31c3ef479ce87083ab87fbef14c2fe92a6bb83b/docs/companion-panel-echo.png", "https://github.com/mitchoster/mpc-vst-multihead-echo/releases/download/v1.0.2/MPC-Multihead-Echo-1.0.2-mpc-armv7.zip", "plugins/mpc-multihead-echo/thumb.ppm",
            "Three-head tape echo with three preamp characters, chorus, tape wear, tempo sync with drift, a spring reverb and a companion screen in the browser.", "./plugins/mpc-multihead-echo/install.sh", 0,
            false,
            false,
            false,
            { "tape-echo", "delay", "saturation", "spring-reverb", "tempo-sync", "companion-app" }
        },
        {
            "mpc-tape-echo", "MPC Tape Echo", "mitchoster", "v1.2.3", "Delay",
            "FX", "Stable", "MPC Live II (MPC OS 3.x (ACVS-MPC3 build))", "MPC OS 3.x", "2026-11-02",
            "489", "11.6 MB", "14%",
            "LarryDaimond - VST - MPC Tape Echo", "mpc_tape_echo.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-tape-echo/main/docs/img/panel.png", "https://github.com/mitchoster/mpc-vst-tape-echo/releases/download/v1.2.3/MPC-Tape-Echo-1.2.3-mpc-armv7.zip", "plugins/mpc-tape-echo/thumb.ppm",
            "Three-head tape echo with a spring tank: 12 head and reverb modes, tape saturation, wow and flutter, tempo sync, live VU.", "./plugins/mpc-tape-echo/install.sh", 0,
            false,
            false,
            false,
            { "delay", "tape", "echo", "spring" }
        },
        {
            "nam", "NAM", "jacob-sabella", "v1.0.10", "Amp Sim",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "3533", "2.8 MB", "14%",
            "jacob-sabella - VST - NAM", "nam_vst.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-nam/f2fa28c8f4086606e929732a4824e72579bdcf80/docs/screenshots/nam.png", "https://github.com/jacob-sabella/mpc-vst-nam/releases/download/v1.0.10/NAM-1.0.10-mpc-armv7.zip", "plugins/nam/thumb.ppm",
            "Neural Amp Modeler captures as an insert effect, with cab IR, tone, gate, pitch, delay, reverb and an on-device TONE3000 downloader.", "./plugins/nam/install.sh", 0,
            false,
            false,
            false,
            { "nam", "guitar", "amp", "cab-ir", "tone3000" }
        },
        {
            "op1fx-nitro", "Nitro", "Bombest", "v0.4.1", "Filter",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "68", "4.9 MB", "14%",
            "bombest - VST - Nitro", "nitro.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/nitro.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Nitro-0.4.1-mpc-armv7.zip", "plugins/op1fx-nitro/thumb.ppm",
            "Filter with a drive stage and a MIX knob, 12 presets. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Nitro\" && if [ -f \"./plugins/op1fx-nitro/nitro.so\" ]; then cp -rf ./plugins/op1fx-nitro/* \"$TARGET_SYNTH_DIR/bombest - VST - Nitro/\"; else (wget -q -O /tmp/op1fx-nitro.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Nitro-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-nitro.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Nitro-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Nitro-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-nitro.zip) && unzip -o /tmp/op1fx-nitro.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Nitro/\" && rm -f /tmp/op1fx-nitro.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "filter", "drive", "dj" }
        },
        {
            "ottmpc", "OTTmpc", "sd88me", "v1.0.1", "Compressor",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "1145", "1.7 MB", "14%",
            "sd88me - VST - OTTmpc", "ottmpc.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-OTT/claude/friendly-wozniak-m9ak1l/docs/screenshot.png", "https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v1.0.1/OTTmpc-1.0.1-mpc-armv7.zip", "plugins/ottmpc/thumb.ppm",
            "Vital's OTT multiband up/down compressor as an insert effect: live band meters with dB numbers, 12 Q-Link controls and 10 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - OTTmpc\" && if [ -f \"./plugins/ottmpc/ottmpc.so\" ]; then cp -rf ./plugins/ottmpc/* \"$TARGET_SYNTH_DIR/sd88me - VST - OTTmpc/\"; else (wget -q -O /tmp/ottmpc.zip \"https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v1.0.1/OTTmpc-1.0.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/ottmpc.zip \"https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v1.0.1/OTTmpc-1.0.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v1.0.1/OTTmpc-1.0.1-mpc-armv7.zip\" -o /tmp/ottmpc.zip) && unzip -o /tmp/ottmpc.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - OTTmpc/\" && rm -f /tmp/ottmpc.zip; fi && sync", 1,
            false,
            false,
            false,
            { "ott", "multiband", "compressor", "dynamics" }
        },
        {
            "overcast", "Overcast", "Padbangers", "v1.0.6", "Granular",
            "FX", "Stable", "Akai MPC X (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-04",
            "3764", "3.8 MB", "9%",
            "Padbangers - VST - Overcast", "overcast.so",
            "https://raw.githubusercontent.com/FullPace/overcast/overcast-v1.0.6/docs/overcast-main.png", "https://github.com/padbangers/overcast/releases/download/overcast-v1.0.6/Overcast-1.0.6-mpc-armv7.zip", "plugins/overcast/thumb.ppm",
            "Mutable Instruments Clouds (Parasites firmware, six modes) with a modulation page: two envelopes, two LFOs and an 8-slot matrix onto Clouds' CV inputs.", "./plugins/overcast/install.sh", 0,
            false,
            false,
            false,
            { "mutable-instruments", "clouds", "parasites", "granular", "texture", "reverb", "delay", "eurorack" }
        },
        {
            "percolator-out", "Percolator OUT", "saustin2010", "v2.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "870", "147 KB", "14%",
            "Steve A - VST - [FX] Percolator OUT", "percolatorout.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-percolator/v2.1.0/percolatorout/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-percolator/releases/download/percolator-out-v2.1.0/FX-Percolator-OUT-2.1.0-mpc-armv7.zip", "plugins/percolator-out/thumb.ppm",
            "Companion to Percolator: plays one of its OUT 1-4 on its own MPC track (an audio track will do), so a voice gets that track's fader, inserts and outputs, in time with the kit. Needs Percolator 2.0.0 or later in the same project.", "./plugins/percolator-out/install.sh", 0,
            false,
            false,
            false,
            { "routing", "multi-out", "drum" }
        },
        {
            "op1fx-phone", "Phone", "Bombest", "v0.4.1", "Filter",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "69", "5.1 MB", "14%",
            "bombest - VST - Phone", "phone.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/phone.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phone-0.4.1-mpc-armv7.zip", "plugins/op1fx-phone/thumb.ppm",
            "Telephone-band squash with a 12-preset bank. OP-1 inspired.", "./plugins/op1fx-phone/install.sh", 0,
            false,
            false,
            false,
            { "op1", "telephone", "band-limit", "lo-fi" }
        },
        {
            "phyxxl", "PhyXXL", "L'Cronx", "v0.1.6", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-25",
            "653", "18.3 MB", "14%",
            "L'Cronx - VST - PhyXXL", "phyxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PhyXXL/v0.1.2/docs/img/overview.png", "https://github.com/sunskiefer/PhyXXL/releases/download/v0.1.6/PhyXXL-0.1.6-mpc-armv7.zip", "plugins/phyxxl/thumb.ppm",
            "Eight filters in the style of classic synths (MS-20, SEM, Minimoog, Matrix-12, 2600, Wasp, prologue, Polivoks) in one effect. Route audio from any filter into any other; modulate each from ten sources by touch patch bay or matrix. 28 patches. By L'Cronx, GPL-3. No third-party code; not endorsed by the makers of the originals.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - PhyXXL\" && if [ -f \"./plugins/phyxxl/phyxxl.so\" ]; then cp -rf ./plugins/phyxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - PhyXXL/\"; else (wget -q -O /tmp/phyxxl.zip \"https://github.com/sunskiefer/PhyXXL/releases/download/v0.1.6/PhyXXL-0.1.6-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/phyxxl.zip \"https://github.com/sunskiefer/PhyXXL/releases/download/v0.1.6/PhyXXL-0.1.6-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/PhyXXL/releases/download/v0.1.6/PhyXXL-0.1.6-mpc-armv7.zip\" -o /tmp/phyxxl.zip) && unzip -o /tmp/phyxxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - PhyXXL/\" && rm -f /tmp/phyxxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "filter", "multi-filter", "ms-20", "sem", "moog", "matrix-12", "arp-2600", "polivoks" }
        },
        {
            "plateauxxl", "PlateauXXL", "L'Cronx", "v0.4.3", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "1459", "12.9 MB", "14%",
            "L'Cronx - VST - PlateauXXL", "plateauxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PlateauXXL/v0.4.1/docs/img/plateau.png", "https://github.com/sunskiefer/PlateauXXL/releases/download/v0.4.3/PlateauXXL-0.4.3-mpc-armv7.zip", "plugins/plateauxxl/thumb.ppm",
            "Valley Audio's Plateau reverb from VCV Rack, patched: every CV input on a built-in source (four Bogaudio LFOs, Tidal Modulator 2, Random Sampler, step and gate sequencers) synced to the MPC tempo, a brickwall limiter, Panic and 16 presets, each page drawn like its module.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - PlateauXXL\" && if [ -f \"./plugins/plateauxxl/plateauxxl.so\" ]; then cp -rf ./plugins/plateauxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - PlateauXXL/\"; else (wget -q -O /tmp/plateauxxl.zip \"https://github.com/sunskiefer/PlateauXXL/releases/download/v0.4.3/PlateauXXL-0.4.3-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/plateauxxl.zip \"https://github.com/sunskiefer/PlateauXXL/releases/download/v0.4.3/PlateauXXL-0.4.3-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/PlateauXXL/releases/download/v0.4.3/PlateauXXL-0.4.3-mpc-armv7.zip\" -o /tmp/plateauxxl.zip) && unzip -o /tmp/plateauxxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - PlateauXXL/\" && rm -f /tmp/plateauxxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "reverb", "plate", "dattorro", "valley", "vcv", "freeze", "lfo", "modulation" }
        },
        {
            "potentialpatina", "PotentialPatina", "PotentialBreaks", "v1.0.2", "Saturation",
            "FX", "Stable", "MPC One (HAKAI 3.10)", "MPC OS 3.x", "2026-11-05",
            "4", "428 KB", "14%",
            "PotentialBreaks - VST - PotentialPatina", "potentialpatina.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialpatina/a64e56fb32abe9f25e7c6b2316a50676f3cd1b53/docs/images/potentialpatina.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialpatina/releases/download/v1.0.2/PotentialPatina-1.0.2-mpc-armv7.zip", "plugins/potentialpatina/thumb.ppm",
            "Retro-colour insert effect: vinyl and tape noise, wow and flutter, tape/tube/cassette drive, bit and sample-rate crush, a tone tilt and a small room, with presets from subtle dust to old radio.", "./plugins/potentialpatina/install.sh", 0,
            false,
            false,
            false,
            { "lo-fi", "vinyl", "tape", "bitcrusher", "wobble" }
        },
        {
            "pulytek", "PulyTek", "MagicStino", "v1.0.2", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-10-23",
            "2090", "14.9 MB", "14%",
            "MagicStino - VST - PulyTek", "pulytek.so",
            "https://raw.githubusercontent.com/MagicStino/pulytek/v1.0.1/docs/preview_0.png", "https://github.com/MagicStino/pulytek/releases/download/v1.0.2/PulyTek-1.0.2-mpc-armv7.zip", "plugins/pulytek/thumb.ppm",
            "Pultec-style WDF EQ with tube warmth, AIR, stereo VU meters and four-band side EQ.", "./plugins/pulytek/install.sh", 0,
            false,
            false,
            false,
            { "eq", "pultec", "wdf", "tube", "stereo" }
        },
        {
            "op1fx-punch", "Punch", "Bombest", "v0.4.1", "Distortion",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "72", "6.0 MB", "14%",
            "bombest - VST - Punch", "punch.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/punch.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Punch-0.4.1-mpc-armv7.zip", "plugins/op1fx-punch/thumb.ppm",
            "Bit-crush style punch for drums and buses, 12 presets, with a Detroit boxer skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Punch\" && if [ -f \"./plugins/op1fx-punch/punch.so\" ]; then cp -rf ./plugins/op1fx-punch/* \"$TARGET_SYNTH_DIR/bombest - VST - Punch/\"; else (wget -q -O /tmp/op1fx-punch.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Punch-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-punch.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Punch-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Punch-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-punch.zip) && unzip -o /tmp/op1fx-punch.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Punch/\" && rm -f /tmp/op1fx-punch.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "compressor", "drive", "drums" }
        },
        {
            "rings-fx", "Resonator FX", "saustin2010", "v1.2.1", "Resonator",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1747", "1.2 MB", "14%",
            "Émilie Gillet - VST - [FX] Resonator FX", "ringsfx.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-resonator-fx/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-resonator-fx/releases/download/v1.2.1/FX-Resonator-FX-1.2.1-mpc-armv7.zip", "plugins/rings-fx/thumb.ppm",
            "Resonator FX: Resonator as an insert effect, the track's audio strums it, turning drums and loops into tuned strings; 12 presets.", "./plugins/rings-fx/install.sh", 0,
            false,
            false,
            false,
            { "rings", "resonator", "eurorack" }
        },
        {
            "rmxxxl", "RMXXXL", "L'Cronx", "v1.3.0", "Multi Fx",
            "FX", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "2247", "3.3 MB", "14%",
            "L'Cronx - VST - RMXXXL", "rmxxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/RMXXXL/0ef9fe195525cada860205f1370a4b9f6896bc60/docs/img/remix.png", "https://github.com/sunskiefer/RMXXXL/releases/download/v1.3.0/RMXXXL-1.3.0-mpc-armv7.zip", "plugins/rmxxxl/thumb.ppm",
            "RMX-1000-style remix effect: isolator, DJ filter, Build Up / Break Down scenes, synced echo, Release FX and a Release kill switch, EQ Kill buttons, a clean Panic, Clouds, Dragonfly reverbs, sample pads with ADSR, 16 user presets and a brickwall limiter, with a big-button touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - RMXXXL\" && if [ -f \"./plugins/rmxxxl/rmxxxl.so\" ]; then cp -rf ./plugins/rmxxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - RMXXXL/\"; else (wget -q -O /tmp/rmxxxl.zip \"https://github.com/sunskiefer/RMXXXL/releases/download/v1.3.0/RMXXXL-1.3.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/rmxxxl.zip \"https://github.com/sunskiefer/RMXXXL/releases/download/v1.3.0/RMXXXL-1.3.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sunskiefer/RMXXXL/releases/download/v1.3.0/RMXXXL-1.3.0-mpc-armv7.zip\" -o /tmp/rmxxxl.zip) && unzip -o /tmp/rmxxxl.zip -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - RMXXXL/\" && rm -f /tmp/rmxxxl.zip; fi && sync", 1,
            false,
            false,
            false,
            { "remix", "dj", "rmx", "isolator", "filter", "clouds", "reverb", "pads" }
        },
        {
            "shapey-5-band-eq", "Shapey 5-Band EQ", "3dthc", "v1.0.1", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "269", "3.4 MB", "14%",
            "3DTHC - VST - Shapey 5-Band EQ", "shapey_5band_eq.so",
            "https://raw.githubusercontent.com/3dthc/Shapey-MPC/ddbf9bbc9f66f2af0802070b38bb3d113140131c/docs/shapey-5-band-eq.png", "https://github.com/3dthc/Shapey-MPC/releases/download/shapey-5-band-eq-vst-v1.0.1/Shapey-5-Band-EQ-1.0.1-mpc-armv7.zip", "plugins/shapey-5-band-eq/thumb.ppm",
            "The 5-band version of Shapey EQ, made for touch: every band has its own lane under a taller curve, so any dot drags directly. Any band can be a bell, shelf, cut, notch or band pass. Live analyzer, zero latency.", "./plugins/shapey-5-band-eq/install.sh", 0,
            false,
            false,
            false,
            { "eq", "parametric-eq", "analyzer", "mixing" }
        },
        {
            "shapey-eq", "Shapey EQ", "3dthc", "v1.6.2", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "313", "4.3 MB", "14%",
            "3DTHC - VST - Shapey EQ", "shapey_eq.so",
            "https://raw.githubusercontent.com/3dthc/Shapey-MPC/ddbf9bbc9f66f2af0802070b38bb3d113140131c/docs/shapey-eq.png", "https://github.com/3dthc/Shapey-MPC/releases/download/shapey-eq-vst-v1.6.2/Shapey-EQ-1.6.2-mpc-armv7.zip", "plugins/shapey-eq/thumb.ppm",
            "An 8-band parametric EQ you shape by touch: tap or drag on the curve, drag numbered dots, and make any band a bell, shelf, cut, notch or band pass. Live analyzer, zero latency, presets and four themes.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey EQ\" && if [ -f \"./plugins/shapey-eq/shapey_eq.so\" ]; then cp -rf ./plugins/shapey-eq/* \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey EQ/\"; else (wget -q -O /tmp/shapey-eq.zip \"https://github.com/3dthc/Shapey-MPC/releases/download/shapey-eq-vst-v1.6.2/Shapey-EQ-1.6.2-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/shapey-eq.zip \"https://github.com/3dthc/Shapey-MPC/releases/download/shapey-eq-vst-v1.6.2/Shapey-EQ-1.6.2-mpc-armv7.zip\" || curl -k -sL \"https://github.com/3dthc/Shapey-MPC/releases/download/shapey-eq-vst-v1.6.2/Shapey-EQ-1.6.2-mpc-armv7.zip\" -o /tmp/shapey-eq.zip) && unzip -o /tmp/shapey-eq.zip -d \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey EQ/\" && rm -f /tmp/shapey-eq.zip; fi && sync", 1,
            false,
            false,
            false,
            { "eq", "parametric-eq", "analyzer", "mixing" }
        },
        {
            "op1fx-spring", "Spring", "Bombest", "v0.4.1", "Reverb",
            "FX", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-11-01",
            "73", "3.4 MB", "14%",
            "bombest - VST - Spring", "spring.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/spring.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Spring-0.4.1-mpc-armv7.zip", "plugins/op1fx-spring/thumb.ppm",
            "Spring reverb with a 12-preset bank. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Spring\" && if [ -f \"./plugins/op1fx-spring/spring.so\" ]; then cp -rf ./plugins/op1fx-spring/* \"$TARGET_SYNTH_DIR/bombest - VST - Spring/\"; else (wget -q -O /tmp/op1fx-spring.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Spring-0.4.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/op1fx-spring.zip \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Spring-0.4.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Spring-0.4.1-mpc-armv7.zip\" -o /tmp/op1fx-spring.zip) && unzip -o /tmp/op1fx-spring.zip -d \"$TARGET_SYNTH_DIR/bombest - VST - Spring/\" && rm -f /tmp/op1fx-spring.zip; fi && sync", 1,
            false,
            false,
            false,
            { "op1", "reverb", "spring" }
        },
        {
            "stompbox", "Stompbox", "jacob-sabella", "v1.0.0", "Amp Sim",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-04",
            "150", "6.7 MB", "21%",
            "jacob-sabella - VST - Stompbox", "stompbox.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-stompbox/bc70be3dd01103bc792db0c2b0e54a589fc37d36/docs/screenshots/stompbox.png", "https://github.com/jacob-sabella/mpc-vst-stompbox/releases/download/stompbox-vst-v1.0.0/Stompbox-1.0.0-mpc-armv7.zip", "plugins/stompbox/thumb.ppm",
            "A whole guitar rig from guitarix's DSP: gate, comp, wah, any number of pedals, amp, tone stack, power amp, cabinet with an IR loader, post FX, mod, delay, reverb and EQ. 219 models and guitarix's 40 factory presets in the PRESET menu. Not affiliated with guitarix.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Stompbox\" && if [ -f \"./plugins/stompbox/stompbox.so\" ]; then cp -rf ./plugins/stompbox/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Stompbox/\"; else (wget -q -O /tmp/stompbox.zip \"https://github.com/jacob-sabella/mpc-vst-stompbox/releases/download/stompbox-vst-v1.0.0/Stompbox-1.0.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/stompbox.zip \"https://github.com/jacob-sabella/mpc-vst-stompbox/releases/download/stompbox-vst-v1.0.0/Stompbox-1.0.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/jacob-sabella/mpc-vst-stompbox/releases/download/stompbox-vst-v1.0.0/Stompbox-1.0.0-mpc-armv7.zip\" -o /tmp/stompbox.zip) && unzip -o /tmp/stompbox.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Stompbox/\" && rm -f /tmp/stompbox.zip; fi && sync", 1,
            false,
            false,
            false,
            { "guitar", "amp", "pedals", "cabinet", "cab-ir", "guitarix", "presets" }
        },
        {
            "stream", "Stream", "johnny", "v1.0.0", "Utility",
            "FX", "Stable", "MPC One (3.9.1.2)", "MPC OS 3.x", "2026-10-03",
            "501", "2.7 MB", "1%",
            "johnny - VST - Stream", "stream.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/mpc-stream/764605ba1a08594e2dc5268e4f12bd6e25edb184/screenshots/stream.png", "https://github.com/stampman3000-jpg/mpc-stream/releases/download/v1.0.0/Stream-1.0.0-mpc-armv7.zip", "plugins/stream/thumb.ppm",
            "Multichannel USB audio effect send from an MPC to a Mac. Needs BlackHole 64ch and a Mac helper app.", "./plugins/stream/install.sh", 0,
            false,
            false,
            false,
            { "utility" }
        },
        {
            "tr-mpc-tap-fx", "TR-MPC Tap FX", "sd88me", "v1.1.0", "Utility",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 3.x", "2026-11-02",
            "969", "515 KB", "14%",
            "sd88me - VST - TR-MPC Tap FX", "trmpc_tapfx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/7e704c6f3be14b661c887631e203494d8cc3f74f/docs/img/tr-mpc-tap-fx.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-tap-fx-vst-v1.1.0/TR-MPC-Tap-FX-1.1.0-mpc-armv7.zip", "plugins/tr-mpc-tap-fx/thumb.ppm",
            "Companion to TR-MPC: sends any slot, or the kit's reverb or delay send, out on its own MPC track, sample-aligned with the kit, so MPC's mixer, submixes and insert effects can process it. Needs TR-MPC in the same project.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC Tap FX\" && if [ -f \"./plugins/tr-mpc-tap-fx/trmpc_tapfx.so\" ]; then cp -rf ./plugins/tr-mpc-tap-fx/* \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC Tap FX/\"; else (wget -q -O /tmp/tr-mpc-tap-fx.zip \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-tap-fx-vst-v1.1.0/TR-MPC-Tap-FX-1.1.0-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/tr-mpc-tap-fx.zip \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-tap-fx-vst-v1.1.0/TR-MPC-Tap-FX-1.1.0-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-tap-fx-vst-v1.1.0/TR-MPC-Tap-FX-1.1.0-mpc-armv7.zip\" -o /tmp/tr-mpc-tap-fx.zip) && unzip -o /tmp/tr-mpc-tap-fx.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC Tap FX/\" && rm -f /tmp/tr-mpc-tap-fx.zip; fi && sync", 1,
            false,
            false,
            false,
            { "routing", "multi-out", "drum" }
        },
        {
            "tuner", "Tuner", "jacob-sabella", "v1.0.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-04",
            "113", "664 KB", "1%",
            "jacob-sabella - VST - Tuner", "tuner.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-tuner/a3e4c099eacce3cdd440d54eb3ff059c96baafa5/docs/screenshots/needle.png", "https://github.com/jacob-sabella/mpc-vst-tuner/releases/download/tuner-vst-v1.0.0/Tuner-1.0.0-mpc-armv7.zip", "plugins/tuner/thumb.ppm",
            "A chromatic tuner insert with a live needle drawn straight onto the MPC's screen at 20-24 fps: note, cents and Hz, A4 from 430 to 450 Hz, and a mute for silent tuning. It writes into MPC's display memory, which MPC OS doesn't offer plugins, so a firmware update could break it. Not affiliated with Akai.", "./plugins/tuner/install.sh", 0,
            false,
            false,
            false,
            { "tuner", "guitar", "bass", "pitch", "screen-draw" }
        },
        {
            "tuney", "Tuney", "3dthc", "v1.4.3", "Pitch",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "233", "9.3 MB", "14%",
            "3DTHC - VST - Tuney", "tuney.so",
            "https://raw.githubusercontent.com/3dthc/Tuney-MPC/55862ef83ab6938b5f1e3e20f5fa1ead7449e9b2/docs/tuney.png", "https://github.com/3dthc/Tuney-MPC/releases/download/tuney-vst-v1.4.3/Tuney-1.4.3-mpc-armv7.zip", "plugins/tuney/thumb.ppm",
            "Real-time vocal pitch correction: key and scale or your own notes on a touch keyboard, a pitch dial, Retune, Transition, Amount, Vibrato and Humanize, voice ranges and a reference pitch. Presets from hard tune to subtle.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3DTHC - VST - Tuney\" && if [ -f \"./plugins/tuney/tuney.so\" ]; then cp -rf ./plugins/tuney/* \"$TARGET_SYNTH_DIR/3DTHC - VST - Tuney/\"; else (wget -q -O /tmp/tuney.zip \"https://github.com/3dthc/Tuney-MPC/releases/download/tuney-vst-v1.4.3/Tuney-1.4.3-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/tuney.zip \"https://github.com/3dthc/Tuney-MPC/releases/download/tuney-vst-v1.4.3/Tuney-1.4.3-mpc-armv7.zip\" || curl -k -sL \"https://github.com/3dthc/Tuney-MPC/releases/download/tuney-vst-v1.4.3/Tuney-1.4.3-mpc-armv7.zip\" -o /tmp/tuney.zip) && unzip -o /tmp/tuney.zip -d \"$TARGET_SYNTH_DIR/3DTHC - VST - Tuney/\" && rm -f /tmp/tuney.zip; fi && sync", 1,
            false,
            false,
            false,
            { "pitch-correction", "vocal", "tuner", "effect" }
        },
        {
            "verglas", "Verglas", "saustin2010", "v1.2.1", "Granular",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1362", "642 KB", "14%",
            "Émilie Gillet - VST - [FX] Verglas", "verglas.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-verglas/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-verglas/releases/download/v1.2.1/FX-Verglas-1.2.1-mpc-armv7.zip", "plugins/verglas/thumb.ppm",
            "Verglas: granular, stretch, looper and spectral modes and freeze as an insert effect; 12 presets, from Émilie Gillet's open-source Clouds.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Verglas\" && if [ -f \"./plugins/verglas/verglas.so\" ]; then cp -rf ./plugins/verglas/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Verglas/\"; else (wget -q -O /tmp/verglas.zip \"https://github.com/saustin2010/mpc-vst-verglas/releases/download/v1.2.1/FX-Verglas-1.2.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/verglas.zip \"https://github.com/saustin2010/mpc-vst-verglas/releases/download/v1.2.1/FX-Verglas-1.2.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/saustin2010/mpc-vst-verglas/releases/download/v1.2.1/FX-Verglas-1.2.1-mpc-armv7.zip\" -o /tmp/verglas.zip) && unzip -o /tmp/verglas.zip -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Verglas/\" && rm -f /tmp/verglas.zip; fi && sync", 1,
            false,
            false,
            false,
            { "clouds", "granular", "texture", "eurorack" }
        },
        {
            "vibe-fx", "Vibe FX", "nachtaktiv303", "v1.0.0", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1078", "906 KB", "14%",
            "nachtaktiv303 - VST - Vibe FX", "mpc_fx.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-vibe-fx/main/preview_1.png", "https://github.com/nachtaktiv303/mpc-vst-vibe-fx/releases/download/vibe-fx-vst-v1.0.0/Vibe-FX-1.0.0-mpc-armv7.zip", "plugins/vibe-fx/thumb.ppm",
            "Drive, chorus, FDN reverb and tape delay in one native MPC OS insert effect, with a touchscreen skin and Q-Links.", "./plugins/vibe-fx/install.sh", 0,
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
            "323", "257 KB", "14%",
            "Mac Digi / Rare Data - VST - Force Link Audio", "forceLinkAudioAddin.so",
            "https://github.com/macdigi/force-link-audio/releases/download/v1.1.0/force-link-audio-card.png", "https://github.com/macdigi/force-link-audio/releases/download/v1.1.0/Force-Link-Audio-1.1.0-mpc-armv7.zip", "plugins/force-link-audio/thumb.ppm",
            "Publish the Force stereo main mix to Link Audio receivers on your trusted LAN while staying standalone. Disable the legacy Force Link Audio addon first. Off until enabled in settings; no remote-control server. Other MPC hardware is unverified.", "./plugins/force-link-audio/install.sh", 0,
            false,
            false,
            false,
            { "link-audio", "network-audio", "recording", "compa" }
        },
        {
            "commander", "MPC Commander", "jacob-sabella", "v1.1.1", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "2808", "64 KB", "14%",
            "jacob-sabella - VST - MPC Commander", "mpc_commander_addin.so",
            "", "https://github.com/jacob-sabella/mpc-addin-commander/releases/download/commander-addin-v1.1.1/MPC-Commander-addin-1.1.1-mpc-armv7.zip", "plugins/commander/thumb.ppm",
            "The plugins MPC loads, with their own skins, and MPC's transport, on a desktop app. Listens on the device only: reach it through an SSH tunnel (no login; bind=0.0.0.0 opens it to your network).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Commander\" && if [ -f \"./plugins/commander/mpc_commander_addin.so\" ]; then cp -rf ./plugins/commander/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Commander/\"; else (wget -q -O /tmp/commander.zip \"https://github.com/jacob-sabella/mpc-addin-commander/releases/download/commander-addin-v1.1.1/MPC-Commander-addin-1.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/commander.zip \"https://github.com/jacob-sabella/mpc-addin-commander/releases/download/commander-addin-v1.1.1/MPC-Commander-addin-1.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/jacob-sabella/mpc-addin-commander/releases/download/commander-addin-v1.1.1/MPC-Commander-addin-1.1.1-mpc-armv7.zip\" -o /tmp/commander.zip) && unzip -o /tmp/commander.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Commander/\" && rm -f /tmp/commander.zip; fi && sync", 1,
            false,
            false,
            false,
            { "desktop", "plugins", "skins", "transport", "midi" }
        },
        {
            "filebrowser", "MPC File Browser", "jacob-sabella", "v1.0.1", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "454", "19.2 MB", "14%",
            "jacob-sabella - VST - MPC File Browser", "mpc_filebrowser_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-filebrowser/88b86eb39f907485995497cf0eaeeb5ddd7aa3e4/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-filebrowser/releases/download/filebrowser-addin-v1.0.1/MPC-File-Browser-1.0.1-mpc-armv7.zip", "plugins/filebrowser/thumb.ppm",
            "FileBrowser Quantum beside MPC: browse the device's drives in a web browser, upload and download samples and projects, play audio, edit text files. Listens on the device only: reach it through an SSH tunnel (login admin / mpcadmin; change it). bind=0.0.0.0 opens it to your network.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC File Browser\" && if [ -f \"./plugins/filebrowser/mpc_filebrowser_addin.so\" ]; then cp -rf ./plugins/filebrowser/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC File Browser/\"; else (wget -q -O /tmp/filebrowser.zip \"https://github.com/jacob-sabella/mpc-addin-filebrowser/releases/download/filebrowser-addin-v1.0.1/MPC-File-Browser-1.0.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/filebrowser.zip \"https://github.com/jacob-sabella/mpc-addin-filebrowser/releases/download/filebrowser-addin-v1.0.1/MPC-File-Browser-1.0.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/jacob-sabella/mpc-addin-filebrowser/releases/download/filebrowser-addin-v1.0.1/MPC-File-Browser-1.0.1-mpc-armv7.zip\" -o /tmp/filebrowser.zip) && unzip -o /tmp/filebrowser.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC File Browser/\" && rm -f /tmp/filebrowser.zip; fi && sync", 1,
            false,
            false,
            false,
            { "files", "browser", "upload", "samples", "editor" }
        },
        {
            "remote", "MPC Remote", "jacob-sabella", "v1.1.1", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "3235", "68 KB", "14%",
            "jacob-sabella - VST - MPC Remote", "mpc_remote_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-remote/c9cc05422873025693cf5527b485ef44da51c5a0/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-remote/releases/download/remote-addin-v1.1.1/MPC-Remote-addin-1.1.1-mpc-armv7.zip", "plugins/remote/thumb.ppm",
            "The MPC screen in a browser, and an MCP server so an AI model can see and touch it, play MIDI and read files. Listens on the device only: reach it through an SSH tunnel (no login; bind=0.0.0.0 opens it to your network).", "./plugins/remote/install.sh", 0,
            false,
            false,
            false,
            { "remote", "screen", "touch", "mcp", "midi" }
        },
        {
            "skipback", "MPC Skipback", "sd88me", "v0.6.2", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "1059", "64 KB", "14%",
            "sd88me - VST - MPC Skipback", "libmpc_skipback.so",
            "https://raw.githubusercontent.com/sd88me/mpc-addin-skipback/dc82602b2a05354d42445c8a330fb92a864892fb/docs/img/skipback.png", "https://github.com/sd88me/mpc-addin-skipback/releases/download/v0.6.2/MPC-Skipback-0.6.2-mpc-armv7.zip", "plugins/skipback/thumb.ppm",
            "Records the main output all the time and saves the last 30 to 60 seconds as a WAV when you double-press a hardware button (Rec Arm on a Force; Overdub, or a button you teach it, on other models). Tested on a Gen1 Force; other models are unverified.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - MPC Skipback\" && if [ -f \"./plugins/skipback/libmpc_skipback.so\" ]; then cp -rf ./plugins/skipback/* \"$TARGET_SYNTH_DIR/sd88me - VST - MPC Skipback/\"; else (wget -q -O /tmp/skipback.zip \"https://github.com/sd88me/mpc-addin-skipback/releases/download/v0.6.2/MPC-Skipback-0.6.2-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/skipback.zip \"https://github.com/sd88me/mpc-addin-skipback/releases/download/v0.6.2/MPC-Skipback-0.6.2-mpc-armv7.zip\" || curl -k -sL \"https://github.com/sd88me/mpc-addin-skipback/releases/download/v0.6.2/MPC-Skipback-0.6.2-mpc-armv7.zip\" -o /tmp/skipback.zip) && unzip -o /tmp/skipback.zip -d \"$TARGET_SYNTH_DIR/sd88me - VST - MPC Skipback/\" && rm -f /tmp/skipback.zip; fi && sync", 1,
            false,
            false,
            false,
            { "skipback", "recording", "sampling", "wav", "buffer" }
        },
        {
            "usb-audio", "MPC USB audio", "jacob-sabella", "v1.1.1", "Audio Interface",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "3186", "64 KB", "14%",
            "jacob-sabella - VST - MPC USB audio", "libmpc_usb_audio.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-usb-audio/f73cf3a1de3c3d005f30b6d38caddf587376e6d4/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-usb-audio/releases/download/usb-audio-addin-v1.1.1/MPC-USB-audio-addin-1.1.1-mpc-armv7.zip", "plugins/usb-audio/thumb.ppm",
            "The MPC as a USB audio interface: its outputs and inputs as channels on the computer, next to its MIDI ports.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./Synths}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC USB audio\" && if [ -f \"./plugins/usb-audio/libmpc_usb_audio.so\" ]; then cp -rf ./plugins/usb-audio/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC USB audio/\"; else (wget -q -O /tmp/usb-audio.zip \"https://github.com/jacob-sabella/mpc-addin-usb-audio/releases/download/usb-audio-addin-v1.1.1/MPC-USB-audio-addin-1.1.1-mpc-armv7.zip\" || wget --no-check-certificate -q -O /tmp/usb-audio.zip \"https://github.com/jacob-sabella/mpc-addin-usb-audio/releases/download/usb-audio-addin-v1.1.1/MPC-USB-audio-addin-1.1.1-mpc-armv7.zip\" || curl -k -sL \"https://github.com/jacob-sabella/mpc-addin-usb-audio/releases/download/usb-audio-addin-v1.1.1/MPC-USB-audio-addin-1.1.1-mpc-armv7.zip\" -o /tmp/usb-audio.zip) && unzip -o /tmp/usb-audio.zip -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC USB audio/\" && rm -f /tmp/usb-audio.zip; fi && sync", 1,
            false,
            false,
            false,
            { "usb", "audio-interface", "recording", "inputs" }
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
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/1a6251e5dc90386a010179cb2173a51831cbbf4b/tools/mpc_patch/auto_exec/auto-exec-patch.sh", "plugins/patch-auto-exec/thumb.ppm",
            "Recommended for a noexec drive. Makes the Synths folder of every noexec drive (SSD, USB stick, SD card) executable through a small boot-time service, with nothing to choose; the drives stay noexec. No MPC restart required and undo is built in.", "curl -sL \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/1a6251e5dc90386a010179cb2173a51831cbbf4b/tools/mpc_patch/auto_exec/auto-exec-patch.sh\" -o /tmp/auto-exec-patch.sh && sh /tmp/auto-exec-patch.sh", 1,
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
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/6ddf68cf92b3f1c0bdd322231ed133c0cc2eed48/tools/mpc_patch/mpc-drum-pad-patch.sh", "plugins/patch-16-pad-drum-layout/thumb.ppm",
            "Machinedrum Module / Machinemodule, 6W6, 8W8, CW-78, 9W9, TR-MPC and Lucky Dip get MPC's drum layout with all 16 pads lit. Backs up /usr/bin/MPC to /sdcard/MPC-backup with built-in undo.", "curl -sL \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/6ddf68cf92b3f1c0bdd322231ed133c0cc2eed48/tools/mpc_patch/mpc-drum-pad-patch.sh\" -o /tmp/mpc-drum-pad-patch.sh && sh /tmp/mpc-drum-pad-patch.sh", 1,
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
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/a9bb8c71ba382fa6c3902fbbdead6d5f620222a5/tools/mpc_patch/hwremap/hwremap-patch.sh", "plugins/patch-button-remap/thumb.ppm",
            "Remap hardware buttons on a standalone MPC or Force via an LD_PRELOAD shim (/usr/lib/hwremap.so): a button can become other buttons, a pad, or a screen tap. Pick which remaps you want at install (Mixer, Edit, Clip, Menu, and Knobs options).", "curl -sL \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/a9bb8c71ba382fa6c3902fbbdead6d5f620222a5/tools/mpc_patch/hwremap/hwremap-patch.sh\" -o /tmp/hwremap-patch.sh && sh /tmp/hwremap-patch.sh", 1,
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
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/f036a8791daff1ccf426c3953e84a8942c979564/tools/mpc_patch/drive_exec/drive-exec-patch.sh", "plugins/patch-drive-exec/thumb.ppm",
            "Makes one selected drive Synths folder executable through a boot-time systemd bind-mount service while keeping the drive itself noexec. (Superseded by Auto exec, retained for uninstall compatibility.)", "curl -sL \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/f036a8791daff1ccf426c3953e84a8942c979564/tools/mpc_patch/drive_exec/drive-exec-patch.sh\" -o /tmp/drive-exec-patch.sh && sh /tmp/drive-exec-patch.sh", 1,
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
            "https://raw.githubusercontent.com/padbangers/mpc-link/main/assets/catalog.png", "https://github.com/padbangers/mpc-link/releases/download/v1.0.2/MPC-Link-1.0.2-macOS.zip", "plugins/companion-mpc-link/thumb.ppm",
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
        std::ifstream catFile("plugins/tkgl_catalog.json", std::ios::binary);
        if (catFile.is_open()) {
            catFile.close();
            std::printf("[TKGL-CFG] %s not found, loading cached plugins/tkgl_catalog.json\n", configPath.c_str());
            std::vector<std::string> syncLogs;
            syncFromOnlineCatalog(outConfig, syncLogs);
            refreshInstallStates(outConfig);
            return true;
        }
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
            extract_string_field(brace, end_brace, "synth_dir", outConfig.synth_dir_name);
            extract_string_field(brace, end_brace, "usb_mount", outConfig.usb_mount_path);
        }
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
                                item.thumbPpmPath = "plugins/" + item.id + "/thumb.ppm";
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

bool ConfigParser::saveToFile(const std::string& configPath, const LauncherConfig& config) {
    std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    out << "{\n";
    out << "  \"global\": {\n";
    out << "    \"title\": \"" << config.app_title << "\",\n";
    out << "    \"rotation\": " << config.screen_rotation << ",\n";
    out << "    \"catalog_url\": \"" << config.catalog_url << "\",\n";
    out << "    \"synth_dir\": \"" << config.synth_dir_name << "\",\n";
    out << "    \"usb_mount\": \"" << config.usb_mount_path << "\"\n";
    out << "  },\n";
    out << "  \"sections\": [\n";

    for (size_t s = 0; s < config.sections.size(); ++s) {
        const auto& sec = config.sections[s];
        out << "    {\n";
        out << "      \"section\": \"" << sec.title << "\",\n";
        out << "      \"items\": [\n";
        for (size_t i = 0; i < sec.items.size(); ++i) {
            const auto& it = sec.items[i];
            out << "        {\n";
            out << "          \"id\": \"" << it.id << "\",\n";
            out << "          \"item\": \"" << it.label << "\",\n";
            out << "          \"author\": \"" << it.author << "\",\n";
            out << "          \"version\": \"" << it.version << "\",\n";
            out << "          \"size\": \"" << it.sizeMb << "\",\n";
            out << "          \"updated\": \"" << it.updatedDate << "\",\n";
            out << "          \"downloads\": \"" << it.downloads << "\",\n";
            out << "          \"tested\": \"" << it.testedDevice << "\",\n";
            out << "          \"screenshot\": \"" << it.screenshotUrl << "\",\n";
            out << "          \"url\": \"" << it.downloadUrl << "\",\n";
            out << "          \"description\": \"" << it.description << "\",\n";
            out << "          \"target_subdir\": \"" << it.targetSubdir << "\",\n";
            out << "          \"binary\": \"" << it.targetBinary << "\",\n";
            out << "          \"installed\": " << (it.installed ? 1 : 0) << ",\n";
            out << "          \"broken\": " << (it.broken ? 1 : 0) << ",\n";
            if (it.is_inline) {
                out << "          \"code\": {\n            " << it.cmd << "\n          }\n";
            } else {
                out << "          \"script\": \"" << it.cmd << "\"\n";
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

    std::string url = config.catalog_url;
    if (url.empty()) url = "https://sd88me.github.io/mpc-vst-plugins/catalog.json";
    if (url.back() == '/') url += "catalog.json";
    std::string fallbackUrl = "https://open-mpc.github.io/catalog.json";

    mkdir("plugins", 0755);
    outLogs.push_back("[TKGL-SYNC] Connecting to online catalog via libcurl.so.4: " + url);
    if (onProgress) onProgress("[TKGL-SYNC] Fetching catalog JSON via libcurl.so.4...");

    // Download to a temporary file first via native libcurl.so.4 (dlopen) so we never truncate
    // plugins/tkgl_catalog.json to 0 bytes on network failure.
    std::string tmpCat = "/tmp/tkgl_catalog_dl.json";
    unlink(tmpCat.c_str());
    bool dlOk = downloadFile(url, tmpCat, 30, onProgress, "[TKGL-SYNC] Downloading catalog");
    if (!dlOk) {
        outLogs.push_back("[TKGL-SYNC] Primary URL failed, trying fallback: " + fallbackUrl);
        dlOk = downloadFile(fallbackUrl, tmpCat, 30, onProgress, "[TKGL-SYNC] Downloading fallback catalog");
    }
    if (dlOk) {
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
        outLogs.push_back("[ERROR] Failed to download or read plugins/tkgl_catalog.json (network/SSL error).");
        return false;
    }

    std::ifstream file("plugins/tkgl_catalog.json", std::ios::binary);
    if (!file.is_open()) {
        outLogs.push_back("[ERROR] Could not open plugins/tkgl_catalog.json.");
        return false;
    }
    outLogs.push_back("[TKGL-SYNC] Saved catalog to plugins/tkgl_catalog.json (" + std::to_string(stCat.st_size / 1024) + " KB)");
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
                extract_string_field(v_obj, v_close, "url", item.downloadUrl);
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

        if (item.targetSubdir.empty()) item.targetSubdir = item.author + " - VST - " + item.label;
        if (item.targetBinary.empty()) item.targetBinary = item.id + ".so";

        // Preserve previous installed detection state so refreshInstallStates verifies MPC.settings & .so
        if ((!item.id.empty() && prevInstalledMap[item.id]) || (!item.label.empty() && prevInstalledMap[item.label])) {
            item.installed = true;
        }

        std::string plugDir = "plugins/" + item.id;
        mkdir("plugins", 0755);
        mkdir(plugDir.c_str(), 0755);
        item.thumbPpmPath = plugDir + "/thumb.ppm";

        // Adjustment 1: Download all thumbnails during catalog refresh via libcurl.so.4
        if (!item.screenshotUrl.empty()) {
            std::string localPng = plugDir + "/thumb.png";
            std::string localPpm = plugDir + "/thumb.ppm";
            struct stat stPng, stPpm;
            if (stat(localPng.c_str(), &stPng) == 0 && stPng.st_size == 0) {
                unlink(localPng.c_str());
            }
            bool hasThumb = ((stat(localPng.c_str(), &stPng) == 0 && stPng.st_size > 0) ||
                             (stat(localPpm.c_str(), &stPpm) == 0 && stPpm.st_size > 0));
            if (!hasThumb) {
                if (onProgress) {
                    int dotCount = (count % 5) + 1;
                    std::string dots;
                    for (int d = 0; d < 5; ++d) dots += (d < dotCount) ? " * " : " . ";
                    onProgress("[SYNC " + dots + "] Downloading thumb: " + item.label);
                }
                downloadFile(item.screenshotUrl, localPng, 8);
            }
        }

        item.is_inline = 1;
        item.cmd = "mkdir -p \"$SYNTH_INSTALL_DIR/" + item.targetSubdir + "\" && (/proc/self/exe --fetch \"" + item.downloadUrl + "\" /tmp/" + item.id + ".zip || wget --no-check-certificate -q -O /tmp/" + item.id + ".zip \"" + item.downloadUrl + "\" || wget -q -O /tmp/" + item.id + ".zip \"" + item.downloadUrl + "\") && unzip -o /tmp/" + item.id + ".zip -d \"$SYNTH_INSTALL_DIR/" + item.targetSubdir + "/\" && rm -f /tmp/" + item.id + ".zip && sync";

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

void ConfigParser::refreshInstallStates(LauncherConfig& config, std::vector<std::string>* outLogs) {
    // 1. Determine MPC.settings file path
    std::string mpcSettingsPath = "/media/az01-internal/Settings/MPC/MPC.settings";
    const char* candMpc[] = {
        "/media/az01-internal/Settings/MPC/MPC.settings",
        "/media/internal/Settings/MPC/MPC.settings",
        "/var/lib/MPC/MPC.settings",
        "/root/.config/Akai/MPC/MPC.settings"
    };
    for (const char* cp : candMpc) {
        struct stat st;
        if (stat(cp, &st) == 0 && S_ISREG(st.st_mode)) {
            mpcSettingsPath = cp;
            break;
        }
    }

    // 2. Gather all SynthContentLocations from MPC.settings + Manager's Synths + mounted media
    std::vector<SynthLocation> synthLocs = getSynthContentLocations(mpcSettingsPath);
    std::vector<std::string> searchPaths;
    for (const auto& sl : synthLocs) {
        std::string p = sl.path;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (!p.empty()) searchPaths.push_back(p);
    }
    if (!config.synth_dir_name.empty()) {
        std::string p = config.synth_dir_name;
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        searchPaths.push_back(p);
    }
    if (!config.usb_mount_path.empty()) {
        std::string p = config.usb_mount_path + "/Synths";
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        searchPaths.push_back(p);
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

    // Remove duplicates from search paths
    std::sort(searchPaths.begin(), searchPaths.end());
    searchPaths.erase(std::unique(searchPaths.begin(), searchPaths.end()), searchPaths.end());

    // 3. Read MPC.settings to check registered plugins in <VALUE name="pluginList-arm"><KNOWNPLUGINS>
    std::string mpcSettingsContent;
    std::ifstream fSett(mpcSettingsPath, std::ios::binary);
    if (fSett.is_open()) {
        std::stringstream ss;
        ss << fSett.rdbuf();
        mpcSettingsContent = ss.str();
    }

    struct RegPlugin {
        std::string rawTag;
        std::string name;
        std::string descName;
        std::string file;
    };
    std::vector<RegPlugin> regPlugins;
    if (!mpcSettingsContent.empty()) {
        size_t armPos = mpcSettingsContent.find("name=\"pluginList-arm\"");
        size_t searchStart = (armPos != std::string::npos) ? armPos : 0;
        size_t searchEnd = mpcSettingsContent.size();
        if (armPos != std::string::npos) {
            size_t closeVal = mpcSettingsContent.find("</VALUE>", armPos);
            if (closeVal != std::string::npos) searchEnd = closeVal;
        }

        size_t ppos = searchStart;
        while ((ppos = mpcSettingsContent.find("<PLUGIN ", ppos)) != std::string::npos && ppos < searchEnd) {
            size_t endp = mpcSettingsContent.find("/>", ppos);
            if (endp == std::string::npos || endp > searchEnd) {
                endp = mpcSettingsContent.find(">", ppos);
                if (endp == std::string::npos || endp > searchEnd) break;
                endp += 1;
            } else {
                endp += 2;
            }
            std::string tag = mpcSettingsContent.substr(ppos, endp - ppos);
            ppos = endp;

            RegPlugin rp;
            rp.rawTag = tag;
            auto getAttr = [](const std::string& str, const std::string& key) -> std::string {
                std::string needle = key + "=\"";
                size_t kpos = str.find(needle);
                if (kpos == std::string::npos) return "";
                size_t vstart = kpos + needle.size();
                size_t vend = str.find("\"", vstart);
                if (vend == std::string::npos) return "";
                return str.substr(vstart, vend - vstart);
            };
            rp.name = getAttr(tag, "name");
            rp.descName = getAttr(tag, "descriptiveName");
            rp.file = getAttr(tag, "file");
            regPlugins.push_back(rp);
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
                    // Scan directory for any *.so
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
            // Direct .so check in basePath
            std::string directSo = basePath + "/" + binName;
            if (stat(directSo.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                outFoundSo = directSo;
                return true;
            }
        }
        return false;
    };

    bool mpcSettingsModified = false;

    // 4. For each plugin in each section, verify installation state and MPC.settings integrity
    for (auto& sec : config.sections) {
        for (auto& item : sec.items) {
            std::string subDir = !item.targetSubdir.empty() ? item.targetSubdir : (item.author + " - VST - " + item.label);
            std::string binName = !item.targetBinary.empty() ? item.targetBinary : (item.id + ".so");

            int matchedIdx = -1;
            for (size_t r = 0; r < regPlugins.size(); ++r) {
                const auto& rp = regPlugins[r];
                bool nameMatch = (!item.label.empty() && (rp.name == item.label || rp.descName == item.label));
                bool idMatch = (!item.id.empty() && (rp.name == item.id || rp.descName == item.id));
                bool fileSubMatch = (!subDir.empty() && rp.file.find("/" + subDir + "/") != std::string::npos);
                bool fileBinMatch = (!binName.empty() && rp.file.find("/" + binName) != std::string::npos);
                if (nameMatch || idMatch || fileSubMatch || fileBinMatch) {
                    matchedIdx = static_cast<int>(r);
                    break;
                }
            }

            bool wasDetectedInstalled = item.installed || (matchedIdx >= 0);

            if (wasDetectedInstalled) {
                // Adjustment 2: Verify existence of the .so file in <KNOWNPLUGINS><PLUGIN file="..."/>
                bool regFileValid = false;
                std::string regFilePath;
                if (matchedIdx >= 0) {
                    regFilePath = regPlugins[matchedIdx].file;
                    struct stat stSo;
                    if (!regFilePath.empty() && stat(regFilePath.c_str(), &stSo) == 0 && S_ISREG(stSo.st_mode)) {
                        regFileValid = true;
                        size_t lastSlash = regFilePath.find_last_of('/');
                        if (lastSlash != std::string::npos) {
                            item.targetBinary = regFilePath.substr(lastSlash + 1);
                            size_t prevSlash = regFilePath.find_last_of('/', lastSlash - 1);
                            if (prevSlash != std::string::npos) {
                                item.targetSubdir = regFilePath.substr(prevSlash + 1, lastSlash - prevSlash - 1);
                            }
                        }
                    }
                }

                if (regFileValid) {
                    item.installed = true;
                    item.broken = false;
                } else {
                    // The .so file in MPC.settings was not found! Check if it exists somewhere in <SynthContentLocations>
                    std::string foundSo, foundSubdir, foundBin;
                    if (findSoInSynthLocations(item, subDir, binName, foundSo, foundSubdir, foundBin)) {
                        item.targetSubdir = foundSubdir;
                        item.targetBinary = foundBin;
                        item.installed = true;
                        item.broken = false;

                        if (matchedIdx >= 0 && !mpcSettingsContent.empty()) {
                            std::string oldTag = regPlugins[matchedIdx].rawTag;
                            std::string newTag = oldTag;
                            std::string fileKey = "file=\"";
                            size_t fkPos = newTag.find(fileKey);
                            if (fkPos != std::string::npos) {
                                size_t fvStart = fkPos + fileKey.size();
                                size_t fvEnd = newTag.find("\"", fvStart);
                                if (fvEnd != std::string::npos) {
                                    newTag.replace(fvStart, fvEnd - fvStart, foundSo);
                                }
                            }
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
                        // Not found in MPC.settings path nor in any SynthContentLocations -> installation is BROKEN
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
                // Not previously marked installed: check if present on disk in SynthContentLocations
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

    // Save updated MPC.settings if any relocated file="..." properties were repaired
    if (mpcSettingsModified && !mpcSettingsContent.empty()) {
        std::string bakPath = mpcSettingsPath + ".bak-" + std::to_string(static_cast<long>(std::time(nullptr)));
        std::ifstream src(mpcSettingsPath, std::ios::binary);
        std::ofstream bak(bakPath, std::ios::binary | std::ios::trunc);
        if (src.is_open() && bak.is_open()) {
            bak << src.rdbuf();
        }
        std::ofstream outSett(mpcSettingsPath, std::ios::binary | std::ios::trunc);
        if (outSett.is_open()) {
            outSett << mpcSettingsContent;
            outSett.close();
            if (outLogs) outLogs->push_back("[TKGL-SYNC] Updated relocated plugin file paths in " + mpcSettingsPath);
        }
    }
}

std::vector<SynthLocation> ConfigParser::getSynthContentLocations(const std::string& mpcSettingsPath) {
    std::vector<SynthLocation> locs;

    // Rule 2 & 3: Manager's subfolder "Synths" is always added as an ABSOLUTE path
    // Uses PLG_MAN_DIR if defined, otherwise /proc/self/exe directory
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

    // Read MPC.settings for registered SynthContentLocations
    std::ifstream f(mpcSettingsPath, std::ios::binary);
    if (f.is_open()) {
        std::stringstream ss;
        ss << f.rdbuf();
        std::string content = ss.str();
        const char* locKey = "<VALUE name=\"SynthContentLocations\">";
        size_t pos = content.find(locKey);
        if (pos != std::string::npos) {
            size_t endPos = content.find("</VALUE>", pos);
            std::string sub = content.substr(pos, endPos - pos);
            size_t tag = 0;
            while ((tag = sub.find("<Location>", tag)) != std::string::npos) {
                size_t closeTag = sub.find("</Location>", tag);
                if (closeTag == std::string::npos) break;
                std::string path = sub.substr(tag + 10, closeTag - (tag + 10));
                tag = closeTag + 11;
                while (path.size() > 1 && path.back() == '/') path.pop_back();
                // Only keep absolute paths, ignore relative ./Synths or duplicate of defaultMgrSynths
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

    std::time_t ts = std::time(nullptr);
    std::string tsStr = std::to_string(static_cast<long>(ts));

    // Discover MPC.settings file
    std::string mpcSettings = "/media/az01-internal/Settings/MPC/MPC.settings";
    const char* candMpc[] = {
        "/media/az01-internal/Settings/MPC/MPC.settings",
        "/media/internal/Settings/MPC/MPC.settings",
        "/var/lib/MPC/MPC.settings",
        "/root/.config/Akai/MPC/MPC.settings"
    };
    for (const char* cp : candMpc) {
        struct stat st;
        if (stat(cp, &st) == 0 && S_ISREG(st.st_mode)) {
            mpcSettings = cp;
            break;
        }
    }

    char scriptPath[] = "/tmp/tkgl_inst_XXXXXX";
    int fd = mkstemp(scriptPath);
    if (fd < 0) {
        outLogs.push_back("[ERROR] Could not create temp installer script.");
        return -1;
    }

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

    std::ostringstream ss;
    ss << "#!/bin/sh\n";
    ss << "export PLG_MAN_DIR=\"" << mgrDir << "\"\n";
    ss << "cd \"$PLG_MAN_DIR\" 2>/dev/null || true\n";
    ss << "SYNTH_DIR=\"" << synthDir << "\"\n";
    ss << "MPC_SETT=\"" << mpcSettings << "\"\n";
    ss << "TS=\"" << tsStr << "\"\n";
    ss << "ITEM_LABEL=\"" << item.label << "\"\n";
    ss << "ITEM_AUTHOR=\"" << item.author << "\"\n";
    ss << "ITEM_VER=\"" << item.version << "\"\n";
    ss << "ITEM_CAT=\"" << item.category << "\"\n";
    ss << "DEFAULT_SUBDIR=\"" << (!item.targetSubdir.empty() ? item.targetSubdir : (item.author + " - VST - " + item.label)) << "\"\n";
    ss << "DEFAULT_BIN=\"" << (!item.targetBinary.empty() ? item.targetBinary : (item.id + ".so")) << "\"\n";

    ss << "echo \"[TKGL-INSTALL] Step 1/4 (Gold Rule): Backing up MPC.settings...\"\n";
    ss << "if [ -f \"$MPC_SETT\" ]; then\n";
    ss << "  cp -f \"$MPC_SETT\" \"${MPC_SETT}.bak-${TS}\"\n";
    ss << "  echo \"[TKGL-INSTALL] Backup created: ${MPC_SETT}.bak-${TS}\"\n";
    ss << "fi\n";

    ss << "echo \"[TKGL-INSTALL] Step 2/4: Ensuring SynthContentLocations has $SYNTH_DIR...\"\n";
    ss << "mkdir -p \"$SYNTH_DIR\"\n";
    ss << "if [ -f \"$MPC_SETT\" ]; then\n";
    ss << "  # Strip erroneous relative ./Synths if present\n";
    ss << "  awk '!index($0, \"<Location>./Synths</Location>\")' \"$MPC_SETT\" > \"${MPC_SETT}.tmp\" && mv \"${MPC_SETT}.tmp\" \"$MPC_SETT\" 2>/dev/null || true\n";
    ss << "  if ! grep -q \"<Location>$SYNTH_DIR</Location>\" \"$MPC_SETT\"; then\n";
    ss << "    awk -v loc=\"      <Location>$SYNTH_DIR</Location>\" '\n";
    ss << "      index($0, \"name=\\\"SynthContentLocations\\\"\") { in_synth=1 }\n";
    ss << "      in_synth && index($0, \"</SynthContentLocations>\") { print loc; in_synth=0 }\n";
    ss << "      { print }\n";
    ss << "    ' \"$MPC_SETT\" > \"${MPC_SETT}.tmp\" && mv \"${MPC_SETT}.tmp\" \"$MPC_SETT\"\n";
    ss << "    echo \"[TKGL-INSTALL] Added $SYNTH_DIR to SynthContentLocations\"\n";
    ss << "  fi\n";
    ss << "fi\n";

    // Step 3A: Pre-download package directly in C++ via native libcurl.so.4 / libcurl.so.4.8.0 (dlopen)
    std::string tmpZipPath = "/tmp/" + item.id + "_" + tsStr + ".zip";
    if (!pkgUrl.empty()) {
        std::string sizeLabel = item.sizeMb.empty() ? "ZIP" : item.sizeMb;
        std::string dlMsg = "[TKGL-INSTALL] Step 3/4: Downloading " + item.label + " (" + sizeLabel + ") via libcurl.so.4...";
        outLogs.push_back(dlMsg);
        if (onProgress) onProgress(dlMsg);

        bool preDownloaded = downloadFile(pkgUrl, tmpZipPath, 180, [&](const std::string& progLine) {
            if (onProgress) onProgress(progLine);
        }, "[TKGL-NET] Downloading");

        if (preDownloaded) {
            struct stat stZip;
            double mb = (stat(tmpZipPath.c_str(), &stZip) == 0) ? (static_cast<double>(stZip.st_size) / 1048576.0) : 0.0;
            char doneBuf[128];
            std::snprintf(doneBuf, sizeof(doneBuf), "[TKGL-NET] libcurl.so.4 download complete (%.2f MB).", mb);
            outLogs.push_back(doneBuf);
            if (onProgress) onProgress(doneBuf);
        } else {
            outLogs.push_back("[TKGL-NET] Notice: direct libcurl.so.4 download did not complete, trying shell fallback...");
        }
    }

    ss << "echo \"[TKGL-INSTALL] Step 3/4: Deploying portable/ subdirectory...\"\n";
    if (!pkgUrl.empty()) {
        ss << "PKG_URL=\"" << pkgUrl << "\"\n";
        ss << "TMP_ZIP=\"" << tmpZipPath << "\"\n";
        ss << "TMP_EXT=\"/tmp/" << item.id << "_${TS}_pkg\"\n";
        ss << "rm -rf \"$TMP_EXT\"\n";
        ss << "mkdir -p \"$TMP_EXT\"\n";
        ss << "if [ ! -s \"$TMP_ZIP\" ]; then\n";
        ss << "  echo \"[TKGL-INSTALL] Source: $PKG_URL\"\n";
        ss << "  tkgl_dl() {\n";
        ss << "    _URL=\"$1\"\n";
        ss << "    _OUT=\"$2\"\n";
        ss << "    rm -f \"$_OUT\"\n";
        ss << "    /proc/self/exe --fetch \"$_URL\" \"$_OUT\" 2>/dev/null && [ -s \"$_OUT\" ] && return 0\n";
        ss << "    if command -v wget >/dev/null 2>&1; then\n";
        ss << "      wget --no-check-certificate -q -O \"$_OUT\" \"$_URL\" 2>/dev/null && [ -s \"$_OUT\" ] && return 0\n";
        ss << "      rm -f \"$_OUT\"\n";
        ss << "      wget -q -O \"$_OUT\" \"$_URL\" 2>/dev/null && [ -s \"$_OUT\" ] && return 0\n";
        ss << "      rm -f \"$_OUT\"\n";
        ss << "    fi\n";
        ss << "    if command -v python3 >/dev/null 2>&1; then\n";
        ss << "      python3 -c 'import urllib.request,ssl,shutil,sys; ctx=ssl._create_unverified_context(); req=urllib.request.Request(sys.argv[1],headers={\"User-Agent\":\"Wget/1.21\"}); r=urllib.request.urlopen(req,context=ctx,timeout=60); f=open(sys.argv[2],\"wb\"); shutil.copyfileobj(r,f,65536); f.close()' \"$_URL\" \"$_OUT\" 2>/dev/null && [ -s \"$_OUT\" ] && return 0\n";
        ss << "      rm -f \"$_OUT\"\n";
        ss << "    fi\n";
        ss << "    return 1\n";
        ss << "  }\n";
        ss << "  tkgl_dl \"$PKG_URL\" \"$TMP_ZIP\" || true\n";
        ss << "fi\n";
        ss << "if [ ! -s \"$TMP_ZIP\" ]; then\n";
        ss << "  echo \"[ERROR] Download failed (0 bytes received) from $PKG_URL\"\n";
        ss << "  rm -rf \"$TMP_ZIP\" \"$TMP_EXT\"\n";
        ss << "  exit 1\n";
        ss << "fi\n";
        ss << "FINAL_SZ=$(stat -c%s \"$TMP_ZIP\" 2>/dev/null || wc -c < \"$TMP_ZIP\" 2>/dev/null | tr -d ' ' || echo 0)\n";
        ss << "[ -z \"$FINAL_SZ\" ] && FINAL_SZ=0\n";
        ss << "FINAL_MB=$(awk -v s=\"$FINAL_SZ\" 'BEGIN { printf \"%.1f\", (s + 0) / 1048576 }')\n";
        ss << "echo \"[TKGL-INSTALL] Package ready (${FINAL_MB} MB). Extracting archive...\"\n";
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

    ss << "echo \"[TKGL-INSTALL] Step 4/4: Registering in MPC.settings pluginList-arm...\"\n";
    ss << "if [ -f \"$MPC_SETT\" ]; then\n";
    ss << "  META_LINE=\"\"\n";
    ss << "  if [ -f \"$TARGET_DIR/plugin-meta.xml\" ]; then\n";
    ss << "    if grep -q \"%payload-path%/$PLUGIN_DIR_NAME\" \"$TARGET_DIR/plugin-meta.xml\"; then\n";
    ss << "      META_LINE=$(grep \"<PLUGIN \" \"$TARGET_DIR/plugin-meta.xml\" | sed \"s|%payload-path%|$SYNTH_DIR|g\" | tr -d '\r')\n";
    ss << "    else\n";
    ss << "      META_LINE=$(grep \"<PLUGIN \" \"$TARGET_DIR/plugin-meta.xml\" | sed \"s|%payload-path%|$TARGET_DIR|g\" | tr -d '\r')\n";
    ss << "    fi\n";
    ss << "  fi\n";
    ss << "  if [ -z \"$META_LINE\" ]; then\n";
    ss << "    SO_FILE=$(find \"$TARGET_DIR\" -name \"*.so\" | head -n 1)\n";
    ss << "    if [ -n \"$SO_FILE\" ]; then\n";
    ss << "      META_LINE=\"<PLUGIN name=\\\"$ITEM_LABEL\\\" descriptiveName=\\\"$ITEM_LABEL\\\" format=\\\"VST\\\" category=\\\"$ITEM_CAT\\\" manufacturer=\\\"$ITEM_AUTHOR\\\" version=\\\"$ITEM_VER\\\" file=\\\"$SO_FILE\\\" isInstrument=\\\"1\\\" fileTime=\\\"0\\\" infoUpdateTime=\\\"0\\\" numInputs=\\\"0\\\" numOutputs=\\\"2\\\" isShell=\\\"0\\\"/>\"\n";
    ss << "    fi\n";
    ss << "  fi\n";
    ss << "  if [ -n \"$META_LINE\" ]; then\n";
    ss << "    if ! grep -q 'name=\"pluginList-arm\"' \"$MPC_SETT\"; then\n";
    ss << "      awk '\n";
    ss << "        index($0, \"</PROPERTIES>\") {\n";
    ss << "          print \"  <VALUE name=\\\"pluginList-arm\\\">\"\n";
    ss << "          print \"    <KNOWNPLUGINS>\"\n";
    ss << "          print \"    </KNOWNPLUGINS>\"\n";
    ss << "          print \"  </VALUE>\"\n";
    ss << "        }\n";
    ss << "        { print }\n";
    ss << "      ' \"$MPC_SETT\" > \"${MPC_SETT}.tmp\" && mv \"${MPC_SETT}.tmp\" \"$MPC_SETT\"\n";
    ss << "    fi\n";
    ss << "    # Purge any previous or broken registration lines in MPC.settings\n";
    ss << "    awk -v pdir=\"$PLUGIN_DIR_NAME\" -v defdir=\"$DEFAULT_SUBDIR\" -v defbin=\"$DEFAULT_BIN\" -v pname=\"name=\\\"$ITEM_LABEL\\\"\" -v is_repair=\"" << (item.broken ? "1" : "0") << "\" '\n";
    ss << "      index($0, \"<PLUGIN \") {\n";
    ss << "        if ((length(pdir) > 2 && index($0, pdir)) || (length(defdir) > 2 && index($0, defdir)) || (length(defbin) > 2 && index($0, defbin)) || (length(pname) > 6 && index($0, pname))) {\n";
    ss << "          next\n";
    ss << "        }\n";
    ss << "        if (is_repair == \"1\" && match($0, /file=\"[^\"]+\"/)) {\n";
    ss << "          fpath = substr($0, RSTART + 6, RLENGTH - 7)\n";
    ss << "          cmd = \"test -f \\\"\" fpath \"\\\"\"\n";
    ss << "          if (system(cmd) != 0) {\n";
    ss << "            next\n";
    ss << "          }\n";
    ss << "        }\n";
    ss << "      }\n";
    ss << "      { print }\n";
    ss << "    ' \"$MPC_SETT\" > \"${MPC_SETT}.tmp\" && mv \"${MPC_SETT}.tmp\" \"$MPC_SETT\"\n";
    ss << "    # Insert new clean registration line into pluginList-arm\n";
    ss << "    awk -v newline=\"      $META_LINE\" '\n";
    ss << "      index($0, \"name=\\\"pluginList-arm\\\"\") { in_arm=1 }\n";
    ss << "      in_arm && index($0, \"<KNOWNPLUGINS/>\") {\n";
    ss << "        print \"    <KNOWNPLUGINS>\"\n";
    ss << "        print newline\n";
    ss << "        print \"    </KNOWNPLUGINS>\"\n";
    ss << "        in_arm=0\n";
    ss << "        next\n";
    ss << "      }\n";
    ss << "      in_arm && index($0, \"</KNOWNPLUGINS>\") {\n";
    ss << "        print newline\n";
    ss << "        in_arm=0\n";
    ss << "      }\n";
    ss << "      { print }\n";
    ss << "    ' \"$MPC_SETT\" > \"${MPC_SETT}.tmp\" && mv \"${MPC_SETT}.tmp\" \"$MPC_SETT\"\n";
    ss << "    echo \"[TKGL-INSTALL] Clean plugin entry registered in KNOWNPLUGINS:\"\n";
    ss << "    echo \"  $META_LINE\"\n";
    ss << "  else\n";
    ss << "    echo \"[WARNING] No <PLUGIN ... /> line found or created for $TARGET_DIR\"\n";
    ss << "  fi\n";
    ss << "fi\n";
    ss << "sync\n";
    ss << "echo \"[TKGL-INSTALL] SUCCESS: $PLUGIN_DIR_NAME " << (item.broken ? "repaired" : "installed") << " in $SYNTH_DIR\"\n";

    std::string sBody = ss.str();
    ssize_t written = write(fd, sBody.c_str(), sBody.size());
    (void)written;
    close(fd);
    chmod(scriptPath, 0755);

    std::string runCmd = std::string("/bin/sh ") + scriptPath + " 2>&1; rm -f " + scriptPath;
    FILE* pipe = popen(runCmd.c_str(), "r");
    if (!pipe) {
        outLogs.push_back("[ERROR] Failed to run installer script.");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;
        outLogs.emplace_back(line);
        if (onProgress) {
            onProgress(line);
        }
    }
    return pclose(pipe);
}

int ConfigParser::executeItemUninstall(
    const LauncherConfig& cfg,
    const MenuItem& item,
    std::vector<std::string>& outLogs,
    const std::function<void(const std::string&)>& onProgress
) {
    (void)cfg;
    std::string mgrDir = getManagerRootDir();
    char scriptPath[] = "/tmp/tkgl_uninst_XXXXXX";
    int fd = mkstemp(scriptPath);
    if (fd < 0) {
        outLogs.push_back("[ERROR] Failed to create temporary uninstall script in /tmp.");
        return -1;
    }

    std::string defaultSubdir = item.targetSubdir.empty()
        ? (item.author + " - VST - " + item.label)
        : item.targetSubdir;
    std::string defaultBin = item.targetBinary.empty()
        ? (item.id + ".so")
        : item.targetBinary;

    std::ostringstream ss;
    ss << "#!/bin/sh\n";
    ss << "export PLG_MAN_DIR=\"" << mgrDir << "\"\n";
    ss << "cd \"$PLG_MAN_DIR\" 2>/dev/null || true\n";
    ss << "MPC_SETT=\"/media/az01-internal/Settings/MPC/MPC.settings\"\n";
    ss << "DEFAULT_SUBDIR=\"" << defaultSubdir << "\"\n";
    ss << "DEFAULT_BIN=\"" << defaultBin << "\"\n";
    ss << "ITEM_LABEL=\"" << item.label << "\"\n";
    ss << "TS=$(date +%s)\n\n";

    ss << "echo \"[TKGL-REMOVE] Step 1/3 (Gold Rule): Backing up MPC.settings...\"\n";
    ss << "if [ -f \"$MPC_SETT\" ]; then\n";
    ss << "  cp -f \"$MPC_SETT\" \"${MPC_SETT}.bak-${TS}\"\n";
    ss << "  echo \"[TKGL-REMOVE] Backup saved: ${MPC_SETT}.bak-${TS}\"\n";
    ss << "fi\n\n";

    ss << "echo \"[TKGL-REMOVE] Step 2/3: Removing plugin entry from MPC.settings KNOWNPLUGINS...\"\n";
    ss << "if [ -f \"$MPC_SETT\" ]; then\n";
    ss << "  awk -v defdir=\"$DEFAULT_SUBDIR\" -v defbin=\"$DEFAULT_BIN\" -v pname=\"name=\\\"$ITEM_LABEL\\\"\" '\n";
    ss << "    index($0, \"<PLUGIN \") && ((length(defdir) > 2 && index($0, defdir)) || (length(defbin) > 2 && index($0, defbin)) || (length(pname) > 6 && index($0, pname))) {\n";
    ss << "      next\n";
    ss << "    }\n";
    ss << "    { print }\n";
    ss << "  ' \"$MPC_SETT\" > \"${MPC_SETT}.tmp\" && mv \"${MPC_SETT}.tmp\" \"$MPC_SETT\"\n";
    ss << "  echo \"[TKGL-REMOVE] Unregistered $ITEM_LABEL from MPC.settings.\"\n";
    ss << "fi\n\n";

    ss << "echo \"[TKGL-REMOVE] Step 3/3: Removing plugin files from SynthContentLocations...\"\n";
    auto locs = getSynthContentLocations();
    for (const auto& loc : locs) {
        if (loc.path.empty()) continue;
        ss << "if [ -d \"" << loc.path << "/$DEFAULT_SUBDIR\" ]; then\n";
        ss << "  rm -rf \"" << loc.path << "/$DEFAULT_SUBDIR\"\n";
        ss << "  echo \"[TKGL-REMOVE] Deleted: " << loc.path << "/$DEFAULT_SUBDIR\"\n";
        ss << "fi\n";
    }
    ss << "if [ -d \"$PLG_MAN_DIR/Synths/$DEFAULT_SUBDIR\" ]; then\n";
    ss << "  rm -rf \"$PLG_MAN_DIR/Synths/$DEFAULT_SUBDIR\"\n";
    ss << "  echo \"[TKGL-REMOVE] Deleted: $PLG_MAN_DIR/Synths/$DEFAULT_SUBDIR\"\n";
    ss << "fi\n";
    ss << "sync\n";
    ss << "echo \"[TKGL-REMOVE] SUCCESS: $ITEM_LABEL cleanly uninstalled.\"\n";

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
        outLogs.emplace_back(line);
        if (onProgress) {
            onProgress(line);
        }
    }
    return pclose(pipe);
}

int ConfigParser::executeItemCommand(
    const LauncherConfig& cfg,
    const MenuItem& item,
    std::vector<std::string>& outLogs
) {
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
            std::string scriptBody = "#!/bin/sh\nexport PLG_MAN_DIR=\"" + mgrDir + "\"\ncd \"$PLG_MAN_DIR\" 2>/dev/null || true\nexport SYNTH_INSTALL_DIR=\"" + cfg.synth_dir_name + "\"\n" + item.cmd + "\n";
            ssize_t written = write(fd, scriptBody.c_str(), scriptBody.size());
            (void)written;
            close(fd);
            chmod(tmpPath, 0755);
            finalCmd = std::string("/bin/sh ") + tmpPath + " 2>&1; rm -f " + tmpPath;
        } else {
            finalCmd = "PLG_MAN_DIR=\"" + mgrDir + "\" SYNTH_INSTALL_DIR=\"" + cfg.synth_dir_name + "\" /bin/sh -c '" + item.cmd + "' 2>&1";
        }
    } else {
        finalCmd = "PLG_MAN_DIR=\"" + mgrDir + "\" SYNTH_INSTALL_DIR=\"" + cfg.synth_dir_name + "\" /bin/sh " + item.cmd + " 2>&1";
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
        (void)line;
        animFrame_++;
        lastActivityTime_ = std::time(nullptr);
        if (execLogs_.size() > 18) {
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

    // 2. Main Title + "By The KikGen Labs" on same baseline (matching image_22ce5693.png)
    int titleX = 76;
    canvas_.drawTextClipped({titleX, 21, 430, 22}, config_.app_title, Palette::TextPrimary, 2, true);
    int titleW = std::min(430, canvas_.measureText(config_.app_title, 2) + 14);
    canvas_.drawText(titleX + titleW, 26, "By The KikGen Labs", Palette::TextSecondary, 1, false);

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

    // 4. Glowing Green ONLINE Dot + "ONLINE" Label
    canvas_.drawGlowDot(W - 292, 31, 5, Palette::StatusGreen);
    canvas_.drawText(W - 278, 23, "ONLINE", Palette::TextPrimary, 2, true);

    // 5. USB /Synth Free Space Progress Pill + GB Free Label
    double freeGB = 202.8;
    int usedPct = 24;
    struct statvfs vfs;
    if (statvfs(config_.usb_mount_path.c_str(), &vfs) == 0 && vfs.f_blocks > 0) {
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

    canvas_.drawText(infoBox.x + 14, infoBox.y + 36, "1. Gold Rule: Auto-snapshot MPC.settings.bak-(timestamp) before any modification", Palette::TextSecondary, 1, false);
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
    // Headless helper mode: mpc_plugin_manager_fb --fetch <url> <dest>
    // Uses dynamic libcurl.so.4 / libcurl.so.4.8.0 (dlopen) for background/script downloads.
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

