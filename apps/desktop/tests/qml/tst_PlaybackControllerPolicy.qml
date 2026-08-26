import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    name: "PlaybackControllerPolicy"

    PlaybackControllerPolicy { id: policy }

    function init() {
        policy.available = true
        policy.reset()
    }

    function test_ambientActivateTargetsPlayback() {
        compare(policy.controlBarFocusMode, false)
        compare(policy.activateTargetsPlayback, true)
    }

    function test_menuTogglesControlBarAndBackExitsOneLayer() {
        compare(policy.handleMenu(), true)
        compare(policy.controlBarFocusMode, true)
        compare(policy.activateTargetsPlayback, false)

        compare(policy.handleBack(), true)
        compare(policy.controlBarFocusMode, false)
        // Ambient Back is deliberately left for the page host to navigate.
        compare(policy.handleBack(), false)

        compare(policy.handleMenu(), true)
        compare(policy.controlBarFocusMode, true)
        compare(policy.handleMenu(), true)
        compare(policy.controlBarFocusMode, false)
    }

    function test_unavailablePlayerCannotEnterControlBar() {
        policy.available = false
        compare(policy.enterControlBar(), false)
        compare(policy.controlBarFocusMode, false)
    }

    function test_pointerTakeoverReturnsToAmbientWithoutASecondToggle() {
        compare(policy.enterControlBar(), true)
        compare(policy.handlePointerTakeover(), true)
        compare(policy.controlBarFocusMode, false)
        compare(policy.activateTargetsPlayback, true)
        compare(policy.handlePointerTakeover(), false)
    }
}
