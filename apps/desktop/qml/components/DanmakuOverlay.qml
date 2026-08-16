import QtQuick

Item {
    id: root

    property var comments: []
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

    property var timeline: []
    property int nextTimelineIndex: 0
    property real anchorMediaTime: 0
    property double anchorWallTime: 0
    property double lastModelSyncTime: 0
    property real lastModelMediaTime: -1
    property var blockedTermList: []

    visible: danmakuEnabled && comments.length > 0
        && (showScroll || showTop || showBottom)
    opacity: commentOpacity
    clip: true

    function normalizedMode(value) {
        const mode = String(value || "scroll").toLowerCase()
        return mode === "top" || mode === "bottom" ? mode : "scroll"
    }

    function estimatedWidth(text) {
        let units = 0
        const value = String(text || "")
        for (let index = 0; index < value.length; ++index)
            units += value.charCodeAt(index) > 255 ? 1.0 : 0.56
        return Math.max(fontSize, units * fontSize)
    }

    function colorString(rgb) {
        const value = Math.max(0, Math.min(0xffffff, Number(rgb || 0xffffff)))
        return "#" + ("000000" + Math.round(value).toString(16)).slice(-6)
    }

    function prepareTimeline() {
        const source = comments || []
        const ordered = []
        for (let index = 0; index < source.length; ++index) {
            const comment = source[index] || ({})
            const text = String(comment.text || "").trim()
            if (text.length === 0)
                continue
            ordered.push({
                "commentId": String(comment.id !== undefined ? comment.id : index),
                "start": Math.max(0, Number(comment.time || 0)),
                "mode": normalizedMode(comment.mode),
                "textColor": colorString(comment.color),
                "commentText": text,
                "lane": 0,
                "densityRank": 0
            })
        }
        ordered.sort((left, right) => left.start - right.start)

        // Layout is calculated once per loaded timeline. Runtime style changes
        // only alter material/geometry bindings; they never rebuild an mpv
        // subtitle track or touch the video decoder.
        const maximumLanes = 64
        const scrollAvailable = new Array(maximumLanes).fill(0)
        const topAvailable = new Array(maximumLanes).fill(0)
        const bottomAvailable = new Array(maximumLanes).fill(0)
        const densitySeconds = ({ "scroll": -1, "top": -1, "bottom": -1 })
        const densityRanks = ({ "scroll": 0, "top": 0, "bottom": 0 })
        for (let index = 0; index < ordered.length; ++index) {
            const entry = ordered[index]
            const second = Math.floor(entry.start)
            if (second !== densitySeconds[entry.mode]) {
                densitySeconds[entry.mode] = second
                densityRanks[entry.mode] = 0
            }
            entry.densityRank = densityRanks[entry.mode]++
            const lanes = entry.mode === "top" ? topAvailable
                        : entry.mode === "bottom" ? bottomAvailable
                        : scrollAvailable
            let lane = -1
            let earliestLane = 0
            for (let candidate = 0; candidate < lanes.length; ++candidate) {
                if (lanes[candidate] < lanes[earliestLane])
                    earliestLane = candidate
                if (lane < 0 && lanes[candidate] <= entry.start)
                    lane = candidate
            }
            if (lane < 0)
                lane = earliestLane
            entry.lane = lane
            if (entry.mode === "scroll") {
                const speed = Math.max(1, width) / Math.max(3, scrollDuration)
                lanes[lane] = entry.start + estimatedWidth(entry.commentText) / speed + 0.12
            } else {
                lanes[lane] = entry.start + 4.5
            }
        }
        timeline = ordered
        reanchor(mediaPosition)
        rebuildActive(renderTime)
    }

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
        return mode === "top" ? showTop : (mode === "bottom" ? showBottom : showScroll)
    }

    function appendActive(entry) {
        if (activeComments.count >= 360)
            return
        activeComments.append(entry)
    }

    function rebuildActive(position) {
        activeComments.clear()
        const lookBack = Math.max(20, scrollDuration + 1)
        let low = 0
        let high = timeline.length
        const threshold = position - timeOffset - lookBack
        while (low < high) {
            const middle = Math.floor((low + high) / 2)
            if (timeline[middle].start < threshold)
                low = middle + 1
            else
                high = middle
        }
        nextTimelineIndex = low
        while (nextTimelineIndex < timeline.length
               && timeline[nextTimelineIndex].start + timeOffset <= position + 0.12) {
            const entry = timeline[nextTimelineIndex]
            if (modeEnabled(entry.mode)
                    && entry.start + timeOffset + durationFor(entry.mode) >= position - 0.12)
                appendActive(entry)
            ++nextTimelineIndex
        }
        lastModelMediaTime = position
        lastModelSyncTime = Date.now()
    }

    function syncActive(position, force) {
        const now = Date.now()
        if (force || lastModelMediaTime < 0
                || position < lastModelMediaTime - 0.2
                || position > lastModelMediaTime + 1.2) {
            rebuildActive(position)
            return
        }
        if (now - lastModelSyncTime < 70)
            return
        lastModelSyncTime = now
        lastModelMediaTime = position
        for (let index = activeComments.count - 1; index >= 0; --index) {
            const entry = activeComments.get(index)
            if (entry.start + timeOffset + durationFor(entry.mode) < position - 0.12)
                activeComments.remove(index)
        }
        while (nextTimelineIndex < timeline.length
               && timeline[nextTimelineIndex].start + timeOffset <= position + 0.12) {
            if (modeEnabled(timeline[nextTimelineIndex].mode))
                appendActive(timeline[nextTimelineIndex])
            ++nextTimelineIndex
        }
    }

    function refreshBlockedTerms() {
        blockedTermList = String(blockedTerms || "")
            .split(/[,\n]/)
            .map(term => term.trim())
            .filter(term => term.length > 0)
    }

    function isBlocked(text) {
        for (let index = 0; index < blockedTermList.length; ++index) {
            if (String(text).indexOf(blockedTermList[index]) >= 0)
                return true
        }
        return false
    }

    onCommentsChanged: prepareTimeline()
    onWidthChanged: {
        if (width > 0 && timeline.length > 0)
            prepareTimeline()
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
    onScrollDurationChanged: {
        if (timeline.length > 0)
            rebuildActive(renderTime)
    }
    onTimeOffsetChanged: rebuildActive(renderTime)
    onBlockedTermsChanged: refreshBlockedTerms()
    onShowScrollChanged: rebuildActive(renderTime)
    onShowTopChanged: rebuildActive(renderTime)
    onShowBottomChanged: rebuildActive(renderTime)
    Component.onCompleted: refreshBlockedTerms()

    ListModel { id: activeComments }

    FrameAnimation {
        running: root.visible
        onTriggered: {
            const now = Date.now()
            root.renderTime = root.predictedTime(now)
            root.syncActive(root.renderTime, false)
        }
    }

    Repeater {
        model: activeComments
        delegate: Text {
            required property string commentId
            required property real start
            required property string mode
            required property color textColor
            required property string commentText
            required property int lane
            required property int densityRank

            readonly property real effectiveStart: start + root.timeOffset
            readonly property real life: root.renderTime - effectiveStart
            readonly property real duration: root.durationFor(mode)
            readonly property real effectiveTopMargin: Math.max(0, Math.min(
                root.topMargin, root.height * root.displayArea - root.fontSize - 8))
            readonly property int laneCount: Math.max(1, Math.floor(
                (root.height * root.displayArea - effectiveTopMargin)
                / Math.max(1, root.fontSize + 8)))
            readonly property real progress: Math.max(0, Math.min(1, life / duration))

            visible: root.modeEnabled(mode) && life >= 0 && life <= duration
                && lane < laneCount
                && densityRank < root.density
                && !root.isBlocked(commentText)
            z: 1
            x: mode === "scroll"
                ? root.width - progress * (root.width + implicitWidth)
                : (root.width - implicitWidth) / 2
            y: mode === "bottom"
                ? root.height * root.displayArea - (lane + 1) * (root.fontSize + 8)
                : effectiveTopMargin + lane * (root.fontSize + 8)
            font.family: Theme.fontForText(text)
            font.pixelSize: root.fontSize
            font.weight: Font.DemiBold
            style: Text.Outline
            styleColor: "#B0000000"
            renderType: Text.QtRendering
            text: commentText
            color: textColor
        }
    }
}
