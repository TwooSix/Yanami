import QtQuick

// Routes feature results only to presentation endpoints. Network workflows
// are owned by their C++ feature view models.
Item {
    id: host
    visible: false

    required property var hostWindow
    required property var refreshMetadataDialog
    required property var homePage
    required property var deleteConfirm

    Connections {
        target: app.mediaActions

        function onMetadataRefreshed(itemId, result) {
            host.refreshMetadataDialog.submitSucceeded(itemId)
        }
        function onRemovedFromPlaylist(itemId, result) {
            host.homePage.playlistEntryRemoved(
                result.removedPlaylistEntryId || "")
            host.hostWindow.showActionToast(qsTr("Removed from playlist"))
        }
        function onItemDeleted(itemId, result) {
            host.deleteConfirm.complete()
            host.hostWindow.pendingDeleteItem = ({})
        }

        function onMetadataRefreshFailed(itemId, message, nonModal) {
            host.hostWindow.reportMediaActionFailure(message, nonModal,
                host.refreshMetadataDialog.submitFailed(itemId, message))
        }
        function onRemoveFromPlaylistFailed(itemId, message, nonModal) {
            host.hostWindow.reportMediaActionFailure(message, nonModal, false)
        }
        function onPlayStateChangeFailed(itemId, message, nonModal) {
            host.hostWindow.reportMediaActionFailure(message, nonModal, false)
        }
        function onFavoriteChangeFailed(itemId, message, nonModal) {
            host.hostWindow.reportMediaActionFailure(message, nonModal, false)
        }
        function onLibraryFilesScanFailed(itemId, message, nonModal) {
            host.hostWindow.reportMediaActionFailure(message, nonModal, false)
        }
        function onItemDeleteFailed(itemId, message, nonModal) {
            host.deleteConfirm.fail(message)
            host.hostWindow.reportMediaActionFailure(message, nonModal, true)
        }
    }

    Connections {
        target: app.danmaku

        function onSearchCompleted(itemId, result) {
            host.hostWindow.deliverDanmakuResult(itemId, result)
        }
        function onAutomaticLoadCompleted(itemId, result) {
            host.hostWindow.deliverDanmakuResult(itemId, result)
        }
        function onMatchApplied(itemId, result) {
            host.hostWindow.deliverDanmakuResult(itemId, result)
        }
        function onSearchFailed(itemId, message, nonModal) {
            host.hostWindow.reportDanmakuFailure(itemId, message, nonModal)
        }
        function onAutomaticLoadFailed(itemId, message, nonModal) {
            host.hostWindow.reportDanmakuFailure(itemId, message, nonModal)
        }
        function onMatchApplyFailed(itemId, message, nonModal) {
            host.hostWindow.reportDanmakuFailure(itemId, message, nonModal)
        }
    }
}
