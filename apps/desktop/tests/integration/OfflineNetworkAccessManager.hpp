#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <utility>

namespace YanamiTest {

class OfflineReply final : public QNetworkReply
{
public:
    OfflineReply(
        const QNetworkRequest &request,
        QByteArray payload,
        bool succeed,
        QObject *parent)
        : QNetworkReply(parent)
        , m_payload(std::move(payload))
        , m_succeed(succeed)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        if (m_succeed) {
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        } else {
            setError(
                ContentNotFoundError,
                QStringLiteral("offline smoke forbids external network access"));
        }
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this] {
            if (isFinished())
                return;
            if (m_succeed && !m_payload.isEmpty())
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
    qint64 readData(char *data, qint64 maximumSize) override
    {
        const qint64 available = m_payload.size() - m_offset;
        if (available <= 0)
            return -1;
        const qint64 count = std::min(maximumSize, available);
        std::memcpy(
            data,
            m_payload.constData() + m_offset,
            static_cast<std::size_t>(count));
        m_offset += count;
        return count;
    }

private:
    QByteArray m_payload;
    bool m_succeed = false;
    qint64 m_offset = 0;
};

class OfflineNetworkAccessManager final : public QNetworkAccessManager
{
public:
    explicit OfflineNetworkAccessManager(
        QByteArray payload,
        bool succeed,
        QObject *parent = nullptr)
        : QNetworkAccessManager(parent)
        , m_payload(std::move(payload))
        , m_succeed(succeed)
    {
    }

    int requestCount() const { return m_requestCount; }

protected:
    QNetworkReply *createRequest(
        Operation operation,
        const QNetworkRequest &request,
        QIODevice *outgoingData) override
    {
        Q_UNUSED(operation)
        Q_UNUSED(outgoingData)
        ++m_requestCount;
        return new OfflineReply(request, m_payload, m_succeed, this);
    }

private:
    QByteArray m_payload;
    bool m_succeed = false;
    int m_requestCount = 0;
};

} // namespace YanamiTest
