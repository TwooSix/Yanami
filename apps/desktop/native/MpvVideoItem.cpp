#include "MpvVideoItem.hpp"

#include "DevelopmentHooks.hpp"

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QEvent>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QDebug>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <clocale>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <mpv/render.h>
#include <mpv/render_gl.h>

namespace {

Q_LOGGING_CATEGORY(playbackLog, "yanami.playback.mpv")

constexpr int playbackStallPollIntervalMs = 250;
constexpr qint64 playbackStartupTimeoutMs = 30'000;
constexpr qint64 playbackMaximumPollGapMs = 2'000;

mpv_handle *createMpvHandle()
{
    // libmpv requires a locale-independent decimal separator before
    // mpv_create(). Qt keeps its user-facing locale in QLocale, so this does
    // not force the UI to use C-locale formatting.
    if (!std::setlocale(LC_NUMERIC, "C"))
        return nullptr;
    return mpv_create();
}

void *resolveOpenGl(void *, const char *name)
{
    auto *context = QOpenGLContext::currentContext();
    if (!context)
        return nullptr;
    const auto function = context->getProcAddress(QByteArray(name));
    return reinterpret_cast<void *>(function);
}

QString stringProperty(mpv_handle *handle, const QByteArray &name)
{
    char *value = nullptr;
    if (mpv_get_property(handle, name.constData(), MPV_FORMAT_STRING, &value) < 0 || !value)
        return {};
    const QString result = QString::fromUtf8(value);
    mpv_free(value);
    return result;
}

QByteArray diagnosticOption(
    DevelopmentHooks::Variable variable, const QByteArray &fallback)
{
    const QByteArray value = DevelopmentHooks::bytes(variable).trimmed();
    return value.isEmpty() ? fallback : value;
}

void logPlaybackDiagnostics(mpv_handle *handle)
{
    const auto logProperty = [handle](const char *name) {
        qInfo().noquote() << "mpv" << name << '='
                          << stringProperty(handle, QByteArray(name));
    };
    logProperty("hwdec");
    logProperty("hwdec-current");
    logProperty("video-sync");
    logProperty("video-format");
    logProperty("width");
    logProperty("height");
    logProperty("video-bitrate");
    logProperty("demuxer-cache-duration");
    logProperty("video-params/primaries");
    logProperty("video-params/gamma");
    logProperty("video-params/colormatrix");
    logProperty("video-out-params/primaries");
    logProperty("video-out-params/gamma");
}

QString sanitizedMpvLog(const char *text)
{
    QString message = QString::fromUtf8(text).trimmed();
    static const QRegularExpression url(
        QStringLiteral("(?i)\\b(https?://)(?:[^@\\s/]+@)?([^/\\s?#]+(?::\\d+)?)(?:[/\\?#][^\\s\\\"'<>]*)?"));
    static const QRegularExpression namedSecret(
        QStringLiteral("(?i)\\b(api[_-]?key|x-emby-token|access[_-]?token|token|authorization)"
                       "\\s*[:=]\\s*(?:bearer\\s+)?[^,;\\s]+"));
    static const QRegularExpression bearer(
        QStringLiteral("(?i)\\bbearer\\s+[^,;\\s]+"));
    message.replace(url, QStringLiteral("\\1\\2/<redacted>"));
    message.replace(namedSecret, QStringLiteral("\\1=<redacted>"));
    message.replace(bearer, QStringLiteral("Bearer <redacted>"));
    return message;
}

const char *endReasonName(int reason)
{
    switch (reason) {
    case MPV_END_FILE_REASON_EOF:
        return "eof";
    case MPV_END_FILE_REASON_STOP:
        return "stop";
    case MPV_END_FILE_REASON_QUIT:
        return "quit";
    case MPV_END_FILE_REASON_ERROR:
        return "error";
    case MPV_END_FILE_REASON_REDIRECT:
        return "redirect";
    default:
        return "unknown";
    }
}

const char *playbackStateName(MpvVideoItem::PlaybackState state)
{
    switch (state) {
    case MpvVideoItem::PlaybackState::Idle:
        return "idle";
    case MpvVideoItem::PlaybackState::Loading:
        return "loading";
    case MpvVideoItem::PlaybackState::Playing:
        return "playing";
    case MpvVideoItem::PlaybackState::Paused:
        return "paused";
    case MpvVideoItem::PlaybackState::Buffering:
        return "buffering";
    case MpvVideoItem::PlaybackState::Ended:
        return "ended";
    }
    return "unknown";
}

} // namespace

struct MpvRenderState
{
    QMutex itemMutex;
    QPointer<MpvVideoItem> item;
    std::atomic_uint64_t callbackCount{0};
    std::atomic_uint64_t renderCount{0};
    std::atomic_uint64_t renderTotalNanoseconds{0};
    std::atomic_uint64_t renderMaximumNanoseconds{0};
};

class MpvRenderer final : public QQuickFramebufferObject::Renderer
{
public:
    explicit MpvRenderer(MpvVideoItem *item)
        : m_mpvOwner(item->m_mpvOwner)
        , m_state(item->m_renderState)
    {
    }

