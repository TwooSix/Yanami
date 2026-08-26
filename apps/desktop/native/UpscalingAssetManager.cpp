#include "UpscalingAssetManager.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLockFile>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace {

Q_LOGGING_CATEGORY(upscalingAssetsLog, "yanami.upscaling.assets")

constexpr qint64 kNetworkReadBufferBytes = 256 * 1024;
constexpr qint64 kMaximumBytesPerGuiDrain = 256 * 1024;
constexpr qint64 kProgressPublishIntervalMs = 100;

qint64 artifactBytes(
    const QVector<YanamiUpscaling::ShaderArtifact> &artifacts)
{
    qint64 result = 0;
    for (const auto &artifact : artifacts)
        result += std::max<qint64>(0, artifact.sizeBytes);
    return result;
}

bool hashMatches(const QString &path, const QString &expectedSha256)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(256 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError)
            return false;
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex())
        .compare(expectedSha256, Qt::CaseInsensitive) == 0;
}

QString lockName(const QString &assetSetId)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        assetSetId.toUtf8(), QCryptographicHash::Sha256).toHex());
}

Qt::CaseSensitivity fileSystemPathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString normalizedAbsolutePath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(
        QFileInfo(path).absoluteFilePath()));
}

QString normalizedCanonicalPath(const QString &path)
{
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty()
        ? QString{}
        : QDir::cleanPath(QDir::fromNativeSeparators(canonical));
}

bool isSameOrDescendantPath(
    const QString &rootPath, const QString &candidatePath)
{
    const Qt::CaseSensitivity sensitivity = fileSystemPathCaseSensitivity();
    if (candidatePath.compare(rootPath, sensitivity) == 0)
        return true;
    QString prefix = rootPath;
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix.append(QLatin1Char('/'));
    return candidatePath.startsWith(prefix, sensitivity);
}

bool redirectsFileSystemPath(const QFileInfo &info)
{
    return info.isSymbolicLink() || info.isJunction();
}

QStringList relativePathComponents(
    const QString &rootPath,
    const QString &candidatePath,
    bool *valid)
{
    *valid = false;
    if (!isSameOrDescendantPath(rootPath, candidatePath))
        return {};
    const QString relative = QDir::cleanPath(QDir::fromNativeSeparators(
        QDir(rootPath).relativeFilePath(candidatePath)));
    if (relative == QStringLiteral(".")) {
        *valid = true;
        return {};
    }
    if (relative.isEmpty() || QDir::isAbsolutePath(relative)
        || relative == QStringLiteral("..")
        || relative.startsWith(QStringLiteral("../"))) {
        return {};
    }
    const QStringList components = relative.split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    if (components.contains(QStringLiteral("..")))
        return {};
    *valid = true;
    return components;
}

bool existingPathStaysWithinAssetRoot(
    const QString &assetRoot, const QString &candidatePath)
{
    const QString rootPath = normalizedAbsolutePath(assetRoot);
    const QString normalizedCandidate = normalizedAbsolutePath(candidatePath);
    bool relativeValid = false;
    const QStringList components = relativePathComponents(
        rootPath, normalizedCandidate, &relativeValid);
    if (!relativeValid)
        return false;

    const QFileInfo rootInfo(rootPath);
    if (redirectsFileSystemPath(rootInfo))
        return false;
    if (!rootInfo.exists())
        return true;
    if (!rootInfo.isDir())
        return false;
    const QString canonicalRoot = normalizedCanonicalPath(rootPath);
    if (canonicalRoot.isEmpty())
        return false;

    QString currentPath = rootPath;
    for (qsizetype index = 0; index < components.size(); ++index) {
        currentPath = QDir(currentPath).filePath(components.at(index));
        const QFileInfo info(currentPath);
        if (redirectsFileSystemPath(info))
            return false;
        if (!info.exists())
            break;
        if (index + 1 < components.size() && !info.isDir())
            return false;
        const QString canonical = normalizedCanonicalPath(currentPath);
        if (canonical.isEmpty()
            || !isSameOrDescendantPath(canonicalRoot, canonical)) {
            return false;
        }
    }
    return true;
}

