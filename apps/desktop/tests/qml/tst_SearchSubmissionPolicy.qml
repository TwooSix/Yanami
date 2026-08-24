import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    name: "SearchSubmissionPolicy"

    SearchSubmissionPolicy {
        id: policy
        delayMs: 60
    }

    SignalSpy {
        id: submitSpy
        target: policy
        signalName: "submitRequested"
    }

    function init() {
        policy.cancel()
        policy.composing = false
        policy.input = ""
        policy.hasSubmitted = false
        policy.lastSubmitted = ""
        policy.divergedSinceSubmit = false
        submitSpy.clear()
    }

    function cleanup() {
        policy.cancel()
    }

    function test_burstTypingSubmitsOnlyTrailingQuery() {
        policy.input = "h"
        wait(10)
        policy.input = "ha"
        wait(10)
        policy.input = "hah"
        wait(10)
        policy.input = "haha"

        compare(submitSpy.count, 0)
        verify(policy.pending)
        tryCompare(submitSpy, "count", 1, 500)
        compare(submitSpy.signalArguments[0][0], "haha")
        compare(policy.pending, false)
    }

    function test_clearCancelsPendingAndSubmitsSynchronously() {
        policy.input = "pending"
        verify(policy.pending)

        policy.input = ""
        compare(submitSpy.count, 1)
        compare(submitSpy.signalArguments[0][0], "")
        compare(policy.pending, false)

        wait(policy.delayMs + 30)
        compare(submitSpy.count, 1)
    }

    function test_clearWhileComposingSubmitsSynchronouslyOnce() {
        policy.input = "submitted"
        tryCompare(submitSpy, "count", 1, 500)
        policy.composing = true
        policy.input = "draft"
        submitSpy.clear()

        policy.input = ""
        compare(submitSpy.count, 1)
        compare(submitSpy.signalArguments[0][0], "")
        compare(policy.pending, false)

        policy.composing = false
        wait(policy.delayMs + 30)
        compare(submitSpy.count, 1)
    }

    function test_compositionDefersUntilCommit() {
        policy.composing = true
        policy.input = "かな"
        wait(policy.delayMs + 30)
        compare(submitSpy.count, 0)
        compare(policy.pending, false)

        policy.composing = false
        verify(policy.pending)
        tryCompare(submitSpy, "count", 1, 500)
        compare(submitSpy.signalArguments[0][0], "かな")
    }

    function test_compositionCancelDoesNotRepeatSubmittedQuery() {
        policy.input = "stable"
        tryCompare(submitSpy, "count", 1, 500)
        submitSpy.clear()

        policy.composing = true
        policy.composing = false
        wait(policy.delayMs + 30)
        compare(submitSpy.count, 0)
    }

    function test_normalizedDuplicateIsSkippedButForceCanResubmit() {
        policy.input = "haha"
        tryCompare(submitSpy, "count", 1, 500)
        submitSpy.clear()

        policy.input = " haha "
        wait(policy.delayMs + 30)
        compare(submitSpy.count, 0)

        policy.forceSchedule()
        tryCompare(submitSpy, "count", 1, 500)
        compare(submitSpy.signalArguments[0][0], "haha")
    }

    function test_returningToSubmittedQueryResubmitsAfterDivergence() {
        policy.input = "abc"
        tryCompare(submitSpy, "count", 1, 500)
        submitSpy.clear()

        policy.input = "abcd"
        verify(policy.pending)
        policy.input = "abc"

        verify(policy.pending)
        tryCompare(submitSpy, "count", 1, 500)
        compare(submitSpy.signalArguments[0][0], "abc")
        compare(policy.divergedSinceSubmit, false)
    }
}
