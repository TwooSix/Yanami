#include "MetadataEditorViewModel.hpp"

#include "BackendPorts.hpp"

#include <QScopedValueRollback>

namespace {

QString metadataResourceKey(const QString &itemId)
{
    return QStringLiteral("metadata:") + itemId;
}

} // namespace

MetadataEditorViewModel::MetadataEditorViewModel(
    MediaPort *port,
    QObject *parent)
    : QObject(parent)
    , m_port(port)
    , m_resourceState(this)
    , m_saveOperation(this)
    , m_reconciliationOperation(this)
{
    if (!port)
        return;
    connect(port, &MediaPort::operationCompleted, this,
        [this](const QString &requestId, const QString &itemId,
            MediaPort::Operation operation,
            const QVariantMap &result) {
            handleCompleted(requestId, itemId, static_cast<int>(operation), result);
        });
    connect(port, &MediaPort::operationFailed, this,
        [this](const QString &requestId, const QString &itemId,
            MediaPort::Operation operation,
            const QString &message, bool nonModal) {
            handleFailed(requestId, itemId, static_cast<int>(operation), message, nonModal);
        });
}

quint64 MetadataEditorViewModel::open(const QVariantMap &item)
{
    const QString nextItemId = item.value(QStringLiteral("id"))
        .toString().trimmed();
    if (nextItemId.isEmpty())
        return 0;

    ++m_viewGeneration;
    m_opened = true;
    m_itemId = nextItemId;
    m_metadata.clear();
    m_saveMutationId.clear();
    m_loadRequestId.clear();
    m_saveOperation.reset();
    requestLoad(false);
    emit openedChanged();
    emit identityChanged();
    emit metadataChanged();
    return m_viewGeneration;
}

void MetadataEditorViewModel::dismiss()
{
    ++m_viewGeneration;
    const bool wasOpened = m_opened;
    m_opened = false;
    m_saveMutationId.clear();
    m_loadRequestId.clear();
    m_resourceState.detach();
    m_saveOperation.reset();
    if (wasOpened)
        emit openedChanged();
    emit identityChanged();
}

void MetadataEditorViewModel::invalidateSession()
{
    dismiss();
    m_reconciliationMutationId.clear();
    m_reconciliationItemId.clear();
    m_pendingReconciliationItems.clear();
    m_reconciliationOperation.reset();
}

void MetadataEditorViewModel::retry()
{
    if (!m_opened || m_itemId.isEmpty())
        return;
    requestLoad(m_resourceState.hasData());
}

bool MetadataEditorViewModel::save(const QVariantMap &changes)
{
    if (!m_opened || m_itemId.isEmpty() || m_saveOperation.busy())
        return false;
    if (!m_port) {
        emit saveFailed(m_itemId, tr("Backend is unavailable."), false);
        return false;
    }

    m_saveMutationId = QStringLiteral("metadata-save.%1.%2")
        .arg(m_itemId)
        .arg(++m_mutationSequence);
    m_saveOperation.begin(m_saveMutationId);
    QScopedValueRollback dispatchGuard(
        m_transportDispatchDepth, m_transportDispatchDepth + 1);
    m_port->updateMetadata(m_saveMutationId, m_itemId, changes);
    return true;
}

void MetadataEditorViewModel::requestLoad(bool preserveData)
{
    const QString resourceKey = metadataResourceKey(m_itemId);
    m_resourceState.begin(resourceKey, 0, m_viewGeneration, preserveData);
    m_loadRequestId = QStringLiteral("metadata-load.%1.%2.%3")
        .arg(m_itemId)
        .arg(m_viewGeneration)
        .arg(m_resourceState.requestId());
    if (m_port) {
        QScopedValueRollback dispatchGuard(
            m_transportDispatchDepth, m_transportDispatchDepth + 1);
        m_port->loadMetadata(m_loadRequestId, m_itemId, m_viewGeneration);
        return;
    }
    const QString message = tr("Backend is unavailable.");
    m_resourceState.reject(m_resourceState.requestId(), resourceKey, 0,
        m_viewGeneration, message);
    emit loadFailed(m_itemId, message, false);
}

