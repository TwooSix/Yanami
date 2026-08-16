import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

Button {
    id: control

    property string kind: "secondary"
    property bool iconOnly: false
    property string iconName: ""
    property string accessibleName: ""
    property bool iconSpinning: false
    property real controlSize: 44
    property string toolTipText: ""
    property bool toolTipVisible: hovered && toolTipText.length > 0
    property int toolTipDelay: 500
    property int toolTipTimeout: 5000
    // Icon-only controls use the same visual proportion throughout the app:
    // 48% of the circular control, clamped for compact and large variants.
    // Deliberate emphasis (for example the primary play control) can still
    // opt in to a different ratio by specifying iconSize explicitly.
    property real iconSize: iconOnly
        ? Math.max(14, Math.min(20, Math.round(controlSize * 0.48))) : 17
    readonly property color foregroundColor: !control.enabled
        ? "#737C8C"
        : (control.kind === "primary" ? "white" : Theme.text)

    implicitWidth: iconOnly ? controlSize : Math.max(96, contentItem.implicitWidth + 36)
    implicitHeight: controlSize
    leftPadding: iconOnly ? 0 : 18
    rightPadding: iconOnly ? 0 : 18
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.Button
    Accessible.name: control.accessibleName.length > 0
        ? control.accessibleName
        : (control.text.length > 0
        ? control.text
        : (control.toolTipText.length > 0
            ? control.toolTipText : control.iconName))
    Keys.onPressed: event => {
        if (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter)
            return
        control.click()
        event.accepted = true
    }

    onIconSpinningChanged: {
        if (!iconSpinning)
            buttonIcon.rotation = 0
    }
    onPressedChanged: {
        if (pressed)
            PopupCoordinator.notePopupContentPress()
    }

    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: control.controlSize

        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: control.iconName.length > 0 && label.visible ? 8 : 0

            AppIcon {
                id: buttonIcon
                anchors.verticalCenter: parent.verticalCenter
                anchors.horizontalCenterOffset: control.iconOnly
                    && control.iconName === "play" ? 0.8 : 0
                visible: control.iconName.length > 0
                width: visible ? control.iconSize : 0
                height: control.iconSize
                name: control.iconName
                color: control.foregroundColor

                Behavior on color { ColorAnimation { duration: 140 } }

                RotationAnimator {
                    target: buttonIcon
                    from: 0
                    to: 360
                    duration: 820
                    loops: Animation.Infinite
                    running: control.iconSpinning
                }

            }

            Text {
                id: label
                anchors.verticalCenter: parent.verticalCenter
                visible: !control.iconOnly || control.iconName.length === 0
                text: control.text
                color: control.foregroundColor
                font.family: Theme.fontForText(control.text)
                font.pixelSize: control.iconOnly ? Math.max(18, control.controlSize * 0.45) : 14
                font.weight: Font.DemiBold
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight

                Behavior on color { ColorAnimation { duration: 140 } }
            }
        }
    }

    background: Rectangle {
        radius: control.height / 2
        color: {
            if (!control.enabled)
                return "#10FFFFFF"
            if (control.kind === "primary")
                return control.down ? "#E65372" : (control.hovered ? Theme.accentHover : Theme.accent)
            if (control.kind === "danger")
                return control.down ? "#42FF718D" : (control.hovered ? "#36FF718D" : "#24FF718D")
            if (control.kind === "glass")
                return control.down ? "#A0242833" : (control.hovered ? "#881B1E27" : "#68151820")
            if (control.checked)
                return control.down ? "#45FF6687" : "#32FF6687"
            if (control.kind === "ghost")
                return control.down ? "#24FFFFFF" : (control.hovered ? "#18FFFFFF" : "#08FFFFFF")
            return control.down ? "#2BFFFFFF" : (control.hovered ? "#20FFFFFF" : "#14FFFFFF")
        }
        border.width: control.visualFocus ? 2
            : (control.kind === "primary" ? 0 : 1)
        border.color: control.visualFocus
            ? (control.kind === "primary" ? "white" : Theme.accent)
            : (control.checked ? "#70FF6687"
                : (control.kind === "glass" ? "#48FFFFFF" : Theme.outline))
        scale: control.down ? 0.97 : 1

        Behavior on color { ColorAnimation { duration: 140 } }
        Behavior on border.color { ColorAnimation { duration: 140 } }
        Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
    }

    AppToolTip {
        parent: control
        visible: control.toolTipVisible && control.toolTipText.length > 0
        text: control.toolTipText
        delay: control.toolTipDelay
        timeout: control.toolTipTimeout
    }
}
