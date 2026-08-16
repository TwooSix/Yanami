#include "MediaCoordinator.hpp"

#include "BackendInfrastructure.hpp"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedValueRollback>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace {

constexpr qsizetype maximumPendingMutations = 32;

int operationCode(MediaPort::Operation operation)
{
    return static_cast<int>(operation);
}

bool isImageMutation(MediaPort::Operation operation)
{
    return operation == MediaPort::Operation::ApplyRemoteImage
        || operation == MediaPort::Operation::UploadImage
        || operation == MediaPort::Operation::RemoveImage;
}

bool usesOptimisticCatalogJournal(MediaPort::Operation operation)
{
    return operation == MediaPort::Operation::SetPlayed
        || operation == MediaPort::Operation::SetFavorite;
}

bool isNonModal(MediaPort::Operation operation)
{
    return operation != MediaPort::Operation::DeleteItem;
}

} // namespace

void MediaCoordinator::submitMutation(MutationRequest request)
{
    if (m_mutations.queuedCount() + m_mutations.activeCount()
        >= maximumPendingMutations) {
        fail(request,
            tr("Too many operations are waiting. Please try again."),
            !isNonModal(request.operation));
        return;
    }
    if (!m_mutations.enqueue(request)) {
        fail(request,
            tr("The media operation could not be queued."),
            !isNonModal(request.operation));
        return;
    }
    // Fence reads only after the mutation has a scheduler slot. A rejected
    // write must not cancel an otherwise valid editor/resource request.
    fenceInteractiveReads(request, "conflicting_mutation");
    qInfo().noquote()
        << "media_mutation"
        << "phase=queued"
        << "requestId=" << request.submissionSequence
        << "item=" << request.itemId
        << "operation=" << operationCode(request.operation)
        << "activeItems=" << m_mutations.activeCount()
        << "queued=" << m_mutations.queuedCount();
    dispatchMutations();
}

void MediaCoordinator::dispatchMutations()
{
    if (!activeSession()
        || m_settlingMutation.has_value()
        || m_dispatchingMutations) {
        return;
    }
    QScopedValueRollback dispatchGuard(
        m_dispatchingMutations, true);
    const QVector<MutationRequest> ready = m_mutations.takeReady();
    for (MutationRequest request : ready) {
        if (!activeSession()) {
            m_mutations.complete(request.itemId);
            fail(request,
                tr("The operation was canceled because the Emby session changed."),
                !isNonModal(request.operation));
            continue;
        }
        auto lane = std::find_if(
            m_mutationLanes.begin(),
            m_mutationLanes.end(),
            [](const MutationLane &candidate) {
                return !candidate.active.has_value()
                    && !candidate.watcher.isRunning();
            });
        Q_ASSERT(lane != m_mutationLanes.end());
        if (lane == m_mutationLanes.end()) {
            qCritical().noquote()
                << "media_mutation"
                << "phase=dispatch_failed"
                << "requestId=" << request.submissionSequence
                << "reason=no_free_lane";
            m_mutations.complete(request.itemId);
            fail(request,
                tr("The media operation could not be started."),
                !isNonModal(request.operation));
            QTimer::singleShot(
                0, this, &MediaCoordinator::dispatchMutations);
            continue;
        }
        startMutation(*lane, std::move(request));
    }
}

