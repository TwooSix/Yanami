import QtQuick
import QtQuick.Layouts
import Yanami.Ui

AppModalPopup {
    id: root

    property string message

    function show(errorMessage) {
        const value = String(errorMessage || "").trim()
        if (value.length === 0)
            return
        root.message = value
        if (!root.opened)
            root.open()
    }

    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(480, Math.max(320, parent.width - 48))
    height: Math.min(300, Math.max(210, dialogContent.implicitHeight + 52))
    padding: 26
    popupRole: PopupCoordinator.errorRole
    scrimColor: "#72000000"
    initialFocusTarget: closeButton

    background: Rectangle {
        radius: 26
        color: "#F31A1D26"
        border.width: 1
        border.color: "#4AFFFFFF"
    }

    contentItem: ColumnLayout {
        id: dialogContent
        spacing: 16

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: 17
                color: "#24FF879E"
                border.width: 1
                border.color: "#58FF879E"

                Text {
                    anchors.centerIn: parent
                    text: "!"
                    color: Theme.danger
                    font.family: Theme.fontFamily
                    font.pixelSize: 19
                    font.weight: Font.Bold
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Something went wrong")
                color: Theme.text
                font.family: Theme.fontForText(text)
                font.pixelSize: 19
                font.weight: Font.DemiBold
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.maximumHeight: 136
            text: root.message
            color: Theme.textMuted
            font.family: Theme.fontForText(text)
            font.pixelSize: 13
            lineHeight: 1.35
            wrapMode: Text.Wrap
            elide: Text.ElideRight
            maximumLineCount: 6
        }

        AppButton {
            id: closeButton
            Layout.alignment: Qt.AlignRight
            kind: "primary"
            text: qsTr("Close")
            onClicked: root.requestDismiss("close-button")
        }
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 150 }
        NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: 180; easing.type: Easing.OutCubic }
    }

    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 120 }
        NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: 120 }
    }
}
