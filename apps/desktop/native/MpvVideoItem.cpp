#include "MpvVideoItem.hpp"

#include "DevelopmentHooks.hpp"
#include "PerformanceTrace.hpp"
#include "UpscalingPerformancePolicy.hpp"
#include "UpscalingRuntimeConfig.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QDebug>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <mpv/render.h>
#include <mpv/render_gl.h>

namespace {

Q_LOGGING_CATEGORY(playbackLog, "yanami.playback.mpv")

constexpr int playbackStallPollIntervalMs = 250;
constexpr qint64 playbackStartupTimeoutMs = 30'000;
constexpr qint64 playbackMaximumPollGapMs = 2'000;
constexpr quint64 upscalingAsyncReplyTag = quint64{1} << 63;
constexpr std::array<quint64, 26> upscalingRenderHistogramLimitsNs{
    1'000'000,
    2'000'000,
    4'000'000,
    6'000'000,
    8'000'000,
    9'000'000,
    10'000'000,
    10'500'000,
    11'000'000,
    11'500'000,
    12'000'000,
    12'500'000,
    13'000'000,
    13'500'000,
    14'000'000,
    14'500'000,
    15'000'000,
    15'500'000,
    16'000'000,
    16'500'000,
    17'000'000,
    18'000'000,
    20'000'000,
    25'000'000,
    33'400'000,
    50'000'000,
};
constexpr std::size_t upscalingRenderHistogramBucketCount =
    upscalingRenderHistogramLimitsNs.size() + 1;
constexpr std::array<quint64, 9> upscalingWrapperHistogramLimitsNs{
    10'000,
    25'000,
    50'000,
    75'000,
    100'000,
    150'000,
    250'000,
    500'000,
    1'000'000,
};
constexpr std::size_t upscalingWrapperHistogramBucketCount =
    upscalingWrapperHistogramLimitsNs.size() + 1;

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

enum class ObservedProperty : uint64_t {
    Pause = 1,
    TimePosition = 2,
    Duration = 3,
    TrackCount = 4,
    DemuxerCacheTime = 5,
    PausedForCache = 6,
    Volume = 7,
    Mute = 8,
    Speed = 9,
    Seekable = 10,
    EofReached = 11,
    DecoderFrameDropCount = 12,
    FrameDropCount = 13,
    MistimedFrameCount = 14,
    DelayedFrameCount = 15,
    AvSync = 16,
    EstimatedVideoFps = 17,
};

constexpr uint64_t observerId(ObservedProperty property) noexcept
{
    return static_cast<uint64_t>(property);
}

} // namespace

struct MpvVideoItem::PendingUpscalingConfiguration
{
    struct PropertyWrite
    {
        enum class Kind {
            String,
            ShaderPaths,
        };

        Kind kind = Kind::String;
        QByteArray name;
        QByteArray stringValue;
        QStringList shaderPaths;
        QString errorCode;
        bool targetWrite = false;
    };

    YanamiUpscaling::ValidatedConfig config;
    QList<PropertyWrite> writes;
    qsizetype nextWriteIndex = 0;
    quint64 replyUserdata = 0;
    double dispatchCpuMs = 0.0;
    QElapsedTimer completionTimer;
    QString requestedProfileId;
    QString failureErrorCode;
    QString transitionFromProfile;
    QString completionFallbackCode;
    QString completionFallbackMessage;
    double transitionRenderP95Ms = 0.0;
    bool requestEnabled = false;
    bool rollingBack = false;
    bool rollbackFailed = false;
    bool cancelled = false;
    bool transitionToOriginal = false;
};

struct MpvVideoItem::DeferredOpenRequest
{
    QUrl url;
    QVariantMap headers;
    QVariantMap runtimeConfig;
};

struct MpvRenderState
{
    QMutex itemMutex;
    QPointer<MpvVideoItem> item;
    bool diagnosticsEnabled = false;
    std::atomic_bool updateQueued{false};
    std::atomic_bool rendererReady{false};
    std::atomic_uint64_t callbackCount{0};
    std::atomic_uint64_t renderCount{0};
    std::atomic_uint64_t renderTotalNanoseconds{0};
    std::atomic_uint64_t renderMaximumNanoseconds{0};
    // Enabled only while an upscaling profile with performance protection is
    // active. The render thread records one relaxed atomic bucket increment
    // per frame; policy evaluation remains on the GUI thread once per second.
    std::atomic_bool upscalingMonitoringEnabled{false};
    std::atomic_uint64_t upscalingRenderCount{0};
    std::atomic_uint64_t upscalingRenderTotalNanoseconds{0};
    std::atomic_uint64_t upscalingRenderMaximumNanoseconds{0};
    std::array<std::atomic_uint64_t,
        upscalingRenderHistogramBucketCount> upscalingRenderHistogram{};
    std::atomic_uint64_t upscalingWrapperCount{0};
    std::atomic_uint64_t upscalingWrapperTotalNanoseconds{0};
    std::atomic_uint64_t upscalingWrapperMaximumNanoseconds{0};
    std::array<std::atomic_uint64_t,
        upscalingWrapperHistogramBucketCount> upscalingWrapperHistogram{};
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
        m_state->rendererReady.store(false, std::memory_order_release);
        if (m_context) {
            mpv_render_context_set_update_callback(m_context, nullptr, nullptr);
            mpv_render_context_free(m_context);
        }
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        QOpenGLFramebufferObjectFormat format;
        // libmpv renders a full-screen color target and never consumes depth
        // or stencil. Avoid allocating an unused attachment (about 31.6 MiB
        // at 3840x2160 for a 32-bit combined buffer).
        format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
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

        const bool monitorUpscaling =
            m_state->upscalingMonitoringEnabled.load(std::memory_order_relaxed);
        QElapsedTimer renderTimer;
        QElapsedTimer wrapperPreTimer;
        if (m_state->diagnosticsEnabled || monitorUpscaling)
            renderTimer.start();
        if (monitorUpscaling)
            wrapperPreTimer.start();

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
            m_state->rendererReady.store(true, std::memory_order_release);
            QMutexLocker locker(&m_state->itemMutex);
            if (m_state->item) {
                const QPointer<MpvVideoItem> item = m_state->item;
                QMetaObject::invokeMethod(
                    m_state->item,
                    [item] {
                        if (item)
                            item->dispatchPendingLoad();
                    },
                    Qt::QueuedConnection);
            }
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
        const quint64 wrapperPreNanoseconds = monitorUpscaling
            ? static_cast<quint64>(wrapperPreTimer.nsecsElapsed()) : 0;
        mpv_render_context_render(m_context, parameters);
        QElapsedTimer wrapperPostTimer;
        if (monitorUpscaling)
            wrapperPostTimer.start();
        if (m_state->diagnosticsEnabled || monitorUpscaling) {
            const auto elapsed = static_cast<quint64>(renderTimer.nsecsElapsed());
            if (monitorUpscaling) {
                m_state->upscalingRenderCount.fetch_add(
                    1, std::memory_order_relaxed);
                m_state->upscalingRenderTotalNanoseconds.fetch_add(
                    elapsed, std::memory_order_relaxed);
                auto maximum = m_state->upscalingRenderMaximumNanoseconds.load(
                    std::memory_order_relaxed);
                while (elapsed > maximum
                       && !m_state->upscalingRenderMaximumNanoseconds
                               .compare_exchange_weak(
                                   maximum,
                                   elapsed,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
                }
                const auto bucket = static_cast<std::size_t>(
                    std::lower_bound(
                        upscalingRenderHistogramLimitsNs.cbegin(),
                        upscalingRenderHistogramLimitsNs.cend(),
                        elapsed)
                    - upscalingRenderHistogramLimitsNs.cbegin());
                m_state->upscalingRenderHistogram.at(bucket).fetch_add(
                    1, std::memory_order_relaxed);
            }
            if (m_state->diagnosticsEnabled) {
                m_state->renderCount.fetch_add(1, std::memory_order_relaxed);
                m_state->renderTotalNanoseconds.fetch_add(
                    elapsed, std::memory_order_relaxed);
                auto maximum = m_state->renderMaximumNanoseconds.load(
                    std::memory_order_relaxed);
                while (elapsed > maximum
                       && !m_state->renderMaximumNanoseconds.compare_exchange_weak(
                           maximum,
                           elapsed,
                           std::memory_order_relaxed,
                           std::memory_order_relaxed)) {
                }
            }
        }
        QQuickOpenGLUtils::resetOpenGLState();
        if (monitorUpscaling) {
            const quint64 elapsed = wrapperPreNanoseconds
                + static_cast<quint64>(wrapperPostTimer.nsecsElapsed());
            m_state->upscalingWrapperCount.fetch_add(
                1, std::memory_order_relaxed);
            m_state->upscalingWrapperTotalNanoseconds.fetch_add(
                elapsed, std::memory_order_relaxed);
            auto maximum = m_state->upscalingWrapperMaximumNanoseconds.load(
                std::memory_order_relaxed);
            while (elapsed > maximum
                   && !m_state->upscalingWrapperMaximumNanoseconds
                           .compare_exchange_weak(
                               maximum,
                               elapsed,
                               std::memory_order_relaxed,
                               std::memory_order_relaxed)) {
            }
            const auto bucket = static_cast<std::size_t>(
                std::lower_bound(
                    upscalingWrapperHistogramLimitsNs.cbegin(),
                    upscalingWrapperHistogramLimitsNs.cend(),
                    elapsed)
                - upscalingWrapperHistogramLimitsNs.cbegin());
            m_state->upscalingWrapperHistogram.at(bucket).fetch_add(
                1, std::memory_order_relaxed);
        }
    }

private:
    static void requestUpdate(void *context)
    {
        auto *renderer = static_cast<MpvRenderer *>(context);
        if (!renderer)
            return;
        const auto state = renderer->m_state;
        if (state->diagnosticsEnabled)
            state->callbackCount.fetch_add(1, std::memory_order_relaxed);
        if (state->updateQueued.exchange(true, std::memory_order_acq_rel))
            return;

        bool queued = false;
        {
            QMutexLocker locker(&state->itemMutex);
            if (state->item) {
                const QPointer<MpvVideoItem> item = state->item;
                queued = QMetaObject::invokeMethod(
                    state->item,
                    [state, item] {
                        // Release the coalescing gate before update(). A callback
                        // racing with this GUI-thread turn can enqueue one more
                        // update, while every callback observed before this point
                        // is covered by the update below.
                        state->updateQueued.store(false, std::memory_order_release);
                        if (item)
                            item->update();
                    },
                    Qt::QueuedConnection);
            }
        }
        if (!queued)
            state->updateQueued.store(false, std::memory_order_release);
    }

