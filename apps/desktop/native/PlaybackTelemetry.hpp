#pragma once

#include "BackendPorts.hpp"

#include <QVariantMap>

namespace YanamiPlayback {

int telemetryVolume(double volume);
QVariantMap telemetryPayload(
    PlaybackPort::Event event,
    const PlaybackPort::Snapshot &snapshot);

} // namespace YanamiPlayback
