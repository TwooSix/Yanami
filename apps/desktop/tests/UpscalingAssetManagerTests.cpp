#include "UpscalingAssetManager.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <cstring>

namespace {

class FakeReply final : public QNetworkReply
{
public:
    FakeReply(
        const QNetworkRequest &request,
        QByteArray payload,
        int delayMs,
        QObject *parent)
        : QNetworkReply(parent)
        , m_payload(std::move(payload))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(delayMs, this, [this] {
            if (isFinished())
                return;
            if (!m_payload.isEmpty())
                emit readyRead();
            setFinished(true);
            emit finished();
        });
    }

    void abort() override
    {
        if (isFinished())
            return;
        setError(OperationCanceledError, QStringLiteral("cancelled"));
        setFinished(true);
        emit finished();
    }

    qint64 bytesAvailable() const override
    {
        return m_payload.size() - m_offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 available = m_payload.size() - m_offset;
        if (available <= 0)
            return -1;
        const qint64 count = std::min(maxSize, available);
        std::memcpy(data, m_payload.constData() + m_offset,
            static_cast<std::size_t>(count));
        m_offset += count;
        return count;
    }

private:
    QByteArray m_payload;
    qint64 m_offset = 0;
};

class FakeNetworkAccessManager final : public QNetworkAccessManager
{
public:
    QByteArray payload;
    int delayMs = 0;
    int requestCount = 0;

protected:
    QNetworkReply *createRequest(
        Operation operation,
        const QNetworkRequest &request,
        QIODevice *outgoingData) override
    {
        Q_UNUSED(operation)
        Q_UNUSED(outgoingData)
        ++requestCount;
        return new FakeReply(request, payload, delayMs, this);
    }
};

YanamiUpscaling::ShaderArtifact artifactFor(
    const QByteArray &payload,
    const QString &assetSetId = QStringLiteral("anime4k-test"))
{
    return {
        .id = QStringLiteral("anime4k-test-shader"),
        .providerId = QStringLiteral("anime4k"),
        .version = QStringLiteral("v1-test"),
        .assetSetId = assetSetId,
        .fileName = QStringLiteral("test.glsl"),
        .installRelativePath = QStringLiteral("anime4k/v1-test/test.glsl"),
        .downloadUrl = QStringLiteral(
            "https://raw.githubusercontent.com/bloc97/Anime4K/test/test.glsl"),
        .sizeBytes = payload.size(),
        .sha256 = QString::fromLatin1(QCryptographicHash::hash(
            payload, QCryptographicHash::Sha256).toHex()),
        .licenseSpdx = QStringLiteral("MIT"),
    };
}

QString phase(UpscalingAssetManager &manager, const QString &assetSetId)
{
    return manager.stateFor(assetSetId)
        .value(QStringLiteral("phase")).toString();
}

bool createDirectoryRedirect(
    const QString &linkPath,
    const QString &targetPath,
    QString *errorMessage)
{
#ifdef Q_OS_WIN
    QProcess process;
    process.start(
        qEnvironmentVariable("ComSpec", "cmd.exe"),
        {
            QStringLiteral("/d"),
            QStringLiteral("/c"),
            QStringLiteral("mklink"),
            QStringLiteral("/J"),
            QDir::toNativeSeparators(linkPath),
            QDir::toNativeSeparators(targetPath),
        });
    if (!process.waitForStarted(3000) || !process.waitForFinished(3000)
        || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        *errorMessage = QString::fromLocal8Bit(process.readAllStandardError())
                            .trimmed();
        if (errorMessage->isEmpty()) {
            *errorMessage = QString::fromLocal8Bit(process.readAllStandardOutput())
                                .trimmed();
        }
        return false;
    }
    return QFileInfo(linkPath).isJunction();
#else
    if (QFile::link(targetPath, linkPath))
        return true;
    *errorMessage = QStringLiteral("QFile::link failed");
    return false;
#endif
}

bool removeDirectoryRedirect(const QString &linkPath)
{
#ifdef Q_OS_WIN
    return QDir().rmdir(linkPath);
#else
    return QFile::remove(linkPath);
#endif
}

class DirectoryRedirectGuard final
{
public:
    explicit DirectoryRedirectGuard(QString path)
        : m_path(std::move(path))
    {
    }

    ~DirectoryRedirectGuard()
    {
        if (QFileInfo::exists(m_path) || QFileInfo(m_path).isSymbolicLink()
            || QFileInfo(m_path).isJunction()) {
            removeDirectoryRedirect(m_path);
        }
    }

private:
    QString m_path;
};

} // namespace

