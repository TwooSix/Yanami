#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariant>

// State for user-initiated commands. A caller-owned mutation id makes stale
// completion impossible to commit after a newer command supersedes it.
class AsyncOperationState final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Phase phase READ phase NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString mutationId READ mutationId NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QVariant result READ result NOTIFY stateChanged)
    Q_PROPERTY(QDateTime startedAt READ startedAt NOTIFY stateChanged)
    Q_PROPERTY(QDateTime finishedAt READ finishedAt NOTIFY stateChanged)

public:
    enum class Phase {
        Idle,
        Running,
        Succeeded,
        Failed,
    };
    Q_ENUM(Phase)

    explicit AsyncOperationState(QObject *parent = nullptr);

    Phase phase() const { return m_phase; }
    bool busy() const { return m_phase == Phase::Running; }
    QString mutationId() const { return m_mutationId; }
    QString errorMessage() const { return m_errorMessage; }
    QVariant result() const { return m_result; }
    QDateTime startedAt() const { return m_startedAt; }
    QDateTime finishedAt() const { return m_finishedAt; }

    Q_INVOKABLE bool begin(const QString &mutationId);
    Q_INVOKABLE bool resolve(
        const QString &mutationId,
        const QVariant &result = {});
    Q_INVOKABLE bool reject(
        const QString &mutationId,
        const QString &message);
    Q_INVOKABLE void reset();

signals:
    void stateChanged();

private:
    bool accepts(const QString &mutationId) const;

    Phase m_phase = Phase::Idle;
    QString m_mutationId;
    QString m_errorMessage;
    QVariant m_result;
    QDateTime m_startedAt;
    QDateTime m_finishedAt;
};