void MediaCoordinator::startMutation(
    MutationLane &lane,
    MutationRequest request)
{
    lane.active = std::move(request);
    MutationRequest &active = *lane.active;
    const bool startsTrackedRefresh =
        active.operation == Operation::ScanLibraryFiles
        || active.operation == Operation::RefreshMetadata;
    if (startsTrackedRefresh
        && (m_requestedLibraryScans.contains(active.itemId)
            || m_requestedMetadataRefreshes.contains(active.itemId))) {
        const quint64 sequence = active.submissionSequence;
        const QString itemId = active.itemId;
        m_settledMutationSequences.insert(sequence);
        fail(active,
            tr("A library or metadata refresh is already in progress for this item."),
            false);
        lane.active.reset();
        m_settledMutationSequences.remove(sequence);
        m_mutations.complete(itemId);
        QTimer::singleShot(
            0, this, &MediaCoordinator::dispatchMutations);
        return;
    }
    if (usesOptimisticCatalogJournal(active.operation)) {
        const QString mutationId =
            m_catalogSink.beginOptimisticStateMutation(
                active.itemId,
                active.operation,
                active.payload,
                active.submissionSequence);
        active.optimisticMutationId = mutationId;
        if (!acceptsSession(active.sessionGeneration)
            || m_settledMutationSequences.contains(
                active.submissionSequence)) {
            const quint64 sequence = active.submissionSequence;
            const QString itemId = active.itemId;
            if (!mutationId.isEmpty()) {
                m_catalogSink.rollbackOptimisticStateMutation(
                    mutationId);
            }
            lane.active.reset();
            m_mutations.complete(itemId);
            m_settledMutationSequences.remove(sequence);
            m_speculativeMutationSequences.remove(sequence);
            QTimer::singleShot(
                0, this, &MediaCoordinator::dispatchMutations);
            QTimer::singleShot(
                0, this, &MediaCoordinator::pumpInteractiveReads);
            return;
        }
    }

    const quint64 transportRequestId = active.submissionSequence;
    const QString itemId = active.itemId;
    const Operation operation = active.operation;
    const QVariantMap payload = active.payload;
    const qint64 enqueuedAtMs = active.enqueuedAtMs;
    lane.timer.start();
    qInfo().noquote()
        << "media_mutation"
        << "phase=start"
        << "requestId=" << transportRequestId
        << "sessionGeneration=" << lane.active->sessionGeneration
        << "item=" << itemId
        << "operation=" << operationCode(operation);
    lane.watcher.setFuture(QtConcurrent::run(
        &m_mutationPool,
        [this, operation, payload, transportRequestId,
         itemId, enqueuedAtMs] {
            const qint64 queueWaitMs = enqueuedAtMs > 0
                ? QDateTime::currentMSecsSinceEpoch() - enqueuedAtMs : 0;
            qInfo().noquote()
                << "media_mutation"
                << "phase=transport_start"
                << "requestId=" << transportRequestId
                << "item=" << itemId
                << "operation=" << operationCode(operation)
                << "queueWaitMs=" << queueWaitMs;
            return m_runtime.media(operation, itemId, payload);
        }));
}

void MediaCoordinator::finishMutation(MutationLane &lane)
{
    const YanamiOperationResult operationResult = lane.watcher.result();
    const qint64 elapsedMs = lane.timer.isValid()
        ? lane.timer.elapsed() : -1;
    lane.timer.invalidate();
    if (!lane.active.has_value()) {
        qWarning().noquote()
            << "media_mutation"
            << "phase=finish"
            << "outcome=missing_active_request";
        QTimer::singleShot(0, this, &MediaCoordinator::dispatchMutations);
        return;
    }

    if (m_settlingMutation.has_value()) {
        MutationLane *lanePointer = &lane;
        qInfo().noquote()
            << "media_mutation"
            << "phase=finish_deferred"
            << "reason=result_commit_in_progress";
        QTimer::singleShot(0, this,
            [this, lanePointer] { finishMutation(*lanePointer); });
        return;
    }
    m_settlingMutation = std::move(*lane.active);
    lane.active.reset();
    MutationRequest &request = *m_settlingMutation;
    const bool consumerAlreadySettled =
        m_settledMutationSequences.contains(
            request.submissionSequence);
    const bool accepted = !consumerAlreadySettled
        && acceptsSession(request.sessionGeneration);
    qInfo().noquote()
        << "media_mutation"
        << "phase=finish"
        << "requestId=" << request.submissionSequence
        << "requestSessionGeneration=" << request.sessionGeneration
        << "currentSessionGeneration=" << currentSession().generation
        << "item=" << request.itemId
        << "operation=" << operationCode(request.operation)
        << "elapsedMs=" << elapsedMs
        << "status=" << operationResult.status
        << "accepted=" << accepted;

    if (accepted) {
        applyMutationResult(
            request, operationResult, elapsedMs);
    } else if (!request.optimisticMutationId.isEmpty()) {
        m_catalogSink.rollbackOptimisticStateMutation(
            request.optimisticMutationId);
        request.optimisticMutationId.clear();
    }
    const QString completedItemId = request.itemId;
    const quint64 completedSequence = request.submissionSequence;
    const bool speculativeOutcome =
        m_speculativeMutationSequences.remove(completedSequence);
    if (speculativeOutcome
        && request.sessionGeneration == currentSession().generation) {
        if (m_sessionFenced) {
            m_completedSpeculativeMutationItems.insert(completedItemId);
        } else {
            m_catalogSink.scheduleContentReconciliation(
                QStringLiteral("session_transition_aborted_mutation"));
        }
    }
    m_settlingMutation.reset();
    m_settledMutationSequences.remove(completedSequence);
    m_mutations.complete(completedItemId);
    QTimer::singleShot(0, this, &MediaCoordinator::dispatchMutations);
    QTimer::singleShot(0, this, &MediaCoordinator::pumpInteractiveReads);
}

