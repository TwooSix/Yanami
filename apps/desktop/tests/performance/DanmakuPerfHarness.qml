import QtQuick
import Yanami.Ui

Window {
    id: window
    objectName: "danmakuPerfWindow"
    width: 1920
    height: 1080
    visible: true
    color: "#050505"

    DanmakuOverlay {
        objectName: "danmakuPerfOverlay"
        anchors.fill: parent
        comments: []
        danmakuEnabled: true
        mediaPosition: 0
        paused: true
        buffering: false
        playbackRate: 1
        fontSize: 36
        commentOpacity: 0.9
        scrollDuration: 9
        displayArea: 1
        density: 64
        blockedTerms: ""
        showScroll: true
        showTop: true
        showBottom: true
    }
}
