#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>

namespace yanami::installer::ui {

// A native single-line EDIT top-aligns its font cell. Center the control's
// measured font cell itself; EM_SETRECT only applies to multiline edits.
inline RECT centeredTextLineRect(RECT bounds, int measuredLineHeight) {
    const LONG available = std::max<LONG>(0, bounds.bottom - bounds.top);
    const LONG height = std::clamp<LONG>(measuredLineHeight, 0, available);
    bounds.top += (available - height) / 2;
    bounds.bottom = bounds.top + height;
    return bounds;
}

} // namespace yanami::installer::ui
