import QtQuick
import QtQuick.Controls.Basic
import Yanami

Popup {
    id: root

    property string heading
    property var tracks: []
    property int selectedId: -1
    property bool allowOff: false
    property real lastWheelTime: 0
    property real wheelBoost: 1
    readonly property int entryCount: tracks.length + (allowOff ? 1 : 0)
    readonly property real listHeight: Math.min(292, entryCount * 44)
    readonly property real bodyHeight: entryCount === 0 ? 62 : listHeight
    signal trackSelected(int trackId)

    width: 288
    height: topPadding + bottomPadding + 36 + bodyHeight
    padding: 10
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        radius: 20
        color: "#F21A1D26"
        border.width: 1
        border.color: "#42FFFFFF"
    }

    contentItem: Column {
        width: parent.width

        Text {
            width: parent.width
            height: 36
            leftPadding: 10
            verticalAlignment: Text.AlignVCenter
            text: root.heading
            color: Theme.textMuted
            font.family: Theme.fontForText(text)
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }

        Text {
            visible: root.entryCount === 0
            width: parent.width
            height: root.bodyHeight
            text: qsTr("No tracks are available for this media")
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
            font.family: Theme.fontForText(text)
            font.pixelSize: 12
        }

        ListView {
            id: trackList
            visible: root.entryCount > 0
            width: parent.width
            height: root.listHeight
            clip: true
            model: root.entryCount
            boundsBehavior: Flickable.StopAtBounds
            boundsMovement: Flickable.StopAtBounds
            flickDeceleration: 1100
            maximumFlickVelocity: 7200
            pixelAligned: false
            reuseItems: true
            cacheBuffer: 88

            WheelHandler {
                target: null
                blocking: true
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: event => {
                    const now = Date.now()
                    root.wheelBoost = now - root.lastWheelTime < 170
                        ? Math.min(2.6, root.wheelBoost + 0.22)
                        : 1
                    root.lastWheelTime = now
                    const rawDelta = event.pixelDelta.y !== 0
                        ? event.pixelDelta.y
                        : event.angleDelta.y * 1.05
                    const currentTarget = wheelAnimation.running
                        ? wheelAnimation.to : trackList.contentY
                    const maximum = Math.max(0, trackList.contentHeight - trackList.height)
                    wheelAnimation.to = Math.max(0, Math.min(maximum,
                        currentTarget - rawDelta * root.wheelBoost))
                    wheelAnimation.restart()
                    event.accepted = true
                }
            }

            NumberAnimation {
                id: wheelAnimation
                target: trackList
                property: "contentY"
                duration: 230
                easing.type: Easing.OutCubic
            }

            delegate: Item {
                id: row
                required property int index
                readonly property bool offRow: root.allowOff && index === 0
                readonly property int trackIndex: index - (root.allowOff ? 1 : 0)
                readonly property var track: offRow ? null : root.tracks[trackIndex]
                readonly property int trackId: offRow ? -1 : Number(track.id)
                readonly property bool selected: offRow
                    ? root.selectedId < 0
                    : (track.selected === true || trackId === root.selectedId)
                readonly property string labelText: offRow
                    ? qsTr("Subtitles off")
                    : (track.label || qsTr("Unnamed track"))

                width: trackList.width
                height: 44

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 2
                    anchors.rightMargin: 2
                    radius: 12
                    color: rowMouse.pressed
                        ? "#2AFFFFFF"
                        : (rowMouse.containsMouse ? "#18FFFFFF"
                            : (row.selected ? "#14FF6687" : "transparent"))
                }

                Text {
                    anchors.left: parent.left
                    anchors.right: selectionMark.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 12
                    anchors.rightMargin: 10
                    text: row.labelText
                    color: row.selected ? Theme.text : Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 13
                    font.weight: row.selected ? Font.DemiBold : Font.Medium
                    elide: Text.ElideRight
                }

                Rectangle {
                    id: selectionMark
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.rightMargin: 12
                    width: 18
                    height: 18
                    radius: 9
                    color: row.selected ? Theme.accent : "transparent"
                    border.width: row.selected ? 0 : 1
                    border.color: "#48FFFFFF"

                    Rectangle {
                        visible: row.selected
                        anchors.centerIn: parent
                        width: 6
                        height: 6
                        radius: 3
                        color: "white"
                    }
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.trackSelected(row.trackId)
                }
            }

            ScrollBar.vertical: AppScrollBar {
                policy: trackList.contentHeight > trackList.height + 1
                    ? ScrollBar.AsNeeded
                    : ScrollBar.AlwaysOff
            }
        }
    }
}
