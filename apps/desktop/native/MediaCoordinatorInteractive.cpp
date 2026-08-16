#include "MediaCoordinator.hpp"

#include "BackendInfrastructure.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace {

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

bool isNonModal(MediaPort::Operation operation)
{
    return operation != MediaPort::Operation::DeleteItem;
}

} // namespace

MediaCoordinator::InteractiveLane *MediaCoordinator::interactiveLane(
    MediaPort::Operation operation)
{
    switch (operation) {
    case Operation::LoadMetadata: return &m_metadataLane;
    case Operation::LoadPlaylistTargets: return &m_playlistTargetsLane;
    case Operation::LoadImages: return &m_imageListLane;
    case Operation::LoadImageProviders: return &m_imageProvidersLane;
    case Operation::SearchImages: return &m_imageSearchLane;
    default: return nullptr;
    }
}

std::array<MediaCoordinator::InteractiveLane *, 5>
MediaCoordinator::interactiveLanes()
{
    return {
        &m_metadataLane,
        &m_playlistTargetsLane,
        &m_imageListLane,
        &m_imageProvidersLane,
        &m_imageSearchLane,
    };
}

void MediaCoordinator::submitInteractive(InteractiveRequest request)
{
    InteractiveLane *lane = interactiveLane(request.operation);
    if (!lane) {
        fail(request, tr("The media request is not supported."), true);
        return;
    }

    const QByteArray payloadJson = QJsonDocument::fromVariant(request.payload)
        .toJson(QJsonDocument::Compact);
    QByteArray identity = request.itemId.toUtf8();
    identity.append('\0');
    identity.append(QByteArray::number(operationCode(request.operation)));
    identity.append('\0');
    identity.append(payloadJson);
    const QString requestKey = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
            .toHex());
    request.token = m_requests.beginLatest(
        lane->key, requestKey, request.sessionGeneration);

    const bool heldForMutation = blockedByMutation(request);
    if (lane->active.has_value()
        || lane->watcher.isRunning()
        || heldForMutation) {
        std::optional<InteractiveRequest> superseded;
        if (lane->queued.has_value()) {
            superseded = std::move(*lane->queued);
            lane->queued.reset();
        }
        lane->queued = std::move(request);
        const quint64 queuedRequestId = lane->queued->token.requestId;
        const QString queuedItemId = lane->queued->itemId;
        qInfo().noquote()
            << "media_query"
            << "phase=queued"
            << "lane=" << lane->key
            << "requestId=" << queuedRequestId
            << "item=" << queuedItemId
            << "reason="
            << (heldForMutation ? "conflicting_mutation" : "lane_busy");
        // Publish the superseded terminal only after the new identity owns
        // the lane. A synchronous consumer retry may then replace this request
        // without the older stack frame overwriting it on return.
        if (superseded.has_value())
            settleSuperseded(std::move(*superseded));
        return;
    }
    startInteractive(*lane, std::move(request));
}

void MediaCoordinator::startInteractive(
    InteractiveLane &lane,
    InteractiveRequest request)
{
    if (lane.active.has_value() || lane.watcher.isRunning()) {
        std::optional<InteractiveRequest> superseded;
        if (lane.queued.has_value()) {
            superseded = std::move(*lane.queued);
            lane.queued.reset();
        }
        lane.queued = std::move(request);
        if (superseded.has_value())
            settleSuperseded(std::move(*superseded));
        return;
    }
    if (blockedByMutation(request)) {
        std::optional<InteractiveRequest> superseded;
        if (lane.queued.has_value()) {
            superseded = std::move(*lane.queued);
            lane.queued.reset();
        }
        lane.queued = std::move(request);
        if (superseded.has_value())
            settleSuperseded(std::move(*superseded));
        return;
    }

    const quint64 transportRequestId = request.token.requestId;
    const QString itemId = request.itemId;
    const Operation operation = request.operation;
    const QVariantMap payload = request.payload;
    const QString laneKey = lane.key;
    const qint64 enqueuedAtMs = request.enqueuedAtMs;
    lane.active = std::move(request);
    lane.timer.start();
    qInfo().noquote()
        << "media_query"
        << "phase=start"
        << "lane=" << laneKey
        << "requestId=" << transportRequestId
        << "sessionGeneration=" << lane.active->sessionGeneration
        << "item=" << itemId
        << "operation=" << operationCode(operation);
    lane.watcher.setFuture(QtConcurrent::run(
        &m_readPool,
        [this, operation, payload, transportRequestId, laneKey,
         itemId, enqueuedAtMs] {
            const qint64 queueWaitMs = enqueuedAtMs > 0
                ? QDateTime::currentMSecsSinceEpoch() - enqueuedAtMs : 0;
            qInfo().noquote()
                << "media_query"
                << "phase=transport_start"
                << "lane=" << laneKey
                << "requestId=" << transportRequestId
                << "item=" << itemId
                << "operation=" << operationCode(operation)
                << "queueWaitMs=" << queueWaitMs;
            return m_runtime.media(operation, itemId, payload);
        }));
}

