import QtQuick
import QtTest
import Yanami.Ui

Item {
    width: 320
    height: 120

    property int navigationActivations: 0
    property int playbackActivations: 0

    NavButton {
        id: navigationButton
        x: 20
        y: 20
        iconName: "settings"
        accessibleName: "Settings"
        onClicked: ++navigationActivations
    }

    MediaPlayButton {
        id: playbackButton
        x: 100
        y: 20
        onClicked: ++playbackActivations
    }

    TestCase {
        name: "ControllerActivation"
        when: windowShown

        function init() {
            navigationActivations = 0
            playbackActivations = 0
            InputModality.noteControllerNavigation()
        }

        function test_return_activates_navigation_button_once() {
            navigationButton.forceActiveFocus(Qt.TabFocusReason)
            tryCompare(navigationButton, "activeFocus", true)
            keyClick(Qt.Key_Return)
            compare(navigationActivations, 1)
        }

        function test_return_activates_playback_button_once() {
            playbackButton.forceActiveFocus(Qt.TabFocusReason)
            tryCompare(playbackButton, "activeFocus", true)
            keyClick(Qt.Key_Return)
            compare(playbackActivations, 1)
        }
    }
}
