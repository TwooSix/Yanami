import QtQuick
import QtQuick.Controls.Basic
import Yanami

Flickable {
    id: root

    property real lastWheelTime: 0
    property real wheelBoost: 1
    readonly property bool canScrollVertically: contentHeight > height + 1

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
            const rawDelta = event.pixelDelta.y !== 0
                ? event.pixelDelta.y
                : event.angleDelta.y * 1.7
            const currentTarget = wheelAnimation.running ? wheelAnimation.to : root.contentY
            const maximum = Math.max(0, root.contentHeight - root.height)
            wheelAnimation.to = Math.max(0, Math.min(maximum,
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