    ~MpvRenderer() override
    {
        if (m_context) {
            mpv_render_context_set_update_callback(m_context, nullptr, nullptr);
            mpv_render_context_free(m_context);
        }
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        format.setTextureTarget(GL_TEXTURE_2D);
        return new QOpenGLFramebufferObject(size, format);
    }

    void synchronize(QQuickFramebufferObject *) override
    {
    }

    void render() override
    {
        if (!m_mpvOwner)
            return;

        QElapsedTimer renderTimer;
        renderTimer.start();

        if (!m_context) {
            if (auto *context = QOpenGLContext::currentContext()) {
                auto *functions = context->functions();
                qInfo().noquote()
                    << "OpenGL vendor="
                    << reinterpret_cast<const char *>(functions->glGetString(GL_VENDOR))
                    << "renderer="
                    << reinterpret_cast<const char *>(functions->glGetString(GL_RENDERER))
                    << "version="
                    << reinterpret_cast<const char *>(functions->glGetString(GL_VERSION));
            }
            mpv_opengl_init_params glInit{resolveOpenGl, nullptr};
            mpv_render_param parameters[] = {
                {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };
            if (mpv_render_context_create(&m_context, m_mpvOwner.get(), parameters) < 0) {
                QMutexLocker locker(&m_state->itemMutex);
                if (m_state->item) {
                    QMetaObject::invokeMethod(m_state->item, [item = m_state->item] {
                        if (item)
                            emit item->playbackError(
                                MpvVideoItem::tr("Unable to initialize the libmpv OpenGL renderer."));
                    });
                }
                return;
            }
            mpv_render_context_set_update_callback(m_context, &MpvRenderer::requestUpdate, this);
        }

        const auto dimensions = framebufferObject()->size();
        mpv_opengl_fbo fbo{
            static_cast<int>(framebufferObject()->handle()),
            dimensions.width(),
            dimensions.height(),
            0,
        };
        int flipY = 0;
        int blockForTargetTime = 0;
        mpv_render_param parameters[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &blockForTargetTime},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };
        mpv_render_context_render(m_context, parameters);
        ++m_state->renderCount;
        const auto elapsed = static_cast<quint64>(renderTimer.nsecsElapsed());
        m_state->renderTotalNanoseconds += elapsed;
        auto maximum = m_state->renderMaximumNanoseconds.load();
        while (elapsed > maximum
               && !m_state->renderMaximumNanoseconds.compare_exchange_weak(maximum, elapsed)) {
        }
        QQuickOpenGLUtils::resetOpenGLState();

    }

private:
    static void requestUpdate(void *context)
    {
        auto *renderer = static_cast<MpvRenderer *>(context);
        if (!renderer)
            return;
        ++renderer->m_state->callbackCount;
        QMutexLocker locker(&renderer->m_state->itemMutex);
        if (renderer->m_state->item) {
            QMetaObject::invokeMethod(renderer->m_state->item, [item = renderer->m_state->item] {
                if (item)
                    item->update();
            });
        }
    }

    std::shared_ptr<mpv_handle> m_mpvOwner;
    std::shared_ptr<MpvRenderState> m_state;
    mpv_render_context *m_context = nullptr;
};

