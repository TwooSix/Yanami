#include "AsyncOperationState.hpp"

AsyncOperationState::AsyncOperationState(QObject *parent)
    : QObject(parent)
{
}

bool AsyncOperationState::begin(const QString &mutationId)
{
    const QString normalized = mutationId.trimmed();
    if (normalized.isEmpty())
        return false;
    m_mutationId = normalized;
    m_phase = Phase::Running;
    m_errorMessage.clear();
    m_result.clear();
    m_startedAt = QDateTime::currentDateTimeUtc();
    m_finishedAt = {};
    emit stateChanged();
    return true;
}

bool AsyncOperationState::resolve(
    const QString &mutationId,
    const QVariant &result)
{
    if (!accepts(mutationId))
        return false;
    m_phase = Phase::Succeeded;
    m_errorMessage.clear();
    m_result = result;
    m_finishedAt = QDateTime::currentDateTimeUtc();
    emit stateChanged();
    return true;
}

bool AsyncOperationState::reject(
    const QString &mutationId,
    const QString &message)
{
    if (!accepts(mutationId))
        return false;
    m_phase = Phase::Failed;
    m_errorMessage = message.trimmed();
    m_result.clear();
    m_finishedAt = QDateTime::currentDateTimeUtc();
    emit stateChanged();
    return true;
}

void AsyncOperationState::reset()
{
    m_phase = Phase::Idle;
    m_mutationId.clear();
    m_errorMessage.clear();
    m_result.clear();
    m_startedAt = {};
    m_finishedAt = {};
    emit stateChanged();
}

bool AsyncOperationState::accepts(const QString &mutationId) const
{
    return m_phase == Phase::Running
        && !mutationId.isEmpty()
        && mutationId == m_mutationId;
}
