import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami

Item {
    id: root

    property var mediaItems: []
    property var libraryViews: []
    property var resumeItems: []
    property var recentItems: []
    property var collectionItems: []
    property var collectionParent: ({})
    property var seriesDetails: ({})
    property int depth: 0
    property int sortMode: 1
    property string libraryId: ""
    property string libraryTitle: ""
    property string seriesId: ""
    property string seriesTitle: ""
    property string seasonTitle: ""
    property var sortOptions: [
        { "id": 0, "label": qsTr("Name") },
        { "id": 1, "label": qsTr("Recently updated") },
        { "id": 2, "label": qsTr("Release date") },
        { "id": 3, "label": qsTr("Recently added") },
        { "id": 4, "label": qsTr("Unplayed first") }
    ]
    property var sortedCollectionItems: sortedItems(collectionItems)

    signal playRequested(string itemId, string title)
    signal settingsRequested()

    function sortLabel() {
        for (let index = 0; index < root.sortOptions.length; ++index) {
            if (root.sortOptions[index].id === root.sortMode)
                return root.sortOptions[index].label
        }
        return qsTr("Name")
    }

    function timeValue(value) {
        const parsed = Date.parse(value || "")
        return Number.isNaN(parsed) ? 0 : parsed
    }

    function sortedItems(items) {
        const result = []
        for (let index = 0; index < items.length; ++index)
            result.push(items[index])
        result.sort(function(first, second) {
            if (root.sortMode === 1)
                return root.timeValue(second.updatedAt) - root.timeValue(first.updatedAt)
            if (root.sortMode === 2) {
                const secondRelease = root.timeValue(second.releaseDate)
                    || Date.UTC(Number(second.productionYear || 0), 0, 1)
                const firstRelease = root.timeValue(first.releaseDate)
                    || Date.UTC(Number(first.productionYear || 0), 0, 1)
                return secondRelease - firstRelease
            }
            if (root.sortMode === 3)
                return root.timeValue(second.dateCreated) - root.timeValue(first.dateCreated)
            if (root.sortMode === 4) {
                const unreadDifference = Number(second.unplayedCount || 0) - Number(first.unplayedCount || 0)
                if (unreadDifference !== 0)
                    return unreadDifference
                return root.timeValue(second.updatedAt) - root.timeValue(first.updatedAt)
            }
            return String(first.title || "").localeCompare(
                String(second.title || ""), i18n.language === "zh_CN" ? "zh-CN" : "en")
        })
        return result
    }

    function goHome() {
        sortMenu.close()
        root.depth = 0
        root.libraryId = ""
        root.libraryTitle = ""
        root.seriesId = ""
        root.seriesTitle = ""
        root.seasonTitle = ""
        root.seriesDetails = ({})
        pageFlickable.contentY = 0
    }

    function openLibraryView(item) {
        root.libraryId = item.id
        root.libraryTitle = item.title
        root.seriesId = ""
        root.seriesTitle = ""
        root.seasonTitle = ""
        root.depth = 1
        pageFlickable.contentY = 0
        backend.loadCollection(item.id)
    }

    function openLibraryItem(item) {
        if (item.itemType === "Series") {
            root.seriesId = item.id
            root.seriesTitle = item.title
            root.seasonTitle = ""
            root.seriesDetails = ({})
            root.depth = 2
            pageFlickable.contentY = 0
            backend.loadCollection(item.id)
        } else {
            root.playRequested(item.id, item.title)
        }
    }

    function openSearchItem(item) {
        root.libraryId = ""
        root.libraryTitle = ""
        root.openLibraryItem(item)
    }

    function openSeason(item) {
        root.seriesDetails = root.collectionParent
        root.seasonTitle = item.title
        root.depth = 3
        pageFlickable.contentY = 0
        backend.loadCollection(item.id)
    }

    function goBack() {
        if (root.depth === 3) {
            root.depth = 2
            root.seasonTitle = ""
            pageFlickable.contentY = 0
            backend.loadCollection(root.seriesId)
        } else if (root.depth === 2 && root.libraryId.length > 0) {
            root.depth = 1
            root.seriesId = ""
            root.seriesTitle = ""
            pageFlickable.contentY = 0
            backend.loadCollection(root.libraryId)
        } else {
            root.goHome()
        }
    }

    SmoothFlickable {
        id: pageFlickable
        anchors.fill: parent
        contentHeight: content.implicitHeight + 48

        ColumnLayout {
            id: content
            width: parent.width - 14
            spacing: 24

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                AppButton {
                    visible: root.depth > 0
                    kind: "ghost"
                    iconOnly: true
                    iconName: "back"
                    controlSize: 42
                    onClicked: root.goBack()
                }

                Column {
                    spacing: 3
                    Text {
                        text: root.depth === 0 ? qsTr("Home")
                            : (root.depth === 1 ? root.libraryTitle
                            : (root.depth === 2 ? qsTr("Series details") : root.seriesTitle))
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 32
                        font.weight: Font.DemiBold
                    }
                    Text {
                        visible: root.depth > 0
                        text: root.depth === 1
                            ? (LocaleText.parentSubtitle(root.collectionParent) || qsTr("Library"))
                            : (root.depth === 2 ? root.seriesTitle : root.seasonTitle)
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 13
                    }
                }

                Item { Layout.fillWidth: true }

                AppButton {
                    visible: root.depth === 1
                    kind: "secondary"
                    text: qsTr("Sort · %1").arg(root.sortLabel())
                    onClicked: sortMenu.open()
                }

                AppButton {
                    visible: root.depth === 0
                    kind: "ghost"
                    iconName: "refresh"
                    text: backend.busy ? qsTr("Loading…") : qsTr("Refresh")
                    enabled: backend.embyConnected && !backend.busy
                    onClicked: backend.loadLibrary()
                }
            }

            ColumnLayout {
                visible: root.depth === 0
                Layout.fillWidth: true
                spacing: 12

                Text {
                    text: qsTr("All libraries")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }

                SmoothHorizontalList {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 158
                    model: root.libraryViews
                    delegate: LibraryCard {
                        required property var modelData
                        required property int index
                        width: 286
                        height: 148
                        title: modelData.title
                        subtitle: LocaleText.libraryTypeLabel(modelData.collectionType)
                        imageUrl: modelData.imageUrl || ""
                        fallbackColor: ["#405E7B", "#6A536F", "#526B5D", "#755358"][index % 4]
                        onActivated: root.openLibraryView(modelData)
                    }
                }

                Text {
                    visible: root.libraryViews.length === 0
                    text: backend.busy ? qsTr("Loading libraries…") : qsTr("No libraries to display")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 13
                }
            }

            ColumnLayout {
                visible: root.depth === 0
                Layout.fillWidth: true
                spacing: 12

                Text {
                    text: qsTr("Recent playback")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }

                SmoothHorizontalList {
                    visible: root.resumeItems.length > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 186 : 0
                    model: root.resumeItems
                    delegate: RecentEpisodeCard {
                        required property var modelData
                        title: modelData.title
                        subtitle: LocaleText.mediaSubtitle(modelData)
                        imageUrl: modelData.imageUrl || ""
                        progress: modelData.progress || 0
                        onPlayRequested: root.playRequested(modelData.id, modelData.title)
                    }
                }

                Text {
                    visible: root.resumeItems.length === 0
                    text: backend.busy ? qsTr("Loading playback history…") : qsTr("Nothing to resume yet")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 13
                }
            }

            ColumnLayout {
                visible: root.depth === 0
                Layout.fillWidth: true
                spacing: 12

                Text {
                    text: qsTr("Recent updates")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }

                SmoothHorizontalList {
                    visible: root.recentItems.length > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 186 : 0
                    model: root.recentItems
                    delegate: RecentEpisodeCard {
                        required property var modelData
                        title: modelData.title
                        subtitle: LocaleText.mediaSubtitle(modelData)
                        imageUrl: modelData.imageUrl || ""
                        progress: modelData.progress || 0
                        onPlayRequested: root.playRequested(modelData.id, modelData.title)
                    }
                }

                Text {
                    visible: root.recentItems.length === 0
                    text: backend.busy ? qsTr("Loading recent updates…") : qsTr("No recent updates")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 13
                }
            }

            RowLayout {
                visible: root.depth === 1
                Layout.fillWidth: true

                Text {
                    text: root.collectionItems.length > 0 ? qsTr("All titles")
                        : (backend.busy ? qsTr("Loading library…") : qsTr("No titles in this library"))
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                Text {
                    visible: root.collectionItems.length > 0
                    text: qsTr("%1 titles · %2").arg(root.collectionItems.length).arg(root.sortLabel())
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 12
                }
            }

            GridView {
                visible: root.depth === 1
                Layout.fillWidth: true
                Layout.preferredHeight: visible
                    ? Math.max(310, Math.ceil(root.sortedCollectionItems.length / Math.max(1, Math.floor(width / cellWidth))) * cellHeight)
                    : 0
                cellWidth: 202
                cellHeight: 322
                interactive: false
                model: root.sortedCollectionItems
                delegate: PosterCard {
                    required property var modelData
                    required property int index
                    title: modelData.title
                    subtitle: LocaleText.mediaSubtitle(modelData)
                    itemType: modelData.itemType
                    posterUrl: modelData.imageUrl || ""
                    progress: modelData.progress || 0
                    unplayedCount: Number(modelData.unplayedCount || 0)
                    posterColor: ["#405E7B", "#6A536F", "#526B5D", "#755358", "#59647C", "#6B6250"][index % 6]
                    onActivated: root.openLibraryItem(modelData)
                    onPlayRequested: root.playRequested(modelData.id, modelData.title)
                }
            }

            DetailHero {
                visible: root.depth >= 2
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 292 : 0
                eyebrow: root.depth === 2 ? qsTr("Series") : root.seasonTitle
                title: root.depth === 2
                    ? (root.collectionParent.title || root.seriesTitle)
                    : root.seriesTitle
                subtitle: LocaleText.parentSubtitle(root.collectionParent)
                overview: root.collectionParent.overview || root.seriesDetails.overview || ""
                continueLabel: root.collectionParent.continueLabel || ""
                posterUrl: root.collectionParent.imageUrl || root.seriesDetails.imageUrl || ""
                backdropUrl: root.collectionParent.backdropUrl || root.seriesDetails.backdropUrl || ""
                onPlayRequested: {
                    const itemId = root.collectionParent.id || root.seriesId
                    root.playRequested(itemId, root.seriesTitle)
                }
            }

            RowLayout {
                visible: root.depth === 2
                Layout.fillWidth: true

                Text {
                    text: root.collectionItems.length > 0 ? qsTr("Seasons and specials")
                        : (backend.busy ? qsTr("Loading seasons…") : qsTr("No seasons to display"))
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                Text {
                    visible: root.collectionItems.length > 0
                    text: qsTr("%1 items").arg(root.collectionItems.length)
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 12
                }
            }

            SmoothHorizontalList {
                visible: root.depth === 2
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 312 : 0
                spacing: 18
                model: root.collectionItems
                delegate: PosterCard {
                    required property var modelData
                    required property int index
                    width: 178
                    height: 302
                    title: modelData.title
                    subtitle: LocaleText.mediaSubtitle(modelData)
                    itemType: modelData.itemType
                    posterUrl: modelData.imageUrl || ""
                    progress: modelData.progress || 0
                    unplayedCount: Number(modelData.unplayedCount || 0)
                    posterColor: ["#405E7B", "#6A536F", "#526B5D", "#755358"][index % 4]
                    onActivated: root.openSeason(modelData)
                    onPlayRequested: root.playRequested(modelData.id, modelData.title)
                }
            }

            RowLayout {
                visible: root.depth === 3
                Layout.fillWidth: true

                Text {
                    text: root.collectionItems.length > 0 ? qsTr("Episodes")
                        : (backend.busy ? qsTr("Loading episodes…") : qsTr("No playable episodes in this season"))
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                Text {
                    visible: root.collectionItems.length > 0
                    text: qsTr("%1 episodes · Horizontal browsing").arg(root.collectionItems.length)
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 12
                }
            }

            SmoothHorizontalList {
                visible: root.depth === 3
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 274 : 0
                spacing: 18
                model: root.collectionItems
                delegate: EpisodeCard {
                    required property var modelData
                    width: 312
                    height: 264
                    title: modelData.title
                    subtitle: LocaleText.mediaSubtitle(modelData)
                    overview: modelData.overview || ""
                    imageUrl: modelData.imageUrl || ""
                    progress: modelData.progress || 0
                    onPlayRequested: root.playRequested(modelData.id, modelData.title)
                }
            }
        }
    }

    TrackMenu {
        id: sortMenu
        parent: root
        x: Math.max(0, root.width - width - 22)
        y: 54
        heading: qsTr("Library sorting")
        tracks: root.sortOptions
        selectedId: root.sortMode
        onTrackSelected: trackId => {
            root.sortMode = trackId
            close()
        }
    }
}
