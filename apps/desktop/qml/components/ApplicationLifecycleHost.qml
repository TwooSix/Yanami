import QtQuick

Item {
    id: host
    visible: false

    required property var hostWindow
    property int observedSessionGeneration: -1
    property bool observedConnected: false

    Component.onCompleted: {
        observedSessionGeneration = Number(app.session.generation || 0)
        observedConnected = Boolean(app.session.connected)
        Qt.callLater(function() {
            if (host.hostWindow.active
                    && host.hostWindow.currentPage === 0
                    && app.session.connected) {
                app.home.ensureActivityFresh()
            }
        })
    }

    Connections {
        target: app.session
        function onStateChanged() {
            const generation = Number(app.session.generation || 0)
            const connected = Boolean(app.session.connected)
            const sessionChanged = host.observedSessionGeneration >= 0
                && generation !== host.observedSessionGeneration
            const disconnected = host.observedConnected && !connected
            const connectedNow = !host.observedConnected && connected
            host.observedSessionGeneration = generation
            host.observedConnected = connected
            if (sessionChanged || disconnected)
                PopupCoordinator.closeScope(host.hostWindow, true)
            if (connectedNow
                    && host.hostWindow.active
                    && host.hostWindow.currentPage === 0) {
                app.home.ensureActivityFresh()
            }
        }
    }

    Connections {
        target: app.status
        function onStateChanged() {
            if (!app.status.error)
                return
            host.hostWindow.forcePlaybackFromBeginning = false
            let handledInPlayer = false
            if (host.hostWindow.currentPage === 2
                    && host.hostWindow.playerPage
                    && (host.hostWindow.playerPage.preparingPlayback
                        || host.hostWindow.playerPage.switchingEpisode)) {
                handledInPlayer = true
            }
            if (!handledInPlayer && app.status.ready)
                host.hostWindow.showActionToast(app.status.message, "error")
            else if (!app.status.ready)
                host.hostWindow.showBackendError()
        }
    }

    Connections {
        target: host.hostWindow
        function onActiveChanged() {
            if (host.hostWindow.active
                    && host.hostWindow.currentPage === 0
                    && app.session.connected) {
                app.home.ensureActivityFresh()
            }
        }
    }

    Timer {
        interval: 60000
        repeat: true
        running: host.hostWindow.active
            && host.hostWindow.currentPage === 0
            && app.session.connected
        onTriggered: app.home.ensureActivityFresh()
    }
}
