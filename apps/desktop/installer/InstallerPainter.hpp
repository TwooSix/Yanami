#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "InstallerLayout.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace yanami::installer::ui {

enum class TextAlign { Left, Center, Right };

// Drawing coordinates and font sizes are physical pixels, like the bound HDC.
// The caller performs DPI scaling exactly once and supplies an untransformed
// MM_TEXT HDC. The render target remains at 96 DPI, with DirectWrite font lookup
// independent of GDI font substitution.
class InstallerPainter final {
public:
    InstallerPainter();
    ~InstallerPainter();
    InstallerPainter(const InstallerPainter&) = delete;
    InstallerPainter& operator=(const InstallerPainter&) = delete;

    bool begin(HDC dc, const RECT& pixelBounds);
    bool end();
    void clear(COLORREF color);
    void fillRect(const RECT& bounds, COLORREF color);
    void roundedRect(const RECT& bounds, float radiusPixels, COLORREF fill,
                     COLORREF outline, float strokePixels = 1.0f);
    void roundedRectSubpixel(const PixelRect& bounds, float radiusPixels,
                             COLORREF fill, COLORREF outline,
                             float strokePixels = 1.0f);
    void ellipse(const RECT& bounds, COLORREF color);
    void line(float x1, float y1, float x2, float y2, COLORREF color,
              float widthPixels = 1.0f);
    // A font-independent outline icon, optically centered inside its bounds.
    void folderIcon(const RECT& bounds, COLORREF color,
                    float strokePixels = 1.5f);
    void pushClip(const RECT& bounds);
    // An antialiased mask with cached geometry/layers. Both clip kinds share
    // the same LIFO stack, and end() also balances any remaining clips.
    void pushRoundedClip(const RECT& bounds, float radiusPixels);
    void popClip();
    void text(std::wstring_view value, const RECT& bounds, float fontSizePixels,
              bool semibold, COLORREF color, TextAlign align = TextAlign::Left,
              bool verticallyCentered = false, bool wrap = false,
              bool ellipsis = false, bool chinese = false,
              bool pathEllipsis = false);

    // Resolves actual installed families through DirectWrite, never a guessed
    // GDI face name. Returns an empty string if DirectWrite is unavailable.
    const std::wstring& fontFamily(bool chinese);
    HRESULT lastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace yanami::installer::ui