bool ensureRealDirectoryWithinAssetRoot(
    const QString &assetRoot, const QString &directoryPath)
{
    const QString rootPath = normalizedAbsolutePath(assetRoot);
    const QString normalizedDirectory = normalizedAbsolutePath(directoryPath);
    bool relativeValid = false;
    const QStringList components = relativePathComponents(
        rootPath, normalizedDirectory, &relativeValid);
    if (!relativeValid)
        return false;

    QFileInfo rootInfo(rootPath);
    if (redirectsFileSystemPath(rootInfo))
        return false;
    if (!rootInfo.exists()) {
        if (!QDir().mkpath(rootPath))
            return false;
        rootInfo = QFileInfo(rootPath);
    }
    if (!rootInfo.exists() || !rootInfo.isDir()
        || redirectsFileSystemPath(rootInfo)) {
        return false;
    }
    const QString canonicalRoot = normalizedCanonicalPath(rootPath);
    if (canonicalRoot.isEmpty())
        return false;

    QString currentPath = rootPath;
    for (const QString &component : components) {
        const QString nextPath = QDir(currentPath).filePath(component);
        QFileInfo info(nextPath);
        if (redirectsFileSystemPath(info))
            return false;
        if (!info.exists()) {
            if (!QDir(currentPath).mkdir(component))
                return false;
            info = QFileInfo(nextPath);
        }
        if (!info.exists() || !info.isDir()
            || redirectsFileSystemPath(info)) {
            return false;
        }
        const QString canonical = normalizedCanonicalPath(nextPath);
        if (canonical.isEmpty()
            || !isSameOrDescendantPath(canonicalRoot, canonical)) {
            return false;
        }
        currentPath = nextPath;
    }
    return true;
}

} // namespace

UpscalingAssetManager::UpscalingAssetManager(
    const QString &assetRoot, QObject *parent)
    : QObject(parent)
    , m_assetRoot(QDir::cleanPath(QFileInfo(assetRoot).absoluteFilePath()))
    , m_network(new QNetworkAccessManager(this))
{
}

UpscalingAssetManager::UpscalingAssetManager(
    QNetworkAccessManager *network,
    const QString &assetRoot,
    QObject *parent)
    : QObject(parent)
    , m_assetRoot(QDir::cleanPath(QFileInfo(assetRoot).absoluteFilePath()))
    , m_network(network)
{
}

UpscalingAssetManager::~UpscalingAssetManager()
{
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
    }
    resetTransport();
}

QVariantMap UpscalingAssetManager::stateFor(const QString &assetSetId) const
{
    const auto iterator = m_states.constFind(assetSetId);
    if (iterator == m_states.cend()) {
        return {
            {QStringLiteral("assetSetId"), assetSetId},
            {QStringLiteral("phase"), QStringLiteral("missing")},
            {QStringLiteral("bytesReceived"), 0},
            {QStringLiteral("bytesTotal"), 0},
            {QStringLiteral("progress"), 0.0},
            {QStringLiteral("errorCode"), QString()},
            {QStringLiteral("errorMessage"), QString()},
        };
    }
    const AssetState &state = iterator.value();
    const double progress = state.bytesTotal > 0
        ? std::clamp(
              static_cast<double>(state.bytesReceived)
                  / static_cast<double>(state.bytesTotal),
              0.0,
              1.0)
        : 0.0;
    return {
        {QStringLiteral("assetSetId"), assetSetId},
        {QStringLiteral("phase"), state.phase},
        {QStringLiteral("version"), state.version},
        {QStringLiteral("bytesReceived"), state.bytesReceived},
        {QStringLiteral("bytesTotal"), state.bytesTotal},
        {QStringLiteral("progress"), progress},
        {QStringLiteral("errorCode"), state.errorCode},
        {QStringLiteral("errorMessage"), state.errorMessage},
    };
}

QString UpscalingAssetManager::absolutePath(
    const YanamiUpscaling::ShaderArtifact &artifact) const
{
    const QString relative = QDir::cleanPath(artifact.installRelativePath);
    if (relative.isEmpty() || QDir::isAbsolutePath(relative)
        || relative == QStringLiteral("..")
        || relative.startsWith(QStringLiteral("../"))
        || relative.contains(QStringLiteral("/../"))) {
        return {};
    }
    const QString path = QDir(m_assetRoot).absoluteFilePath(relative);
    return existingPathStaysWithinAssetRoot(m_assetRoot, path)
        ? path : QString{};
}

void UpscalingAssetManager::verify(
    const QString &assetSetId,
    const QVector<YanamiUpscaling::ShaderArtifact> &artifacts)
{
    beginVerification(
        assetSetId, artifacts, false, QStringLiteral("checking"));
}

