#include "PlaybackDescriptor.hpp"

#include <QJsonArray>
#include <QUrl>

namespace YanamiPlayback {

QVariantMap descriptorFromResponse(
    const QJsonObject &response,
    const QVariantMap &playbackContext)
{
    const QJsonValue introStart = response.value(QStringLiteral("introStartTicks"));
    const QJsonValue introEnd = response.value(QStringLiteral("introEndTicks"));
    return {
        {QStringLiteral("mediaUrl"),
            QUrl(response.value(QStringLiteral("url")).toString())},
        {QStringLiteral("headers"),
            response.value(QStringLiteral("headers")).toObject().toVariantMap()},
        {QStringLiteral("resumeTicks"),
            response.value(QStringLiteral("resumeTicks")).toVariant().toLongLong()},
        {QStringLiteral("title"), response.value(QStringLiteral("title")).toString()},
        {QStringLiteral("previousItem"),
            response.value(QStringLiteral("previousItem")).toObject().toVariantMap()},
        {QStringLiteral("nextItem"),
            response.value(QStringLiteral("nextItem")).toObject().toVariantMap()},
        {QStringLiteral("playbackContext"), playbackContext},
        {QStringLiteral("playbackQueue"),
            response.value(QStringLiteral("playbackQueue")).toArray().toVariantList()},
        {QStringLiteral("currentQueueIndex"),
            response.value(QStringLiteral("currentQueueIndex")).toInt(-1)},
        {QStringLiteral("externalSubtitles"),
            response.value(QStringLiteral("externalSubtitles")).toArray().toVariantList()},
        {QStringLiteral("playbackWarnings"),
            response.value(QStringLiteral("playbackWarnings")).toArray().toVariantList()},
        {QStringLiteral("introStartTicks"),
            introStart.isDouble() ? introStart.toVariant().toLongLong() : -1},
        {QStringLiteral("introEndTicks"),
            introEnd.isDouble() ? introEnd.toVariant().toLongLong() : -1},
        {QStringLiteral("itemId"), response.value(QStringLiteral("itemId")).toString()},
        {QStringLiteral("danmakuSearchAnime"),
            response.value(QStringLiteral("danmakuSearchAnime")).toString()},
        {QStringLiteral("danmakuSearchEpisode"),
            response.value(QStringLiteral("danmakuSearchEpisode")).toString()},
        {QStringLiteral("reportSessionId"),
            response.value(QStringLiteral("reportSessionId")).toString()},
        {QStringLiteral("audioTracks"),
            response.value(QStringLiteral("audioTracks")).toArray().toVariantList()},
        {QStringLiteral("subtitleTracks"),
            response.value(QStringLiteral("subtitleTracks")).toArray().toVariantList()},
    };
}

} // namespace YanamiPlayback
