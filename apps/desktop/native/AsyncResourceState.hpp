#pragma once

#include <QDateTime>
#include <QObject>
#include <QVariant>

class AsyncResourceState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Phase phase READ phase NOTIFY stateChanged)
    Q_PROPERTY(bool hasData READ hasData NOTIFY stateChanged)
    Q_PROPERTY(bool stale READ stale NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(quint64 requestId READ requestId NOTIFY stateChanged)
    Q_PROPERTY(QString resourceKey READ resourceKey NOTIFY stateChanged)
    Q_PROPERTY(quint64 sessionGeneration READ sessionGeneration NOTIFY stateChanged)
    Q_PROPERTY(quint64 viewGeneration READ viewGeneration NOTIFY stateChanged)
    Q_PROPERTY(QDateTime updatedAt READ updatedAt NOTIFY stateChanged)
    Q_PROPERTY(QVariant data READ data NOTIFY stateChanged)
    Q_PROPERTY(bool remoteChanged READ remoteChanged NOTIFY stateChanged)

public:
    enum class Phase {
        Idle,
        Loading,
        Ready,
        Refreshing,
        Error,
    };
    Q_ENUM(Phase)

    explicit AsyncResourceState(QObject *parent = nullptr);

    Phase phase() const { return m_phase; }
    bool hasData() const { return m_data.isValid() && !m_data.isNull(); }
    bool stale() const { return m_stale; }
    QString errorMessage() const { return m_errorMessage; }
    quint64 requestId() const { return m_requestId; }
    QString resourceKey() const { return m_resourceKey; }
    quint64 sessionGeneration() const { return m_sessionGeneration; }
    quint64 viewGeneration() const { return m_viewGeneration; }
    QDateTime updatedAt() const { return m_updatedAt; }
    QVariant data() const { return m_data; }
    bool remoteChanged() const { return m_remoteChanged; }

    Q_INVOKABLE quint64 begin(
        const QString &resourceKey,
        quint64 sessionGeneration,
        quint64 viewGeneration,
        bool preserveData = true);
    Q_INVOKABLE bool resolve(
        quint64 requestId,
        const QString &resourceKey,
        quint64 sessionGeneration,
        quint64 viewGeneration,
        const QVariant &data,
        bool stale = false);
    Q_INVOKABLE bool reject(
        quint64 requestId,
        const QString &resourceKey,
        quint64 sessionGeneration,
        quint64 viewGeneration,
        const QString &message);
    Q_INVOKABLE void detach();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void markRemoteChanged();
    Q_INVOKABLE void clearRemoteChanged();

signals:
    void stateChanged();
    void retryRequested();

private:
    bool accepts(
        quint64 requestId,
        const QString &resourceKey,
        quint64 sessionGeneration,
        quint64 viewGeneration) const;

    Phase m_phase = Phase::Idle;
    QVariant m_data;
    bool m_stale = false;
    QString m_errorMessage;
    quint64 m_requestId = 0;
    QString m_resourceKey;
    quint64 m_sessionGeneration = 0;
    quint64 m_viewGeneration = 0;
    QDateTime m_updatedAt;
    bool m_remoteChanged = false;
};
