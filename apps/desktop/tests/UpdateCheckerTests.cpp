#include "UpdateChecker.hpp"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

class FakeReply final : public QNetworkReply
{
public:
    FakeReply(
        const QNetworkRequest &request,
        int httpStatus,
        QByteArray payload,
        QNetworkReply::NetworkError error,
        QObject *parent)
        : QNetworkReply(parent)
        , m_payload(std::move(payload))
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, httpStatus);
        if (error != QNetworkReply::NoError)
            setError(error, QStringLiteral("synthetic network error"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
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
    int httpStatus = 200;
    QByteArray payload;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
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
        return new FakeReply(request, httpStatus, payload, error, this);
    }
};

QByteArray releasePayload(const char *tag)
{
    return QByteArrayLiteral("{\"tag_name\":\"") + tag
        + QByteArrayLiteral(
            "\",\"html_url\":\"https://github.com/TwooSix/Yanami/releases/tag/")
        + tag + QByteArrayLiteral("\"}");
}

} // namespace

class UpdateCheckerTests final : public QObject
{
    Q_OBJECT

private slots:
    void helperCheckPublishesBusyStateBeforeDispatchCompletes()
    {
        FakeNetworkAccessManager network;
        UpdateChecker checker(
            &network, QUrl(QStringLiteral("https://api.example.test/releases")),
            QStringLiteral("0.2.0-dev.15"),
            QString::fromUtf8(YANAMI_UPDATE_HELPER_FIXTURE));
        QSignalSpy changes(&checker, &UpdateChecker::stateChanged);

        checker.check();
        QVERIFY(checker.checking());
        QCOMPARE(checker.hasChecked(), false);
        QCOMPARE(changes.count(), 1);
        checker.check();
        QCOMPARE(changes.count(), 1);
        QTRY_VERIFY(!checker.checking());
        QVERIFY(checker.directUpdateSupported());
        QCOMPARE(network.requestCount, 0);
    }

    void failedHelperStartFallsBackToNetwork()
    {
        FakeNetworkAccessManager network;
        network.payload = releasePayload("v0.2.0-dev.16");
        UpdateChecker checker(
            &network, QUrl(QStringLiteral("https://api.example.test/releases")),
            QStringLiteral("0.2.0-dev.15"),
            QStringLiteral("missing-yanami-update-helper-fixture-64B19693"));

        checker.check();
        QVERIFY(checker.checking());
        QCOMPARE(network.requestCount, 0);
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(network.requestCount, 1);
        QVERIFY(checker.updateAvailable());
        QVERIFY(!checker.directUpdateSupported());
        QCOMPARE(checker.errorMessage(), QString());
    }

