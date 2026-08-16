#pragma once

#include <QByteArray>
#include <QString>

namespace DevelopmentHooks {

enum class Variable {
    AutoSkipIntro,
    AutoplayContextJson,
    AutoplayFirst,
    AutoplayItemId,
    AutoplayRecentTitle,
    AutoplayResumeFirst,
    CollectionDelayMs,
    CollectionSequence,
    DanmakuPreviewFontSize,
    DanmakuStyleStressCount,
    DanmakuSearchQuery,
    DanmakuToggleStressCount,
    DisableDanmakuAfterLoad,
    FullscreenDiagnostics,
    FullscreenGeometry,
    LibraryScanProgress,
    LocalMedia,
    LogPath,
    MediaMenuPreview,
    MpvDemuxerMaxBackBytes,
    MpvDemuxerMaxBytes,
    MpvHwdec,
    MpvHwdecExtraFrames,
    MpvVideoSync,
    PlaybackAutostopMs,
    PlaybackSeekSeconds,
    ReenableDanmakuAfterDisable,
    RenderDiagnostics,
    ScreenshotDelayMs,
    ScreenshotPage,
    ScreenshotPath,
    ScrollRegression,
    SearchQuery,
    ShowDanmakuMenu,
    ShowLoading,
    ShowPlaybackQueue,
    SwitchLanguage,
    WindowHeight,
    WindowWidth,
};

[[nodiscard]] constexpr bool enabled() noexcept
{
#ifdef YANAMI_ENABLE_DEV_HOOKS
    return true;
#else
    return false;
#endif
}

[[nodiscard]] QString value(Variable variable);
[[nodiscard]] QByteArray bytes(Variable variable);
[[nodiscard]] bool isSet(Variable variable);
[[nodiscard]] int intValue(Variable variable, bool *ok = nullptr);

} // namespace DevelopmentHooks
