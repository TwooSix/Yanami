#include "PlaybackTelemetry.hpp"

#include <algorithm>
#include <cmath>

namespace {

constexpr double ticksPerSecond = 10'000'000.0;

QString eventName(PlaybackPort::Event event)
{
    switch (event) {
    case PlaybackPort::Event::Started: return QStringLiteral("started");
    case PlaybackPort::Event::Progress: return QStringLiteral("progress");
    case PlaybackPort::Event::Stopped: return QStringLiteral("stopped");
    }
    return {};
}

} // namespace

namespace YanamiPlayback {

int telemetryVolume(double volume)
{
    if (std::isnan(volume))
        return 100;
    if (volume <= 0.0)
        return 0;
    if (volume >= 100.0)
        return 100;
    return static_cast<int>(std::lround(volume));
}

QVariantMap telemetryPayload(
    PlaybackPort::Event event,
    const PlaybackPort::Snapshot &snapshot)
{
    const double safeSeconds = std::isfinite(snapshot.positionSeconds)
        ? std::max(0.0, snapshot.positionSeconds) : 0.0;
    QVariantMap telemetry {
        {QStringLiteral("reportSessionId"), snapshot.reportSessionId},
        {QStringLiteral("event"), eventName(event)},
        {QStringLiteral("positionTicks"),
            static_cast<qulonglong>(std::llround(safeSeconds * ticksPerSecond))},
        {QStringLiteral("paused"), snapshot.paused},
        {QStringLiteral("muted"), snapshot.muted},
        {QStringLiteral("volume"), telemetryVolume(snapshot.volume)},
        {QStringLiteral("rate"),
            std::isfinite(snapshot.rate) ? snapshot.rate : 1.0},
        {QStringLiteral("seekable"), snapshot.seekable},
    };
    if (snapshot.audioStreamIndex >= 0) {
        telemetry.insert(QStringLiteral("audioStreamIndex"),
            snapshot.audioStreamIndex);
    }
    if (snapshot.subtitleStreamIndex >= 0) {
        telemetry.insert(QStringLiteral("subtitleStreamIndex"),
            snapshot.subtitleStreamIndex);
    }
    return telemetry;
}

} // namespace YanamiPlayback