void MediaCoordinator::resetMutations(const char *reason)
{
    const QString message =
        tr("The operation was canceled because the Emby session changed.");
    const QVector<MutationRequest> queued = m_mutations.takeQueued();
    for (const MutationRequest &request : queued) {
        qInfo().noquote()
            << "media_mutation"
            << "phase=dropped"
            << "requestId=" << request.submissionSequence
            << "item=" << request.itemId
            << "operation=" << operationCode(request.operation)
            << "reason=" << reason;
        fail(request, message, !isNonModal(request.operation));
    }
    for (MutationLane &lane : m_mutationLanes) {
        if (!lane.active.has_value())
            continue;
        MutationRequest &request = *lane.active;
        if (m_settledMutationSequences.contains(
                request.submissionSequence)) {
            continue;
        }
        m_settledMutationSequences.insert(request.submissionSequence);
        if (!m_shuttingDown) {
            m_speculativeMutationSequences.insert(
                request.submissionSequence);
        }
        if (!request.optimisticMutationId.isEmpty()) {
            m_catalogSink.rollbackOptimisticStateMutation(
                request.optimisticMutationId);
            request.optimisticMutationId.clear();
        }
        qInfo().noquote()
            << "media_mutation"
            << "phase=consumer_settled"
            << "requestId=" << request.submissionSequence
            << "item=" << request.itemId
            << "operation=" << operationCode(request.operation)
            << "reason=" << reason;
        fail(request, message, !isNonModal(request.operation));
    }
    if (m_settlingMutation.has_value()) {
        MutationRequest &request = *m_settlingMutation;
        if (!m_settledMutationSequences.contains(
                request.submissionSequence)) {
            m_settledMutationSequences.insert(request.submissionSequence);
            if (!m_shuttingDown) {
                m_speculativeMutationSequences.insert(
                    request.submissionSequence);
            }
            if (!request.optimisticMutationId.isEmpty()) {
                m_catalogSink.rollbackOptimisticStateMutation(
                    request.optimisticMutationId);
                request.optimisticMutationId.clear();
            }
            qInfo().noquote()
                << "media_mutation"
                << "phase=consumer_settled"
                << "requestId=" << request.submissionSequence
                << "item=" << request.itemId
                << "operation=" << operationCode(request.operation)
                << "reason=" << reason;
            fail(request, message, !isNonModal(request.operation));
        }
    }
}

