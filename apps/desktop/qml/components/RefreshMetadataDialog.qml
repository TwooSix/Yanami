import QtQuick
import QtQuick.Layouts
import Yanami.Ui

AppModalPopup {
    id: root

    property var mediaItem: ({})
    property string refreshMode: "all"
    property bool replaceImages: false
    property bool submitting: false
    property string inlineError: ""
    signal refreshRequested(string itemId, string mode, bool replaceImages)

    dismissBlocked: root.submitting

    function openFor(item) {
        root.mediaItem = item || ({})
        // Match Emby Web's refresh dialog: a full metadata refresh is the
        // default, while the non-destructive missing-only mode stays explicit.
        root.refreshMode = "all"
        root.replaceImages = false
        root.submitting = false
        root.inlineError = ""
        root.open()
    }

    function submitSucceeded(itemId) {
        if (!root.opened
                || String(root.mediaItem.id || "") !== String(itemId || ""))
            return false
        root.submitting = false
        root.forceDismiss()
        return true
    }

    function submitFailed(itemId, message) {
        if (!root.opened
                || String(root.mediaItem.id || "") !== String(itemId || ""))
            return false
        root.submitting = false
        root.inlineError = String(message || qsTr("Unable to start the refresh."))
        return true
    }

    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(520, Math.max(360, parent.width - 48))
    height: contentColumn.implicitHeight + 52
    padding: 26
    scrimColor: "#72000000"
    background: Rectangle {
        radius: 26
        color: "#F51A1D26"
        border.width: 1
        border.color: "#4AFFFFFF"
    }

    contentItem: ColumnLayout {
        id: contentColumn
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            Text {
                Layout.fillWidth: true
                text: qsTr("Refresh metadata")
                color: Theme.text
                font.family: Theme.fontForText(text)
                font.pixelSize: 21
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                text: String(root.mediaItem.title || "")
                color: Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
                elide: Text.ElideRight
            }
        }

        Text {
            text: qsTr("Refresh mode")
            color: Theme.textMuted
            font.family: Theme.fontForText(text)
            font.pixelSize: 12
            font.weight: Font.Medium
        }

        Repeater {
            model: [
                {
                    id: "all",
                    title: qsTr("Replace all metadata"),
                    description: qsTr("Download metadata again and overwrite existing fields.")
                },
                {
                    id: "missing",
                    title: qsTr("Search for missing metadata"),
                    description: qsTr("Keep existing metadata and fill only missing information.")
                }
            ]

            delegate: Rectangle {
                id: modeRow
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: 66
                radius: 16
                color: modeMouse.containsMouse ? "#18FFFFFF" : "#0EFFFFFF"
                border.width: root.refreshMode === modelData.id ? 1.5 : 1
                border.color: root.refreshMode === modelData.id ? Theme.accent : Theme.outline

                Rectangle {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    width: 20
                    height: 20
                    radius: 10
                    color: "transparent"
                    border.width: 2
                    border.color: root.refreshMode === modeRow.modelData.id
                        ? Theme.accent : Theme.textMuted

                    Rectangle {
                        visible: root.refreshMode === modeRow.modelData.id
                        anchors.centerIn: parent
                        width: 10
                        height: 10
                        radius: 5
                        color: Theme.accent
                    }
                }

                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: 50
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    Text {
                        width: parent.width
                        text: modeRow.modelData.title
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: modeRow.modelData.description
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    id: modeMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.refreshMode = modeRow.modelData.id
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 50
            radius: 15
            color: replaceMouse.containsMouse ? "#18FFFFFF" : "#0EFFFFFF"
            border.width: 1
            border.color: Theme.outline

            Rectangle {
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                width: 21
                height: 21
                radius: 7
                color: root.replaceImages ? Theme.accent : "transparent"
                border.width: 1.5
                border.color: root.replaceImages ? Theme.accent : Theme.textMuted

                AppIcon {
                    visible: root.replaceImages
                    anchors.centerIn: parent
                    width: 14
                    height: 14
                    name: "check"
                    color: "white"
                    strokeWidth: 2.1
                }
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 51
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Replace existing images")
                color: Theme.text
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
                font.weight: Font.Medium
            }

            MouseArea {
                id: replaceMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.replaceImages = !root.replaceImages
            }
        }

        Text {
            visible: root.refreshMode === "all" || root.replaceImages
            Layout.fillWidth: true
            text: qsTr("Existing custom metadata or images may be overwritten.")
            color: "#FFC46B"
            font.family: Theme.fontForText(text)
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }

        Text {
            visible: root.inlineError.length > 0
            Layout.fillWidth: true
            text: root.inlineError
            color: Theme.danger
            font.family: Theme.fontForText(text)
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                kind: "ghost"
                text: qsTr("Cancel")
                enabled: !root.submitting
                onClicked: root.requestDismiss("cancel")
            }
            AppButton {
                kind: "primary"
                text: root.submitting ? qsTr("Starting…") : qsTr("Refresh")
                enabled: !root.submitting
                onClicked: {
                    const itemId = String(root.mediaItem.id || "")
                    const mode = root.refreshMode
                    const replace = root.replaceImages
                    root.submitting = true
                    root.inlineError = ""
                    root.refreshRequested(itemId, mode, replace)
                }
            }
        }
    }
}
