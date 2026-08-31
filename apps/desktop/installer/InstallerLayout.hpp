#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>

namespace yanami::installer::ui {

// Center the complete native frame within one monitor's working area, not
// the virtual desktop. Preserve negative monitor coordinates. If the fixed
// layout cannot fit, keep its top/left edge accessible without resizing it.
inline POINT centeredWindowPosition(const RECT& workArea, const SIZE& outerSize) noexcept {
    return POINT{
        workArea.left + std::max<LONG>(0, (workArea.right - workArea.left - outerSize.cx) / 2),
        workArea.top + std::max<LONG>(0, (workArea.bottom - workArea.top - outerSize.cy) / 2)};
}

// A native single-line EDIT top-aligns its font cell. Center the control's
// measured font cell itself; EM_SETRECT only applies to multiline edits.
inline RECT centeredTextLineRect(RECT bounds, int measuredLineHeight) {
    const LONG available = std::max<LONG>(0, bounds.bottom - bounds.top);
    const LONG height = std::clamp<LONG>(measuredLineHeight, 0, available);
    bounds.top += (available - height) / 2;
    bounds.bottom = bounds.top + height;
    return bounds;
}

inline constexpr std::uint64_t kProgressCycleMilliseconds = 2400;

struct PixelRect {
    float left;
    float top;
    float right;
    float bottom;
};

// Two copies form one right-moving band across the track's wrap boundary.
// Draw both with the track clip applied: the part leaving the right edge is
// already entering at the left edge, so a cycle never contains an empty frame.
// Preserve subpixels; integer RECT positions make slow motion visibly step.
inline std::array<PixelRect, 2> indeterminateProgressSegments(
    const RECT& track, std::uint64_t elapsedMilliseconds) noexcept {
    const float trackWidth = static_cast<float>(
        std::max<LONG>(0, track.right - track.left));
    const float segmentWidth = trackWidth * 2.0f / 7.0f;
    const float phase = static_cast<float>(
        elapsedMilliseconds % kProgressCycleMilliseconds)
        / static_cast<float>(kProgressCycleMilliseconds);
    const float left = static_cast<float>(track.left) + trackWidth * phase;
    const float top = static_cast<float>(track.top);
    const float bottom = static_cast<float>(track.bottom);
    return {PixelRect{left, top, left + segmentWidth, bottom},
            PixelRect{left - trackWidth, top, left - trackWidth + segmentWidth, bottom}};
}

} // namespace yanami::installer::ui