void MediaCoordinator::applyMutationResult(
    MutationRequest &request,
    const YanamiOperationResult &operationResult,
    qint64 elapsedMs)
{
    const bool nonModal = isNonModal(request.operation);
    const auto stillCurrent = [this, &request] {
        return !m_settledMutationSequences.contains(
                   request.submissionSequence)
            && acceptsSession(request.sessionGeneration);
    };
    const auto rollback = [this, &request] {
        if (!request.optimisticMutationId.isEmpty()) {
            m_catalogSink.rollbackOptimisticStateMutation(
                request.optimisticMutationId);
            request.optimisticMutationId.clear();
        }
    };
    const auto reject = [this, &request, &rollback, &stillCurrent, nonModal](
                            const char *reason,
                            const QString &message) {
        rollback();
        qWarning().noquote()
            << "media_mutation"
            << "phase=failed"
            << "requestId=" << request.submissionSequence
            << "item=" << request.itemId
            << "operation=" << operationCode(request.operation)
            << "reason=" << reason;
        if (stillCurrent()) {
            m_settledMutationSequences.insert(
                request.submissionSequence);
            fail(request, message, !nonModal);
        }
    };

    if (operationResult.status != 0) {
        reject(
            "backend_error",
            nonModal
                ? tr("The server could not complete this operation. Please try again.")
                : m_statusSink.userFacingBackendError(
                    operationResult.errorCode, operationResult.error));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        operationResult.payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        reject("invalid_json",
            tr("The server returned an invalid response for this operation."));
        return;
    }
    const QJsonObject object = document.object();
    if (!validateResponseSchema(object, "media-mutation", !nonModal)) {
        reject("incompatible_schema",
            tr("The server returned an invalid response for this operation."));
        return;
    }
    const QString responseItemId =
        object.value(QStringLiteral("itemId")).toString();
    if (responseItemId.isEmpty() || responseItemId != request.itemId) {
        reject("response_mismatch",
            tr("The server returned an invalid response for this operation."));
        return;
    }

    QVariantMap result = object.value(QStringLiteral("result"))
        .toObject().toVariantMap();
    if (request.operation == Operation::SetPlayed) {
        result.insert(QStringLiteral("requestedPlayed"),
            request.payload.value(QStringLiteral("played")).toBool());
    } else if (request.operation == Operation::SetFavorite) {
        result.insert(QStringLiteral("requestedFavorite"),
            request.payload.value(QStringLiteral("favorite")).toBool());
    }

    m_catalogSink.applyInvalidationEvent(
        object.value(QStringLiteral("invalidation"))
            .toObject().toVariantMap());
    if (!stillCurrent())
        return;
    if (request.operation == Operation::ScanLibraryFiles
        && result.value(QStringLiteral("refreshStarted")).toBool()) {
        trackLibraryScan(request.itemId, elapsedMs);
        if (!stillCurrent())
            return;
    }
    if (request.operation == Operation::RefreshMetadata
        && result.value(QStringLiteral("refreshStarted")).toBool()) {
        trackMetadataRefresh(request.itemId, elapsedMs);
        if (!stillCurrent())
            return;
    }

    const QVariantList authoritativeStates =
        result.value(QStringLiteral("affectedItems")).toList();
    const bool authoritativeStateProvided =
        !authoritativeStates.isEmpty();
    const bool authoritativeReconcileComplete =
        result.value(QStringLiteral("reconcileComplete")).toBool();
    // Remove the optimistic overlay before applying server state so the
    // authoritative response is always the final visible value.
    if (!request.optimisticMutationId.isEmpty()) {
        m_catalogSink.commitOptimisticStateMutation(
            request.optimisticMutationId);
        request.optimisticMutationId.clear();
        if (!stillCurrent())
            return;
    }
    m_catalogSink.applyAuthoritativeStates(authoritativeStates);
    if (!stillCurrent())
        return;

    const bool targetedContainerMutation =
        request.operation == Operation::AddToPlaylist
        || request.operation == Operation::RemoveFromPlaylist;
    if (targetedContainerMutation) {
        m_catalogSink.applyContainerMutation(
            request.itemId, request.operation, result);
        if (!stillCurrent())
            return;
    }
    if (request.operation == Operation::UpdateMetadata) {
        m_catalogSink.applyPendingMetadataPatch(
            request.itemId, request.payload);
        if (!stillCurrent())
            return;
    }

    if (request.operation == Operation::SetPlayed
        || request.operation == Operation::SetFavorite) {
        if (!authoritativeStateProvided
            || !authoritativeReconcileComplete) {
            m_catalogSink.invalidatePresentationCache();
            if (!stillCurrent())
                return;
        }
        m_catalogSink.scheduleContentReconciliation(
            request.operation == Operation::SetFavorite
                ? QStringLiteral("favorite_state_mutation")
                : QStringLiteral("played_state_mutation"));
        if (!stillCurrent())
            return;
    } else if (request.operation == Operation::RefreshMetadata) {
        // Emby accepted an asynchronous scrape. Completion/fallback tracking
        // owns subsequent reads; only stale presentation snapshots are fenced
        // here.
        m_catalogSink.invalidatePresentationCache();
        if (!stillCurrent())
            return;
    } else if (request.operation != Operation::ScanLibraryFiles
               && !targetedContainerMutation) {
        m_catalogSink.scheduleContentReconciliation(
            QStringLiteral("media_mutation"));
        if (!stillCurrent())
            return;
    }

    if (request.operation == Operation::SetPlayed
        || request.operation == Operation::SetFavorite) {
        // Card state is already visible through the optimistic/authoritative
        // catalog path; a global success toast would duplicate that feedback.
    } else if (request.operation == Operation::ScanLibraryFiles) {
        m_statusSink.publishStatus(
            tr("Library file scan requested."), false);
    } else if (request.operation == Operation::RefreshMetadata) {
        m_statusSink.publishStatus(
            tr("Metadata refresh requested."), false);
    } else if (request.operation == Operation::AddToPlaylist) {
        m_statusSink.publishStatus(tr("Added to playlist."), false);
    } else if (request.operation == Operation::RemoveFromPlaylist) {
        m_statusSink.publishStatus(tr("Removed from playlist."), false);
    } else if (isImageMutation(request.operation)) {
        m_statusSink.publishStatus(tr("Images updated."), false);
    } else {
        m_statusSink.publishStatus(
            tr("The media item was updated."), false);
    }
    if (!stillCurrent())
        return;
    m_settledMutationSequences.insert(request.submissionSequence);
    complete(request, result);
}
