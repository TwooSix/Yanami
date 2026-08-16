#include "DevelopmentHooks.hpp"

#include <QtGlobal>

namespace DevelopmentHooks {
namespace {

#ifdef YANAMI_ENABLE_DEV_HOOKS
const char *environmentName(Variable variable)
{
    switch (variable) {
    case Variable::AutoSkipIntro:
        return "YANAMI_DEV_AUTO_SKIP_INTRO";
    case Variable::AutoplayContextJson:
        return "YANAMI_DEV_AUTOPLAY_CONTEXT_JSON";
    case Variable::AutoplayFirst:
        return "YANAMI_DEV_AUTOPLAY_FIRST";
    case Variable::AutoplayItemId:
        return "YANAMI_DEV_AUTOPLAY_ITEM_ID";
    case Variable::AutoplayRecentTitle:
        return "YANAMI_DEV_AUTOPLAY_RECENT_TITLE";
    case Variable::AutoplayResumeFirst:
        return "YANAMI_DEV_AUTOPLAY_RESUME_FIRST";
    case Variable::CollectionDelayMs:
        return "YANAMI_DEV_COLLECTION_DELAY_MS";
    case Variable::CollectionSequence:
        return "YANAMI_DEV_COLLECTION_SEQUENCE";
    case Variable::DanmakuPreviewFontSize:
        return "YANAMI_DEV_DANMAKU_PREVIEW_FONT_SIZE";
    case Variable::DanmakuStyleStressCount:
        return "YANAMI_DEV_DANMAKU_STYLE_STRESS_COUNT";
    case Variable::DanmakuSearchQuery:
        return "YANAMI_DEV_DANMAKU_SEARCH_QUERY";
    case Variable::DanmakuToggleStressCount:
        return "YANAMI_DEV_DANMAKU_TOGGLE_STRESS_COUNT";
    case Variable::DisableDanmakuAfterLoad:
        return "YANAMI_DEV_DISABLE_DANMAKU_AFTER_LOAD";
    case Variable::FullscreenDiagnostics:
        return "YANAMI_DEV_FULLSCREEN_DIAGNOSTICS";
    case Variable::FullscreenGeometry:
        return "YANAMI_DEV_FULLSCREEN_GEOMETRY";
    case Variable::LibraryScanProgress:
        return "YANAMI_DEV_LIBRARY_SCAN_PROGRESS";
    case Variable::LocalMedia:
        return "YANAMI_DEV_LOCAL_MEDIA";
    case Variable::LogPath:
        return "YANAMI_DEV_LOG_PATH";
    case Variable::MediaMenuPreview:
        return "YANAMI_DEV_MEDIA_MENU_PREVIEW";
    case Variable::MpvDemuxerMaxBackBytes:
        return "YANAMI_DEV_MPV_DEMUXER_MAX_BACK_BYTES";
    case Variable::MpvDemuxerMaxBytes:
        return "YANAMI_DEV_MPV_DEMUXER_MAX_BYTES";
    case Variable::MpvHwdec:
        return "YANAMI_DEV_MPV_HWDEC";
    case Variable::MpvHwdecExtraFrames:
        return "YANAMI_DEV_MPV_HWDEC_EXTRA_FRAMES";
    case Variable::MpvVideoSync:
        return "YANAMI_DEV_MPV_VIDEO_SYNC";
    case Variable::PlaybackAutostopMs:
        return "YANAMI_DEV_PLAYBACK_AUTOSTOP_MS";
    case Variable::PlaybackSeekSeconds:
        return "YANAMI_DEV_PLAYBACK_SEEK_SECONDS";
    case Variable::ReenableDanmakuAfterDisable:
        return "YANAMI_DEV_REENABLE_DANMAKU_AFTER_DISABLE";
    case Variable::RenderDiagnostics:
        return "YANAMI_DEV_RENDER_DIAGNOSTICS";
    case Variable::ScreenshotDelayMs:
        return "YANAMI_DEV_SCREENSHOT_DELAY_MS";
    case Variable::ScreenshotPage:
        return "YANAMI_DEV_SCREENSHOT_PAGE";
    case Variable::ScreenshotPath:
        return "YANAMI_DEV_SCREENSHOT_PATH";
    case Variable::ScrollRegression:
        return "YANAMI_DEV_SCROLL_REGRESSION";
    case Variable::SearchQuery:
        return "YANAMI_DEV_SEARCH_QUERY";
    case Variable::ShowDanmakuMenu:
        return "YANAMI_DEV_SHOW_DANMAKU_MENU";
    case Variable::ShowLoading:
        return "YANAMI_DEV_SHOW_LOADING";
    case Variable::ShowPlaybackQueue:
        return "YANAMI_DEV_SHOW_PLAYBACK_QUEUE";
    case Variable::SwitchLanguage:
        return "YANAMI_DEV_SWITCH_LANGUAGE";
    case Variable::WindowHeight:
        return "YANAMI_DEV_WINDOW_HEIGHT";
    case Variable::WindowWidth:
        return "YANAMI_DEV_WINDOW_WIDTH";
    }
    return "";
}
#endif

} // namespace

QString value(Variable variable)
{
#ifdef YANAMI_ENABLE_DEV_HOOKS
    return qEnvironmentVariable(environmentName(variable));
#else
    Q_UNUSED(variable);
    return {};
#endif
}

QByteArray bytes(Variable variable)
{
#ifdef YANAMI_ENABLE_DEV_HOOKS
    return qgetenv(environmentName(variable));
#else
    Q_UNUSED(variable);
    return {};
#endif
}

bool isSet(Variable variable)
{
#ifdef YANAMI_ENABLE_DEV_HOOKS
    return qEnvironmentVariableIsSet(environmentName(variable));
#else
    Q_UNUSED(variable);
    return false;
#endif
}

int intValue(Variable variable, bool *ok)
{
#ifdef YANAMI_ENABLE_DEV_HOOKS
    return qEnvironmentVariableIntValue(environmentName(variable), ok);
#else
    Q_UNUSED(variable);
    if (ok)
        *ok = false;
    return 0;
#endif
}

} // namespace DevelopmentHooks
