#pragma once

#include "AsyncOperationState.hpp"
#include "AsyncResourceState.hpp"

#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <QVariantMap>

class MediaPort;

class MediaTargetFlowViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(AsyncResourceState *targetsState READ targetsState CONSTANT)
    Q_PROPERTY(AsyncOperationState *submitOperation READ submitOperation CONSTANT)
    Q_PROPERTY(QString itemId READ itemId NOTIFY requestChanged)
    Q_PROPERTY(QVariantMap item READ item NOTIFY requestChanged)
    Q_PROPERTY(QVariantList options READ options NOTIFY optionsChanged)

public:
    explicit MediaTargetFlowViewModel(MediaPort *port, QObject *parent = nullptr);

    AsyncResourceState *targetsState() { return &m_targetsState; }
    AsyncOperationState *submitOperation() { return &m_submitOperation; }
    QString itemId() const { return m_itemId; }
    QVariantMap item() const { return m_item; }
    QVariantList options() const { return m_options; }

    Q_INVOKABLE bool load(const QVariantMap &item);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool submit(const QString &targetId, const QString &newName);

signals:
    void requestChanged();
    void optionsChanged();
    void targetsReady(const QVariantMap &item, const QVariantList &options);
    void loadFailed(const QString &itemId, const QString &message, bool nonModal);
    void submitCompleted(const QString &itemId, const QVariantMap &result);
    void submitFailed(const QString &itemId, const QString &message, bool nonModal);

private:
    void handleCompleted(
        const QString &requestId,
        const QString &itemId,
        int operationValue,
        const QVariantMap &result);
    void handleFailed(
        const QString &requestId,
        const QString &itemId,
        int operationValue,
        const QString &message,
        bool nonModal);

    QPointer<MediaPort> m_port;
    AsyncResourceState m_targetsState;
    AsyncOperationState m_submitOperation;
    QString m_itemId;
    QString m_loadRequestId;
    QString m_submitMutationId;
    QVariantMap m_item;
    QVariantList m_options;
    quint64 m_requestGeneration = 0;
    quint64 m_mutationSequence = 0;
    int m_transportDispatchDepth = 0;
};