MpvVideoItem::MpvVideoItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
    , m_mpvOwner(createMpvHandle(), [](mpv_handle *handle) {
        if (handle)
            mpv_terminate_destroy(handle);
    })
    , m_mpv(m_mpvOwner.get())
    , m_renderState(std::make_shared<MpvRenderState>())
{
    if (!m_mpv)
        throw std::runtime_error("mpv_create failed");
    m_renderState->item = this;

    const QByteArray hardwareDecoding = diagnosticOption(
        DevelopmentHooks::Variable::MpvHwdec, "auto");
    const QByteArray videoSync = diagnosticOption(
        DevelopmentHooks::Variable::MpvVideoSync, "audio");
    const QByteArray demuxerMaxBytes =
        diagnosticOption(DevelopmentHooks::Variable::MpvDemuxerMaxBytes, "150MiB");
    const QByteArray demuxerMaxBackBytes =
        diagnosticOption(DevelopmentHooks::Variable::MpvDemuxerMaxBackBytes, "50MiB");
    const QByteArray hardwareExtraFrames =
        diagnosticOption(DevelopmentHooks::Variable::MpvHwdecExtraFrames, "6");
    const std::pair<const char *, QByteArray> options[] = {
        {"vo", "libmpv"},
        {"hwdec", hardwareDecoding},
        {"video-sync", videoSync},
        {"demuxer-max-bytes", demuxerMaxBytes},
        {"demuxer-max-back-bytes", demuxerMaxBackBytes},
        {"hwdec-extra-frames", hardwareExtraFrames},
        // Qt owns presentation and display synchronization. Asking libmpv to
        // wait inside mpv_render_context_render() stalls the shared Qt Quick
        // render thread and caps UI animations at the video's frame rate.
        {"video-timing-offset", "0"},
        {"terminal", "no"},
        {"keep-open", "yes"},
        // The UI owns the completed state. Preserve the final frame without
        // letting keep-open turn natural EOF into a sticky pause that carries
        // into the automatically loaded queue entry.
        {"keep-open-pause", "no"},
        // libmpv defaults to not verifying HTTPS certificates. Playback must
        // honor the same strict TLS boundary as the Rust API client.
        {"tls-verify", "yes"},
        {"sub-auto", "no"},
        {"sub-ass-override", "scale"},
        {"audio-client-name", "Yanami"},
    };
    for (const auto &[name, value] : options) {
        const int result = mpv_set_option_string(m_mpv, name, value.constData());
        if (result < 0) {
            throw std::runtime_error(
                QStringLiteral("mpv option %1 rejected: %2")
                    .arg(QString::fromUtf8(name), QString::fromUtf8(mpv_error_string(result)))
                    .toStdString());
        }
    }

    qInfo().noquote() << "mpv requested hwdec=" << hardwareDecoding
                      << "video-sync=" << videoSync
                      << "demuxer-max-bytes=" << demuxerMaxBytes
                      << "demuxer-max-back-bytes=" << demuxerMaxBackBytes
                      << "hwdec-extra-frames=" << hardwareExtraFrames;

    if (mpv_initialize(m_mpv) < 0)
        throw std::runtime_error("mpv_initialize failed");

    const QString effectiveTlsVerification =
        stringProperty(m_mpv, QByteArrayLiteral("options/tls-verify"));
    if (effectiveTlsVerification != QStringLiteral("yes"))
        throw std::runtime_error("libmpv TLS certificate verification is not enabled");
    qInfo().noquote() << "mpv security tlsVerify=" << effectiveTlsVerification;

    // Network and demux warnings are part of the normal support log. The
    // message is sanitized before it reaches the persistent logger.
    mpv_request_log_messages(m_mpv, "warn");

    mpv_observe_property(m_mpv, 1, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 2, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 3, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 4, "track-list/count", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 5, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 6, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 7, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 8, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 9, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 10, "seekable", MPV_FORMAT_FLAG);
    // MPV_EVENT_END_FILE is not emitted at natural EOF while keep-open keeps
    // the file loaded. eof-reached is the authoritative completion boundary.
    mpv_observe_property(m_mpv, 11, "eof-reached", MPV_FORMAT_FLAG);
    mpv_set_wakeup_callback(m_mpv, &MpvVideoItem::wakeup, this);

    m_playbackStallClock.start();
    m_playbackStallTimer.setInterval(playbackStallPollIntervalMs);
    m_playbackStallTimer.setTimerType(Qt::CoarseTimer);
    connect(
        &m_playbackStallTimer,
        &QTimer::timeout,
        this,
        &MpvVideoItem::pollPlaybackStall);

#ifdef YANAMI_ENABLE_DEV_HOOKS
    if (DevelopmentHooks::isSet(DevelopmentHooks::Variable::RenderDiagnostics)) {
        auto *diagnosticTimer = new QTimer(this);
        diagnosticTimer->setInterval(1000);
        connect(diagnosticTimer, &QTimer::timeout, this, [this] {
            const quint64 renderCount = m_renderState->renderCount.exchange(0);
            const quint64 totalNanoseconds = m_renderState->renderTotalNanoseconds.exchange(0);
            const quint64 maximumNanoseconds = m_renderState->renderMaximumNanoseconds.exchange(0);
            qInfo() << "render-load mpvCallbacks=" << m_renderState->callbackCount.exchange(0)
                    << "mpvRenders=" << renderCount
                    << "averageRenderMs=" << (renderCount > 0
                               ? totalNanoseconds / 1'000'000.0 / renderCount
                               : 0.0)
                    << "maximumRenderMs=" << maximumNanoseconds / 1'000'000.0;
        });
        diagnosticTimer->start();
    }
#endif
}

MpvVideoItem::~MpvVideoItem()
{
    if (m_mpv)
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
    {
        QMutexLocker locker(&m_renderState->itemMutex);
        m_renderState->item.clear();
    }
    m_mpv = nullptr;
    // The render context is owned by the scene graph render thread and must
    // be released before the final mpv handle reference is destroyed.
    m_mpvOwner.reset();
}

QQuickFramebufferObject::Renderer *MpvVideoItem::createRenderer() const
{
    return new MpvRenderer(const_cast<MpvVideoItem *>(this));
}

void MpvVideoItem::open(const QUrl &url, const QVariantMap &headers)
{
    resetPlaybackStall();
    ++m_loadGeneration;
    m_loadTimer.restart();
    m_bufferingTimer.invalidate();
    m_bufferingTransitions = 0;
    m_totalBufferingMs = 0;
    m_pauseRequested = m_paused;
    qCInfo(playbackLog).noquote()
        << "playback_mpv_open"
        << "generation=" << m_loadGeneration
        << "source=" << (url.isLocalFile() ? "local" : "remote")
        << "scheme=" << url.scheme().toLower()
        << "headerCount=" << headers.size();
    m_buffering = false;
    m_fileLoaded = false;
    m_completionGate.reset();
    m_position = 0.0;
    m_duration = 0.0;
    m_bufferedPosition = 0.0;
    m_seekable = false;
    emit positionChanged();
    emit durationChanged();
    emit bufferedPositionChanged();
    emit seekableChanged();
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    m_selectedAudioTrack = -1;
    m_selectedSubtitleTrack = -1;
    emit tracksChanged();
    setHeaders(headers);
    setPlaybackState(PlaybackState::Loading);
    m_startupWatchStartedMs = playbackStallNow();
    m_lastPlaybackStallPollMs = m_startupWatchStartedMs;
    m_playbackStallTimer.start();
    command({"loadfile", url.toString(QUrl::FullyEncoded).toUtf8(), "replace"});
}

