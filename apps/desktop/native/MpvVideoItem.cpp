#include "MpvVideoItem.hpp"

#include <QMetaObject>
#include <QElapsedTimer>
#include <QEvent>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QDebug>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include <mpv/render.h>
#include <mpv/render_gl.h>

namespace {

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

QByteArray diagnosticOption(const char *environmentName, const QByteArray &fallback)
{
    const QByteArray value = qgetenv(environmentName).trimmed();
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
    logProperty("video-params/primaries");
    logProperty("video-params/gamma");
    logProperty("video-params/colormatrix");
    logProperty("video-out-params/primaries");
    logProperty("video-out-params/gamma");
}

QString sanitizedMpvLog(const char *text)
{
    QString message = QString::fromUtf8(text).trimmed();
    static const QRegularExpression secret(
        QStringLiteral("(?i)(api_key|x-emby-token|access_token|token)=([^&\\s]+)"));
    message.replace(secret, QStringLiteral("\\1=<redacted>"));
    return message;
}

} // namespace

class MpvRenderer final : public QQuickFramebufferObject::Renderer
{
public:
    explicit MpvRenderer(MpvVideoItem *item)
        : m_item(item)
    {
    }

    ~MpvRenderer() override
    {
        if (m_context)
            mpv_render_context_free(m_context);
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override
    {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        format.setTextureTarget(GL_TEXTURE_2D);
        return new QOpenGLFramebufferObject(size, format);
    }

    void synchronize(QQuickFramebufferObject *item) override
    {
        m_item = static_cast<MpvVideoItem *>(item);
    }

    void render() override
    {
        if (!m_item || !m_item->m_mpv)
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
            if (mpv_render_context_create(&m_context, m_item->m_mpv, parameters) < 0) {
                QMetaObject::invokeMethod(m_item, [item = QPointer(m_item)] {
                    if (item)
                        emit item->playbackError(
                            MpvVideoItem::tr("Unable to initialize the libmpv OpenGL renderer."));
                });
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
        ++m_item->m_renderCount;
        const auto elapsed = static_cast<quint64>(renderTimer.nsecsElapsed());
        m_item->m_renderTotalNanoseconds += elapsed;
        auto maximum = m_item->m_renderMaximumNanoseconds.load();
        while (elapsed > maximum
               && !m_item->m_renderMaximumNanoseconds.compare_exchange_weak(maximum, elapsed)) {
        }
        QQuickOpenGLUtils::resetOpenGLState();

    }

private:
    static void requestUpdate(void *context)
    {
        auto *renderer = static_cast<MpvRenderer *>(context);
        if (!renderer || !renderer->m_item)
            return;
        ++renderer->m_item->m_renderCallbackCount;
        QMetaObject::invokeMethod(renderer->m_item, [item = QPointer(renderer->m_item)] {
            if (item)
                item->update();
        });
    }

    QPointer<MpvVideoItem> m_item;
    mpv_render_context *m_context = nullptr;
};

MpvVideoItem::MpvVideoItem(QQuickItem *parent)
    : QQuickFramebufferObject(parent)
    , m_mpv(mpv_create())
{
    if (!m_mpv)
        throw std::runtime_error("mpv_create failed");

    const QByteArray hardwareDecoding = diagnosticOption("YANAMI_DEV_MPV_HWDEC", "auto");
    const QByteArray videoSync = diagnosticOption("YANAMI_DEV_MPV_VIDEO_SYNC", "audio");
    const std::pair<const char *, QByteArray> options[] = {
        {"vo", "libmpv"},
        {"hwdec", hardwareDecoding},
        {"video-sync", videoSync},
        // Qt owns presentation and display synchronization. Asking libmpv to
        // wait inside mpv_render_context_render() stalls the shared Qt Quick
        // render thread and caps UI animations at the video's frame rate.
        {"video-timing-offset", "0"},
        {"terminal", "no"},
        {"keep-open", "yes"},
        {"sub-auto", "no"},
        {"sub-ass-override", "scale"},
        {"secondary-sub-ass-override", "no"},
        {"audio-client-name", "Yanami"},
    };
    for (const auto &[name, value] : options)
        mpv_set_option_string(m_mpv, name, value.constData());

    qInfo().noquote() << "mpv requested hwdec=" << hardwareDecoding
                      << "video-sync=" << videoSync;

    if (mpv_initialize(m_mpv) < 0)
        throw std::runtime_error("mpv_initialize failed");

    mpv_observe_property(m_mpv, 1, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 2, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 3, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 4, "track-list/count", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, 5, "demuxer-cache-time", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 6, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, 7, "volume", MPV_FORMAT_DOUBLE);
    mpv_set_wakeup_callback(m_mpv, &MpvVideoItem::wakeup, this);

    if (qEnvironmentVariableIsSet("YANAMI_DEV_RENDER_DIAGNOSTICS")) {
        mpv_request_log_messages(m_mpv, "warn");
        auto *diagnosticTimer = new QTimer(this);
        diagnosticTimer->setInterval(1000);
        connect(diagnosticTimer, &QTimer::timeout, this, [this] {
            const quint64 renderCount = m_renderCount.exchange(0);
            const quint64 totalNanoseconds = m_renderTotalNanoseconds.exchange(0);
            const quint64 maximumNanoseconds = m_renderMaximumNanoseconds.exchange(0);
            qInfo() << "render-load mpvCallbacks=" << m_renderCallbackCount.exchange(0)
                    << "mpvRenders=" << renderCount
                    << "averageRenderMs=" << (renderCount > 0
                               ? totalNanoseconds / 1'000'000.0 / renderCount
                               : 0.0)
                    << "maximumRenderMs=" << maximumNanoseconds / 1'000'000.0;
        });
        diagnosticTimer->start();
    }
}

MpvVideoItem::~MpvVideoItem()
{
    if (m_mpv) {
        mpv_set_wakeup_callback(m_mpv, nullptr, nullptr);
        mpv_terminate_destroy(m_mpv);
    }
}

QQuickFramebufferObject::Renderer *MpvVideoItem::createRenderer() const
{
    return new MpvRenderer(const_cast<MpvVideoItem *>(this));
}

void MpvVideoItem::open(const QUrl &url, const QVariantMap &headers)
{
    m_buffering = false;
    m_fileLoaded = false;
    m_position = 0.0;
    m_duration = 0.0;
    m_bufferedPosition = 0.0;
    emit positionChanged();
    emit durationChanged();
    emit bufferedPositionChanged();
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    m_selectedAudioTrack = -1;
    m_selectedSubtitleTrack = -1;
    emit tracksChanged();
    setHeaders(headers);
    setPlaybackState(QStringLiteral("loading"));
    command({"loadfile", url.toString(QUrl::FullyEncoded).toUtf8(), "replace"});
}

void MpvVideoItem::stop()
{
    command({"stop"});
    m_buffering = false;
    m_fileLoaded = false;
    m_bufferedPosition = 0.0;
    emit bufferedPositionChanged();
    setPlaybackState(QStringLiteral("idle"));
}

void MpvVideoItem::seek(double seconds)
{
    command({"seek", QByteArray::number(std::max(0.0, seconds), 'f', 3), "absolute+exact"});
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

void MpvVideoItem::setDanmakuFile(const QUrl &url)
{
    m_pendingDanmaku = url.isLocalFile() ? url.toLocalFile() : url.toString(QUrl::FullyEncoded);
    command({"sub-add", m_pendingDanmaku.toUtf8(), "auto", "Danmaku"});
    QTimer::singleShot(100, this, &MpvVideoItem::selectDanmakuTrack);
}

void MpvVideoItem::setDanmakuVisible(bool visible)
{
    int flag = visible ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "secondary-sub-visibility", MPV_FORMAT_FLAG, &flag);
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
    int flag = paused ? 1 : 0;
    mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &flag);
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
            setPlaybackState(m_buffering ? QStringLiteral("loading")
                                         : (m_paused ? QStringLiteral("paused") : QStringLiteral("playing")));
            refreshTracks();
            logPlaybackDiagnostics(m_mpv);
            emit fileLoaded();
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            m_fileLoaded = false;
            m_buffering = false;
            setPlaybackState(QStringLiteral("ended"));
            auto *endFile = static_cast<mpv_event_end_file *>(event->data);
            if (endFile && endFile->error < 0) {
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
                    if (m_fileLoaded && !m_buffering)
                        setPlaybackState(value ? QStringLiteral("paused") : QStringLiteral("playing"));
                }
            } else if (name == "paused-for-cache" && property->format == MPV_FORMAT_FLAG) {
                m_buffering = *static_cast<int *>(property->data) != 0;
                if (m_fileLoaded) {
                    setPlaybackState(m_buffering
                                         ? QStringLiteral("loading")
                                         : (m_paused ? QStringLiteral("paused") : QStringLiteral("playing")));
                }
            } else if (name == "time-pos" && property->format == MPV_FORMAT_DOUBLE) {
                m_position = *static_cast<double *>(property->data);
                emit positionChanged();
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
            }
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            auto *message = static_cast<mpv_event_log_message *>(event->data);
            if (message) {
                qWarning().noquote() << "mpv[" << message->prefix << ']'
                                     << sanitizedMpvLog(message->text);
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
        if (type == QStringLiteral("sub") && title == QStringLiteral("Danmaku"))
            continue;

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

        const QVariantMap track{
            {QStringLiteral("id"), id},
            {QStringLiteral("label"), labelParts.join(QStringLiteral(" · "))},
            {QStringLiteral("selected"), selected != 0},
        };
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

void MpvVideoItem::selectDanmakuTrack()
{
    if (m_pendingDanmaku.isEmpty())
        return;
    int64_t count = 0;
    if (mpv_get_property(m_mpv, "track-list/count", MPV_FORMAT_INT64, &count) < 0)
        return;
    for (int64_t index = 0; index < count; ++index) {
        char *type = nullptr;
        char *path = nullptr;
        int64_t id = 0;
        const QByteArray prefix = "track-list/" + QByteArray::number(index) + "/";
        const QByteArray typeProperty = prefix + "type";
        const QByteArray pathProperty = prefix + "external-filename";
        const QByteArray idProperty = prefix + "id";
        mpv_get_property(m_mpv, typeProperty.constData(), MPV_FORMAT_STRING, &type);
        mpv_get_property(m_mpv, pathProperty.constData(), MPV_FORMAT_STRING, &path);
        mpv_get_property(m_mpv, idProperty.constData(), MPV_FORMAT_INT64, &id);
        const bool matches = type && path && QByteArray(type) == "sub"
            && QString::fromUtf8(path) == m_pendingDanmaku;
        mpv_free(type);
        mpv_free(path);
        if (matches) {
            mpv_set_property_async(m_mpv, 0, "secondary-sid", MPV_FORMAT_INT64, &id);
            break;
        }
    }
}

void MpvVideoItem::setPlaybackState(const QString &state)
{
    if (m_playbackState == state)
        return;
    m_playbackState = state;
    emit playbackStateChanged();
}

void MpvVideoItem::command(const QList<QByteArray> &arguments)
{
    std::vector<const char *> pointers;
    pointers.reserve(arguments.size() + 1);
    for (const auto &argument : arguments)
        pointers.push_back(argument.constData());
    pointers.push_back(nullptr);
    if (mpv_command_async(m_mpv, 0, pointers.data()) < 0)
        emit playbackError(tr("libmpv rejected a playback command."));
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
