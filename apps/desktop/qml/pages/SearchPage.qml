import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami.Ui
import Yanami.Native

Item {
    id: root

    property string query: ""
    property string effectiveQuery: ""
    property bool resetScrollAfterFilter: false
    property bool developmentDiagnostics: false
    signal itemRequested(var item)
    signal playRequested(string itemId, string title)
    signal mediaContextRequested(var item, var sourceItem, real x, real y,
                                 bool keyboardInvocation)

    onQueryChanged: {
        if (searchField.text !== root.query)
            searchField.text = root.query
        root.resetScrollAfterFilter = true
        scheduleSearch()
    }
    MediaQueryProxyModel {
        id: animatedResultsModel
        sourceModel: app.home.mediaStore.libraryModel
        searchText: root.effectiveQuery
        requireSearchText: true
    }

    function scheduleSearch() {
        if (root.query.trim().length === 0) {
            searchTimer.stop()
            root.effectiveQuery = ""
            Qt.callLater(resultsGrid.resetScrollPosition)
            return
        }
        searchTimer.restart()
    }

    function resetSearch() {
        root.query = ""
        searchField.text = ""
        resultsGrid.resetScrollPosition()
        searchField.focusInput()
    }

    function focusSearch() {
        searchField.focusInput()
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
                        text: qsTr("Find movies and series by title")
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
                }

                AppButton {
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
                        : (animatedResultsModel.count > 0
                            ? qsTr("%1 results").arg(animatedResultsModel.count)
                            : qsTr("No matches"))
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                SmoothGridView {
                    id: resultsGrid
                    anchors.fill: parent
                    visible: opacity > 0
                    opacity: animatedResultsModel.count > 0 ? 1 : 0
                    enabled: opacity > 0.5
                    cellWidth: 202
                    cellHeight: 322
                    cacheBuffer: cellHeight
                    model: animatedResultsModel

                    Behavior on opacity { NumberAnimation { duration: 150 } }

                    delegate: PosterCard {
                        required property var modelData
                        required property int index
                        title: modelData.title
                        subtitle: LocaleText.mediaSubtitle(modelData)
                        itemType: modelData.itemType
                        posterUrl: modelData.imageUrl || ""
                        progress: modelData.progress || 0
                        unplayedCount: Number(modelData.unplayedCount || 0)
                        mediaItem: modelData
                        posterColor: ["#405E7B", "#6A536F", "#526B5D", "#755358", "#59647C"][Math.max(0, index) % 5]
                        onActivated: root.itemRequested(modelData)
                        onPlayRequested: root.playRequested(modelData.id, modelData.title)
                        onContextMenuRequested: (item, sourceItem, x, y,
                                                 keyboardInvocation) =>
                            root.mediaContextRequested(
                                item, sourceItem, x, y, keyboardInvocation)
                    }
                }

                GlassPanel {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 150
                    visible: opacity > 0
                    opacity: animatedResultsModel.count === 0 ? 1 : 0
                    radius: 24
                    color: "#80151920"

                    Behavior on opacity { NumberAnimation { duration: 150 } }

                    Column {
                        anchors.centerIn: parent
                        spacing: 8
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: root.query.trim().length === 0
                                ? qsTr("Start searching your Emby library")
                                : qsTr("Try another keyword")
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: root.query.trim().length === 0
                                ? qsTr("Matches titles and latest episode information instantly")
                                : qsTr("Try a shorter title or episode name")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 12
                        }
                    }
                }
            }
        }

    Timer {
        id: searchTimer
        // Coalesce edits from the same event-loop turn without imposing a
        // fixed delay on the small, locally cached library model.
        interval: 90
        onTriggered: {
            const started = Date.now()
            root.effectiveQuery = root.query.trim()
            if (root.developmentDiagnostics)
                console.info("search-filter items=", app.home.mediaStore.libraryModel.count,
                    "matches=", animatedResultsModel.count,
                    "elapsedMs=", Date.now() - started)
            if (root.resetScrollAfterFilter) {
                Qt.callLater(resultsGrid.resetScrollPosition)
                root.resetScrollAfterFilter = false
            }
        }
    }

    Connections {
        target: i18n
        function onLanguageChanged() { root.scheduleSearch() }
    }
}