void MpvVideoItem::stop()
{
    if (m_buffering && m_bufferingTimer.isValid()) {
        m_totalBufferingMs += m_bufferingTimer.elapsed();
        m_bufferingTimer.invalidate();
    }
    qCInfo(playbackLog).noquote()
        << "playback_mpv_stop"
        << "generation=" << m_loadGeneration
        << "elapsedMs=" << (m_loadTimer.isValid() ? m_loadTimer.elapsed() : -1)
        << "positionSeconds=" << m_position
        << "state=" << playbackStateName(m_playbackState)
        << "bufferingTransitions=" << m_bufferingTransitions
        << "totalBufferingMs=" << m_totalBufferingMs;
    command({"stop"});
    setHeaders({});
    resetPlaybackStall();
    m_buffering = false;
    m_fileLoaded = false;
    m_completionGate.reset();
    m_bufferedPosition = 0.0;
    if (m_seekable) {
        m_seekable = false;
        emit seekableChanged();
    }
    emit bufferedPositionChanged();
    setPlaybackState(PlaybackState::Idle);
}

void MpvVideoItem::seek(double seconds)
{
    const double target = std::max(0.0, seconds);
    qCInfo(playbackLog).noquote()
        << "playback_mpv_seek"
        << "generation=" << m_loadGeneration
        << "fromSeconds=" << m_position
        << "targetSeconds=" << target
        << "fileLoaded=" << m_fileLoaded;
    if (m_fileLoaded) {
        m_playbackStallWatchdog.beginSeek(playbackStallNow(), m_position);
        m_playbackStallTimer.start();
    }
    command({"seek", QByteArray::number(target, 'f', 3), "absolute+exact"});
    emit seekRequested(target);
}

void MpvVideoItem::setVolume(double volume)
{
    double bounded = std::clamp(volume, 0.0, 100.0);
    if (std::abs(m_volume - bounded) > 0.01) {
        m_volume = bounded;
        emit volumeChanged();
    }
    mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE, &bounded);
}

void MpvVideoItem::setMuted(bool muted)
{
    int flag = muted ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "mute", MPV_FORMAT_FLAG, &flag);
}

void MpvVideoItem::setRate(double rate)
{
    double bounded = std::clamp(rate, 0.25, 4.0);
    mpv_set_property_async(m_mpv, 0, "speed", MPV_FORMAT_DOUBLE, &bounded);
}

void MpvVideoItem::addSubtitle(const QUrl &url, const QString &title, bool selected)
{
    command({
        "sub-add",
        url.toString(QUrl::FullyEncoded).toUtf8(),
        selected ? "select" : "auto",
        title.toUtf8(),
    });
    QTimer::singleShot(120, this, &MpvVideoItem::refreshTracks);
}

void MpvVideoItem::selectAudioTrack(qint64 trackId)
{
    int64_t id = trackId;
    if (mpv_set_property(m_mpv, "aid", MPV_FORMAT_INT64, &id) < 0) {
        emit playbackError(tr("Unable to switch the audio track."));
        return;
    }
    QTimer::singleShot(60, this, &MpvVideoItem::refreshTracks);
}

void MpvVideoItem::selectSubtitleTrack(qint64 trackId)
{
    int64_t id = trackId;
    if (mpv_set_property(m_mpv, "sid", MPV_FORMAT_INT64, &id) < 0) {
        emit playbackError(tr("Unable to switch the subtitle track."));
        return;
    }
    QTimer::singleShot(60, this, &MpvVideoItem::refreshTracks);
}

void MpvVideoItem::disableSubtitles()
{
    if (mpv_set_property_string(m_mpv, "sid", "no") < 0) {
        emit playbackError(tr("Unable to disable subtitles."));
        return;
    }
    QTimer::singleShot(60, this, &MpvVideoItem::refreshTracks);
}

void MpvVideoItem::setPaused(bool paused)
{
    m_pauseRequested = paused;
    updatePlaybackPauseMonitoring(paused);
    int flag = paused ? 1 : 0;
    const int status =
        mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
    if (status >= 0)
        return;

    m_pauseRequested = m_paused;
    updatePlaybackPauseMonitoring(m_paused);
    qCWarning(playbackLog).noquote()
        << "playback_mpv_pause_failed"
        << "generation=" << m_loadGeneration
        << "requested=" << paused
        << "errorCode=" << status;
    emit playbackError(tr("libmpv rejected a playback command."));
}

