#pragma once

#include <QtTypes>

struct CatalogQueryFreshness
{
    bool available = false;
    bool stale = true;
    qint64 fetchedAtMs = 0;
};

class CatalogFreshnessPolicy final
{
public:
    static constexpr qint64 activityRefreshAdmissionMs = 30 * 1000;

    static bool activityIsFresh(
        const CatalogQueryFreshness &resume,
        const CatalogQueryFreshness &recent,
        qint64 nowMs)
    {
        return queryIsFresh(resume, nowMs)
            && queryIsFresh(recent, nowMs);
    }

    static bool activitySnapshotMayCommit(
        quint64 candidateRevision,
        quint64 committedRevision)
    {
        return candidateRevision >= committedRevision;
    }

    static bool activityResultMayAffectState(
        bool requestTokenCurrent,
        quint64 candidateRevision,
        quint64 committedRevision)
    {
        return requestTokenCurrent
            && activitySnapshotMayCommit(
                candidateRevision, committedRevision);
    }

private:
    static bool queryIsFresh(
        const CatalogQueryFreshness &query,
        qint64 nowMs)
    {
        return query.available
            && !query.stale
            && query.fetchedAtMs > 0
            && query.fetchedAtMs <= nowMs
            && nowMs - query.fetchedAtMs < activityRefreshAdmissionMs;
    }
};
