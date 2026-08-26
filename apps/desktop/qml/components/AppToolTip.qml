import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

ToolTip {
    id: control

    property real maximumWidth: 360
    property real verticalGap: 9
    property bool keepOpenOnHover: false
    property bool selectableText: false
    property bool coordinatePopupLifecycle: false
    property int popupRole: PopupCoordinator.transientRole
    property int stackOrder: 0
    property bool exclusiveWithinScope: false
    property var exclusiveScope: null
    property var popupWindow: null
    property bool blocksShortcuts: false
    property bool dismissOnEscape: true
    readonly property bool hovered: keepOpenOnHover
        && (contentHoverHandler.hovered || backgroundHoverHandler.hovered)
    readonly property bool textHasActiveFocus: selectableText
        && toolTipText.activeFocus
    signal escapeRequested

    function requestDismiss(reason) {
        control.escapeRequested()
        return true
    }

    function forceDismiss() {
        control.escapeRequested()
        return true
    }

    x: parent ? Math.round((parent.width - implicitWidth) / 2) : 0
    y: -implicitHeight - verticalGap
    z: PopupCoordinator.toolTipZ
    implicitWidth: Math.min(maximumWidth,
        Math.max(implicitBackgroundWidth + leftInset + rightInset,
                 implicitContentWidth + leftPadding + rightPadding))
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
        implicitContentHeight + topPadding + bottomPadding)
    margins: 8
    leftPadding: 12
    rightPadding: 12
    topPadding: 8
    bottomPadding: 8
    timeout: 5000
    font.family: Theme.fontForText(text)
    font.pixelSize: 12
    font.weight: Font.Medium

    onVisibleChanged: {
        if (coordinatePopupLifecycle) {
            if (visible)
                PopupCoordinator.registerPopup(control)
            else
                PopupCoordinator.unregisterPopup(control)
        }
        if (!visible) {
            toolTipText.focus = false
            toolTipText.deselect()
        }
    }
    onCoordinatePopupLifecycleChanged: {
        if (coordinatePopupLifecycle && visible)
            PopupCoordinator.registerPopup(control)
        else
            PopupCoordinator.unregisterPopup(control)
    }
    Component.onDestruction: PopupCoordinator.unregisterPopup(control)

    enter: Transition {
        ParallelAnimation {
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: 130
                easing.type: Easing.OutCubic
            }
            NumberAnimation {
                property: "scale"
                from: 0.96
                to: 1
                duration: 150
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
                duration: 90
                easing.type: Easing.InCubic
            }
            NumberAnimation {
                property: "scale"
                from: 1
                to: 0.98
                duration: 90
                easing.type: Easing.InCubic
            }
        }
    }

    contentItem: Item {
        implicitWidth: control.selectableText
                       ? toolTipText.implicitWidth : toolTipLabel.implicitWidth
        implicitHeight: control.selectableText
                        ? toolTipText.implicitHeight : toolTipLabel.implicitHeight

        Text {
            id: toolTipLabel

            anchors.fill: parent
            visible: !control.selectableText
            text: control.text
            color: Theme.text
            font: control.font
            wrapMode: Text.Wrap
            verticalAlignment: Text.AlignVCenter
            lineHeight: 1.08
            lineHeightMode: Text.ProportionalHeight
        }

        TextEdit {
            id: toolTipText

            anchors.fill: parent
            visible: control.selectableText
            text: control.text
            color: Theme.text
            font: control.font
            wrapMode: TextEdit.Wrap
            verticalAlignment: TextEdit.AlignVCenter
            readOnly: true
            selectByMouse: control.selectableText
            persistentSelection: true
            activeFocusOnPress: control.selectableText
            activeFocusOnTab: false
            selectionColor: Theme.accent
            selectedTextColor: Theme.text

            onActiveFocusChanged: {
                if (activeFocus)
                    PopupCoordinator.notePopupContentPress()
            }

            Keys.onEscapePressed: event => {
                control.escapeRequested()
                event.accepted = true
            }
        }

        HoverHandler {
            id: contentHoverHandler
            enabled: control.keepOpenOnHover
        }
    }

    background: Item {
        HoverHandler {
            id: backgroundHoverHandler
            enabled: control.keepOpenOnHover
        }

        Rectangle {
            x: 2
            y: 4
            width: parent.width
            height: parent.height
            radius: Theme.radiusSmall
            color: "#66000000"
        }

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusSmall
            color: "#F5262A34"
            border.width: 1
            border.color: "#46FFFFFF"
        }

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            width: Math.min(38, Math.max(0, parent.width - 24))
            height: 2
            radius: 1
            color: Theme.accent
            opacity: 0.72
        }
    }
}
