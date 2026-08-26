#include "UpscalingCapabilityProbe.hpp"

#include <QCoreApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QThread>
#include <QVariantList>

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <optional>
#include <utility>

namespace {

constexpr int minimumGlMajor = 4;
constexpr int minimumGlMinor = 3;
constexpr int minimumTextureSize = 4096;

QString graphicsApiName(QSGRendererInterface::GraphicsApi graphicsApi)
{
    switch (graphicsApi) {
    case QSGRendererInterface::Software:
        return QStringLiteral("Software");
    case QSGRendererInterface::OpenVG:
        return QStringLiteral("OpenVG");
    case QSGRendererInterface::OpenGL:
        return QStringLiteral("OpenGL");
    case QSGRendererInterface::Direct3D11:
        return QStringLiteral("Direct3D11");
    case QSGRendererInterface::Vulkan:
        return QStringLiteral("Vulkan");
    case QSGRendererInterface::Metal:
        return QStringLiteral("Metal");
    case QSGRendererInterface::Null:
        return QStringLiteral("Null");
    case QSGRendererInterface::Direct3D12:
        return QStringLiteral("Direct3D12");
    case QSGRendererInterface::Unknown:
    default:
        return QStringLiteral("Unknown");
    }
}

bool containsAny(const QString &value, std::initializer_list<QStringView> needles)
{
    for (const QStringView needle : needles) {
        if (value.contains(needle, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

bool isSoftwareRenderer(
    QSGRendererInterface::GraphicsApi graphicsApi,
    const QString &vendor,
    const QString &renderer)
{
    if (graphicsApi == QSGRendererInterface::Software)
        return true;

    const QString identity = vendor + QLatin1Char(' ') + renderer;
    return containsAny(identity, {
        QStringView(u"llvmpipe"),
        QStringView(u"softpipe"),
        QStringView(u"swiftshader"),
        QStringView(u"software rasterizer"),
        QStringView(u"gdi generic"),
        QStringView(u"microsoft basic render driver"),
        QStringView(u"mesa offscreen"),
    });
}

QVariantMap providerResult(
    const QString &id,
    bool supported,
    const QString &unavailableReason,
    const QString &requiredBackend)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("supported"), supported},
        {QStringLiteral("unavailableReason"), unavailableReason},
        {QStringLiteral("requiredBackend"), requiredBackend},
    };
}

QString glString(const GLubyte *value)
{
    return value
        ? QString::fromLatin1(reinterpret_cast<const char *>(value)).trimmed()
        : QString{};
}

struct RendererSnapshot
{
    QSGRendererInterface::GraphicsApi graphicsApi =
        QSGRendererInterface::Unknown;
    int glMajor = 0;
    int glMinor = 0;
    QString vendor;
    QString renderer;
    int maximumTextureSize = 0;
};

std::optional<RendererSnapshot> captureCurrentRenderer(QQuickWindow *window)
{
    if (!window || !window->rendererInterface())
        return std::nullopt;

    const auto graphicsApi = window->rendererInterface()->graphicsApi();
    if (graphicsApi != QSGRendererInterface::OpenGL)
        return RendererSnapshot {.graphicsApi = graphicsApi};

    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context)
        return std::nullopt;
    QOpenGLFunctions *functions = context->functions();
    if (!functions)
        return std::nullopt;

    int glMajor = context->format().majorVersion();
    int glMinor = context->format().minorVersion();
    if (glMajor >= 3) {
        GLint reportedMajor = 0;
        GLint reportedMinor = 0;
        functions->glGetIntegerv(GL_MAJOR_VERSION, &reportedMajor);
        functions->glGetIntegerv(GL_MINOR_VERSION, &reportedMinor);
        if (reportedMajor > 0) {
            glMajor = reportedMajor;
            glMinor = std::max(0, static_cast<int>(reportedMinor));
        }
    }

    GLint maximumTextureSize = 0;
    functions->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
    return RendererSnapshot {
        .graphicsApi = graphicsApi,
        .glMajor = glMajor,
        .glMinor = glMinor,
        .vendor = glString(functions->glGetString(GL_VENDOR)),
        .renderer = glString(functions->glGetString(GL_RENDERER)),
        .maximumTextureSize = std::max(
            0, static_cast<int>(maximumTextureSize)),
    };
}

} // namespace

struct UpscalingCapabilityProbe::ObservationFence
{
    std::atomic<quint64> epoch {1};
    std::atomic<quint64> capturedEpoch {0};
};

UpscalingCapabilityProbe::UpscalingCapabilityProbe(QObject *parent)
    : QObject(parent)
{
}

UpscalingCapabilityProbe::~UpscalingCapabilityProbe()
{
    disconnectWindow();
}

