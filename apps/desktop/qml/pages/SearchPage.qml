import QtQuick
import QtQuick.Layouts
import Yanami.Ui
import Yanami.Native

Item {
    id: root

    property string query: ""
    property string effectiveQuery: ""
    readonly property double sessionGeneration: Number(app.session.generation)
    property bool sessionObserverReady: false
    property bool developmentDiagnostics: false
    property string focusedResultSection: ""
    property string focusedResultId: ""
    property string pendingFocusSection: ""
    property int pendingFocusIndex: -1
    property int pendingFocusAttempts: 0
    property bool resultKeyboardFocusArmed: false
    property bool suppressNextScrollAnchor: false
    property Item controllerExitTarget: null
    property string scrollAnchorSection: ""
    property string scrollAnchorItemId: ""
    property string scrollAnchorRowKey: ""
    property real scrollAnchorOffset: 0
    readonly property bool hostWindowActive: !root.Window.window
        || root.Window.window.active
    signal itemRequested(var item)
    signal playRequested(string itemId, string title, var playbackContext)
    signal mediaContextRequested(var item, var sourceItem, real x, real y,
                                 bool keyboardInvocation)

    onQueryChanged: {
        if (searchField.text !== root.query)
            searchField.text = root.query
        if (root.query.trim() !== root.effectiveQuery)
            app.search.inputPending()
    }
    onVisibleChanged: {
        if (visible)
            refreshCatalogRevision()
        else {
            root.cancelResultFocusRestore()
            root.clearScrollAnchor()
        }
    }
    onSessionGenerationChanged: {
        if (!root.sessionObserverReady)
            return
        searchSubmission.cancel()
        root.effectiveQuery = ""
        root.query = ""
        searchField.text = ""
        Qt.callLater(root.resetResultsPosition)
    }
    function scheduleSearch(force) {
        searchSubmission.schedule(force === true)
    }

    function submitSearch(query) {
        const changed = root.effectiveQuery !== query
        root.effectiveQuery = query
        if (changed) {
            root.suppressNextScrollAnchor = true
            scrollAnchorSuppressionTimeout.restart()
            root.resetResultsPosition()
        }
        app.search.submit(query)
        if (root.developmentDiagnostics)
            console.info("catalog-search-submit cached=", app.search.cachedCount,
                "queryLength=", query.length)
    }

    function resetSearch() {
        root.query = ""
        searchField.text = ""
        root.resetResultsPosition()
        searchField.focusInput()
    }

    function refreshCatalogRevision() {
        if (!root.visible || !root.hostWindowActive
                || root.effectiveQuery.length === 0
                || app.search.searching || searchSubmission.pending
                || searchField.inputMethodComposing) {
            return
        }
        app.search.refresh()
    }

    function resetResultsPosition() {
        root.cancelResultFocusRestore()
        root.focusedResultSection = ""
        root.focusedResultId = ""
        root.clearScrollAnchor()
        resultsList.resetScrollPosition()
    }

    function cancelResultFocusRestore() {
        focusRetry.stop()
        root.pendingFocusSection = ""
        root.pendingFocusIndex = -1
        root.resultKeyboardFocusArmed = false
    }

    function clearScrollAnchor() {
        root.scrollAnchorSection = ""
        root.scrollAnchorItemId = ""
        root.scrollAnchorRowKey = ""
        root.scrollAnchorOffset = 0
    }

    function captureScrollAnchor() {
        root.clearScrollAnchor()
        if (!root.visible || !root.hostWindowActive
                || root.resultKeyboardFocusArmed || resultsList.count <= 0) {
            return
        }
        const visualRow = resultsList.indexAt(
            1, resultsList.contentY + 1)
        const rowItem = visualRow >= 0
            ? resultsList.itemAtIndex(visualRow) : null
        if (!rowItem)
            return
        root.scrollAnchorSection = rowItem.mediaSection
        root.scrollAnchorRowKey = rowItem.rowKey
        root.scrollAnchorOffset = rowItem.y - resultsList.contentY
        if (rowItem.rowType === "cards" && rowItem.items
                && rowItem.items.length > 0) {
            root.scrollAnchorItemId = String(
                rowItem.items[0].id || "")
        }
    }

    function restoreScrollAnchor() {
        let visualRow = -1
        if (root.scrollAnchorItemId.length > 0) {
            visualRow = app.search.resultRows.rowForId(
                root.scrollAnchorSection, root.scrollAnchorItemId)
        }
        if (visualRow < 0 && root.scrollAnchorRowKey.length > 0)
            visualRow = app.search.resultRows.rowForKey(root.scrollAnchorRowKey)
        if (visualRow < 0) {
            root.clearScrollAnchor()
            return
        }
        const savedOffset = root.scrollAnchorOffset
        resultsList.positionViewAtIndex(visualRow, ListView.Beginning)
        Qt.callLater(function() {
            const rowItem = resultsList.itemAtIndex(visualRow)
            if (rowItem)
                resultsList.restoreScrollPosition(rowItem.y - savedOffset)
            root.clearScrollAnchor()
        })
    }

    function revealResultCard(card) {
        if (!card || !resultsList.visible)
            return
        const point = card.mapToItem(resultsList.contentItem, 0, 0)
        const margin = 10
        const viewportTop = resultsList.contentY
        const viewportBottom = viewportTop + resultsList.height
        if (point.y < viewportTop + margin)
            resultsList.revealContentY(point.y - margin)
        else if (point.y + card.height > viewportBottom - margin)
            resultsList.revealContentY(
                point.y + card.height - resultsList.height + margin)
    }

    function resultModel(section) {
        return section === "titles"
            ? app.search.titleResults : app.search.episodeResults
    }

    function focusResultCard(section, index) {
        const sourceModel = root.resultModel(section)
        if (!sourceModel || index < 0 || index >= sourceModel.count)
            return false
        const visualRow = app.search.resultRows.rowFor(section, index)
        if (visualRow < 0)
            return false
        root.pendingFocusSection = section
        root.pendingFocusIndex = index
        root.pendingFocusAttempts = 0
        root.resultKeyboardFocusArmed = InputModality.focusNavigationActive
        resultsList.currentIndex = visualRow
        resultsList.positionViewAtIndex(visualRow, ListView.Contain)
        focusRetry.restart()
        return true
    }

    function tryPendingResultFocus() {
        if (!root.visible || !root.hostWindowActive
                || !root.resultKeyboardFocusArmed
                || !InputModality.focusNavigationActive
                || root.pendingFocusSection.length === 0
                || root.pendingFocusIndex < 0) {
            focusRetry.stop()
            return
        }
        const visualRow = app.search.resultRows.rowFor(
            root.pendingFocusSection, root.pendingFocusIndex)
        const visualColumn = app.search.resultRows.columnFor(
            root.pendingFocusSection, root.pendingFocusIndex)
        const rowItem = visualRow >= 0
            ? resultsList.itemAtIndex(visualRow) : null
        const card = rowItem && visualColumn >= 0
            ? rowItem.cardAt(visualColumn) : null
        if (card) {
            focusRetry.stop()
            root.pendingFocusSection = ""
            root.pendingFocusIndex = -1
            card.forceActiveFocus(Qt.TabFocusReason)
            root.revealResultCard(card)
            return
        }
        root.pendingFocusAttempts += 1
        if (root.pendingFocusAttempts >= 24) {
            focusRetry.stop()
            root.pendingFocusSection = ""
            root.pendingFocusIndex = -1
        }
    }

    function moveResultFocus(section, index, horizontal, vertical, event) {
        const sourceModel = root.resultModel(section)
        const columns = resultsList.columns
        if (!sourceModel || columns <= 0)
            return
        let targetIndex = -1
        if (horizontal !== 0) {
            const rowStart = Math.floor(index / columns) * columns
            const rowEnd = Math.min(
                sourceModel.count, rowStart + columns) - 1
            const candidate = index + horizontal
            if (candidate >= rowStart && candidate <= rowEnd)
                targetIndex = candidate
        } else {
            const candidate = index + vertical * columns
            if (candidate >= 0 && candidate < sourceModel.count) {
                targetIndex = candidate
            } else if (vertical > 0 && section === "titles"
                       && app.search.episodeResults.count > 0) {
                targetIndex = Math.min(
                    index % columns, app.search.episodeResults.count - 1)
                event.accepted = root.focusResultCard(
                    "episodes", targetIndex)
                return
            } else if (vertical < 0 && section === "episodes"
                       && app.search.titleResults.count > 0) {
                const lastRow = Math.floor(
                    (app.search.titleResults.count - 1) / columns) * columns
                targetIndex = Math.min(
                    lastRow + index % columns,
                    app.search.titleResults.count - 1)
                event.accepted = root.focusResultCard("titles", targetIndex)
                return
            }
        }
        event.accepted = targetIndex >= 0
            && root.focusResultCard(section, targetIndex)
    }

    function focusSearch() {
        searchField.focusInput()
    }

    function controllerDefaultFocusItem() {
        return searchField.focusTarget
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.rightMargin: 14
        spacing: 22

            RowLayout {
                Layout.fillWidth: true

                Column {
                    spacing: 3
                    Text {
                        text: qsTr("Search")
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 32
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: qsTr("Find movies, series, seasons, and episodes")
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 13
                    }
                }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                AppTextField {
                    id: searchField
                    Layout.fillWidth: true
                    Layout.maximumWidth: 680
                    label: qsTr("Search library")
                    placeholderText: qsTr("Enter a title, episode, or season")
                    onTextChanged: root.query = text
                    controllerLeftHandler: function() {
                        if (!root.controllerExitTarget
                                || !root.controllerExitTarget.visible
                                || !root.controllerExitTarget.enabled) {
                            return false
                        }
                        root.controllerExitTarget.forceActiveFocus(
                            Qt.TabFocusReason)
                        return true
                    }
                    controllerDownHandler: function() {
                        return app.search.titleResults.count > 0
                            ? root.focusResultCard("titles", 0)
                            : root.focusResultCard("episodes", 0)
                    }
                    controllerRightHandler: function() {
                        if (!clearButton.enabled)
                            return false
                        clearButton.forceActiveFocus(Qt.TabFocusReason)
                        return true
                    }
                }

                AppButton {
                    id: clearButton
                    visible: true
                    opacity: root.query.length > 0 ? 1 : 0
                    enabled: root.query.length > 0
                    Layout.alignment: Qt.AlignBottom
                    Layout.bottomMargin: 1
                    kind: "ghost"
                    text: qsTr("Clear")
                    onClicked: root.resetSearch()

                    Behavior on opacity { NumberAnimation { duration: 130 } }
                }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true

                Text {
                    text: root.query.trim().length === 0 ? qsTr("Search results")
                        : (searchSubmission.pending || app.search.searching
                            ? qsTr("Searching…")
                        : (app.search.results.count > 0
                            ? (app.search.hasMore
                                ? qsTr("Showing first %1 results")
                                    .arg(app.search.results.count)
                                : (app.search.totalMatches > app.search.results.count
                                ? qsTr("Showing %1 of %2 results")
                                    .arg(app.search.results.count)
                                    .arg(app.search.totalMatches)
                                : qsTr("%1 results").arg(app.search.totalMatches)))
                            : qsTr("No matches")))
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                visible: text.length > 0
                text: {
                    if (app.search.error.length > 0) {
                        return app.search.cachedCount > 0
                            ? qsTr("Searching %1 cached items. %2")
                                .arg(app.search.cachedCount).arg(app.search.error)
                            : app.search.error
                    }
                    if (app.search.syncing) {
                        return app.search.totalCount > 0
                            ? qsTr("Searching %1 cached items while indexing continues (%1 of %2).")
                                .arg(app.search.cachedCount).arg(app.search.totalCount)
                            : qsTr("Searching %1 cached items while indexing continues.")
                                .arg(app.search.cachedCount)
                    }
                    if (!app.search.complete && app.search.cachedCount > 0) {
                        return qsTr("Searching %1 cached items; the catalog index is not complete yet.")
                            .arg(app.search.cachedCount)
                    }
                    return ""
                }
                color: app.search.error.length > 0 ? Theme.danger : Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                SmoothListView {
                    id: resultsList
                    readonly property int cellWidth: 202
                    readonly property int cardWidth: 178
                    readonly property int cardRowHeight: 322
                    readonly property int columns: Math.max(
                        1, Math.floor(width / cellWidth))

                    anchors.fill: parent
                    anchors.rightMargin: 14
                    visible: app.search.results.count > 0
                    enabled: visible
                    model: app.search.resultRows
                    cacheBuffer: cardRowHeight
                    keyNavigationEnabled: false
                    currentIndex: -1

                    onColumnsChanged: {
                        if (app.search.resultRows)
                            app.search.resultRows.columns = columns
                    }
                    Component.onCompleted: {
                        if (app.search.resultRows)
                            app.search.resultRows.columns = columns
                    }

                    delegate: Item {
                        id: resultRow
                        required property int index
                        required property string rowType
                        required property string mediaSection
                        required property var items
                        required property int startIndex
                        required property int itemCount
                        required property string rowKey

                        function cardAt(column) {
                            return cardRepeater.itemAt(column)
                        }

                        width: resultsList.width
                        height: rowType === "header"
                            ? (mediaSection === "episodes"
                                && app.search.titleResults.count > 0 ? 56 : 34)
                            : resultsList.cardRowHeight

                        RowLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 12
                            visible: resultRow.rowType === "header"

                            Text {
                                text: resultRow.mediaSection === "titles"
                                    ? qsTr("Series") + " · " + qsTr("Movies")
                                    : qsTr("Episodes")
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: qsTr("%1 results").arg(
                                    resultRow.mediaSection === "titles"
                                        ? app.search.titleResults.count
                                        : app.search.episodeResults.count)
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 12
                            }
                        }

                        Repeater {
                            id: cardRepeater
                            model: resultRow.rowType === "cards"
                                ? resultRow.items : []

                            delegate: PosterCard {
                                id: resultCard
                                required property var modelData
                                required property int index
                                readonly property int sourceIndex:
                                    resultRow.startIndex + index

                                x: index * resultsList.cellWidth
                                width: resultsList.cardWidth
                                height: 302
                                title: resultRow.mediaSection === "titles"
                                    ? String(modelData.title || "")
                                    : String(modelData.seriesTitle
                                             || modelData.title || "")
                                subtitle: LocaleText.mediaSubtitle(modelData)
                                itemType: modelData.itemType
                                posterUrl: modelData.imageUrl || ""
                                progress: modelData.progress || 0
                                unplayedCount: Number(
                                    modelData.unplayedCount || 0)
                                mediaItem: modelData
                                posterColor: ["#405E7B", "#6A536F",
                                    "#526B5D", "#755358", "#59647C"]
                                    [Math.max(0, sourceIndex) % 5]
                                onActivated: root.itemRequested(modelData)
                                onPlayRequested: root.playRequested(
                                    modelData.id, modelData.title,
                                    modelData.playbackContext || ({}))
                                onContextMenuRequested: (item, sourceItem, x, y,
                                                         keyboardInvocation) =>
                                    root.mediaContextRequested(
                                        item, sourceItem, x, y,
                                        keyboardInvocation)
                                onActiveFocusChanged: {
                                    if (activeFocus) {
                                        root.resultKeyboardFocusArmed =
                                            InputModality.focusNavigationActive
                                        root.focusedResultSection =
                                            resultRow.mediaSection
                                        root.focusedResultId = String(
                                            modelData.id || "")
                                        resultsList.currentIndex = resultRow.index
                                        root.revealResultCard(resultCard)
                                    }
                                }
                                Keys.onLeftPressed: event =>
                                    root.moveResultFocus(
                                        resultRow.mediaSection, sourceIndex,
                                        -1, 0, event)
                                Keys.onRightPressed: event =>
                                    root.moveResultFocus(
                                        resultRow.mediaSection, sourceIndex,
                                        1, 0, event)
                                Keys.onUpPressed: event =>
                                    root.moveResultFocus(
                                        resultRow.mediaSection, sourceIndex,
                                        0, -1, event)
                                Keys.onDownPressed: event =>
                                    root.moveResultFocus(
                                        resultRow.mediaSection, sourceIndex,
                                        0, 1, event)
                            }
                        }
                    }

                    footer: Item {
                        width: resultsList.width
                        height: 12
                    }
                }

                GlassPanel {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 150
                    visible: app.search.results.count === 0
                    radius: 24
                    color: "#80151920"

                    Column {
                        anchors.centerIn: parent
                        spacing: 8
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: root.query.trim().length === 0
                                ? qsTr("Start searching your Emby library")
                                : (app.search.error.length > 0
                                    ? app.search.error
                                : (searchSubmission.pending || app.search.searching
                                    ? qsTr("Searching the cached catalog…")
                                    : (app.search.syncing && app.search.cachedCount === 0
                                        ? qsTr("Building your local catalog…")
                                        : qsTr("Try another keyword"))))
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: root.query.trim().length === 0
                                ? qsTr("Cached results appear instantly while the full catalog indexes in the background")
                                : (app.search.error.length > 0
                                    ? qsTr("The current keyword is preserved; results will update after search recovers.")
                                : (app.search.syncing
                                    ? qsTr("More matches can appear as indexing progresses")
                                    : qsTr("Try a shorter title, alias, or episode code")))
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 12
                        }
                    }
                }
            }
        }

    SearchSubmissionPolicy {
        id: searchSubmission
        input: root.query
        composing: searchField.inputMethodComposing
        delayMs: 100
        onSubmitRequested: query => root.submitSearch(query)
    }

    Timer {
        id: focusRetry
        interval: 16
        repeat: true
        onTriggered: root.tryPendingResultFocus()
    }

    Timer {
        id: scrollAnchorSuppressionTimeout
        interval: 1000
        onTriggered: root.suppressNextScrollAnchor = false
    }

    Timer {
        interval: 750
        repeat: true
        // Search is the one page that progressively exposes newly indexed
        // matches. The synchronous bridge has no revision push channel, so a
        // cheap empty-query status poll is scoped to an active visible search.
        running: root.visible && root.effectiveQuery.length > 0
            && root.hostWindowActive && !app.search.searching
            && !searchSubmission.pending && !searchField.inputMethodComposing
        onTriggered: root.refreshCatalogRevision()
    }

    Connections {
        target: root.Window.window
        function onActiveChanged() {
            if (root.hostWindowActive)
                root.refreshCatalogRevision()
        }
    }

    Connections {
        target: app.search.resultRows
        function onRowsAboutToBeRebuilt() {
            if (!root.suppressNextScrollAnchor)
                root.captureScrollAnchor()
        }
        function onRowsRebuilt() {
            if (root.suppressNextScrollAnchor) {
                root.suppressNextScrollAnchor = false
                scrollAnchorSuppressionTimeout.stop()
                root.clearScrollAnchor()
            } else {
                root.restoreScrollAnchor()
            }
            if (!root.visible || !root.hostWindowActive
                    || !root.resultKeyboardFocusArmed
                    || !InputModality.focusNavigationActive
                    || root.focusedResultSection.length === 0
                    || root.focusedResultId.length === 0) {
                return
            }
            const sourceIndex = app.search.resultRows.sourceIndexForId(
                root.focusedResultSection, root.focusedResultId)
            if (sourceIndex < 0) {
                root.focusedResultSection = ""
                root.focusedResultId = ""
                root.resultKeyboardFocusArmed = false
                searchField.focusInput()
                return
            }
            root.focusResultCard(root.focusedResultSection, sourceIndex)
        }
    }

    Connections {
        target: InputModality
        function onModalityChanged() {
            if (!InputModality.focusNavigationActive)
                root.cancelResultFocusRestore()
        }
    }

    Connections {
        target: i18n
        function onLanguageChanged() { root.scheduleSearch(true) }
    }

    Component.onCompleted: {
        root.sessionObserverReady = true
        scheduleSearch()
    }
}
