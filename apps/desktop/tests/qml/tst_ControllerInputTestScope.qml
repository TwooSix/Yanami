import QtQuick
import QtTest
import Yanami.Ui

Item {
    width: 100
    height: 100

    Component {
        id: scopeFactory
        ControllerInputTestScope {}
    }

    TestCase {
        id: testCase
        name: "ControllerInputTestScope"
        when: windowShown

        property var scope: null

        function init() {
            verify(!InputModality.controllerInputTestActive)
            scope = scopeFactory.createObject(testCase)
            verify(scope !== null)
        }

        function cleanup() {
            if (scope) {
                scope.destroy()
                scope = null
            }
            tryCompare(InputModality, "controllerInputTestActive", false)
        }

        function test_explicitStartOwnsAndStopReleasesCapture() {
            compare(scope.acquired, false)
            compare(scope.running, false)
            compare(scope.start(), false)
            compare(InputModality.controllerInputTestActive, false)

            scope.available = true
            compare(scope.acquired, false)
            verify(scope.start())
            tryCompare(scope, "acquired", true)
            compare(scope.running, true)
            compare(InputModality.controllerInputTestActive, true)

            scope.stop()
            tryCompare(scope, "acquired", false)
            compare(scope.running, false)
            compare(InputModality.controllerInputTestActive, false)
        }

        function test_backExitsAndLeavingAvailabilityResetsSession() {
            scope.available = true
            verify(scope.start())
            compare(scope.handleAction(InputModality.Activate, false), false)
            compare(scope.handleAction(InputModality.Back, true), true)
            compare(scope.running, false)
            compare(scope.acquired, false)
            compare(InputModality.controllerInputTestActive, false)

            verify(scope.start())
            scope.available = false
            compare(scope.running, false)
            compare(scope.acquired, false)
            compare(InputModality.controllerInputTestActive, false)
        }

        function test_destructionReleasesRunningCapture() {
            scope.available = true
            verify(scope.start())
            tryCompare(scope, "acquired", true)
            compare(InputModality.controllerInputTestActive, true)

            scope.destroy()
            scope = null
            tryCompare(InputModality, "controllerInputTestActive", false)
        }
    }
}
