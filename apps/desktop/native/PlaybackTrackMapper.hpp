#pragma once

#include <QString>
#include <QVariantList>

#include <optional>

namespace YanamiPlayback {

// Resolves the selected libmpv track to the stream index understood by Emby.
// libmpv track ids are process-local identifiers and must never be reported as
// Emby stream indexes.
[[nodiscard]] std::optional<int> selectedStreamIndex(
    const QVariantList &mpvTracks,
    qint64 selectedMpvTrackId,
    const QVariantList &embyTracks,
    const QString &kind);

} // namespace YanamiPlayback
