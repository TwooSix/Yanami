import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    name: "PlaybackAdvancePolicy"

    PlaybackAdvancePolicy { id: policy }

    function test_queueNextTakesPriority() {
        const queue = [
            { "id": "episode-1", "queueIndex": 0 },
            { "id": "episode-2", "queueIndex": 1 }
        ]
        const decision = policy.decide(
            queue, 0, { "id": "fallback" }, true)
        compare(decision.action, "open-next")
        compare(decision.item.id, "episode-2")
        compare(decision.queueIndex, 1)
    }

    function test_adjacentItemIsTheFallback() {
        const decision = policy.decide(
            [{ "id": "episode-1", "queueIndex": 0 }],
            0, { "id": "episode-2" }, true)
        compare(decision.action, "open-next")
        compare(decision.item.id, "episode-2")
        compare(decision.queueIndex, 1)
    }

    function test_resolvedQueueWithoutCandidateCompletes() {
        const decision = policy.decide([], 0, {}, true)
        compare(decision.action, "complete")
    }

    function test_unresolvedQueueWithoutCandidateRefreshes() {
        const decision = policy.decide([], 0, {}, false)
        compare(decision.action, "refresh-queue")
    }
}
