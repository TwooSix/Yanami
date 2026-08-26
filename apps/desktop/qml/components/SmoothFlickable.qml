import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

Flickable {
    id: root

    property real lastWheelTime: 0
    property real wheelBoost: 1
    readonly property bool canScrollVertically: contentHeight > height + 1

    function scrollToContentY(value) {
        const maximum = Math.max(0, root.contentHeight - root.height)
        root.cancelFlick()
        root.lastWheelTime = 0
        root.wheelBoost = 1
        wheelAnimation.to = Math.max(0, Math.min(maximum, value))
        wheelAnimation.restart()
    }

    function prepareForFocusReveal() {
        wheelAnimation.stop()
        root.cancelFlick()
        root.lastWheelTime = 0
        root.wheelBoost = 1
    }

    // Focus moves may repeat every 90 ms. A 190 ms wheel animation restarted
    // on every step lets focus outrun the viewport, so focus reveal has a
    // separate synchronous contract. Pointer-wheel scrolling remains smooth.
    function revealContentY(value) {
        const maximum = Math.max(0, root.contentHeight - root.height)
        root.prepareForFocusReveal()
        root.contentY = Math.max(0, Math.min(maximum, value))
    }

    // Controller scroll repeats can arrive before the previous animation
    // finishes. Accumulate from the in-flight destination so no 88 px step is
    // lost merely because contentY has not caught up yet.
    function scrollContentYBy(delta) {
        const maximum = Math.max(0, root.contentHeight - root.height)
        root.cancelFlick()
        const currentTarget = wheelAnimation.running
            ? wheelAnimation.to : root.contentY
        const target = Math.max(0, Math.min(maximum,
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

    ScrollBar.vertical: AppScrollBar {
        id: verticalBar
        policy: root.canScrollVertically ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        visible: root.canScrollVertically
        opacity: visible
            ? (root.moving || root.wheelAnimationRunning || hovered || pressed ? 1 : 0.48)
            : 0

        Behavior on opacity { NumberAnimation { duration: 170 } }
    }

    readonly property bool wheelAnimationRunning: wheelAnimation.running

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