void MediaCoordinator::finishInteractive(InteractiveLane &lane)
{
    const YanamiOperationResult operationResult = lane.watcher.result();
    const qint64 elapsedMs = lane.timer.isValid()
        ? lane.timer.elapsed() : -1;
    lane.timer.invalidate();
    if (!lane.active.has_value()) {
        qWarning().noquote()
            << "media_query"
            << "phase=finish"
            << "lane=" << lane.key
            << "outcome=missing_active_request";
        QTimer::singleShot(0, this, &MediaCoordinator::pumpInteractiveReads);
        return;
    }

    const InteractiveRequest request = *lane.active;
    const auto releaseLane = [this, &lane, &request] {
        m_settledInteractiveSequences.remove(
            request.submissionSequence);
        lane.active.reset();
        QTimer::singleShot(
            0, this, &MediaCoordinator::pumpInteractiveReads);
    };
    const auto stillCurrent = [this, &request] {
        return !m_settledInteractiveSequences.contains(
                   request.submissionSequence)
            && acceptsSession(request.sessionGeneration)
            && m_requests.acceptsLatest(
                request.token, currentSession().generation);
    };
    const bool consumerAlreadySettled =
        m_settledInteractiveSequences.contains(
            request.submissionSequence);
    const bool accepted = !consumerAlreadySettled
        && stillCurrent();
    qInfo().noquote()
        << "media_query"
        << "phase=finish"
        << "lane=" << lane.key
        << "requestId=" << request.token.requestId
        << "requestSessionGeneration=" << request.sessionGeneration
        << "currentSessionGeneration=" << currentSession().generation
        << "item=" << request.itemId
        << "operation=" << operationCode(request.operation)
        << "elapsedMs=" << elapsedMs
        << "status=" << operationResult.status
        << "accepted=" << accepted;
    if (consumerAlreadySettled) {
        releaseLane();
        return;
    }
    if (!accepted) {
        settleSuperseded(request);
        releaseLane();
        return;
    }

    const bool nonModal = isNonModal(request.operation);
    const auto reject = [this, &request, &stillCurrent, nonModal](
                            const char *reason,
                            const QString &message) {
        qWarning().noquote()
            << "media_query"
            << "phase=failed"
            << "requestId=" << request.token.requestId
            << "item=" << request.itemId
            << "operation=" << operationCode(request.operation)
            << "reason=" << reason;
        if (stillCurrent()) {
            m_settledInteractiveSequences.insert(
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
        releaseLane();
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        operationResult.payload, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        reject("invalid_json",
            tr("The server returned an invalid response for this operation."));
        releaseLane();
        return;
    }
    const QJsonObject object = document.object();
    if (!validateResponseSchema(object, "media-query", !nonModal)) {
        reject("incompatible_schema",
            tr("The server returned an invalid response for this operation."));
        releaseLane();
        return;
    }
    const QString responseItemId =
        object.value(QStringLiteral("itemId")).toString();
    if (responseItemId != request.itemId) {
        reject("response_mismatch",
            tr("The server returned an invalid response for this operation."));
        releaseLane();
        return;
    }

    m_catalogSink.applyInvalidationEvent(
        object.value(QStringLiteral("invalidation"))
            .toObject().toVariantMap());
    if (!stillCurrent()) {
        if (!m_settledInteractiveSequences.contains(
                request.submissionSequence)) {
            settleSuperseded(request);
        }
        releaseLane();
        return;
    }
    QVariantMap result = object.value(QStringLiteral("result"))
        .toObject().toVariantMap();
    static const QStringList presentationContextKeys {
        QStringLiteral("clientViewGeneration"),
        QStringLiteral("clientSearchGeneration"),
    };
    for (const QString &key : presentationContextKeys) {
        if (request.payload.contains(key))
            result.insert(key, request.payload.value(key));
    }
    result.insert(QStringLiteral("clientRequestId"),
        QVariant::fromValue(request.token.requestId));
    result.insert(QStringLiteral("clientResourceKey"),
        request.token.requestKey);
    result.insert(QStringLiteral("clientSessionGeneration"),
        QVariant::fromValue(request.sessionGeneration));
    if (stillCurrent()) {
        m_settledInteractiveSequences.insert(
            request.submissionSequence);
        complete(request, result);
    } else if (!m_settledInteractiveSequences.contains(
                   request.submissionSequence)) {
        settleSuperseded(request);
    }
    releaseLane();
}

void MediaCoordinator::pumpInteractive(InteractiveLane &lane)
{
    if (lane.active.has_value()
        || lane.watcher.isRunning()
        || !lane.queued.has_value()) {
        return;
    }
    if (!acceptsSession(lane.queued->sessionGeneration)
        || !m_requests.acceptsLatest(
            lane.queued->token, currentSession().generation)) {
        InteractiveRequest request = std::move(*lane.queued);
        lane.queued.reset();
        settleSuperseded(std::move(request));
        return;
    }
    if (blockedByMutation(*lane.queued))
        return;

    InteractiveRequest request = std::move(*lane.queued);
    lane.queued.reset();
    startInteractive(lane, std::move(request));
}

void MediaCoordinator::pumpInteractiveReads()
{
    for (InteractiveLane *lane : interactiveLanes())
        pumpInteractive(*lane);
}

void MediaCoordinator::settleSuperseded(InteractiveRequest request)
{
    if (m_settledInteractiveSequences.contains(
            request.submissionSequence)) {
        return;
    }
    const auto lanes = interactiveLanes();
    const bool stillInFlight = std::any_of(
        lanes.cbegin(),
        lanes.cend(),
        [&request](const InteractiveLane *lane) {
            return lane->active.has_value()
                && lane->active->submissionSequence
                    == request.submissionSequence;
        });
    if (stillInFlight) {
        m_settledInteractiveSequences.insert(
            request.submissionSequence);
    }
    qInfo().noquote()
        << "media_query"
        << "phase=consumer_settled"
        << "requestId=" << request.token.requestId
        << "item=" << request.itemId
        << "operation=" << operationCode(request.operation)
        << "outcome=superseded";
    emit operationFailed(
        request.clientRequestId,
        request.itemId,
        request.operation,
        tr("The request was superseded."),
        true);
}

void MediaCoordinator::fenceInteractiveReads(
    const MutationRequest &mutation,
    const char *reason)
{
    for (InteractiveLane *lane : interactiveLanes()) {
        const auto predatesAndConflicts =
            [this, &mutation](const InteractiveRequest &read) {
                return read.submissionSequence
                        < mutation.submissionSequence
                    && conflicts(read, mutation);
            };
        bool invalidated = false;
        if (lane->active.has_value()
            && predatesAndConflicts(*lane->active)
            && m_requests.acceptsLatest(
                lane->active->token, currentSession().generation)) {
            m_requests.invalidateLatestLane(lane->key);
            invalidated = true;
            qInfo().noquote()
                << "media_query"
                << "phase=fenced"
                << "lane=" << lane->key
                << "requestId=" << lane->active->token.requestId
                << "mutation=" << operationCode(mutation.operation)
                << "reason=" << reason;
            settleSuperseded(*lane->active);
        }
        if (lane->queued.has_value()
            && predatesAndConflicts(*lane->queued)) {
            if (!invalidated
                && m_requests.acceptsLatest(
                    lane->queued->token,
                    currentSession().generation)) {
                m_requests.invalidateLatestLane(lane->key);
            }
            InteractiveRequest request = std::move(*lane->queued);
            lane->queued.reset();
            qInfo().noquote()
                << "media_query"
                << "phase=dropped"
                << "lane=" << lane->key
                << "requestId=" << request.token.requestId
                << "mutation=" << operationCode(mutation.operation)
                << "reason=" << reason;
            settleSuperseded(std::move(request));
        }
    }
}

void MediaCoordinator::resetInteractiveReads(const char *reason)
{
    for (InteractiveLane *lane : interactiveLanes()) {
        m_requests.invalidateLatestLane(lane->key);
        if (lane->active.has_value()) {
            qInfo().noquote()
                << "media_query"
                << "phase=reset"
                << "lane=" << lane->key
                << "requestId=" << lane->active->token.requestId
                << "reason=" << reason;
            settleSuperseded(*lane->active);
        }
        if (lane->queued.has_value()) {
            InteractiveRequest request = std::move(*lane->queued);
            lane->queued.reset();
            settleSuperseded(std::move(request));
        }
    }
}

bool MediaCoordinator::blockedByMutation(
    const InteractiveRequest &request) const
{
    return m_mutations.anyOf(
        [this, &request](const MutationRequest &mutation) {
            return conflicts(request, mutation);
        });
}

bool MediaCoordinator::conflicts(
    const InteractiveRequest &read,
    const MutationRequest &mutation) const
{
    if (mutation.operation == Operation::DeleteItem)
        return read.itemId == mutation.itemId;
    if (read.operation == Operation::LoadImages
        || read.operation == Operation::SearchImages) {
        return read.itemId == mutation.itemId
            && (isImageMutation(mutation.operation)
                || (mutation.operation == Operation::RefreshMetadata
                    && mutation.payload.value(
                        QStringLiteral("replaceImages")).toBool()));
    }
    if (read.operation == Operation::LoadMetadata) {
        return read.itemId == mutation.itemId
            && (mutation.operation == Operation::UpdateMetadata
                || mutation.operation == Operation::RefreshMetadata
                || mutation.operation == Operation::ScanLibraryFiles);
    }
    if (read.operation == Operation::LoadPlaylistTargets)
        return mutation.operation == Operation::AddToPlaylist;
    return false;
}
