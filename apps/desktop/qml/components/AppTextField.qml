import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

Item {
    id: root

    property string label: ""
    property alias text: field.text
    property alias placeholderText: field.placeholderText
    property alias echoMode: field.echoMode
    property alias validator: field.validator
    property alias inputMethodHints: field.inputMethodHints
    readonly property bool inputMethodComposing: field.inputMethodComposing
    signal accepted()
    implicitWidth: 280
    implicitHeight: 78

    function focusInput() {
        field.forceActiveFocus()
        field.selectAll()
    }

    Text {
        id: labelText
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        text: root.label
        color: field.activeFocus ? Theme.text : Theme.textMuted
        font.family: Theme.fontForText(root.label)
        font.pixelSize: 12
        font.weight: Font.Medium

        Behavior on color { ColorAnimation { duration: 140 } }
    }

    TextField {
        id: field
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 52
        leftPadding: 16
        rightPadding: 16
        color: Theme.text
        placeholderTextColor: "#6F788A"
        selectionColor: Theme.accent
        selectedTextColor: "white"
        font.family: Theme.fontForText(field.text.length > 0 ? field.text : field.placeholderText)
        font.pixelSize: 14
        selectByMouse: true
        onAccepted: root.accepted()

        background: Rectangle {
            radius: Theme.radiusSmall
            color: field.activeFocus ? "#E0161922" : Theme.field
            border.width: field.activeFocus ? 1.5 : 1
            border.color: field.activeFocus ? Theme.accent : (field.hovered ? Theme.outlineStrong : Theme.outline)

            Behavior on color { ColorAnimation { duration: 140 } }
            Behavior on border.color { ColorAnimation { duration: 140 } }
        }
    }

    TapHandler {
        parent: field
        acceptedButtons: Qt.AllButtons
        onPressedChanged: {
            if (pressed)
                PopupCoordinator.notePopupContentPress()
        }
    }
}
