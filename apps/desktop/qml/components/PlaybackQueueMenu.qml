pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

AppTransientPopup {
    id: root

    property var playbackContext: ({})
    property var queueItems: []
    property int currentIndex: -1
    property bool switching: false
    property bool paused: false
    property real maximumHeight: 520
    property real lastWheelTime: 0
    property real wheelBoost: 1

    readonly property int entryCount: queueItems ? queueItems.length : 0
    readonly property real headerHeight: 62
    readonly property real availableListHeight: Math.max(64,
        maximumHeight - topPadding - bottomPadding - headerHeight)
    readonly property real listHeight: entryCount === 0
        ? Math.min(78, availableListHeight)
        : Math.min(entryCount * 64, availableListHeight)
    readonly property string sourceTitle: contextText(
        ["sourceTitle", "title", "name"])
    readonly property string sourceType: contextText(
        ["sourceType", "kind", "type"]).toLowerCase()

    signal itemRequested(var item, int queueIndex)
    signal interactionStarted()
    signal interactionEnded()

    function contextText(keys) {
        const context = root.playbackContext || ({})
        for (let index = 0; index < keys.length; ++index) {
            const value = String(context[keys[index]] || "").trim()
            if (value.length > 0)
                return value
        }
        return ""
    }

    function queueIndexFor(item, fallbackIndex) {
        const value = Number((item || {}).queueIndex)
        return Number.isInteger(value) && value >= 0 ? value : fallbackIndex
    }

    function entryTitle(item) {
        return String((item || {}).title || (item || {}).name || qsTr("Untitled"))
    }

    function entrySubtitle(item) {
        const entry = item || ({})
        const direct = String(entry.subtitle || entry.label || "").trim()
        if (direct.length > 0)
            return direct
        const code = String(entry.episodeCode || entry.indexLabel || "").trim()
        const series = String(entry.seriesTitle || entry.seriesName || "").trim()
        if (series.length > 0 && code.length > 0)
            return series + "  ·  " + code
        return code.length > 0 ? code : series
    }

    function sourceDescription() {
        const title = root.sourceTitle
        if (root.sourceType === "playlist")
            return title.length > 0 ? qsTr("Playlist · %1").arg(title) : qsTr("Playlist")
        if (root.sourceType === "series" || root.sourceType === "episode")
            return title.length > 0 ? qsTr("Series · %1").arg(title) : qsTr("Series")
        if (root.sourceType === "season")
            return title.length > 0 ? qsTr("Season · %1").arg(title) : qsTr("Season")
        return title.length > 0 ? title : qsTr("Current playback")
    }

    function revealCurrent() {
        if (root.currentIndex < 0 || root.entryCount === 0)
            return
        let modelIndex = -1
        for (let index = 0; index < root.entryCount; ++index) {
            if (root.queueIndexFor(root.queueItems[index], index) === root.currentIndex) {
                modelIndex = index
                break
            }
        }
        if (modelIndex >= 0)
            queueList.positionViewAtIndex(modelIndex, ListView.Center)
    }

    width: 404
    height: topPadding + bottomPadding + headerHeight + listHeight
    padding: 10

    background: Rectangle {
        radius: 22
        color: "#F21A1D26"
        border.width: 1
        border.color: "#42FFFFFF"
    }

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 150; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 180; easing.type: Easing.OutCubic }
        }
    }

    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 120; easing.type: Easing.InCubic }
            NumberAnimation { property: "scale"; from: 1; to: 0.98; duration: 120; easing.type: Easing.InCubic }
        }
    }

    contentItem: Column {
        width: parent.width

        Item {
            width: parent.width
            height: root.headerHeight

            Column {
                anchors.left: parent.left
                anchors.right: positionLabel.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 10
                anchors.rightMargin: 14
                spacing: 3

                Text {
                    width: parent.width
                    text: qsTr("Play queue")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    text: root.sourceDescription()
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
            }

            Text {
                id: positionLabel
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: root.currentIndex >= 0 && root.entryCount > 0
                    ? qsTr("%1 of %2").arg(root.currentIndex + 1).arg(root.entryCount)
                    : qsTr("%1 items").arg(root.entryCount)
                color: Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 11
                font.weight: Font.Medium
            }
        }

        Item {
            visible: root.entryCount === 0
            width: parent.width
            height: root.listHeight

            Text {
                anchors.centerIn: parent
                width: parent.width - 24
                text: qsTr("Nothing else is queued")
                color: Theme.textMuted
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                font.family: Theme.fontForText(text)
                font.pixelSize: 12
            }
        }

        ListView {
            id: queueList
            visible: root.entryCount > 0
            width: parent.width
            height: root.listHeight
            clip: true
            model: root.queueItems
            boundsBehavior: Flickable.StopAtBounds
            boundsMovement: Flickable.StopAtBounds
            flickDeceleration: 1050
            maximumFlickVelocity: 6800
            pixelAligned: false
            reuseItems: true
            cacheBuffer: 128

            WheelHandler {
                target: null
                enabled: queueList.contentHeight > queueList.height + 1
                blocking: true
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: event => {
                    const now = Date.now()
                    root.wheelBoost = now - root.lastWheelTime < 160
                        ? Math.min(2.35, root.wheelBoost + 0.2)
                        : 1
                    root.lastWheelTime = now
                    const rawDelta = event.pixelDelta.y !== 0
                        ? event.pixelDelta.y : event.angleDelta.y * 1.2
                    const currentTarget = wheelAnimation.running
                        ? wheelAnimation.to : queueList.contentY
                    const maximum = Math.max(0,
                        queueList.contentHeight - queueList.height)
                    wheelAnimation.to = Math.max(0, Math.min(maximum,
                        currentTarget - rawDelta * root.wheelBoost))
                    wheelAnimation.restart()
                    event.accepted = true
                }
            }

            NumberAnimation {
                id: wheelAnimation
                target: queueList
                property: "contentY"
                duration: 200
                easing.type: Easing.OutCubic
            }

            delegate: Item {
                id: queueRow
                required property var modelData
                required property int index
                readonly property int itemQueueIndex: root.queueIndexFor(modelData, index)
                readonly property bool current: itemQueueIndex === root.currentIndex
                readonly property bool canActivate: !current && !root.switching
                    && String((modelData || {}).id || "").length > 0

                transform: Translate { x: Theme.scrollBarGutter }
                width: Math.max(0, queueList.width - 2 * Theme.scrollBarGutter)
                height: 64
                Accessible.role: Accessible.ListItem
                Accessible.name: root.entryTitle(modelData)
                    + (current ? ", " + qsTr("Playing") : "")

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    anchors.rightMargin: 2
                    anchors.topMargin: 2
                    anchors.bottomMargin: 2
                    radius: 14
                    color: queueMouse.pressed && queueRow.canActivate
                        ? "#2AFFFFFF"
                        : (queueMouse.containsMouse && queueRow.canActivate
                            ? "#18FFFFFF"
                            : (queueRow.current ? "#18FF6687" : "transparent"))
                    border.width: queueRow.current ? 1 : 0
                    border.color: "#42FF6687"

                    Behavior on color { ColorAnimation { duration: 130 } }
                    Behavior on border.color { ColorAnimation { duration: 130 } }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.leftMargin: 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: 3
                    height: queueRow.current ? 28 : 0
                    radius: 1.5
                    color: Theme.accent

                    Behavior on height {
                        NumberAnimation { duration: 170; easing.type: Easing.OutCubic }
                    }
                }

                Text {
                    id: entryNumber
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: 24
                    text: String(queueRow.itemQueueIndex + 1)
                    color: queueRow.current ? Theme.accent : Theme.textMuted
                    horizontalAlignment: Text.AlignHCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.Medium
                }

                Rectangle {
                    id: thumbnailFrame
                    anchors.left: entryNumber.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    width: 72
                    height: 42
                    radius: 10
                    color: "#273140"
                    clip: true

                    RoundedImage {
                        id: thumbnail
                        anchors.fill: parent
                        source: String((queueRow.modelData || {}).imageUrl || "")
                        radius: parent.radius
                        asynchronous: true
                        cache: true
                        fillMode: Image.PreserveAspectCrop
                    }

                    AppIcon {
                        visible: thumbnail.status !== Image.Ready
                        anchors.centerIn: parent
                        width: 16
                        height: 16
                        name: "play"
                        color: "#8E98AA"
                    }
                }

                Column {
                    anchors.left: thumbnailFrame.right
                    anchors.right: stateIcon.left
                    anchors.leftMargin: 12
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    Text {
                        width: parent.width
                        text: root.entryTitle(queueRow.modelData)
                        color: queueRow.current ? Theme.text : "#E9EDF5"
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 13
                        font.weight: queueRow.current ? Font.DemiBold : Font.Medium
                        elide: Text.ElideRight
                    }

                    Text {
                        width: parent.width
                        text: root.entrySubtitle(queueRow.modelData)
                        color: queueRow.current ? "#CF9EAC" : Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 11
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                }

                AppIcon {
                    id: stateIcon
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16
                    height: 16
                    visible: queueRow.current
                    name: root.paused ? "pause" : "play"
                    color: Theme.accent
                }

                MouseArea {
                    id: queueMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: queueRow.canActivate
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: root.itemRequested(queueRow.modelData,
                                                   queueRow.itemQueueIndex)
                }
            }

            ScrollBar.vertical: AppScrollBar {
                policy: queueList.contentHeight > queueList.height + 1
                    ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            }
        }
    }

    onOpened: {
        Qt.callLater(root.revealCurrent)
        root.interactionStarted()
    }
    onCurrentIndexChanged: {
        if (opened)
            Qt.callLater(root.revealCurrent)
    }
    onQueueItemsChanged: {
        wheelAnimation.stop()
        if (opened)
            Qt.callLater(root.revealCurrent)
    }
    onClosed: {
        wheelAnimation.stop()
        root.interactionEnded()
    }
}
