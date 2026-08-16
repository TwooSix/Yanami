import QtQuick
import Yanami.Ui

AppButton {
    id: control

    property var popupTarget: null
    property var peerPopups: []
    property bool _popupWasOpenOnPress: false
    property bool _suppressPhysicalClick: false
    property int _openRequestSerial: 0

    signal popupActivated()

    function closePeerPopups() {
        const peers = control.peerPopups || []
        for (let index = 0; index < peers.length; ++index) {
            const popup = peers[index]
            if (popup && popup !== control.popupTarget)
                popup.close()
        }
    }

    function togglePopupFromTrigger(keyboardInvocation) {
        const popup = control.popupTarget
        if (!popup)
            return

        ++control._openRequestSerial
        popup.focusReturnTarget = control
        control.closePeerPopups()
        if (control._popupWasOpenOnPress) {
            popup.close()
            return
        }

        const requestSerial = control._openRequestSerial
        // Opening on the next event-loop turn keeps the new popup out of the
        // outside-release event that dismissed the previous popup.
        Qt.callLater(function() {
            if (requestSerial !== control._openRequestSerial
                    || !control.enabled || !control.visible)
                return
            if (typeof popup.openPreferred === "function")
                popup.openPreferred(Boolean(keyboardInvocation))
            else
                popup.open()
        })
    }

    function activatePopupTrigger(keyboardInvocation) {
        control.popupActivated()
        control.togglePopupFromTrigger(Boolean(keyboardInvocation))
    }

    function triggerFromOverlayClick() {
        control._popupWasOpenOnPress = Boolean(
            control.popupTarget && control.popupTarget.opened)
        control._suppressPhysicalClick = true
        control.activatePopupTrigger(false)
        Qt.callLater(function() {
            control._suppressPhysicalClick = false
        })
    }

    checkable: false
    checked: Boolean(control.popupTarget && control.popupTarget.opened)

    Connections {
        target: control

        function onPressedChanged() {
            if (control.pressed)
                control._popupWasOpenOnPress = Boolean(
                    control.popupTarget && control.popupTarget.opened)
        }

        function onClicked() {
            if (!control._suppressPhysicalClick)
                control.activatePopupTrigger(
                    InputModality.focusNavigationActive)
        }
    }

    Component.onCompleted:
        PopupCoordinator.registerOverlayClickTarget(control)
    Component.onDestruction: {
        ++control._openRequestSerial
        PopupCoordinator.unregisterOverlayClickTarget(control)
    }
}
