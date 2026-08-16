#include "AsyncImageProvider.hpp"
#include "AsyncImageKey.hpp"

#include <QDir>
#include <QFileInfo>
#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QQuickTextureFactory>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrentRun>

#include <atomic>
#include <cmath>
#include <memory>

namespace {
constexpr int kMaximumDecodeDimension = 8192;
constexpr qint64 kMaximumDecodePixels = 32LL * 1024 * 1024;
constexpr int kFilePollIntervalMs = 50;
constexpr int kFilePollAttempts = 600;

struct ImageLoadResult
{
    QImage image;
    QString error;
};

std::atomic_bool &shutdownRequested()
{
    static std::atomic_bool requested = false;
    return requested;
}

QSize boundedDecodeSize(const QSize &sourceSize, const QSize &requestedSize)
{
    if (!sourceSize.isValid())
        return {};

    int widthLimit = kMaximumDecodeDimension;
    int heightLimit = kMaximumDecodeDimension;
    if (requestedSize.width() > 0)
        widthLimit = qMin(widthLimit, requestedSize.width());
    if (requestedSize.height() > 0)
        heightLimit = qMin(heightLimit, requestedSize.height());

    // Never ask an image plugin to upscale while decoding. It adds memory and
    // CPU cost without adding information and can make a hostile sourceSize
    // request allocate an unbounded image.
    widthLimit = qBound(1, widthLimit, sourceSize.width());
    heightLimit = qBound(1, heightLimit, sourceSize.height());
    QSize target = sourceSize.scaled(
        QSize(widthLimit, heightLimit), Qt::KeepAspectRatio);

    const qint64 pixels = qint64(target.width()) * target.height();
    if (pixels > kMaximumDecodePixels) {
        const double scale = std::sqrt(
            double(kMaximumDecodePixels) / double(pixels));
        target = QSize(
            qMax(1, int(std::floor(target.width() * scale))),
            qMax(1, int(std::floor(target.height() * scale))));
    }

    return target == sourceSize ? QSize{} : target;
}

enum class CacheFileState {
    Missing,
    Ready,
    Unsafe,
};

struct CacheFileProbe
{
    CacheFileState state = CacheFileState::Missing;
    QString canonicalPath;
};

CacheFileProbe probeCacheFile(const QString &candidate, const QString &cacheRoot)
{
    const QFileInfo info(candidate);
    if (!info.exists())
        return {};
    if (!info.isFile())
        return {CacheFileState::Unsafe, {}};

    const QString canonicalPath = QDir::fromNativeSeparators(
        QDir::cleanPath(info.canonicalFilePath()));
    if (!YanamiAsyncImageKey::isStrictDescendant(canonicalPath, cacheRoot))
        return {CacheFileState::Unsafe, {}};
    return {CacheFileState::Ready, canonicalPath};
}

ImageLoadResult loadImage(
    const QString &candidate,
    const QString &cacheRoot,
    const QSize &requestedSize,
    const std::shared_ptr<std::atomic_int> &consumers)
{
    ImageLoadResult result;
    if (candidate.isEmpty()) {
        result.error = QStringLiteral("Invalid Yanami image cache key");
        return result;
    }

    QString readablePath;
    for (int attempt = 0; attempt < kFilePollAttempts; ++attempt) {
        if (shutdownRequested().load(std::memory_order_acquire)
            || consumers->load(std::memory_order_acquire) == 0) {
            return result;
        }

        const CacheFileProbe probe = probeCacheFile(candidate, cacheRoot);
        if (probe.state == CacheFileState::Ready) {
            readablePath = probe.canonicalPath;
            break;
        }
        if (probe.state == CacheFileState::Unsafe) {
            result.error = QStringLiteral("Image cache entry is not a regular cache file");
            return result;
        }
        if (attempt + 1 < kFilePollAttempts)
            QThread::msleep(kFilePollIntervalMs);
    }

    if (readablePath.isEmpty()) {
        result.error = QStringLiteral("Image download did not complete");
        return result;
    }
    if (shutdownRequested().load(std::memory_order_acquire)
        || consumers->load(std::memory_order_acquire) == 0) {
        return result;
    }

    QImageReader reader(readablePath);
    reader.setAutoTransform(true);
    const QSize decodeSize = boundedDecodeSize(reader.size(), requestedSize);
    if (decodeSize.isValid())
        reader.setScaledSize(decodeSize);
    result.image = reader.read();
    if (result.image.isNull())
        result.error = reader.errorString();
    return result;
}

QThreadPool &imageDecodePool()
{
    // Function-static construction is synchronized, and destruction waits for
    // the outstanding jobs. This keeps jobs alive through QQmlEngine teardown
    // without leaving a provider-owned pool racing its destructor.
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount(), 16));
        pool.setExpiryTimeout(15'000);
        return true;
    }();
    Q_UNUSED(configured)
    return pool;
}