void UpscalingAssetManager::download(
    const QString &assetSetId,
    const QVector<YanamiUpscaling::ShaderArtifact> &artifacts)
{
    if (assetSetId.isEmpty() || artifacts.isEmpty() || !m_network)
        return;
    const AssetState current = m_states.value(assetSetId);
    if (current.phase == QStringLiteral("downloading")
        || current.phase == QStringLiteral("queued")
        || current.phase == QStringLiteral("verifying")) {
        return;
    }
    beginVerification(
        assetSetId, artifacts, true, QStringLiteral("checking"));
}

void UpscalingAssetManager::cancel(const QString &assetSetId)
{
    auto iterator = m_states.find(assetSetId);
    if (iterator == m_states.end())
        return;
    ++iterator->generation;
    m_downloadQueue.removeAll(assetSetId);

    if (m_currentTask && m_currentTask->assetSetId == assetSetId) {
        if (m_reply) {
            disconnect(m_reply, nullptr, this, nullptr);
            m_reply->abort();
            m_reply->deleteLater();
            m_reply.clear();
        }
        resetTransport();
        m_currentTask.reset();
    }

    iterator->phase = QStringLiteral("missing");
    iterator->bytesReceived = 0;
    iterator->errorCode = QStringLiteral("cancelled");
    iterator->errorMessage = tr("The component download was cancelled.");
    publishState(assetSetId);
    startNextDownload();
}

void UpscalingAssetManager::beginVerification(
    const QString &assetSetId,
    const QVector<YanamiUpscaling::ShaderArtifact> &artifacts,
    bool downloadWhenMissing,
    const QString &phase)
{
    if (assetSetId.isEmpty() || artifacts.isEmpty())
        return;
    AssetState &state = m_states[assetSetId];
    const quint64 generation = ++state.generation;
    state.phase = phase;
    state.version = artifacts.constFirst().version;
    state.errorCode.clear();
    state.errorMessage.clear();
    state.bytesTotal = artifactBytes(artifacts);
    state.bytesReceived = phase == QStringLiteral("verifying")
        ? state.bytesTotal : 0;
    state.artifacts = artifacts;
    publishState(assetSetId);

    struct VerificationInput {
        YanamiUpscaling::ShaderArtifact artifact;
        QString path;
    };
    QVector<VerificationInput> inputs;
    inputs.reserve(artifacts.size());
    for (const auto &artifact : artifacts)
        inputs.append({artifact, absolutePath(artifact)});

    auto *watcher = new QFutureWatcher<VerificationResult>(this);
    connect(watcher, &QFutureWatcher<VerificationResult>::finished,
        this,
        [this, watcher, assetSetId, generation, downloadWhenMissing] {
            const VerificationResult result = watcher->result();
            watcher->deleteLater();
            auto iterator = m_states.find(assetSetId);
            if (iterator == m_states.end()
                || iterator->generation != generation) {
                return;
            }
            AssetState &state = iterator.value();
            if (result.missingOrInvalid.isEmpty()) {
                const QVector<YanamiUpscaling::ShaderArtifact>
                    verifiedArtifacts = state.artifacts;
                state.phase = QStringLiteral("ready");
                state.bytesReceived = state.bytesTotal;
                state.errorCode.clear();
                state.errorMessage.clear();
                publishState(assetSetId);
                reverifyStatesSharingArtifacts(
                    assetSetId, verifiedArtifacts);
                return;
            }
            const qint64 missingBytes = artifactBytes(result.missingOrInvalid);
            state.bytesReceived = std::max<qint64>(
                0, state.bytesTotal - missingBytes);
            if (!downloadWhenMissing) {
                state.phase = result.hadInvalidFile
                    ? QStringLiteral("failed") : QStringLiteral("missing");
                state.errorCode = result.hadInvalidFile
                    ? QStringLiteral("integrity") : QString();
                state.errorMessage = result.hadInvalidFile
                    ? tr("Installed upscaling components failed verification.")
                    : QString();
                publishState(assetSetId);
                return;
            }
            enqueueDownload(
                assetSetId, generation, result.missingOrInvalid);
        });
    watcher->setFuture(QtConcurrent::run(
        [inputs = std::move(inputs), assetRoot = m_assetRoot] {
            VerificationResult result;
            for (const VerificationInput &input : inputs) {
                if (input.path.isEmpty()
                    || !existingPathStaysWithinAssetRoot(
                        assetRoot, input.path)) {
                    result.hadInvalidFile = true;
                    result.missingOrInvalid.append(input.artifact);
                    continue;
                }
                const QFileInfo info(input.path);
                if (!info.exists() || !info.isFile()) {
                    result.missingOrInvalid.append(input.artifact);
                    continue;
                }
                if (info.size() != input.artifact.sizeBytes
                    || !hashMatches(input.path, input.artifact.sha256)) {
                    result.hadInvalidFile = true;
                    result.missingOrInvalid.append(input.artifact);
                }
            }
            return result;
        }));
}

