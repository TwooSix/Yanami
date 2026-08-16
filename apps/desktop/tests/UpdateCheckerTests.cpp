#include "UpdateChecker.hpp"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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
