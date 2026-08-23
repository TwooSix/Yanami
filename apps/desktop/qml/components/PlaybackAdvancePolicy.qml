import QtQuick

QtObject {
    function queueEntryAt(queueItems, queueIndex) {
        if (!Array.isArray(queueItems) || !Number.isInteger(queueIndex)
                || queueIndex < 0)
            return ({})
        for (let index = 0; index < queueItems.length; ++index) {
            const item = queueItems[index] || ({})
            const itemIndex = Number(item.queueIndex)
            if ((Number.isInteger(itemIndex) ? itemIndex : index) === queueIndex)
                return item
        }
        return ({})
    }

    function decide(queueItems, currentQueueIndex, adjacentItem,
                    queueResolutionSucceeded) {
        const nextIndex = Number.isInteger(currentQueueIndex)
                && currentQueueIndex >= 0 ? currentQueueIndex + 1 : -1
        const queueCandidate = queueEntryAt(queueItems, nextIndex)
        const fallbackCandidate = adjacentItem || ({})
        const item = String(queueCandidate.id || "").length > 0
            ? queueCandidate : fallbackCandidate
        if (String(item.id || "").length > 0) {
            const encodedIndex = Number(item.queueIndex)
            return {
                "action": "open-next",
                "item": item,
                "queueIndex": Number.isInteger(encodedIndex)
                    ? encodedIndex : nextIndex
            }
        }
        return {
            "action": queueResolutionSucceeded === true
                ? "complete" : "refresh-queue",
            "item": ({}),
            "queueIndex": -1
        }
    }
}
