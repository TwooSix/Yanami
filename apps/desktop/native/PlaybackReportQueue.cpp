#include "PlaybackReportQueue.hpp"

namespace YanamiPlayback {

void enqueueReport(
    QQueue<YanamiPlaybackReportRequest> &queue,
    const YanamiPlaybackReportRequest &request)
{
    if (request.event == PlaybackPort::Event::Progress
        || request.event == PlaybackPort::Event::Stopped) {
        for (auto iterator = queue.begin(); iterator != queue.end();) {
            if (iterator->event == PlaybackPort::Event::Progress
                && iterator->snapshot.reportSessionId
                    == request.snapshot.reportSessionId) {
                iterator = queue.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }
    queue.enqueue(request);
}

QString latestStoppedReportSessionId(
    const std::optional<YanamiPlaybackReportRequest> &active,
    const QQueue<YanamiPlaybackReportRequest> &queue)
{
    QString result;
    if (active.has_value()
        && active->event == PlaybackPort::Event::Stopped) {
        result = active->snapshot.reportSessionId;
    }
    for (const YanamiPlaybackReportRequest &request : queue) {
        if (request.event == PlaybackPort::Event::Stopped)
            result = request.snapshot.reportSessionId;
    }
    return result;
}

bool belongsToSession(
    const YanamiPlaybackReportRequest &request,
    quint64 sessionGeneration,
    bool sessionTransitioning)
{
    return !sessionTransitioning
        && request.sessionGeneration == sessionGeneration;
}

void discardForeignSessionReports(
    QQueue<YanamiPlaybackReportRequest> &queue,
    quint64 sessionGeneration,
    bool sessionTransitioning)
{
    for (auto iterator = queue.begin(); iterator != queue.end();) {
        if (!belongsToSession(
                *iterator, sessionGeneration, sessionTransitioning)) {
            iterator = queue.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

} // namespace YanamiPlayback
