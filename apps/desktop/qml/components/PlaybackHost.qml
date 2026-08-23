import QtQuick

// Converts a prepared descriptor into player presentation state. Playback
// telemetry itself remains entirely in C++ PlaybackReporter.
Item {
    id: host
    visible: false

    required property var hostWindow

    Connections {
        target: app.playback

        function onReady(descriptor) {
            console.info("playback_ui_ready",
                         "itemId=", descriptor.itemId,
                         "resumeTicks=", descriptor.resumeTicks,
                         "externalSubtitles=", descriptor.externalSubtitles.length,
                         "returnPage=", host.hostWindow.currentPage)
            host.hostWindow.ensurePlayerPage()
            const page = host.hostWindow.playerPage
            if (page.automaticQueueRefreshPending) {
                page.consumeQueueRefresh(descriptor)
                return
            }
            page.preparingPlayback = false
            page.mediaTitle = descriptor.title
            page.previousItem = descriptor.previousItem
            page.nextItem = descriptor.nextItem
            page.playbackContext = descriptor.playbackContext
            page.playbackQueue = descriptor.playbackQueue
            page.currentQueueIndex = descriptor.currentQueueIndex
            page.queueResolutionSucceeded =
                descriptor.queueResolutionSucceeded === true
            page.resumeTicks = host.hostWindow.forcePlaybackFromBeginning
                ? 0 : descriptor.resumeTicks
            host.hostWindow.forcePlaybackFromBeginning = false
            page.introStartSeconds = descriptor.introStartTicks >= 0
                ? descriptor.introStartTicks / 10000000 : -1
            page.introEndSeconds = descriptor.introEndTicks >= 0
                ? descriptor.introEndTicks / 10000000 : -1
            page.currentItemId = descriptor.itemId
            page.danmakuSearchAnime = descriptor.danmakuSearchAnime
            page.danmakuSearchEpisode = descriptor.danmakuSearchEpisode
            page.externalSubtitles = descriptor.externalSubtitles
            page.reportSessionId = descriptor.reportSessionId
            page.embyTracks = (descriptor.audioTracks || [])
                .concat(descriptor.subtitleTracks || [])
            page.requestHeaders = descriptor.headers
            page.mediaUrl = descriptor.mediaUrl
            const warningMessage = host.hostWindow.playbackWarningMessage(
                descriptor.playbackWarnings)
            if (warningMessage.length > 0)
                host.hostWindow.showActionToast(warningMessage, "warning")
            host.hostWindow.currentPage = 2
            console.info("playback_ui_navigated",
                         "itemId=", descriptor.itemId, "page=", 2)
        }

        function onFailed(itemId, message) {
            const page = host.hostWindow.playerPage
            if (!page)
                return
            if (page.automaticQueueRefreshPending)
                page.recoverQueueRefreshFailure(message)
            else if (page.switchingEpisode)
                page.recoverFromPlaybackSwitchFailure(message)
            else if (page.preparingPlayback)
                page.failPreparation(message)
            else
                return
            console.info("playback_ui_failed",
                         "itemId=", itemId,
                         "page=", host.hostWindow.currentPage)
        }
    }
}
