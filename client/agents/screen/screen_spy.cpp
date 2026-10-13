// screen_spy.cpp — GDI 抓屏实现，对照逆向 sub_10007960（CScreenSpy 构造）
#include "screen_spy.h"

#include "desktop_switch.h"
#include "protocol.h"

#include <cstring>

namespace c2 {

ScreenSpy::~ScreenSpy() {
    if (mem_dc_) {
        if (dib_bitmap_)
            SelectObject(mem_dc_, (HGDIOBJ)GetStockObject(NULL_BRUSH));  // 解除选择
        DeleteDC(mem_dc_);
    }
    if (dib_bitmap_)
        DeleteObject(dib_bitmap_);
    if (screen_dc_)
        ReleaseDC(desktop_wnd_ ? desktop_wnd_ : nullptr, screen_dc_);
    if (bmi_)
        delete[] reinterpret_cast<char*>(bmi_);
}

ScreenSpy* ScreenSpy::create(int bits_per_pixel, bool use_palette,
                             int region_x, int region_y,
                             int region_w, int region_h, float scale) {
    // 原代码 bitsPerPixel 特殊值：3→4bpp、7→8bpp；其余保持原值
    int bpp = bits_per_pixel;
    if (bpp == 3)      bpp = 4;    // 4bpp（demo 不支持，见下）
    else if (bpp == 7) bpp = 8;    // 8bpp
    if (bpp != 8 && bpp != 24 && bpp != 32)
        bpp = 32;                  // 默认 32bpp，对应 StartScreenCapture(32)

    ScreenSpy* spy = new ScreenSpy();
    spy->bpp_ = bpp;
    spy->use_palette_ = use_palette;

    // --- 屏幕 DC：对应原代码 f_SwitchThreadToInpurDesktop() 分支 ---
    // 若线程已切到活动桌面则 GetDC(0)，否则 GetDesktopWindow + GetDC
    HDC dc = nullptr;
    if (switch_thread_to_input_desktop()) {
        dc = GetDC(nullptr);
    } else {
        spy->desktop_wnd_ = GetDesktopWindow();
        dc = GetDC(spy->desktop_wnd_);
    }
    if (!dc) {
        delete spy;
        return nullptr;
    }
    spy->screen_dc_ = dc;

    // --- DPI 缩放：原 +20 = GetDeviceCaps(HORZSIZE=118) / GetDeviceCaps(HORZRES=8) ---
    HDC probe = GetDC(nullptr);
    int horz_res = GetDeviceCaps(probe, HORZRES);
    int horz_size = GetDeviceCaps(probe, HORZSIZE);
    // 原逆向代码：dpi_scale = HORZSIZE / HORZRES（物理毫米 / 像素），
    // 会把 2560x1440 缩放成 ~598x336（降采样，只看到左上角）。
    // 此处改为 1.0 全分辨率抓屏；如需降低带宽可改回该比值。
    (void)horz_size;
    spy->dpi_scale_ = (scale > 0.0f) ? scale : 1.0f;  // 可配置抓屏缩放
    ReleaseDC(nullptr, probe);

    // 1. 根据参数或全屏确定原始区域
    if (region_w > 0 && region_h > 0) {
        spy->src_x_ = region_x;
        spy->src_y_ = region_y;
        spy->src_width_ = region_w;
        spy->src_height_ = region_h;
    }
    else {
        spy->src_x_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
        spy->src_y_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
        spy->src_width_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        spy->src_height_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

    // 2. 根据缩放系数计算目标区域（用于 DIB 位图尺寸和内存 DC）
    spy->origin_x_ = (int)(spy->src_x_ * spy->dpi_scale_);
    spy->origin_y_ = (int)(spy->src_y_ * spy->dpi_scale_);
    spy->width_ = (int)(spy->src_width_ * spy->dpi_scale_);
    spy->height_ = (int)(spy->src_height_ * spy->dpi_scale_);

    // 3. 目标宽高偶数化（只影响目标）
    if (spy->width_ % 2)  spy->width_ -= 1;
    if (spy->height_ % 2) spy->height_ += 1;


    if (spy->width_ <= 0 || spy->height_ <= 0) {
        delete spy;
        return nullptr;
    }

    // --- DIB 描述（原 sub_10007EA0 工厂）---
    size_t bmi_size = sizeof(BITMAPINFOHEADER) +
                      (bpp == 8 ? 256 * sizeof(RGBQUAD) : 0);
    spy->bmi_ = reinterpret_cast<BITMAPINFO*>(new char[bmi_size]);
    std::memset(spy->bmi_, 0, bmi_size);
    BITMAPINFOHEADER& bi = spy->bmi_->bmiHeader;
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = spy->width_;
    bi.biHeight = -spy->height_;   // 负值：自顶向下，便于直接传输/显示
    bi.biPlanes = 1;
    bi.biBitCount = (WORD)bpp;
    bi.biCompression = BI_RGB;
    if (bpp == 8) {
        // 8bpp 标准灰度调色板（use_palette 时），BitBlt 会自动量化
        for (int i = 0; i < 256; ++i) {
            spy->bmi_->bmiColors[i].rgbRed = (BYTE)i;
            spy->bmi_->bmiColors[i].rgbGreen = (BYTE)i;
            spy->bmi_->bmiColors[i].rgbBlue = (BYTE)i;
        }
    }

    // --- DIB section + 内存 DC（原 +60/+64/+68 三 DC、双 DIB，demo 单缓冲）---
    spy->mem_dc_ = CreateCompatibleDC(spy->screen_dc_);
    if (!spy->mem_dc_) {
        delete spy;
        return nullptr;
    }
    spy->dib_bitmap_ = CreateDIBSection(spy->screen_dc_, spy->bmi_,
                                        DIB_RGB_COLORS, (void**)&spy->pixels_,
                                        nullptr, 0);
    if (!spy->dib_bitmap_ || !spy->pixels_) {
        delete spy;
        return nullptr;
    }
    SelectObject(spy->mem_dc_, spy->dib_bitmap_);

    // 行字节数（4 字节对齐）
    int bits_per_row = spy->width_ * bpp;
    spy->stride_ = ((bits_per_row + 31) / 32) * 4;

    return spy;
}

bool ScreenSpy::grab() {
    if (!screen_dc_ || !mem_dc_) return false;

    // 设置拉伸模式（可放在 create 中一次性设置，也可每次设置）
    SetStretchBltMode(mem_dc_, HALFTONE);

    BOOL ok = StretchBlt(mem_dc_, 0, 0, width_, height_,
        screen_dc_, src_x_, src_y_, src_width_, src_height_,
        SRCCOPY);
    return ok != FALSE;
}

}  // namespace c2
