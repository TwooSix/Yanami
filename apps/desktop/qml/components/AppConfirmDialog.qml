import QtQuick
import QtQuick.Layouts
import Yanami.Ui

AppModalPopup {
    id: root

    property string heading: ""
    property string message: ""
    property string confirmText: qsTr("Confirm")
    property string confirmKind: "danger"
    property bool closeOnConfirm: true
    property bool submitting: false
    property string inlineError: ""
    signal confirmed()
    signal rejected(string reason)

    function show(title, body, actionText) {
        root.heading = title
        root.message = body
        root.confirmText = actionText || qsTr("Confirm")
        root.submitting = false
        root.inlineError = ""
        root.open()
    }

    function complete() {
        root.submitting = false
        root.forceDismiss()
    }

    function fail(message) {
        root.submitting = false
        root.inlineError = String(message || qsTr("The operation failed."))
    }

    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(470, Math.max(330, parent.width - 48))
    height: contentColumn.implicitHeight + 52
    padding: 26
    popupRole: PopupCoordinator.confirmRole
    scrimColor: "#72000000"
    initialFocusTarget: cancelButton
    dismissBlocked: root.submitting
    onDismissedByUser: reason => root.rejected(reason)
    background: Rectangle {
        radius: 26
        color: "#F31A1D26"
        border.width: 1
        border.color: "#4AFFFFFF"
    }

    PopupControllerNavigator { popup: root }

    contentItem: ColumnLayout {
        id: contentColumn
        spacing: 16

        Text {
            Layout.fillWidth: true
            text: root.heading
            color: Theme.text
            font.family: Theme.fontForText(text)
            font.pixelSize: 19
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
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
        Text {
            Layout.fillWidth: true
            text: root.message
            color: Theme.textMuted
            font.family: Theme.fontForText(text)
            font.pixelSize: 13
            lineHeight: 1.35
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            AppButton {
                id: cancelButton
                kind: "ghost"
                text: qsTr("Cancel")
                enabled: !root.submitting
                onClicked: root.requestDismiss("cancel")
                KeyNavigation.left: confirmButton
                KeyNavigation.right: confirmButton
                KeyNavigation.tab: confirmButton
                KeyNavigation.backtab: confirmButton
            }
            AppButton {
                id: confirmButton
                kind: root.confirmKind
                text: root.submitting ? qsTr("Working…") : root.confirmText
                enabled: !root.submitting
                onClicked: {
                    if (root.closeOnConfirm)
                        root.forceDismiss()
                    else {
                        root.submitting = true
                        root.inlineError = ""
                    }
                    root.confirmed()
                }
                KeyNavigation.left: cancelButton
                KeyNavigation.right: cancelButton
                KeyNavigation.tab: cancelButton
                KeyNavigation.backtab: cancelButton
            }
        }
    }
}
