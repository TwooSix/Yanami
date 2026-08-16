pragma Singleton

import QtQuick

QtObject {
    function libraryTypeLabel(collectionType) {
        const language = typeof i18n === "undefined" ? "" : i18n.language
        switch (String(collectionType || "").toLowerCase()) {
        case "tvshows":
            return qsTr("TV library")
        case "movies":
            return qsTr("Movie library")
        case "homevideos":
            return qsTr("Home video library")
        case "musicvideos":
            return qsTr("Music video library")
        case "playlists":
            return qsTr("Playlists")
        default:
            return qsTr("Mixed media library")
        }
    }

    function mediaSubtitle(item) {
        const language = typeof i18n === "undefined" ? "" : i18n.language
        if (!item)
            return ""
        const itemType = String(item.itemType || "")
        if (itemType === "Playlist") {
            const count = Number(item.childCount || 0)
            return count > 0 ? qsTr("%1 items").arg(count) : qsTr("Playlist")
        }
        if ((itemType === "Series" || itemType === "Season") && item.hasLatestEpisode !== true) {
            const total = Number(item.childCount || 0)
            const unplayed = Number(item.unplayedCount || 0)
            if (total > 0 && unplayed > 0)
                return qsTr("%1 episodes · %2 unplayed").arg(total).arg(unplayed)
            if (total > 0)
                return qsTr("%1 episodes").arg(total)
            if (Number(item.productionYear || 0) > 0)
                return String(item.productionYear)
            return qsTr("Series")
        }
        return String(item.subtitle || "")
    }

    function itemTypeLabel(itemType) {
        const language = typeof i18n === "undefined" ? "" : i18n.language
        switch (String(itemType || "")) {
        case "Series": return qsTr("Series")
        case "Season": return qsTr("Season")
        case "Episode": return qsTr("Episode")
        case "Movie": return qsTr("Movie")
        case "Video": return qsTr("Video")
        case "Playlist": return qsTr("Playlist")
        default: return qsTr("Media item")
        }
    }

    function parentSubtitle(item) {
        const language = typeof i18n === "undefined" ? "" : i18n.language
        if (!item)
            return ""
        if (String(item.collectionType || "").length > 0)
            return libraryTypeLabel(item.collectionType)
        return mediaSubtitle(item)
    }
}
