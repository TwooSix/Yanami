import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami

Item {
    id: root

    property var mediaItems: []
    property string query: ""
    property var results: filterItems()
    signal itemRequested(var item)
    signal playRequested(string itemId, string title)

    function resetSearch() {
        root.query = ""
        searchField.text = ""
        pageFlickable.contentY = 0
        searchField.focusInput()
    }

    function focusSearch() {
        searchField.focusInput()
    }

    function filterItems() {
        const needle = root.query.trim().toLocaleLowerCase()
        if (needle.length === 0)
            return []
        const matches = []
        for (let index = 0; index < root.mediaItems.length; ++index) {
            const item = root.mediaItems[index]
            const title = String(item.title || "").toLocaleLowerCase()
            const subtitle = LocaleText.mediaSubtitle(item).toLocaleLowerCase()
            if (title.indexOf(needle) >= 0 || subtitle.indexOf(needle) >= 0)
                matches.push(item)
        }
        return matches
    }

    SmoothFlickable {
        id: pageFlickable
        anchors.fill: parent
        contentHeight: content.implicitHeight + 48

        ColumnLayout {
            id: content
            width: parent.width - 14
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
                    visible: root.query.length > 0
                    Layout.alignment: Qt.AlignBottom
                    Layout.bottomMargin: 1
                    kind: "ghost"
                    text: qsTr("Clear")
                    onClicked: root.resetSearch()
                }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true

                Text {
                    text: root.query.trim().length === 0 ? qsTr("Search results")
                        : (root.results.length > 0
                            ? qsTr("%1 results").arg(root.results.length)
                            : qsTr("No matches"))
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
            }

            GridView {
                visible: root.results.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: visible
                    ? Math.max(310, Math.ceil(root.results.length / Math.max(1, Math.floor(width / cellWidth))) * cellHeight)
                    : 0
                cellWidth: 202
                cellHeight: 322
                interactive: false
                model: root.results
                delegate: PosterCard {
                    required property var modelData
                    required property int index
                    title: modelData.title
                    subtitle: LocaleText.mediaSubtitle(modelData)
                    itemType: modelData.itemType
                    posterUrl: modelData.imageUrl || ""
                    progress: modelData.progress || 0
                    unplayedCount: Number(modelData.unplayedCount || 0)
                    posterColor: ["#405E7B", "#6A536F", "#526B5D", "#755358", "#59647C"][index % 5]
                    onActivated: root.itemRequested(modelData)
                    onPlayRequested: root.playRequested(modelData.id, modelData.title)
                }
            }

            GlassPanel {
                visible: root.results.length === 0
                Layout.fillWidth: true
                Layout.preferredHeight: 150
                radius: 24
                color: "#80151920"

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
}
