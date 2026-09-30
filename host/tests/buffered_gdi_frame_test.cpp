#include "buffered_gdi_frame.h"
#include <array>
#include <cstdio>
#include <cstdlib>

static void check(bool ok) { if (!ok) std::abort(); }
struct Target {
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = nullptr;
    HGDIOBJ original = nullptr;
    unsigned *bits = nullptr;
    Target() {
        BITMAPINFO info{};
        info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=12; info.bmiHeader.biHeight=-8;
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void **>(&bits),nullptr,0);
        check(dc && bitmap && bits);original=SelectObject(dc,bitmap);check(original!=nullptr);
    }
    ~Target() { SelectObject(dc,original);DeleteObject(bitmap);DeleteDC(dc); }
};
int main() {
    Target target;
    std::array<unsigned,16> image{};
    const std::array<unsigned,4> palette{0xFFC7D6A3u,0xFF91A873u,0xFF526742u,0xFF1D2B22u};
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)image[y*4+x]=palette[y];
    const DWORD before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    {
        BufferedGdiFrame surface;
        check(!surface.present(target.dc));check(!surface.ensure(0,8));
        check(surface.ensure(12,8));check(!surface.present(target.dc));
        // Off-screen composition MUST NOT touch the destination.
        for(int i=0;i<96;++i)target.bits[i]=0x123456;
        check(surface.compose(image.data(),4,4));
        for(int i=0;i<96;++i)check(target.bits[i]==0x123456);
        check(surface.present(target.dc));
        // Four exact palette bands at 2x scale and black left/right margins.
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<12;++x)
            check((target.bits[y*12+x]&0xffffff)==((x<2 || x>=10)?0:(palette[y/2]&0xffffff)));
        std::array<unsigned,96> previous{};
        for(int i=0;i<96;++i)previous[i]=target.bits[i];
        check(!surface.compose(nullptr,4,4));check(!surface.present(target.dc));
        for(int i=0;i<96;++i)check(target.bits[i]==previous[i]);
        // A legitimately dark game frame must not be filtered or held back.
        image.fill(palette[3]);check(surface.compose(image.data(),4,4));check(surface.present(target.dc));
        check((target.bits[4*12+4]&0xffffff)==(palette[3]&0xffffff));
        for(int i=0;i<2000;++i){check(surface.ensure(i%2?8:12,8));check(surface.compose(image.data(),4,4));check(surface.present(target.dc));}
    }
    check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==before);
    std::puts("Buffered GDI: completed-only presentation, exact pixels/margins, failure hold, dark frames and 2000 leak-free resizes OK");
}