bool MpvVideoItem::event(QEvent *event)
{
    const bool handled = QQuickFramebufferObject::event(event);
    if (event->type() == QEvent::LanguageChange)
        refreshTracks();
    return handled;
}

void MpvVideoItem::wakeup(void *context)
{
    auto *item = static_cast<MpvVideoItem *>(context);
    QMetaObject::invokeMethod(item, &MpvVideoItem::drainEvents, Qt::QueuedConnection);
}

void MpvVideoItem::drainEvents()
{
    while (const mpv_event *event = mpv_wait_event(m_mpv, 0)) {
        if (event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_FILE_LOADED) {
            m_fileLoaded = true;
            m_completionGate.reset();
            const qint64 nowMs = playbackStallNow();
            m_playbackStallWatchdog.arm(nowMs, m_position);
            if (m_pauseRequested)
                m_playbackStallWatchdog.setPaused(true, nowMs, m_position);
            m_watchdogBuffering = m_timeoutReported && !m_pauseRequested;
            if (m_watchdogBuffering)
                m_playbackStallWatchdog.markStalled(nowMs, m_position);
            m_playbackStallTimer.start();
            refreshPlaybackState();
            refreshTracks();
            logPlaybackDiagnostics(m_mpv);
            qCInfo(playbackLog).noquote()
                << "playback_mpv_file_loaded"
                << "generation=" << m_loadGeneration
                << "elapsedMs=" << (m_loadTimer.isValid() ? m_loadTimer.elapsed() : -1)
                << "paused=" << m_paused
                << "buffering=" << m_buffering
                << "durationSeconds=" << m_duration
                << "audioTracks=" << m_audioTracks.size()
                << "subtitleTracks=" << m_subtitleTracks.size();
            emit fileLoaded();
        } else if (event->event_id == MPV_EVENT_SEEK) {
            if (m_fileLoaded) {
                m_playbackStallWatchdog.observeSeekStarted(
                    playbackStallNow(), m_position);
            }
        } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            if (m_fileLoaded)
                m_playbackStallWatchdog.observePlaybackRestart(
                    playbackStallNow(), m_position);
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            if (m_buffering && m_bufferingTimer.isValid()) {
                m_totalBufferingMs += m_bufferingTimer.elapsed();
                m_bufferingTimer.invalidate();
            }
            m_fileLoaded = false;
            m_buffering = false;
            resetPlaybackStall();
            setPlaybackState(PlaybackState::Ended);
            auto *endFile = static_cast<mpv_event_end_file *>(event->data);
            qCInfo(playbackLog).noquote()
                << "playback_mpv_file_ended"
                << "generation=" << m_loadGeneration
                << "elapsedMs=" << (m_loadTimer.isValid() ? m_loadTimer.elapsed() : -1)
                << "reason=" << (endFile ? endReasonName(endFile->reason) : "missing")
                << "errorCode=" << (endFile ? endFile->error : 0)
                << "positionSeconds=" << m_position
                << "durationSeconds=" << m_duration
                << "bufferingTransitions=" << m_bufferingTransitions
                << "totalBufferingMs=" << m_totalBufferingMs;
            if (endFile && endFile->error < 0) {
                qCWarning(playbackLog).noquote()
                    << "playback_mpv_error"
                    << "generation=" << m_loadGeneration
                    << "errorCode=" << endFile->error
                    << "error=" << mpv_error_string(endFile->error);
                emit playbackError(
                    tr("Playback failed: %1")
                        .arg(QString::fromUtf8(mpv_error_string(endFile->error))));
            }
            emit fileEnded();
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto *property = static_cast<mpv_event_property *>(event->data);
            if (!property)
                continue;
            const QByteArray name(property->name);
            if (name == "track-list/count") {
                refreshTracks();
            } else if (!property->data) {
                continue;
            } else if (name == "pause" && property->format == MPV_FORMAT_FLAG) {
                const bool value = *static_cast<int *>(property->data) != 0;
                if (m_paused != value) {
                    m_paused = value;
                    emit pausedChanged();
                }
                m_pauseRequested = value;
                updatePlaybackPauseMonitoring(value);
            } else if (name == "paused-for-cache" && property->format == MPV_FORMAT_FLAG) {
                const bool buffering = *static_cast<int *>(property->data) != 0;
                if (m_buffering != buffering) {
                    qint64 intervalMs = 0;
                    if (buffering) {
                        ++m_bufferingTransitions;
                        m_bufferingTimer.restart();
                    } else if (m_bufferingTimer.isValid()) {
                        intervalMs = m_bufferingTimer.elapsed();
                        m_totalBufferingMs += intervalMs;
                        m_bufferingTimer.invalidate();
                    }
                    qCInfo(playbackLog).noquote()
                        << "playback_mpv_buffering"
                        << "generation=" << m_loadGeneration
                        << "active=" << buffering
                        << "fileLoaded=" << m_fileLoaded
                        << "intervalMs=" << intervalMs
                        << "totalBufferingMs=" << m_totalBufferingMs
                        << "positionSeconds=" << m_position
                        << "bufferedPositionSeconds=" << m_bufferedPosition;
                    m_buffering = buffering;
                }
                refreshPlaybackState();
            } else if (name == "time-pos" && property->format == MPV_FORMAT_DOUBLE) {
                m_position = *static_cast<double *>(property->data);
                emit positionChanged();
                if (m_fileLoaded) {
                    handlePlaybackStallEvent(
                        m_playbackStallWatchdog.observePosition(
                            playbackStallNow(), m_position));
                }
            } else if (name == "duration" && property->format == MPV_FORMAT_DOUBLE) {
                m_duration = *static_cast<double *>(property->data);
                emit durationChanged();
            } else if (name == "demuxer-cache-time" && property->format == MPV_FORMAT_DOUBLE) {
                const double value = *static_cast<double *>(property->data);
                if (std::isfinite(value)) {
                    m_bufferedPosition = std::max(0.0, value);
                    emit bufferedPositionChanged();
                }
            } else if (name == "volume" && property->format == MPV_FORMAT_DOUBLE) {
                const double value = std::clamp(*static_cast<double *>(property->data), 0.0, 100.0);
                if (std::abs(m_volume - value) > 0.01) {
                    m_volume = value;
                    emit volumeChanged();
                }
            } else if (name == "mute" && property->format == MPV_FORMAT_FLAG) {
                const bool value = *static_cast<int *>(property->data) != 0;
                if (m_muted != value) {
                    m_muted = value;
                    emit mutedChanged();
                }
            } else if (name == "speed" && property->format == MPV_FORMAT_DOUBLE) {
                const double value = *static_cast<double *>(property->data);
                if (std::isfinite(value) && std::abs(m_rate - value) > 0.001) {
                    m_rate = value;
                    emit rateChanged();
                }
            } else if (name == "seekable" && property->format == MPV_FORMAT_FLAG) {
                const bool value = *static_cast<int *>(property->data) != 0;
                if (m_seekable != value) {
                    m_seekable = value;
                    emit seekableChanged();
                }
            } else if (name == "eof-reached" && property->format == MPV_FORMAT_FLAG) {
                const bool value = *static_cast<int *>(property->data) != 0;
                const std::optional<YanamiPlayback::CompletionBoundary> boundary =
                    m_completionGate.observe(
                        value, m_fileLoaded, m_position, m_duration);
                if (boundary.has_value()) {
                    if (m_buffering && m_bufferingTimer.isValid()) {
                        m_totalBufferingMs += m_bufferingTimer.elapsed();
                        m_bufferingTimer.invalidate();
                    }
                    m_buffering = false;
                    resetPlaybackStall();
                    setPlaybackState(PlaybackState::Ended);
                    qCInfo(playbackLog).noquote()
                        << "playback_mpv_eof"
                        << "generation=" << m_loadGeneration
                        << "classification="
                        << (*boundary == YanamiPlayback::CompletionBoundary::Natural
                                ? "natural" : "premature")
                        << "elapsedMs="
                        << (m_loadTimer.isValid() ? m_loadTimer.elapsed() : -1)
                        << "positionSeconds=" << m_position
                        << "durationSeconds=" << m_duration
                        << "bufferingTransitions=" << m_bufferingTransitions
                        << "totalBufferingMs=" << m_totalBufferingMs;
                    if (*boundary == YanamiPlayback::CompletionBoundary::Natural) {
                        emit playbackCompleted();
                    } else {
                        qCWarning(playbackLog).noquote()
                            << "playback_mpv_premature_eof"
                            << "generation=" << m_loadGeneration
                            << "positionSeconds=" << m_position
                            << "durationSeconds=" << m_duration;
                        emit playbackError(tr(
                            "Playback ended before the media was complete."));
                        emit fileEnded();
                    }
                }
            }
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            auto *message = static_cast<mpv_event_log_message *>(event->data);
            if (message) {
                qCWarning(playbackLog).noquote()
                    << "mpv_warning"
                    << "generation=" << m_loadGeneration
                    << "component=" << message->prefix
                    << "message=" << sanitizedMpvLog(message->text);
            }
        }
    }
}

