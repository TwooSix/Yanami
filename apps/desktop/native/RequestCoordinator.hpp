#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

struct RequestCommitToken
{
    QString queryKey;
    quint64 sessionGeneration = 0;
    quint64 queryGeneration = 0;
    quint64 mutationEpochAtStart = 0;
};

// A latest-request token is scoped to a presentation lane rather than to a
// cached query. Starting a new request in the same lane supersedes every older
// result in that lane, while independent lanes can continue concurrently.
// `requestKey` identifies an equivalent in-flight request so callers can
// coalesce duplicate work without advancing the lane generation.
struct LatestRequestToken
{
    QString laneKey;
    QString requestKey;
    quint64 requestId = 0;
    quint64 sessionGeneration = 0;
    quint64 laneGeneration = 0;
};

class RequestCoordinator
{
public:
    RequestCommitToken begin(const QString &queryKey, quint64 sessionGeneration)
    {
        return RequestCommitToken{
            queryKey,
            sessionGeneration,
            m_queryGenerations.value(queryKey),
            m_mutationEpoch,
        };
    }

    bool accepts(const RequestCommitToken &token, quint64 sessionGeneration) const
    {
        return !token.queryKey.isEmpty()
            && token.sessionGeneration == sessionGeneration
            && token.queryGeneration == m_queryGenerations.value(token.queryKey);
    }

    LatestRequestToken beginLatest(
        const QString &laneKey,
        const QString &requestKey,
        quint64 sessionGeneration)
    {
        const quint64 generation = ++m_laneGenerations[laneKey];
        return LatestRequestToken{
            laneKey,
            requestKey,
            ++m_nextRequestId,
            sessionGeneration,
            generation,
        };
    }

    bool acceptsLatest(
        const LatestRequestToken &token,
        quint64 sessionGeneration) const
    {
        return !token.laneKey.isEmpty()
            && token.requestId != 0
            && token.sessionGeneration == sessionGeneration
            && token.laneGeneration == m_laneGenerations.value(token.laneKey);
    }

    bool representsLatest(
        const LatestRequestToken &token,
        const QString &laneKey,
        const QString &requestKey,
        quint64 sessionGeneration) const
    {
        return token.laneKey == laneKey
            && token.requestKey == requestKey
            && acceptsLatest(token, sessionGeneration);
    }

    quint64 invalidateLatestLane(const QString &laneKey)
    {
        return ++m_laneGenerations[laneKey];
    }

    quint64 invalidate(const QString &queryKey)
    {
        ++m_mutationEpoch;
        return ++m_queryGenerations[queryKey];
    }

    void invalidate(const QStringList &queryKeys)
    {
        if (queryKeys.isEmpty())
            return;
        ++m_mutationEpoch;
        for (const QString &key : queryKeys)
            ++m_queryGenerations[key];
    }

    void invalidateAll()
    {
        ++m_mutationEpoch;
        for (auto iterator = m_queryGenerations.begin();
             iterator != m_queryGenerations.end(); ++iterator) {
            ++iterator.value();
        }
        for (auto iterator = m_laneGenerations.begin();
             iterator != m_laneGenerations.end(); ++iterator) {
            ++iterator.value();
        }
    }

    quint64 generation(const QString &queryKey) const
    {
        return m_queryGenerations.value(queryKey);
    }

    quint64 mutationEpoch() const { return m_mutationEpoch; }

private:
    QHash<QString, quint64> m_queryGenerations;
    QHash<QString, quint64> m_laneGenerations;
    quint64 m_nextRequestId = 0;
    quint64 m_mutationEpoch = 0;
};
