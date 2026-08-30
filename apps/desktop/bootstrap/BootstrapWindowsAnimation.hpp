#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace YanamiBootstrap {

inline constexpr unsigned int baselineDpi = 96;
inline constexpr std::chrono::milliseconds spinnerFrameInterval {16};
inline constexpr int spinnerDegreesPerSecond = 240;
inline constexpr int spinnerSweepDegrees = 82;
inline constexpr int spinnerSupersample = 4;
inline constexpr std::chrono::milliseconds handoffFadeDuration {220};
inline constexpr std::array<int, 7> splashIconPhysicalSizes {
    112, 140, 168, 196, 224, 252, 256};

struct SplashMetrics {
    unsigned int dpi = baselineDpi;
    int width = 540;
    int height = 320;
    int iconSize = 112;
    int iconY = 38;
    int spinnerSize = 20;
    int spinnerTop = 262;
    int spinnerPaintPadding = 3;
    int cornerRadius = 24;
    int borderThickness = 1;
    int iconCornerDiameter = 40;
    int spinnerThickness = 2;
    int titleFontHeight = 30;
    int statusFontHeight = 16;
    int titleTop = 168;
    int titleBottom = 208;
    int statusTop = 216;
    int statusBottom = 244;
    int horizontalTextMargin = 48;
};

[[nodiscard]] constexpr int scaleForDpi(
    int logicalPixels, unsigned int dpi) noexcept
{
    const auto effectiveDpi = dpi == 0 ? baselineDpi : dpi;
    return static_cast<int>(
        (static_cast<std::int64_t>(logicalPixels) * effectiveDpi
         + baselineDpi / 2)
        / baselineDpi);
}

[[nodiscard]] constexpr SplashMetrics splashMetricsForDpi(
    unsigned int dpi) noexcept
{
    const unsigned int effectiveDpi = dpi == 0 ? baselineDpi : dpi;
    return {
        effectiveDpi,
        scaleForDpi(540, effectiveDpi),
        scaleForDpi(320, effectiveDpi),
        scaleForDpi(112, effectiveDpi),
        scaleForDpi(38, effectiveDpi),
        scaleForDpi(20, effectiveDpi),
        scaleForDpi(262, effectiveDpi),
        scaleForDpi(3, effectiveDpi),
        scaleForDpi(24, effectiveDpi),
        std::max(1, scaleForDpi(1, effectiveDpi)),
        scaleForDpi(40, effectiveDpi),
        std::max(2, scaleForDpi(2, effectiveDpi)),
        scaleForDpi(30, effectiveDpi),
        scaleForDpi(16, effectiveDpi),
        scaleForDpi(168, effectiveDpi),
        scaleForDpi(208, effectiveDpi),
        scaleForDpi(216, effectiveDpi),
        scaleForDpi(244, effectiveDpi),
        scaleForDpi(48, effectiveDpi),
    };
}

struct HandoffFadeFrame {
    std::uint8_t opacity = 255;
    bool complete = false;
};

[[nodiscard]] constexpr HandoffFadeFrame handoffFadeFrameAt(
    std::chrono::milliseconds elapsed) noexcept
{
    const auto elapsedMilliseconds = elapsed.count();
    if (elapsedMilliseconds <= 0)
        return {};

    const auto durationMilliseconds = handoffFadeDuration.count();
    if (elapsedMilliseconds >= durationMilliseconds)
        return {0, true};

    // Match QML's Easing.OutCubic fade. Because opacity moves from one to
    // zero, the remaining opacity is (1 - progress)^3.
    const auto remaining = static_cast<std::uint64_t>(
        durationMilliseconds - elapsedMilliseconds);
    const auto duration = static_cast<std::uint64_t>(durationMilliseconds);
    const auto denominator = duration * duration * duration;
    const auto numerator = 255ULL * remaining * remaining * remaining;
    return {
        static_cast<std::uint8_t>((numerator + denominator / 2) / denominator),
        false,
    };
}

[[nodiscard]] constexpr int nearestSplashIconPhysicalSize(
    int targetSize) noexcept
{
    int nearest = splashIconPhysicalSizes.front();
    int nearestDistance = targetSize >= nearest
        ? targetSize - nearest : nearest - targetSize;
    for (const int candidate : splashIconPhysicalSizes) {
        const int distance = targetSize >= candidate
            ? targetSize - candidate : candidate - targetSize;
        if (distance < nearestDistance) {
            nearest = candidate;
            nearestDistance = distance;
        }
    }
    return nearest;
}

struct SpinnerAnimationFrame {
    std::uint64_t ordinal = 0;
    int startDegrees = 0;
};

[[nodiscard]] constexpr SpinnerAnimationFrame spinnerAnimationFrameAt(
    std::chrono::milliseconds elapsed) noexcept
{
    const auto signedMilliseconds = elapsed.count();
    if (signedMilliseconds <= 0)
        return {};

    const auto milliseconds = static_cast<std::uint64_t>(signedMilliseconds);
    const auto wholeSeconds = milliseconds / 1000;
    const auto remainingMilliseconds = milliseconds % 1000;
    const auto accumulatedDegrees =
        (wholeSeconds % 360) * spinnerDegreesPerSecond
        + remainingMilliseconds * spinnerDegreesPerSecond / 1000;
    const auto counterClockwiseOffset = accumulatedDegrees % 360;
    const auto clockwiseDegrees = counterClockwiseOffset == 0
        ? 0 : 360 - counterClockwiseOffset;
    return {
        milliseconds
            / static_cast<std::uint64_t>(spinnerFrameInterval.count()),
        static_cast<int>(clockwiseDegrees),
    };
}

} // namespace YanamiBootstrap
