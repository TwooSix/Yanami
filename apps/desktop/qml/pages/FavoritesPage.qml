import QtQuick
import QtQuick.Layouts
import Yanami.Ui
import Yanami.Native

Item {
    id: root

    property bool refreshing: false
    property bool loadFailed: false
    signal itemRequested(var item)
    signal playRequested(string itemId, string title)
    signal mediaContextRequested(var item, var sourceItem, real x, real y,
                                 bool keyboardInvocation)
    signal retryRequested()

    readonly property int totalFavoritesCount: seriesModel.count + moviesModel.count
        + episodesModel.count + otherModel.count
    readonly property int seriesFavoriteCount: seriesModel.count
    readonly property int movieFavoriteCount: moviesModel.count
    readonly property int episodeFavoriteCount: episodesModel.count
    readonly property int otherFavoriteCount: otherModel.count
    readonly property bool initialLoading: root.refreshing
        && root.totalFavoritesCount === 0

    MediaQueryProxyModel {
        id: seriesModel
        sourceModel: app.home.mediaStore.favoritesModel
        category: "series"
    }
    MediaQueryProxyModel {
        id: moviesModel
        sourceModel: app.home.mediaStore.favoritesModel
        category: "movies"
    }
    MediaQueryProxyModel {
        id: episodesModel
        sourceModel: app.home.mediaStore.favoritesModel
        category: "episodes"
    }
    MediaQueryProxyModel {
        id: otherModel
        sourceModel: app.home.mediaStore.favoritesModel
        category: "other"
    }

    function isPlayable(item) {
        const type = String((item || {}).itemType || "")
        return type === "Episode" || type === "Movie" || type === "Series"
            || type === "Season" || type === "Video" || type === "MusicVideo"
    }

    SmoothFlickable {
        id: pageFlickable
        anchors.fill: parent
        contentHeight: content.implicitHeight + 36

        ColumnLayout {
            id: content
            width: parent.width - 14
            spacing: 28

            RowLayout {
                Layout.fillWidth: true

                Column {
                    spacing: 3
                    Text {
                        text: qsTr("Favorites")
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 32
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: qsTr("Your saved media, organized by type")
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 13
                    }
                }
                Item { Layout.fillWidth: true }

                RowLayout {
                    spacing: 9
                    opacity: root.refreshing && root.totalFavoritesCount > 0 ? 1 : 0
                    visible: opacity > 0.01
                    LoadingIndicator {
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        indicatorSize: 18
                        running: parent.visible
                    }
                    Text {
                        text: qsTr("Updating")
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 12
                    }
                    Behavior on opacity { NumberAnimation { duration: 160 } }
                }
            }

            ColumnLayout {
                visible: seriesModel.count > 0
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? implicitHeight : 0
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: qsTr("Series")
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: qsTr("%1 items").arg(seriesModel.count)
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 12
                    }
                }

                SmoothHorizontalList {
                    id: seriesList
                    Layout.fillWidth: true
                    Layout.preferredHeight: 312
                    spacing: 18
                    model: seriesModel
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
                        playable: root.isPlayable(modelData)
                        posterColor: ["#405E7B", "#6A536F", "#526B5D", "#755358"]
                            [Math.max(0, index) % 4]
                        onActivated: root.itemRequested(modelData)
                        onPlayRequested: root.playRequested(modelData.id, modelData.title)
                        onContextMenuRequested: (item, sourceItem, x, y,
                                                 keyboardInvocation) =>
                            root.mediaContextRequested(
                                item, sourceItem, x, y, keyboardInvocation)
                    }
                }
            }

            ColumnLayout {
                visible: moviesModel.count > 0
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? implicitHeight : 0
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: qsTr("Movies")
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: qsTr("%1 items").arg(moviesModel.count)
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 12
                    }
                }

                SmoothHorizontalList {
                    id: moviesList
                    Layout.fillWidth: true
                    Layout.preferredHeight: 312
                    spacing: 18
                    model: moviesModel
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
                        playable: root.isPlayable(modelData)
                        posterColor: ["#526B5D", "#755358", "#405E7B", "#6A536F"]
                            [Math.max(0, index) % 4]
                        onActivated: root.itemRequested(modelData)
                        onPlayRequested: root.playRequested(modelData.id, modelData.title)
                        onContextMenuRequested: (item, sourceItem, x, y,
                                                 keyboardInvocation) =>
                            root.mediaContextRequested(
                                item, sourceItem, x, y, keyboardInvocation)
                    }
                }
            }

            ColumnLayout {
                visible: episodesModel.count > 0
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? implicitHeight : 0
                spacing: 12

                RowLayout {
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
                        text: qsTr("%1 items").arg(episodesModel.count)
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 12
                    }
                }

                SmoothHorizontalList {
                    id: episodesList
                    Layout.fillWidth: true
                    Layout.preferredHeight: 186
                    spacing: 18
                    model: episodesModel
                    delegate: RecentEpisodeCard {
                        required property var modelData
                        title: modelData.title
                        subtitle: LocaleText.mediaSubtitle(modelData)
                        imageUrl: modelData.imageUrl || ""
                        progress: modelData.progress || 0
                        mediaItem: modelData
                        onPlayRequested: root.playRequested(modelData.id, modelData.title)
                        onContextMenuRequested: (item, sourceItem, x, y,
                                                 keyboardInvocation) =>
                            root.mediaContextRequested(
                                item, sourceItem, x, y, keyboardInvocation)
                    }
                }
            }

            ColumnLayout {
                visible: otherModel.count > 0
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? implicitHeight : 0
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: qsTr("Other")
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 20
                        font.weight: Font.DemiBold
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: qsTr("%1 items").arg(otherModel.count)
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 12
                    }
                }

                SmoothHorizontalList {
                    id: otherList
                    Layout.fillWidth: true
                    Layout.preferredHeight: 312
                    spacing: 18
                    model: otherModel
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
                        playable: root.isPlayable(modelData)
                        posterColor: ["#59647C", "#6A536F", "#526B5D", "#755358"]
                            [Math.max(0, index) % 4]
                        onActivated: root.itemRequested(modelData)
                        onPlayRequested: root.playRequested(modelData.id, modelData.title)
                        onContextMenuRequested: (item, sourceItem, x, y,
                                                 keyboardInvocation) =>
                            root.mediaContextRequested(
                                item, sourceItem, x, y, keyboardInvocation)
                    }
                }
            }

            GlassPanel {
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 168 : 0
                visible: opacity > 0.01
                opacity: (root.totalFavoritesCount === 0 && !root.initialLoading)
                    || (root.loadFailed && !root.initialLoading) ? 1 : 0
                radius: 24
                color: "#80151920"

                Behavior on opacity { NumberAnimation { duration: 180 } }

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 26
                    spacing: 18

                    Rectangle {
                        Layout.preferredWidth: 54
                        Layout.preferredHeight: 54
                        radius: 17
                        color: Theme.accentSoft
                        border.width: 1
                        border.color: "#40FF8FA7"
                        AppIcon {
                            anchors.centerIn: parent
                            width: 24
                            height: 24
                            name: root.loadFailed ? "warning" : "heart"
                            color: root.loadFailed ? Theme.danger : Theme.accent
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Text {
                            text: root.loadFailed
                                ? qsTr("Favorites could not be updated")
                                : qsTr("Your favorites will appear here")
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 17
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.loadFailed
                                ? qsTr("Cached favorites stay available while you try again.")
                                : qsTr("Use the heart action on any media card to save it for later.")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }

                    AppButton {
                        visible: root.loadFailed
                        kind: "secondary"
                        text: qsTr("Try again")
                        onClicked: root.retryRequested()
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 168 : 0
                visible: root.initialLoading

                Column {
                    anchors.centerIn: parent
                    spacing: 12
                    LoadingIndicator {
                        anchors.horizontalCenter: parent.horizontalCenter
                        indicatorSize: 28
                        running: visible
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: qsTr("Loading favorites")
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 13
                    }
                }
            }
        }
    }
}
