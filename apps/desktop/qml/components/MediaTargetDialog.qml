import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami.Ui

AppModalPopup {
    id: root

    property var viewModel: null
    property var mediaItem: ({})
    property var options: []
    property string selectedId: ""
    property bool submitting: false
    property string inlineError: ""
    signal validationError(string message)
    signal actionFailure(string message, bool nonModal, bool handledInPlace)
    signal added(string itemId)

    dismissBlocked: root.submitting

    function openFor(item, targets) {
        root.mediaItem = item || ({})
        root.options = targets || []
        root.selectedId = ""
        root.submitting = false
        root.inlineError = ""
        newNameField.text = ""
        root.open()
    }

    function submit() {
        const name = newNameField.text.trim()
        if (root.selectedId.length === 0 && name.length === 0) {
            root.validationError(qsTr("Choose an existing destination or enter a new name."))
            return
        }
        const itemId = String(root.mediaItem.id || "")
        const targetId = name.length > 0 ? "" : root.selectedId
        root.submitting = true
        root.inlineError = ""
        if (!root.viewModel || !root.viewModel.submit(targetId, name)) {
            root.submitting = false
            root.inlineError = qsTr("Unable to add this item.")
        }
    }

    function submitSucceeded(itemId) {
        if (String(root.mediaItem.id || "") !== String(itemId || ""))
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
        root.inlineError = String(message || qsTr("Unable to add this item."))
        return true
    }

    onClosed: if (root.viewModel) root.viewModel.cancel()

    Connections {
        target: root.viewModel

        function onTargetsReady(item, options) {
            root.openFor(item, options)
        }
        function onLoadFailed(itemId, message, nonModal) {
            root.actionFailure(message, nonModal, false)
        }
        function onSubmitCompleted(itemId, result) {
            if (root.submitSucceeded(itemId))
                root.added(itemId)
        }
        function onSubmitFailed(itemId, message, nonModal) {
            const handled = root.submitFailed(itemId, message)
            root.actionFailure(message, nonModal, handled)
        }
    }

    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(520, Math.max(380, parent.width - 48))
    height: Math.min(650, Math.max(420, parent.height - 72))
    padding: 26
    scrimColor: "#76000000"
    background: Rectangle {
        radius: 26
        color: "#F51A1D26"
        border.width: 1
        border.color: "#4AFFFFFF"
    }

    contentItem: ColumnLayout {
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            Text {
                Layout.fillWidth: true
                text: qsTr("Add to playlist")
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
            text: qsTr("Existing destinations")
            color: Theme.textMuted
            font.family: Theme.fontForText(text)
            font.pixelSize: 12
            font.weight: Font.Medium
        }

        ListView {
            id: targetList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 6
            model: root.options
            ScrollBar.vertical: AppScrollBar {
                policy: ScrollBar.AlwaysOn
                visible: root.options.length * 54 > targetList.height
            }

            delegate: Rectangle {
                id: targetRow
                required property var modelData
                transform: Translate { x: Theme.scrollBarGutter }
                width: Math.max(0, targetList.width - 2 * Theme.scrollBarGutter)
                height: 48
                radius: 14
                color: rowMouse.containsMouse ? "#18FFFFFF" : "#0EFFFFFF"
                border.width: root.selectedId === String(modelData.id || "") ? 1.5 : 1
                border.color: root.selectedId === String(modelData.id || "")
                    ? Theme.accent : Theme.outline

                Rectangle {
                    anchors.left: parent.left
                    anchors.leftMargin: 15
                    anchors.verticalCenter: parent.verticalCenter
                    width: 19
                    height: 19
                    radius: 9.5
                    color: "transparent"
                    border.width: 2
                    border.color: root.selectedId === String(targetRow.modelData.id || "")
                        ? Theme.accent : Theme.textMuted
                    Rectangle {
                        visible: root.selectedId === String(targetRow.modelData.id || "")
                        anchors.centerIn: parent
                        width: 9
                        height: 9
                        radius: 4.5
                        color: Theme.accent
                    }
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 48
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: String(targetRow.modelData.title || "")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.selectedId = String(targetRow.modelData.id || "")
                        newNameField.text = ""
                    }
                }
            }

            Text {
                visible: root.options.length === 0
                anchors.centerIn: parent
                text: qsTr("No existing destinations")
                color: Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
            }
        }

        AppTextField {
            id: newNameField
            Layout.fillWidth: true
            label: qsTr("New playlist")
            placeholderText: qsTr("Enter a name to create a new destination")
            onTextChanged: if (text.trim().length > 0) root.selectedId = ""
        }

        Text {
            Layout.fillWidth: true
            visible: root.inlineError.length > 0
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
                text: root.submitting ? qsTr("Adding…") : qsTr("Add")
                enabled: !root.submitting
                onClicked: root.submit()
            }
        }
    }
}
