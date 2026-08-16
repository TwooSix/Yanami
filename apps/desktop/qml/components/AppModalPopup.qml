pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

AppPopup {
    id: root

    property color scrimColor: "#76000000"
    property bool _insidePressObserved: false
    property bool _wasTopAtPress: false
    property int _chromePressSerialAtPress: 0
    property int _contentPressSerialAtPress: 0

    popupRole: PopupCoordinator.modalRole
    parent: Overlay.overlay
    modal: true
    dim: true
    managesOutsideDismissal: true

    function dismissFromOutside() {
        if (root.opened && root.dismissOnOutside
                && PopupCoordinator.isTop(root))
            root.requestDismiss("outside")
    }

    Overlay.modal: Rectangle { color: root.scrimColor }

    HoverHandler {
        id: contentHover
        parent: root.contentItem
        enabled: root.opened
    }

    HoverHandler {
        id: backgroundHover
        parent: root.background
        enabled: root.opened
    }

    // Overlay.pressed arrives before popup descendants. Give the original
    // event time to reach the popup surface/title bar, then classify it. This
    // avoids closing on buttons or blank content without intercepting input.
    TapHandler {
        parent: root.contentItem
        acceptedButtons: Qt.AllButtons
        onPressedChanged: {
            if (pressed)
                root._insidePressObserved = true
        }
    }

    TapHandler {
        parent: root.background
        acceptedButtons: Qt.AllButtons
        onPressedChanged: {
            if (pressed)
                root._insidePressObserved = true
        }
    }

    Timer {
        id: outsidePressClassifier

        interval: 50
        onTriggered: {
            const wasInside = contentHover.hovered || backgroundHover.hovered
                    || root._insidePressObserved
                    || root._contentPressSerialAtPress
                        !== PopupCoordinator.popupContentPressSerial
            const wasChrome = root._chromePressSerialAtPress
                    !== PopupCoordinator.applicationChromePressSerial
                    || Date.now()
                        - PopupCoordinator.applicationChromePressTimestamp < 150
            const wasTop = root._wasTopAtPress
            root._insidePressObserved = false
            root._wasTopAtPress = false
            if (wasTop && !wasInside && !wasChrome)
                root.dismissFromOutside()
        }
    }

    Overlay.onPressed: {
        root._insidePressObserved = false
        root._wasTopAtPress = PopupCoordinator.isTop(root)
        root._chromePressSerialAtPress = PopupCoordinator.applicationChromePressSerial
        root._contentPressSerialAtPress = PopupCoordinator.popupContentPressSerial
        outsidePressClassifier.restart()
    }
}
