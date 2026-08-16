import QtQuick
import QtQuick.Effects

Item {
    id: root

    property url source
    property alias fillMode: sourceImage.fillMode
    property alias sourceSize: sourceImage.sourceSize
    property alias cache: sourceImage.cache
    property alias asynchronous: sourceImage.asynchronous
    property real radius: 0
    readonly property int status: sourceImage.status
    readonly property real effectiveDevicePixelRatio:
        typeof windowShell !== "undefined" && windowShell
            ? Math.max(1, Number(windowShell.devicePixelRatio || 1)) : 1

    Rectangle {
        id: mask
        anchors.fill: parent
        radius: root.radius
        color: "white"
        visible: false
        layer.enabled: true
    }

    Image {
        id: sourceImage
        property bool hasDisplayedPixels: false
        anchors.fill: parent
        source: root.source
        // Decode at the displayed width instead of retaining the full cached
        // poster. Leaving height at zero preserves the source aspect ratio.
        sourceSize: Qt.size(Math.max(1,
            Math.ceil(root.width * root.effectiveDevicePixelRatio)), 0)
        asynchronous: true
        cache: true
        retainWhileLoading: true
        fillMode: Image.PreserveAspectCrop
        opacity: status === Image.Ready
            || (status === Image.Loading && hasDisplayedPixels) ? 1 : 0

        onStatusChanged: {
            if (status === Image.Ready)
                hasDisplayedPixels = true
            else if (status === Image.Error || status === Image.Null)
                hasDisplayedPixels = false
        }
        layer.enabled: root.radius > 0
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: mask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }

        Behavior on opacity {
            NumberAnimation { duration: 190; easing.type: Easing.OutCubic }
        }

    }

}
