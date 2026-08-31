#include "InstallerPainter.hpp"
#include "InstallerLayout.hpp"

#include <dwrite.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using yanami::installer::ui::InstallerPainter;
using yanami::installer::ui::TextAlign;

unsigned assertions = 0;

void expect(bool condition, const char* message) {
    ++assertions;
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename T>
struct ComValue {
    T* value = nullptr;
    ~ComValue() { if (value) { value->Release(); } }
    ComValue() = default;
    ComValue(const ComValue&) = delete;
    ComValue& operator=(const ComValue&) = delete;
    T* operator->() const noexcept { return value; }
};

// Observe the faces and glyphs actually selected by DirectWrite, including its
// fallback runs. Checking the requested family alone cannot detect CJK tofu.
class GlyphProbe final : public IDWriteTextRenderer {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining) {
            delete this;
        }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (IsEqualIID(iid, __uuidof(IUnknown))
            || IsEqualIID(iid, __uuidof(IDWriteTextRenderer))
            || IsEqualIID(iid, __uuidof(IDWritePixelSnapping))) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* matrix) override {
        *matrix = {1, 0, 0, 1, 0, 0};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override {
        *pixels = 1.0f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*, FLOAT, FLOAT, DWRITE_MEASURING_MODE,
        const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
        IUnknown*) override {
        if (!run || !run->fontFace || !run->glyphIndices) {
            return E_INVALIDARG;
        }
        glyphCount += run->glyphCount;
        for (UINT32 index = 0; index < run->glyphCount; ++index) {
            if (run->glyphIndices[index] == 0) {
                ++missingGlyphCount;
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(
        void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(
        void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(
        void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object,
        BOOL sideways, BOOL rtl, IUnknown* effect) override {
        return object->Draw(context, this, x, y, sideways, rtl, effect);
    }

    unsigned glyphCount = 0;
    unsigned missingGlyphCount = 0;

private:
    ULONG references_ = 1;
};

class Surface final {
public:
    Surface(int width, int height) : width_(width), height_(height) {
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) {
            throw std::runtime_error("CreateCompatibleDC failed");
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap_ = CreateDIBSection(dc_, &info, DIB_RGB_COLORS,
                                   reinterpret_cast<void**>(&pixels_), nullptr, 0);
        if (!bitmap_) {
            DeleteDC(dc_);
            dc_ = nullptr;
            throw std::runtime_error("CreateDIBSection failed");
        }
        original_ = SelectObject(dc_, bitmap_);
        if (!original_ || original_ == HGDI_ERROR) {
            DeleteObject(bitmap_);
            DeleteDC(dc_);
            dc_ = nullptr;
            bitmap_ = nullptr;
            throw std::runtime_error("SelectObject failed");
        }
        std::fill_n(pixels_, width_ * height_, 0);
    }

    ~Surface() {
        if (dc_) {
            SelectObject(dc_, original_);
            DeleteObject(bitmap_);
            DeleteDC(dc_);
        }
    }
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    HDC dc() const noexcept { return dc_; }
    RECT bounds() const noexcept { return {0, 0, width_, height_}; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    std::uint32_t pixel(int x, int y) const noexcept {
        return pixels_[y * width_ + x] & 0xffffffu;
    }
    std::vector<std::uint32_t> rgbPixels() const {
        std::vector<std::uint32_t> result(width_ * height_);
        std::transform(pixels_, pixels_ + result.size(), result.begin(),
                       [](std::uint32_t pixel) { return pixel & 0xffffffu; });
        return result;
    }

private:
    int width_;
    int height_;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_ = nullptr;
    std::uint32_t* pixels_ = nullptr;
};

RECT scaledRect(float scale, int left, int top, int right, int bottom) {
    return {static_cast<LONG>(std::lround(left * scale)),
            static_cast<LONG>(std::lround(top * scale)),
            static_cast<LONG>(std::lround(right * scale)),
            static_cast<LONG>(std::lround(bottom * scale))};
}

unsigned intermediatePixels(const Surface& surface, const RECT& bounds) {
    unsigned count = 0;
    for (int y = bounds.top; y < bounds.bottom; ++y) {
        for (int x = bounds.left; x < bounds.right; ++x) {
            const auto pixel = surface.pixel(x, y);
            if (pixel != 0 && pixel != 0xffffff) {
                ++count;
            }
        }
    }
    return count;
}

void expectGrayscale(const Surface& surface) {
    for (int y = 0; y < surface.height(); ++y) {
        for (int x = 0; x < surface.width(); ++x) {
            const auto value = surface.pixel(x, y);
            const auto blue = value & 0xff;
            const auto green = (value >> 8) & 0xff;
            const auto red = (value >> 16) & 0xff;
            if (red != green || green != blue) {
                expect(false, "grayscale antialiasing must not add colored fringes");
            }
        }
    }
    expect(true, "grayscale antialiasing must not add colored fringes");
}

void testGeometryAtScale(float scale) {
    Surface surface(static_cast<int>(300 * scale), static_cast<int>(160 * scale));
    InstallerPainter painter;
    expect(painter.begin(surface.dc(), surface.bounds()), "geometry begin should succeed");
    painter.clear(RGB(0, 0, 0));
    const RECT rounded = scaledRect(scale, 12, 12, 172, 64);
    const RECT circle = scaledRect(scale, 200, 12, 252, 64);
    painter.roundedRect(rounded, 16.0f * scale, RGB(255, 255, 255),
                        RGB(255, 255, 255), 0.0f);
    painter.ellipse(circle, RGB(255, 255, 255));
    painter.line(18.0f * scale, 100.0f * scale, 168.0f * scale,
                 137.0f * scale, RGB(255, 255, 255), scale);
    expect(painter.end(), "geometry end should succeed");
    GdiFlush();
    expect(intermediatePixels(surface, rounded) > 25,
           "rounded corners must have antialiased intermediate pixels");
    expect(intermediatePixels(surface, circle) > 25,
           "circular toggle thumbs must have antialiased intermediate pixels");
    expect(intermediatePixels(surface, scaledRect(scale, 16, 98, 171, 140)) > 25,
           "diagonal lines must have antialiased intermediate pixels");
    expectGrayscale(surface);

    const auto integerGeometry = surface.rgbPixels();
    expect(painter.begin(surface.dc(), surface.bounds()), "subpixel compatibility begin should succeed");
    painter.clear(RGB(0, 0, 0));
    painter.roundedRectSubpixel(
        {static_cast<float>(rounded.left), static_cast<float>(rounded.top),
         static_cast<float>(rounded.right), static_cast<float>(rounded.bottom)},
        16.0f * scale, RGB(255, 255, 255), RGB(255, 255, 255), 0.0f);
    painter.ellipse(circle, RGB(255, 255, 255));
    painter.line(18.0f * scale, 100.0f * scale, 168.0f * scale,
                 137.0f * scale, RGB(255, 255, 255), scale);
    expect(painter.end(), "subpixel compatibility end should succeed");
    GdiFlush();
    expect(integerGeometry == surface.rgbPixels(),
           "integer rounded rectangles must retain their exact rendering through the subpixel implementation");

    expect(painter.begin(surface.dc(), surface.bounds()), "button begin should succeed");
    painter.clear(RGB(9, 11, 16));
    painter.roundedRect(rounded, 12.0f * scale, RGB(255, 102, 135),
                        RGB(255, 102, 135), 0.0f);
    expect(painter.end(), "button end should succeed");
    GdiFlush();
    bool brightBorder = false;
    for (int y = 0; y < surface.height(); ++y) {
        for (int x = 0; x < surface.width(); ++x) {
            const auto value = surface.pixel(x, y);
            if (((value >> 8) & 0xff) > 102 || (value & 0xff) > 135) {
                brightBorder = true;
            }
        }
    }
    expect(!brightBorder, "unfocused accent button must never acquire a white outline");
}

void testTextAtScale(float scale, bool chinese) {
    Surface surface(static_cast<int>(700 * scale), static_cast<int>(180 * scale));
    InstallerPainter painter;
    const auto value = chinese ? L"选择安装方式 · 安装 Yanami" : L"Install Yanami · Agypj";
    const RECT compact = scaledRect(scale, 20, 20, 670, 76);
    RECT roomy = compact;
    roomy.bottom = static_cast<LONG>(150 * scale);
    expect(painter.begin(surface.dc(), surface.bounds()), "text begin should succeed");
    painter.clear(RGB(0, 0, 0));
    painter.text(value, compact, 32.0f * scale, true, RGB(255, 255, 255),
                 TextAlign::Left, false, false, false, chinese);
    expect(painter.end(), "text end should succeed");
    GdiFlush();
    const auto compactPixels = surface.rgbPixels();
    expect(intermediatePixels(surface, compact) > 100,
           "text must use antialiased fractional glyph outlines");
    expectGrayscale(surface);

    LONG firstInk = surface.height();
    LONG lastInk = -1;
    for (LONG y = 0; y < surface.height(); ++y) {
        for (LONG x = 0; x < surface.width(); ++x) {
            if (surface.pixel(x, y) != 0) {
                firstInk = std::min(firstInk, y);
                lastInk = std::max(lastInk, y);
            }
        }
    }
    expect(firstInk > compact.top && lastInk < compact.bottom - 1,
           "title text must leave headroom and descender room inside its line box");

    expect(painter.begin(surface.dc(), surface.bounds()), "roomy text begin should succeed");
    painter.clear(RGB(0, 0, 0));
    painter.text(value, roomy, 32.0f * scale, true, RGB(255, 255, 255),
                 TextAlign::Left, false, false, false, chinese);
    expect(painter.end(), "roomy text end should succeed");
    GdiFlush();
    expect(compactPixels == surface.rgbPixels(),
           "compact title box must render exactly the same glyphs as an unclipped tall box");

    expect(painter.begin(surface.dc(), surface.bounds()), "aligned text begin should succeed");
    painter.clear(RGB(0, 0, 0));
    painter.text(chinese ? L"继续" : L"Continue", compact, 16.0f * scale,
                 true, RGB(255, 255, 255), TextAlign::Center, true,
                 false, false, chinese);
    expect(painter.end(), "aligned text end should succeed");
    GdiFlush();
    LONG left = surface.width();
    LONG right = -1;
    LONG top = surface.height();
    LONG bottom = -1;
    for (LONG y = 0; y < surface.height(); ++y) {
        for (LONG x = 0; x < surface.width(); ++x) {
            if (surface.pixel(x, y)) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
    }
    expect(left > compact.left && right < compact.right && top > compact.top
               && bottom < compact.bottom,
           "button caption must remain completely within its box");
    expect(std::abs((left + right) - (compact.left + compact.right)) < 8 * scale,
           "center-aligned button caption should stay visually centered");
}

void testFolderIconAtScale(float scale) {
    Surface surface(static_cast<int>(48 * scale), static_cast<int>(40 * scale));
    InstallerPainter painter;
    // The real installer uses a 20-DIP icon inside a 36-DIP button. Also exercise
    // a wider box so its position is not accidentally tied to square controls.
    for (const RECT bounds : {scaledRect(scale, 14, 10, 34, 30),
                              scaledRect(scale, 10, 10, 38, 30)}) {
        expect(painter.begin(surface.dc(), surface.bounds()),
               "folder icon begin should succeed");
        painter.clear(RGB(0, 0, 0));
        painter.folderIcon(bounds, RGB(255, 255, 255), 1.5f * scale);
        expect(painter.end(), "folder icon end should succeed");
        GdiFlush();

        LONG left = surface.width();
        LONG right = -1;
        LONG top = surface.height();
        LONG bottom = -1;
        unsigned litPixels = 0;
        bool outsideBounds = false;
        for (LONG y = 0; y < surface.height(); ++y) {
            for (LONG x = 0; x < surface.width(); ++x) {
                if (surface.pixel(x, y) == 0) {
                    continue;
                }
                ++litPixels;
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
                outsideBounds |= x < bounds.left || x >= bounds.right
                    || y < bounds.top || y >= bounds.bottom;
            }
        }
        expect(litPixels > 50, "folder outline must produce visible pixels");
        expect(!outsideBounds, "folder stroke must never paint outside its bounds");
        expect(left > bounds.left && right < bounds.right - 1
                   && top > bounds.top && bottom < bounds.bottom - 1,
               "folder stroke must retain antialiasing padding on every side");
        expect(std::abs((left + right + 1) - (bounds.left + bounds.right)) <= 1
                   && std::abs((top + bottom + 1) - (bounds.top + bounds.bottom)) <= 1,
               "folder outline bounds must stay visually centered at every DPI");
        expect(intermediatePixels(surface, bounds) > 25,
               "folder curves and diagonal tab must have antialiased edge pixels");
        expectGrayscale(surface);
        expect(surface.pixel((bounds.left + bounds.right) / 2,
                             (bounds.top + bounds.bottom) / 2) == 0,
               "folder icon must remain a light outline, not a solid glyph");
    }
}

void testProgressFramesAtScale(float scale) {
    using yanami::installer::ui::indeterminateProgressSegments;
    using yanami::installer::ui::kProgressCycleMilliseconds;
    using yanami::installer::ui::PixelRect;
    constexpr COLORREF trackColor = RGB(0x27, 0x2b, 0x35);
    constexpr COLORREF segmentColor = RGB(0xff, 0x66, 0x87);
    constexpr COLORREF outsideColor = RGB(0x0d, 0x3b, 0x25);
    constexpr std::uint32_t trackPixel = 0x272b35;
    constexpr std::uint32_t segmentPixel = 0xff6687;
    constexpr std::uint32_t outsidePixel = 0x0d3b25;

    Surface surface(static_cast<int>(296 * scale), static_cast<int>(22 * scale));
    InstallerPainter painter;
    // The production 280 x 6 DIP track, translated into a compact surface so
    // two complete cycles can be checked pixel-by-pixel without a real window.
    const RECT track = scaledRect(scale, 8, 8, 288, 14);
    const LONG trackWidth = track.right - track.left;
    const LONG trackHeight = track.bottom - track.top;
    const float expectedWidth = static_cast<float>(trackWidth) * 2.0f / 7.0f;
    const float tolerance = 0.001f * scale;
    const auto sameRect = [tolerance](const PixelRect& left, const PixelRect& right) {
        return std::abs(left.left - right.left) <= tolerance
            && std::abs(left.top - right.top) <= tolerance
            && std::abs(left.right - right.right) <= tolerance
            && std::abs(left.bottom - right.bottom) <= tolerance;
    };
    expect(kProgressCycleMilliseconds == 2400,
           "progress cycle should use the declared 2400ms duration");
    const auto start = indeterminateProgressSegments(track, 0);
    const auto afterOneMillisecond = indeterminateProgressSegments(track, 1);
    const auto nextCycle = indeterminateProgressSegments(track, kProgressCycleMilliseconds);
    expect(start[0].left == track.left,
           "the right-moving progress segment should start at the track's left edge");
    expect(sameRect(start[0], nextCycle[0]) && sameRect(start[1], nextCycle[1]),
           "a complete cycle must join its initial state without an off-track reset");
    const float subpixelStep = afterOneMillisecond[0].left - start[0].left;
    expect(subpixelStep > 0.0f && subpixelStep < 1.0f,
           "a one-millisecond time change must retain a positive subpixel displacement");
    for (unsigned quarter = 1; quarter < 4; ++quarter) {
        const auto segments = indeterminateProgressSegments(track, kProgressCycleMilliseconds * quarter / 4);
        expect(std::abs(segments[0].left - (track.left + trackWidth * quarter / 4.0f)) <= tolerance,
               "progress motion must remain linear and rightward throughout every quarter of the cycle");
    }

    std::vector<std::uint64_t> times;
    for (std::uint64_t elapsed = 0; elapsed <= kProgressCycleMilliseconds * 2; elapsed += 16) {
        times.push_back(elapsed);
    }
    // Explicitly inspect either side of the middle and cycle boundaries, not
    // only regular frame times that can accidentally miss a reversal or gap.
    for (const auto boundary : {kProgressCycleMilliseconds / 2, kProgressCycleMilliseconds,
                                kProgressCycleMilliseconds * 3 / 2, kProgressCycleMilliseconds * 2}) {
        times.push_back(boundary - 1);
        times.push_back(boundary);
        if (boundary < kProgressCycleMilliseconds * 2) {
            times.push_back(boundary + 1);
        }
    }
    for (std::uint64_t cycle = 0; cycle < 2; ++cycle) {
        const auto split = cycle * kProgressCycleMilliseconds
            + kProgressCycleMilliseconds * 5 / 7;
        times.push_back(split - 1);
        times.push_back(split);
        times.push_back(split + 1);
    }
    times.push_back(1);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());

    Surface clipEnvelope(surface.width(), surface.height());
    expect(painter.begin(clipEnvelope.dc(), clipEnvelope.bounds()),
           "rounded progress envelope begin should succeed");
    painter.clear(outsideColor);
    painter.roundedRect(track, 3.0f * scale, trackColor, trackColor, scale);
    painter.pushRoundedClip(track, 3.0f * scale);
    painter.fillRect(clipEnvelope.bounds(), segmentColor);
    painter.popClip();
    expect(painter.end(), "rounded progress envelope end should succeed");
    GdiFlush();
    const auto envelopePixels = clipEnvelope.rgbPixels();
    for (const POINT corner : {POINT{track.left, track.top}, POINT{track.right - 1, track.top},
                               POINT{track.left, track.bottom - 1}, POINT{track.right - 1, track.bottom - 1}}) {
        expect(clipEnvelope.pixel(corner.x, corner.y) != segmentPixel,
               "rounded track corners must never become fully opaque square corners");
    }

    expect(painter.begin(surface.dc(), surface.bounds()), "progress fixture begin should succeed");
    painter.clear(outsideColor);
    expect(painter.end(), "progress fixture end should succeed");
    GdiFlush();

    auto previous = start;
    std::uint64_t previousTime = 0;
    std::vector<std::uint32_t> firstFramePixels;
    for (const auto elapsed : times) {
        const auto segments = indeterminateProgressSegments(track, elapsed);
        const auto laterCycle = indeterminateProgressSegments(track, elapsed + kProgressCycleMilliseconds * 13);
        float visibleWidth = 0;
        for (size_t index = 0; index < segments.size(); ++index) {
            const auto& segment = segments[index];
            expect(std::abs((segment.right - segment.left) - expectedWidth) <= tolerance && expectedWidth > 0,
                   "both progress copies must retain the same nonempty segment width");
            expect(segment.top == track.top && segment.bottom == track.bottom,
                   "both progress copies must retain the track's vertical bounds");
            expect(sameRect(segment, laterCycle[index]),
                   "progress position must depend on elapsed time modulo the cycle, not invocation count");
            visibleWidth += std::max(0.0f,
                std::min(segment.right, static_cast<float>(track.right))
                    - std::max(segment.left, static_cast<float>(track.left)));
        }
        expect(std::abs(visibleWidth - expectedWidth) <= tolerance,
               "the clipped progress copies must join into one constant visible width, including wrap frames");
        expect(std::abs((segments[0].left - segments[1].left) - trackWidth) <= tolerance,
               "the entering copy must be exactly one track width behind the exiting copy");
        float displacement = segments[0].left - previous[0].left;
        if (displacement < 0) {
            displacement += trackWidth;
        }
        const float expectedStep = static_cast<float>(trackWidth)
            * static_cast<float>(elapsed - previousTime)
            / static_cast<float>(kProgressCycleMilliseconds);
        expect(std::abs(displacement - expectedStep) <= tolerance,
               "every frame must advance rightward by elapsed time, including across the cycle seam");

        expect(painter.begin(surface.dc(), surface.bounds()), "progress frame begin should succeed");
        // Reset the backing pixels so the 1ms comparison cannot be satisfied
        // by accumulating antialiasing over an otherwise stationary segment.
        painter.clear(outsideColor);
        painter.roundedRect(track, 3.0f * scale, trackColor, trackColor, scale);
        painter.pushRoundedClip(track, 3.0f * scale);
        for (const auto& segment : segments) {
            painter.roundedRectSubpixel(segment, 3.0f * scale, segmentColor, segmentColor, scale);
        }
        painter.popClip();
        expect(painter.end(), "progress frame end should succeed");
        GdiFlush();

        unsigned grayPixels = 0;
        unsigned accentPixels = 0;
        bool outsideUnchanged = true;
        bool roundedOutlinePreserved = true;
        for (int y = 0; y < surface.height(); ++y) {
            for (int x = 0; x < surface.width(); ++x) {
                const auto pixel = surface.pixel(x, y);
                const auto envelopePixel = envelopePixels[y * surface.width() + x];
                // Pink has the largest red channel used in the track. A solid
                // pink fill through the rounded mask is therefore an upper
                // bound on coverage, including antialiased endpoint corners.
                roundedOutlinePreserved &= ((pixel >> 16) & 0xff)
                    <= ((envelopePixel >> 16) & 0xff) + 1;
                if (envelopePixel == outsidePixel) {
                    roundedOutlinePreserved &= pixel == outsidePixel;
                }
                if (x < track.left || x >= track.right || y < track.top || y >= track.bottom) {
                    outsideUnchanged &= pixel == outsidePixel;
                } else {
                    grayPixels += pixel == trackPixel;
                    accentPixels += pixel == segmentPixel;
                }
            }
        }
        expect(grayPixels >= static_cast<unsigned>((trackWidth - expectedWidth) * trackHeight / 2),
               "every painted frame must retain a visibly nonempty gray track");
        expect(accentPixels >= static_cast<unsigned>(expectedWidth * trackHeight / 2),
               "the painted progress segment must never vanish at a split or cycle boundary");
        expect(outsideUnchanged, "progress drawing must never alter pixels outside its clip");
        expect(roundedOutlinePreserved,
               "wrapped progress segments must preserve the rounded track outline at every frame");
        if (elapsed == 0) {
            firstFramePixels = surface.rgbPixels();
        } else if (elapsed == 1) {
            expect(firstFramePixels != surface.rgbPixels(),
                   "actual progress drawing must retain 1ms subpixel movement instead of rounding it away");
        } else if (elapsed % kProgressCycleMilliseconds == 0) {
            expect(firstFramePixels == surface.rgbPixels(),
                   "painted cycles must close seamlessly onto the same initial pixels");
        }
        previous = segments;
        previousTime = elapsed;
    }
}

void testRoundedClipLifetime() {
    Surface surface(160, 100);
    InstallerPainter painter;
    const RECT rounded{20, 20, 80, 80};

    expect(painter.begin(surface.dc(), surface.bounds()), "mixed clip begin should succeed");
    painter.clear(RGB(0, 0, 0));
    painter.pushClip({10, 10, 100, 90});
    painter.pushRoundedClip(rounded, 16.0f);
    painter.fillRect(surface.bounds(), RGB(255, 255, 255));
    painter.popClip();
    painter.fillRect({84, 40, 90, 46}, RGB(255, 0, 0));
    painter.popClip();
    painter.fillRect({120, 40, 128, 48}, RGB(0, 255, 0));
    expect(painter.end(), "mixed clip end should succeed");
    GdiFlush();
    expect(surface.pixel(40, 40) == 0xffffff && surface.pixel(20, 20) == 0,
           "rounded clip must keep its center and exclude its corner");
    expect(surface.pixel(86, 42) == 0xff0000 && surface.pixel(124, 44) == 0x00ff00,
           "popping rounded then axis clips must restore their parent regions in order");

    expect(painter.begin(surface.dc(), surface.bounds()), "reverse nested clip begin should succeed");
    painter.clear(RGB(0, 0, 0));
    painter.pushRoundedClip(rounded, 16.0f);
    painter.pushClip({30, 30, 60, 60});
    painter.fillRect(surface.bounds(), RGB(255, 255, 255));
    painter.popClip();
    painter.fillRect({64, 40, 70, 46}, RGB(255, 0, 0));
    painter.popClip();
    painter.fillRect({120, 40, 128, 48}, RGB(0, 255, 0));
    expect(painter.end(), "reverse nested clip end should succeed");
    GdiFlush();
    expect(surface.pixel(40, 40) == 0xffffff && surface.pixel(20, 20) == 0
               && surface.pixel(66, 42) == 0xff0000 && surface.pixel(124, 44) == 0x00ff00,
           "popping axis then rounded clips must also restore their parent regions in order");

    expect(painter.begin(surface.dc(), surface.bounds()), "same-key nested clip begin should succeed");
    painter.clear(RGB(0, 0, 0));
    painter.pushRoundedClip(rounded, 16.0f);
    painter.pushRoundedClip(rounded, 16.0f);
    painter.pushClip({35, 35, 45, 45});
    painter.fillRect(surface.bounds(), RGB(255, 255, 255));
    // Both rounded masks have the same cache key, but an active Direct2D layer
    // cannot be pushed a second time. End must release all three stack entries.
    expect(painter.end(), "end must balance nested same-key rounded and axis clips");
    GdiFlush();
    expect(surface.pixel(40, 40) == 0xffffff && surface.pixel(34, 40) == 0,
           "same-key rounded masks must preserve their inner axis clip");
    expect(painter.begin(surface.dc(), surface.bounds()), "frame after automatic clip balance should begin");
    painter.clear(RGB(0, 0, 0));
    painter.fillRect({120, 40, 128, 48}, RGB(0, 255, 0));
    expect(painter.end(), "frame after automatic clip balance should finish");
    GdiFlush();
    expect(surface.pixel(124, 44) == 0x00ff00,
           "automatically balanced clips must never leak into the next frame");

    expect(painter.begin(surface.dc(), {60, 20, 120, 80}), "non-origin rounded clip begin should succeed");
    painter.clear(RGB(0, 0, 0));
    painter.pushRoundedClip({68, 28, 108, 68}, 12.0f);
    painter.fillRect({60, 20, 120, 80}, RGB(255, 255, 255));
    expect(painter.end(), "non-origin rounded clip end should succeed");
    GdiFlush();
    expect(surface.pixel(88, 48) == 0xffffff && surface.pixel(68, 28) == 0
               && surface.pixel(67, 48) == 0 && surface.pixel(108, 48) == 0,
           "rounded masks must preserve HDC coordinates when bound to a non-origin rectangle");

    for (const float invalidRadius : {std::numeric_limits<float>::quiet_NaN(),
                                      std::numeric_limits<float>::infinity()}) {
        expect(painter.begin(surface.dc(), surface.bounds()), "invalid rounded radius test should begin");
        painter.pushClip({10, 10, 100, 90});
        painter.pushRoundedClip(rounded, 16.0f);
        painter.pushRoundedClip(rounded, invalidRadius);
        expect(!painter.end(), "nonfinite rounded radius must fail and balance existing clips");
        expect(FAILED(painter.lastError()), "invalid rounded clip must expose a failure");
        expect(painter.begin(surface.dc(), surface.bounds()), "rounded clip should recover on the next frame");
        painter.clear(RGB(0, 0, 0));
        painter.pushRoundedClip(rounded, 16.0f);
        painter.fillRect(surface.bounds(), RGB(255, 255, 255));
        expect(painter.end(), "cached geometry must work with a recreated target and layer");
        GdiFlush();
        expect(surface.pixel(40, 40) == 0xffffff && surface.pixel(20, 20) == 0,
               "recreated rounded clipping must preserve its original shape");
    }
}

void testInstalledFonts() {
    InstallerPainter painter;
    const auto chinese = painter.fontFamily(true);
    const auto latin = painter.fontFamily(false);
    expect(!chinese.empty() && !latin.empty(), "both languages need actual installed font families");
    ComValue<IDWriteFactory> factory;
    expect(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&factory.value))),
        "DirectWrite factory should be available");
    ComValue<IDWriteFontCollection> collection;
    const HRESULT collectionResult = factory->GetSystemFontCollection(&collection.value, FALSE);
    expect(SUCCEEDED(collectionResult), "system font collection should be available");
    bool allPresent = true;
    for (const auto& family : {chinese, latin}) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        allPresent = allPresent
            && SUCCEEDED(collection->FindFamilyName(family.c_str(), &index, &exists))
            && exists;
    }
    expect(allPresent, "font resolver must not return invented or substituted family names");

    ComValue<IDWriteTextFormat> format;
    expect(SUCCEEDED(factory->CreateTextFormat(chinese.c_str(), collection.value,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"zh-CN", &format.value)),
        "Chinese text format must be available");
    constexpr std::wstring_view characters = L"选择安装方式继续桌面快捷卸载";
    ComValue<IDWriteTextLayout> layout;
    expect(SUCCEEDED(factory->CreateTextLayout(characters.data(),
        static_cast<UINT32>(characters.size()), format.value, 1000, 100,
        &layout.value)), "Chinese glyph-probe layout must be available");
    ComValue<GlyphProbe> probe;
    probe.value = new GlyphProbe;
    expect(SUCCEEDED(layout->Draw(nullptr, probe.value, 0, 0)),
           "DirectWrite must produce its actual Chinese glyph runs");
    expect(probe->glyphCount >= characters.size() && probe->missingGlyphCount == 0,
           "every Chinese installer character must have a real glyph, never tofu");
    std::wcout << L"Chinese family: " << chinese << L"; Latin family: " << latin << L'\n';
}

void testLifetimeAndClip() {
    Surface surface(160, 100);
    InstallerPainter painter;
    expect(!painter.begin(nullptr, surface.bounds()), "null HDC must fail safely");
    expect(FAILED(painter.lastError()), "invalid begin should expose an error");
    expect(painter.begin(surface.dc(), surface.bounds()), "next valid frame should recover");
    painter.clear(RGB(0, 0, 0));
    painter.pushClip({20, 20, 60, 60});
    painter.fillRect(surface.bounds(), RGB(255, 255, 255));
    expect(painter.end(), "end should safely balance forgotten clips");
    GdiFlush();
    expect(surface.pixel(19, 20) == 0 && surface.pixel(20, 20) == 0xffffff
               && surface.pixel(59, 59) == 0xffffff && surface.pixel(60, 59) == 0,
           "clip must be exact in physical pixel coordinates");
    expect(painter.begin(surface.dc(), {60, 20, 120, 80}),
           "reusable target should bind a non-origin subrectangle");
    painter.clear(RGB(0, 0, 0));
    painter.fillRect({70, 30, 80, 40}, RGB(255, 255, 255));
    expect(painter.end(), "subrectangle end should succeed");
    GdiFlush();
    expect(surface.pixel(70, 30) == 0xffffff && surface.pixel(69, 30) == 0
               && surface.pixel(80, 30) == 0,
           "non-origin BindDC must preserve HDC pixel coordinates");
    expect(painter.begin(surface.dc(), surface.bounds()), "frame should begin before invalid draw");
    painter.line(0, 0, 20, 20, RGB(0, 0, 0), -1.0f);
    expect(!painter.end(), "invalid draw parameter must propagate through end");
    expect(painter.begin(surface.dc(), surface.bounds()), "next frame should recover from invalid draw");
    painter.clear(RGB(0, 0, 0));
    expect(painter.end(), "recovered frame should finish successfully");
}

} // namespace

int main() {
    try {
        testInstalledFonts();
        testLifetimeAndClip();
        testRoundedClipLifetime();
        for (float scale : {1.0f, 1.5f, 2.0f}) {
            testGeometryAtScale(scale);
            testFolderIconAtScale(scale);
            testProgressFramesAtScale(scale);
            testTextAtScale(scale, true);
            testTextAtScale(scale, false);
        }
        std::cout << "Installer painter: " << assertions
                  << " assertions passed at 100%, 150%, and 200% scaling.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Installer painter test failed: " << error.what() << '\n';
        return 1;
    }
}
