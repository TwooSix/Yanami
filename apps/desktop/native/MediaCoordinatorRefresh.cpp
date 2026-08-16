#include "MediaCoordinator.hpp"

#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrentRun>

#include <array>
#include <utility>

namespace {

constexpr qint64 requestedLibraryScanIndicatorStaleMs = 2 * 60 * 1000;
constexpr qint64 libraryScanCompletionRaceMs = 30 * 1000;
constexpr qint64 metadataRefreshTrackingMs = 2 * 60 * 1000;
constexpr std::array<qint64, 5> metadataRefreshFallbackDelaysMs {
    1500,
    4000,
    9000,
    18000,
    35000,
};

} // namespace

void MediaCoordinator::pollRefreshProgress()
{
    if (!activeSession() || m_refreshProgressWatcher.isRunning())
        return;
    m_refreshProgressSessionGeneration = currentSession().generation;
    m_refreshProgressWatcher.setFuture(QtConcurrent::run(
        &m_readPool,
        [this] { return m_runtime.refreshProgress(); }));
}

void MediaCoordinator::finishRefreshProgress()
{
    const YanamiOperationResult operationResult =
        m_refreshProgressWatcher.result();
    const MediaSessionState session = currentSession();
    if (m_sessionFenced
        && !m_shuttingDown
        && session.connected
        && m_refreshProgressSessionGeneration
            == session.generation) {
        // Rust consumes notification snapshots. Retain the single in-flight
        // snapshot until the authentication transition commits or aborts so
        // a failed login cannot lose scan/metadata completion events.
        m_bufferedRefreshProgress = operationResult;
        m_bufferedRefreshProgressSessionGeneration =
            m_refreshProgressSessionGeneration;
        return;
    }
    if (!acceptsSession(m_refreshProgressSessionGeneration))
        return;
    applyRefreshProgress(operationResult);
}

void MediaCoordinator::applyRefreshProgress(
    const YanamiOperationResult &operationResult)
{
    if (operationResult.status != 0) {
        // Refresh notifications are optional telemetry. The Rust transport
        // reconnects independently; a temporary outage is not a user-facing
        // media operation failure.
        qWarning().noquote()
            << "media_refresh_progress"
            << "phase=poll_failed"
            << "status=" << operationResult.status;
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        operationResult.payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return;
    }
    const QJsonObject object = document.object();
    if (!validateResponseSchema(object, "refresh-progress", false)
        || !object.value(QStringLiteral("items")).isArray()) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto iterator = m_recentRefreshCompletionsMs.begin();
         iterator != m_recentRefreshCompletionsMs.end();) {
        if (now - iterator.value() >= libraryScanCompletionRaceMs)
            iterator = m_recentRefreshCompletionsMs.erase(iterator);
        else
            ++iterator;
    }

    QVariantMap progress;
    QSet<QString> completed;
    const QJsonArray items =
        object.value(QStringLiteral("items")).toArray();
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        const QString itemId =
            item.value(QStringLiteral("itemId")).toString();
        if (itemId.isEmpty())
            continue;
        const qreal percentage = qBound<qreal>(
            0.0,
            item.value(QStringLiteral("progress")).toDouble(),
            100.0);
        if (item.value(QStringLiteral("complete")).toBool()
            || percentage >= 100.0) {
            completed.insert(itemId);
        } else if (m_requestedLibraryScans.contains(itemId)) {
            progress.insert(itemId, percentage);
        }
    }

    for (const QString &itemId :
         std::as_const(m_requestedLibraryScans)) {
        const qint64 requestedAt =
            m_libraryScanRequestedAtMs.value(itemId);
        if (progress.contains(itemId)) {
            m_hiddenLibraryScanIndicators.remove(itemId);
        } else if (!completed.contains(itemId)
                   && !m_hiddenLibraryScanIndicators.contains(itemId)
                   && requestedAt > 0
                   && now - requestedAt
                       < requestedLibraryScanIndicatorStaleMs) {
            progress.insert(itemId, 0.0);
        } else if (!completed.contains(itemId)
                   && !m_hiddenLibraryScanIndicators.contains(itemId)) {
            m_hiddenLibraryScanIndicators.insert(itemId);
            qWarning().noquote()
                << "media_refresh_progress"
                << "phase=indicator_expired"
                << "item=" << itemId
                << "reason=notification_stale";
        }
    }

    QSet<QString> requestedScanCompletions = completed;
    requestedScanCompletions.intersect(m_requestedLibraryScans);
    const QSet<QString> newlyCompletedScans =
        requestedScanCompletions - m_completedLibraryScans;
    m_completedLibraryScans.unite(requestedScanCompletions);
    QSet<QString> requestedMetadataCompletions = completed;
    requestedMetadataCompletions.intersect(
        m_requestedMetadataRefreshes);
    const QSet<QString> attributedCompletions =
        requestedScanCompletions | requestedMetadataCompletions;
    for (const QString &itemId : completed - attributedCompletions)
        m_recentRefreshCompletionsMs.insert(itemId, now);

    for (const QString &itemId : completed) {
        m_requestedLibraryScans.remove(itemId);
        m_libraryScanRequestedAtMs.remove(itemId);
        m_hiddenLibraryScanIndicators.remove(itemId);
        m_requestedMetadataRefreshes.remove(itemId);
        m_metadataRefreshRequestedAtMs.remove(itemId);
        m_metadataRefreshReconcileAttempts.remove(itemId);
    }
    if (m_libraryScanProgress != progress) {
        m_libraryScanProgress = progress;
        emit scanProgressChanged();
    }
    reconcileCompletedLibraryScans(newlyCompletedScans);
    reconcileCompletedMetadataRefreshes(
        requestedMetadataCompletions);
    if (m_requestedMetadataRefreshes.isEmpty()
        && m_refreshProtectionReleaseAtMs.isEmpty()) {
        m_metadataRefreshFallbackTimer.stop();
    }
}

