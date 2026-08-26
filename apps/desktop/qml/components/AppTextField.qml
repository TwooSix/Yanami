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
    readonly property Item focusTarget: field
    readonly property bool inputActiveFocus: field.activeFocus
    // TextField consumes arrow keys for cursor movement. Controller and remote
    // navigation can opt into an explicit escape route without changing normal
    // keyboard editing behavior.
    property var controllerUpHandler: null
    property var controllerDownHandler: null
    property var controllerLeftHandler: null
    property var controllerRightHandler: null
    signal accepted()
    implicitWidth: 280
    implicitHeight: 78

    function focusInput() {
        field.forceActiveFocus()
        field.selectAll()
    }

    function clearInput() {
        field.clear()
    }

    function handleControllerDirection(key) {
        if (InputModality.modality !== InputModality.Controller
                && InputModality.modality !== InputModality.Remote) {
            return false
        }
        let handler = null
        if (key === Qt.Key_Up)
            handler = root.controllerUpHandler
        else if (key === Qt.Key_Down)
            handler = root.controllerDownHandler
        else if (key === Qt.Key_Left)
            handler = root.controllerLeftHandler
        else if (key === Qt.Key_Right)
            handler = root.controllerRightHandler
        if (typeof handler === "function" && handler() === true)
            return true

        // A text input without a page-specific route must still be escapable
        // by controller. Tab-chain fallback is deterministic and avoids a
        // focus trap while keyboard arrows keep their editing semantics.
        const forward = key === Qt.Key_Down || key === Qt.Key_Right
        const target = field.nextItemInFocusChain(forward)
        if (!target || target === field)
            return false
        target.forceActiveFocus(forward
                                ? Qt.TabFocusReason : Qt.BacktabFocusReason)
        return target.activeFocus === true
    }

    function handleControllerCommand(key) {
        if (InputModality.modality !== InputModality.Controller
                && InputModality.modality !== InputModality.Remote) {
            return false
        }
        if (key === Qt.Key_Menu) {
            root.clearInput()
            return true
        }
        return root.handleControllerDirection(key)
    }

    Text {
        id: labelText
        anchors.left: parent.left
        anchors.right: controllerClearHint.visible
            ? controllerClearHint.left : parent.right
        anchors.rightMargin: controllerClearHint.visible ? 10 : 0
        anchors.top: parent.top
        text: root.label
        color: field.activeFocus ? Theme.text : Theme.textMuted
        font.family: Theme.fontForText(root.label)
        font.pixelSize: 12
        font.weight: Font.Medium

        Behavior on color { ColorAnimation { duration: 140 } }
    }

    Text {
        id: controllerClearHint
        anchors.right: parent.right
        anchors.top: parent.top
        visible: field.activeFocus && field.text.length > 0
            && (InputModality.modality === InputModality.Controller
                || InputModality.modality === InputModality.Remote)
        text: InputModality.promptForAction(InputModality.Context)
            + "  " + qsTr("Clear")
        color: Theme.textMuted
        font.family: Theme.fontForText(text)
        font.pixelSize: 10
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
        Keys.priority: Keys.BeforeItem
        Keys.onPressed: event => {
            if (root.handleControllerCommand(event.key))
                event.accepted = true
        }

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
