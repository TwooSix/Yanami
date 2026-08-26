import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    id: testCase

    name: "RecentEpisodeCard"
    width: 420
    height: 260
    visible: true
    when: windowShown

    Rectangle {
        anchors.fill: parent
        color: "#090B10"
        z: -10
    }

    RecentEpisodeCard {
        id: recentCard
        x: 40
        y: 30
        width: implicitWidth
        height: implicitHeight
        title: "Recent"
        subtitle: "S02E45"
        imageUrl: Qt.resolvedUrl("fixtures/solid-white.ppm")
        progress: 0
    }

    function init() {
        InputModality.notePointerInput()
        mouseMove(testCase, width - 2, height - 2)
    }

    function test_pointerHoverRemovesDecorativeSurfaceOutline() {
        const surface = findChild(recentCard, "media-card-surface")
        const progressStrip = findChild(
            recentCard, "media-card-progress-strip")
        const backdropLayer = findChild(
            recentCard, "media-card-backdrop-layer")
        const focusFrame = findChild(recentCard, "media-card-focus-frame")
        verify(surface !== null, "missing recent episode surface")
        verify(progressStrip !== null, "missing recent episode progress strip")
        verify(backdropLayer !== null, "missing flattened artwork backdrop")
        verify(focusFrame !== null, "missing recent episode focus frame")
        compare(backdropLayer.layer.enabled, true,
                "image and overlays must be flattened before hover scaling")
        compare(backdropLayer.layer.smooth, true)
        compare(backdropLayer.x, -1,
                "the flattened texture needs a transparent sampling gutter")
        compare(backdropLayer.width, surface.width + 2)
        compare(surface.border.width, 1)
        compare(progressStrip.opacity, 0,
                "an unstarted episode must not render a progress track")

        mouseMove(recentCard,
                  recentCard.width / 2, recentCard.height / 2)
        tryCompare(recentCard, "pointerHovered", true)
        tryCompare(surface, "scale", 1.012)
        compare(surface.border.width, 0,
                "pointer hover must not scale a light outline below the card")
        compare(progressStrip.opacity, 0)
        compare(focusFrame.visible, false,
                "pointer hover must not replace the keyboard focus frame")
    }

    function test_hoverZoomHasNoBrightTextureSeam() {
        const surface = findChild(recentCard, "media-card-surface")
        verify(surface !== null)
        tryCompare(recentCard, "artworkReady", true, 5000)

        mouseMove(recentCard,
                  recentCard.width / 2, recentCard.height / 2)
        tryCompare(recentCard, "pointerHovered", true)
        tryCompare(surface, "scale", 1.012)
        wait(50)

        const frame = grabImage(testCase)
        const dpr = frame.width / testCase.width
        const centerX = (recentCard.x + recentCard.width / 2) * dpr
        const halfSampleWidth = recentCard.width * 0.16 * dpr
        const visualBottom = (recentCard.y + recentCard.height / 2
            + recentCard.height * surface.scale / 2) * dpr
        const edgeY = Math.min(frame.height - 1, Math.floor(visualBottom))
        const innerY = Math.max(0, edgeY - Math.max(2, Math.ceil(2 * dpr)))
        const startX = Math.max(0, Math.floor(centerX - halfSampleWidth))
        const endX = Math.min(frame.width - 1,
                              Math.ceil(centerX + halfSampleWidth))

        function averageLuma(y) {
            let total = 0
            let count = 0
            for (let x = startX; x <= endX; ++x) {
                total += (frame.red(x, y) + frame.green(x, y)
                          + frame.blue(x, y)) / 3
                ++count
            }
            return count > 0 ? total / count : 0
        }

        const edgeLuma = averageLuma(edgeY)
        const innerLuma = averageLuma(innerY)
        verify(edgeLuma <= innerLuma + 24,
               "hover edge must not expose an undarkened image row: edge="
               + edgeLuma + ", inner=" + innerLuma)
    }
}
