pragma Singleton

import QtQuick

QtObject {
    function libraryTypeLabel(collectionType) {
        const language = i18n.language
        switch (String(collectionType || "").toLowerCase()) {
        case "tvshows":
            return qsTr("TV library")
        case "movies":
            return qsTr("Movie library")
        case "homevideos":
            return qsTr("Home video library")
        case "musicvideos":
            return qsTr("Music video library")
        default:
            return qsTr("Mixed media library")
        }
    }

    function mediaSubtitle(item) {
        const language = i18n.language
        if (!item)
            return ""
        const itemType = String(item.itemType || "")
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

    function parentSubtitle(item) {
        const language = i18n.language
        if (!item)
            return ""
        if (String(item.collectionType || "").length > 0)
            return libraryTypeLabel(item.collectionType)
        return mediaSubtitle(item)
    }
}