void MpvVideoItem::refreshTracks()
{
    int64_t count = 0;
    if (mpv_get_property(m_mpv, "track-list/count", MPV_FORMAT_INT64, &count) < 0)
        return;

    QVariantList audioTracks;
    QVariantList subtitleTracks;
    qint64 selectedAudio = -1;
    qint64 selectedSubtitle = -1;

    for (int64_t index = 0; index < count; ++index) {
        const QByteArray prefix = "track-list/" + QByteArray::number(index) + "/";
        const QString type = stringProperty(m_mpv, prefix + "type");
        if (type != QStringLiteral("audio") && type != QStringLiteral("sub"))
            continue;

        int64_t id = 0;
        int selected = 0;
        if (mpv_get_property(m_mpv, (prefix + "id").constData(), MPV_FORMAT_INT64, &id) < 0)
            continue;
        mpv_get_property(m_mpv, (prefix + "selected").constData(), MPV_FORMAT_FLAG, &selected);

        const QString title = stringProperty(m_mpv, prefix + "title").trimmed();
        const QString language = stringProperty(m_mpv, prefix + "lang").trimmed();
        const QString codec = stringProperty(m_mpv, prefix + "codec").trimmed().toUpper();
        const QString externalFile =
            stringProperty(m_mpv, prefix + "external-filename");
        int64_t ffIndex = -1;
        const bool hasFfIndex = mpv_get_property(
            m_mpv, (prefix + "ff-index").constData(), MPV_FORMAT_INT64,
            &ffIndex) >= 0;

        QStringList labelParts;
        if (!title.isEmpty())
            labelParts.push_back(title);
        if (!language.isEmpty() && !title.contains(language, Qt::CaseInsensitive))
            labelParts.push_back(language);
        if (!codec.isEmpty() && !title.contains(codec, Qt::CaseInsensitive))
            labelParts.push_back(codec);
        if (labelParts.isEmpty()) {
            labelParts.push_back(type == QStringLiteral("audio")
                    ? tr("Audio track %1").arg(id)
                    : tr("Subtitle %1").arg(id));
        }

        const QVariant trackId = QVariant::fromValue(static_cast<qint64>(id));
        QVariantMap track{
            {QStringLiteral("id"), trackId},
            {QStringLiteral("mpvTrackId"), trackId},
            {QStringLiteral("label"), labelParts.join(QStringLiteral(" · "))},
            {QStringLiteral("title"), title},
            {QStringLiteral("language"), language},
            {QStringLiteral("codec"), codec},
            {QStringLiteral("externalUrl"), externalFile},
            {QStringLiteral("selected"), selected != 0},
        };
        if (hasFfIndex) {
            track.insert(QStringLiteral("ffIndex"),
                QVariant::fromValue(static_cast<qint64>(ffIndex)));
        }
        if (type == QStringLiteral("audio")) {
            audioTracks.push_back(track);
            if (selected)
                selectedAudio = id;
        } else {
            subtitleTracks.push_back(track);
            if (selected)
                selectedSubtitle = id;
        }
    }

    m_audioTracks = audioTracks;
    m_subtitleTracks = subtitleTracks;
    m_selectedAudioTrack = selectedAudio;
    m_selectedSubtitleTrack = selectedSubtitle;
    emit tracksChanged();
}