QThreadPool &imageWaitPool()
{
    // Missing files are downloads already scheduled by Rust. They may wait
    // for network I/O for up to 30 seconds and must never occupy the decoder
    // threads needed by cache hits that can paint immediately.
    static QThreadPool pool;
    static const bool configured = [] {
        pool.setMaxThreadCount(64);
        pool.setExpiryTimeout(15'000);
        return true;
    }();
    Q_UNUSED(configured)
    return pool;
}

struct SharedImageLoad
{
    std::shared_ptr<std::atomic_int> consumers;
    QFuture<ImageLoadResult> future;
};

class ImageLoadRegistry final
{
public:
    std::shared_ptr<SharedImageLoad> acquire(
        const QString &candidate,
        const QString &cacheRoot,
        const QSize &requestedSize)
    {
        const QString key = candidate + QChar::Null
            + QString::number(requestedSize.width()) + QLatin1Char('x')
            + QString::number(requestedSize.height());
        QMutexLocker locker(&m_mutex);
        if (const auto existing = m_loads.value(key).lock(); existing) {
            int consumerCount = existing->consumers->load(std::memory_order_acquire);
            while (consumerCount > 0) {
                if (existing->consumers->compare_exchange_weak(
                        consumerCount,
                        consumerCount + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    return existing;
                }
            }
        }

        if ((++m_requestCount % 64) == 0 || m_loads.size() > 1024) {
            for (auto iterator = m_loads.begin(); iterator != m_loads.end();) {
                if (iterator.value().expired())
                    iterator = m_loads.erase(iterator);
                else
                    ++iterator;
            }
        }

        auto load = std::make_shared<SharedImageLoad>();
        load->consumers = std::make_shared<std::atomic_int>(1);
        const CacheFileProbe initialProbe = probeCacheFile(candidate, cacheRoot);
        QThreadPool *pool = initialProbe.state == CacheFileState::Ready
            ? &imageDecodePool() : &imageWaitPool();
        load->future = QtConcurrent::run(
            pool,
            [candidate, cacheRoot, requestedSize, consumers = load->consumers] {
                return loadImage(candidate, cacheRoot, requestedSize, consumers);
            });
        m_loads.insert(key, load);
        return load;
    }

private:
    QMutex m_mutex;
    QHash<QString, std::weak_ptr<SharedImageLoad>> m_loads;
    quint64 m_requestCount = 0;
};

ImageLoadRegistry &imageLoadRegistry()
{
    // Construct the registry before the pool on the first request. Static
    // destruction then joins the pool before releasing the weak registry.
    static ImageLoadRegistry registry;
    return registry;
}

class AsyncImageResponse final : public QQuickImageResponse
{
    Q_OBJECT

public:
    explicit AsyncImageResponse(std::shared_ptr<SharedImageLoad> load)
        : m_load(std::move(load))
    {
        // Connect before setFuture(). Invalid keys and already cached images
        // can finish immediately; doing this in the opposite order can lose
        // the finished notification and strand the QML Image in Loading.
        connect(&m_watcher, &QFutureWatcher<ImageLoadResult>::finished,
                this, [this] {
            const ImageLoadResult result = m_watcher.result();
            if (!m_cancelled.load(std::memory_order_acquire)) {
                m_image = result.image;
                m_error = result.error;
            }
            releaseConsumer();
            // QQuickImageResponse requires this to be the response's final
            // action; the engine schedules the response for deletion here.
            emit finished();
        });
        m_watcher.setFuture(m_load->future);
    }

    ~AsyncImageResponse() override
    {
        releaseConsumer();
    }

    QQuickTextureFactory *textureFactory() const override
    {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    QString errorString() const override { return m_error; }

    void cancel() override
    {
        m_cancelled.store(true, std::memory_order_release);
        releaseConsumer();
        // A cancelled response still waits for the shared future and emits
        // finished(), as required by QQuickImageResponse.
    }

private:
    void releaseConsumer()
    {
        if (!m_released.exchange(true, std::memory_order_acq_rel))
            m_load->consumers->fetch_sub(1, std::memory_order_acq_rel);
    }

    std::shared_ptr<SharedImageLoad> m_load;
    QFutureWatcher<ImageLoadResult> m_watcher;
    std::atomic_bool m_cancelled = false;
    std::atomic_bool m_released = false;
    QImage m_image;
    QString m_error;
};
}

AsyncImageProvider::AsyncImageProvider(QString cacheRoot)
    : m_cacheRoot(YanamiAsyncImageKey::normalizedCacheRoot(std::move(cacheRoot)))
{
    shutdownRequested().store(false, std::memory_order_release);
}

AsyncImageProvider::~AsyncImageProvider()
{
    // The engine owns the provider and destroys it before QGuiApplication.
    // Stop cache polling and join any in-progress decoder while image plugins
    // and Qt global state are still alive. The static pool is empty by the time
    // its own process-shutdown destructor runs.
    shutdownRequested().store(true, std::memory_order_release);
    imageWaitPool().waitForDone();
    imageDecodePool().waitForDone();
}

QQuickImageResponse *AsyncImageProvider::requestImageResponse(
    const QString &id,
    const QSize &requestedSize)
{
    const QString candidate = YanamiAsyncImageKey::decode(id, m_cacheRoot);
    return new AsyncImageResponse(imageLoadRegistry().acquire(
        candidate, m_cacheRoot, requestedSize));
}

#include "AsyncImageProvider.moc"
