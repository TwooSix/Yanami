#include "ImageEditorViewModel.hpp"

#include "BackendPorts.hpp"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMetaType>
#include <QScopedValueRollback>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

const QString imageTypeKey = QStringLiteral("imageType");
const QString imageIndexKey = QStringLiteral("imageIndex");
const QString modeKey = QStringLiteral("mode");
const QString requestIdKey = QStringLiteral("requestId");
const QString resourceKeyKey = QStringLiteral("resourceKey");
const QString sessionGenerationKey = QStringLiteral("sessionGeneration");
const QString viewGenerationKey = QStringLiteral("viewGeneration");
const QString searchGenerationKey = QStringLiteral("searchGeneration");
const QString mutationIdKey = QStringLiteral("mutationId");

QString normalizedImageType(const QVariant &value)
{
    const QString type = value.toString().trimmed();
    return type.isEmpty() ? QStringLiteral("Primary") : type;
}

QString normalizedMode(const QVariant &value)
{
    const QString mode = value.toString().trimmed().toLower();
    return mode.isEmpty() ? QStringLiteral("replace") : mode;
}

} // namespace

ImageEditorViewModel::ImageEditorViewModel(QObject *parent)
    : ImageEditorViewModel(nullptr, parent)
{
}

ImageEditorViewModel::ImageEditorViewModel(MediaPort *port, QObject *parent)
    : QObject(parent)
    , m_port(port)
    , m_initial(this)
    , m_search(this)
{
    rebuildModels();
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

void ImageEditorViewModel::setSessionGeneration(quint64 generation)
{
    if (m_sessionGeneration == generation)
        return;

    if (m_opened)
        dismiss();
    else {
        // Invalidate results which may have been detached from a previously
        // closed view before the session changed.
        m_initial.detach();
        m_search.detach();
        ++m_viewGeneration;
        ++m_searchGeneration;
        emit identityChanged();
        emit searchGenerationChanged();
    }
    m_sessionGeneration = generation;
    emit sessionGenerationChanged();
}

void ImageEditorViewModel::setPendingContext(const QVariantMap &context)
{
    const QVariantMap normalized = normalizedTargetContext(context);
    if (sameTargetContext(m_pendingContext, normalized))
        return;
    m_pendingContext = normalized;
    emit pendingContextChanged();
}

QVariantMap ImageEditorViewModel::open(const QVariantMap &item)
{
    const QString newItemId = item.value(QStringLiteral("id")).toString().trimmed();
    if (newItemId.isEmpty())
        return {};

    // Opening A and then B invalidates A even when its transport cannot be
    // cancelled. A fresh view generation is deliberately used on every open,
    // including reopening the same item from the in-memory cache.
    ++m_viewGeneration;
    ++m_searchGeneration;
    m_opened = true;
    m_itemId = newItemId;
    m_resourceKey = QStringLiteral("images:") + m_itemId;
    m_pendingContext.clear();
    m_mutations.clear();
    m_mutationStates.clear();
    m_reconciliationContexts.clear();
    m_pendingMutationContexts.clear();
    m_search.detach();

    m_editor = item;
    m_editor.insert(QStringLiteral("id"), m_itemId);
    m_editor.insert(QStringLiteral("images"), QVariantList{});
    m_editor.insert(QStringLiteral("providers"), QVariantList{});
    rebuildModels();

    requestInitial(false);
    requestProviders();
    const QVariantMap context = m_initialRequestContext;

    emit openedChanged();
    emit identityChanged();
    emit searchGenerationChanged();
    emit pendingContextChanged();
    emit mutationStatesChanged();
    emit editorChanged();
    return context;
}

void ImageEditorViewModel::dismiss()
{
    ++m_viewGeneration;
    ++m_searchGeneration;
    const bool wasOpened = m_opened;
    m_opened = false;
    m_pendingContext.clear();
    m_mutations.clear();
    m_mutationStates.clear();
    m_initialRequestContext.clear();
    m_searchRequestContext.clear();
    m_initialTransportRequestId.clear();
    m_providerRequestId.clear();
    m_searchTransportRequestId.clear();
    m_reconciliationContexts.clear();
    m_pendingMutationContexts.clear();
    m_initial.detach();
    m_search.detach();

    if (wasOpened)
        emit openedChanged();
    emit identityChanged();
    emit searchGenerationChanged();
    emit pendingContextChanged();
    emit mutationStatesChanged();
}

void ImageEditorViewModel::retry()
{
    if (m_search.phase() == AsyncResourceState::Phase::Error
        || !m_search.errorMessage().isEmpty()) {
        m_search.retry();
        if (!m_searchRequestContext.isEmpty() && m_port) {
            const QVariantMap filters = m_searchRequestContext
                .value(QStringLiteral("filters")).toMap();
            search(filters);
        }
        return;
    }
    m_initial.retry();
    requestInitial(m_initial.hasData());
    requestProviders();
}

bool ImageEditorViewModel::applyEditor(
    const QVariantMap &editor,
    const QVariantMap &requestContext)
{
    if (!responseMatchesCurrentView(requestContext))
        return false;
    const QString responseItemId = editor.value(QStringLiteral("id")).toString().trimmed();
    if (responseItemId != m_itemId)
        return false;

    QVariantMap mergedEditor = editor;
    if (!mergedEditor.contains(QStringLiteral("providers"))) {
        mergedEditor.insert(QStringLiteral("providers"),
            m_editor.value(QStringLiteral("providers")).toList());
    }
    const quint64 requestId = requestContext.value(requestIdKey).toULongLong();
    if (!m_initial.resolve(
            requestId,
            requestContext.value(resourceKeyKey).toString(),
            requestContext.value(sessionGenerationKey).toULongLong(),
            requestContext.value(viewGenerationKey).toULongLong(),
            mergedEditor)) {
        return false;
    }

    m_editor = mergedEditor;
    rebuildModels();
    emit editorChanged();
    return true;
}

bool ImageEditorViewModel::failInitial(
    const QString &message,
    const QVariantMap &requestContext)
{
    if (!responseMatchesCurrentView(requestContext))
        return false;
    return m_initial.reject(
        requestContext.value(requestIdKey).toULongLong(),
        requestContext.value(resourceKeyKey).toString(),
        requestContext.value(sessionGenerationKey).toULongLong(),
        requestContext.value(viewGenerationKey).toULongLong(),
        message);
}

QVariantMap ImageEditorViewModel::selectTarget(
    const QString &imageType,
    const QVariant &imageIndex,
    const QString &mode)
{
    const QVariantMap context{
        {imageTypeKey, imageType},
        {imageIndexKey, imageIndex},
        {modeKey, mode},
    };
    setPendingContext(context);
    return m_pendingContext;
}

void ImageEditorViewModel::clearPendingContext()
{
    if (m_pendingContext.isEmpty())
        return;
    m_pendingContext.clear();
    emit pendingContextChanged();
}

QVariantMap ImageEditorViewModel::beginSearch(const QVariantMap &filters)
{
    if (!m_opened || m_pendingContext.isEmpty())
        return {};

    ++m_searchGeneration;
    const QString key = searchResourceKey(filters);
    const quint64 requestId = m_search.begin(
        key,
        m_sessionGeneration,
        m_viewGeneration,
        false);
    QVariantMap context = requestIdentity(
        requestId,
        key,
        m_sessionGeneration,
        m_viewGeneration);
    context.insert(searchGenerationKey, m_searchGeneration);
    context.insert(QStringLiteral("itemId"), m_itemId);
    context.insert(QStringLiteral("target"), m_pendingContext);
    context.insert(QStringLiteral("filters"), filters);
    emit searchGenerationChanged();
    return context;
}

QVariantMap ImageEditorViewModel::search(const QVariantMap &filters)
{
    const QVariantMap context = beginSearch(filters);
    if (context.isEmpty())
        return context;
    m_searchRequestContext = context;
    m_searchTransportRequestId = QStringLiteral("image-search.%1.%2.%3")
        .arg(m_itemId)
        .arg(m_viewGeneration)
        .arg(m_searchGeneration);
    if (m_port) {
        QScopedValueRollback dispatchGuard(
            m_transportDispatchDepth, m_transportDispatchDepth + 1);
        m_port->searchImages(
            m_searchTransportRequestId,
            m_itemId,
            context.value(QStringLiteral("target")).toMap()
                .value(imageTypeKey).toString(),
            filters.value(QStringLiteral("providerName")).toString(),
            filters.value(QStringLiteral("includeAllLanguages")).toBool(),
            filters.value(QStringLiteral("enableSeriesImages")).toBool(),
            m_viewGeneration,
            m_searchGeneration,
            filters.value(QStringLiteral("startIndex"), 0).toInt(),
            filters.value(QStringLiteral("limit"), 36).toInt());
    }
    return context;
}

bool ImageEditorViewModel::applySearch(
    const QVariantMap &result,
    const QVariantMap &requestContext)
{
    if (!searchResponseMatches(requestContext, result))
        return false;
    return m_search.resolve(
        requestContext.value(requestIdKey).toULongLong(),
        requestContext.value(resourceKeyKey).toString(),
        requestContext.value(sessionGenerationKey).toULongLong(),
        requestContext.value(viewGenerationKey).toULongLong(),
        result);
}

bool ImageEditorViewModel::failSearch(
    const QString &message,
    const QVariantMap &requestContext)
{
    if (!searchResponseMatches(requestContext, {}))
        return false;
    return m_search.reject(
        requestContext.value(requestIdKey).toULongLong(),
        requestContext.value(resourceKeyKey).toString(),
        requestContext.value(sessionGenerationKey).toULongLong(),
        requestContext.value(viewGenerationKey).toULongLong(),
        message);
}

void ImageEditorViewModel::cancelSearch()
{
    ++m_searchGeneration;
    m_search.detach();
    m_searchRequestContext.clear();
    m_searchTransportRequestId.clear();
    emit searchGenerationChanged();
}

QVariantMap ImageEditorViewModel::beginMutation(
    const QString &kind,
    const QVariantMap &targetContext,
    const QVariantMap &optimisticImage)
{
    if (!m_opened)
        return {};

    const QVariantMap context = normalizedTargetContext(
        targetContext.isEmpty() ? m_pendingContext : targetContext);
    if (context.isEmpty())
        return {};
    const QString key = cardKey(context);
    if (m_mutations.contains(key))
        return {};

    const QString normalizedKind = kind.trimmed().toLower();
    if (normalizedKind != QStringLiteral("apply")
        && normalizedKind != QStringLiteral("upload")
        && normalizedKind != QStringLiteral("delete")) {
        return {};
    }

    MutationRecord record;
    record.mutationId = ++m_nextMutationId;
    record.cardKey = key;
    record.kind = normalizedKind;
    record.context = context;
    record.resourceKey = QStringLiteral("image-mutation:") + m_itemId + QLatin1Char(':') + key;

    QVariantList images = editorImages(m_editor);
    const int targetPosition = findTargetImage(images, context);
    if (targetPosition >= 0) {
        record.hadPreviousImage = true;
        record.previousImage = images.at(targetPosition).toMap();
        record.previousPosition = targetPosition;
    }

    if (normalizedKind == QStringLiteral("delete")) {
        if (targetPosition < 0)
            return {};
        images.removeAt(targetPosition);
    } else {
        // A remote result may only carry a new URL while an upload may carry
        // no dimensions yet. Preserve the known card metadata until the
        // authoritative reconciliation replaces it.
        QVariantMap replacement = record.previousImage;
        for (auto iterator = optimisticImage.cbegin();
             iterator != optimisticImage.cend(); ++iterator) {
            replacement.insert(iterator.key(), iterator.value());
        }
        replacement.insert(imageTypeKey, context.value(imageTypeKey));
        replacement.insert(imageIndexKey, context.value(imageIndexKey));
        record.optimisticImage = replacement;
        record.insertedOptimisticImage = true;
        if (targetPosition >= 0)
            images[targetPosition] = replacement;
        else
            images.append(replacement);
    }

    m_editor.insert(QStringLiteral("images"), images);
    m_mutations.insert(key, record);
    setMutationState(key, mutationState(record, QStringLiteral("submitting")));
    rebuildModels();
    emit editorChanged();

    QVariantMap requestContext = requestIdentity(
        record.mutationId,
        record.resourceKey,
        m_sessionGeneration,
        m_viewGeneration);
    requestContext.insert(mutationIdKey, record.mutationId);
    requestContext.insert(QStringLiteral("itemId"), m_itemId);
    requestContext.insert(QStringLiteral("kind"), record.kind);
    requestContext.insert(imageTypeKey, context.value(imageTypeKey));
    requestContext.insert(imageIndexKey, context.value(imageIndexKey));
    requestContext.insert(modeKey, context.value(modeKey));
    return requestContext;
}

QVariantMap ImageEditorViewModel::applyRemote(
    const QVariantMap &targetContext,
    const QVariantMap &remoteImage)
{
    const QVariantMap context = beginMutation(
        QStringLiteral("apply"), targetContext, remoteImage);
    if (!context.isEmpty() && m_port) {
        const QString requestId = QStringLiteral("image-apply.%1.%2")
            .arg(m_itemId)
            .arg(context.value(mutationIdKey).toULongLong());
        m_pendingMutationContexts.insert(requestId, context);
        QScopedValueRollback dispatchGuard(
            m_transportDispatchDepth, m_transportDispatchDepth + 1);
        m_port->applyRemoteImage(
            requestId,
            m_itemId,
            context.value(imageTypeKey).toString(),
            QUrl(remoteImage.value(QStringLiteral("imageUrl")).toString()),
            remoteImage.value(QStringLiteral("providerName")).toString(),
            context.value(imageIndexKey));
    }
    return context;
}

QVariantMap ImageEditorViewModel::upload(
    const QVariantMap &targetContext,
    const QString &fileUrl)
{
    const QVariantMap context = beginMutation(
        QStringLiteral("upload"),
        targetContext,
        QVariantMap{{QStringLiteral("previewUrl"), fileUrl}});
    if (!context.isEmpty() && m_port) {
        const QString requestId = QStringLiteral("image-upload.%1.%2")
            .arg(m_itemId)
            .arg(context.value(mutationIdKey).toULongLong());
        m_pendingMutationContexts.insert(requestId, context);
        QScopedValueRollback dispatchGuard(
            m_transportDispatchDepth, m_transportDispatchDepth + 1);
        m_port->uploadImage(
            requestId,
            m_itemId,
            context.value(imageTypeKey).toString(),
            QUrl(fileUrl),
            context.value(imageIndexKey));
    }
    return context;
}

QVariantMap ImageEditorViewModel::remove(const QVariantMap &targetContext)
{
    const QVariantMap context = beginMutation(
        QStringLiteral("delete"), targetContext, {});
    if (!context.isEmpty() && m_port) {
        const QString requestId = QStringLiteral("image-delete.%1.%2")
            .arg(m_itemId)
            .arg(context.value(mutationIdKey).toULongLong());
        m_pendingMutationContexts.insert(requestId, context);
        QScopedValueRollback dispatchGuard(
            m_transportDispatchDepth, m_transportDispatchDepth + 1);
        m_port->removeImage(
            requestId,
            m_itemId,
            context.value(imageTypeKey).toString(),
            context.value(imageIndexKey));
    }
    return context;
}

bool ImageEditorViewModel::mutationSucceeded(const QVariantMap &requestContext)
{
    const QVariantMap context = normalizedTargetContext(requestContext);
    const QString key = cardKey(context);
    const auto iterator = m_mutations.constFind(key);
    if (iterator == m_mutations.cend()
        || !mutationResponseMatches(iterator.value(), requestContext)) {
        return false;
    }

    const MutationRecord record = iterator.value();
    m_mutations.remove(key);
    setMutationState(key, mutationState(record, QStringLiteral("refreshing")));
    rebuildModels();
    return true;
}

bool ImageEditorViewModel::mutationFailed(
    const QVariantMap &requestContext,
    const QString &message)
{
    const QVariantMap context = normalizedTargetContext(requestContext);
    const QString key = cardKey(context);
    const auto iterator = m_mutations.constFind(key);
    if (iterator == m_mutations.cend()
        || !mutationResponseMatches(iterator.value(), requestContext)) {
        return false;
    }

    const MutationRecord record = iterator.value();
    QVariantList images = editorImages(m_editor);
    if (record.insertedOptimisticImage) {
        const int optimisticPosition = findImageIdentity(images, record.optimisticImage);
        if (optimisticPosition >= 0)
            images.removeAt(optimisticPosition);
    }
    if (record.hadPreviousImage) {
        const int insertionPoint = std::clamp(
            record.previousPosition,
            0,
            static_cast<int>(images.size()));
        images.insert(insertionPoint, record.previousImage);
    }

    m_editor.insert(QStringLiteral("images"), images);
    m_mutations.remove(key);
    setMutationState(key, mutationState(record, QStringLiteral("error"), message));
    rebuildModels();
    emit editorChanged();
    return true;
}

void ImageEditorViewModel::mutationReconciled(const QVariantMap &requestContext)
{
    const QVariantMap context = normalizedTargetContext(requestContext);
    const QString key = cardKey(context);
    const QVariantMap state = m_mutationStates.value(key).toMap();
    if (state.value(mutationIdKey).toULongLong()
        != requestContext.value(mutationIdKey).toULongLong()) {
        return;
    }
    clearMutationState(context);
}

void ImageEditorViewModel::clearMutationState(const QVariantMap &targetContext)
{
    const QString key = cardKey(normalizedTargetContext(targetContext));
    if (key.isEmpty() || !m_mutationStates.contains(key))
        return;
    m_mutationStates.remove(key);
    emit mutationStatesChanged();
    rebuildModels();
}

void ImageEditorViewModel::requestInitial(bool preserveData)
{
    const quint64 requestId = m_initial.begin(
        m_resourceKey,
        m_sessionGeneration,
        m_viewGeneration,
        preserveData);
    m_initialRequestContext = requestIdentity(
        requestId,
        m_resourceKey,
        m_sessionGeneration,
        m_viewGeneration);
    m_initialTransportRequestId = QStringLiteral("image-load.%1.%2.%3")
        .arg(m_itemId)
        .arg(m_viewGeneration)
        .arg(requestId);
    if (m_port) {
        QScopedValueRollback dispatchGuard(
            m_transportDispatchDepth, m_transportDispatchDepth + 1);
        m_port->loadImages(
            m_initialTransportRequestId, m_itemId, m_viewGeneration);
    }
}

void ImageEditorViewModel::requestProviders()
{
    if (!m_port || !m_opened)
        return;
    m_providerRequestId = QStringLiteral("image-providers.%1.%2.%3")
        .arg(m_itemId)
        .arg(m_viewGeneration)
        .arg(++m_nextProviderRequestId);
    QScopedValueRollback dispatchGuard(
        m_transportDispatchDepth, m_transportDispatchDepth + 1);
    m_port->loadImageProviders(
        m_providerRequestId, m_itemId, m_viewGeneration);
}

void ImageEditorViewModel::reconcileMutations()
{
    if (!m_opened || m_reconciliationContexts.isEmpty())
        return;
    requestInitial(true);
}

void ImageEditorViewModel::handleCompleted(
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
    if (!m_opened || itemId != m_itemId)
        return;

    if (operation == MediaPort::Operation::LoadImages) {
        if (requestId != m_initialTransportRequestId)
            return;
        if (!applyEditor(result, m_initialRequestContext))
            return;
        m_initialTransportRequestId.clear();
        const QList<QVariantMap> reconciled = m_reconciliationContexts;
        m_reconciliationContexts.clear();
        for (const QVariantMap &context : reconciled)
            mutationReconciled(context);
        emit editorReady(m_editor, m_viewGeneration);
        return;
    }

    if (operation == MediaPort::Operation::LoadImageProviders) {
        if (requestId != m_providerRequestId
            || result.value(QStringLiteral("clientViewGeneration"), m_viewGeneration)
                   .toULongLong() != m_viewGeneration) {
            return;
        }
        m_editor.insert(
            QStringLiteral("providers"),
            result.value(QStringLiteral("providers")).toList());
        m_providerRequestId.clear();
        emit editorChanged();
        emit providersReady(result, m_viewGeneration);
        return;
    }

    if (operation == MediaPort::Operation::SearchImages) {
        if (requestId != m_searchTransportRequestId
            || !applySearch(result, m_searchRequestContext)) {
            return;
        }
        m_searchTransportRequestId.clear();
        emit searchReady(result, m_searchGeneration);
        return;
    }

    if (operation != MediaPort::Operation::ApplyRemoteImage
        && operation != MediaPort::Operation::UploadImage
        && operation != MediaPort::Operation::RemoveImage) {
        return;
    }
    const QVariantMap context = m_pendingMutationContexts.take(requestId);
    if (context.isEmpty() || !mutationSucceeded(context))
        return;
    m_reconciliationContexts.append(context);
    emit mutationCommitted(context);
    reconcileMutations();
}

void ImageEditorViewModel::handleFailed(
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
    if (!m_opened || itemId != m_itemId)
        return;

    if (operation == MediaPort::Operation::LoadImages) {
        if (requestId != m_initialTransportRequestId)
            return;
        const bool reconciling = !m_reconciliationContexts.isEmpty();
        if (!failInitial(message, m_initialRequestContext))
            return;
        m_initialTransportRequestId.clear();
        for (const QVariantMap &context : std::as_const(m_reconciliationContexts)) {
            const QString key = cardKey(context);
            QVariantMap state = m_mutationStates.value(key).toMap();
            state.insert(QStringLiteral("phase"), QStringLiteral("error"));
            state.insert(QStringLiteral("errorMessage"), message);
            setMutationState(key, state);
        }
        m_reconciliationContexts.clear();
        if (reconciling)
            emit reconciliationRequestFailed(itemId, message, nonModal);
        else
            emit editorRequestFailed(itemId, message, nonModal);
        return;
    }
    if (operation == MediaPort::Operation::LoadImageProviders) {
        if (requestId != m_providerRequestId)
            return;
        m_providerRequestId.clear();
        emit providersRequestFailed(itemId, message, nonModal);
        return;
    }
    if (operation == MediaPort::Operation::SearchImages) {
        if (requestId != m_searchTransportRequestId)
            return;
        if (!failSearch(message, m_searchRequestContext))
            return;
        m_searchTransportRequestId.clear();
        emit searchRequestFailed(itemId, message, nonModal);
        return;
    }

    if (operation != MediaPort::Operation::ApplyRemoteImage
        && operation != MediaPort::Operation::UploadImage
        && operation != MediaPort::Operation::RemoveImage) {
        return;
    }

    const QVariantMap context = m_pendingMutationContexts.take(requestId);
    if (context.isEmpty())
        return;
    if (!mutationFailed(context, message))
        return;
    // A newer image mutation fences any authoritative reload started by an
    // earlier success. If that newer mutation then fails, issue a fresh reload
    // so the earlier committed mutation cannot remain permanently refreshing.
    if (!m_reconciliationContexts.isEmpty())
        reconcileMutations();
    emit mutationRequestFailed(itemId, message, nonModal);
}

QString ImageEditorViewModel::cardKey(const QVariantMap &targetContext) const
{
    const QVariantMap context = normalizedTargetContext(targetContext);
    if (context.isEmpty())
        return {};
    const QVariant index = normalizedImageIndex(context.value(imageIndexKey));
    return context.value(imageTypeKey).toString()
        + QLatin1Char('/')
        + (index.isValid() ? QString::number(index.toLongLong()) : QStringLiteral("canonical"));
}

const QStringList &ImageEditorViewModel::ordinaryImageTypes()
{
    static const QStringList types{
        QStringLiteral("Primary"),
        QStringLiteral("Logo"),
        QStringLiteral("Thumb"),
        QStringLiteral("Banner"),
        QStringLiteral("Disc"),
        QStringLiteral("Art"),
    };
    return types;
}

QVariant ImageEditorViewModel::normalizedImageIndex(const QVariant &value)
{
    if (!value.isValid() || value.isNull() || value.metaType().id() == QMetaType::Bool)
        return {};
    if (value.metaType().id() == QMetaType::QString && value.toString().trimmed().isEmpty())
        return {};

    bool ok = false;
    const double number = value.toDouble(&ok);
    if (!ok || !std::isfinite(number) || number < 0.0
        || std::floor(number) != number
        || number > static_cast<double>(std::numeric_limits<qint64>::max())) {
        return {};
    }
    return QVariant::fromValue(static_cast<qint64>(number));
}

bool ImageEditorViewModel::sameImageIndex(const QVariant &left, const QVariant &right)
{
    const QVariant normalizedLeft = normalizedImageIndex(left);
    const QVariant normalizedRight = normalizedImageIndex(right);
    if (!normalizedLeft.isValid() || !normalizedRight.isValid())
        return !normalizedLeft.isValid() && !normalizedRight.isValid();
    return normalizedLeft.toLongLong() == normalizedRight.toLongLong();
}

bool ImageEditorViewModel::imageComesBefore(const QVariantMap &left, const QVariantMap &right)
{
    const QVariant leftIndex = normalizedImageIndex(left.value(imageIndexKey));
    const QVariant rightIndex = normalizedImageIndex(right.value(imageIndexKey));
    if (!leftIndex.isValid() || !rightIndex.isValid())
        return !leftIndex.isValid() && rightIndex.isValid();
    return leftIndex.toLongLong() < rightIndex.toLongLong();
}

QVariantMap ImageEditorViewModel::normalizedTargetContext(const QVariantMap &context)
{
    if (context.isEmpty())
        return {};
    return QVariantMap{
        {imageTypeKey, normalizedImageType(context.value(imageTypeKey))},
        {imageIndexKey, normalizedImageIndex(context.value(imageIndexKey))},
        {modeKey, normalizedMode(context.value(modeKey))},
    };
}

bool ImageEditorViewModel::sameTargetContext(
    const QVariantMap &left,
    const QVariantMap &right)
{
    if (left.isEmpty() || right.isEmpty())
        return left.isEmpty() && right.isEmpty();
    return normalizedImageType(left.value(imageTypeKey))
            == normalizedImageType(right.value(imageTypeKey))
        && sameImageIndex(left.value(imageIndexKey), right.value(imageIndexKey))
        && normalizedMode(left.value(modeKey)) == normalizedMode(right.value(modeKey));
}

QVariantList ImageEditorViewModel::editorImages(const QVariantMap &editor)
{
    return editor.value(QStringLiteral("images")).toList();
}

QVariantMap ImageEditorViewModel::requestIdentity(
    quint64 requestId,
    const QString &resourceKey,
    quint64 sessionGeneration,
    quint64 viewGeneration)
{
    return QVariantMap{
        {requestIdKey, requestId},
        {resourceKeyKey, resourceKey},
        {sessionGenerationKey, sessionGeneration},
        {viewGenerationKey, viewGeneration},
    };
}

bool ImageEditorViewModel::responseMatchesCurrentView(
    const QVariantMap &requestContext) const
{
    return m_opened
        && requestContext.value(requestIdKey).toULongLong() != 0
        && requestContext.value(resourceKeyKey).toString() == m_resourceKey
        && requestContext.value(sessionGenerationKey).toULongLong() == m_sessionGeneration
        && requestContext.value(viewGenerationKey).toULongLong() == m_viewGeneration;
}

bool ImageEditorViewModel::searchResponseMatches(
    const QVariantMap &requestContext,
    const QVariantMap &result) const
{
    if (!m_opened
        || requestContext.value(requestIdKey).toULongLong() == 0
        || requestContext.value(sessionGenerationKey).toULongLong() != m_sessionGeneration
        || requestContext.value(viewGenerationKey).toULongLong() != m_viewGeneration
        || requestContext.value(searchGenerationKey).toULongLong() != m_searchGeneration
        || !sameTargetContext(requestContext.value(QStringLiteral("target")).toMap(),
            m_pendingContext)) {
        return false;
    }
    const QString resultType = result.value(imageTypeKey).toString().trimmed();
    return resultType.isEmpty()
        || resultType == m_pendingContext.value(imageTypeKey).toString();
}

bool ImageEditorViewModel::mutationResponseMatches(
    const MutationRecord &record,
    const QVariantMap &requestContext) const
{
    return m_opened
        && requestContext.value(requestIdKey).toULongLong() == record.mutationId
        && requestContext.value(mutationIdKey).toULongLong() == record.mutationId
        && requestContext.value(resourceKeyKey).toString() == record.resourceKey
        && requestContext.value(sessionGenerationKey).toULongLong() == m_sessionGeneration
        && requestContext.value(viewGenerationKey).toULongLong() == m_viewGeneration
        && sameTargetContext(requestContext, record.context);
}

int ImageEditorViewModel::findTargetImage(
    const QVariantList &images,
    const QVariantMap &context) const
{
    const QString type = context.value(imageTypeKey).toString();
    const QVariant requestedIndex = normalizedImageIndex(context.value(imageIndexKey));
    int canonical = -1;
    for (int index = 0; index < images.size(); ++index) {
        const QVariantMap image = images.at(index).toMap();
        if (image.value(imageTypeKey).toString() != type)
            continue;
        if (requestedIndex.isValid()) {
            if (sameImageIndex(image.value(imageIndexKey), requestedIndex))
                return index;
            continue;
        }
        if (canonical < 0
            || imageComesBefore(image, images.at(canonical).toMap())) {
            canonical = index;
        }
    }
    return canonical;
}

int ImageEditorViewModel::findImageIdentity(
    const QVariantList &images,
    const QVariantMap &image) const
{
    for (int index = 0; index < images.size(); ++index) {
        const QVariantMap candidate = images.at(index).toMap();
        if (candidate.value(imageTypeKey).toString() == image.value(imageTypeKey).toString()
            && sameImageIndex(candidate.value(imageIndexKey), image.value(imageIndexKey))) {
            return index;
        }
    }
    return -1;
}

QString ImageEditorViewModel::searchResourceKey(const QVariantMap &filters) const
{
    const QVariantMap identity{
        {QStringLiteral("itemId"), m_itemId},
        {QStringLiteral("target"), m_pendingContext},
        {QStringLiteral("filters"), filters},
    };
    const QByteArray canonical = QJsonDocument::fromVariant(identity).toJson(
        QJsonDocument::Compact);
    return QStringLiteral("image-search:")
        + QString::fromLatin1(QCryptographicHash::hash(
            canonical,
            QCryptographicHash::Sha256).toHex());
}

QVariantMap ImageEditorViewModel::mutationState(
    const MutationRecord &record,
    const QString &phase,
    const QString &errorMessage) const
{
    return QVariantMap{
        {QStringLiteral("phase"), phase},
        {QStringLiteral("kind"), record.kind},
        {imageTypeKey, record.context.value(imageTypeKey)},
        {imageIndexKey, record.context.value(imageIndexKey)},
        {modeKey, record.context.value(modeKey)},
        {mutationIdKey, record.mutationId},
        {QStringLiteral("errorMessage"), errorMessage.trimmed()},
    };
}

void ImageEditorViewModel::setMutationState(
    const QString &key,
    const QVariantMap &state)
{
    m_mutationStates.insert(key, state);
    emit mutationStatesChanged();
}

void ImageEditorViewModel::rebuildModels()
{
    const QVariantList images = editorImages(m_editor);
    QVariantList fixedSlots;
    fixedSlots.reserve(ordinaryImageTypes().size());

    for (const QString &type : ordinaryImageTypes()) {
        QVariantList matching;
        for (const QVariant &value : images) {
            const QVariantMap image = value.toMap();
            if (image.value(imageTypeKey).toString() == type)
                matching.append(image);
        }
        std::stable_sort(
            matching.begin(),
            matching.end(),
            [](const QVariant &left, const QVariant &right) {
                return imageComesBefore(left.toMap(), right.toMap());
            });

        const QVariantMap canonical = matching.isEmpty()
            ? QVariantMap{}
            : matching.constFirst().toMap();
        const QVariant targetIndex = canonical.isEmpty()
            ? QVariant{}
            : normalizedImageIndex(canonical.value(imageIndexKey));
        const QVariantMap target{
            {imageTypeKey, type},
            {imageIndexKey, targetIndex},
            {modeKey, canonical.isEmpty() ? QStringLiteral("add") : QStringLiteral("replace")},
        };
        const QString key = cardKey(target);
        fixedSlots.append(QVariantMap{
            {imageTypeKey, type},
            {QStringLiteral("hasImage"), !canonical.isEmpty()},
            {QStringLiteral("count"), matching.size()},
            {QStringLiteral("extraCount"), std::max<qsizetype>(0, matching.size() - 1)},
            {QStringLiteral("hasMultiple"), matching.size() > 1},
            {QStringLiteral("image"), canonical},
            {QStringLiteral("canonicalImage"), canonical},
            {imageIndexKey, targetIndex},
            {QStringLiteral("images"), matching},
            {QStringLiteral("targetContext"), target},
            {QStringLiteral("mutationState"), m_mutationStates.value(key).toMap()},
        });
    }

    QVariantList backdrops;
    for (const QVariant &value : images) {
        QVariantMap image = value.toMap();
        if (image.value(imageTypeKey).toString() != QStringLiteral("Backdrop"))
            continue;
        backdrops.append(image);
    }
    std::stable_sort(
        backdrops.begin(),
        backdrops.end(),
        [](const QVariant &left, const QVariant &right) {
            return imageComesBefore(left.toMap(), right.toMap());
        });

    QVariantList decoratedBackdrops;
    decoratedBackdrops.reserve(backdrops.size());
    QHash<qint64, bool> usedIndices;
    for (int position = 0; position < backdrops.size(); ++position) {
        QVariantMap image = backdrops.at(position).toMap();
        const QVariant index = normalizedImageIndex(image.value(imageIndexKey));
        const qint64 occupiedIndex = index.isValid() ? index.toLongLong() : position;
        usedIndices.insert(occupiedIndex, true);
        const QVariantMap target{
            {imageTypeKey, QStringLiteral("Backdrop")},
            {imageIndexKey, index},
            {modeKey, QStringLiteral("replace")},
        };
        image.insert(QStringLiteral("targetContext"), target);
        image.insert(
            QStringLiteral("mutationState"),
            m_mutationStates.value(cardKey(target)).toMap());
        decoratedBackdrops.append(image);
    }

    int nextBackdrop = 0;
    while (usedIndices.contains(nextBackdrop))
        ++nextBackdrop;

    m_slotsModel = fixedSlots;
    m_backdropsModel = decoratedBackdrops;
    m_nextBackdropIndex = nextBackdrop;
    emit modelsChanged();
}
