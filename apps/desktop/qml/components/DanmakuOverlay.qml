import QtQuick

Item {
    id: root

    property alias comments: nativeTimelineModel.comments
    property bool danmakuEnabled: true
    property real mediaPosition: 0
    property bool paused: false
    property bool buffering: false
    property real playbackRate: 1
    property real fontSize: 42
    property real commentOpacity: 0.88
    property real scrollDuration: 9
    property real displayArea: 0.70
    property int density: 14
    property real timeOffset: 0
    property string blockedTerms: ""
    property bool showScroll: true
    property bool showTop: true
    property bool showBottom: true
    property real topMargin: 0
    property real renderTime: 0

    // These aliases are intentionally read-only product observability. The
    // performance probe can inspect the native timeline and renderer without
    // walking the full source list or forcing a per-frame QStringList binding.
    readonly property alias timelineModel: nativeTimelineModel
    readonly property alias rendererItem: nativeRenderer
    readonly property alias timelineCount: nativeTimelineModel.timelineCount
    readonly property alias preparing: nativeTimelineModel.preparing
    readonly property alias activeCommentCount: nativeTimelineModel.count

    property real anchorMediaTime: 0
    property double anchorWallTime: 0
    property double lastModelSyncTime: 0
    property real lastModelMediaTime: -1

    readonly property real effectiveTopMargin: Math.max(0, Math.min(
        topMargin, height * displayArea - fontSize - 8))
    readonly property int laneCount: Math.max(1, Math.floor(
        (height * displayArea - effectiveTopMargin)
        / Math.max(1, fontSize + 8)))

    visible: danmakuEnabled && comments.length > 0
        && (showScroll || showTop || showBottom)
    opacity: commentOpacity
    clip: true

    function reanchor(position) {
        anchorMediaTime = Math.max(0, Number(position || 0))
        anchorWallTime = Date.now()
        renderTime = anchorMediaTime
    }

    function predictedTime(now) {
        if (paused || buffering)
            return Math.max(0, mediaPosition)
        return Math.max(0, anchorMediaTime
                        + (now - anchorWallTime) / 1000 * playbackRate)
    }

    function durationFor(mode) {
        return mode === "scroll" ? Math.max(3, scrollDuration) : 4.5
    }

    function modeEnabled(mode) {
        return mode === "top" ? showTop
             : (mode === "bottom" ? showBottom : showScroll)
    }

    function syncActive(position, force) {
        const normalizedPosition = Math.max(0, Number(position || 0))
        const now = Date.now()
        const rebuild = force || lastModelMediaTime < 0
                || normalizedPosition < lastModelMediaTime - 0.2
                || normalizedPosition > lastModelMediaTime + 1.2
        lastModelSyncTime = now
        lastModelMediaTime = normalizedPosition
        // The native model scans only the compact active window. Updating it
        // each frame preserves exact start times without the old JavaScript
        // model's 0.12 s look-ahead allocation.
        nativeTimelineModel.syncActive(normalizedPosition, rebuild)
    }

    onCommentsChanged: {
        reanchor(mediaPosition)
        syncActive(renderTime, true)
    }
    onWidthChanged: {
        if (width > 0 && comments.length > 0) {
            reanchor(mediaPosition)
            syncActive(renderTime, true)
        }
    }
    onMediaPositionChanged: {
        const now = Date.now()
        const prediction = predictedTime(now)
        const error = mediaPosition - prediction
        if (paused || buffering || Math.abs(error) > 0.35) {
            reanchor(mediaPosition)
            syncActive(renderTime, true)
        } else {
            // Gently correct normal mpv clock observations without snapping a
            // moving comment to the source video's frame cadence.
            anchorMediaTime = prediction + Math.max(-0.02, Math.min(0.02, error))
            anchorWallTime = now
        }
    }
    onPausedChanged: {
        reanchor(mediaPosition)
        syncActive(renderTime, true)
    }
    onBufferingChanged: {
        reanchor(mediaPosition)
        syncActive(renderTime, true)
    }
    onDanmakuEnabledChanged: {
        reanchor(mediaPosition)
        syncActive(renderTime, true)
    }
    onScrollDurationChanged: syncActive(renderTime, true)
    onTimeOffsetChanged: syncActive(renderTime, true)
    Component.onCompleted: {
        reanchor(mediaPosition)
        syncActive(renderTime, true)
    }

    DanmakuTimelineModel {
        id: nativeTimelineModel

        viewportWidth: root.width
        fontSize: root.fontSize
        scrollDuration: root.scrollDuration
        timeOffset: root.timeOffset
        blockedTerms: root.blockedTerms
        showScroll: root.showScroll
        showTop: root.showTop
        showBottom: root.showBottom
        density: root.density
        laneCount: root.laneCount
    }

    DanmakuRenderItem {
        id: nativeRenderer
        objectName: "danmakuRenderer"
        anchors.fill: parent
        z: 1
        model: nativeTimelineModel
        renderTime: root.renderTime
        timeOffset: root.timeOffset
        fontSize: root.fontSize
        scrollDuration: root.scrollDuration
        displayArea: root.displayArea
        topMargin: root.topMargin
        fontFamily: Theme.fontFamily
        cjkFontFamily: Theme.cjkFontFamily
    }

    FrameAnimation {
        running: root.visible
        onTriggered: {
            const now = Date.now()
            root.renderTime = root.predictedTime(now)
            root.syncActive(root.renderTime, false)
        }
    }
}
