#include "MediaTargetFlowViewModel.hpp"

#include "BackendPorts.hpp"

#include <QScopedValueRollback>

namespace {

QString targetsResourceKey(const QString &itemId)
{
    return QStringLiteral("playlist-targets:") + itemId;
}

} // namespace

MediaTargetFlowViewModel::MediaTargetFlowViewModel(
    MediaPort *port,
    QObject *parent)
    : QObject(parent)
    , m_port(port)
    , m_targetsState(this)
    , m_submitOperation(this)
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

bool MediaTargetFlowViewModel::load(const QVariantMap &item)
{
    const QString nextItemId = item.value(QStringLiteral("id"))
        .toString().trimmed();
    if (nextItemId.isEmpty())
        return false;

    ++m_requestGeneration;
    m_itemId = nextItemId;
    m_item = item;
    m_options.clear();
    m_submitMutationId.clear();
    m_submitOperation.reset();
    const QString key = targetsResourceKey(m_itemId);
    m_targetsState.begin(key, 0, m_requestGeneration, false);
    m_loadRequestId = QStringLiteral("playlist-targets.%1.%2.%3")
        .arg(m_itemId)
        .arg(m_requestGeneration)
        .arg(m_targetsState.requestId());
    emit requestChanged();
    emit optionsChanged();
    if (m_port) {
        QScopedValueRollback dispatchGuard(
            m_transportDispatchDepth, m_transportDispatchDepth + 1);
        m_port->loadPlaylistTargets(m_loadRequestId, m_itemId);
        return true;
    }
    const QString message = tr("Backend is unavailable.");
    m_targetsState.reject(m_targetsState.requestId(), key, 0,
        m_requestGeneration, message);
    emit loadFailed(m_itemId, message, false);
    return false;
}

void MediaTargetFlowViewModel::cancel()
{
    ++m_requestGeneration;
    m_itemId.clear();
    m_loadRequestId.clear();
    m_submitMutationId.clear();
    m_item.clear();
    m_options.clear();
    m_targetsState.detach();
    m_submitOperation.reset();
    emit requestChanged();
    emit optionsChanged();
}

bool MediaTargetFlowViewModel::submit(
    const QString &targetId,
    const QString &newName)
{
    if (m_itemId.isEmpty() || m_submitOperation.busy() || !m_port)
        return false;
    const QString normalizedTarget = targetId.trimmed();
    const QString normalizedName = newName.trimmed();
    if (normalizedTarget.isEmpty() == normalizedName.isEmpty())
        return false;

    m_submitMutationId = QStringLiteral("playlist-add.%1.%2")
        .arg(m_itemId)
        .arg(++m_mutationSequence);
    m_submitOperation.begin(m_submitMutationId);
    QScopedValueRollback dispatchGuard(
        m_transportDispatchDepth, m_transportDispatchDepth + 1);
    if (!normalizedTarget.isEmpty())
        m_port->addToPlaylist(
            m_submitMutationId, m_itemId, normalizedTarget);
    else
        m_port->createPlaylistAndAdd(
            m_submitMutationId, m_itemId, normalizedName);
    return true;
}

void MediaTargetFlowViewModel::handleCompleted(
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
    if (operation == MediaPort::Operation::LoadPlaylistTargets) {
        if (itemId != m_itemId || m_itemId.isEmpty()
            || requestId != m_loadRequestId)
            return;
        const QString key = targetsResourceKey(m_itemId);
        const QVariantList options = result.value(QStringLiteral("options")).toList();
        if (!m_targetsState.resolve(m_targetsState.requestId(), key, 0,
                m_requestGeneration, options)) {
            return;
        }
        m_options = options;
        m_loadRequestId.clear();
        emit optionsChanged();
        emit targetsReady(m_item, m_options);
        return;
    }

    if (operation != MediaPort::Operation::AddToPlaylist
        || itemId != m_itemId || !m_submitOperation.busy()
        || requestId != m_submitMutationId) {
        return;
    }
    const QString mutationId = m_submitMutationId;
    m_submitMutationId.clear();
    m_submitOperation.resolve(mutationId, result);
    emit submitCompleted(itemId, result);
}

void MediaTargetFlowViewModel::handleFailed(
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
    if (operation == MediaPort::Operation::LoadPlaylistTargets) {
        if (itemId != m_itemId || m_itemId.isEmpty()
            || requestId != m_loadRequestId)
            return;
        const QString key = targetsResourceKey(m_itemId);
        if (!m_targetsState.reject(m_targetsState.requestId(), key, 0,
                m_requestGeneration, message)) {
            return;
        }
        m_loadRequestId.clear();
        emit loadFailed(itemId, message, nonModal);
        return;
    }
    if (operation != MediaPort::Operation::AddToPlaylist
        || itemId != m_itemId || !m_submitOperation.busy()
        || requestId != m_submitMutationId) {
        return;
    }
    const QString mutationId = m_submitMutationId;
    m_submitMutationId.clear();
    m_submitOperation.reject(mutationId, message);
    emit submitFailed(itemId, message, nonModal);
}
