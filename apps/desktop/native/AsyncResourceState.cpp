#include "AsyncResourceState.hpp"

AsyncResourceState::AsyncResourceState(QObject *parent)
    : QObject(parent)
{
}

quint64 AsyncResourceState::begin(
    const QString &resourceKey,
    quint64 sessionGeneration,
    quint64 viewGeneration,
    bool preserveData)
{
    ++m_requestId;
    m_resourceKey = resourceKey;
    m_sessionGeneration = sessionGeneration;
    m_viewGeneration = viewGeneration;
    m_errorMessage.clear();
    m_remoteChanged = false;
    if (preserveData && hasData()) {
        m_phase = Phase::Refreshing;
        m_stale = true;
    } else {
        m_data.clear();
        m_phase = Phase::Loading;
        m_stale = false;
        m_updatedAt = {};
    }
    emit stateChanged();
    return m_requestId;
}

bool AsyncResourceState::resolve(
    quint64 requestId,
    const QString &resourceKey,
    quint64 sessionGeneration,
    quint64 viewGeneration,
    const QVariant &data,
    bool stale)
{
    if (!accepts(requestId, resourceKey, sessionGeneration, viewGeneration))
        return false;
    m_data = data;
    m_stale = stale;
    m_errorMessage.clear();
    m_phase = Phase::Ready;
    m_updatedAt = QDateTime::currentDateTimeUtc();
    m_remoteChanged = false;
    emit stateChanged();
    return true;
}

bool AsyncResourceState::reject(
    quint64 requestId,
    const QString &resourceKey,
    quint64 sessionGeneration,
    quint64 viewGeneration,
    const QString &message)
{
    if (!accepts(requestId, resourceKey, sessionGeneration, viewGeneration))
        return false;
    m_errorMessage = message.trimmed();
    if (hasData()) {
        // A failed SWR refresh must not turn usable cached content into a
        // blocking error page.
        m_phase = Phase::Ready;
        m_stale = true;
    } else {
        m_phase = Phase::Error;
        m_stale = false;
    }
    emit stateChanged();
    return true;
}

void AsyncResourceState::detach()
{
    ++m_requestId;
    ++m_viewGeneration;
    m_errorMessage.clear();
    m_phase = hasData() ? Phase::Ready : Phase::Idle;
    emit stateChanged();
}

void AsyncResourceState::reset()
{
    ++m_requestId;
    m_phase = Phase::Idle;
    m_data.clear();
    m_stale = false;
    m_errorMessage.clear();
    m_resourceKey.clear();
    m_updatedAt = {};
    m_remoteChanged = false;
    emit stateChanged();
}

void AsyncResourceState::retry()
{
    emit retryRequested();
}

void AsyncResourceState::markRemoteChanged()
{
    if (m_remoteChanged)
        return;
    m_remoteChanged = true;
    emit stateChanged();
}

void AsyncResourceState::clearRemoteChanged()
{
    if (!m_remoteChanged)
        return;
    m_remoteChanged = false;
    emit stateChanged();
}

bool AsyncResourceState::accepts(
    quint64 requestId,
    const QString &resourceKey,
    quint64 sessionGeneration,
    quint64 viewGeneration) const
{
    const bool pending = m_phase == Phase::Loading
        || m_phase == Phase::Refreshing;
    return pending
        && requestId != 0
        && requestId == m_requestId
        && resourceKey == m_resourceKey
        && sessionGeneration == m_sessionGeneration
        && viewGeneration == m_viewGeneration;
}
