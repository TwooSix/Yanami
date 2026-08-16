#include "MediaCoordinator.hpp"

#include "BackendInfrastructure.hpp"

#include <QDateTime>
#include <QDebug>
#include <QJsonObject>

#include <utility>

namespace {

constexpr int desktopSchemaVersion = 8;

const QString metadataLaneKey = QStringLiteral("metadata-editor.load");
const QString playlistTargetsLaneKey =
    QStringLiteral("media-targets.load");
const QString imageListLaneKey = QStringLiteral("image-editor.load");
const QString imageProvidersLaneKey =
    QStringLiteral("image-editor.providers");
const QString imageSearchLaneKey = QStringLiteral("image-editor.search");

bool isInteractiveRead(MediaPort::Operation operation)
{
    return operation == MediaPort::Operation::LoadMetadata
        || operation == MediaPort::Operation::LoadPlaylistTargets
        || operation == MediaPort::Operation::LoadImages
        || operation == MediaPort::Operation::LoadImageProviders
        || operation == MediaPort::Operation::SearchImages;
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

QString schemaVersionLabel(const QJsonValue &value)
{
    if (value.isUndefined())
        return QStringLiteral("missing");
    if (!value.isDouble())
        return QStringLiteral("invalid");
    return QString::number(value.toDouble(), 'g', 16);
}

QVariantMap viewContext(quint64 viewGeneration)
{
    return {
        {QStringLiteral("clientViewGeneration"), viewGeneration},
    };
}

void insertOptionalImageIndex(
    QVariantMap &payload,
    const QVariant &imageIndex)
{
    if (!imageIndex.isValid() || imageIndex.isNull()) {
        payload.insert(QStringLiteral("imageIndex"), QVariant());
        return;
    }
    payload.insert(QStringLiteral("imageIndex"), imageIndex.toInt());
}

} // namespace

MediaCoordinator::MediaCoordinator(
    RustBridgeRuntime &runtime,
    QThreadPool &readPool,
    QThreadPool &mutationPool,
    SessionStateProvider sessionStateProvider,
    StatusSink &statusSink,
    CatalogMutationSink &catalogSink,
    QObject *parent)
    : MediaPort(parent)
    , m_runtime(runtime)
    , m_readPool(readPool)
    , m_mutationPool(mutationPool)
    , m_sessionStateProvider(std::move(sessionStateProvider))
    , m_statusSink(statusSink)
    , m_catalogSink(catalogSink)
    , m_mutations(
          2,
          [](const MutationRequest &request) { return request.itemId; },
          [](const MutationRequest &left, const MutationRequest &right) {
              // Catalog's optimistic journal is deliberately single-writer.
              // Other operations do not use it and may occupy both lanes.
              return !usesOptimisticCatalogJournal(left.operation)
                  || !usesOptimisticCatalogJournal(right.operation);
          })
{
    m_metadataLane.key = metadataLaneKey;
    m_playlistTargetsLane.key = playlistTargetsLaneKey;
    m_imageListLane.key = imageListLaneKey;
    m_imageProvidersLane.key = imageProvidersLaneKey;
    m_imageSearchLane.key = imageSearchLaneKey;

    for (InteractiveLane *lane : interactiveLanes()) {
        connect(&lane->watcher,
            &QFutureWatcher<YanamiOperationResult>::finished,
            this,
            [this, lane] { finishInteractive(*lane); });
    }
    for (MutationLane &lane : m_mutationLanes) {
        MutationLane *lanePointer = &lane;
        connect(&lane.watcher,
            &QFutureWatcher<YanamiOperationResult>::finished,
            this,
            [this, lanePointer] { finishMutation(*lanePointer); });
    }
    connect(&m_refreshProgressWatcher,
        &QFutureWatcher<YanamiOperationResult>::finished,
        this,
        &MediaCoordinator::finishRefreshProgress);
    m_refreshProgressTimer.setInterval(1000);
    connect(&m_refreshProgressTimer,
        &QTimer::timeout,
        this,
        &MediaCoordinator::pollRefreshProgress);
    m_metadataRefreshFallbackTimer.setInterval(500);
    connect(&m_metadataRefreshFallbackTimer,
        &QTimer::timeout,
        this,
        &MediaCoordinator::pollMetadataRefreshFallbacks);
}

MediaCoordinator::~MediaCoordinator()
{
    shutdown();
    drain();
}

bool MediaCoordinator::initializeFromSession()
{
    if (m_initialized)
        return activeSession();
    m_initialized = true;
    m_shuttingDown = false;
    sessionCommitted();
    return activeSession();
}

void MediaCoordinator::sessionTransitionStarted(const char *reason)
{
    m_sessionFenced = true;
    m_refreshProgressTimer.stop();
    m_metadataRefreshFallbackTimer.stop();
    m_requests.invalidateAll();
    resetInteractiveReads(reason);
    resetMutations(reason);
}

void MediaCoordinator::sessionTransitionAborted()
{
    if (!m_initialized || m_shuttingDown)
        return;
    const MediaSessionState session = currentSession();
    m_sessionFenced = !session.connected || !m_runtime.ready();
    if (m_sessionFenced)
        return;
    if (!m_completedSpeculativeMutationItems.isEmpty()) {
        m_completedSpeculativeMutationItems.clear();
        m_catalogSink.scheduleContentReconciliation(
            QStringLiteral("session_transition_aborted_mutation"));
    }
    if (m_bufferedRefreshProgress.has_value()
        && m_bufferedRefreshProgressSessionGeneration
            == session.generation) {
        const YanamiOperationResult buffered =
            std::move(*m_bufferedRefreshProgress);
        m_bufferedRefreshProgress.reset();
        applyRefreshProgress(buffered);
    } else {
        m_bufferedRefreshProgress.reset();
    }
    m_refreshProgressTimer.start();
    if (!m_requestedMetadataRefreshes.isEmpty()
        || !m_refreshProtectionReleaseAtMs.isEmpty()) {
        m_metadataRefreshFallbackTimer.start();
    }
    QTimer::singleShot(0, this, &MediaCoordinator::pollRefreshProgress);
    QTimer::singleShot(0, this, &MediaCoordinator::dispatchMutations);
    QTimer::singleShot(0, this, &MediaCoordinator::pumpInteractiveReads);
}

void MediaCoordinator::sessionCommitted()
{
    if (!m_initialized || m_shuttingDown)
        return;
    m_bufferedRefreshProgress.reset();
    m_speculativeMutationSequences.clear();
    m_completedSpeculativeMutationItems.clear();
    resetRefreshTracking();
    const MediaSessionState session = currentSession();
    m_sessionFenced = !session.connected || !m_runtime.ready();
    if (m_sessionFenced) {
        resetRefreshTracking();
        return;
    }
    m_refreshProgressTimer.start();
    QTimer::singleShot(0, this, &MediaCoordinator::pollRefreshProgress);
    QTimer::singleShot(0, this, &MediaCoordinator::dispatchMutations);
    QTimer::singleShot(0, this, &MediaCoordinator::pumpInteractiveReads);
}

void MediaCoordinator::shutdown()
{
    if (m_shuttingDown)
        return;
    m_shuttingDown = true;
    sessionTransitionStarted("shutdown");
    m_bufferedRefreshProgress.reset();
    resetRefreshTracking();
    m_initialized = false;
}

void MediaCoordinator::drain()
{
    for (InteractiveLane *lane : interactiveLanes()) {
        if (lane->watcher.isRunning())
            lane->watcher.waitForFinished();
    }
    for (MutationLane &lane : m_mutationLanes) {
        if (lane.watcher.isRunning())
            lane.watcher.waitForFinished();
    }
    if (m_refreshProgressWatcher.isRunning())
        m_refreshProgressWatcher.waitForFinished();
}

MediaSessionState MediaCoordinator::currentSession() const
{
    return m_sessionStateProvider
        ? m_sessionStateProvider() : MediaSessionState {};
}

bool MediaCoordinator::activeSession() const
{
    const MediaSessionState session = currentSession();
    return m_initialized
        && !m_shuttingDown
        && !m_sessionFenced
        && m_runtime.ready()
        && session.connected;
}

bool MediaCoordinator::acceptsSession(quint64 generation) const
{
    const MediaSessionState session = currentSession();
    return activeSession() && generation == session.generation;
}

void MediaCoordinator::loadMetadata(
    const QString &requestId,
    const QString &itemId,
    quint64 viewGeneration)
{
    submit(requestId, itemId, Operation::LoadMetadata,
        viewContext(viewGeneration));
}

void MediaCoordinator::updateMetadata(
    const QString &requestId,
    const QString &itemId,
    const QVariantMap &changes)
{
    submit(requestId, itemId, Operation::UpdateMetadata, changes);
}

void MediaCoordinator::loadImages(
    const QString &requestId,
    const QString &itemId,
    quint64 viewGeneration)
{
    submit(requestId, itemId, Operation::LoadImages,
        viewContext(viewGeneration));
}

void MediaCoordinator::loadImageProviders(
    const QString &requestId,
    const QString &itemId,
    quint64 viewGeneration)
{
    submit(requestId, itemId, Operation::LoadImageProviders,
        viewContext(viewGeneration));
}

void MediaCoordinator::searchImages(
    const QString &requestId,
    const QString &itemId,
    const QString &imageType,
    const QString &providerName,
    bool includeAllLanguages,
    bool enableSeriesImages,
    quint64 viewGeneration,
    quint64 searchGeneration,
    int startIndex,
    int limit)
{
    submit(requestId, itemId, Operation::SearchImages, {
        {QStringLiteral("imageType"), imageType},
        {QStringLiteral("providerName"), providerName},
        {QStringLiteral("includeAllLanguages"), includeAllLanguages},
        {QStringLiteral("enableSeriesImages"), enableSeriesImages},
        {QStringLiteral("clientViewGeneration"), viewGeneration},
        {QStringLiteral("clientSearchGeneration"), searchGeneration},
        {QStringLiteral("startIndex"), startIndex},
        {QStringLiteral("limit"), limit},
    });
}

void MediaCoordinator::applyRemoteImage(
    const QString &requestId,
    const QString &itemId,
    const QString &imageType,
    const QUrl &imageUrl,
    const QString &providerName,
    const QVariant &imageIndex)
{
    QVariantMap payload {
        {QStringLiteral("imageType"), imageType},
        {QStringLiteral("imageUrl"), imageUrl},
        {QStringLiteral("providerName"), providerName},
    };
    insertOptionalImageIndex(payload, imageIndex);
    submit(requestId, itemId, Operation::ApplyRemoteImage,
        std::move(payload));
}

void MediaCoordinator::uploadImage(
    const QString &requestId,
    const QString &itemId,
    const QString &imageType,
    const QUrl &fileUrl,
    const QVariant &imageIndex)
{
    QVariantMap payload {
        {QStringLiteral("imageType"), imageType},
        {QStringLiteral("fileUrl"), fileUrl},
    };
    insertOptionalImageIndex(payload, imageIndex);
    submit(requestId, itemId, Operation::UploadImage,
        std::move(payload));
}

void MediaCoordinator::removeImage(
    const QString &requestId,
    const QString &itemId,
    const QString &imageType,
    const QVariant &imageIndex)
{
    QVariantMap payload {
        {QStringLiteral("imageType"), imageType},
    };
    insertOptionalImageIndex(payload, imageIndex);
    submit(requestId, itemId, Operation::RemoveImage,
        std::move(payload));
}

void MediaCoordinator::refreshMetadata(
    const QString &requestId,
    const QString &itemId,
    const QString &mode,
    bool replaceImages,
    const QString &source)
{
    QVariantMap payload {
        {QStringLiteral("mode"), mode},
        {QStringLiteral("replaceImages"), replaceImages},
    };
    if (!source.isEmpty())
        payload.insert(QStringLiteral("source"), source);
    submit(requestId, itemId, Operation::RefreshMetadata,
        std::move(payload));
}

void MediaCoordinator::loadPlaylistTargets(
    const QString &requestId,
    const QString &itemId)
{
    submit(requestId, itemId, Operation::LoadPlaylistTargets, {});
}

void MediaCoordinator::addToPlaylist(
    const QString &requestId,
    const QString &itemId,
    const QString &targetId)
{
    submit(requestId, itemId, Operation::AddToPlaylist, {
        {QStringLiteral("targetId"), targetId},
    });
}

void MediaCoordinator::createPlaylistAndAdd(
    const QString &requestId,
    const QString &itemId,
    const QString &newName)
{
    submit(requestId, itemId, Operation::AddToPlaylist, {
        {QStringLiteral("newName"), newName},
    });
}

void MediaCoordinator::removeFromPlaylist(
    const QString &requestId,
    const QString &itemId,
    const QString &playlistId,
    const QString &entryId)
{
    submit(requestId, itemId, Operation::RemoveFromPlaylist, {
        {QStringLiteral("playlistId"), playlistId},
        {QStringLiteral("entryId"), entryId},
    });
}

void MediaCoordinator::setPlayed(
    const QString &requestId,
    const QString &itemId,
    bool played)
{
    submit(requestId, itemId, Operation::SetPlayed, {
        {QStringLiteral("played"), played},
    });
}

void MediaCoordinator::setFavorite(
    const QString &requestId,
    const QString &itemId,
    bool favorite)
{
    submit(requestId, itemId, Operation::SetFavorite, {
        {QStringLiteral("favorite"), favorite},
    });
}

void MediaCoordinator::scanLibraryFiles(
    const QString &requestId,
    const QString &itemId)
{
    submit(requestId, itemId, Operation::ScanLibraryFiles, {});
}

void MediaCoordinator::deleteItem(
    const QString &requestId,
    const QString &itemId)
{
    submit(requestId, itemId, Operation::DeleteItem, {});
}

void MediaCoordinator::submit(
    const QString &requestId,
    const QString &itemId,
    MediaPort::Operation operation,
    QVariantMap payload)
{
    MediaRequest base;
    base.clientRequestId = requestId;
    base.itemId = itemId;
    base.operation = operation;
    base.payload = std::move(payload);
    base.submissionSequence = ++m_nextSubmissionSequence;
    base.sessionGeneration = currentSession().generation;
    base.enqueuedAtMs = QDateTime::currentMSecsSinceEpoch();

    if (itemId.trimmed().isEmpty()) {
        rejectImmediately(requestId, itemId, operation,
            tr("A media item is required for this operation."));
        return;
    }
    if (!activeSession()) {
        rejectImmediately(requestId, itemId, operation,
            tr("This operation requires an active Emby connection."));
        return;
    }

    if (isInteractiveRead(operation)) {
        InteractiveRequest read;
        static_cast<MediaRequest &>(read) = std::move(base);
        submitInteractive(std::move(read));
        return;
    }

    MutationRequest mutation;
    static_cast<MediaRequest &>(mutation) = std::move(base);
    submitMutation(std::move(mutation));
}

void MediaCoordinator::rejectImmediately(
    const QString &requestId,
    const QString &itemId,
    MediaPort::Operation operation,
    const QString &message)
{
    const bool nonModal = isNonModal(operation);
    m_statusSink.publishStatus(message, !nonModal);
    emit operationFailed(
        requestId, itemId, operation, message, nonModal);
}

bool MediaCoordinator::validateResponseSchema(
    const QJsonObject &object,
    const char *responseName,
    bool publishError)
{
    const QJsonValue version =
        object.value(QStringLiteral("schemaVersion"));
    if (version.isDouble()
        && version.toDouble()
            == static_cast<double>(desktopSchemaVersion)) {
        return true;
    }
    const QString received = schemaVersionLabel(version);
    qWarning().noquote()
        << "backend_schema_incompatible"
        << "response=" << responseName
        << "expected=" << desktopSchemaVersion
        << "received=" << received;
    if (publishError) {
        m_statusSink.publishStatus(
            tr("The backend %1 response uses an incompatible schema version "
               "(expected %2, received %3).")
                .arg(QString::fromLatin1(responseName))
                .arg(desktopSchemaVersion)
                .arg(received),
            true);
    }
    return false;
}

void MediaCoordinator::complete(
    const MediaRequest &request,
    const QVariantMap &result)
{
    emit operationCompleted(
        request.clientRequestId,
        request.itemId,
        request.operation,
        result);
}

void MediaCoordinator::fail(
    const MediaRequest &request,
    const QString &message,
    bool publishStatus)
{
    const bool nonModal = isNonModal(request.operation);
    if (publishStatus)
        m_statusSink.publishStatus(message, !nonModal);
    emit operationFailed(
        request.clientRequestId,
        request.itemId,
        request.operation,
        message,
        nonModal);
}
