#pragma once

#include "BackendPorts.hpp"

#include <QQueue>
#include <QString>

#include <optional>

struct YanamiPlaybackReportRequest
{
    PlaybackPort::Event event = PlaybackPort::Event::Progress;
    PlaybackPort::Snapshot snapshot;
    quint64 sessionGeneration = 0;
};

namespace YanamiPlayback {

void enqueueReport(
    QQueue<YanamiPlaybackReportRequest> &queue,
    const YanamiPlaybackReportRequest &request);

QString latestStoppedReportSessionId(
    const std::optional<YanamiPlaybackReportRequest> &active,
    const QQueue<YanamiPlaybackReportRequest> &queue);

bool belongsToSession(
    const YanamiPlaybackReportRequest &request,
    quint64 sessionGeneration,
    bool sessionTransitioning);

void discardForeignSessionReports(
    QQueue<YanamiPlaybackReportRequest> &queue,
    quint64 sessionGeneration,
    bool sessionTransitioning);

} // namespace YanamiPlayback
