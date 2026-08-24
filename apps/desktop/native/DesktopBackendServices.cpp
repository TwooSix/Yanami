#include "DesktopBackendServices.hpp"

#include "BackendInfrastructure.hpp"
#include "CatalogCoordinator.hpp"
#include "DanmakuCoordinator.hpp"
#include "MediaCoordinator.hpp"
#include "PlaybackCoordinator.hpp"
#include "PlaybackReporter.hpp"
#include "SearchCoordinator.hpp"
#include "SessionCoordinator.hpp"

#include <QString>

#include <memory>
#include <optional>
#include <utility>

namespace {

SessionCapabilities sessionCapabilities(
    const CatalogUserCapabilities &capabilities)
{
    return {
        .administrator = capabilities.administrator,
        .canDownload = capabilities.canDownload,
        .canDelete = capabilities.canDelete,
    };
}

CatalogUserCapabilities catalogCapabilities(
    const SessionCoordinator &session)
{
    return {
        .administrator = session.administrator(),
        .canDownload = session.canDownload(),
        .canDelete = session.canDelete(),
    };
}

} // namespace

struct DesktopBackendServices::Impl
{
    Impl();
    ~Impl();

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    BackendPortSet portSet() const;

private:
    void initialize();
    void constructCoordinators();
    void connectSessionLifecycle();
    void fenceFeatures();
    void commitSessionTransition();
    void abortSessionTransition();
    void shutdown();

    CatalogSessionState catalogSessionState() const;
    SearchSessionState searchSessionState() const;
    MediaSessionState mediaSessionState() const;
    PlaybackCoordinator::SessionState playbackSessionState() const;
    DanmakuCoordinator::SessionState danmakuSessionState() const;

    // Destruction is reverse declaration order. RuntimeHost is deliberately
    // first so the bridge remains alive until every coordinator, reporter and
    // worker pool has gone away.
    RuntimeHost runtimeHost;
    WorkerPools pools;
    std::unique_ptr<ApplicationStatusService> status;
    std::unique_ptr<SessionCoordinator> session;
    std::unique_ptr<CatalogCoordinator> catalog;
    std::unique_ptr<SearchCoordinator> search;
    std::unique_ptr<MediaCoordinator> media;
    std::unique_ptr<PlaybackCoordinator> playback;
    std::unique_ptr<PlaybackReporter> playbackReporter;
    std::unique_ptr<DanmakuCoordinator> danmaku;

    QString transitionRequestId;
    std::optional<SessionPort::Operation> transitionOperation;
    bool transitionInProgress = false;
    bool shuttingDown = false;
};

DesktopBackendServices::Impl::Impl()
{
    initialize();
}

DesktopBackendServices::Impl::~Impl()
{
    shutdown();
}

void DesktopBackendServices::Impl::initialize()
{
    status = std::make_unique<ApplicationStatusService>(runtimeHost);

    const RuntimeInitializationResult runtimeResult =
        runtimeHost.initialize();
    if (runtimeResult.ready) {
        status->publishStatus({}, false);
    } else {
        status->publishStatus(
            status->userFacingBackendError(
                runtimeResult.errorCode,
                runtimeResult.errorMessage),
            true);
    }

    // Coordinators are safe to construct around an unopened RuntimeHost: all
    // request entry points check ready state. Building the complete port set
    // also lets the UI render a deterministic backend-startup error state.
    constructCoordinators();
    connectSessionLifecycle();
    if (!runtimeResult.ready)
        return;

    // Session must publish its authoritative generation and connection before
    // feature coordinators scope caches or accept work.
    session->initialize();
    catalog->initializeFromSession();
    search->initializeFromSession();
    media->initializeFromSession();
    danmaku->initializeCredentialStatus();
}

void DesktopBackendServices::Impl::constructCoordinators()
{
    RustBridgeRuntime *runtime = runtimeHost.runtime();
    Q_ASSERT(runtime);

    session = std::make_unique<SessionCoordinator>(
        runtimeHost,
        pools.sessionControl(),
        *status);

    catalog = std::make_unique<CatalogCoordinator>(
        *runtime,
        pools.catalog(),
        [this] { return catalogSessionState(); },
        *status,
        runtimeHost.cacheDirectory(),
        [this](quint64 generation,
            const CatalogUserCapabilities &capabilities) {
            if (session) {
                session->replaceCapabilities(
                    generation,
                    sessionCapabilities(capabilities));
            }
        });

    search = std::make_unique<SearchCoordinator>(
        *runtime,
        pools.search(),
        pools.searchHydration(),
        [this] { return searchSessionState(); },
        *status);

    media = std::make_unique<MediaCoordinator>(
        *runtime,
        pools.mediaRead(),
        pools.mediaMutation(),
        [this] { return mediaSessionState(); },
        *status,
        *catalog);

    playback = std::make_unique<PlaybackCoordinator>(
        *runtime,
        pools.playbackPrepare(),
        pools.playbackReport(),
        [this] { return playbackSessionState(); },
        *status);
    playbackReporter =
        std::make_unique<PlaybackReporter>(playback.get());

    // Search and ordered mutations have separate logical lanes. Passing the
    // same bounded pool keeps DanDanPlay isolated without adding idle threads.
    danmaku = std::make_unique<DanmakuCoordinator>(
        *runtime,
        pools.danmakuControl(),
        pools.danmakuControl(),
        [this] { return danmakuSessionState(); },
        *status);
}