void MediaCoordinator::trackLibraryScan(
    const QString &itemId,
    qint64 operationElapsedMs)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 completedAt =
        m_recentRefreshCompletionsMs.value(itemId);
    const qint64 requestStartedAt = operationElapsedMs >= 0
        ? now - operationElapsedMs : now;
    const bool alreadyCompleted = completedAt > 0
        && completedAt >= requestStartedAt
        && now - completedAt < libraryScanCompletionRaceMs;
    m_completedLibraryScans.remove(itemId);
    if (alreadyCompleted) {
        m_completedLibraryScans.insert(itemId);
        reconcileCompletedLibraryScans(QSet<QString> { itemId });
    } else {
        m_requestedLibraryScans.insert(itemId);
        m_libraryScanRequestedAtMs.insert(itemId, now);
        m_hiddenLibraryScanIndicators.remove(itemId);
        QVariantMap progress = m_libraryScanProgress;
        progress.insert(itemId, 0.0);
        if (progress != m_libraryScanProgress) {
            m_libraryScanProgress = progress;
            emit scanProgressChanged();
        }
    }
    pollRefreshProgress();
}

void MediaCoordinator::trackMetadataRefresh(
    const QString &itemId,
    qint64 operationElapsedMs)
{
    const quint64 sessionGeneration = currentSession().generation;
    // A new refresh supersedes any delayed release belonging to an earlier
    // completion for the same item.
    m_refreshProtectionReleaseAtMs.remove(itemId);
    m_refreshProtectedItems.insert(itemId);
    m_catalogSink.beginRefreshProtection(itemId);
    if (!acceptsSession(sessionGeneration)) {
        if (m_refreshProtectedItems.remove(itemId) > 0)
            m_catalogSink.endRefreshProtection(itemId);
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 completedAt =
        m_recentRefreshCompletionsMs.value(itemId);
    const qint64 requestStartedAt = operationElapsedMs >= 0
        ? now - operationElapsedMs : now;
    const bool alreadyCompleted = completedAt > 0
        && completedAt >= requestStartedAt
        && now - completedAt < libraryScanCompletionRaceMs;
    if (alreadyCompleted) {
        reconcileCompletedMetadataRefreshes(
            QSet<QString> { itemId });
    } else {
        m_requestedMetadataRefreshes.insert(itemId);
        m_metadataRefreshRequestedAtMs.insert(itemId, now);
        m_metadataRefreshReconcileAttempts.insert(itemId, 0);
        if (!m_metadataRefreshFallbackTimer.isActive())
            m_metadataRefreshFallbackTimer.start();
    }
    pollRefreshProgress();
}

void MediaCoordinator::reconcileCompletedLibraryScans(
    const QSet<QString> &itemIds)
{
    if (itemIds.isEmpty())
        return;
    qInfo().noquote()
        << "media_refresh_progress"
        << "phase=scan_complete"
        << "count=" << itemIds.size();
    m_catalogSink.scheduleContentReconciliation(
        QStringLiteral("library_scan_complete"));
}

void MediaCoordinator::reconcileCompletedMetadataRefreshes(
    const QSet<QString> &itemIds)
{
    if (itemIds.isEmpty())
        return;
    qInfo().noquote()
        << "media_refresh_progress"
        << "phase=metadata_complete"
        << "count=" << itemIds.size();
    m_catalogSink.scheduleContentReconciliation(
        QStringLiteral("metadata_complete"));
    const qint64 releaseAtMs =
        QDateTime::currentMSecsSinceEpoch() + 30000;
    for (const QString &itemId : itemIds) {
        m_refreshProtectionReleaseAtMs.insert(
            itemId, releaseAtMs);
    }
    if (!m_metadataRefreshFallbackTimer.isActive())
        m_metadataRefreshFallbackTimer.start();
}

void MediaCoordinator::pollMetadataRefreshFallbacks()
{
    if (!activeSession()) {
        m_metadataRefreshFallbackTimer.stop();
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto iterator = m_refreshProtectionReleaseAtMs.begin();
         iterator != m_refreshProtectionReleaseAtMs.end();) {
        if (now < iterator.value()) {
            ++iterator;
            continue;
        }
        const QString itemId = iterator.key();
        iterator = m_refreshProtectionReleaseAtMs.erase(iterator);
        if (m_refreshProtectedItems.remove(itemId) > 0)
            m_catalogSink.endRefreshProtection(itemId);
    }
    bool reconcile = false;
    for (auto iterator = m_requestedMetadataRefreshes.begin();
         iterator != m_requestedMetadataRefreshes.end();) {
        const QString itemId = *iterator;
        const qint64 requestedAt =
            m_metadataRefreshRequestedAtMs.value(itemId);
        const qint64 age = requestedAt > 0 ? now - requestedAt : 0;
        int attempt =
            m_metadataRefreshReconcileAttempts.value(itemId);
        if (attempt
                < static_cast<int>(
                    metadataRefreshFallbackDelaysMs.size())
            && age >= metadataRefreshFallbackDelaysMs.at(
                static_cast<size_t>(attempt))) {
            ++attempt;
            m_metadataRefreshReconcileAttempts.insert(itemId, attempt);
            reconcile = true;
            qInfo().noquote()
                << "media_refresh_progress"
                << "phase=fallback_reconcile"
                << "item=" << itemId
                << "attempt=" << attempt
                << "ageMs=" << age;
        }
        if (age >= metadataRefreshTrackingMs) {
            if (m_refreshProtectedItems.remove(itemId) > 0)
                m_catalogSink.endRefreshProtection(itemId);
            m_metadataRefreshRequestedAtMs.remove(itemId);
            m_metadataRefreshReconcileAttempts.remove(itemId);
            iterator = m_requestedMetadataRefreshes.erase(iterator);
        } else {
            ++iterator;
        }
    }
    if (reconcile) {
        m_catalogSink.scheduleContentReconciliation(
            QStringLiteral("metadata_fallback"));
    }
    if (m_requestedMetadataRefreshes.isEmpty()
        && m_refreshProtectionReleaseAtMs.isEmpty()) {
        m_metadataRefreshFallbackTimer.stop();
    }
}

void MediaCoordinator::resetRefreshTracking()
{
    for (const QString &itemId : std::as_const(m_refreshProtectedItems)) {
        m_catalogSink.endRefreshProtection(itemId);
    }
    m_requestedLibraryScans.clear();
    m_libraryScanRequestedAtMs.clear();
    m_hiddenLibraryScanIndicators.clear();
    m_completedLibraryScans.clear();
    m_requestedMetadataRefreshes.clear();
    m_refreshProtectedItems.clear();
    m_metadataRefreshRequestedAtMs.clear();
    m_metadataRefreshReconcileAttempts.clear();
    m_refreshProtectionReleaseAtMs.clear();
    m_recentRefreshCompletionsMs.clear();
    if (!m_libraryScanProgress.isEmpty()) {
        m_libraryScanProgress.clear();
        emit scanProgressChanged();
    }
}
