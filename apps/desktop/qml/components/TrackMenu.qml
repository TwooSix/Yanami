import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models
import Yanami.Ui

AppMenu {
    id: root

    property string heading
    property var tracks: []
    property int selectedId: -1
    property bool allowOff: false
    readonly property int entryCount: tracks.length + (allowOff ? 1 : 0)
    readonly property real bodyHeight: entryCount === 0
        ? 62 : Math.min(292, entryCount * 44)
    signal trackSelected(int trackId)

    function selectedEntryIndex() {
        if (root.allowOff && root.selectedId < 0)
            return 0
        for (let index = 0; index < root.tracks.length; ++index) {
            const track = root.tracks[index] || ({})
            if (track.selected === true
                    || Number(track.id) === root.selectedId)
                return index + (root.allowOff ? 1 : 0)
        }
        return 0
    }

    function focusRelative(index, step) {
        if (root.entryCount <= 0)
            return
        const next = (index + step + root.entryCount) % root.entryCount
        root.focusItem(next)
    }

    width: 288
    height: topPadding + bottomPadding + bodyHeight
    leftPadding: 10
    rightPadding: 10
    topPadding: 46
    bottomPadding: 10
    preferredCurrentIndex: root.selectedEntryIndex()
    title: root.heading

    background: Rectangle {
        radius: 20
        color: "#F21A1D26"
        border.width: 1
        border.color: "#42FFFFFF"

        Text {
            x: 20
            y: 10
            width: parent.width - 40
            height: 36
            verticalAlignment: Text.AlignVCenter
            text: root.heading
            color: Theme.textMuted
            font.family: Theme.fontForText(text)
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }

        Text {
            visible: root.entryCount === 0
            x: 20
            y: root.topPadding
            width: parent.width - 40
            height: root.bodyHeight
            text: qsTr("No tracks are available for this media")
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.WordWrap
            font.family: Theme.fontForText(text)
            font.pixelSize: 12
        }
    }

    Instantiator {
        model: root.entryCount

        delegate: MenuItem {
            id: row
            required property int index
            readonly property bool offRow: root.allowOff && index === 0
            readonly property int trackIndex: index - (root.allowOff ? 1 : 0)
            readonly property var track: offRow
                ? null : (root.tracks[trackIndex] || ({}))
            readonly property int trackId: offRow ? -1 : Number(track.id)
            readonly property bool selected: offRow
                ? root.selectedId < 0
                : (track.selected === true || trackId === root.selectedId)
            readonly property string labelText: offRow
                ? qsTr("Subtitles off")
                : (track.label || qsTr("Unnamed track"))

            width: root.availableWidth
            height: 44
            padding: 0
            text: row.labelText
            checkable: true
            checked: row.selected
            autoExclusive: true
            // Selection is rendered by selectionMark on the right. Suppress
            // the Basic style's default left-side check indicator so the row
            // does not show two competing selection marks.
            indicator: null
            hoverEnabled: true
            readonly property bool keyboardCurrent:
                root.keyboardFocusVisible && root.currentIndex === row.index
            Accessible.id: "track-menu-item-" + row.index
            Accessible.name: row.labelText
            Accessible.focusable: true
            Accessible.focused: row.activeFocus
            Accessible.selectable: true
            Accessible.selected: row.selected

            onTriggered: {
                root.close()
                root.trackSelected(row.trackId)
            }
            Keys.onUpPressed: event => {
                root.focusRelative(row.index, -1)
                event.accepted = true
            }
            Keys.onDownPressed: event => {
                root.focusRelative(row.index, 1)
                event.accepted = true
            }
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Home)
                    root.focusItem(0)
                else if (event.key === Qt.Key_End)
                    root.focusItem(root.entryCount - 1)
                else
                    return
                event.accepted = true
            }

            background: Rectangle {
                anchors.fill: parent
                anchors.leftMargin: 2
                anchors.rightMargin: 2
                radius: 12
                color: row.down
                    ? "#2AFFFFFF"
                    : ((row.keyboardCurrent
                        || (!root.keyboardFocusVisible
                            && row.hovered))
                        ? "#20FFFFFF" : "transparent")
                // Mouse use is fill-only. The accent frame appears only
                // after a keyboard invocation or keyboard navigation.
                border.width: row.keyboardCurrent ? 2 : 0
                border.color: Theme.accent
            }

            contentItem: Item {
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
            }
        }

        onObjectAdded: (index, object) => root.insertItem(index, object)
        onObjectRemoved: (index, object) => root.removeItem(object)
    }
}