void UpscalingAssetManager::reverifyStatesSharingArtifacts(
    const QString &readyAssetSetId,
    const QVector<YanamiUpscaling::ShaderArtifact> &readyArtifacts)
{
    QSet<QString> installedArtifactIds;
    for (const auto &artifact : readyArtifacts) {
        if (!artifact.id.isEmpty())
            installedArtifactIds.insert(artifact.id);
    }
    if (installedArtifactIds.isEmpty())
        return;

    struct PendingVerification {
        QString assetSetId;
        QVector<YanamiUpscaling::ShaderArtifact> artifacts;
    };
    QVector<PendingVerification> pending;
    for (auto iterator = m_states.cbegin(); iterator != m_states.cend();
         ++iterator) {
        if (iterator.key() == readyAssetSetId
            || (iterator->phase != QStringLiteral("missing")
                && iterator->phase != QStringLiteral("failed"))) {
            continue;
        }
        const bool sharesInstalledArtifact = std::any_of(
            iterator->artifacts.cbegin(),
            iterator->artifacts.cend(),
            [&installedArtifactIds](const auto &artifact) {
                return installedArtifactIds.contains(artifact.id);
            });
        if (sharesInstalledArtifact) {
            pending.append({iterator.key(), iterator->artifacts});
        }
    }

    // beginVerification publishes synchronously and mutates m_states, so work
    // from copies collected above rather than keeping hash iterators alive.
    for (const PendingVerification &verification : std::as_const(pending)) {
        beginVerification(
            verification.assetSetId,
            verification.artifacts,
            false,
            QStringLiteral("checking"));
    }
}

void UpscalingAssetManager::enqueueDownload(
    const QString &assetSetId,
    quint64 generation,
    const QVector<YanamiUpscaling::ShaderArtifact> &missingArtifacts)
{
    auto iterator = m_states.find(assetSetId);
    if (iterator == m_states.end() || iterator->generation != generation)
        return;
    iterator->phase = QStringLiteral("queued");
    iterator->errorCode.clear();
    iterator->errorMessage.clear();
    publishState(assetSetId);

    // Preserve the exact preflight result. A queued task must not be affected
    // by a later UI selection change, while the complete required list stays
    // available for the final verification pass.
    iterator->downloadArtifacts = missingArtifacts;
    if (!m_downloadQueue.contains(assetSetId))
        m_downloadQueue.append(assetSetId);
    startNextDownload();
}

