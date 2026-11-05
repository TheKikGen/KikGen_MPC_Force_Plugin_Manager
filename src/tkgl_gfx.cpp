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

#include "tkgl_gfx.hpp"
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