void UpscalingCapabilityProbe::observe(QQuickWindow *window)
{
    if (QThread::currentThread() != thread()) {
        qWarning("UpscalingCapabilityProbe::observe must run on the probe's GUI thread");
        return;
    }

    disconnectWindow();
    m_window = window;
    m_fence = std::make_shared<ObservationFence>();
    publish({}, false);
    if (!window)
        return;

    const std::shared_ptr<ObservationFence> fence = m_fence;
    const QPointer<UpscalingCapabilityProbe> probe(this);
    const auto capture = [window, fence, probe] {
        const quint64 epoch = fence->epoch.load(std::memory_order_acquire);
        if (fence->capturedEpoch.load(std::memory_order_acquire) == epoch)
            return;

        const std::optional<RendererSnapshot> snapshot =
            captureCurrentRenderer(window);
        if (!snapshot.has_value())
            return;

        // A retired render thread can finish after a replacement thread has
        // sampled a newer epoch. Never let that stale completion move the
        // one-shot fence backwards.
        quint64 expected = fence->capturedEpoch.load(std::memory_order_relaxed);
        while (expected < epoch
               && !fence->capturedEpoch.compare_exchange_weak(
                   expected,
                   epoch,
                   std::memory_order_acq_rel,
                   std::memory_order_relaxed)) {
        }
        if (expected >= epoch || !probe)
            return;

        QMetaObject::invokeMethod(
            probe.data(),
            [probe, fence, epoch, snapshot = *snapshot] {
                if (!probe || probe->m_fence != fence
                    || fence->epoch.load(std::memory_order_acquire) != epoch) {
                    return;
                }
                probe->publish(
                    UpscalingCapabilityProbe::evaluate(
                        snapshot.graphicsApi,
                        snapshot.glMajor,
                        snapshot.glMinor,
                        snapshot.vendor,
                        snapshot.renderer,
                        snapshot.maximumTextureSize),
                    true);
            },
            Qt::QueuedConnection);
    };

    m_connections.append(connect(
        window,
        &QQuickWindow::sceneGraphInitialized,
        window,
        capture,
        Qt::DirectConnection));
    m_connections.append(connect(
        window,
        &QQuickWindow::beforeRendering,
        window,
        capture,
        Qt::DirectConnection));
    m_connections.append(connect(
        window,
        &QQuickWindow::sceneGraphInvalidated,
        window,
        [fence, probe] {
            const quint64 epoch = fence->epoch.fetch_add(
                1, std::memory_order_acq_rel) + 1;
            if (!probe)
                return;
            QMetaObject::invokeMethod(
                probe.data(),
                [probe, fence, epoch] {
                    if (!probe || probe->m_fence != fence
                        || fence->epoch.load(std::memory_order_acquire) != epoch
                        || fence->capturedEpoch.load(std::memory_order_acquire)
                            == epoch) {
                        return;
                    }
                    probe->publish({}, false);
                },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection));
    m_connections.append(connect(
        window,
        &QObject::destroyed,
        this,
        [this, fence] {
            if (m_fence != fence)
                return;
            fence->epoch.fetch_add(1, std::memory_order_acq_rel);
            m_window.clear();
            publish({}, false);
        }));

    // Covers observe() calls made after scene-graph initialization.
    window->update();
}

QVariantMap UpscalingCapabilityProbe::evaluate(
    QSGRendererInterface::GraphicsApi graphicsApi,
    int glMajor,
    int glMinor,
    const QString &vendor,
    const QString &renderer,
    int maximumTextureSize)
{
    glMajor = std::max(0, glMajor);
    glMinor = std::max(0, glMinor);
    maximumTextureSize = std::max(0, maximumTextureSize);

    const bool software = isSoftwareRenderer(graphicsApi, vendor, renderer);
    QString unavailableReason;
    if (software) {
        unavailableReason = QCoreApplication::translate(
            "UpscalingCapabilityProbe",
            "Software rendering is active; a hardware OpenGL renderer is required.");
    } else if (graphicsApi != QSGRendererInterface::OpenGL) {
        unavailableReason = QCoreApplication::translate(
            "UpscalingCapabilityProbe",
            "Yanami's current libmpv renderer requires the OpenGL backend.");
    } else if (glMajor < minimumGlMajor
               || (glMajor == minimumGlMajor && glMinor < minimumGlMinor)) {
        unavailableReason = QCoreApplication::translate(
            "UpscalingCapabilityProbe",
            "OpenGL 4.3 or newer is required.");
    } else if (maximumTextureSize < minimumTextureSize) {
        unavailableReason = QCoreApplication::translate(
            "UpscalingCapabilityProbe",
            "A maximum OpenGL texture size of at least 4096 is required.");
    }

    const bool anime4kSupported = unavailableReason.isEmpty();
    QVariantList providers;
    providers.push_back(providerResult(
        QStringLiteral("anime4k"),
        anime4kSupported,
        anime4kSupported ? QString{} : unavailableReason,
        QStringLiteral("OpenGL 4.3 + libmpv GLSL")));

    return {
        {QStringLiteral("graphicsApi"), static_cast<int>(graphicsApi)},
        {QStringLiteral("graphicsApiName"), graphicsApiName(graphicsApi)},
        {QStringLiteral("glMajor"), glMajor},
        {QStringLiteral("glMinor"), glMinor},
        {QStringLiteral("vendor"), vendor.trimmed()},
        {QStringLiteral("renderer"), renderer.trimmed()},
        {QStringLiteral("maximumTextureSize"), maximumTextureSize},
        {QStringLiteral("softwareRenderer"), software},
        {QStringLiteral("providers"), providers},
    };
}

void UpscalingCapabilityProbe::disconnectWindow()
{
    for (const QMetaObject::Connection &connection : std::as_const(m_connections))
        QObject::disconnect(connection);
    m_connections.clear();
    if (m_fence)
        m_fence->epoch.fetch_add(1, std::memory_order_acq_rel);
    m_fence.reset();
    m_window.clear();
}

void UpscalingCapabilityProbe::publish(const QVariantMap &result, bool ready)
{
    if (m_result == result && m_ready == ready)
        return;
    m_result = result;
    m_ready = ready;
    emit resultChanged();
}