    std::shared_ptr<mpv_handle> m_mpvOwner;
    std::shared_ptr<MpvRenderState> m_state;
    mpv_render_context *m_context = nullptr;
};

MpvVideoItem::MpvVideoItem(QQuickItem *parent)
    : MpvVideoItem(
          parent,
          QDir(QStandardPaths::writableLocation(
              QStandardPaths::AppDataLocation))
              .filePath(QStringLiteral("models/upscaling")))
{
}

MpvVideoItem::MpvVideoItem(
    QQuickItem *parent,
    const QString &upscalingAssetRoot)
    : QQuickFramebufferObject(parent)
    , m_mpvOwner(createMpvHandle(), [](mpv_handle *handle) {
        if (handle)
            mpv_terminate_destroy(handle);
    })
    , m_mpv(m_mpvOwner.get())
    , m_renderState(std::make_shared<MpvRenderState>())
    , m_upscalingAssetRoot(QDir::cleanPath(
          QFileInfo(upscalingAssetRoot).absoluteFilePath()))
{
    if (!m_mpv)
        throw std::runtime_error("mpv_create failed");
    m_renderState->item = this;
    m_renderState->diagnosticsEnabled = DevelopmentHooks::isSet(
        DevelopmentHooks::Variable::RenderDiagnostics);
    m_performanceTraceEnabled =
        YanamiPerformance::PerformanceTrace::enabled();

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

    for (const QByteArray &name : {
             QByteArrayLiteral("scale"),
             QByteArrayLiteral("cscale"),
             QByteArrayLiteral("dscale"),
             QByteArrayLiteral("sigmoid-upscaling"),
             QByteArrayLiteral("correct-downscaling"),
             QByteArrayLiteral("linear-downscaling"),
             QByteArrayLiteral("scale-antiring"),
             QByteArrayLiteral("cscale-antiring")}) {
        const QString value = stringProperty(
            m_mpv, QByteArrayLiteral("options/") + name);
        if (!value.isEmpty())
            m_upscalingBaselineOptions.insert(name, value.toUtf8());
    }

    // Network and demux warnings are part of the normal support log. The
    // message is sanitized before it reaches the persistent logger.
    mpv_request_log_messages(m_mpv, "warn");

    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::Pause), "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::TimePosition), "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::Duration), "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::TrackCount), "track-list/count", MPV_FORMAT_INT64);
    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::DemuxerCacheTime), "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::PausedForCache), "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::Volume), "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::Mute), "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::Speed), "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::Seekable), "seekable", MPV_FORMAT_FLAG);
    // MPV_EVENT_END_FILE is not emitted at natural EOF while keep-open keeps
    // the file loaded. eof-reached is the authoritative completion boundary.
    mpv_observe_property(
        m_mpv, observerId(ObservedProperty::EofReached), "eof-reached", MPV_FORMAT_FLAG);
    if (m_performanceTraceEnabled)
        ensurePerformanceObserversRegistered();
    mpv_set_wakeup_callback(m_mpv, &MpvVideoItem::wakeup, this);

    m_playbackStallClock.start();
    m_playbackStallTimer.setInterval(playbackStallPollIntervalMs);
    m_playbackStallTimer.setTimerType(Qt::CoarseTimer);
    connect(
        &m_playbackStallTimer,
        &QTimer::timeout,
        this,
        &MpvVideoItem::pollPlaybackStall);

    m_upscalingHealthTimer.setInterval(1000);
    m_upscalingHealthTimer.setTimerType(Qt::CoarseTimer);
    connect(
        &m_upscalingHealthTimer,
        &QTimer::timeout,
        this,
        &MpvVideoItem::pollUpscalingHealth);

