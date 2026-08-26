import QtQuick
import QtTest
import Yanami.Ui

Item {
    width: 560
    height: 180

    property int rightCalls: 0

    AppTextField {
        id: input
        x: 20
        y: 20
        width: 320
        label: "Search"
        controllerRightHandler: function() {
            ++rightCalls
            destination.forceActiveFocus(Qt.TabFocusReason)
            return true
        }
    }

    Rectangle {
        id: destination
        x: 380
        y: 52
        width: 120
        height: 52
        activeFocusOnTab: true
    }

    TestCase {
        name: "AppTextFieldController"
        when: windowShown

        function init() {
            rightCalls = 0
            input.controllerRightHandler = function() {
                ++rightCalls
                destination.forceActiveFocus(Qt.TabFocusReason)
                return true
            }
            input.text = "Yanami"
            InputModality.noteKeyboardNavigation()
            input.focusInput()
            tryCompare(input, "inputActiveFocus", true)
        }

        function test_keyboardArrowKeepsEditingText() {
            input.focusTarget.deselect()
            input.focusTarget.cursorPosition = 0
            keyClick(Qt.Key_Right)
            compare(rightCalls, 0)
            compare(input.focusTarget.cursorPosition, 1)
            verify(input.inputActiveFocus)
        }

        function test_controllerArrowUsesExplicitEscapeRoute() {
            InputModality.noteControllerNavigation()
            // QuickTest keyClick is intentionally classified as a real
            // keyboard event by the process-wide modality filter. Exercise
            // the controller-only branch directly; the architecture test
            // separately pins its Keys.BeforeItem wiring on the inner field.
            verify(input.handleControllerDirection(Qt.Key_Right))
            compare(rightCalls, 1)
            tryCompare(destination, "activeFocus", true)
        }

        function test_controllerArrowFallsBackToFocusChain() {
            input.controllerRightHandler = null
            InputModality.noteControllerNavigation()
            verify(input.handleControllerDirection(Qt.Key_Right))
            tryCompare(destination, "activeFocus", true)
        }

        function test_controllerContextClearsWithoutTyping() {
            InputModality.noteControllerNavigation()
            verify(input.handleControllerCommand(Qt.Key_Menu))
            compare(input.text, "")
        }
    }
}
