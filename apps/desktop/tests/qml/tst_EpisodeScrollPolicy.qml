import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    id: testCase
    name: "EpisodeScrollPolicy"
    width: 640
    height: 180
    visible: true
    when: windowShown

    Keys.priority: Keys.AfterItem
    Keys.onPressed: event => {
        let direction = ""
        if (event.key === Qt.Key_Left)
            direction = "left"
        else if (event.key === Qt.Key_Right)
            direction = "right"
        if (direction.length > 0)
            event.accepted = navigator.move(direction)
    }

    SpatialFocusNavigator {
        id: navigator
        navigationRoot: testCase
    }

    ListModel { id: episodeModel }

    QtObject {
        id: synchronizedModel
        property var rows: []
        readonly property int count: rows.length
        signal rowsSynchronized()
        function get(index) { return rows[index] }
    }

    SmoothHorizontalList {
        id: episodeList
        width: 260
        height: 96
        spacing: 10
        model: episodeModel
        delegate: Rectangle {
            required property int index
            width: 100
            height: 80
            activeFocusOnTab: true
            objectName: "episode-" + String(index)
        }
    }

    EpisodeScrollPolicy {
        id: policy
        activeScopeId: "season-a"
        model: episodeModel
        view: episodeList
        ready: true
    }

    SignalSpy {
        id: resolvedSpy
        target: policy
        signalName: "targetResolved"
    }

    SignalSpy {
        id: userScrollSpy
        target: episodeList
        signalName: "userScrollStarted"
    }

    function init() {
        policy.cancel()
        policy.activeScopeId = "season-a"
        policy.model = episodeModel
        policy.view = episodeList
        policy.ready = true
        policy.refreshing = false
        synchronizedModel.rows = []
        episodeModel.clear()
        episodeList.resetScrollPosition()
        wait(0)
        resolvedSpy.clear()
        userScrollSpy.clear()
    }

    function appendEpisode(id, played, progress) {
        episodeModel.append({
            "id": id,
            "played": played,
            "progress": progress || 0
        })
    }

    function populateEpisodes(count, firstUnplayed) {
        for (let index = 0; index < count; ++index) {
            appendEpisode("episode-" + String(index),
                index < firstUnplayed, index < firstUnplayed ? 100 : 0)
        }
    }

    function test_selectsFirstNotFullyPlayedEpisodeInModelOrder() {
        appendEpisode("episode-1", true, 100)
        appendEpisode("episode-2", false, 0)
        appendEpisode("episode-3", false, 62)

        policy.request("season-a")

        tryCompare(resolvedSpy, "count", 1)
        compare(resolvedSpy.signalArguments[0][0], "season-a")
        compare(resolvedSpy.signalArguments[0][1], 1)
        verify(!policy.pending)
    }

    function test_partiallyWatchedEpisodeIsStillUnplayed() {
        appendEpisode("episode-1", true, 100)
        appendEpisode("episode-2", false, 38)
        appendEpisode("episode-3", false, 0)

        policy.request("season-a")

        tryCompare(resolvedSpy, "count", 1)
        compare(resolvedSpy.signalArguments[0][1], 1)
    }

    function test_waitsForTheRequestedScopeAndFreshModel() {
        appendEpisode("stale-1", false, 0)
        policy.activeScopeId = "season-b"
        policy.refreshing = true
        policy.request("season-a")
        wait(0)
        compare(resolvedSpy.count, 0)
        verify(policy.pending)

        episodeModel.clear()
        appendEpisode("episode-1", true, 100)
        appendEpisode("episode-2", true, 100)
        appendEpisode("episode-3", false, 0)
        policy.activeScopeId = "season-a"
        wait(0)
        compare(resolvedSpy.count, 0)

        policy.refreshing = false
        tryCompare(resolvedSpy, "count", 1)
        compare(resolvedSpy.signalArguments[0][1], 2)
    }

    function test_waitsUntilTheCollectionIsReady() {
        appendEpisode("episode-1", true, 100)
        appendEpisode("episode-2", false, 0)
        policy.ready = false
        policy.request("season-a")
        wait(0)
        compare(resolvedSpy.count, 0)
        verify(policy.pending)

        policy.ready = true
        tryCompare(resolvedSpy, "count", 1)
        compare(resolvedSpy.signalArguments[0][1], 1)
    }

    function test_allPlayedFallsBackToTheBeginning() {
        appendEpisode("episode-1", true, 100)
        appendEpisode("episode-2", true, 100)

        policy.request("season-a")

        tryCompare(resolvedSpy, "count", 1)
        compare(resolvedSpy.signalArguments[0][1], 0)
    }

    function test_readyEmptySeasonResolvesWithoutADelegateTarget() {
        policy.request("season-a")

        tryCompare(resolvedSpy, "count", 1)
        compare(resolvedSpy.signalArguments[0][1], -1)
    }

    function test_settledRequestDoesNotRepositionAfterUserStateChanges() {
        populateEpisodes(12, 4)
        policy.request("season-a")
        tryCompare(resolvedSpy, "count", 1)
        tryVerify(function() {
            return episodeList.indexAt(
                episodeList.contentX + 1, episodeList.contentY + 1) === 4
        })

        episodeList.positionViewAtIndex(1, ListView.Beginning)
        const userPosition = episodeList.contentX
        episodeModel.setProperty(4, "played", true)
        episodeModel.setProperty(5, "played", false)
        wait(0)

        verify(Math.abs(episodeList.contentX - userPosition) < 0.5)
        compare(episodeList.indexAt(
            episodeList.contentX + 1, episodeList.contentY + 1), 1)
        compare(resolvedSpy.count, 1)
        verify(!policy.pending)
    }

    function test_positionsTheActualVirtualListAtTheResolvedEpisode() {
        populateEpisodes(12, 6)

        policy.request("season-a")

        tryCompare(resolvedSpy, "count", 1)
        tryVerify(function() {
            return episodeList.indexAt(
                episodeList.contentX + 1, episodeList.contentY + 1) === 6
        })
        verify(episodeList.contentX > episodeList.originX)
        compare(episodeList.currentIndex, 6)
        compare(userScrollSpy.count, 0)
    }

    function test_keyboardNavigationContinuesFromTheAutoPositionedEpisode() {
        populateEpisodes(12, 8)
        episodeList.currentIndex = 0
        episodeList.forceLayout()
        const first = episodeList.itemAtIndex(0)
        verify(first !== null)
        first.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(first, "activeFocus", true)

        policy.request("season-a")

        tryCompare(resolvedSpy, "count", 1)
        compare(episodeList.currentIndex, 8)
        tryVerify(function() {
            return episodeList.indexAt(
                episodeList.contentX + 1, episodeList.contentY + 1) === 8
        })
        const positioned = episodeList.itemAtIndex(8)
        verify(positioned !== null)
        verify(navigator.focusItem(positioned))
        const settledPosition = episodeList.contentX
        keyClick(Qt.Key_Right)
        tryCompare(episodeList, "currentIndex", 9)
        verify(episodeList.contentX >= settledPosition - 0.5)
        verify(episodeList.indexAt(
            episodeList.contentX + 1, episodeList.contentY + 1) >= 8)
    }

    function test_sameSeasonCancelAndReentryRejectsTheOldGeneration() {
        policy.ready = false
        populateEpisodes(12, 3)
        policy.request("season-a")
        const firstGeneration = policy.generation
        policy.cancel()

        episodeModel.clear()
        populateEpisodes(12, 7)
        policy.request("season-a")
        const secondGeneration = policy.generation
        verify(secondGeneration > firstGeneration)
        policy.ready = true

        tryCompare(resolvedSpy, "count", 1)
        compare(resolvedSpy.signalArguments[0][1], 7)
        tryVerify(function() {
            return episodeList.indexAt(
                episodeList.contentX + 1, episodeList.contentY + 1) === 7
        })
    }

    function test_userScrollSignalCancelsPendingAutoPosition() {
        populateEpisodes(12, 8)
        policy.refreshing = true
        policy.request("season-a")
        wait(0)
        verify(policy.pending)

        episodeList.forceLayout()
        episodeList.positionViewAtIndex(2, ListView.Beginning)
        const userPosition = episodeList.contentX
        episodeList.userScrollStarted()
        verify(!policy.pending)

        policy.refreshing = false
        wait(0)
        compare(resolvedSpy.count, 0)
        verify(Math.abs(episodeList.contentX - userPosition) < 0.5)
        compare(episodeList.indexAt(
            episodeList.contentX + 1, episodeList.contentY + 1), 2)
    }

    function test_keyboardNavigationCancelsPendingWithoutBlockingTheList() {
        populateEpisodes(12, 8)
        episodeList.forceLayout()
        episodeList.currentIndex = 0
        const first = episodeList.itemAtIndex(0)
        verify(first !== null)
        first.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(first, "activeFocus", true)
        policy.refreshing = true
        policy.request("season-a")
        verify(policy.pending)

        keyClick(Qt.Key_Right)

        tryCompare(userScrollSpy, "count", 1)
        verify(!policy.pending)
        compare(episodeList.currentIndex, 1)
        policy.refreshing = false
        wait(0)
        compare(resolvedSpy.count, 0)
    }

    function test_unsupportedHomeKeyDoesNotCancelPendingPositioning() {
        populateEpisodes(12, 8)
        episodeList.forceLayout()
        episodeList.currentIndex = 0
        episodeList.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(episodeList, "activeFocus", true)
        policy.refreshing = true
        policy.request("season-a")

        keyClick(Qt.Key_Home)

        compare(userScrollSpy.count, 0)
        verify(policy.pending)
        policy.refreshing = false
        tryCompare(resolvedSpy, "count", 1)
        compare(resolvedSpy.signalArguments[0][1], 8)
    }

    function test_rowsSynchronizedSignalSettlesTheCurrentGeneration() {
        policy.model = synchronizedModel
        policy.view = null
        policy.refreshing = true
        policy.request("season-a")
        wait(0)
        compare(resolvedSpy.count, 0)

        synchronizedModel.rows = [
            { "id": "episode-1", "played": true },
            { "id": "episode-2", "played": false }
        ]
        synchronizedModel.rowsSynchronized()
        policy.refreshing = false

        tryCompare(resolvedSpy, "count", 1)
        compare(resolvedSpy.signalArguments[0][1], 1)
    }

    function test_cancelPreventsLateResolution() {
        appendEpisode("episode-1", false, 0)
        policy.ready = false
        policy.request("season-a")
        policy.cancel()
        policy.ready = true
        wait(0)

        compare(resolvedSpy.count, 0)
        verify(!policy.pending)
    }
}
