import QtQuick
import QtQuick.Layouts
import Yanami.Ui

Item {
    id: root

    property bool shown: false
    property bool autoDismiss: true
    property bool dismissible: true
    property int timeout: 4200
    property string message: ""
    property string defaultTone: "info"
    property string tone: defaultTone
    property real minimumWidth: 300
    property real maximumWidth: 520
    readonly property color toneColor: tone === "warning"
        ? "#FFB86B"
        : (tone === "success" ? Theme.success
            : (tone === "error" ? Theme.danger : Theme.info))

    signal dismissed()

    implicitWidth: Math.min(maximumWidth,
                            Math.max(minimumWidth,
                                     toastText.implicitWidth
                                     + (dismissible ? 80 : 46)))
    implicitHeight: Math.max(48, toastText.implicitHeight + 20)
    visible: shown || opacity > 0.001
    opacity: shown ? 1 : 0
    scale: shown ? 1 : 0.98
    Accessible.role: Accessible.StaticText
    Accessible.name: message

    function show(nextMessage, nextTone, durationMs) {
        message = String(nextMessage || "")
        tone = nextTone !== undefined && String(nextTone).length > 0
            ? String(nextTone) : defaultTone
        if (durationMs !== undefined && Number(durationMs) > 0)
            dismissTimer.interval = Number(durationMs)
        else
            dismissTimer.interval = timeout
        shown = message.length > 0
        if (shown && autoDismiss)
            dismissTimer.restart()
        else
            dismissTimer.stop()
    }

    function dismiss() {
        dismissTimer.stop()
        if (!shown)
            return
        shown = false
        dismissed()
    }

    transform: Translate {
        y: root.shown ? 0 : -12

        Behavior on y {
            NumberAnimation {
                duration: root.shown ? 220 : 150
                easing.type: root.shown ? Easing.OutCubic : Easing.InCubic
            }
        }
    }

    GlassPanel {
        id: panel
        objectName: "statusToastPanel"
        anchors.fill: parent
        radius: 16
        color: Theme.surfaceStrong
        border.color: Qt.rgba(root.toneColor.r,
                              root.toneColor.g,
                              root.toneColor.b, 0.38)

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 14
            anchors.rightMargin: 8
            anchors.topMargin: 10
            anchors.bottomMargin: 10
            spacing: 8

            Rectangle {
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                Layout.alignment: Qt.AlignVCenter
                radius: 4
                color: root.toneColor
                scale: root.shown ? 1 : 0.7

                Behavior on scale {
                    NumberAnimation {
                        duration: root.shown ? 240 : 120
                        easing.type: root.shown
                            ? Easing.OutCubic : Easing.InCubic
                    }
                }
            }

            Text {
                id: toastText
                objectName: "statusToastMessage"
                Layout.fillWidth: true
                text: root.message
                color: Theme.text
                font.family: Theme.fontForText(text)
                font.pixelSize: 14
                font.weight: Font.DemiBold
                lineHeight: 1.12
                lineHeightMode: Text.ProportionalHeight
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }

            AppButton {
                id: closeButton
                objectName: "statusToastCloseButton"
                visible: root.dismissible
                enabled: root.shown
                Layout.preferredWidth: visible ? 28 : 0
                Layout.preferredHeight: 28
                Layout.alignment: Qt.AlignVCenter
                iconOnly: true
                controlSize: 28
                iconSize: 11
                iconName: "window-close"
                accessibleName: qsTr("Close notification")
                kind: "ghost"
                onClicked: root.dismiss()

                background: Item {
                    implicitWidth: closeButton.controlSize
                    implicitHeight: closeButton.controlSize

                    Rectangle {
                        id: closeHighlight
                        objectName: "statusToastCloseHighlight"
                        anchors.centerIn: parent
                        width: 20
                        height: 20
                        radius: 10
                        color: closeButton.down ? "#24FFFFFF"
                            : (closeButton.hovered
                                ? "#18FFFFFF" : "transparent")
                        border.width: closeButton.visualFocus ? 1 : 0
                        border.color: Theme.text
                        scale: closeButton.down ? 0.92 : 1

                        Behavior on color {
                            ColorAnimation { duration: 120 }
                        }
                        Behavior on scale {
                            NumberAnimation {
                                duration: 90
                                easing.type: Easing.OutCubic
                            }
                        }
                    }
                }
            }
        }
    }

    Timer {
        id: dismissTimer
        interval: root.timeout
        onTriggered: root.dismiss()
    }

    Behavior on opacity {
        NumberAnimation {
            duration: root.shown ? 180 : 140
            easing.type: root.shown ? Easing.OutCubic : Easing.InCubic
        }
    }

    Behavior on scale {
        NumberAnimation {
            duration: root.shown ? 210 : 140
            easing.type: root.shown ? Easing.OutCubic : Easing.InCubic
        }
    }
}
