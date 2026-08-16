import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models
import Yanami.Ui

AppMenu {
    id: root

    enum Intent {
        Open,
        Resume,
        PlayFromBeginning,
        SetPlayed,
        SetFavorite,
        AddToPlaylist,
        EditMetadata,
        EditImages,
        ScanLibraryFiles,
        RefreshMetadata,
        DeleteItem
    }

    property var mediaItem: ({})
    property bool isAdministrator: false
    property bool canDelete: false
    property var menuItems: []
    readonly property real bodyHeight: root.menuBodyHeight()
    signal openRequested(var item)
    signal playbackRequested(var item, bool fromBeginning)
    signal playedChangeRequested(var item, bool played)
    signal favoriteChangeRequested(var item, bool favorite)
    signal playlistAddRequested(var item, var opener)
    signal metadataEditRequested(var item, var opener)
    signal imageEditRequested(var item, var opener)
    signal libraryScanRequested(var item)
    signal metadataRefreshRequested(var item, var opener)
    signal deleteRequested(var item, var opener)

    function menuBodyHeight() {
        let result = 0
        for (let index = 0; index < root.menuItems.length; ++index) {
            const row = root.menuItems[index] || ({})
            result += 38 + (row.separatorBefore === true ? 8 : 0)
        }
        return result
    }

    function focusRelative(index, step) {
        if (root.menuItems.length <= 0)
            return
        const next = (index + step + root.menuItems.length)
            % root.menuItems.length
        root.focusItem(next)
    }

    function triggerIntent(row) {
        if (!row || row.intent === undefined)
            return
        const item = root.mediaItem
        const opener = root.focusReturnTarget
        root.close()
        switch (Number(row.intent)) {
        case MediaContextMenu.Open:
            root.openRequested(item)
            break
        case MediaContextMenu.Resume:
            root.playbackRequested(item, false)
            break
        case MediaContextMenu.PlayFromBeginning:
            root.playbackRequested(item, true)
            break
        case MediaContextMenu.SetPlayed:
            root.playedChangeRequested(item, Boolean(row.value))
            break
        case MediaContextMenu.SetFavorite:
            root.favoriteChangeRequested(item, Boolean(row.value))
            break
        case MediaContextMenu.AddToPlaylist:
            root.playlistAddRequested(item, opener)
            break
        case MediaContextMenu.EditMetadata:
            root.metadataEditRequested(item, opener)
            break
        case MediaContextMenu.EditImages:
            root.imageEditRequested(item, opener)
            break
        case MediaContextMenu.ScanLibraryFiles:
            root.libraryScanRequested(item)
            break
        case MediaContextMenu.RefreshMetadata:
            root.metadataRefreshRequested(item, opener)
            break
        case MediaContextMenu.DeleteItem:
            root.deleteRequested(item, opener)
            break
        }
    }

    function isPlayable(item) {
        const type = String(item.itemType || "")
        return type === "Episode" || type === "Movie" || type === "Series"
            || type === "Season" || type === "Video"
    }

    function isLibraryRoot(type) {
        return type === "CollectionFolder" || type === "UserView"
            || type === "Folder" || type === "AggregateFolder"
    }

    function supportsMetadataEditing(type) {
        return type === "Movie" || type === "Series" || type === "Season"
            || type === "Episode" || type === "Video"
    }

    function supportsFavorite(type) {
        // Mirrors Emby's canRate rule for the media types currently exposed by
        // Yanami: library roots and Season containers are not rateable.
        return type === "Movie" || type === "Series"
            || type === "Episode" || type === "Video"
    }

    function rebuild() {
        const item = root.mediaItem || ({})
        const rows = []
        const type = String(item.itemType || "")
        const hasProgress = Number(item.resumeTicks || 0) > 0
            || (Number(item.progress || 0) > 0 && Number(item.progress || 0) < 100)
        if (root.isLibraryRoot(type) || type === "Series" || type === "Season"
                || type === "Playlist")
            rows.push({ intent: MediaContextMenu.Open, label: qsTr("Open"), icon: "open" })
        if (root.isPlayable(item)) {
            rows.push({
                intent: MediaContextMenu.Resume,
                label: hasProgress ? qsTr("Resume") : qsTr("Play"),
                icon: "play"
            })
            if (hasProgress) {
                rows.push({
                    intent: MediaContextMenu.PlayFromBeginning,
                    label: qsTr("Play from beginning"),
                    icon: "restart"
                })
            }
            rows.push({ separator: true })
            rows.push({
                intent: MediaContextMenu.SetPlayed,
                value: !item.played,
                label: item.played ? qsTr("Mark as unplayed") : qsTr("Mark as played"),
                icon: item.played ? "circle" : "check"
            })
            if (root.supportsFavorite(type)) {
                rows.push({
                    intent: MediaContextMenu.SetFavorite,
                    value: !item.favorite,
                    label: item.favorite ? qsTr("Remove from favorites") : qsTr("Add to favorites"),
                    icon: item.favorite ? "heart-filled" : "heart"
                })
            }
            rows.push({
                intent: MediaContextMenu.AddToPlaylist,
                label: qsTr("Add to playlist"),
                icon: "playlist"
            })
        }
        if (!root.isPlayable(item) && root.supportsFavorite(type)) {
            rows.push({
                intent: MediaContextMenu.SetFavorite,
                value: !item.favorite,
                label: item.favorite ? qsTr("Remove from favorites") : qsTr("Add to favorites"),
                icon: item.favorite ? "heart-filled" : "heart"
            })
        }
        if (root.isAdministrator
                && (root.supportsMetadataEditing(type) || root.isLibraryRoot(type))) {
            if (rows.length > 0 && rows[rows.length - 1].separator !== true)
                rows.push({ separator: true })
            if (root.supportsMetadataEditing(type)) {
                rows.push({
                    intent: MediaContextMenu.EditMetadata,
                    label: qsTr("Edit metadata"),
                    icon: "edit"
                })
            }
            // Emby library roots do not expose the regular metadata form, but
            // administrators can still manage their visible artwork.
            rows.push({
                intent: MediaContextMenu.EditImages,
                label: qsTr("Edit images"),
                icon: "image"
            })
            if (root.isLibraryRoot(type) || type === "Series" || type === "Season") {
                rows.push({
                    intent: MediaContextMenu.ScanLibraryFiles,
                    label: qsTr("Scan library files"),
                    icon: "scan"
                })
            }
            rows.push({
                intent: MediaContextMenu.RefreshMetadata,
                label: qsTr("Refresh metadata"),
                icon: "refresh"
            })
        }
        if (root.canDelete && !root.isLibraryRoot(type)) {
            rows.push({ separator: true })
            rows.push({
                intent: MediaContextMenu.DeleteItem,
                label: qsTr("Delete"),
                icon: "trash",
                destructive: true
            })
        }

        // Keep separators visual while giving Menu a pure sequence of
        // actionable MenuItems. This lets QQuickMenu own current focus and
        // prevents disabled separator rows from leaking into UIA navigation.
        const actions = []
        let separatorBefore = false
        for (let index = 0; index < rows.length; ++index) {
            const row = rows[index]
            if (row.separator === true) {
                separatorBefore = actions.length > 0
                continue
            }
            row.separatorBefore = separatorBefore
            separatorBefore = false
            actions.push(row)
        }
        root.menuItems = actions
    }

    function openFor(item, sourceItem, localX, localY,
                     keyboardInvocation) {
        if (!sourceItem || !root.parent)
            return false
        root.mediaItem = item || ({})
        root.focusReturnTarget = sourceItem
        root.rebuild()
        const point = sourceItem.mapToItem(root.parent, localX, localY)
        root.x = Math.max(10, Math.min(
            point.x, root.parent.width - root.width - 10))
        const desiredHeight = Math.min(
            root.height, root.parent.height - 20)
        root.y = Math.max(10, Math.min(
            point.y, root.parent.height - desiredHeight - 10))
        const showKeyboardFocus = keyboardInvocation === undefined
            ? InputModality.focusNavigationActive
            : Boolean(keyboardInvocation)
        root.openPreferred(showKeyboardFocus)
        return true
    }

    parent: Overlay.overlay
    width: 244
    height: Math.min(
        bodyHeight + topPadding + bottomPadding,
        Math.max(120, root.parent ? root.parent.height - 20 : 600))
    padding: 6
    preferredCurrentIndex: 0
    title: qsTr("Media actions")

    background: Rectangle {
        radius: 16
        color: "#F51A1D26"
        border.width: 1
        border.color: "#42FFFFFF"
    }

    Instantiator {
        model: root.menuItems

        delegate: MenuItem {
            id: menuRow
            required property int index
            required property var modelData

            width: root.availableWidth
            height: 38 + (modelData.separatorBefore === true ? 8 : 0)
            topPadding: modelData.separatorBefore === true ? 8 : 0
            leftPadding: 0
            rightPadding: 0
            bottomPadding: 0
            text: modelData.label || ""
            hoverEnabled: true
            readonly property bool keyboardCurrent:
                root.keyboardFocusVisible
                    && root.currentIndex === menuRow.index
            Accessible.id: "media-context-menu-item-" + menuRow.index
            Accessible.name: menuRow.text
            Accessible.focusable: true
            Accessible.focused: menuRow.activeFocus

            onTriggered: root.triggerIntent(menuRow.modelData)
            Keys.onUpPressed: event => {
                root.focusRelative(menuRow.index, -1)
                event.accepted = true
            }
            Keys.onDownPressed: event => {
                root.focusRelative(menuRow.index, 1)
                event.accepted = true
            }
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Home)
                    root.focusItem(0)
                else if (event.key === Qt.Key_End)
                    root.focusItem(root.menuItems.length - 1)
                else
                    return
                event.accepted = true
            }

            background: Item {
                Rectangle {
                    visible: menuRow.modelData.separatorBefore === true
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin: 9
                    anchors.rightMargin: 9
                    anchors.topMargin: 3
                    height: 1
                    color: "#20FFFFFF"
                }

                Rectangle {
                    objectName: "menu-hover-surface"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.topMargin: menuRow.modelData.separatorBefore === true ? 8 : 0
                    radius: 10
                    color: menuRow.down ? "#2AFFFFFF"
                        : ((menuRow.keyboardCurrent
                            || (!root.keyboardFocusVisible
                                && menuRow.hovered))
                            ? "#20FFFFFF" : "transparent")
                    // Mouse use is fill-only. The accent frame appears only
                    // after a keyboard invocation or keyboard navigation.
                    border.width: menuRow.keyboardCurrent ? 2 : 0
                    border.color: Theme.accent
                }
            }

            contentItem: Item {
                AppIcon {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    name: menuRow.modelData.icon || "circle"
                    color: menuRow.modelData.destructive
                        ? Theme.danger : Theme.textMuted
                    strokeWidth: 1.7
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 38
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: menuRow.text
                    color: menuRow.modelData.destructive
                        ? Theme.danger : Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
            }
        }

        onObjectAdded: (index, object) => root.insertItem(index, object)
        onObjectRemoved: (index, object) => root.removeItem(object)
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 110 }
        NumberAnimation {
            property: "scale"
            from: 0.97
            to: 1
            duration: 140
            easing.type: Easing.OutCubic
        }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 90 }
    }
}
