import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

GridView {
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
            // A GridView can keep a numeric currentIndex across a proxy-model
            // reset and reveal that stale item during its deferred layout.
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

    function prepareForFocusReveal() {
        wheelAnimation.stop()
        root.cancelFlick()
        root.lastWheelTime = 0
        root.wheelBoost = 1
    }

    function revealContentY(value) {
        const minimum = root.originY
        const maximum = Math.max(minimum,
            minimum + root.contentHeight - root.height)
        root.prepareForFocusReveal()
        root.contentY = Math.max(minimum, Math.min(maximum, value))
    }

    function scrollContentYBy(delta) {
        const minimum = root.originY
        const maximum = Math.max(minimum,
            minimum + root.contentHeight - root.height)
        root.cancelFlick()
        const currentTarget = wheelAnimation.running
            ? wheelAnimation.to : root.contentY
        const target = Math.max(minimum, Math.min(maximum,
            currentTarget + delta))
        if (Math.abs(target - currentTarget) < 0.5)
            return
        wheelAnimation.to = target
        wheelAnimation.restart()
    }

    clip: true
    boundsBehavior: Flickable.StopAtBounds
    flickDeceleration: 1050
    maximumFlickVelocity: 6800

    // Do not animate delegate geometry here. GridView virtualizes delegates and
    // may create/destroy them while a large keyed reorder is still running.
    // Geometry transitions then retain transient x/y/opacity values on pooled
    // delegates, which presents as permanent holes until the next relayout.
    // Card-level state (image, progress, badges) remains animated.

    ScrollBar.vertical: AppScrollBar {
        policy: root.canScrollVertically ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        visible: root.canScrollVertically
        opacity: visible
            ? (root.moving || root.wheelAnimationRunning || hovered || pressed ? 1 : 0.48)
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
            const rawDelta = event.pixelDelta.y !== 0
                ? event.pixelDelta.y
                : event.angleDelta.y * 1.7
            root.scrollContentYBy(-rawDelta * root.wheelBoost)
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
