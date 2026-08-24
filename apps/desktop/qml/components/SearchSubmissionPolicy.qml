import QtQuick

Item {
    id: root

    property string input: ""
    property bool composing: false
    property int delayMs: 100
    property bool pendingForce: false
    property bool hasSubmitted: false
    property string lastSubmitted: ""
    property bool divergedSinceSubmit: false
    readonly property bool pending: delay.running
    signal submitRequested(string query)

    function publish(query, force) {
        if (!force && root.hasSubmitted && query === root.lastSubmitted)
            return
        root.hasSubmitted = true
        root.lastSubmitted = query
        root.pendingForce = false
        root.divergedSinceSubmit = false
        root.submitRequested(query)
    }

    function schedule(force) {
        delay.stop()
        const normalized = root.input.trim()
        if (root.hasSubmitted && normalized !== root.lastSubmitted)
            root.divergedSinceSubmit = true
        const returnedToSubmitted = root.hasSubmitted
            && normalized === root.lastSubmitted
            && root.divergedSinceSubmit
        root.pendingForce = force === true || root.pendingForce
            || returnedToSubmitted
        if (normalized.length === 0) {
            root.publish("", root.pendingForce)
            return
        }
        if (root.composing) {
            return
        }
        delay.start()
    }

    function forceSchedule() {
        schedule(true)
    }

    function cancel() {
        delay.stop()
        root.pendingForce = false
        root.divergedSinceSubmit = false
    }

    onInputChanged: schedule()
    onComposingChanged: {
        if (composing)
            delay.stop()
        else
            schedule(root.pendingForce)
    }

    visible: false
    width: 0
    height: 0

    Timer {
        id: delay
        interval: root.delayMs
        onTriggered: root.publish(root.input.trim(), root.pendingForce)
    }
}