void MetadataEditorViewModel::handleCompleted(
    const QString &requestId,
    const QString &itemId,
    int operationValue,
    const QVariantMap &result)
{
    if (m_transportDispatchDepth > 0) {
        QMetaObject::invokeMethod(this,
            [this, requestId, itemId, operationValue, result] {
                handleCompleted(requestId, itemId, operationValue, result);
            },
            Qt::QueuedConnection);
        return;
    }
    const auto operation = static_cast<MediaPort::Operation>(operationValue);
    if (operation == MediaPort::Operation::LoadMetadata) {
        if (!accepts(requestId, itemId, result))
            return;
        const QString key = metadataResourceKey(m_itemId);
        if (!m_resourceState.resolve(m_resourceState.requestId(), key, 0,
                m_viewGeneration, result)) {
            return;
        }
        m_loadRequestId.clear();
        m_metadata = result;
        emit metadataChanged();
        emit metadataReady(m_metadata, m_viewGeneration);
        return;
    }

    if (operation == MediaPort::Operation::RefreshMetadata) {
        if (!m_reconciliationOperation.busy()
            || requestId != m_reconciliationMutationId
            || itemId != m_reconciliationItemId) {
            return;
        }
        const QString mutationId = m_reconciliationMutationId;
        m_reconciliationMutationId.clear();
        m_reconciliationItemId.clear();
        if (!m_reconciliationOperation.resolve(mutationId, result))
            return;
        startNextReconciliation();
        return;
    }

    if (!m_opened || itemId != m_itemId)
        return;

    if (operation == MediaPort::Operation::UpdateMetadata) {
        if (!m_saveOperation.busy() || requestId != m_saveMutationId)
            return;
        const QString mutationId = m_saveMutationId;
        m_saveMutationId.clear();
        m_saveOperation.resolve(mutationId, result);
        emit saveCompleted(m_itemId, result);
        if (result.value(QStringLiteral("providerIdsChanged")).toBool())
            enqueueReconciliation(itemId);
        return;
    } else {
        return;
    }
}

void MetadataEditorViewModel::handleFailed(
    const QString &requestId,
    const QString &itemId,
    int operationValue,
    const QString &message,
    bool nonModal)
{
    if (m_transportDispatchDepth > 0) {
        QMetaObject::invokeMethod(this,
            [this, requestId, itemId, operationValue, message, nonModal] {
                handleFailed(requestId, itemId, operationValue, message, nonModal);
            },
            Qt::QueuedConnection);
        return;
    }
    const auto operation = static_cast<MediaPort::Operation>(operationValue);
    if (operation == MediaPort::Operation::LoadMetadata) {
        if (!m_opened || itemId != m_itemId || requestId != m_loadRequestId)
            return;
        const QString key = metadataResourceKey(m_itemId);
        if (!m_resourceState.reject(m_resourceState.requestId(), key, 0,
                m_viewGeneration, message)) {
            return;
        }
        m_loadRequestId.clear();
        emit loadFailed(itemId, message, nonModal);
        return;
    }

    if (operation == MediaPort::Operation::RefreshMetadata) {
        if (!m_reconciliationOperation.busy()
            || requestId != m_reconciliationMutationId
            || itemId != m_reconciliationItemId) {
            return;
        }
        const QString mutationId = m_reconciliationMutationId;
        m_reconciliationMutationId.clear();
        m_reconciliationItemId.clear();
        if (!m_reconciliationOperation.reject(mutationId, message))
            return;
        emit reconciliationFailed(itemId, message, nonModal);
        startNextReconciliation();
        return;
    }

    if (!m_opened || itemId != m_itemId)
        return;
    if (operation == MediaPort::Operation::UpdateMetadata
        && m_saveOperation.busy() && requestId == m_saveMutationId) {
        const QString mutationId = m_saveMutationId;
        m_saveMutationId.clear();
        m_saveOperation.reject(mutationId, message);
        emit saveFailed(itemId, message, nonModal);
        return;
    }
}

void MetadataEditorViewModel::enqueueReconciliation(const QString &itemId)
{
    if (itemId.isEmpty())
        return;
    if (m_reconciliationOperation.busy()) {
        if (!m_pendingReconciliationItems.contains(itemId))
            m_pendingReconciliationItems.enqueue(itemId);
        return;
    }
    m_pendingReconciliationItems.enqueue(itemId);
    startNextReconciliation();
}

void MetadataEditorViewModel::startNextReconciliation()
{
    if (m_reconciliationOperation.busy()
        || m_pendingReconciliationItems.isEmpty()) {
        return;
    }
    m_reconciliationItemId = m_pendingReconciliationItems.dequeue();
    m_reconciliationMutationId = QStringLiteral("metadata-reconcile.%1.%2")
        .arg(m_reconciliationItemId)
        .arg(++m_mutationSequence);
    m_reconciliationOperation.begin(m_reconciliationMutationId);
    if (m_port) {
        QScopedValueRollback dispatchGuard(
            m_transportDispatchDepth, m_transportDispatchDepth + 1);
        m_port->refreshMetadata(
            m_reconciliationMutationId,
            m_reconciliationItemId,
            QStringLiteral("all"),
            true,
            QStringLiteral("metadata-save"));
        return;
    }
    const QString mutationId = m_reconciliationMutationId;
    const QString itemId = m_reconciliationItemId;
    m_reconciliationMutationId.clear();
    m_reconciliationItemId.clear();
    m_reconciliationOperation.reject(mutationId, tr("Backend is unavailable."));
    emit reconciliationFailed(itemId, tr("Backend is unavailable."), false);
}

bool MetadataEditorViewModel::accepts(
    const QString &requestId,
    const QString &itemId,
    const QVariantMap &result) const
{
    return m_opened
        && requestId == m_loadRequestId
        && itemId == m_itemId
        && result.value(QStringLiteral("id"), itemId).toString() == m_itemId
        && result.value(QStringLiteral("clientViewGeneration"), m_viewGeneration)
               .toULongLong() == m_viewGeneration;
}
