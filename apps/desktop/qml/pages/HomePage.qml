import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami.Ui
import Yanami.Native

Item {
    id: root

    property var collectionParent: ({})
    property real developmentLibraryScanProgress: -1
    property var seriesDetails: ({})
    property int depth: 0
    readonly property int sortMode: app.preferences.librarySortMode
    property string libraryId: ""
    property string libraryTitle: ""
    property string seriesId: ""
    property string seriesTitle: ""
    property string seasonId: ""
    property string seasonTitle: ""
    property string containerId: ""
    property string containerTitle: ""
    property string containerType: ""
    property int detailReturnPage: -1
    property bool directExternalSeason: false
    property string pendingPlaylistRemovalId: ""
    property string pendingLibraryScrollResetId: ""
    readonly property string routeCollectionId: root.depth === 1 ? root.libraryId
        : (root.depth === 2 ? root.seriesId
        : (root.depth === 3 ? root.seasonId : (root.depth === 4 ? root.containerId : "")))
    readonly property bool collectionReady: root.routeCollectionId.length > 0
        && app.home.collectionDisplayedId === root.routeCollectionId
    readonly property bool collectionRefreshing: root.routeCollectionId.length > 0
        && app.home.collectionTargetId === root.routeCollectionId
        && app.home.collectionLoading
    readonly property bool collectionFailed: root.routeCollectionId.length > 0
        && app.home.collectionErrorId === root.routeCollectionId
    property real resumeSectionHeight: (animatedResumeModel.count > 0
        || resumeRemovalDelay.running || app.home.libraryRefreshing
        || app.home.activityRefreshing) ? 186 : 18

    Behavior on resumeSectionHeight {
        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
    }
    property var sortOptions: [
        { "id": 0, "label": qsTr("Name") },
        { "id": 1, "label": qsTr("Recently updated") },
        { "id": 2, "label": qsTr("Release date") },
        { "id": 3, "label": qsTr("Recently added") },
        { "id": 4, "label": qsTr("Unplayed first") }
    ]
    MediaQueryProxyModel {
        id: animatedCollectionModel
        sourceModel: app.home.mediaStore.queryModel("collection", root.libraryId)
        sortMode: root.sortMode
        sortLocale: i18n.language === "zh_CN" ? "zh_CN" : "en_US"
    }
    readonly property var animatedLibraryViewsModel: app.home.mediaStore.libraryViewsModel
    readonly property var animatedResumeModel: app.home.mediaStore.resumeModel
    readonly property var animatedLatestSectionsModel:
        app.home.mediaStore.queryModel("latestSections")
    readonly property var animatedSeasonsModel:
        app.home.mediaStore.queryModel("collection", root.seriesId)
    readonly property var animatedSeriesContinueModel:
        app.home.mediaStore.queryModel("seriesContinue", root.seriesId)
    readonly property var animatedEpisodesModel:
        app.home.mediaStore.queryModel("collection", root.seasonId)
    readonly property var animatedContainerModel:
        app.home.mediaStore.queryModel("collection", root.containerId)

    onCollectionReadyChanged: {
        if (root.collectionReady)
            Qt.callLater(root.completeLibraryScrollReset)
    }
    onSortModeChanged: {
        if (root.depth === 1)
            root.requestLibraryScrollReset(root.libraryId)
    }

    signal externalReturnRequested(int page)

    component LoadingStrip: Item {
        id: strip
        property real cardWidth: 292
        property real cardHeight: 176
        property real cardRadius: 20
        property bool structuredCards: false
        property real artworkHeight: cardHeight
        property int cardCount: Math.max(3, Math.ceil(width / (cardWidth + 14)))
        implicitHeight: cardHeight
        clip: true

        Row {
            spacing: 14
            Repeater {
                model: strip.cardCount
                LoadingPlaceholder {
                    width: strip.cardWidth
                    height: strip.cardHeight
                    cornerRadius: strip.cardRadius
                    structured: strip.structuredCards
                    artworkHeight: strip.artworkHeight
                }
            }
        }
    }

    Connections {
        target: app.session
        function onStateChanged() {
            if (!app.session.connected)
                root.goHome()
        }
    }

    Timer { id: resumeRemovalDelay; interval: 180 }
    Timer {
        id: playlistRemovalDelay
        interval: 170
        onTriggered: {
            root.pendingPlaylistRemovalId = ""
        }
    }

    EpisodeScrollPolicy {
        id: episodeScrollPolicy
        activeScopeId: root.depth === 3 ? root.seasonId : ""
        model: root.animatedEpisodesModel
        view: episodesList
        ready: root.depth === 3 && root.collectionReady
        refreshing: root.depth === 3 && root.collectionRefreshing
    }

    signal playRequested(string itemId, string title, var playbackContext)
    signal settingsRequested()
    signal mediaContextRequested(var item, var sourceItem, real x, real y,
                                 bool keyboardInvocation)

    function firstLatestMediaItem() {
        if (animatedLatestSectionsModel.count <= 0)
            return null
        const section = animatedLatestSectionsModel.get(0) || ({})
        const sectionId = String(section.id || "")
        if (sectionId.length === 0)
            return null
        const sectionModel = app.home.mediaStore.queryModel("latest", sectionId)
        return sectionModel.count > 0 ? sectionModel.get(0) : null
    }

    function developmentPreviewItem() {
        if (animatedResumeModel.count > 0)
            return animatedResumeModel.get(0)
        const latestItem = root.firstLatestMediaItem()
        if (latestItem)
            return latestItem
        if (app.home.mediaStore.libraryModel.count > 0)
            return app.home.mediaStore.libraryModel.get(0)
        return null
    }

    function runDevelopmentScrollRegression() {
        if (root.depth === 1 && libraryGrid.count > 0) {
            libraryGrid.positionViewAtIndex(libraryGrid.count - 1, GridView.End)
            libraryGrid.resetScrollPosition()
            Qt.callLater(function() {
                const minimum = libraryGrid.originY
                const maximum = Math.max(minimum,
                    minimum + libraryGrid.contentHeight - libraryGrid.height)
                console.info("development_grid_scroll_regression",
                    "count=", libraryGrid.count,
                    "contentY=", libraryGrid.contentY,
                    "originY=", minimum,
                    "maximum=", maximum,
                    "firstVisibleIndex=", libraryGrid.indexAt(
                        libraryGrid.contentX + 1, libraryGrid.contentY + 1))
            })
            return
        }
        const list = root.depth === 2 ? seasonsList
            : (root.depth === 3 ? episodesList : null)
        if (!list || list.count <= 0)
            return
        list.positionViewAtIndex(list.count - 1, ListView.End)
        list.resetScrollPosition()
        Qt.callLater(function() {
            const minimum = list.originX
            const maximum = Math.max(minimum,
                minimum + list.contentWidth - list.width)
            console.info("development_horizontal_scroll_regression",
                "depth=", root.depth,
                "count=", list.count,
                "contentX=", list.contentX,
                "originX=", minimum,
                "maximum=", maximum,
                "firstVisibleIndex=", list.indexAt(
                    list.contentX + 1, list.contentY + 1))
        })
    }

    function openDevelopmentContextPreview() {
        let card = resumePreviewList.itemAtIndex(0)
        if (!card && latestSectionsRepeater.count > 0) {
            const section = latestSectionsRepeater.itemAt(0)
            if (section)
                card = section.previewList.itemAtIndex(0)
        }
        if (!card)
            return false
        root.mediaContextRequested(card.mediaItem, card,
                                   card.width * 0.62, card.height * 0.42, false)
        return true
    }

    function openDevelopmentLibraryContextPreview() {
        const card = libraryPreviewList.itemAtIndex(0)
        if (!card)
            return false
        root.mediaContextRequested(card.mediaItem, card,
                                   card.width * 0.58, card.height * 0.42, false)
        return true
    }

    function sortLabel() {
        for (let index = 0; index < root.sortOptions.length; ++index) {
            if (root.sortOptions[index].id === root.sortMode)
                return root.sortOptions[index].label
        }
        return qsTr("Name")
    }

    function requestLibraryScrollReset(targetId) {
        const normalizedId = String(targetId || "")
        if (normalizedId.length === 0)
            return
        root.pendingLibraryScrollResetId = normalizedId
        libraryGrid.resetScrollPosition()
        Qt.callLater(root.completeLibraryScrollReset)
    }

    function completeLibraryScrollReset() {
        if (root.pendingLibraryScrollResetId.length === 0
                || root.depth !== 1
                || root.libraryId !== root.pendingLibraryScrollResetId) {
            return
        }
        libraryGrid.resetScrollPosition()
        if (root.collectionReady)
            root.pendingLibraryScrollResetId = ""
    }

    function requestEpisodeScroll(targetId) {
        const normalizedId = String(targetId || "")
        if (normalizedId.length === 0)
            return
        episodeScrollPolicy.request(normalizedId)
    }

    function goHome() {
        sortMenu.close()
        episodeScrollPolicy.cancel()
        root.pendingLibraryScrollResetId = ""
        root.depth = 0
        root.libraryId = ""
        root.libraryTitle = ""
        root.seriesId = ""
        root.seriesTitle = ""
        root.seasonTitle = ""
        root.seasonId = ""
        root.containerId = ""
        root.containerTitle = ""
        root.containerType = ""
        root.detailReturnPage = -1
        root.directExternalSeason = false
        root.seriesDetails = ({})
        pageFlickable.contentY = 0
    }

    function openLibraryView(item) {
        episodeScrollPolicy.cancel()
        root.detailReturnPage = -1
        root.directExternalSeason = false
        root.libraryId = item.id
        root.libraryTitle = root.localizedLibraryTitle(item)
        root.seriesId = ""
        root.seriesTitle = ""
        root.seasonTitle = ""
        root.seasonId = ""
        root.containerId = ""
        root.containerTitle = ""
        root.containerType = ""
        root.depth = 1
        root.requestLibraryScrollReset(root.libraryId)
        app.home.loadCollection(item.id)
    }

    function openLibraryItem(item) {
        episodeScrollPolicy.cancel()
        if (item.itemType === "Playlist") {
            root.containerId = item.id
            root.containerTitle = item.title
            root.containerType = item.itemType
            root.depth = 4
            pageFlickable.contentY = 0
            app.home.loadCollection(item.id)
        } else if (item.itemType === "Series") {
            root.seriesId = item.id
            root.seriesTitle = item.title
            root.seasonTitle = ""
            root.seasonId = ""
            root.seriesDetails = ({})
            root.depth = 2
            pageFlickable.contentY = 0
            app.home.loadCollection(item.id)
        } else {
            root.playRequested(item.id, item.title, ({}))
        }
    }

    function openSearchItem(item) {
        root.detailReturnPage = -1
        root.libraryId = ""
        root.libraryTitle = ""
        root.openLibraryItem(item)
    }

    function openExternalItem(item, returnPage) {
        root.libraryId = ""
        root.libraryTitle = ""
        root.detailReturnPage = returnPage
        root.directExternalSeason = false
        root.openLibraryItem(item)
    }

    function openSeason(item) {
        root.directExternalSeason = false
        root.seriesDetails = root.collectionParent
        if (String(item.seriesId || "").length > 0)
            root.seriesId = String(item.seriesId)
        root.seasonId = item.id
        root.seasonTitle = item.title
        root.depth = 3
        pageFlickable.contentY = 0
        root.requestEpisodeScroll(root.seasonId)
        app.home.loadCollection(item.id)
    }

    function openExternalSeason(item, returnPage) {
        root.libraryId = ""
        root.libraryTitle = ""
        root.seriesId = String(item.seriesId || "")
        root.seriesTitle = String(item.seriesTitle || "")
        root.seriesDetails = ({})
        root.detailReturnPage = returnPage
        root.directExternalSeason = true
        root.seasonId = item.id
        root.seasonTitle = item.title
        root.depth = 3
        pageFlickable.contentY = 0
        root.requestEpisodeScroll(root.seasonId)
        app.home.loadCollection(item.id)
    }

    function goBack() {
        if (root.depth === 4) {
            if (root.libraryId.length > 0) {
                root.depth = 1
                root.containerId = ""
                root.containerTitle = ""
                root.containerType = ""
                pageFlickable.contentY = 0
                app.home.loadCollection(root.libraryId)
            } else if (root.detailReturnPage >= 0) {
                const returnPage = root.detailReturnPage
                root.goHome()
                root.externalReturnRequested(returnPage)
            } else {
                root.goHome()
            }
        } else if (root.depth === 3) {
            episodeScrollPolicy.cancel()
            if (root.directExternalSeason && root.detailReturnPage >= 0) {
                const returnPage = root.detailReturnPage
                root.goHome()
                root.externalReturnRequested(returnPage)
            } else if (root.seriesId.length > 0) {
                root.depth = 2
                root.seasonTitle = ""
                root.seasonId = ""
                pageFlickable.contentY = 0
                app.home.loadCollection(root.seriesId)
            } else if (root.detailReturnPage >= 0) {
                const returnPage = root.detailReturnPage
                root.goHome()
                root.externalReturnRequested(returnPage)
            } else {
                root.goHome()
            }
        } else if (root.depth === 2 && root.libraryId.length > 0) {
            root.depth = 1
            root.seriesId = ""
            root.seriesTitle = ""
            pageFlickable.contentY = 0
            app.home.loadCollection(root.libraryId)
        } else if (root.depth === 2 && root.detailReturnPage >= 0) {
            const returnPage = root.detailReturnPage
            root.goHome()
            root.externalReturnRequested(returnPage)
        } else {
            root.goHome()
        }
    }

    function localizedLibraryTitle(item) {
        const kind = String((item || {}).collectionType || "").toLowerCase()
        if (kind === "playlists")
            return qsTr("Playlists")
        return String((item || {}).title || "")
    }

    function libraryCountLabel() {
        const kind = String((root.collectionParent || {}).collectionType || "").toLowerCase()
        if (kind === "playlists")
            return qsTr("%1 playlists").arg(animatedCollectionModel.count)
        return qsTr("%1 titles").arg(animatedCollectionModel.count)
    }

    function emptyLibraryLabel() {
        const kind = String((root.collectionParent || {}).collectionType || "").toLowerCase()
        if (kind === "playlists")
            return qsTr("No playlists yet")
        return qsTr("No titles in this library")
    }

    function containerItemPlayable(item) {
        const type = String((item || {}).itemType || "")
        return type === "Episode" || type === "Movie" || type === "Series"
            || type === "Season" || type === "Video"
    }

    function seriesPlaybackContext() {
        const sourceId = String(root.seriesId || root.collectionParent.seriesId || "")
        if (sourceId.length === 0)
            return ({})
        return {
            "kind": "series",
            "sourceId": sourceId,
            "sourceTitle": String(root.seriesTitle || root.collectionParent.title || "")
        }
    }

    function playlistPlaybackContext(item, queueIndex) {
        return {
            "kind": "playlist",
            "sourceId": String(root.containerId || ""),
            "sourceTitle": String(root.containerTitle || ""),
            "playlistEntryId": String((item || {}).playlistEntryId || ""),
            "queueIndex": Number(queueIndex)
        }
    }

    function itemWithPlaybackContext(item, context) {
        const result = Object.assign({}, item || ({}))
        result.playbackContext = context || ({})
        return result
    }

    function playContainerItem(item, queueIndex) {
        if (!root.containerItemPlayable(item))
            return
        const context = root.playlistPlaybackContext(item, queueIndex)
        root.playRequested(item.id, item.title, context)
    }

    function playlistEntryRemoved(entryId) {
        const normalized = String(entryId || "")
        if (normalized.length === 0)
            return
        root.pendingPlaylistRemovalId = normalized
        playlistRemovalDelay.restart()
    }

    function firstPlayableContainerItem() {
        for (let index = 0; index < animatedContainerModel.count; ++index) {
            const item = animatedContainerModel.get(index) || ({})
            if (root.containerItemPlayable(item))
                return item
        }
        return null
    }

    function firstPlayableContainerIndex() {
        for (let index = 0; index < animatedContainerModel.count; ++index) {
            const item = animatedContainerModel.get(index) || ({})
            if (root.containerItemPlayable(item))
                return index
        }
        return -1
    }

    SmoothFlickable {
        id: pageFlickable
        anchors.fill: parent
        visible: app.session.connected && root.depth !== 1
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
                            : (root.depth === 2 ? qsTr("Series details")
                            : (root.depth === 4 ? root.containerTitle : root.seriesTitle)))
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 32
                        font.weight: Font.DemiBold
                    }
                    Text {
                        visible: root.depth > 0
                        text: root.depth === 1
                            ? (LocaleText.parentSubtitle(root.collectionParent) || qsTr("Library"))
                            : (root.depth === 2 ? root.seriesTitle
                            : (root.depth === 4
                                ? qsTr("Playlist")
                                : root.seasonTitle))
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 13
                    }
                }

                Item { Layout.fillWidth: true }

                AppPopupButton {
                    visible: root.depth === 1
                    kind: "secondary"
                    text: qsTr("Sort · %1").arg(root.sortLabel())
                    popupTarget: sortMenu
                }

                AppButton {
                    visible: root.depth === 0
                    kind: "ghost"
                    iconName: "refresh"
                    iconSpinning: app.home.libraryRefreshing || app.home.activityRefreshing
                    text: qsTr("Refresh")
                    enabled: app.session.connected && !app.home.libraryRefreshing
                    onClicked: app.home.loadLibrary()
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
                    id: libraryPreviewList
                    Layout.fillWidth: true
                    Layout.preferredHeight: 158
                    visible: animatedLibraryViewsModel.count > 0
                    model: animatedLibraryViewsModel
                    delegate: LibraryCard {
                        required property var modelData
                        required property int index
                        width: 286
                        height: 148
                        title: root.localizedLibraryTitle(modelData)
                        subtitle: LocaleText.libraryTypeLabel(modelData.collectionType)
                        imageUrl: modelData.imageUrl || ""
                        mediaItem: modelData
                        scanProgress: root.developmentLibraryScanProgress >= 0 && index === 0
                            ? root.developmentLibraryScanProgress
                            : (app.mediaActions.libraryScanProgress[modelData.id] === undefined
                               ? -1 : Number(app.mediaActions.libraryScanProgress[modelData.id]))
                        fallbackColor: ["#405E7B", "#6A536F", "#526B5D", "#755358"][Math.max(0, index) % 4]
                        onActivated: root.openLibraryView(modelData)
                        onContextMenuRequested: (item, sourceItem, x, y,
                                                 keyboardInvocation) => {
                            if (String(item.itemType || "") !== "VirtualView")
                                root.mediaContextRequested(
                                    item, sourceItem, x, y, keyboardInvocation)
                        }
                    }
                }

                LoadingStrip {
                    visible: animatedLibraryViewsModel.count === 0 && app.home.libraryRefreshing
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 148 : 0
                    cardWidth: 286
                    cardHeight: 148
                    cardRadius: 22
                }

                Text {
                    visible: animatedLibraryViewsModel.count === 0 && !app.home.libraryRefreshing
                    text: app.home.libraryLoadFailed
                        ? qsTr("Unable to load libraries") : qsTr("No libraries to display")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 13
                }

                AppButton {
                    visible: animatedLibraryViewsModel.count === 0 && app.home.libraryLoadFailed
                    kind: "secondary"
                    text: qsTr("Try again")
                    onClicked: app.home.loadLibrary()
                }
            }

            ColumnLayout {
                visible: root.depth === 0
                Layout.fillWidth: true
                spacing: 12

                Text {
                    text: qsTr("Continue watching")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.resumeSectionHeight > 0.5
                        ? root.resumeSectionHeight : emptyResumeLabel.implicitHeight

                    SmoothHorizontalList {
                        id: resumePreviewList
                        anchors.fill: parent
                        visible: opacity > 0
                        opacity: animatedResumeModel.count > 0 ? 1 : 0
                        model: animatedResumeModel
                        delegate: RecentEpisodeCard {
                            required property var modelData
                            title: modelData.title
                            subtitle: LocaleText.mediaSubtitle(modelData)
                            imageUrl: modelData.imageUrl || ""
                            progress: modelData.progress || 0
                            mediaItem: modelData
                            onPlayRequested: root.playRequested(
                                modelData.id, modelData.title, ({}))
                            onContextMenuRequested: (item, sourceItem, x, y,
                                                     keyboardInvocation) =>
                                root.mediaContextRequested(
                                    item, sourceItem, x, y, keyboardInvocation)
                        }
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                    }

                    LoadingStrip {
                        anchors.fill: parent
                        visible: opacity > 0
                        opacity: animatedResumeModel.count === 0
                            && (app.home.libraryRefreshing || app.home.activityRefreshing)
                            && !resumeRemovalDelay.running ? 1 : 0
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                    }

                    Text {
                        id: emptyResumeLabel
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        visible: opacity > 0
                        opacity: animatedResumeModel.count === 0
                            && !app.home.libraryRefreshing && !app.home.activityRefreshing
                            && !resumeRemovalDelay.running ? 1 : 0
                        text: app.home.activityLoadFailed
                            ? qsTr("Could not refresh Continue Watching")
                            : qsTr("Nothing to continue yet")
                        color: app.home.activityLoadFailed
                            ? Theme.danger : Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 13
                        Behavior on opacity { NumberAnimation { duration: 150 } }
                    }
                }
            }

            Repeater {
                id: latestSectionsRepeater
                model: root.depth === 0 ? animatedLatestSectionsModel : null

                delegate: ColumnLayout {
                    id: latestSection
                    required property var modelData
                    required property int index
                    readonly property var latestModel:
                        app.home.mediaStore.queryModel("latest", String(modelData.id || ""))
                    property alias previewList: latestPreviewList

                    visible: latestModel.count > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 346 : 0
                    spacing: 12

                    Text {
                        text: qsTr("Latest %1").arg(
                            root.localizedLibraryTitle(latestSection.modelData))
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }

                    SmoothHorizontalList {
                        id: latestPreviewList
                        Layout.fillWidth: true
                        Layout.preferredHeight: 312
                        spacing: 18
                        model: latestSection.latestModel

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
                            unplayedCount: Number(
                                modelData.childCount || modelData.unplayedCount || 0)
                            mediaItem: modelData
                            playable: root.containerItemPlayable(modelData)
                            posterColor: ["#405E7B", "#6A536F", "#526B5D", "#755358",
                                          "#59647C", "#6B6250"][Math.max(0, index) % 6]
                            onActivated: root.openLibraryItem(modelData)
                            onPlayRequested: root.playRequested(
                                modelData.id, modelData.title, ({}))
                            onContextMenuRequested: (item, sourceItem, x, y,
                                                     keyboardInvocation) =>
                                root.mediaContextRequested(
                                    item, sourceItem, x, y, keyboardInvocation)
                        }
                    }
                }
            }

            ColumnLayout {
                visible: root.depth === 0 && animatedLatestSectionsModel.count === 0
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? implicitHeight : 0
                spacing: 12

                Text {
                    visible: app.home.libraryRefreshing || app.home.activityRefreshing
                    text: qsTr("Latest media")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }

                LoadingStrip {
                    visible: app.home.libraryRefreshing || app.home.activityRefreshing
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 302 : 0
                    cardWidth: 178
                    cardHeight: 302
                    cardRadius: 18
                    structuredCards: true
                    artworkHeight: 238
                }

                Text {
                    visible: !app.home.libraryRefreshing && !app.home.activityRefreshing
                    text: app.home.libraryLoadFailed || app.home.activityLoadFailed
                        ? qsTr("Could not refresh latest media")
                        : qsTr("No recently added media")
                    color: app.home.libraryLoadFailed || app.home.activityLoadFailed
                        ? Theme.danger : Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 13
                }
            }

            DetailHero {
                visible: root.depth >= 2 && root.depth <= 3 && root.collectionReady
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 292 : 0
                eyebrow: root.depth === 2 ? qsTr("Series") : root.seasonTitle
                title: root.depth === 2
                    ? (root.collectionParent.title || root.seriesTitle)
                    : root.seriesTitle
                subtitle: LocaleText.parentSubtitle(root.collectionParent)
                overview: root.collectionParent.overview || root.seriesDetails.overview || ""
                continueLabel: root.collectionParent.continueLabel || ""
                playButtonVisible: root.depth !== 2
                    || animatedSeriesContinueModel.count > 0
                posterUrl: root.collectionParent.imageUrl || root.seriesDetails.imageUrl || ""
                backdropUrl: root.collectionParent.backdropUrl || root.seriesDetails.backdropUrl || ""
                onPlayRequested: {
                    const itemId = root.collectionParent.id || root.seriesId
                    root.playRequested(
                        itemId, root.seriesTitle, root.seriesPlaybackContext())
                }
            }

            GlassPanel {
                visible: root.depth === 4 && root.collectionReady
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 216 : 0
                radius: Theme.radiusLarge
                color: "#DD11151D"
                border.color: "#32FFFFFF"

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 22
                    spacing: 22

                    Rectangle {
                        Layout.preferredWidth: 116
                        Layout.fillHeight: true
                        radius: 18
                        color: "#273140"
                        border.width: 1
                        border.color: "#28FFFFFF"
                        clip: true

                        RoundedImage {
                            anchors.fill: parent
                            source: root.collectionParent.imageUrl || ""
                            radius: parent.radius
                            asynchronous: true
                            fillMode: Image.PreserveAspectCrop
                        }

                        AppIcon {
                            anchors.centerIn: parent
                            width: 38
                            height: 38
                            visible: !root.collectionParent.imageUrl
                            name: "playlist"
                            color: Theme.textMuted
                            strokeWidth: 1.45
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.topMargin: 8
                        Layout.bottomMargin: 8
                        spacing: 8

                        Text {
                            text: qsTr("PLAYLIST")
                            color: Theme.accent
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.2
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.collectionParent.title || root.containerTitle
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 28
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("%1 items in playback order").arg(animatedContainerModel.count)
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 13
                        }
                        Item { Layout.fillHeight: true }
                        AppButton {
                            kind: "primary"
                            iconName: "play"
                            text: qsTr("Play")
                            enabled: root.firstPlayableContainerItem() !== null
                            onClicked: root.playContainerItem(
                                root.firstPlayableContainerItem(),
                                root.firstPlayableContainerIndex())
                        }
                    }
                }
            }

            LoadingPlaceholder {
                visible: root.depth >= 2 && !root.collectionReady
                    && root.collectionRefreshing
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 292 : 0
                cornerRadius: 28
            }

            GlassPanel {
                visible: root.depth >= 2 && root.collectionFailed && !root.collectionReady
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 150 : 0
                radius: 24
                color: "#80151920"

                Column {
                    anchors.centerIn: parent
                    spacing: 12
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Unable to load this page")
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }
                    AppButton {
                        anchors.horizontalCenter: parent.horizontalCenter
                        kind: "secondary"
                        text: qsTr("Try again")
                        onClicked: app.home.loadCollection(root.routeCollectionId)
                    }
                }
            }

            RowLayout {
                visible: root.depth === 2 && root.collectionReady
                    && animatedSeriesContinueModel.count > 0
                Layout.fillWidth: true

                Text {
                    text: qsTr("Continue watching")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
            }

            SmoothHorizontalList {
                id: seriesContinueList
                visible: root.depth === 2 && root.collectionReady
                    && animatedSeriesContinueModel.count > 0
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 186 : 0
                spacing: 18
                model: animatedSeriesContinueModel
                delegate: RecentEpisodeCard {
                    required property var modelData
                    width: 292
                    height: 176
                    title: modelData.title
                    subtitle: LocaleText.mediaSubtitle(modelData)
                    imageUrl: modelData.imageUrl || ""
                    progress: modelData.progress || 0
                    mediaItem: modelData
                    onPlayRequested: root.playRequested(
                        modelData.id, modelData.title,
                        root.seriesPlaybackContext())
                    onContextMenuRequested: (item, sourceItem, x, y,
                                             keyboardInvocation) =>
                        root.mediaContextRequested(
                            root.itemWithPlaybackContext(
                                item, root.seriesPlaybackContext()),
                            sourceItem, x, y, keyboardInvocation)
                }
            }

            RowLayout {
                visible: root.depth === 2
                Layout.fillWidth: true

                Text {
                    text: qsTr("Seasons and specials")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                Text {
                    visible: animatedSeasonsModel.count > 0
                    text: qsTr("%1 items").arg(animatedSeasonsModel.count)
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 12
                }
            }

            SmoothHorizontalList {
                id: seasonsList
                visible: root.depth === 2 && root.collectionReady
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 312 : 0
                spacing: 18
                model: animatedSeasonsModel
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
                    mediaItem: modelData
                    playable: root.containerItemPlayable(modelData)
                    posterColor: ["#405E7B", "#6A536F", "#526B5D", "#755358"][Math.max(0, index) % 4]
                    onActivated: root.openSeason(modelData)
                    onPlayRequested: root.playRequested(
                        modelData.id, modelData.title, root.seriesPlaybackContext())
                    onContextMenuRequested: (item, sourceItem, x, y,
                                             keyboardInvocation) =>
                        root.mediaContextRequested(
                            root.itemWithPlaybackContext(
                                item, root.seriesPlaybackContext()),
                            sourceItem, x, y, keyboardInvocation)
                }
            }

            Text {
                visible: root.depth === 2 && root.collectionReady
                    && animatedSeasonsModel.count === 0
                text: qsTr("No seasons to display")
                color: Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
            }

            LoadingStrip {
                visible: root.depth === 2 && !root.collectionReady && root.collectionRefreshing
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 302 : 0
                cardWidth: 178
                cardHeight: 302
                cardRadius: Theme.radius
                structuredCards: true
                artworkHeight: 246
            }

            RowLayout {
                visible: root.depth === 3
                Layout.fillWidth: true

                Text {
                    text: qsTr("Episodes")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                Text {
                    visible: animatedEpisodesModel.count > 0
                    text: qsTr("%1 episodes · Horizontal browsing").arg(animatedEpisodesModel.count)
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 12
                }
            }

            SmoothHorizontalList {
                id: episodesList
                visible: root.depth === 3 && root.collectionReady
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 274 : 0
                spacing: 18
                model: animatedEpisodesModel
                delegate: EpisodeCard {
                    required property var modelData
                    width: 312
                    height: 264
                    title: modelData.title
                    subtitle: LocaleText.mediaSubtitle(modelData)
                    overview: modelData.overview || ""
                    imageUrl: modelData.imageUrl || ""
                    progress: modelData.progress || 0
                    played: Boolean(modelData.played)
                    mediaItem: modelData
                    onPlayRequested: root.playRequested(
                        modelData.id, modelData.title, root.seriesPlaybackContext())
                    onContextMenuRequested: (item, sourceItem, x, y,
                                             keyboardInvocation) =>
                        root.mediaContextRequested(
                            root.itemWithPlaybackContext(
                                item, root.seriesPlaybackContext()),
                            sourceItem, x, y, keyboardInvocation)
                }
            }

            Text {
                visible: root.depth === 3 && root.collectionReady
                    && animatedEpisodesModel.count === 0
                text: qsTr("No playable episodes in this season")
                color: Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
            }

            LoadingStrip {
                visible: root.depth === 3 && !root.collectionReady && root.collectionRefreshing
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 264 : 0
                cardWidth: 312
                cardHeight: 264
                cardRadius: 18
                structuredCards: true
                artworkHeight: 164
            }

            RowLayout {
                visible: root.depth === 4 && root.collectionReady
                Layout.fillWidth: true

                Text {
                    text: qsTr("Playlist items")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: qsTr("%1 items").arg(animatedContainerModel.count)
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 12
                }
            }

            ColumnLayout {
                visible: root.depth === 4 && root.collectionReady
                Layout.fillWidth: true
                spacing: 9

                Repeater {
                    model: animatedContainerModel
                    delegate: GlassPanel {
                        id: playlistRow
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.pendingPlaylistRemovalId
                            === String(playlistRow.modelData.playlistEntryId || "") ? 0 : 82
                        radius: 17
                        color: rowMouse.containsMouse ? "#D9232832" : "#B7191D25"
                        border.color: rowMouse.containsMouse ? "#3DFFFFFF" : "#24FFFFFF"
                        opacity: Layout.preferredHeight > 0 ? 1 : 0
                        clip: true

                        Behavior on color { ColorAnimation { duration: 120 } }
                        Behavior on border.color { ColorAnimation { duration: 120 } }
                        Behavior on Layout.preferredHeight {
                            NumberAnimation { duration: 170; easing.type: Easing.InOutCubic }
                        }
                        Behavior on opacity { NumberAnimation { duration: 130 } }

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 14
                            z: 1

                            Text {
                                Layout.preferredWidth: 28
                                horizontalAlignment: Text.AlignHCenter
                                text: String(playlistRow.index + 1)
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 12
                                font.weight: Font.Medium
                            }

                            Rectangle {
                                Layout.preferredWidth: 98
                                Layout.fillHeight: true
                                radius: 11
                                color: "#273140"
                                clip: true

                                RoundedImage {
                                    anchors.fill: parent
                                    source: playlistRow.modelData.imageUrl || ""
                                    radius: parent.radius
                                    asynchronous: true
                                    fillMode: Image.PreserveAspectCrop
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3
                                Text {
                                    Layout.fillWidth: true
                                    text: playlistRow.modelData.title || ""
                                    color: Theme.text
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 14
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: LocaleText.mediaSubtitle(playlistRow.modelData)
                                    color: Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                            }

                            AppButton {
                                visible: root.collectionParent.canEditItems === true
                                    && String(playlistRow.modelData.playlistEntryId || "").length > 0
                                kind: "ghost"
                                iconOnly: true
                                iconName: "trash"
                                controlSize: 38
                                toolTipText: qsTr("Remove from playlist")
                                onClicked: app.mediaActions.removeFromPlaylist(
                                    playlistRow.modelData.id,
                                    root.containerId,
                                    playlistRow.modelData.playlistEntryId)
                            }
                            AppButton {
                                kind: "ghost"
                                iconOnly: true
                                iconName: "play"
                                controlSize: 38
                                enabled: root.containerItemPlayable(playlistRow.modelData)
                                onClicked: root.playContainerItem(
                                    playlistRow.modelData, playlistRow.index)
                            }
                            AppButton {
                                kind: "ghost"
                                iconOnly: true
                                iconName: "more"
                                controlSize: 38
                                onClicked: root.mediaContextRequested(
                                    root.itemWithPlaybackContext(
                                        playlistRow.modelData,
                                     root.playlistPlaybackContext(
                                             playlistRow.modelData, playlistRow.index)),
                                    this, width / 2, height,
                                    this.visualFocus)
                            }
                        }

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton
                            cursorShape: root.containerItemPlayable(playlistRow.modelData)
                                ? Qt.PointingHandCursor : Qt.ArrowCursor
                            z: 0
                            onClicked: root.playContainerItem(
                                playlistRow.modelData, playlistRow.index)
                        }
                    }
                }
            }

            Text {
                visible: root.depth === 4 && root.collectionReady
                    && animatedContainerModel.count === 0
                text: qsTr("This playlist is empty")
                color: Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
            }
        }
    }

    Item {
        id: libraryPage
        anchors.fill: parent
        anchors.rightMargin: 14
        visible: app.session.connected && root.depth === 1

        RowLayout {
            id: libraryHeader
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            spacing: 12

            AppButton {
                kind: "ghost"
                iconOnly: true
                iconName: "back"
                controlSize: 42
                onClicked: root.goBack()
            }

            Column {
                spacing: 3
                Text {
                    text: root.libraryTitle
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 32
                    font.weight: Font.DemiBold
                }
                Text {
                    text: root.collectionReady
                        ? (LocaleText.parentSubtitle(root.collectionParent) || qsTr("Library"))
                        : qsTr("Library")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 13
                }
            }

            Item { Layout.fillWidth: true }

            Text {
                visible: animatedCollectionModel.count > 0
                text: qsTr("%1 · %2").arg(root.libraryCountLabel()).arg(root.sortLabel())
                color: Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 12
            }

            AppPopupButton {
                kind: "secondary"
                text: qsTr("Sort · %1").arg(root.sortLabel())
                popupTarget: sortMenu
            }
        }

        SmoothGridView {
            id: libraryGrid
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: libraryHeader.bottom
            anchors.bottom: parent.bottom
            anchors.topMargin: 22
            anchors.bottomMargin: 24
            cacheBuffer: cellHeight
            cellWidth: 202
            cellHeight: 322
            model: animatedCollectionModel
            visible: root.collectionReady

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
                playable: root.containerItemPlayable(modelData)
                posterColor: ["#405E7B", "#6A536F", "#526B5D", "#755358", "#59647C", "#6B6250"][Math.max(0, index) % 6]
                onActivated: root.openLibraryItem(modelData)
                onPlayRequested: root.playRequested(modelData.id, modelData.title, ({}))
                onContextMenuRequested: (item, sourceItem, x, y,
                                         keyboardInvocation) =>
                    root.mediaContextRequested(
                        item, sourceItem, x, y, keyboardInvocation)
            }

            Text {
                anchors.centerIn: parent
                visible: root.collectionReady && animatedCollectionModel.count === 0
                text: root.emptyLibraryLabel()
                color: Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 14
            }
        }

        Flow {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: libraryHeader.bottom
            anchors.bottom: parent.bottom
            anchors.topMargin: 22
            visible: !root.collectionReady && root.collectionRefreshing
            spacing: 24

            Repeater {
                model: Math.max(6, Math.ceil(parent.width / 202) * 2)
                LoadingPlaceholder {
                    width: 178
                    height: 302
                    cornerRadius: Theme.radius
                    structured: true
                    artworkHeight: 246
                }
            }
        }

        GlassPanel {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: libraryHeader.bottom
            anchors.topMargin: 22
            height: 150
            visible: root.collectionFailed && !root.collectionReady
            radius: 24
            color: "#80151920"

            Column {
                anchors.centerIn: parent
                spacing: 12
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Unable to load this library")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                AppButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    kind: "secondary"
                    text: qsTr("Try again")
                    onClicked: app.home.loadCollection(root.routeCollectionId)
                }
            }
        }
    }

    Item {
        id: disconnectedHome

        anchors.fill: parent
        visible: !app.session.connected
        z: 2

        GlassPanel {
            anchors.centerIn: parent
            width: Math.min(disconnectedHome.width - 48, 620)
            height: disconnectedContent.implicitHeight + 64
            radius: Theme.radiusLarge
            color: Theme.surfaceStrong

            ColumnLayout {
                id: disconnectedContent

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 32
                spacing: 18

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 64
                    Layout.preferredHeight: 64
                    radius: 21
                    color: Theme.accentSoft
                    border.width: 1
                    border.color: "#52FF6687"

                    Text {
                        anchors.centerIn: parent
                        text: "E"
                        color: Theme.accent
                        font.family: Theme.fontFamily
                        font.pixelSize: 23
                        font.weight: Font.Bold
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Connect your Emby server")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 26
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Set up Emby in Settings to start browsing your media library.")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }

                AppButton {
                    Layout.alignment: Qt.AlignHCenter
                    kind: "primary"
                    iconName: "settings"
                    text: qsTr("Go to Settings")
                    onClicked: root.settingsRequested()
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
            app.preferences.librarySortMode = trackId
            close()
        }
    }
}