    void reportsNewerRelease()
    {
        FakeNetworkAccessManager network;
        network.payload = releasePayload("v0.2.0");
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases/latest")),
            QStringLiteral("0.1.0"));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(network.requestCount, 1);
        QCOMPARE(checker.hasChecked(), true);
        QCOMPARE(checker.releaseFound(), true);
        QCOMPARE(checker.updateAvailable(), true);
        QCOMPARE(checker.latestVersion(), QStringLiteral("v0.2.0"));
        QCOMPARE(checker.releaseUrl().host(), QStringLiteral("github.com"));
    }

    void reportsCurrentReleaseAndSuppressesConcurrentChecks()
    {
        FakeNetworkAccessManager network;
        network.payload = releasePayload("v0.1.0");
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases/latest")),
            QStringLiteral("0.1.0"));

        checker.check();
        checker.check();
        QCOMPARE(network.requestCount, 1);
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(checker.releaseFound(), true);
        QCOMPARE(checker.updateAvailable(), false);
    }

    void treatsAStableReleaseAsNewerThanItsDevelopmentBuild()
    {
        FakeNetworkAccessManager network;
        network.payload = releasePayload("v0.1.0");
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases/latest")),
            QStringLiteral("0.1.0-dev.27"));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(checker.releaseFound(), true);
        QCOMPARE(checker.updateAvailable(), true);
    }

    void comparesDevelopmentSequenceNumbersNumerically()
    {
        FakeNetworkAccessManager network;
        network.payload = releasePayload("v0.1.0-dev.11");
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases/latest")),
            QStringLiteral("0.1.0-dev.2"));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(checker.releaseFound(), true);
        QCOMPARE(checker.updateAvailable(), true);
    }

    void selectsNewestPrereleaseFromReleaseList()
    {
        FakeNetworkAccessManager network;
        network.payload = QByteArrayLiteral(
            "[{\"tag_name\":\"v0.2.0-dev.14\","
            "\"html_url\":\"https://github.com/TwooSix/Yanami/releases/tag/"
            "v0.2.0-dev.14\",\"prerelease\":true},"
            "{\"tag_name\":\"v0.2.0-dev.16\","
            "\"html_url\":\"https://github.com/TwooSix/Yanami/releases/tag/"
            "v0.2.0-dev.16\",\"prerelease\":true}]");
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases")),
            QStringLiteral("0.2.0-dev.15"));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(checker.releaseFound(), true);
        QCOMPARE(checker.updateAvailable(), true);
        QCOMPARE(checker.latestVersion(), QStringLiteral("v0.2.0-dev.16"));
    }

    void stableChannelIgnoresPrereleaseEntries()
    {
        FakeNetworkAccessManager network;
        network.payload = QByteArrayLiteral(
            "[{\"tag_name\":\"v0.3.0-dev.1\","
            "\"html_url\":\"https://github.com/TwooSix/Yanami/releases/tag/"
            "v0.3.0-dev.1\",\"prerelease\":true},"
            "{\"tag_name\":\"v0.2.1\","
            "\"html_url\":\"https://github.com/TwooSix/Yanami/releases/tag/"
            "v0.2.1\",\"prerelease\":false}]");
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases")),
            QStringLiteral("0.2.0"));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(checker.releaseFound(), true);
        QCOMPARE(checker.updateAvailable(), true);
        QCOMPARE(checker.latestVersion(), QStringLiteral("v0.2.1"));
    }

    void installedHelperChecksAndDownloadsIncrementalUpdate()
    {
        FakeNetworkAccessManager network;
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases")),
            QStringLiteral("0.2.0-dev.15"),
            QString::fromUtf8(YANAMI_UPDATE_HELPER_FIXTURE));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(network.requestCount, 0);
        QCOMPARE(checker.directUpdateSupported(), true);
        QCOMPARE(checker.updateAvailable(), true);
        QCOMPARE(checker.incrementalUpdate(), true);
        QCOMPARE(checker.downloadSize(), 1048576);
        QCOMPARE(checker.latestVersion(), QStringLiteral("v0.2.0-dev.16"));

        checker.downloadUpdate();
        QTRY_VERIFY(checker.updateReady());
        QTRY_VERIFY(!checker.downloading());
        QCOMPARE(checker.downloadProgress(), 100);
        QCOMPARE(checker.errorMessage(), QString());
    }

    void ignoresDownloadClickUntilCheckHelperExits()
    {
        FakeNetworkAccessManager network;
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases")),
            QStringLiteral("0.2.0-dev.15"),
            QString::fromUtf8(YANAMI_UPDATE_HELPER_FIXTURE));

        QVERIFY(qputenv(
            "YANAMI_UPDATE_HELPER_FIXTURE_DELAY_CHECK_EXIT", "1"));
        checker.check();
        QTRY_VERIFY(checker.updateAvailable());
        QVERIFY(checker.checking());

        checker.downloadUpdate();
        QCOMPARE(checker.downloading(), false);
        QTRY_VERIFY(!checker.checking());
        qunsetenv("YANAMI_UPDATE_HELPER_FIXTURE_DELAY_CHECK_EXIT");
    }

    void ignoresApplyClickUntilDownloadHelperExits()
    {
        FakeNetworkAccessManager network;
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases")),
            QStringLiteral("0.2.0-dev.15"),
            QString::fromUtf8(YANAMI_UPDATE_HELPER_FIXTURE));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QVERIFY(qputenv(
            "YANAMI_UPDATE_HELPER_FIXTURE_DELAY_DOWNLOAD_EXIT", "1"));
        checker.downloadUpdate();
        QTRY_VERIFY(checker.updateReady());
        QVERIFY(checker.downloading());

        checker.applyUpdate();
        QCOMPARE(checker.applying(), false);
        QTRY_VERIFY(!checker.downloading());
        qunsetenv("YANAMI_UPDATE_HELPER_FIXTURE_DELAY_DOWNLOAD_EXIT");
    }

    void downloadSettlesWhenUpdateDisappearsAfterCheck()
    {
        FakeNetworkAccessManager network;
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases")),
            QStringLiteral("0.2.0-dev.15"),
            QString::fromUtf8(YANAMI_UPDATE_HELPER_FIXTURE));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QVERIFY(checker.updateAvailable());

        QVERIFY(qputenv(
            "YANAMI_UPDATE_HELPER_FIXTURE_DOWNLOAD_RESULT", "current"));
        checker.downloadUpdate();
        QTRY_VERIFY(!checker.downloading());
        qunsetenv("YANAMI_UPDATE_HELPER_FIXTURE_DOWNLOAD_RESULT");

        QCOMPARE(checker.updateReady(), false);
        QCOMPARE(checker.updateAvailable(), false);
        QCOMPARE(checker.downloadProgress(), 0);
        QCOMPARE(checker.errorMessage(), QString());
    }

    void cancelsDownloadDuringBackgroundDispatch()
    {
        FakeNetworkAccessManager network;
        UpdateChecker checker(
            &network, QUrl(QStringLiteral("https://api.example.test/releases")),
            QStringLiteral("0.2.0-dev.15"),
            QString::fromUtf8(YANAMI_UPDATE_HELPER_FIXTURE));
        checker.check();
        QTRY_VERIFY(!checker.checking());

        checker.downloadUpdate();
        QVERIFY(checker.downloading());
        checker.cancelDownload();
        QTRY_VERIFY(!checker.downloading());
        QCOMPARE(checker.downloadProgress(), 0);
        QVERIFY(!checker.updateReady());
        QCOMPARE(checker.errorMessage(), QString());
    }

    void treatsEmptyReleaseListAsAValidResult()
    {
        FakeNetworkAccessManager network;
        network.payload = QByteArrayLiteral("[]");
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases")),
            QStringLiteral("0.2.0-dev.15"));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(checker.releaseFound(), false);
        QCOMPARE(checker.errorMessage(), QString());
    }

    void treatsMissingPublishedReleaseAsAValidResult()
    {
        FakeNetworkAccessManager network;
        network.httpStatus = 404;
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases/latest")),
            QStringLiteral("0.1.0"));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(checker.hasChecked(), true);
        QCOMPARE(checker.releaseFound(), false);
        QCOMPARE(checker.errorMessage(), QString());
    }

    void rejectsMalformedReleaseData()
    {
        FakeNetworkAccessManager network;
        network.payload = QByteArrayLiteral("{\"tag_name\":\"latest\"}");
        UpdateChecker checker(
            &network,
            QUrl(QStringLiteral("https://api.example.test/releases/latest")),
            QStringLiteral("0.1.0"));

        checker.check();
        QTRY_VERIFY(!checker.checking());
        QCOMPARE(checker.releaseFound(), false);
        QVERIFY(!checker.errorMessage().isEmpty());
    }
};

QTEST_MAIN(UpdateCheckerTests)
#include "UpdateCheckerTests.moc"
