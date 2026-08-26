import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

ListView {
    id: root

    property real lastWheelTime: 0
    property real wheelBoost: 1
    readonly property bool canScrollHorizontally: contentWidth > width + 1

    signal userScrollStarted()

    onDraggingChanged: {
        if (dragging)
            root.userScrollStarted()
    }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Left || event.key === Qt.Key_Right) {
            root.userScrollStarted()
            event.accepted = false
        }
        // ListView keeps its default keyboard and controller navigation.
    }

    onCountChanged: {
        wheelAnimation.stop()
        if (count === 0)
            positionViewAtBeginning()
        else
            Qt.callLater(root.clampScrollPosition)
    }
    onContentWidthChanged: {
        wheelAnimation.stop()
        Qt.callLater(root.clampScrollPosition)
    }
    onWidthChanged: {
        wheelAnimation.stop()
        Qt.callLater(root.clampScrollPosition)
    }
    onOriginXChanged: {
        wheelAnimation.stop()
        Qt.callLater(root.clampScrollPosition)
    }

    function resetScrollPosition() {
        wheelAnimation.stop()
        root.positionViewAtBeginning()
        Qt.callLater(root.clampScrollPosition)
    }

    function clampScrollPosition() {
        const minimum = root.originX
        const maximum = Math.max(minimum,
            minimum + root.contentWidth - root.width)
        if (root.contentX < minimum)
            root.contentX = minimum
        else if (root.contentX > maximum)
            root.contentX = maximum
    }

    orientation: ListView.Horizontal
    spacing: 14
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    flickDeceleration: 1050
    maximumFlickVelocity: 6800

    // ListView delegates are virtualized. Animating x/y across a sequence of
    // ListModel moves can leave recycled delegates at stale coordinates and
    // can also make contentX snap while the user is scrolling. Keep scrolling
    // geometry deterministic; visual state changes animate inside each card.

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

        onPressedChanged: {
            if (pressed)
                root.userScrollStarted()
        }

        Behavior on opacity { NumberAnimation { duration: 160 } }
    }

    WheelHandler {
        target: null
        enabled: root.canScrollHorizontally
        blocking: true
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: event => {
            root.userScrollStarted()
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
            const minimum = root.originX
            const maximum = Math.max(minimum,
                minimum + root.contentWidth - root.width)
            wheelAnimation.to = Math.max(minimum, Math.min(maximum,
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
