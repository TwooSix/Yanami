import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

AppPopup {
    id: root

    popupRole: PopupCoordinator.transientRole
    modal: false
    dim: false
    exclusiveWithinScope: true
    // The Overlay classifier below handles outside dismissal without relying
    // on Qt close-policy values that are unsupported for modeless popups.
    managesOutsideDismissal: true
    margins: 8

    function dismissFromOutside() {
        if (root.opened && root.dismissOnOutside
                && PopupCoordinator.isTop(root))
            root.requestDismiss("outside")
    }

    function containsOverlayPoint(item, overlayItem, position) {
        if (!item || !overlayItem || item.visible === false)
            return false
        const local = item.mapFromItem(
            overlayItem, position.x, position.y)
        return local.x >= 0 && local.x <= item.width
            && local.y >= 0 && local.y <= item.height
    }

    function handleOverlayTap(overlayItem, position) {
        if (!root.opened || !overlayItem || !PopupCoordinator.isTop(root))
            return

        const inside = root.containsOverlayPoint(
                root.contentItem, overlayItem, position)
            || root.containsOverlayPoint(
                root.background, overlayItem, position)
        const applicationChrome = PopupCoordinator.applicationChromeAt(
            overlayItem, position)
        if (inside || applicationChrome)
            return

        // Qt's modeless Popup consumes the click that lands on another
        // registered control. Forward that one interaction explicitly so
        // switching a menu or toggling a control remains a single click.
        const clickTarget = PopupCoordinator.overlayClickTargetAt(
            overlayItem, position)
        if (clickTarget
                && typeof clickTarget.triggerFromOverlayClick === "function") {
            clickTarget.triggerFromOverlayClick()
            return
        }

        Qt.callLater(function() {
            if (root.opened)
                root.dismissFromOutside()
        })
    }

    // Observe release without taking an exclusive pointer grab. Modeless Popup
    // still consumes arbitrary outside clicks, while handleOverlayTap forwards
    // registered control activations explicitly.
    TapHandler {
        id: overlayPressHandler
        parent: root.Overlay.overlay
        enabled: root.opened && parent !== null
        target: null
        acceptedButtons: Qt.AllButtons
        gesturePolicy: TapHandler.DragThreshold
        onTapped: (eventPoint, button) => {
            const overlayItem = root.Overlay.overlay
            root.handleOverlayTap(overlayItem, eventPoint.position)
        }
    }
}
