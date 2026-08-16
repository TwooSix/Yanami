#include "PlaybackTrackMapper.hpp"

#include <QUrl>
#include <QVariantMap>

#include <limits>

namespace YanamiPlayback {
namespace {

QString normalizedText(const QVariantMap &track, const char *key)
{
    return track.value(QString::fromLatin1(key)).toString().trimmed().toCaseFolded();
}

QString normalizedUrl(const QVariant &value)
{
    QUrl url(value.toString().trimmed());
    if (!url.isValid() || url.isEmpty())
        return {};
    url.setFragment({});
    url = url.adjusted(QUrl::NormalizePathSegments | QUrl::StripTrailingSlash);
    return url.toString(QUrl::FullyEncoded);
}

std::optional<int> streamIndex(const QVariantMap &track)
{
    bool ok = false;
    const qlonglong value = track.value(QStringLiteral("streamIndex")).toLongLong(&ok);
    if (!ok || value < std::numeric_limits<int>::min()
        || value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

bool kindMatches(const QVariantMap &track, const QString &kind)
{
    const QString candidate = normalizedText(track, "kind");
    return !candidate.isEmpty()
        && candidate == kind.trimmed().toCaseFolded();
}

std::optional<QVariantMap> selectedTrack(
    const QVariantList &tracks, qint64 selectedMpvTrackId)
{
    for (const QVariant &entry : tracks) {
        const QVariantMap track = entry.toMap();
        bool ok = false;
        const qint64 id = track.value(QStringLiteral("mpvTrackId"),
                                      track.value(QStringLiteral("id")))
                              .toLongLong(&ok);
        if (ok && id == selectedMpvTrackId)
            return track;
    }
    return std::nullopt;
}

template<typename Predicate>
std::optional<int> uniqueMatch(
    const QVariantList &tracks, const QString &kind, Predicate predicate)
{
    std::optional<int> match;
    for (const QVariant &entry : tracks) {
        const QVariantMap track = entry.toMap();
        if (!kindMatches(track, kind) || !predicate(track))
            continue;
        const std::optional<int> candidate = streamIndex(track);
        if (!candidate)
            continue;
        if (match && match != candidate)
            return std::nullopt;
        match = candidate;
    }
    return match;
}

} // namespace

std::optional<int> selectedStreamIndex(
    const QVariantList &mpvTracks,
    qint64 selectedMpvTrackId,
    const QVariantList &embyTracks,
    const QString &kind)
{
    if (selectedMpvTrackId < 0)
        return std::nullopt;
    const std::optional<QVariantMap> selected =
        selectedTrack(mpvTracks, selectedMpvTrackId);
    if (!selected)
        return std::nullopt;

    const QString externalUrl = normalizedUrl(
        selected->value(QStringLiteral("externalUrl")));
    const bool external = !externalUrl.isEmpty();

    if (!external) {
        bool ok = false;
        const qlonglong ffIndex =
            selected->value(QStringLiteral("ffIndex")).toLongLong(&ok);
        if (ok) {
            const std::optional<int> exact = uniqueMatch(
                embyTracks, kind, [ffIndex](const QVariantMap &track) {
                    const std::optional<int> index = streamIndex(track);
                    return !track.value(QStringLiteral("external")).toBool()
                        && index && *index == ffIndex;
                });
            if (exact)
                return exact;
        }
    } else {
        const std::optional<int> exact = uniqueMatch(
            embyTracks, kind, [&externalUrl](const QVariantMap &track) {
                return track.value(QStringLiteral("external")).toBool()
                    && normalizedUrl(track.value(QStringLiteral("deliveryUrl")))
                        == externalUrl;
            });
        if (exact)
            return exact;
    }

    const QString title = normalizedText(*selected, "title");
    const QString language = normalizedText(*selected, "language");
    const QString codec = normalizedText(*selected, "codec");
    if (title.isEmpty() && language.isEmpty() && codec.isEmpty())
        return std::nullopt;

    return uniqueMatch(
        embyTracks, kind,
        [external, &title, &language, &codec](const QVariantMap &track) {
            if (track.value(QStringLiteral("external")).toBool() != external)
                return false;
            return (title.isEmpty() || normalizedText(track, "title") == title)
                && (language.isEmpty()
                    || normalizedText(track, "language") == language)
                && (codec.isEmpty() || normalizedText(track, "codec") == codec);
        });
}

} // namespace YanamiPlayback
