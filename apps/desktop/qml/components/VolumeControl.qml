import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

Item {
    id: root

    property real volume: 100
    property real lastAudibleVolume: 70
    // A player can provide a stable, always-visible item from whose Window
    // overlay the popup is hosted.  The popup must use the overlay's coordinate
    // system too; Qt reparents Popup visuals there, so coordinates relative to
    // a faded transport row would otherwise be clamped to the window corner.
    property Item popupHost: null
    readonly property bool opened: volumePopup.opened
    readonly property point popupPosition:
        Qt.point(volumePopup.x, volumePopup.y)
    readonly property bool keyboardInteractionActive: volumePopup.opened
        && (volumeSlider.activeFocus || volumeSlider.pressed)
    readonly property Item focusTarget: volumeButton
    property Item navigationLeft: null
    property Item navigationRight: null
    property Item navigationUp: null
    property Item navigationDown: null
    signal volumeRequested(real value)

    implicitWidth: 38
    implicitHeight: 38

    onVolumeChanged: {
        if (volume > 0.5)
            lastAudibleVolume = volume
    }

    function showVolume() {
        closeTimer.stop()
        root.updatePopupPosition()
        if (!volumePopup.opened)
            volumePopup.open()
        // Popup creates/reparents its visual item while opening. Recalculate
        // once that has happened so a RowLayout's final position is used.
        Qt.callLater(root.updatePopupPosition)
    }

    function popupAnchor() {
        const host = volumePopup.parent || root.popupHost || root
        return root.mapToItem(host, root.width / 2, 0)
    }

    function updatePopupPosition() {
        if (!volumePopup.parent)
            return
        const anchor = root.popupAnchor()
        volumePopup.x = anchor.x - volumePopup.width / 2
        volumePopup.y = anchor.y - volumePopup.height - 12
    }

    function scheduleClose() {
        closeTimer.interval = 260
        closeTimer.restart()
    }

    function showTransientVolume() {
        root.showVolume()
        closeTimer.interval = 1200
        closeTimer.restart()
    }

    function closePopup() {
        closeTimer.stop()
        volumePopup.close()
    }

    function toggleMuted() {
        root.closePopup()
        if (root.volume > 0.5) {
            root.lastAudibleVolume = root.volume
            root.volumeRequested(0)
        } else {
            root.volumeRequested(Math.max(5, root.lastAudibleVolume))
        }
    }

    function triggerFromOverlayClick() {
        const popup = PopupCoordinator.topPopup()
        if (popup && popup !== volumePopup)
            popup.requestDismiss("overlay-action")
        root.toggleMuted()
    }

    AppButton {
        id: volumeButton
        anchors.fill: parent
        kind: "ghost"
        iconOnly: true
        iconName: root.volume <= 0
            ? "volume-muted"
            : (root.volume < 55 ? "volume-low" : "volume-high")
        iconSize: 18
        controlSize: 38
        Accessible.name: root.volume <= 0 ? qsTr("Unmute") : qsTr("Mute")
        toolTipVisible: hovered && !volumePopup.opened
        toolTipText: Accessible.name
        onHoveredChanged: {
            if (hovered)
                root.showVolume()
            else
                root.scheduleClose()
        }
        onClicked: root.toggleMuted()
        KeyNavigation.left: root.navigationLeft
        KeyNavigation.right: root.navigationRight
        KeyNavigation.up: root.navigationUp
        KeyNavigation.down: root.navigationDown
    }

    AppTransientPopup {
        id: volumePopup
        parent: {
            const host = root.popupHost || root
            return host.Overlay.overlay || host
        }
        x: 0
        y: 0
        width: 64
        height: 174
        padding: 8
        takesFocus: false
        blocksShortcuts: false
        exclusiveWithinScope: false

        background: Rectangle {
            radius: 22
            color: "#F21A1D26"
            border.width: 1
            border.color: "#42FFFFFF"
        }

        contentItem: Item {
            HoverHandler {
                id: popupHover
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onHoveredChanged: {
                    if (hovered)
                        root.showVolume()
                    else
                        root.scheduleClose()
                }
            }

            Slider {
                id: volumeSlider
                objectName: "volume-slider"
                readonly property real handleBoxSize: 18
                readonly property real axisX: Math.round(
                    leftPadding + availableWidth / 2)
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 8
                width: 32
                height: 126
                orientation: Qt.Vertical
                from: 0
                to: 100
                value: root.volume
                live: true
                hoverEnabled: true
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                Accessible.name: qsTr("Volume")
                onMoved: root.volumeRequested(value)

                background: Item {
                    objectName: "volume-slider-axis"
                    x: 0
                    y: 0
                    width: volumeSlider.width
                    height: volumeSlider.height

                    Rectangle {
                        objectName: "volume-slider-rail"
                        x: volumeSlider.axisX - width / 2
                        y: volumeSlider.topPadding
                            + volumeSlider.handleBoxSize / 2
                        width: 4
                        height: volumeSlider.availableHeight
                            - volumeSlider.handleBoxSize
                        radius: width / 2
                        color: "#34FFFFFF"
                    }

                    Rectangle {
                        objectName: "volume-slider-progress"
                        x: volumeSlider.axisX - width / 2
                        y: volumeHandle.y + volumeHandle.height / 2
                        width: 4
                        height: volumeSlider.topPadding
                            + volumeSlider.availableHeight
                            - volumeSlider.handleBoxSize / 2 - y
                        radius: width / 2
                        color: Theme.accent
                    }
                }

                handle: Item {
                    id: volumeHandle
                    objectName: "volume-slider-handle"
                    x: volumeSlider.axisX - width / 2
                    y: Math.round(volumeSlider.topPadding
                        + (1 - volumeSlider.position)
                            * (volumeSlider.availableHeight - height))
                    width: volumeSlider.handleBoxSize
                    height: volumeSlider.handleBoxSize

                    Rectangle {
                        anchors.centerIn: parent
                        width: volumeSlider.pressed || volumeSlider.hovered ? 18 : 16
                        height: width
                        radius: width / 2
                        color: "white"
                        border.width: 1
                        border.color: "#33000000"

                        Behavior on width {
                            NumberAnimation {
                                duration: 110
                                easing.type: Easing.OutCubic
                            }
                        }
                    }
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 4
                text: Math.round(root.volume) + "%"
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Medium
            }

            WheelHandler {
                target: null
                blocking: true
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: event => {
                    let delta = event.pixelDelta.y !== 0
                        ? event.pixelDelta.y * 0.12
                        : event.angleDelta.y / 120 * 5
                    if (event.inverted)
                        delta = -delta
                    root.volumeRequested(Math.max(0, Math.min(100,
                        root.volume + delta)))
                    event.accepted = true
                }
            }
        }

        onClosed: closeTimer.stop()
    }

    onXChanged: {
        if (volumePopup.opened)
            root.updatePopupPosition()
    }
    onYChanged: {
        if (volumePopup.opened)
            root.updatePopupPosition()
    }

    Connections {
        target: root.popupHost
        enabled: target !== null

        function onWidthChanged() {
            if (volumePopup.opened)
                Qt.callLater(root.updatePopupPosition)
        }

        function onHeightChanged() {
            if (volumePopup.opened)
                Qt.callLater(root.updatePopupPosition)
        }
    }

    Timer {
        id: closeTimer
        interval: 260
        onTriggered: {
            if (!volumeButton.hovered && !popupHover.hovered)
                volumePopup.close()
        }
    }

    Component.onCompleted:
        PopupCoordinator.registerOverlayClickTarget(root)
    Component.onDestruction:
        PopupCoordinator.unregisterOverlayClickTarget(root)
}
