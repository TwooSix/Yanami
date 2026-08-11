import QtQuick
import QtQuick.Controls.Basic
import Yanami

Item {
    id: root

    property real volume: 100
    property real lastAudibleVolume: 70
    readonly property bool opened: volumePopup.opened
    signal volumeRequested(real value)

    implicitWidth: 38
    implicitHeight: 38

    onVolumeChanged: {
        if (volume > 0.5)
            lastAudibleVolume = volume
    }

    function showVolume() {
        closeTimer.stop()
        if (!volumePopup.opened)
            volumePopup.open()
    }

    function scheduleClose() {
        closeTimer.restart()
    }

    function closePopup() {
        closeTimer.stop()
        volumePopup.close()
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
        ToolTip.visible: hovered && !volumePopup.opened
        ToolTip.text: Accessible.name
        ToolTip.delay: 500
        onHoveredChanged: {
            if (hovered)
                root.showVolume()
            else
                root.scheduleClose()
        }
        onClicked: {
            if (root.volume > 0.5) {
                root.lastAudibleVolume = root.volume
                root.volumeRequested(0)
            } else {
                root.volumeRequested(Math.max(5, root.lastAudibleVolume))
            }
            root.showVolume()
        }
    }

    Popup {
        id: volumePopup
        parent: root
        x: (root.width - width) / 2
        y: -height - 12
        width: 64
        height: 174
        padding: 8
        modal: false
        focus: false
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

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
                Accessible.name: qsTr("Volume")
                onMoved: root.volumeRequested(value)

                background: Item {
                    x: volumeSlider.leftPadding
                    y: volumeSlider.topPadding
                    width: volumeSlider.availableWidth
                    height: volumeSlider.availableHeight

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 5
                        height: parent.height
                        radius: 2.5
                        color: "#34FFFFFF"
                    }

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        width: 5
                        height: volumeSlider.position * parent.height
                        radius: 2.5
                        color: Theme.accent
                    }
                }

                handle: Rectangle {
                    x: volumeSlider.leftPadding
                        + (volumeSlider.availableWidth - width) / 2
                    y: volumeSlider.topPadding
                        + (1 - volumeSlider.position)
                            * (volumeSlider.availableHeight - height)
                    width: volumeSlider.pressed || volumeSlider.hovered ? 18 : 15
                    height: width
                    radius: width / 2
                    color: "white"
                    border.width: 1
                    border.color: "#33000000"

                    Behavior on width {
                        NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
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
                    const delta = event.pixelDelta.y !== 0
                        ? event.pixelDelta.y : event.angleDelta.y
                    root.volumeRequested(Math.max(0, Math.min(100,
                        root.volume + (delta > 0 ? 5 : -5))))
                    event.accepted = true
                }
            }
        }

        onClosed: closeTimer.stop()
    }

    Timer {
        id: closeTimer
        interval: 260
        onTriggered: {
            if (!volumeButton.hovered && !popupHover.hovered)
                volumePopup.close()
        }
    }
}
