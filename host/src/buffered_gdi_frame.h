#pragma once
#include <windows.h>

// A single reusable composition surface, NOT a video-frame queue. All calls
// are on the UI thread. Only a completed surface may be copied to the window.
class BufferedGdiFrame {
public:
    BufferedGdiFrame() = default;
    BufferedGdiFrame(const BufferedGdiFrame &) = delete;
    BufferedGdiFrame &operator=(const BufferedGdiFrame &) = delete;
    ~BufferedGdiFrame() { reset(); }

    bool ensure(int width, int height) {
        if (width <= 0 || height <= 0) return false;
        if (dc_ && width == width_ && height == height_) return true;
        HDC next_dc = CreateCompatibleDC(nullptr);
        if (!next_dc) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HBITMAP next_bitmap = CreateDIBSection(next_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!next_bitmap) { DeleteDC(next_dc); return false; }
        HGDIOBJ next_original = SelectObject(next_dc, next_bitmap);
        if (!next_original || next_original == HGDI_ERROR) {
            DeleteObject(next_bitmap); DeleteDC(next_dc); return false;
        }
        // Allocation failure never destroys a usable old surface.
        reset();
        dc_ = next_dc; bitmap_ = next_bitmap; original_ = next_original;
        width_ = width; height_ = height;
        return true;
    }

    bool compose(const void *pixels, int source_width, int source_height) {
        complete_ = false;
        if (!dc_ || !pixels || source_width <= 0 || source_height <= 0) return false;
        RECT client{0,0,width_,height_};
        if (!FillRect(dc_, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)))) return false;
        const int side = width_ < height_ ? width_ : height_;
        BITMAPINFO source{};
        source.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        source.bmiHeader.biWidth = source_width;
        source.bmiHeader.biHeight = -source_height;
        source.bmiHeader.biPlanes = 1;
        source.bmiHeader.biBitCount = 32;
        source.bmiHeader.biCompression = BI_RGB;
        if (!SetStretchBltMode(dc_, COLORONCOLOR)) return false;
        const int copied = StretchDIBits(dc_, (width_-side)/2, (height_-side)/2,
            side, side, 0, 0, source_width, source_height, pixels, &source, DIB_RGB_COLORS, SRCCOPY);
        if (copied == 0 || static_cast<unsigned>(copied) == GDI_ERROR) return false;
        // Complete off-screen GDI work before permitting a presentation. If
        // drawing fails, the visible client area has never been erased.
        complete_ = GdiFlush() != FALSE;
        return complete_;
    }

    bool present(HDC destination) const {
        if (!complete_ || !destination) return false;
        if (!BitBlt(destination, 0, 0, width_, height_, dc_, 0, 0, SRCCOPY)) return false;
        return GdiFlush() != FALSE;
    }

private:
    void reset() {
        if (dc_ && original_) SelectObject(dc_, original_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr; bitmap_ = nullptr; original_ = nullptr;
        width_ = height_ = 0; complete_ = false;
    }
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_ = nullptr;
    int width_ = 0, height_ = 0;
    bool complete_ = false;
};