#ifdef YANAMI_ENABLE_DEV_HOOKS
    if (m_renderState->diagnosticsEnabled) {
        auto *diagnosticTimer = new QTimer(this);
        diagnosticTimer->setInterval(1000);
        connect(diagnosticTimer, &QTimer::timeout, this, [this] {
            const quint64 renderCount = m_renderState->renderCount.exchange(
                0, std::memory_order_relaxed);
            const quint64 totalNanoseconds = m_renderState->renderTotalNanoseconds.exchange(
                0, std::memory_order_relaxed);
            const quint64 maximumNanoseconds = m_renderState->renderMaximumNanoseconds.exchange(
                0, std::memory_order_relaxed);
            qInfo() << "render-load mpvCallbacks="
                    << m_renderState->callbackCount.exchange(
                           0, std::memory_order_relaxed)
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

bool MpvVideoItem::ensurePerformanceObserversRegistered()
{
    if (m_performanceObserversRegistered)
        return true;
    if (!m_mpv)
        return false;
    bool registered = true;
    registered &= mpv_observe_property(
        m_mpv,
        observerId(ObservedProperty::DecoderFrameDropCount),
        "decoder-frame-drop-count",
        MPV_FORMAT_INT64) >= 0;
    registered &= mpv_observe_property(
        m_mpv,
        observerId(ObservedProperty::FrameDropCount),
        "frame-drop-count",
        MPV_FORMAT_INT64) >= 0;
    registered &= mpv_observe_property(
        m_mpv,
        observerId(ObservedProperty::MistimedFrameCount),
        "mistimed-frame-count",
        MPV_FORMAT_INT64) >= 0;
    registered &= mpv_observe_property(
        m_mpv,
        observerId(ObservedProperty::DelayedFrameCount),
        "vo-delayed-frame-count",
        MPV_FORMAT_INT64) >= 0;
    registered &= mpv_observe_property(
        m_mpv,
        observerId(ObservedProperty::AvSync),
        "avsync",
        MPV_FORMAT_DOUBLE) >= 0;
    registered &= mpv_observe_property(
        m_mpv,
        observerId(ObservedProperty::EstimatedVideoFps),
        "estimated-vf-fps",
        MPV_FORMAT_DOUBLE) >= 0;
    m_performanceObserversRegistered = registered;
    return registered;
}

bool MpvVideoItem::setUpscalingShaderPathsAsync(
    const QStringList &paths, quint64 replyUserdata)
{
    std::vector<QByteArray> encoded;
    encoded.reserve(paths.size());
    for (const QString &path : paths)
        encoded.push_back(QFile::encodeName(path));
    std::vector<mpv_node> values(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        values[index].format = MPV_FORMAT_STRING;
        values[index].u.string = encoded[index].data();
    }
    mpv_node_list list {
        static_cast<int>(values.size()),
        values.empty() ? nullptr : values.data(),
        nullptr,
    };
    mpv_node node;
    node.format = MPV_FORMAT_NODE_ARRAY;
    node.u.list = &list;
    return mpv_set_property_async(
        m_mpv,
        replyUserdata,
        "glsl-shaders",
        MPV_FORMAT_NODE,
        &node) >= 0;
}

bool MpvVideoItem::setUpscalingStringPropertyAsync(
    const QByteArray &name,
    const QByteArray &value,
    quint64 replyUserdata)
{
    char *encoded = const_cast<char *>(value.constData());
    return mpv_set_property_async(
        m_mpv,
        replyUserdata,
        name.constData(),
        MPV_FORMAT_STRING,
        &encoded) >= 0;
}

void MpvVideoItem::appendUpscalingBaselineWrites(
    PendingUpscalingConfiguration &pending)
{
    pending.writes.append({
        PendingUpscalingConfiguration::PropertyWrite::Kind::ShaderPaths,
        QByteArrayLiteral("glsl-shaders"),
        {},
        {},
        QStringLiteral("mpv-shader-list-rejected"),
    });
    // Yanami does not currently set shader parameters, but clearing this
    // option prevents a future profile revision from leaking into the next
    // episode or into original-quality playback.
    pending.writes.append({
        PendingUpscalingConfiguration::PropertyWrite::Kind::String,
        QByteArrayLiteral("glsl-shader-opts"),
        {},
        {},
        QStringLiteral("mpv-option-rejected"),
    });

    QList<QByteArray> baselineNames = m_upscalingBaselineOptions.keys();
    std::sort(baselineNames.begin(), baselineNames.end());
    for (const QByteArray &name : baselineNames) {
        pending.writes.append({
            PendingUpscalingConfiguration::PropertyWrite::Kind::String,
            name,
            m_upscalingBaselineOptions.value(name),
            {},
            QStringLiteral("mpv-option-rejected"),
        });
    }
}

void MpvVideoItem::appendUpscalingTargetWrites(
    PendingUpscalingConfiguration &pending,
    const YanamiUpscaling::ValidatedConfig &config)
{
    for (auto iterator = config.whitelistedOptions.cbegin();
         iterator != config.whitelistedOptions.cend(); ++iterator) {
        QByteArray encoded;
        if (iterator.value().metaType().id() == QMetaType::Bool) {
            encoded = iterator.value().toBool()
                ? QByteArrayLiteral("yes") : QByteArrayLiteral("no");
        } else if (iterator.value().canConvert<double>()
                   && iterator.value().metaType().id() != QMetaType::QString) {
            encoded = QByteArray::number(iterator.value().toDouble(), 'f', 3);
        } else {
            encoded = iterator.value().toString().toUtf8();
        }
        pending.writes.append({
            PendingUpscalingConfiguration::PropertyWrite::Kind::String,
            iterator.key().toUtf8(),
            encoded,
            {},
            QStringLiteral("mpv-option-rejected"),
            true,
        });
    }
    pending.writes.append({
        PendingUpscalingConfiguration::PropertyWrite::Kind::ShaderPaths,
        QByteArrayLiteral("glsl-shaders"),
        {},
        config.orderedShaderPaths,
        QStringLiteral("mpv-shader-list-rejected"),
        true,
    });
}

void MpvVideoItem::resetUpscalingRenderCounters()
{
    m_renderState->upscalingRenderCount.store(0, std::memory_order_relaxed);
    m_renderState->upscalingRenderTotalNanoseconds.store(
        0, std::memory_order_relaxed);
    m_renderState->upscalingRenderMaximumNanoseconds.store(
        0, std::memory_order_relaxed);
    for (auto &bucket : m_renderState->upscalingRenderHistogram)
        bucket.store(0, std::memory_order_relaxed);
    m_renderState->upscalingWrapperCount.store(0, std::memory_order_relaxed);
    m_renderState->upscalingWrapperTotalNanoseconds.store(
        0, std::memory_order_relaxed);
    m_renderState->upscalingWrapperMaximumNanoseconds.store(
        0, std::memory_order_relaxed);
    for (auto &bucket : m_renderState->upscalingWrapperHistogram)
        bucket.store(0, std::memory_order_relaxed);
    m_upscalingLastRenderP95Ms = 0.0;
    m_upscalingLastRenderAverageMs = 0.0;
    m_upscalingLastRenderMaximumMs = 0.0;
    m_upscalingPreviousOutputDroppedFrames = m_outputDroppedFrames;
    m_upscalingPreviousMistimedFrames = m_mistimedFrames;
    m_upscalingPreviousDelayedFrames = m_delayedFrames;
}

void MpvVideoItem::clearUpscalingState()
{
    const bool changed = m_upscalingActive
        || !m_effectiveUpscalingProfile.isEmpty();
    m_upscalingActive = false;
    m_upscalingPerformanceProtection = false;
    m_effectiveUpscalingProfile.clear();
    m_effectiveUpscalingProvider.clear();
    m_effectiveUpscalingVersion.clear();
    m_upscalingFallbacks.clear();
    m_renderState->upscalingMonitoringEnabled.store(
        false, std::memory_order_release);
    m_upscalingHealthTimer.stop();
    resetUpscalingRenderCounters();
    if (changed)
        emit upscalingStateChanged();
}

void MpvVideoItem::commitUpscalingState(
    const YanamiUpscaling::ValidatedConfig &config)
{
    const bool changed = !m_upscalingActive
        || m_effectiveUpscalingProfile != config.profileId;
    m_upscalingActive = true;
    m_effectiveUpscalingProfile = config.profileId;
    m_effectiveUpscalingProvider = config.providerId;
    m_effectiveUpscalingVersion = config.modelVersion;
    m_upscalingFallbacks.clear();
    for (const QVariantMap &fallback : config.fallbacks)
        m_upscalingFallbacks.append(fallback);
    m_upscalingPerformanceProtection = config.performanceProtection
        && ensurePerformanceObserversRegistered();
    m_upscalingProtection.reset(
        m_upscalingFallbacks.size(), config.reservedHeadroomPercent);
    resetUpscalingRenderCounters();
    m_renderState->upscalingMonitoringEnabled.store(
        m_upscalingPerformanceProtection, std::memory_order_release);
    if (m_upscalingPerformanceProtection)
        m_upscalingHealthTimer.start();
    else
        m_upscalingHealthTimer.stop();
    if (changed)
        emit upscalingStateChanged();
}

void MpvVideoItem::recordUpscalingApplied(
    const YanamiUpscaling::ValidatedConfig &config,
    double dispatchCpuMs,
    double completionMs)
{
    qCInfo(playbackLog).noquote()
        << "playback_upscaling_applied"
        << "profile=" << m_effectiveUpscalingProfile
        << "provider=" << m_effectiveUpscalingProvider
        << "version=" << m_effectiveUpscalingVersion
        << "shaderCount=" << config.orderedShaderPaths.size()
        << "applyMs=" << dispatchCpuMs
        << "completionMs=" << completionMs;
    if (!m_performanceTraceEnabled)
        return;
    YanamiPerformance::PerformanceTrace::mark(
        QStringLiteral("playback_upscaling_applied"),
        {
            {QStringLiteral("profileId"), m_effectiveUpscalingProfile},
            {QStringLiteral("providerId"), m_effectiveUpscalingProvider},
            {QStringLiteral("modelVersion"), m_effectiveUpscalingVersion},
            {QStringLiteral("shaderCount"), config.orderedShaderPaths.size()},
            {QStringLiteral("applyMs"), dispatchCpuMs},
            {QStringLiteral("completionMs"), completionMs},
        });
}

void MpvVideoItem::open(const QUrl &url, const QVariantMap &headers)
{
    openWithUpscaling(url, headers, {});
}

bool MpvVideoItem::beginUpscalingRuntimeConfig(
    const QVariantMap &runtimeConfig,
    const QString &transitionFromProfile,
    double transitionRenderP95Ms,
    bool transitionToOriginal,
    const QString &completionFallbackCode,
    const QString &completionFallbackMessage)
{
    QElapsedTimer dispatchTimer;
    dispatchTimer.start();

    const auto validation = YanamiUpscaling::validateRuntimeConfig(
        runtimeConfig, m_upscalingAssetRoot);
    if (validation.isValid() && !validation.config.enabled
        && !m_upscalingPropertiesDirty) {
        clearUpscalingState();
        const double dispatchCpuMs =
            dispatchTimer.nsecsElapsed() / 1'000'000.0;
        emit upscalingConfigurationFinished(
            false, true, dispatchCpuMs, dispatchCpuMs);
        continueAfterUpscalingConfiguration();
        return true;
    }
    auto pending = std::make_unique<PendingUpscalingConfiguration>();
    pending->requestedProfileId = runtimeConfig.value(
        QStringLiteral("profileId")).toString();
    pending->requestEnabled = runtimeConfig.value(
        QStringLiteral("enabled")).toBool();
    pending->transitionFromProfile = transitionFromProfile;
    pending->transitionRenderP95Ms = transitionRenderP95Ms;
    pending->transitionToOriginal = transitionToOriginal;
    pending->completionFallbackCode = completionFallbackCode;
    pending->completionFallbackMessage = completionFallbackMessage;
    pending->completionTimer.start();
    appendUpscalingBaselineWrites(*pending);

    bool accepted = true;
    if (!validation.isValid()) {
        accepted = false;
        pending->rollingBack = true;
        pending->failureErrorCode = validation.errorCode;
        qCWarning(playbackLog).noquote()
            << "playback_upscaling_config_rejected"
            << "errorCode=" << validation.errorCode;
    } else {
        pending->config = validation.config;
        pending->requestEnabled = validation.config.enabled;
        if (!validation.config.profileId.isEmpty())
            pending->requestedProfileId = validation.config.profileId;
    }

    // This renderer exposes the classic libmpv OpenGL API. Keep a final
    // identity check here even though the runtime validator already enforces
    // the Anime4K + GLSL product contract.
    if (accepted && pending->config.enabled
        && (pending->config.providerId
                != QLatin1String(YanamiUpscaling::Id::ProviderAnime4k)
            || pending->config.backendKind
                != YanamiUpscaling::BackendKind::GlslShaders)) {
        accepted = false;
        pending->rollingBack = true;
        pending->failureErrorCode =
            QStringLiteral("unsupported-render-backend");
        qCWarning(playbackLog).noquote()
            << "playback_upscaling_backend_unsupported"
            << "provider=" << pending->config.providerId
            << "backendKind="
            << static_cast<int>(pending->config.backendKind);
    }

    if (accepted && pending->config.enabled) {
        appendUpscalingTargetWrites(*pending, pending->config);
    }

    // The logical state changes immediately, while every potentially
    // expensive libmpv property mutation is serialized asynchronously. This
    // keeps QML responsive and prevents mixed old/new option sets.
    clearUpscalingState();
    pending->dispatchCpuMs =
        dispatchTimer.nsecsElapsed() / 1'000'000.0;
    m_pendingUpscalingConfiguration = std::move(pending);
    emit upscalingConfigurationPendingChanged();
    dispatchNextUpscalingProperty();
    return accepted;
}

bool MpvVideoItem::configureUpscaling(const QVariantMap &runtimeConfig)
{
    if (m_pendingUpscalingConfiguration) {
        if (m_deferredOpenRequest) {
            m_deferredOpenRequest->runtimeConfig = runtimeConfig;
            return true;
        }
        m_queuedUpscalingRuntimeConfig = runtimeConfig;
        m_hasQueuedUpscalingRuntimeConfig = true;
        return true;
    }
    return beginUpscalingRuntimeConfig(runtimeConfig);
}

void MpvVideoItem::dispatchNextUpscalingProperty()
{
    if (!m_pendingUpscalingConfiguration)
        return;

    while (m_pendingUpscalingConfiguration) {
        auto &pending = *m_pendingUpscalingConfiguration;
        if (pending.nextWriteIndex >= pending.writes.size()) {
            completeUpscalingRuntimeConfig();
            return;
        }

        ++m_nextUpscalingRequestSerial;
        m_nextUpscalingRequestSerial &= ~upscalingAsyncReplyTag;
        if (m_nextUpscalingRequestSerial == 0)
            ++m_nextUpscalingRequestSerial;
        pending.replyUserdata = upscalingAsyncReplyTag
            | m_nextUpscalingRequestSerial;

        const auto &write = pending.writes.at(pending.nextWriteIndex);
        if (write.targetWrite)
            m_upscalingPropertiesDirty = true;
        QElapsedTimer callTimer;
        callTimer.start();
        const bool dispatched = write.kind
                == PendingUpscalingConfiguration::PropertyWrite::Kind::ShaderPaths
            ? setUpscalingShaderPathsAsync(
                  write.shaderPaths, pending.replyUserdata)
            : setUpscalingStringPropertyAsync(
                  write.name, write.stringValue, pending.replyUserdata);
        pending.dispatchCpuMs += callTimer.nsecsElapsed() / 1'000'000.0;
        if (dispatched)
            return;

        // mpv promises no reply when the async call itself is rejected.
        if (!pending.rollingBack) {
            beginUpscalingRollback(write.errorCode);
            return;
        }
        pending.rollbackFailed = true;
        qCWarning(playbackLog).noquote()
            << "playback_upscaling_baseline_restore_failed"
            << "option=" << write.name;
        ++pending.nextWriteIndex;
    }
}

void MpvVideoItem::beginUpscalingRollback(const QString &errorCode)
{
    if (!m_pendingUpscalingConfiguration)
        return;
    auto &pending = *m_pendingUpscalingConfiguration;
    if (pending.failureErrorCode.isEmpty())
        pending.failureErrorCode = errorCode;
    pending.rollingBack = true;
    pending.writes.clear();
    pending.nextWriteIndex = 0;
    pending.replyUserdata = 0;
    appendUpscalingBaselineWrites(pending);
    dispatchNextUpscalingProperty();
}

void MpvVideoItem::finishUpscalingRuntimeConfig(int errorCode)
{
    if (!m_pendingUpscalingConfiguration)
        return;

    // New media has priority over every obsolete configuration, and a rapid
    // toggle keeps only the latest desired state. A fresh transaction always
    // begins with the baseline, so abandoning the remainder is safe once the
    // single in-flight reply has arrived.
    if (m_deferredOpenRequest) {
        m_queuedUpscalingRuntimeConfig.clear();
        m_hasQueuedUpscalingRuntimeConfig = false;
        m_pendingUpscalingConfiguration.reset();
        emit upscalingConfigurationPendingChanged();
        continueAfterUpscalingConfiguration();
        return;
    }
    if (m_hasQueuedUpscalingRuntimeConfig) {
        const QVariantMap queued = m_queuedUpscalingRuntimeConfig;
        m_queuedUpscalingRuntimeConfig.clear();
        m_hasQueuedUpscalingRuntimeConfig = false;
        m_pendingUpscalingConfiguration.reset();
        emit upscalingConfigurationPendingChanged();
        beginUpscalingRuntimeConfig(queued);
        return;
    }

    auto &pending = *m_pendingUpscalingConfiguration;
    const auto &write = pending.writes.at(pending.nextWriteIndex);
    if (pending.cancelled && !pending.rollingBack) {
        beginUpscalingRollback({});
        return;
    }
    if (errorCode < 0) {
        if (!pending.rollingBack) {
            beginUpscalingRollback(write.errorCode);
            return;
        }
        pending.rollbackFailed = true;
        qCWarning(playbackLog).noquote()
            << "playback_upscaling_baseline_restore_failed"
            << "option=" << write.name
            << "mpvError=" << errorCode;
    }
    ++pending.nextWriteIndex;
    dispatchNextUpscalingProperty();
}

void MpvVideoItem::completeUpscalingRuntimeConfig()
{
    if (!m_pendingUpscalingConfiguration)
        return;

    auto completed = std::move(m_pendingUpscalingConfiguration);
    emit upscalingConfigurationPendingChanged();
    const double completionMs = completed->completionTimer.nsecsElapsed()
        / 1'000'000.0;

    if (completed->rollingBack) {
        clearUpscalingState();
        if (!completed->rollbackFailed)
            m_upscalingPropertiesDirty = false;
        if (!completed->cancelled
            && !completed->failureErrorCode.isEmpty()) {
            const QString message = completed->rollbackFailed
                ? tr("Real-time upscaling cleanup failed. Restart playback to restore original quality.")
                : (!completed->transitionFromProfile.isEmpty()
                       && !completed->transitionToOriginal
                    ? tr("Real-time upscaling was disabled because the lower performance tier could not be applied.")
                    : tr("Real-time upscaling could not be applied. Playback is continuing in original quality."));
            const QString fallbackErrorCode = completed->rollbackFailed
                ? QStringLiteral("mpv-baseline-restore-failed")
                : completed->failureErrorCode;
            emit upscalingFallback(
                completed->requestedProfileId,
                fallbackErrorCode,
                message);
            emit upscalingConfigurationFinished(
                completed->requestEnabled,
                false,
                completed->dispatchCpuMs,
                completionMs);
        }
    } else if (completed->config.enabled) {
        m_upscalingPropertiesDirty = true;
        commitUpscalingState(completed->config);
        recordUpscalingApplied(
            completed->config,
            completed->dispatchCpuMs,
            completionMs);
        if (!completed->transitionFromProfile.isEmpty()) {
            qCWarning(playbackLog).noquote()
                << "playback_upscaling_auto_downgraded"
                << "from=" << completed->transitionFromProfile
                << "to=" << m_effectiveUpscalingProfile
                << "renderP95Ms=" << completed->transitionRenderP95Ms;
            emit upscalingTierChanged(
                completed->transitionFromProfile,
                m_effectiveUpscalingProfile,
                QStringLiteral("frame-budget"));
            if (m_performanceTraceEnabled) {
                YanamiPerformance::PerformanceTrace::mark(
                    QStringLiteral("playback_upscaling_auto_downgraded"),
                    {
                        {QStringLiteral("fromProfileId"),
                         completed->transitionFromProfile},
                        {QStringLiteral("toProfileId"),
                         m_effectiveUpscalingProfile},
                        {QStringLiteral("renderP95Ms"),
                         completed->transitionRenderP95Ms},
                    });
            }
        }
        emit upscalingConfigurationFinished(
            true,
            true,
            completed->dispatchCpuMs,
            completionMs);
    } else {
        m_upscalingPropertiesDirty = false;
        clearUpscalingState();
        if (completed->transitionToOriginal
            && !completed->transitionFromProfile.isEmpty()) {
            qCWarning(playbackLog).noquote()
                << "playback_upscaling_auto_disabled"
                << "from=" << completed->transitionFromProfile
                << "reason=frame-budget";
            emit upscalingTierChanged(
                completed->transitionFromProfile,
                QStringLiteral("original"),
                QStringLiteral("frame-budget"));
            emit upscalingFallback(
                completed->transitionFromProfile,
                QStringLiteral("frame-budget"),
                tr("Real-time upscaling was disabled for this playback to keep video smooth."));
        } else if (!completed->completionFallbackCode.isEmpty()) {
            emit upscalingFallback(
                completed->requestedProfileId,
                completed->completionFallbackCode,
                completed->completionFallbackMessage);
        }
        emit upscalingConfigurationFinished(
            false,
            true,
            completed->dispatchCpuMs,
            completionMs);
    }

    continueAfterUpscalingConfiguration();
}

void MpvVideoItem::continueAfterUpscalingConfiguration()
{
    if (m_pendingUpscalingConfiguration)
        return;
    if (m_deferredOpenRequest) {
        m_pendingLoadTarget.clear();
        m_pendingLoadGeneration = 0;
        m_pendingLoadWaitsForUpscaling = false;
        auto deferred = std::move(m_deferredOpenRequest);
        openWithUpscaling(
            deferred->url,
            deferred->headers,
            deferred->runtimeConfig);
        return;
    }
    if (m_pendingLoadWaitsForUpscaling) {
        m_pendingLoadWaitsForUpscaling = false;
        update();
        dispatchPendingLoad();
    }
}

void MpvVideoItem::openWithUpscaling(
    const QUrl &url,
    const QVariantMap &headers,
    const QVariantMap &runtimeConfig)
{
    if (m_pendingUpscalingConfiguration) {
        m_deferredOpenRequest = std::make_unique<DeferredOpenRequest>(
            DeferredOpenRequest {url, headers, runtimeConfig});
        return;
    }
    resetPlaybackStall();
    ++m_loadGeneration;
    m_loadTimer.restart();
    m_bufferingTimer.invalidate();
    m_bufferingTransitions = 0;
    m_totalBufferingMs = 0;
    if (m_performanceTraceEnabled || m_upscalingPerformanceProtection) {
        m_decoderDroppedFrames = 0;
        m_outputDroppedFrames = 0;
        m_mistimedFrames = 0;
        m_delayedFrames = 0;
        m_avSyncSeconds = 0.0;
        m_estimatedVideoFps = 0.0;
        m_decoderDroppedFramesAvailable = false;
        m_outputDroppedFramesAvailable = false;
        m_mistimedFramesAvailable = false;
        m_delayedFramesAvailable = false;
        m_avSyncAvailable = false;
        m_estimatedVideoFpsAvailable = false;
        m_firstPlaybackRestartObserved = false;
        m_seekTimer.invalidate();
    }
    m_pauseRequested = m_paused;
    qCInfo(playbackLog).noquote()
        << "playback_mpv_open"
        << "generation=" << m_loadGeneration
        << "source=" << (url.isLocalFile() ? "local" : "remote")
        << "scheme=" << url.scheme().toLower()
        << "headerCount=" << headers.size();
    if (m_performanceTraceEnabled) {
        YanamiPerformance::PerformanceTrace::mark(
            QStringLiteral("playback_open_requested"),
            {
                {QStringLiteral("generation"), m_loadGeneration},
                {QStringLiteral("source"), url.isLocalFile() ? QStringLiteral("local")
                                                             : QStringLiteral("remote")},
            });
    }
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
    m_pendingLoadTarget = url.toString(QUrl::FullyEncoded).toUtf8();
    m_pendingLoadGeneration = m_loadGeneration;
    m_pendingLoadWaitsForUpscaling = true;
    configureUpscaling(runtimeConfig);
    if (!m_pendingUpscalingConfiguration)
        m_pendingLoadWaitsForUpscaling = false;
    // QQuickFramebufferObject creates libmpv's render context lazily on the
    // scene-graph thread. Queue the load until that context exists; otherwise
    // an immediate first playback can fail with "No render context set".
    update();
    dispatchPendingLoad();
}

void MpvVideoItem::dispatchPendingLoad()
{
    if (m_pendingLoadTarget.isEmpty()
        || m_pendingLoadGeneration != m_loadGeneration
        || m_pendingLoadWaitsForUpscaling
        || !m_renderState->rendererReady.load(std::memory_order_acquire)) {
        return;
    }
    const QByteArray target = std::exchange(
        m_pendingLoadTarget, QByteArray{});
    m_pendingLoadGeneration = 0;
    qCInfo(playbackLog).noquote()
        << "playback_mpv_load_dispatched"
        << "generation=" << m_loadGeneration;
    command({"loadfile", target, "replace"});
}

void MpvVideoItem::stop()
{
    m_deferredOpenRequest.reset();
    m_queuedUpscalingRuntimeConfig.clear();
    m_hasQueuedUpscalingRuntimeConfig = false;
    m_pendingLoadTarget.clear();
    m_pendingLoadGeneration = 0;
    m_pendingLoadWaitsForUpscaling = false;
    if (m_pendingUpscalingConfiguration)
        m_pendingUpscalingConfiguration->cancelled = true;
    else
        beginUpscalingRuntimeConfig({
            {QStringLiteral("schema"), 1},
            {QStringLiteral("enabled"), false},
        });
    clearUpscalingState();
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
    if (m_performanceTraceEnabled) {
        YanamiPerformance::PerformanceTrace::mark(
            QStringLiteral("playback_stopped"), performanceSnapshot());
    }
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
        if (m_performanceTraceEnabled)
            m_seekTimer.restart();
        m_playbackStallWatchdog.beginSeek(playbackStallNow(), m_position);
        m_playbackStallTimer.start();
    }
    command({"seek", QByteArray::number(target, 'f', 3), "absolute+exact"});
    emit seekRequested(target);
    if (m_performanceTraceEnabled) {
        YanamiPerformance::PerformanceTrace::mark(
            QStringLiteral("playback_seek_requested"),
            {
                {QStringLiteral("generation"), m_loadGeneration},
                {QStringLiteral("targetSeconds"), target},
            });
    }
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
    if (!item || item->m_eventDrainQueued.exchange(true, std::memory_order_acq_rel))
        return;
    if (!QMetaObject::invokeMethod(
            item, &MpvVideoItem::drainEvents, Qt::QueuedConnection)) {
        item->m_eventDrainQueued.store(false, std::memory_order_release);
    }
}

void MpvVideoItem::drainEvents()
{
    // Clear before draining. Any event that arrives during this turn may queue
    // one redundant follow-up drain, but an event racing with MPV_EVENT_NONE
    // can never be stranded behind a still-set coalescing flag.
    m_eventDrainQueued.store(false, std::memory_order_release);
    while (const mpv_event *event = mpv_wait_event(m_mpv, 0)) {
        if (event->event_id == MPV_EVENT_NONE)
            break;
        if (event->event_id == MPV_EVENT_SET_PROPERTY_REPLY
            && m_pendingUpscalingConfiguration
            && event->reply_userdata
                == m_pendingUpscalingConfiguration->replyUserdata) {
            finishUpscalingRuntimeConfig(event->error);
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
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
            if (m_performanceTraceEnabled) {
                QVariantMap loadedAttributes = performanceSnapshot();
                loadedAttributes.insert(
                    QStringLiteral("hwdecCurrent"),
                    stringProperty(m_mpv, QByteArrayLiteral("hwdec-current")));
                YanamiPerformance::PerformanceTrace::mark(
                    QStringLiteral("playback_file_loaded"), loadedAttributes);
            }
            emit fileLoaded();
        } else if (event->event_id == MPV_EVENT_SEEK) {
            if (m_fileLoaded) {
                m_playbackStallWatchdog.observeSeekStarted(
                    playbackStallNow(), m_position);
            }
        } else if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            if (m_fileLoaded) {
                m_playbackStallWatchdog.observePlaybackRestart(
                    playbackStallNow(), m_position);
                if (m_performanceTraceEnabled) {
                    if (!m_firstPlaybackRestartObserved) {
                        m_firstPlaybackRestartObserved = true;
                        QVariantMap firstFrameAttributes = performanceSnapshot();
                        firstFrameAttributes.insert(
                            QStringLiteral("hwdecCurrent"),
                            stringProperty(m_mpv, QByteArrayLiteral("hwdec-current")));
                        YanamiPerformance::PerformanceTrace::mark(
                            QStringLiteral("playback_first_frame_candidate"),
                            firstFrameAttributes);
                    } else if (m_seekTimer.isValid()) {
                        QVariantMap seekAttributes = performanceSnapshot();
                        seekAttributes.insert(
                            QStringLiteral("seekElapsedMs"), m_seekTimer.elapsed());
                        YanamiPerformance::PerformanceTrace::mark(
                            QStringLiteral("playback_seek_present_candidate"),
                            seekAttributes);
                        m_seekTimer.invalidate();
                    }
                }
            }
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            auto *endFile = static_cast<mpv_event_end_file *>(event->data);
            const bool explicitlyStopped = endFile
                && endFile->reason == MPV_END_FILE_REASON_STOP;
            if (m_buffering && m_bufferingTimer.isValid()) {
                m_totalBufferingMs += m_bufferingTimer.elapsed();
                m_bufferingTimer.invalidate();
            }
            m_fileLoaded = false;
            m_buffering = false;
            resetPlaybackStall();
            setPlaybackState(explicitlyStopped
                    ? PlaybackState::Idle : PlaybackState::Ended);
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
            if (m_performanceTraceEnabled) {
                YanamiPerformance::PerformanceTrace::mark(
                    QStringLiteral("playback_file_ended"), performanceSnapshot());
            }
            if (endFile && endFile->error < 0) {
                qCWarning(playbackLog).noquote()
                    << "playback_mpv_error"
                    << "generation=" << m_loadGeneration
                    << "errorCode=" << endFile->error
                    << "error=" << mpv_error_string(endFile->error);
                if (m_performanceTraceEnabled) {
                    YanamiPerformance::PerformanceTrace::mark(
                        QStringLiteral("playback_error"),
                        {
                            {QStringLiteral("generation"), m_loadGeneration},
                            {QStringLiteral("errorCode"), endFile->error},
                        });
                }
                emit playbackError(
                    tr("Playback failed: %1")
                        .arg(QString::fromUtf8(mpv_error_string(endFile->error))));
            }
            emit fileEnded();
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto *property = static_cast<mpv_event_property *>(event->data);
            if (!property)
                continue;
            const auto observedProperty = static_cast<ObservedProperty>(
                event->reply_userdata);
            if (observedProperty == ObservedProperty::TrackCount) {
                refreshTracks();
            } else if (!property->data) {
                continue;
            } else if (observedProperty == ObservedProperty::Pause
                       && property->format == MPV_FORMAT_FLAG) {
                const bool value = *static_cast<int *>(property->data) != 0;
                if (m_paused != value) {
                    m_paused = value;
                    emit pausedChanged();
                }
                m_pauseRequested = value;
                updatePlaybackPauseMonitoring(value);
            } else if (observedProperty == ObservedProperty::PausedForCache
                       && property->format == MPV_FORMAT_FLAG) {
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
                    if (m_performanceTraceEnabled) {
                        QVariantMap bufferingAttributes = performanceSnapshot();
                        bufferingAttributes.insert(QStringLiteral("active"), buffering);
                        bufferingAttributes.insert(QStringLiteral("intervalMs"), intervalMs);
                        YanamiPerformance::PerformanceTrace::mark(
                            QStringLiteral("playback_buffering_changed"),
                            bufferingAttributes);
                    }
                    m_buffering = buffering;
                }
                refreshPlaybackState();
            } else if (observedProperty == ObservedProperty::TimePosition
                       && property->format == MPV_FORMAT_DOUBLE) {
                m_position = *static_cast<double *>(property->data);
                emit positionChanged();
                if (m_fileLoaded) {
                    handlePlaybackStallEvent(
                        m_playbackStallWatchdog.observePosition(
                            playbackStallNow(), m_position));
                }
            } else if (observedProperty == ObservedProperty::Duration
                       && property->format == MPV_FORMAT_DOUBLE) {
                m_duration = *static_cast<double *>(property->data);
                emit durationChanged();
            } else if (observedProperty == ObservedProperty::DemuxerCacheTime
                       && property->format == MPV_FORMAT_DOUBLE) {
                const double value = *static_cast<double *>(property->data);
                if (std::isfinite(value)) {
                    m_bufferedPosition = std::max(0.0, value);
                    emit bufferedPositionChanged();
                }
            } else if (observedProperty == ObservedProperty::Volume
                       && property->format == MPV_FORMAT_DOUBLE) {
                const double value = std::clamp(*static_cast<double *>(property->data), 0.0, 100.0);
                if (std::abs(m_volume - value) > 0.01) {
                    m_volume = value;
                    emit volumeChanged();
                }
            } else if (observedProperty == ObservedProperty::Mute
                       && property->format == MPV_FORMAT_FLAG) {
                const bool value = *static_cast<int *>(property->data) != 0;
                if (m_muted != value) {
                    m_muted = value;
                    emit mutedChanged();
                }
            } else if (observedProperty == ObservedProperty::Speed
                       && property->format == MPV_FORMAT_DOUBLE) {
                const double value = *static_cast<double *>(property->data);
                if (std::isfinite(value) && std::abs(m_rate - value) > 0.001) {
                    m_rate = value;
                    emit rateChanged();
                }
            } else if (observedProperty == ObservedProperty::Seekable
                       && property->format == MPV_FORMAT_FLAG) {
                const bool value = *static_cast<int *>(property->data) != 0;
                if (m_seekable != value) {
                    m_seekable = value;
                    emit seekableChanged();
                }
            } else if (observedProperty == ObservedProperty::EofReached
                       && property->format == MPV_FORMAT_FLAG) {
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
                    if (m_performanceTraceEnabled) {
                        QVariantMap eofAttributes = performanceSnapshot();
                        eofAttributes.insert(
                            QStringLiteral("natural"),
                            *boundary == YanamiPlayback::CompletionBoundary::Natural);
                        YanamiPerformance::PerformanceTrace::mark(
                            QStringLiteral("playback_eof"), eofAttributes);
                    }
                    if (*boundary == YanamiPlayback::CompletionBoundary::Natural) {
                        emit playbackCompleted();
                    } else {
                        qCWarning(playbackLog).noquote()
                            << "playback_mpv_premature_eof"
                            << "generation=" << m_loadGeneration
                            << "positionSeconds=" << m_position
                            << "durationSeconds=" << m_duration;
                        if (m_performanceTraceEnabled) {
                            YanamiPerformance::PerformanceTrace::mark(
                                QStringLiteral("playback_premature_eof"),
                                performanceSnapshot());
                        }
                        emit playbackError(tr(
                            "Playback ended before the media was complete."));
                        emit fileEnded();
                    }
                }
            } else if (observedProperty == ObservedProperty::DecoderFrameDropCount
                       && property->format == MPV_FORMAT_INT64) {
                m_decoderDroppedFrames = *static_cast<int64_t *>(property->data);
                m_decoderDroppedFramesAvailable = true;
            } else if (observedProperty == ObservedProperty::FrameDropCount
                       && property->format == MPV_FORMAT_INT64) {
                m_outputDroppedFrames = *static_cast<int64_t *>(property->data);
                m_outputDroppedFramesAvailable = true;
            } else if (observedProperty == ObservedProperty::MistimedFrameCount
                       && property->format == MPV_FORMAT_INT64) {
                m_mistimedFrames = *static_cast<int64_t *>(property->data);
                m_mistimedFramesAvailable = true;
            } else if (observedProperty == ObservedProperty::DelayedFrameCount
                       && property->format == MPV_FORMAT_INT64) {
                m_delayedFrames = *static_cast<int64_t *>(property->data);
                m_delayedFramesAvailable = true;
            } else if (observedProperty == ObservedProperty::AvSync
                       && property->format == MPV_FORMAT_DOUBLE) {
                const double value = *static_cast<double *>(property->data);
                if (std::isfinite(value)) {
                    m_avSyncSeconds = value;
                    m_avSyncAvailable = true;
                }
            } else if (observedProperty == ObservedProperty::EstimatedVideoFps
                       && property->format == MPV_FORMAT_DOUBLE) {
                const double value = *static_cast<double *>(property->data);
                if (std::isfinite(value)) {
                    m_estimatedVideoFps = value;
                    m_estimatedVideoFpsAvailable = true;
                }
            }
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            auto *message = static_cast<mpv_event_log_message *>(event->data);
            if (message) {
                const QString sanitized = sanitizedMpvLog(message->text);
                qCWarning(playbackLog).noquote()
                    << "mpv_warning"
                    << "generation=" << m_loadGeneration
                    << "component=" << message->prefix
                    << "message=" << sanitized;
                const QString diagnostic =
                    QString::fromUtf8(message->prefix) + QLatin1Char(' ')
                    + sanitized;
                const bool shaderDiagnostic =
                    diagnostic.contains(
                        QStringLiteral("shader"), Qt::CaseInsensitive)
                    || diagnostic.contains(
                        QStringLiteral("glsl"), Qt::CaseInsensitive);
                const bool compileFailure =
                    diagnostic.contains(
                        QStringLiteral("failed"), Qt::CaseInsensitive)
                    || diagnostic.contains(
                        QStringLiteral("error"), Qt::CaseInsensitive)
                    || diagnostic.contains(
                        QStringLiteral("could not"), Qt::CaseInsensitive);
                if (m_upscalingActive && shaderDiagnostic && compileFailure) {
                    const QString failedProfile =
                        m_effectiveUpscalingProfile;
                    beginUpscalingRuntimeConfig(
                        {
                            {QStringLiteral("schema"), 1},
                            {QStringLiteral("enabled"), false},
                            {QStringLiteral("profileId"), failedProfile},
                        },
                        {},
                        0.0,
                        false,
                        QStringLiteral("shader-compile"),
                        tr("The selected upscaling shader could not be compiled. Playback is continuing in original quality."));
                    if (m_performanceTraceEnabled) {
                        YanamiPerformance::PerformanceTrace::mark(
                            QStringLiteral("playback_upscaling_fallback"),
                            {
                                {QStringLiteral("profileId"), failedProfile},
                                {QStringLiteral("errorCode"),
                                 QStringLiteral("shader-compile")},
                            });
                    }
                }
            }
        }
    }
}

