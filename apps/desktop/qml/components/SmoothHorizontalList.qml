import QtQuick
import QtQuick.Controls.Basic
import Yanami

ListView {
    id: root

    property real lastWheelTime: 0
    property real wheelBoost: 1
    readonly property bool canScrollHorizontally: contentWidth > width + 1

    orientation: ListView.Horizontal
    spacing: 14
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    flickDeceleration: 1050
    maximumFlickVelocity: 6800

    HoverHandler {
        id: rowHover
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    }

    ScrollBar.horizontal: AppScrollBar {
        id: horizontalBar
        policy: root.canScrollHorizontally ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
        visible: root.canScrollHorizontally
        opacity: visible
            && (rowHover.hovered || root.moving || wheelAnimation.running || hovered || pressed)
            ? 1 : 0

        Behavior on opacity { NumberAnimation { duration: 160 } }
    }

    WheelHandler {
        target: null
        enabled: root.canScrollHorizontally
        blocking: true
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            const now = Date.now()
            root.wheelBoost = now - root.lastWheelTime < 150
                ? Math.min(2.1, root.wheelBoost + 0.16)
                : 1
            root.lastWheelTime = now
            const pixelDelta = Math.abs(event.pixelDelta.x) > Math.abs(event.pixelDelta.y)
                ? event.pixelDelta.x
                : event.pixelDelta.y
            const rawDelta = pixelDelta !== 0 ? pixelDelta : event.angleDelta.y * 1.7
            const currentTarget = wheelAnimation.running ? wheelAnimation.to : root.contentX
            const maximum = Math.max(0, root.contentWidth - root.width)
            wheelAnimation.to = Math.max(0, Math.min(maximum,
                currentTarget - rawDelta * root.wheelBoost))
            wheelAnimation.restart()
            event.accepted = true
        }
    }

    NumberAnimation {
        id: wheelAnimation
        target: root
        property: "contentX"
        duration: 190
        easing.type: Easing.OutCubic
    }
}