void MpvVideoItem::refreshPlaybackState()
{
    if (!m_fileLoaded || m_completionGate.handled())
        return;
    const bool effectivelyBuffering = m_buffering || m_watchdogBuffering;
    setPlaybackState(m_pauseRequested
            ? PlaybackState::Paused
            : (effectivelyBuffering ? PlaybackState::Buffering
                                    : PlaybackState::Playing));
}

void MpvVideoItem::updatePlaybackPauseMonitoring(bool paused)
{
    if (!m_fileLoaded || m_completionGate.handled())
        return;

    const qint64 nowMs = playbackStallNow();
    const YanamiPlayback::PlaybackStallEvent event =
        m_playbackStallWatchdog.setPaused(paused, nowMs, m_position);
    if (paused) {
        if (event == YanamiPlayback::PlaybackStallEvent::StallCleared)
            m_watchdogBuffering = false;
    } else {
        handlePlaybackStallEvent(event);
        if (m_timeoutReported) {
            m_playbackStallWatchdog.markStalled(nowMs, m_position);
            m_watchdogBuffering = true;
        }
    }
    refreshPlaybackState();
}

void MpvVideoItem::pollPlaybackStall()
{
    if (!m_loadTimer.isValid())
        return;

    const qint64 nowMs = playbackStallNow();
    if (!m_fileLoaded) {
        const qint64 pollGapMs = nowMs - m_lastPlaybackStallPollMs;
        m_lastPlaybackStallPollMs = nowMs;
        if (pollGapMs < 0 || pollGapMs > playbackMaximumPollGapMs) {
            // Suspending Windows or blocking the UI event loop must not turn
            // into an apparent media startup timeout on the first wake tick.
            m_startupWatchStartedMs = nowMs;
            return;
        }
        if (m_playbackState != PlaybackState::Loading || m_timeoutReported
            || nowMs - m_startupWatchStartedMs < playbackStartupTimeoutMs) {
            return;
        }
        m_timeoutReported = true;
        qCWarning(playbackLog).noquote()
            << "playback_mpv_startup_timeout"
            << "generation=" << m_loadGeneration
            << "elapsedMs=" << m_loadTimer.elapsed()
            << "positionSeconds=" << m_position
            << "durationSeconds=" << m_duration
            << "bufferedPositionSeconds=" << m_bufferedPosition;
        emit playbackTimedOut(tr(
            "The connection is slow. Playback will resume automatically."));
        return;
    }

    handlePlaybackStallEvent(
        m_playbackStallWatchdog.poll(nowMs));
}