QVariantMap MpvVideoItem::performanceSnapshot() const
{
    qint64 totalBufferingMs = m_totalBufferingMs;
    if (m_buffering && m_bufferingTimer.isValid())
        totalBufferingMs += m_bufferingTimer.elapsed();
    return {
        {QStringLiteral("generation"), m_loadGeneration},
        {QStringLiteral("elapsedMs"),
         m_loadTimer.isValid() ? m_loadTimer.elapsed() : -1},
        {QStringLiteral("positionSeconds"), m_position},
        {QStringLiteral("durationSeconds"), m_duration},
        {QStringLiteral("bufferingTransitions"), m_bufferingTransitions},
        {QStringLiteral("totalBufferingMs"), totalBufferingMs},
        {QStringLiteral("decoderDroppedFrames"), m_decoderDroppedFrames},
        {QStringLiteral("outputDroppedFrames"), m_outputDroppedFrames},
        {QStringLiteral("mistimedFrames"), m_mistimedFrames},
        {QStringLiteral("delayedFrames"), m_delayedFrames},
        {QStringLiteral("avSyncMs"), m_avSyncSeconds * 1000.0},
        {QStringLiteral("estimatedVideoFps"), m_estimatedVideoFps},
        {QStringLiteral("performanceObserversRegistered"),
         m_performanceObserversRegistered},
        {QStringLiteral("decoderDroppedFramesAvailable"),
         m_decoderDroppedFramesAvailable},
        {QStringLiteral("outputDroppedFramesAvailable"),
         m_outputDroppedFramesAvailable},
        {QStringLiteral("mistimedFramesAvailable"), m_mistimedFramesAvailable},
        {QStringLiteral("delayedFramesAvailable"), m_delayedFramesAvailable},
        {QStringLiteral("avSyncAvailable"), m_avSyncAvailable},
        {QStringLiteral("estimatedVideoFpsAvailable"),
         m_estimatedVideoFpsAvailable},
        {QStringLiteral("upscalingActive"), m_upscalingActive},
        {QStringLiteral("upscalingProviderId"),
         m_effectiveUpscalingProvider},
        {QStringLiteral("upscalingProfileId"),
         m_effectiveUpscalingProfile},
        {QStringLiteral("upscalingModelVersion"),
         m_effectiveUpscalingVersion},
        {QStringLiteral("upscalingRenderP95Ms"),
         m_upscalingLastRenderP95Ms},
        {QStringLiteral("upscalingRenderAverageMs"),
         m_upscalingLastRenderAverageMs},
        {QStringLiteral("upscalingRenderMaximumMs"),
         m_upscalingLastRenderMaximumMs},
        {QStringLiteral("upscalingOverloadWindows"),
         m_upscalingProtection.consecutiveOverloadWindows()},
    };
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

void MpvVideoItem::pollUpscalingHealth()
{
    if (!m_upscalingActive || !m_upscalingPerformanceProtection)
        return;
    QElapsedTimer healthPollTimer;
    healthPollTimer.start();

    std::array<quint64, upscalingRenderHistogramBucketCount> histogram{};
    quint64 histogramSamples = 0;
    for (std::size_t index = 0; index < histogram.size(); ++index) {
        histogram[index] = m_renderState->upscalingRenderHistogram[index]
            .exchange(0, std::memory_order_relaxed);
        histogramSamples += histogram[index];
    }
    const quint64 renderCount = m_renderState->upscalingRenderCount.exchange(
        0, std::memory_order_relaxed);
    const quint64 totalNanoseconds =
        m_renderState->upscalingRenderTotalNanoseconds.exchange(
            0, std::memory_order_relaxed);
    const quint64 maximumNanoseconds =
        m_renderState->upscalingRenderMaximumNanoseconds.exchange(
            0, std::memory_order_relaxed);
    std::array<quint64, upscalingWrapperHistogramBucketCount> wrapperHistogram{};
    quint64 wrapperHistogramSamples = 0;
    for (std::size_t index = 0; index < wrapperHistogram.size(); ++index) {
        wrapperHistogram[index] = m_renderState->upscalingWrapperHistogram[index]
            .exchange(0, std::memory_order_relaxed);
        wrapperHistogramSamples += wrapperHistogram[index];
    }
    const quint64 wrapperCount = m_renderState->upscalingWrapperCount.exchange(
        0, std::memory_order_relaxed);
    const quint64 wrapperTotalNanoseconds =
        m_renderState->upscalingWrapperTotalNanoseconds.exchange(
            0, std::memory_order_relaxed);
    const quint64 wrapperMaximumNanoseconds =
        m_renderState->upscalingWrapperMaximumNanoseconds.exchange(
            0, std::memory_order_relaxed);
    if (renderCount == 0 || histogramSamples == 0)
        return;

    const quint64 target = std::max<quint64>(
        1, static_cast<quint64>(std::ceil(histogramSamples * 0.95)));
    quint64 cumulative = 0;
    quint64 p95Nanoseconds = maximumNanoseconds;
    for (std::size_t index = 0; index < histogram.size(); ++index) {
        cumulative += histogram[index];
        if (cumulative < target)
            continue;
        p95Nanoseconds = index < upscalingRenderHistogramLimitsNs.size()
            ? upscalingRenderHistogramLimitsNs[index]
            : maximumNanoseconds;
        break;
    }
    m_upscalingLastRenderP95Ms = p95Nanoseconds / 1'000'000.0;
    m_upscalingLastRenderAverageMs =
        totalNanoseconds / 1'000'000.0 / renderCount;
    m_upscalingLastRenderMaximumMs = maximumNanoseconds / 1'000'000.0;

    quint64 wrapperP95Nanoseconds = wrapperMaximumNanoseconds;
    if (wrapperCount > 0 && wrapperHistogramSamples > 0) {
        const quint64 wrapperTarget = std::max<quint64>(
            1, static_cast<quint64>(std::ceil(wrapperHistogramSamples * 0.95)));
        quint64 wrapperCumulative = 0;
        for (std::size_t index = 0; index < wrapperHistogram.size(); ++index) {
            wrapperCumulative += wrapperHistogram[index];
            if (wrapperCumulative < wrapperTarget)
                continue;
            wrapperP95Nanoseconds =
                index < upscalingWrapperHistogramLimitsNs.size()
                ? upscalingWrapperHistogramLimitsNs[index]
                : wrapperMaximumNanoseconds;
            break;
        }
    }
    const double wrapperP95Us = wrapperP95Nanoseconds / 1'000.0;
    const double wrapperAverageUs = wrapperCount > 0
        ? wrapperTotalNanoseconds / 1'000.0 / wrapperCount : 0.0;
    const double wrapperMaximumUs = wrapperMaximumNanoseconds / 1'000.0;

    const qint64 outputDropped = std::max<qint64>(
        0, m_outputDroppedFrames - m_upscalingPreviousOutputDroppedFrames);
    const qint64 mistimed = std::max<qint64>(
        0, m_mistimedFrames - m_upscalingPreviousMistimedFrames);
    const qint64 delayed = std::max<qint64>(
        0, m_delayedFrames - m_upscalingPreviousDelayedFrames);
    m_upscalingPreviousOutputDroppedFrames = m_outputDroppedFrames;
    m_upscalingPreviousMistimedFrames = m_mistimedFrames;
    m_upscalingPreviousDelayedFrames = m_delayedFrames;

    YanamiUpscaling::HealthWindow window;
    window.renderSamples = renderCount;
    window.renderP95Ms = m_upscalingLastRenderP95Ms;
    window.estimatedFps = m_estimatedVideoFpsAvailable
        ? m_estimatedVideoFps : 0.0;
    window.outputDroppedFrames = outputDropped;
    window.mistimedFrames = mistimed;
    window.delayedFrames = delayed;
    window.avSyncMs = m_avSyncAvailable ? m_avSyncSeconds * 1000.0 : 0.0;
    window.playing = m_playbackState == PlaybackState::Playing;
    window.buffering = m_buffering || m_watchdogBuffering;
    window.paused = m_paused || m_pauseRequested;

    const auto action = m_upscalingProtection.evaluate(window);
    const double healthPollCpuUs = healthPollTimer.nsecsElapsed() / 1'000.0;
    if (m_performanceTraceEnabled) {
        YanamiPerformance::PerformanceTrace::mark(
            QStringLiteral("playback_upscaling_health_window"),
            {
                {QStringLiteral("profileId"), m_effectiveUpscalingProfile},
                {QStringLiteral("renderSamples"),
                 QVariant::fromValue<qulonglong>(renderCount)},
                {QStringLiteral("renderP95Ms"), m_upscalingLastRenderP95Ms},
                {QStringLiteral("renderAverageMs"),
                 m_upscalingLastRenderAverageMs},
                {QStringLiteral("renderMaximumMs"),
                 m_upscalingLastRenderMaximumMs},
                {QStringLiteral("wrapperP95Us"), wrapperP95Us},
                {QStringLiteral("wrapperAverageUs"), wrapperAverageUs},
                {QStringLiteral("wrapperMaximumUs"), wrapperMaximumUs},
                {QStringLiteral("healthPollCpuUs"), healthPollCpuUs},
                {QStringLiteral("estimatedVideoFps"), window.estimatedFps},
                {QStringLiteral("outputDroppedFrames"), outputDropped},
                {QStringLiteral("mistimedFrames"), mistimed},
                {QStringLiteral("delayedFrames"), delayed},
                {QStringLiteral("avSyncMs"), window.avSyncMs},
                {QStringLiteral("playing"), window.playing},
                {QStringLiteral("buffering"), window.buffering},
                {QStringLiteral("paused"), window.paused},
                {QStringLiteral("frameBudgetMs"),
                 m_upscalingProtection.lastFrameBudgetMs()},
                {QStringLiteral("guardBudgetMs"),
                 m_upscalingProtection.lastGuardBudgetMs()},
                {QStringLiteral("overloaded"),
                 m_upscalingProtection.lastWindowOverloaded()},
            });
    }
    if (action == YanamiUpscaling::PerformanceProtection::Action::None)
        return;

    const QString previousProfile = m_effectiveUpscalingProfile;
    const double overloadedRenderP95Ms = m_upscalingLastRenderP95Ms;
    if (action == YanamiUpscaling::PerformanceProtection::Action::Downgrade
        && !m_upscalingFallbacks.isEmpty()) {
        QVariantList remainingFallbacks = m_upscalingFallbacks;
        QVariantMap next = remainingFallbacks.takeFirst().toMap();
        next.insert(QStringLiteral("fallbacks"), remainingFallbacks);
        beginUpscalingRuntimeConfig(
            next,
            previousProfile,
            overloadedRenderP95Ms,
            false);
        return;
    }

    beginUpscalingRuntimeConfig(
        {
            {QStringLiteral("schema"), 1},
            {QStringLiteral("enabled"), false},
        },
        previousProfile,
        overloadedRenderP95Ms,
        true);
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
