import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

ListView {
    id: root

    property real lastWheelTime: 0
    property real wheelBoost: 1
    readonly property bool canScrollVertically: contentHeight > height + 1
    readonly property bool wheelAnimationRunning: wheelAnimation.running

    onCountChanged: {
        wheelAnimation.stop()
        if (count === 0)
            positionViewAtBeginning()
        else
            Qt.callLater(root.clampScrollPosition)
    }
    onContentHeightChanged: {
        wheelAnimation.stop()
        Qt.callLater(root.clampScrollPosition)
    }
    onHeightChanged: {
        wheelAnimation.stop()
        Qt.callLater(root.clampScrollPosition)
    }
    onOriginYChanged: {
        wheelAnimation.stop()
        Qt.callLater(root.clampScrollPosition)
    }

    function resetScrollPosition() {
        wheelAnimation.stop()
        root.currentIndex = -1
        root.positionViewAtBeginning()
        Qt.callLater(function() {
            root.currentIndex = -1
            root.positionViewAtBeginning()
            root.clampScrollPosition()
        })
    }

    function clampScrollPosition() {
        const minimum = root.originY
        const maximum = Math.max(minimum,
            minimum + root.contentHeight - root.height)
        if (root.contentY < minimum)
            root.contentY = minimum
        else if (root.contentY > maximum)
            root.contentY = maximum
    }

    function scrollToContentY(value) {
        const minimum = root.originY
        const maximum = Math.max(minimum,
            minimum + root.contentHeight - root.height)
        root.cancelFlick()
        root.lastWheelTime = 0
        root.wheelBoost = 1
        wheelAnimation.to = Math.max(minimum, Math.min(maximum, value))
        wheelAnimation.restart()
    }

    function restoreScrollPosition(value) {
        wheelAnimation.stop()
        root.cancelFlick()
        root.lastWheelTime = 0
        root.wheelBoost = 1
        root.contentY = value
        root.clampScrollPosition()
    }

    clip: true
    boundsBehavior: Flickable.StopAtBounds
    flickDeceleration: 1050
    maximumFlickVelocity: 6800

    // Delegates are pooled. Keep geometry and root opacity deterministic;
    // card-local hover, progress, badge, and image state may still animate.
    reuseItems: true

    ScrollBar.vertical: AppScrollBar {
        policy: root.canScrollVertically
            ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        visible: root.canScrollVertically
        opacity: visible
            ? (root.moving || root.wheelAnimationRunning || hovered || pressed
                ? 1 : 0.48)
            : 0

        Behavior on opacity { NumberAnimation { duration: 170 } }
    }

    WheelHandler {
        target: null
        enabled: root.canScrollVertically
        blocking: true
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            const now = Date.now()
            root.wheelBoost = now - root.lastWheelTime < 150
                ? Math.min(2.25, root.wheelBoost + 0.18)
                : 1
            root.lastWheelTime = now
            let rawDelta = event.pixelDelta.y !== 0
                ? event.pixelDelta.y
                : event.angleDelta.y * 1.7
            if (event.inverted)
                rawDelta = -rawDelta
            const currentTarget = wheelAnimation.running
                ? wheelAnimation.to : root.contentY
            const minimum = root.originY
            const maximum = Math.max(minimum,
                minimum + root.contentHeight - root.height)
            wheelAnimation.to = Math.max(minimum, Math.min(maximum,
                currentTarget - rawDelta * root.wheelBoost))
            wheelAnimation.restart()
            event.accepted = true
        }
    }

    NumberAnimation {
        id: wheelAnimation
        target: root
        property: "contentY"
        duration: 190
        easing.type: Easing.OutCubic
    }
}
