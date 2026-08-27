#include "BootstrapWindowsAnimation.hpp"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <string_view>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, std::string_view message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

int main()
{
    using YanamiBootstrap::spinnerAnimationFrameAt;

    const auto initial = spinnerAnimationFrameAt(0ms);
    expect(initial.ordinal == 0, "the initial frame must have ordinal zero");
    expect(initial.startDegrees == 0, "the initial arc must start at zero degrees");

    const auto firstTick = spinnerAnimationFrameAt(16ms);
    const auto secondTick = spinnerAnimationFrameAt(32ms);
    expect(firstTick.ordinal == 1, "the first 16 ms tick must advance the frame");
    expect(secondTick.ordinal == 2, "the second 16 ms tick must advance the frame");
    expect(firstTick.startDegrees == 357,
           "the first tick must move the arc clockwise on screen");
    expect(secondTick.startDegrees == 353,
           "successive ticks must keep rotating clockwise");
    expect((firstTick.startDegrees - secondTick.startDegrees + 360) % 360 > 0,
           "the visible angular delta must remain clockwise");

    const auto wrapped = spinnerAnimationFrameAt(1500ms);
    const auto afterWrap = spinnerAnimationFrameAt(1530ms);
    expect(wrapped.startDegrees == 0,
           "the arc angle must wrap cleanly after one revolution");
    expect(afterWrap.startDegrees == 353,
           "the arc must continue clockwise after wrapping");
    expect(afterWrap.ordinal > wrapped.ordinal,
           "wrapping the angle must not reset the animation counter");

    const auto negative = spinnerAnimationFrameAt(-100ms);
    expect(negative.ordinal == 0 && negative.startDegrees == 0,
           "negative elapsed time must clamp to the initial frame");
    expect(YanamiBootstrap::spinnerSweepDegrees == 82,
           "the indeterminate arc sweep is part of the visual contract");
    expect(YanamiBootstrap::spinnerFrameInterval <= 17ms,
           "the native spinner must target approximately 60 FPS");
    expect(YanamiBootstrap::spinnerSupersample >= 4,
           "the native spinner must retain its anti-aliasing quality floor");

    const auto metrics100 = YanamiBootstrap::splashMetricsForDpi(96);
    const auto metrics150 = YanamiBootstrap::splashMetricsForDpi(144);
    const auto metrics200 = YanamiBootstrap::splashMetricsForDpi(192);
    expect(metrics100.width == 540 && metrics100.height == 320,
           "96-DPI splash geometry must preserve the visual baseline");
    expect(metrics150.width == 810 && metrics150.height == 480,
           "150-percent scaling must apply to the complete splash surface");
    expect(metrics200.iconSize == metrics100.iconSize * 2,
           "the logo must scale with the monitor DPI");
    expect(metrics150.statusFontHeight == 24,
           "status text must use a DPI-scaled physical font height");
    expect(metrics200.spinnerThickness == metrics100.spinnerThickness * 2,
           "the spinner stroke must scale with the monitor DPI");
    expect(YanamiBootstrap::splashMetricsForDpi(0).dpi == 96,
           "an unavailable DPI must fall back to the Windows baseline");
    expect(metrics100.iconY == 38 && metrics100.iconSize == 112,
           "the app icon must preserve its readable launch size");
    expect(metrics100.titleTop == 168 && metrics100.statusTop == 216,
           "the title and status must follow the balanced vertical rhythm");
    expect(metrics100.spinnerTop == 262 && metrics100.spinnerSize == 20,
           "the spinner must remain a small independent progress affordance");

    using YanamiBootstrap::nearestSplashIconPhysicalSize;
    expect(nearestSplashIconPhysicalSize(metrics100.iconSize) == 112,
           "100-percent DPI must select the exact 112-pixel splash frame");
    expect(nearestSplashIconPhysicalSize(metrics150.iconSize) == 168,
           "150-percent DPI must select the exact 168-pixel splash frame");
    expect(nearestSplashIconPhysicalSize(metrics200.iconSize) == 224,
           "200-percent DPI must select the exact 224-pixel splash frame");
    expect(nearestSplashIconPhysicalSize(336) == 256,
           "oversized custom DPI must use the unscaled 256-pixel fallback");

#ifdef YANAMI_SPLASH_ICON_PATH
    std::ifstream icon(YANAMI_SPLASH_ICON_PATH, std::ios::binary);
    expect(icon.good(), "the dedicated splash icon resource must be readable");
    unsigned char header[6] {};
    icon.read(reinterpret_cast<char *>(header), sizeof(header));
    const unsigned int imageCount =
        static_cast<unsigned int>(header[4])
        | (static_cast<unsigned int>(header[5]) << 8);
    expect(header[0] == 0 && header[1] == 0
            && header[2] == 1 && header[3] == 0,
           "the splash resource must be a valid ICO container");
    expect(imageCount == YanamiBootstrap::splashIconPhysicalSizes.size(),
           "the splash ICO must contain every exact physical frame");
    std::set<int> iconSizes;
    for (unsigned int index = 0; index < imageCount; ++index) {
        unsigned char entry[16] {};
        icon.read(reinterpret_cast<char *>(entry), sizeof(entry));
        expect(icon.good(), "the splash ICO directory must not be truncated");
        const int width = entry[0] == 0 ? 256 : entry[0];
        const int height = entry[1] == 0 ? 256 : entry[1];
        expect(width == height, "every splash ICO frame must be square");
        iconSizes.insert(width);
    }
    expect(std::equal(
            iconSizes.begin(), iconSizes.end(),
            YanamiBootstrap::splashIconPhysicalSizes.begin(),
            YanamiBootstrap::splashIconPhysicalSizes.end()),
           "the splash ICO directory must match the DPI frame contract");
#endif

    std::cout << "Windows bootstrap spinner animation logic passed\n";
    return EXIT_SUCCESS;
}