class UpscalingAssetManagerTests final : public QObject
{
    Q_OBJECT

private slots:
    void downloadsVerifiesAndAtomicallyInstallsAnArtifact()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload = QByteArrayLiteral(
            "//!HOOK MAIN\n//!BIND HOOKED\nvec4 hook(){return HOOKED_tex(HOOKED_pos);}\n");
        const auto artifact = artifactFor(payload);
        FakeNetworkAccessManager network;
        network.payload = payload;
        UpscalingAssetManager manager(&network, directory.path());
        QSignalSpy changed(&manager, &UpscalingAssetManager::stateChanged);

        manager.verify(artifact.assetSetId, {artifact});
        QTRY_COMPARE(phase(manager, artifact.assetSetId),
            QStringLiteral("missing"));
        manager.download(artifact.assetSetId, {artifact});
        QTRY_COMPARE_WITH_TIMEOUT(phase(manager, artifact.assetSetId),
            QStringLiteral("ready"), 3000);

        QCOMPARE(network.requestCount, 1);
        QFile installed(manager.absolutePath(artifact));
        QVERIFY(installed.open(QIODevice::ReadOnly));
        QCOMPARE(installed.readAll(), payload);
        const QVariantMap state = manager.stateFor(artifact.assetSetId);
        QCOMPARE(state.value(QStringLiteral("bytesReceived")).toLongLong(),
            payload.size());
        QCOMPARE(state.value(QStringLiteral("bytesTotal")).toLongLong(),
            payload.size());
        QCOMPARE(state.value(QStringLiteral("progress")).toDouble(), 1.0);
        QVERIFY(changed.count() >= 4);
    }

    void successfulInstallReverifiesProfilesSharingArtifacts()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray payload = QByteArrayLiteral(
            "//!HOOK MAIN\n//!BIND HOOKED\nvec4 hook(){return HOOKED_tex(HOOKED_pos);}\n");
        auto shared = artifactFor(payload);
        shared.id = QStringLiteral("shared-shader");
        shared.assetSetId = QStringLiteral("shared-catalog");

        auto extra = artifactFor(payload);
        extra.id = QStringLiteral("quality-only-shader");
        extra.assetSetId = QStringLiteral("shared-catalog");
        extra.fileName = QStringLiteral("quality.glsl");
        extra.installRelativePath = QStringLiteral(
            "anime4k/v1-test/quality.glsl");

        FakeNetworkAccessManager network;
        network.payload = payload;
        UpscalingAssetManager manager(&network, directory.path());
        const QString balancedKey = QStringLiteral("profile-balanced");
        const QString qualityKey = QStringLiteral("profile-quality");

        manager.verify(balancedKey, {shared});
        manager.verify(qualityKey, {shared, extra});
        QTRY_COMPARE(phase(manager, balancedKey), QStringLiteral("missing"));
        QTRY_COMPARE(phase(manager, qualityKey), QStringLiteral("missing"));

        manager.download(qualityKey, {shared, extra});
        QTRY_COMPARE_WITH_TIMEOUT(
            phase(manager, qualityKey), QStringLiteral("ready"), 3000);
        // No explicit second verify/download is issued for balanced. The
        // quality install must invalidate its stale missing result because the
        // two profiles share the installed shader.
        QTRY_COMPARE_WITH_TIMEOUT(
            phase(manager, balancedKey), QStringLiteral("ready"), 3000);
        QCOMPARE(network.requestCount, 2);
    }

    void rejectsHashMismatchWithoutPublishingTheFile()
    {
        QTemporaryDir directory;
        const QByteArray payload = QByteArrayLiteral("shader payload");
        auto artifact = artifactFor(payload);
        artifact.sha256 = QString(64, QLatin1Char('0'));
        FakeNetworkAccessManager network;
        network.payload = payload;
        UpscalingAssetManager manager(&network, directory.path());

        manager.download(artifact.assetSetId, {artifact});
        QTRY_COMPARE_WITH_TIMEOUT(phase(manager, artifact.assetSetId),
            QStringLiteral("failed"), 3000);
        QCOMPARE(manager.stateFor(artifact.assetSetId)
                     .value(QStringLiteral("errorCode")).toString(),
            QStringLiteral("integrity"));
        QVERIFY(!QFileInfo::exists(manager.absolutePath(artifact)));
    }

    void rejectsPayloadLargerThanThePinnedManifestSize()
    {
        QTemporaryDir directory;
        const QByteArray payload = QByteArrayLiteral("expected");
        auto artifact = artifactFor(payload);
        FakeNetworkAccessManager network;
        network.payload = payload + QByteArrayLiteral("extra");
        UpscalingAssetManager manager(&network, directory.path());

        manager.download(artifact.assetSetId, {artifact});
        QTRY_COMPARE_WITH_TIMEOUT(phase(manager, artifact.assetSetId),
            QStringLiteral("failed"), 3000);
        QCOMPARE(manager.stateFor(artifact.assetSetId)
                     .value(QStringLiteral("errorCode")).toString(),
            QStringLiteral("size-or-storage"));
    }

    void verifiedCacheAvoidsASecondNetworkRequest()
    {
        QTemporaryDir directory;
        const QByteArray payload = QByteArrayLiteral("cached shader");
        const auto artifact = artifactFor(payload);
        FakeNetworkAccessManager network;
        network.payload = payload;
        UpscalingAssetManager manager(&network, directory.path());
        QVERIFY(QDir().mkpath(
            QFileInfo(manager.absolutePath(artifact)).absolutePath()));
        QFile cached(manager.absolutePath(artifact));
        QVERIFY(cached.open(QIODevice::WriteOnly));
        QCOMPARE(cached.write(payload), payload.size());
        cached.close();

        manager.download(artifact.assetSetId, {artifact});
        QTRY_COMPARE(phase(manager, artifact.assetSetId),
            QStringLiteral("ready"));
        QCOMPARE(network.requestCount, 0);
    }

    void multiArtifactProgressNeverRegresses()
    {
        QTemporaryDir directory;
        const QByteArray payload(64 * 1024, 'p');
        auto first = artifactFor(payload, QStringLiteral("multi-test"));
        first.id = QStringLiteral("multi-first");
        first.fileName = QStringLiteral("first.glsl");
        first.installRelativePath = QStringLiteral("multi/first.glsl");
        auto second = first;
        second.id = QStringLiteral("multi-second");
        second.fileName = QStringLiteral("second.glsl");
        second.installRelativePath = QStringLiteral("multi/second.glsl");
        second.downloadUrl = QStringLiteral(
            "https://raw.githubusercontent.com/bloc97/Anime4K/test/second.glsl");

        FakeNetworkAccessManager network;
        network.payload = payload;
        UpscalingAssetManager manager(&network, directory.path());
        QList<qint64> publishedBytes;
        connect(&manager, &UpscalingAssetManager::stateChanged,
            this, [&] (const QString &assetSetId) {
                if (assetSetId != QStringLiteral("multi-test"))
                    return;
                const QVariantMap state = manager.stateFor(assetSetId);
                if (state.value(QStringLiteral("phase")).toString()
                    != QStringLiteral("checking")) {
                    publishedBytes.append(state.value(
                        QStringLiteral("bytesReceived")).toLongLong());
                }
            });

        manager.download(QStringLiteral("multi-test"), {first, second});
        QTRY_COMPARE_WITH_TIMEOUT(
            phase(manager, QStringLiteral("multi-test")),
            QStringLiteral("ready"), 3000);
        QCOMPARE(network.requestCount, 2);
        QVERIFY(publishedBytes.size() >= 4);
        for (qsizetype index = 1; index < publishedBytes.size(); ++index) {
            QVERIFY2(publishedBytes.at(index) >= publishedBytes.at(index - 1),
                qPrintable(QStringLiteral("progress regressed from %1 to %2")
                    .arg(publishedBytes.at(index - 1))
                    .arg(publishedBytes.at(index))));
        }
        QCOMPARE(publishedBytes.constLast(), 2 * payload.size());
    }

    void cancelFencesAStaleReply()
    {
        QTemporaryDir directory;
        const QByteArray payload(128 * 1024, 'x');
        const auto artifact = artifactFor(payload);
        FakeNetworkAccessManager network;
        network.payload = payload;
        network.delayMs = 150;
        UpscalingAssetManager manager(&network, directory.path());

        manager.download(artifact.assetSetId, {artifact});
        QTRY_COMPARE(phase(manager, artifact.assetSetId),
            QStringLiteral("downloading"));
        manager.cancel(artifact.assetSetId);
        QCOMPARE(phase(manager, artifact.assetSetId),
            QStringLiteral("missing"));
        QTest::qWait(250);
        QCOMPARE(phase(manager, artifact.assetSetId),
            QStringLiteral("missing"));
        QVERIFY(!QFileInfo::exists(manager.absolutePath(artifact)));
    }

    void rejectsEscapingOrUntrustedCatalogEntries()
    {
        QTemporaryDir directory;
        const QByteArray payload = QByteArrayLiteral("shader");
        auto artifact = artifactFor(payload);
        FakeNetworkAccessManager network;
        network.payload = payload;
        UpscalingAssetManager manager(&network, directory.path());

        artifact.installRelativePath = QStringLiteral("../escape.glsl");
        QVERIFY(manager.absolutePath(artifact).isEmpty());
        artifact.installRelativePath = QStringLiteral("safe/test.glsl");
        artifact.downloadUrl = QStringLiteral("https://example.test/model.glsl");
        manager.download(artifact.assetSetId, {artifact});
        QTRY_COMPARE(phase(manager, artifact.assetSetId),
            QStringLiteral("failed"));
        QCOMPARE(manager.stateFor(artifact.assetSetId)
                     .value(QStringLiteral("errorCode")).toString(),
            QStringLiteral("catalog-invalid"));
        QCOMPARE(network.requestCount, 0);
    }

    void rejectsPreexistingDirectoryRedirectOutsideAssetRoot()
    {
        QTemporaryDir assetRoot;
        QTemporaryDir outsideDirectory;
        QVERIFY(assetRoot.isValid());
        QVERIFY(outsideDirectory.isValid());
        const QString redirectPath = QDir(assetRoot.path()).filePath(
            QStringLiteral("anime4k"));
        QString redirectError;
        QVERIFY2(
            createDirectoryRedirect(
                redirectPath, outsideDirectory.path(), &redirectError),
            qPrintable(redirectError));
        DirectoryRedirectGuard redirectGuard(redirectPath);

        const QByteArray payload = QByteArrayLiteral("junction escape shader");
        const auto artifact = artifactFor(payload);
        FakeNetworkAccessManager network;
        network.payload = payload;
        UpscalingAssetManager manager(&network, assetRoot.path());

        QVERIFY(manager.absolutePath(artifact).isEmpty());
        manager.download(artifact.assetSetId, {artifact});
        QTRY_COMPARE_WITH_TIMEOUT(
            phase(manager, artifact.assetSetId),
            QStringLiteral("failed"),
            3000);
        const QString errorCode = manager.stateFor(artifact.assetSetId)
                                      .value(QStringLiteral("errorCode"))
                                      .toString();
        QVERIFY(errorCode == QStringLiteral("catalog-invalid")
                || errorCode == QStringLiteral("storage"));
        QCOMPARE(network.requestCount, 0);
        QVERIFY(!QFileInfo::exists(QDir(outsideDirectory.path()).filePath(
            QStringLiteral("v1-test/test.glsl"))));
        QVERIFY(!QFileInfo::exists(QDir(outsideDirectory.path()).filePath(
            QStringLiteral("v1-test"))));
    }

    void rejectsPreexistingLockDirectoryRedirectOutsideAssetRoot()
    {
        QTemporaryDir assetRoot;
        QTemporaryDir outsideDirectory;
        QVERIFY(assetRoot.isValid());
        QVERIFY(outsideDirectory.isValid());
        const QString redirectPath = QDir(assetRoot.path()).filePath(
            QStringLiteral(".locks"));
        QString redirectError;
        QVERIFY2(
            createDirectoryRedirect(
                redirectPath, outsideDirectory.path(), &redirectError),
            qPrintable(redirectError));
        DirectoryRedirectGuard redirectGuard(redirectPath);

        const QByteArray payload = QByteArrayLiteral("lock escape shader");
        const auto artifact = artifactFor(payload);
        FakeNetworkAccessManager network;
        network.payload = payload;
        UpscalingAssetManager manager(&network, assetRoot.path());

        manager.download(artifact.assetSetId, {artifact});
        QTRY_COMPARE_WITH_TIMEOUT(
            phase(manager, artifact.assetSetId),
            QStringLiteral("failed"),
            3000);
        QCOMPARE(manager.stateFor(artifact.assetSetId)
                     .value(QStringLiteral("errorCode")).toString(),
            QStringLiteral("storage"));
        QCOMPARE(network.requestCount, 0);
        QCOMPARE(QDir(outsideDirectory.path()).entryList(
                     QDir::NoDotAndDotDot | QDir::AllEntries),
            QStringList{});
    }
};

QTEST_GUILESS_MAIN(UpscalingAssetManagerTests)

#include "UpscalingAssetManagerTests.moc"
