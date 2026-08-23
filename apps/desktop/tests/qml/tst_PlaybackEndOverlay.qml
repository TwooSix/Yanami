import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    id: testCase

    name: "PlaybackEndOverlay"
    width: 720
    height: 520
    visible: true
    when: windowShown

    property int backgroundPressCount: 0

    Item {
        id: backgroundSurface
        anchors.fill: parent
        focus: true

        MouseArea {
            anchors.fill: parent
            onClicked: ++testCase.backgroundPressCount
        }
    }

    PlaybackEndOverlay {
        id: overlay
        anchors.fill: parent
        heading: "Playback complete"
        detail: "Episode 12"
    }

    SignalSpy {
        id: doneSpy
        target: overlay
        signalName: "doneRequested"
    }

    SignalSpy {
        id: replaySpy
        target: overlay
        signalName: "replayRequested"
    }

    SignalSpy {
        id: retrySpy
        target: overlay
        signalName: "retryRequested"
    }

    function button(name) {
        const result = findChild(overlay, name)
        verify(result !== null, "missing " + name)
        return result
    }

    function init() {
        overlay.shown = false
        overlay.retryMode = false
        overlay.heading = "Playback complete"
        overlay.detail = "Episode 12"
        doneSpy.clear()
        replaySpy.clear()
        retrySpy.clear()
        testCase.backgroundPressCount = 0
        InputModality.notePointerInput()
        backgroundSurface.forceActiveFocus(Qt.MouseFocusReason)
        tryCompare(overlay, "opacity", 0)
    }

    function cleanup() {
        init()
    }

    function test_pointerOpeningBlocksContentWithoutFocusRing() {
        const done = button("playbackEndDoneButton")
        const replay = button("playbackEndReplayButton")

        overlay.shown = true
        tryCompare(overlay, "opacity", 1)
        compare(InputModality.focusNavigationActive, false)
        tryCompare(done, "activeFocus", false)
        compare(done.visualFocus, false)

        mouseClick(testCase, 20, 20)
        compare(testCase.backgroundPressCount, 0)

        mouseClick(replay, replay.width / 2, replay.height / 2)
        compare(replaySpy.count, 1)
        compare(doneSpy.count, 0)
    }

    function test_keyboardOpeningFocusesDoneAndTrapsTab() {
        const done = button("playbackEndDoneButton")
        const replay = button("playbackEndReplayButton")

        InputModality.noteKeyboardNavigation()
        overlay.shown = true
        tryCompare(done, "activeFocus", true)
        compare(done.kind, "primary")
        tryCompare(done, "visualFocus", true)

        keyClick(Qt.Key_Tab)
        tryCompare(replay, "activeFocus", true)
        keyClick(Qt.Key_Tab)
        tryCompare(done, "activeFocus", true)

        keyClick(Qt.Key_Return)
        compare(doneSpy.count, 1)
    }

    function test_retryModeFocusesRetryAndKeepsDoneSecondary() {
        const done = button("playbackEndDoneButton")
        const replay = button("playbackEndReplayButton")
        const retry = button("playbackEndRetryButton")

        overlay.retryMode = true
        InputModality.noteControllerNavigation()
        overlay.shown = true
        tryCompare(retry, "activeFocus", true)
        compare(replay.visible, false)
        compare(done.kind, "secondary")
        compare(retry.kind, "primary")

        keyClick(Qt.Key_Return)
        compare(retrySpy.count, 1)
        compare(doneSpy.count, 0)
    }

    function test_pointerTakesFocusRingBackFromNavigation() {
        const done = button("playbackEndDoneButton")

        InputModality.noteKeyboardNavigation()
        overlay.shown = true
        tryCompare(done, "activeFocus", true)
        InputModality.notePointerInput()
        tryCompare(done, "activeFocus", false)
        compare(done.visualFocus, false)
        compare(overlay.activeFocus, true)
    }

    function test_firstKeyboardConfirmFromPointerActivatesDefault() {
        InputModality.notePointerInput()
        overlay.shown = true
        tryCompare(overlay, "activeFocus", true)

        keyClick(Qt.Key_Return)

        tryCompare(doneSpy, "count", 1)
        compare(replaySpy.count, 0)
    }
}