void UpscalingAssetManager::startNextDownload()
{
    if (m_currentTask || m_downloadQueue.isEmpty())
        return;
    const QString assetSetId = m_downloadQueue.takeFirst();
    auto iterator = m_states.find(assetSetId);
    if (iterator == m_states.end()
        || iterator->phase != QStringLiteral("queued")) {
        QTimer::singleShot(0, this, &UpscalingAssetManager::startNextDownload);
        return;
    }

    const QString lockDirectory = QDir(m_assetRoot).filePath(
        QStringLiteral(".locks"));
    if (!ensureRealDirectoryWithinAssetRoot(m_assetRoot, lockDirectory)) {
        iterator->phase = QStringLiteral("failed");
        iterator->errorCode = QStringLiteral("storage");
        iterator->errorMessage = tr(
            "Yanami could not create a safe component lock directory.");
        publishState(assetSetId);
        QTimer::singleShot(0, this, &UpscalingAssetManager::startNextDownload);
        return;
    }
    m_lock = std::make_unique<QLockFile>(QDir(lockDirectory).filePath(
        QStringLiteral("%1.lock").arg(lockName(assetSetId))));
    m_lock->setStaleLockTime(30'000);
    if (!m_lock->tryLock(0)) {
        iterator->phase = QStringLiteral("failed");
        iterator->errorCode = QStringLiteral("busy");
        iterator->errorMessage = tr(
            "Another Yanami process is installing these components.");
        publishState(assetSetId);
        m_lock.reset();
        QTimer::singleShot(0, this, &UpscalingAssetManager::startNextDownload);
        return;
    }

    m_currentTask = std::make_unique<DownloadTask>();
    m_currentTask->assetSetId = assetSetId;
    m_currentTask->generation = iterator->generation;
    m_currentTask->artifacts = iterator->downloadArtifacts;
    m_currentTask->cachedBytes = iterator->bytesReceived;
    iterator->phase = QStringLiteral("downloading");
    publishState(assetSetId);
    m_progressPublishTimer.restart();
    startCurrentArtifact();
}

void UpscalingAssetManager::startCurrentArtifact()
{
    if (!m_currentTask)
        return;
    if (m_currentTask->artifactIndex >= m_currentTask->artifacts.size()) {
        finishDownloadTask();
        return;
    }
    if (!m_network) {
        failCurrentTask(
            QStringLiteral("network-unavailable"),
            tr("The component download service is unavailable."));
        return;
    }

    const auto &artifact = m_currentTask->artifacts.at(
        m_currentTask->artifactIndex);
    const QUrl url(artifact.downloadUrl);
    const QString destination = absolutePath(artifact);
    if (!isAllowedDownloadUrl(url) || destination.isEmpty()) {
        failCurrentTask(
            QStringLiteral("catalog-invalid"),
            tr("The upscaling component catalog is invalid."));
        return;
    }
    if (!ensureRealDirectoryWithinAssetRoot(
            m_assetRoot, QFileInfo(destination).absolutePath())
        || absolutePath(artifact) != destination) {
        failCurrentTask(
            QStringLiteral("storage"),
            tr("Yanami could not create a safe component directory."));
        return;
    }

    m_saveFile = std::make_unique<QSaveFile>(destination);
    m_saveFile->setDirectWriteFallback(false);
    if (!m_saveFile->open(QIODevice::WriteOnly)) {
        failCurrentTask(
            QStringLiteral("storage"),
            tr("Yanami could not write the downloaded component."));
        return;
    }
    m_hash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    m_currentTask->currentFileBytes = 0;

    QNetworkRequest request(url);
    request.setTransferTimeout(30'000);
    request.setMaximumRedirectsAllowed(4);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("Accept", "application/octet-stream");
    request.setRawHeader("User-Agent", "Yanami-upscaling/1");
    m_reply = m_network->get(request);
    m_reply->setReadBufferSize(kNetworkReadBufferBytes);
    connect(m_reply, &QNetworkReply::readyRead,
        this, [this] { drainCurrentReply(false); });
    connect(m_reply, &QNetworkReply::finished,
        this, &UpscalingAssetManager::finishCurrentReply);
}

void UpscalingAssetManager::drainCurrentReply(bool drainAll)
{
    if (!m_reply || !m_currentTask || !m_saveFile || !m_hash)
        return;
    const auto &artifact = m_currentTask->artifacts.at(
        m_currentTask->artifactIndex);
    qint64 processed = 0;
    while (m_reply->bytesAvailable() > 0
           && (drainAll || processed < kMaximumBytesPerGuiDrain)) {
        const qint64 requested = drainAll
            ? std::min<qint64>(m_reply->bytesAvailable(), 256 * 1024)
            : std::min<qint64>(
                  {m_reply->bytesAvailable(),
                   64 * 1024,
                   kMaximumBytesPerGuiDrain - processed});
        const QByteArray chunk = m_reply->read(requested);
        if (chunk.isEmpty())
            break;
        if (m_currentTask->currentFileBytes + chunk.size()
                > artifact.sizeBytes
            || m_saveFile->write(chunk) != chunk.size()) {
            failCurrentTask(
                QStringLiteral("size-or-storage"),
                tr("The downloaded component exceeded its expected size or could not be saved."));
            return;
        }
        m_hash->addData(chunk);
        m_currentTask->currentFileBytes += chunk.size();
        processed += chunk.size();
    }
    publishProgress(false);
    if (!drainAll && m_reply && m_reply->bytesAvailable() > 0) {
        QTimer::singleShot(0, this,
            [this] { drainCurrentReply(false); });
    }
}

void UpscalingAssetManager::finishCurrentReply()
{
    if (!m_reply || !m_currentTask)
        return;
    QPointer<QNetworkReply> reply = m_reply;
    drainCurrentReply(true);
    if (!m_currentTask || !reply)
        return;
    m_reply.clear();

    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QUrl finalUrl = reply->url();
    reply->deleteLater();
    if (networkError != QNetworkReply::NoError || httpStatus != 200
        || !isAllowedDownloadUrl(finalUrl)) {
        failCurrentTask(
            QStringLiteral("network"),
            tr("The upscaling component could not be downloaded."));
        return;
    }
    finishCurrentArtifact();
}

void UpscalingAssetManager::finishCurrentArtifact()
{
    if (!m_currentTask || !m_saveFile || !m_hash)
        return;
    const auto &artifact = m_currentTask->artifacts.at(
        m_currentTask->artifactIndex);
    const QString digest = QString::fromLatin1(m_hash->result().toHex());
    if (m_currentTask->currentFileBytes != artifact.sizeBytes
        || digest.compare(artifact.sha256, Qt::CaseInsensitive) != 0) {
        failCurrentTask(
            QStringLiteral("integrity"),
            tr("The downloaded component failed verification and was not installed."));
        return;
    }
    if (absolutePath(artifact).isEmpty()) {
        failCurrentTask(
            QStringLiteral("storage"),
            tr("The component directory changed before installation completed."));
        return;
    }
    if (!m_saveFile->commit()) {
        failCurrentTask(
            QStringLiteral("storage"),
            tr("Yanami could not install the verified component."));
        return;
    }

    m_currentTask->completedDownloadBytes += artifact.sizeBytes;
    m_currentTask->currentFileBytes = 0;
    ++m_currentTask->artifactIndex;
    m_saveFile.reset();
    m_hash.reset();
    publishProgress(true);
    startCurrentArtifact();
}

void UpscalingAssetManager::finishDownloadTask()
{
    if (!m_currentTask)
        return;
    const QString assetSetId = m_currentTask->assetSetId;
    auto iterator = m_states.find(assetSetId);
    if (iterator == m_states.end()
        || iterator->generation != m_currentTask->generation) {
        resetTransport();
        m_currentTask.reset();
        startNextDownload();
        return;
    }
    const QVector<YanamiUpscaling::ShaderArtifact> verificationArtifacts =
        iterator->artifacts;

    iterator->phase = QStringLiteral("verifying");
    iterator->bytesReceived = iterator->bytesTotal;
    publishState(assetSetId);
    resetTransport();
    m_currentTask.reset();
    beginVerification(
        assetSetId,
        verificationArtifacts,
        false,
        QStringLiteral("verifying"));
    startNextDownload();
}

void UpscalingAssetManager::failCurrentTask(
    const QString &errorCode, const QString &message)
{
    if (!m_currentTask)
        return;
    const QString assetSetId = m_currentTask->assetSetId;
    auto iterator = m_states.find(assetSetId);
    if (iterator != m_states.end()
        && iterator->generation == m_currentTask->generation) {
        iterator->phase = QStringLiteral("failed");
        iterator->errorCode = errorCode;
        iterator->errorMessage = message;
        publishState(assetSetId);
    }
    qCWarning(upscalingAssetsLog).noquote()
        << "upscaling_asset_download_failed"
        << "assetSetId=" << assetSetId
        << "errorCode=" << errorCode;
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply.clear();
    }
    resetTransport();
    m_currentTask.reset();
    startNextDownload();
}

void UpscalingAssetManager::publishProgress(bool force)
{
    if (!m_currentTask)
        return;
    if (!force && m_progressPublishTimer.isValid()
        && m_progressPublishTimer.elapsed() < kProgressPublishIntervalMs) {
        return;
    }
    auto iterator = m_states.find(m_currentTask->assetSetId);
    if (iterator == m_states.end()
        || iterator->generation != m_currentTask->generation) {
        return;
    }
    iterator->bytesReceived = std::min(
        iterator->bytesTotal,
        m_currentTask->cachedBytes
            + m_currentTask->completedDownloadBytes
            + m_currentTask->currentFileBytes);
    m_progressPublishTimer.restart();
    publishState(m_currentTask->assetSetId);
}

void UpscalingAssetManager::resetTransport()
{
    if (m_saveFile)
        m_saveFile->cancelWriting();
    m_saveFile.reset();
    m_hash.reset();
    if (m_lock)
        m_lock->unlock();
    m_lock.reset();
}

bool UpscalingAssetManager::isAllowedDownloadUrl(const QUrl &url) const
{
    if (!url.isValid() || url.scheme() != QStringLiteral("https"))
        return false;
    const QString host = url.host().toLower();
    return host == QStringLiteral("github.com")
        || host == QStringLiteral("raw.githubusercontent.com")
        || host == QStringLiteral("release-assets.githubusercontent.com")
        || host.endsWith(QStringLiteral(".githubusercontent.com"));
}

void UpscalingAssetManager::publishState(const QString &assetSetId)
{
    emit stateChanged(assetSetId);
}