void MpvVideoItem::handlePlaybackStallEvent(
    YanamiPlayback::PlaybackStallEvent event)
{
    if (event == YanamiPlayback::PlaybackStallEvent::None)
        return;

    const qint64 nowMs = playbackStallNow();
    const qint64 noProgressMs =
        m_playbackStallWatchdog.noProgressForMs(nowMs);
    if (event == YanamiPlayback::PlaybackStallEvent::EnteredStall) {
        m_watchdogBuffering = true;
        qCInfo(playbackLog).noquote()
            << "playback_mpv_stall_detected"
            << "generation=" << m_loadGeneration
            << "noProgressMs=" << noProgressMs
            << "positionSeconds=" << m_position
            << "durationSeconds=" << m_duration
            << "bufferedPositionSeconds=" << m_bufferedPosition
            << "rate=" << m_rate
            << "pausedForCache=" << m_buffering
            << "seeking=" << m_playbackStallWatchdog.seeking();
        refreshPlaybackState();
        return;
    }

    if (event == YanamiPlayback::PlaybackStallEvent::TimedOut) {
        m_watchdogBuffering = true;
        const bool shouldNotify = !m_timeoutReported;
        qCWarning(playbackLog).noquote()
            << "playback_mpv_stall_timeout"
            << "generation=" << m_loadGeneration
            << "noProgressMs=" << noProgressMs
            << "positionSeconds=" << m_position
            << "durationSeconds=" << m_duration
            << "bufferedPositionSeconds=" << m_bufferedPosition
            << "rate=" << m_rate
            << "pausedForCache=" << m_buffering
            << "seeking=" << m_playbackStallWatchdog.seeking()
            << "timeoutReported=" << m_timeoutReported;
        refreshPlaybackState();
        if (shouldNotify) {
            m_timeoutReported = true;
            emit playbackTimedOut(tr(
                "The connection is slow. Playback will resume automatically."));
        }
        return;
    }

    const bool recoveredFromTimeout = m_timeoutReported;
    const bool recoveredFromStall = m_watchdogBuffering;
    m_watchdogBuffering = false;
    m_timeoutReported = false;
    if (recoveredFromStall || recoveredFromTimeout) {
        qCInfo(playbackLog).noquote()
            << "playback_mpv_stall_recovered"
            << "generation=" << m_loadGeneration
            << "positionSeconds=" << m_position
            << "durationSeconds=" << m_duration
            << "bufferedPositionSeconds=" << m_bufferedPosition
            << "pausedForCache=" << m_buffering;
    }
    refreshPlaybackState();
    if (recoveredFromTimeout)
        emit playbackRecovered();
}

void MpvVideoItem::resetPlaybackStall()
{
    m_playbackStallTimer.stop();
    m_playbackStallWatchdog.reset();
    m_watchdogBuffering = false;
    m_timeoutReported = false;
    m_startupWatchStartedMs = 0;
    m_lastPlaybackStallPollMs = 0;
}

qint64 MpvVideoItem::playbackStallNow() const
{
    return m_playbackStallClock.isValid() ? m_playbackStallClock.elapsed() : 0;
}

void MpvVideoItem::setPlaybackState(PlaybackState state)
{
    if (m_playbackState == state)
        return;
    const PlaybackState previous = m_playbackState;
    m_playbackState = state;
    qCInfo(playbackLog).noquote()
        << "playback_mpv_state"
        << "generation=" << m_loadGeneration
        << "from=" << playbackStateName(previous)
        << "to=" << playbackStateName(state)
        << "positionSeconds=" << m_position
        << "bufferedPositionSeconds=" << m_bufferedPosition;
    emit playbackStateChanged();
}

void MpvVideoItem::command(const QList<QByteArray> &arguments, quint64 replyUserdata)
{
    std::vector<const char *> pointers;
    pointers.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
        pointers.push_back(argument.constData());
    pointers.push_back(nullptr);
    const int status = mpv_command_async(m_mpv, replyUserdata, pointers.data());
    if (status < 0) {
        qCWarning(playbackLog).noquote()
            << "playback_mpv_command_failed"
            << "generation=" << m_loadGeneration
            << "command=" << (arguments.isEmpty() ? QByteArrayLiteral("missing") : arguments.constFirst())
            << "errorCode=" << status;
        emit playbackError(tr("libmpv rejected a playback command."));
    }
}

void MpvVideoItem::setHeaders(const QVariantMap &headers)
{
    std::vector<QByteArray> encodedHeaders;
    encodedHeaders.reserve(headers.size());
    for (auto iterator = headers.cbegin(); iterator != headers.cend(); ++iterator) {
        QString name = iterator.key();
        name.remove('\r');
        name.remove('\n');
        name.remove(':');
        QString value = iterator.value().toString();
        value.remove('\r');
        value.remove('\n');
        encodedHeaders.push_back((name + QStringLiteral(": ") + value).toUtf8());
    }

    std::vector<mpv_node> values(encodedHeaders.size());
    for (std::size_t index = 0; index < encodedHeaders.size(); ++index) {
        values[index].format = MPV_FORMAT_STRING;
        values[index].u.string = encodedHeaders[index].data();
    }
    mpv_node_list list{
        static_cast<int>(values.size()),
        values.data(),
        nullptr,
    };
    mpv_node node;
    node.format = MPV_FORMAT_NODE_ARRAY;
    node.u.list = &list;
    mpv_set_property(m_mpv, "http-header-fields", MPV_FORMAT_NODE, &node);
}
