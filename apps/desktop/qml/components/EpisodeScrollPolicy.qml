import QtQuick

Item {
    id: root

    property string activeScopeId: ""
    property string requestedScopeId: ""
    property var model: null
    property var view: null
    property bool ready: false
    property bool refreshing: false
    property int generation: 0
    property int scheduledGeneration: -1
    readonly property bool pending: requestedScopeId.length > 0

    signal targetResolved(string scopeId, int index)

    visible: false
    width: 0
    height: 0

    function request(scopeId) {
        const normalizedId = String(scopeId || "")
        if (normalizedId.length === 0) {
            root.cancel()
            return
        }
        root.generation += 1
        root.requestedScopeId = normalizedId
        if (root.view
                && typeof root.view.resetScrollPosition === "function") {
            root.view.resetScrollPosition()
        }
        root.scheduleResolution()
    }

    function cancel() {
        root.generation += 1
        root.requestedScopeId = ""
        root.scheduledGeneration = -1
    }

    function notifyUserScroll() {
        if (root.pending)
            root.cancel()
    }

    function firstUnplayedIndex(sourceModel) {
        if (!sourceModel || typeof sourceModel.get !== "function")
            return -1
        const count = Math.max(0, Number(sourceModel.count || 0))
        for (let index = 0; index < count; ++index) {
            const item = sourceModel.get(index)
            if (item && !Boolean(item.played))
                return index
        }
        return -1
    }

    function targetIndex(sourceModel) {
        if (!sourceModel || typeof sourceModel.get !== "function")
            return -1
        const count = Math.max(0, Number(sourceModel.count || 0))
        if (count === 0)
            return -1
        const unplayedIndex = root.firstUnplayedIndex(sourceModel)
        return unplayedIndex >= 0 ? unplayedIndex : 0
    }

    function scheduleResolution() {
        if (!root.pending || root.scheduledGeneration === root.generation)
            return
        const expectedGeneration = root.generation
        root.scheduledGeneration = expectedGeneration
        Qt.callLater(function() {
            if (root.scheduledGeneration === expectedGeneration)
                root.scheduledGeneration = -1
            root.resolvePendingRequest(expectedGeneration)
        })
    }

    function positionTarget(expectedGeneration, index) {
        if (expectedGeneration !== root.generation || !root.view)
            return
        if (index < 0) {
            root.view.currentIndex = -1
            if (typeof root.view.resetScrollPosition === "function")
                root.view.resetScrollPosition()
            return
        }
        if (typeof root.view.forceLayout !== "function"
                || typeof root.view.positionViewAtIndex !== "function") {
            return
        }
        // Keep keyboard/controller navigation anchored to the episode that is
        // now visible; positionViewAtIndex() does not update currentIndex.
        root.view.currentIndex = index
        root.view.forceLayout()
        root.view.positionViewAtIndex(index, ListView.Beginning)
    }

    function resolvePendingRequest(expectedGeneration) {
        if (expectedGeneration !== root.generation)
            return
        const scopeId = root.requestedScopeId
        if (scopeId.length === 0
                || root.activeScopeId !== scopeId
                || !root.ready || root.refreshing
                || !root.model || typeof root.model.get !== "function") {
            return
        }
        // HomePage only marks a collection ready after MediaStore has committed
        // its rows and published the matching displayed ID. A zero count here
        // is therefore an authoritative empty season, not a loading placeholder.
        const index = root.targetIndex(root.model)
        root.requestedScopeId = ""
        root.positionTarget(expectedGeneration, index)
        root.targetResolved(scopeId, index)
    }

    onActiveScopeIdChanged: root.scheduleResolution()
    onModelChanged: root.scheduleResolution()
    onReadyChanged: root.scheduleResolution()
    onRefreshingChanged: root.scheduleResolution()

    Connections {
        target: root.model
        enabled: root.model !== null
        ignoreUnknownSignals: true

        function onCountChanged() { root.scheduleResolution() }
        function onRowsSynchronized() { root.scheduleResolution() }
        function onModelReset() { root.scheduleResolution() }
        function onLayoutChanged() { root.scheduleResolution() }
        function onDataChanged() { root.scheduleResolution() }
    }

    Connections {
        target: root.view
        enabled: root.view !== null
        ignoreUnknownSignals: true

        function onUserScrollStarted() { root.notifyUserScroll() }
    }
}
