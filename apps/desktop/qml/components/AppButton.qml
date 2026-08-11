import QtQuick
import QtQuick.Controls.Basic
import Yanami

Button {
    id: control

    property string kind: "secondary"
    property bool iconOnly: false
    property string iconName: ""
    property real iconSize: iconOnly ? 20 : 17
    property real controlSize: 44
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

    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: control.controlSize

        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: control.iconName.length > 0 && label.visible ? 8 : 0

            AppIcon {
                anchors.verticalCenter: parent.verticalCenter
                visible: control.iconName.length > 0
                width: visible ? control.iconSize : 0
                height: control.iconSize
                name: control.iconName
                color: control.foregroundColor

                Behavior on color { ColorAnimation { duration: 140 } }
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
        border.width: control.kind === "primary" ? 0 : 1
        border.color: control.checked ? "#70FF6687"
            : (control.kind === "glass" ? "#48FFFFFF" : Theme.outline)
        scale: control.down ? 0.97 : 1

        Behavior on color { ColorAnimation { duration: 140 } }
        Behavior on border.color { ColorAnimation { duration: 140 } }
        Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
    }
}
