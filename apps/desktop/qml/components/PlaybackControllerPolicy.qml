import QtQuick

QtObject {
    id: root

    enum Mode {
        Ambient,
        ControlBar
    }

    property bool available: true
    property int mode: PlaybackControllerPolicy.Ambient
    readonly property bool controlBarFocusMode:
        mode === PlaybackControllerPolicy.ControlBar
    readonly property bool activateTargetsPlayback: !controlBarFocusMode

    function enterControlBar() {
        if (!root.available)
            return false
        root.mode = PlaybackControllerPolicy.ControlBar
        return true
    }

    function exitControlBar() {
        if (!root.controlBarFocusMode)
            return false
        root.mode = PlaybackControllerPolicy.Ambient
        return true
    }

    function handleMenu() {
        return root.controlBarFocusMode
            ? root.exitControlBar() : root.enterControlBar()
    }

    function handleBack() {
        return root.exitControlBar()
    }

    function handlePointerTakeover() {
        return root.exitControlBar()
    }

    function reset() {
        root.mode = PlaybackControllerPolicy.Ambient
    }
}
