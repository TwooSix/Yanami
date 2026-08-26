import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

AppTransientPopup {
    id: root

    property bool upscalingEnabled: false
    property bool upscalingAvailable: false

    signal upscalingToggleRequested(bool enabled)

    width: 292
    height: topPadding + bottomPadding + 52
    padding: 8
    initialFocusTarget: upscalingToggle

    background: Rectangle {
        radius: 20
        color: "#F21A1D26"
        border.width: 1
        border.color: "#42FFFFFF"
    }

    enter: Transition {
        ParallelAnimation {
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: 150
                easing.type: Easing.OutCubic
            }
            NumberAnimation {
                property: "scale"
                from: 0.97
                to: 1
                duration: 180
                easing.type: Easing.OutCubic
            }
        }
    }

    exit: Transition {
        ParallelAnimation {
            NumberAnimation {
                property: "opacity"
                from: 1
                to: 0
                duration: 120
                easing.type: Easing.InCubic
            }
            NumberAnimation {
                property: "scale"
                from: 1
                to: 0.98
                duration: 120
                easing.type: Easing.InCubic
            }
        }
    }

    contentItem: Button {
        id: upscalingToggle

        checkable: true
        checked: root.upscalingEnabled
        enabled: root.upscalingAvailable
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.CheckBox
        Accessible.name: qsTr("Real-time anime upscaling")
        Accessible.description: root.upscalingAvailable
            ? qsTr("Toggle real-time anime upscaling for the current playback.")
            : qsTr("Open Settings to check Anime4K availability and components before turning it on.")

        onPressedChanged: {
            if (pressed)
                PopupCoordinator.notePopupContentPress()
        }
        onClicked: root.upscalingToggleRequested(checked)
        Keys.onPressed: event => {
            if (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter)
                return
            click()
            event.accepted = true
        }

        background: Rectangle {
            radius: 14
            color: !upscalingToggle.enabled
                ? "#08FFFFFF"
                : upscalingToggle.down
                    ? "#2AFFFFFF"
                    : upscalingToggle.hovered ? "#20FFFFFF" : "transparent"
            border.width: upscalingToggle.visualFocus ? 2 : 0
            border.color: Theme.accent

            Behavior on color {
                ColorAnimation { duration: 120 }
            }
        }

        contentItem: Item {
            Text {
                anchors.left: parent.left
                anchors.right: switchTrack.left
                anchors.leftMargin: 12
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Real-time anime upscaling")
                color: upscalingToggle.enabled ? Theme.text : "#697282"
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
                font.weight: Font.DemiBold
                elide: Text.ElideRight

                Behavior on color {
                    ColorAnimation { duration: 140 }
                }
            }

            Rectangle {
                id: switchTrack

                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 46
                height: 26
                radius: height / 2
                color: upscalingToggle.checked && upscalingToggle.enabled
                    ? Theme.accent : "#24FFFFFF"
                border.width: 1
                border.color: upscalingToggle.checked
                    && upscalingToggle.enabled
                    ? Theme.accentHover : Theme.outlineStrong

                Rectangle {
                    width: 18
                    height: 18
                    radius: 9
                    x: upscalingToggle.checked && upscalingToggle.enabled
                        ? parent.width - width - 4 : 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: upscalingToggle.checked
                        && upscalingToggle.enabled ? "white" : "#AAB2C1"

                    Behavior on x {
                        NumberAnimation {
                            duration: 160
                            easing.type: Easing.OutCubic
                        }
                    }
                }

                Behavior on color {
                    ColorAnimation { duration: 140 }
                }
                Behavior on border.color {
                    ColorAnimation { duration: 140 }
                }
            }
        }
    }
}
