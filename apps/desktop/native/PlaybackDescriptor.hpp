#pragma once

#include <QJsonObject>
#include <QVariantMap>

namespace YanamiPlayback {

QVariantMap descriptorFromResponse(
    const QJsonObject &response,
    const QVariantMap &playbackContext);

} // namespace YanamiPlayback
