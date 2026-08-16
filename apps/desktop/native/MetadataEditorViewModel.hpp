#pragma once

#include "AsyncOperationState.hpp"
#include "AsyncResourceState.hpp"

#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QVariantMap>

class MediaPort;

class MetadataEditorViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(AsyncResourceState *resourceState READ resourceState CONSTANT)
    Q_PROPERTY(AsyncOperationState *saveOperation READ saveOperation CONSTANT)
    Q_PROPERTY(AsyncOperationState *reconciliationOperation
            READ reconciliationOperation CONSTANT)
    Q_PROPERTY(bool opened READ opened NOTIFY openedChanged)
    Q_PROPERTY(QString itemId READ itemId NOTIFY identityChanged)
    Q_PROPERTY(quint64 viewGeneration READ viewGeneration NOTIFY identityChanged)
    Q_PROPERTY(QVariantMap metadata READ metadata NOTIFY metadataChanged)

public:
    explicit MetadataEditorViewModel(MediaPort *port, QObject *parent = nullptr);

    AsyncResourceState *resourceState() { return &m_resourceState; }
    AsyncOperationState *saveOperation() { return &m_saveOperation; }
    AsyncOperationState *reconciliationOperation() { return &m_reconciliationOperation; }
    bool opened() const { return m_opened; }
    QString itemId() const { return m_itemId; }
    quint64 viewGeneration() const { return m_viewGeneration; }
    QVariantMap metadata() const { return m_metadata; }

    Q_INVOKABLE quint64 open(const QVariantMap &item);
    Q_INVOKABLE void dismiss();
    void invalidateSession();
    Q_INVOKABLE void retry();
    Q_INVOKABLE bool save(const QVariantMap &changes);

signals:
    void openedChanged();
    void identityChanged();
    void metadataChanged();
    void metadataReady(const QVariantMap &metadata, quint64 viewGeneration);
    void loadFailed(const QString &itemId, const QString &message, bool nonModal);
    void saveCompleted(const QString &itemId, const QVariantMap &result);
    void saveFailed(const QString &itemId, const QString &message, bool nonModal);
    void reconciliationFailed(
        const QString &itemId,
        const QString &message,
        bool nonModal);

private:
    void requestLoad(bool preserveData);
    void enqueueReconciliation(const QString &itemId);
    void startNextReconciliation();
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
    bool accepts(
        const QString &requestId,
        const QString &itemId,
        const QVariantMap &result) const;

    QPointer<MediaPort> m_port;
    AsyncResourceState m_resourceState;
    AsyncOperationState m_saveOperation;
    AsyncOperationState m_reconciliationOperation;
    bool m_opened = false;
    QString m_itemId;
    QString m_loadRequestId;
    QString m_saveMutationId;
    QString m_reconciliationMutationId;
    QString m_reconciliationItemId;
    QQueue<QString> m_pendingReconciliationItems;
    quint64 m_viewGeneration = 0;
    quint64 m_mutationSequence = 0;
    QVariantMap m_metadata;
    int m_transportDispatchDepth = 0;
};
