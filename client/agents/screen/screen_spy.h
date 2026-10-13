// screen_spy.h — GDI 屏幕捕获对象，对照逆向 sub_10007960（CScreenSpy 构造）
#pragma once

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <cstdint>

namespace c2 {

class ScreenSpy {
public:
    ~ScreenSpy();

    // 创建抓屏对象：bits_per_pixel 支持 8/24/32（原代码 3→4bpp、7→8bpp，其余原样；
    // 默认 32，对应原 StartScreenCapture(32)）。use_palette 对应原 a3（8bpp 调色板）。
    // 区域参数（虚拟屏幕坐标，可负值）：region_w/region_h <= 0 时抓全屏；
    // 否则抓取 (region_x, region_y) 起 region_w x region_h 的区域。
    // 失败返回 nullptr。
    static ScreenSpy* create(int bits_per_pixel, bool use_palette,
                             int region_x = -1, int region_y = -1,
                             int region_w = -1, int region_h = -1,
                             float scale = 1.0f);

    // 抓一帧到内部 DIB；成功返回 true
    bool grab();

    // --- 元数据 ---
    const uint8_t* pixels() const { return pixels_; }
    int stride() const { return stride_; }
    int data_size() const { return stride_ * height_; }

    int width() const { return width_; }       // 偶数化后的宽（原 +24）
    int height() const { return height_; }     // 偶数化后的高（原 +28）
    int bpp() const { return bpp_; }           // 每像素位数（原 +32）
    int origin_x() const { return origin_x_; } // 虚拟屏幕原点 X（原 +12）
    int origin_y() const { return origin_y_; } // 虚拟屏幕原点 Y（原 +16）

private:
    ScreenSpy() = default;

    int src_x_;      // 原始区域左上角 X（虚拟屏幕坐标）
    int src_y_;      // 原始区域左上角 Y
    int src_width_;  // 原始区域宽度
    int src_height_; // 原始区域高度

    // 成员布局对照 sub_10007960 的偏移（仅保留 demo 需要的字段）
    HWND       desktop_wnd_ = nullptr;   // +8  GetDesktopWindow()（未切换桌面时）
    int        origin_x_ = 0;            // +12 SM_XVIRTUALSCREEN * dpi_scale
    int        origin_y_ = 0;            // +16 SM_YVIRTUALSCREEN * dpi_scale
    float      dpi_scale_ = 1.0f;        // +20 HORZSIZE / HORZRES
    int        width_ = 0;               // +24 偶数化宽度
    int        height_ = 0;              // +28 偶数化高度
    int        bpp_ = 0;                 // +32 每像素位数
    bool       use_palette_ = false;     // +48 调色板标志（原 a3）
    HDC        screen_dc_ = nullptr;     // +56 屏幕 DC
    HDC        mem_dc_ = nullptr;        // +60 内存 DC（原 CreateCompatibleDC 之一）
    uint8_t*   pixels_ = nullptr;        // +72 DIB 位图内存（原双 DIB 之一，demo 单缓冲）
    BITMAPINFO* bmi_ = nullptr;          // +84 DIB 描述（原 sub_10007EA0 创建）
    HBITMAP    dib_bitmap_ = nullptr;    // +92 DIB section 位图
    int        stride_ = 0;              // 行字节数（4 字节对齐）
    // 原 +36/+40/+44 为位图压缩标志与扩展字段（13369376/1087111200、14、1），
    // demo 不实现 RLE 压缩，相关语义注释于此：13369376 = 0xCC0040，
    // 1087111200 = 0x40CC0040（压缩模式位 + 调色板参数），见 StartScreenCapture。
};

}  // namespace c2