void DesktopBackendServices::Impl::connectSessionLifecycle()
{
    QObject::connect(
        session.get(),
        &SessionCoordinator::transitionStarted,
        session.get(),
        [this](quint64,
            SessionPort::Operation operation,
            const QString &requestId) {
            if (shuttingDown)
                return;
            transitionInProgress = true;
            transitionOperation = operation;
            transitionRequestId = requestId;
            fenceFeatures();
        });
    QObject::connect(
        session.get(),
        &SessionCoordinator::committed,
        session.get(),
        [this](quint64, bool) {
            if (shuttingDown || !transitionInProgress)
                return;
            commitSessionTransition();
        });
    QObject::connect(
        session.get(),
        &SessionPort::operationFailed,
        session.get(),
        [this](const QString &requestId,
            SessionPort::Operation operation,
            const QString &) {
            if (shuttingDown
                || !transitionInProgress
                || session->transitioning()
                || requestId != transitionRequestId
                || !transitionOperation.has_value()
                || operation != *transitionOperation) {
                return;
            }
            abortSessionTransition();
        });
}

void DesktopBackendServices::Impl::fenceFeatures()
{
    if (playbackReporter)
        playbackReporter->abandonSessionForTransition();
    catalog->sessionTransitionStarted("session_transition");
    search->sessionTransitionStarted();
    media->sessionTransitionStarted("session_transition");
    playback->fenceSessionTransition("session_transition");
    danmaku->sessionTransitionStarted("session_transition");
}

void DesktopBackendServices::Impl::commitSessionTransition()
{
    // SessionCoordinator has already published the new generation. Only the
    // successful path resets session-scoped stores and cache identity.
    catalog->sessionCommitted();
    search->sessionCommitted();
    media->sessionCommitted();
    playback->resumeAfterSessionTransition();
    danmaku->sessionTransitionCommitted();

    transitionInProgress = false;
    transitionOperation.reset();
    transitionRequestId.clear();
}

void DesktopBackendServices::Impl::abortSessionTransition()
{
    // Login/logout failure retains the previously committed session. Feature
    // fences reopen, but no cache, MediaStore or current playback identity is
    // replaced with a speculative session.
    catalog->sessionTransitionAborted();
    search->sessionTransitionAborted();
    media->sessionTransitionAborted();
    playback->resumeAfterSessionTransition();
    danmaku->sessionTransitionCommitted();

    transitionInProgress = false;
    transitionOperation.reset();
    transitionRequestId.clear();
}

CatalogSessionState
DesktopBackendServices::Impl::catalogSessionState() const
{
    if (!session)
        return {};
    return {
        .generation = session->generation(),
        .connected = session->connected(),
        .displayName = session->displayName(),
        .serverUrl = session->serverUrl(),
        .userName = session->userName(),
        .serverDomain = session->serverDomain(),
        .capabilities = catalogCapabilities(*session),
    };
}

MediaSessionState
DesktopBackendServices::Impl::mediaSessionState() const
{
    if (!session)
        return {};
    return {
        .generation = session->generation(),
        .connected = session->connected(),
    };
}

SearchSessionState
DesktopBackendServices::Impl::searchSessionState() const
{
    if (!session)
        return {};
    return {
        .generation = session->generation(),
        .connected = session->connected(),
    };
}

PlaybackCoordinator::SessionState
DesktopBackendServices::Impl::playbackSessionState() const
{
    if (!session)
        return {};
    return {
        .generation = session->generation(),
        .connected = session->connected(),
    };
}

DanmakuCoordinator::SessionState
DesktopBackendServices::Impl::danmakuSessionState() const
{
    if (!session)
        return {};
    return {
        .generation = session->generation(),
        .connected = session->connected(),
    };
}

void DesktopBackendServices::Impl::shutdown()
{
    if (shuttingDown)
        return;
    shuttingDown = true;

    if (playbackReporter)
        playbackReporter->stopSession();
    if (danmaku)
        danmaku->shutdown();
    if (playback)
        playback->shutdown();
    if (media)
        media->shutdown();
    if (search)
        search->shutdown();
    if (catalog)
        catalog->shutdown();
    if (session)
        session->shutdown();

    // No feature can enqueue new work past this point. Cancel Rust-owned
    // operations before joining native futures, but keep the bridge handle
    // alive until every watcher and pool has drained.
    if (RustBridgeRuntime *runtime = runtimeHost.runtime())
        runtime->cancelAll();

    if (danmaku)
        danmaku->drain();
    if (playback)
        playback->drain();
    if (media)
        media->drain();
    if (search)
        search->drain();
    if (catalog)
        catalog->drain();
    if (session)
        session->drain();
    pools.drain();
    runtimeHost.shutdown();
}

BackendPortSet DesktopBackendServices::Impl::portSet() const
{
    return {
        .session = session.get(),
        .catalog = catalog.get(),
        .search = search.get(),
        .playback = playback.get(),
        .playbackReporter = playbackReporter.get(),
        .danmaku = danmaku.get(),
        .media = media.get(),
        .status = status.get(),
    };
}

DesktopBackendServices::DesktopBackendServices(QObject *parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>())
{
}

DesktopBackendServices::~DesktopBackendServices() = default;

BackendPortSet DesktopBackendServices::portSet() const
{
    return m_impl ? m_impl->portSet() : BackendPortSet {};
}
