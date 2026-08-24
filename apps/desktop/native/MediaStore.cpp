#include "MediaStore.hpp"

#include <QBitArray>
#include <QCollator>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QVarLengthArray>

#include <algorithm>
#include <numeric>
#include <utility>

namespace {

const QString libraryKind = QStringLiteral("library");
const QString viewsKind = QStringLiteral("views");
const QString resumeKind = QStringLiteral("resume");
const QString recentKind = QStringLiteral("recent");
const QString favoritesKind = QStringLiteral("favorites");
const QString collectionKind = QStringLiteral("collection");

const QSet<QString> decorationKeys {
    QStringLiteral("playlistEntryId"),
    QStringLiteral("subtitle"),
    QStringLiteral("hasLatestEpisode"),
    QStringLiteral("continueLabel"),
};

const QSet<QString> versionedMetadataKeys {
    QStringLiteral("title"),
    QStringLiteral("overview"),
    QStringLiteral("imageUrl"),
    QStringLiteral("imageItemId"),
    QStringLiteral("imageItemType"),
    QStringLiteral("imageTag"),
    QStringLiteral("primaryImageAspectRatio"),
    QStringLiteral("backdropUrl"),
    QStringLiteral("productionYear"),
    QStringLiteral("itemType"),
    QStringLiteral("seriesId"),
    QStringLiteral("seriesTitle"),
    QStringLiteral("seasonId"),
    QStringLiteral("childCount"),
    QStringLiteral("collectionType"),
    QStringLiteral("providerIds"),
    QStringLiteral("sourceUpdatedAt"),
    QStringLiteral("sourceVersion"),
};

quint64 searchGramKey(QStringView text, qsizetype offset, qsizetype length)
{
    quint64 key = static_cast<quint64>(length) << 48;
    for (qsizetype index = 0; index < length; ++index) {
        const int shift = static_cast<int>((2 - index) * 16);
        key |= static_cast<quint64>(text.at(offset + index).unicode()) << shift;
    }
    return key;
}

QVector<quint64> searchGramKeys(
    const QPair<QString, QString> &texts)
{
    QVarLengthArray<quint64, 128> keys;
    for (const QString &text : {texts.first, texts.second}) {
        const QStringView view(text);
        for (qsizetype length = 1; length <= 3; ++length) {
            if (view.size() < length)
                continue;
            for (qsizetype offset = 0; offset <= view.size() - length; ++offset)
                keys.push_back(searchGramKey(view, offset, length));
        }
    }
    std::sort(keys.begin(), keys.end());
    const auto uniqueEnd = std::unique(keys.begin(), keys.end());
    QVector<quint64> result;
    result.reserve(std::distance(keys.begin(), uniqueEnd));
    for (auto key = keys.begin(); key != uniqueEnd; ++key)
        result.push_back(*key);
    return result;
}

qint64 dateValue(const QVariant &value)
{
    const QDateTime parsed = QDateTime::fromString(value.toString(), Qt::ISODate);
    return parsed.isValid() ? parsed.toMSecsSinceEpoch() : 0;
}

QString itemCategory(const QVariantMap &item)
{
    const QString type = item.value(QStringLiteral("itemType")).toString();
    if (type == QStringLiteral("Series"))
        return QStringLiteral("series");
    if (type == QStringLiteral("Movie"))
        return QStringLiteral("movies");
    if (type == QStringLiteral("Episode"))
        return QStringLiteral("episodes");
    return QStringLiteral("other");
}

} // namespace

MediaQueryModel::MediaQueryModel(
    MediaStore *store,
    QString kind,
    QString scopeId,
    QObject *parent)
    : QAbstractListModel(parent),
      m_store(store),
      m_kind(std::move(kind)),
      m_scopeId(std::move(scopeId))
{
}

int MediaQueryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant MediaQueryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const MediaQueryRow &row = m_rows.at(index.row());
    switch (role) {
    case ModelDataRole:
        return m_store->materialize(row, m_kind);
    case EntityIdRole:
        return row.entityId;
    case RowKeyRole:
        return row.rowKey;
    default:
        return {};
    }
}

QHash<int, QByteArray> MediaQueryModel::roleNames() const
{
    return {
        {ModelDataRole, QByteArrayLiteral("modelData")},
        {EntityIdRole, QByteArrayLiteral("entityId")},
        {RowKeyRole, QByteArrayLiteral("rowKey")},
    };
}

QVariantMap MediaQueryModel::get(int row) const
{
    return data(index(row), ModelDataRole).toMap();
}

void MediaQueryModel::synchronizeRows(QVector<MediaQueryRow> rows)
{
    if (m_rows == rows) {
        ++m_contentRevision;
        return;
    }
    m_synchronizingRows = true;
    const auto finishSynchronization = [this] {
        ++m_contentRevision;
        m_synchronizingRows = false;
        emit rowsSynchronized();
    };
    if (m_rows.isEmpty()) {
        if (!rows.isEmpty()) {
            beginInsertRows({}, 0, rows.size() - 1);
            m_rows = std::move(rows);
            endInsertRows();
            emit countChanged();
        }
        finishSynchronization();
        return;
    }

    // A server-side sort refresh can reorder the entire library. Preserve
    // persistent indexes by rowKey and publish one layout permutation instead
    // of N linear searches plus QVector moves (quadratic for a reversed list).
    if (m_rows.size() == rows.size()) {
        QHash<QString, int> oldRowsByKey;
        QHash<QString, int> newRowsByKey;
        oldRowsByKey.reserve(m_rows.size());
        newRowsByKey.reserve(rows.size());
        for (int row = 0; row < m_rows.size(); ++row) {
            oldRowsByKey.insert(m_rows.at(row).rowKey, row);
            newRowsByKey.insert(rows.at(row).rowKey, row);
        }
        const bool uniqueKeys = oldRowsByKey.size() == m_rows.size()
            && newRowsByKey.size() == rows.size();
        bool sameKeys = uniqueKeys;
        if (sameKeys) {
            for (auto key = oldRowsByKey.cbegin(); key != oldRowsByKey.cend(); ++key) {
                if (!newRowsByKey.contains(key.key())) {
                    sameKeys = false;
                    break;
                }
            }
        }
        if (sameKeys) {
            bool orderChanged = false;
            for (int row = 0; row < m_rows.size(); ++row) {
                if (m_rows.at(row).rowKey != rows.at(row).rowKey) {
                    orderChanged = true;
                    break;
                }
            }
            if (!orderChanged) {
                m_rows = std::move(rows);
                emit dataChanged(index(0), index(m_rows.size() - 1));
                finishSynchronization();
                return;
            }

            emit layoutAboutToBeChanged(
                {}, QAbstractItemModel::VerticalSortHint);
            const QModelIndexList from = persistentIndexList();
            QModelIndexList to;
            to.reserve(from.size());
            for (const QModelIndex &oldIndex : from) {
                const QString rowKey = oldIndex.row() >= 0
                        && oldIndex.row() < m_rows.size()
                    ? m_rows.at(oldIndex.row()).rowKey
                    : QString{};
                const auto found = newRowsByKey.constFind(rowKey);
                to.push_back(found == newRowsByKey.cend()
                        ? QModelIndex{}
                        : createIndex(found.value(), oldIndex.column()));
            }
            m_rows = std::move(rows);
            changePersistentIndexList(from, to);
            emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
            finishSynchronization();
            return;
        }
    }

    int target = 0;
    while (target < rows.size()) {
        const MediaQueryRow &wanted = rows.at(target);
        int found = -1;
        for (int current = target; current < m_rows.size(); ++current) {
            if (m_rows.at(current).rowKey == wanted.rowKey) {
                found = current;
                break;
            }
        }
        if (found < 0) {
            beginInsertRows({}, target, target);
            m_rows.insert(target, wanted);
            endInsertRows();
            emit countChanged();
        } else {
            if (found != target) {
                beginMoveRows({}, found, found, {}, target);
                m_rows.move(found, target);
                endMoveRows();
            }
            if (m_rows.at(target) != wanted) {
                m_rows[target] = wanted;
                emit dataChanged(index(target), index(target));
            }
        }
        ++target;
    }
    if (m_rows.size() > rows.size()) {
        beginRemoveRows({}, rows.size(), m_rows.size() - 1);
        m_rows.erase(m_rows.begin() + rows.size(), m_rows.end());
        endRemoveRows();
        emit countChanged();
    }
    finishSynchronization();
}

bool MediaQueryModel::filterAccepts(
    int row,
    const QString &needle,
    const QString &category) const
{
    return m_store && row >= 0 && row < m_rows.size()
        && m_store->filterAccepts(m_rows.at(row), m_kind, needle, category);
}

QPair<QString, QString> MediaQueryModel::filterTexts(int row) const
{
    return m_store && row >= 0 && row < m_rows.size()
        ? m_store->filterTexts(m_rows.at(row), m_kind)
        : QPair<QString, QString>{};
}

void MediaQueryModel::notifyEntityChanged(const QString &entityId)
{
    notifyEntitiesChanged(QSet<QString>{entityId});
}

void MediaQueryModel::notifyEntitiesChanged(const QSet<QString> &entityIds)
{
    if (entityIds.isEmpty())
        return;
    int firstChangedRow = -1;
    int lastChangedRow = -1;
    for (int row = 0; row < m_rows.size(); ++row) {
        if (!entityIds.contains(m_rows.at(row).entityId))
            continue;
        if (firstChangedRow < 0)
            firstChangedRow = row;
        lastChangedRow = row;
    }
    if (firstChangedRow >= 0)
        emit dataChanged(index(firstChangedRow), index(lastChangedRow));
}

bool MediaQueryModel::removeFirst(
    const QString &entityId,
    const QString &rowKey,
    int *removedIndex,
    MediaQueryRow *removedRow)
{
    for (int index = 0; index < m_rows.size(); ++index) {
        const MediaQueryRow &candidate = m_rows.at(index);
        if ((!rowKey.isEmpty() && candidate.rowKey != rowKey)
            || (rowKey.isEmpty() && candidate.entityId != entityId)) {
            continue;
        }
        if (removedIndex)
            *removedIndex = index;
        if (removedRow)
            *removedRow = candidate;
        beginRemoveRows({}, index, index);
        m_rows.removeAt(index);
        endRemoveRows();
        ++m_contentRevision;
        emit countChanged();
        return true;
    }
    return false;
}

void MediaQueryModel::restoreRow(int index, const MediaQueryRow &row)
{
    const int insertionIndex = std::clamp(index, 0, static_cast<int>(m_rows.size()));
    if (std::any_of(m_rows.cbegin(), m_rows.cend(), [&row](const MediaQueryRow &candidate) {
            return candidate.rowKey == row.rowKey;
        })) {
        return;
    }
    beginInsertRows({}, insertionIndex, insertionIndex);
    m_rows.insert(insertionIndex, row);
    endInsertRows();
    ++m_contentRevision;
    emit countChanged();
}

void MediaQueryModel::clearRows()
{
    if (m_rows.isEmpty())
        return;
    beginResetModel();
    m_rows.clear();
    m_parentId.clear();
    m_parentDecoration.clear();
    m_fetchedAtMs = 0;
    m_stale = true;
    ++m_contentRevision;
    endResetModel();
    emit countChanged();
    emit stateChanged();
}

MediaStore::MediaStore(QObject *parent)
    : QObject(parent)
{
    ensureQueryModel(libraryKind, {});
    ensureQueryModel(viewsKind, {});
    ensureQueryModel(resumeKind, {});
    ensureQueryModel(recentKind, {});
    ensureQueryModel(favoritesKind, {});
}

MediaQueryModel *MediaStore::libraryModel() const
{
    return m_queries.value(makeQueryKey(libraryKind, {}));
}

MediaQueryModel *MediaStore::libraryViewsModel() const
{
    return m_queries.value(makeQueryKey(viewsKind, {}));
}

MediaQueryModel *MediaStore::resumeModel() const
{
    return m_queries.value(makeQueryKey(resumeKind, {}));
}

MediaQueryModel *MediaStore::recentModel() const
{
    return m_queries.value(makeQueryKey(recentKind, {}));
}

MediaQueryModel *MediaStore::favoritesModel() const
{
    return m_queries.value(makeQueryKey(favoritesKind, {}));
}

MediaQueryModel *MediaStore::queryModel(const QString &kind, const QString &scopeId)
{
    return ensureQueryModel(kind, scopeId);
}

QVariantMap MediaStore::entity(const QString &entityId) const
{
    const auto found = m_entities.constFind(entityId);
    if (found == m_entities.cend())
        return {};
    QVariantMap result = found->fields;
    for (auto overlay = found->overlay.cbegin(); overlay != found->overlay.cend(); ++overlay)
        result.insert(overlay.key(), overlay.value());
    return result;
}

QVariantMap MediaStore::queryParent(const QString &kind, const QString &scopeId) const
{
    const auto model = m_queries.value(makeQueryKey(kind, scopeId));
    if (!model)
        return {};
    QVariantMap result = entity(model->parentId());
    for (auto iterator = model->m_parentDecoration.cbegin();
         iterator != model->m_parentDecoration.cend(); ++iterator) {
        result.insert(iterator.key(), iterator.value());
    }
    return result;
}

bool MediaStore::hasQuery(const QString &kind, const QString &scopeId) const
{
    const auto model = m_queries.value(makeQueryKey(kind, scopeId));
    return model && model->fetchedAtMs() > 0;
}

QVariantList MediaStore::queryItems(const QString &kind, const QString &scopeId) const
{
    const auto model = m_queries.value(makeQueryKey(kind, scopeId));
    QVariantList result;
    if (!model)
        return result;
    result.reserve(model->m_rows.size());
    for (const MediaQueryRow &row : model->m_rows)
        result.push_back(materialize(row, model->kind()));
    return result;
}

qint64 MediaStore::queryFetchedAtMs(const QString &kind, const QString &scopeId) const
{
    const auto model = m_queries.value(makeQueryKey(kind, scopeId));
    return model ? model->fetchedAtMs() : 0;
}

bool MediaStore::queryStale(const QString &kind, const QString &scopeId) const
{
    const auto model = m_queries.value(makeQueryKey(kind, scopeId));
    return !model || model->stale();
}

void MediaStore::setQuery(
    const QString &kind,
    const QString &scopeId,
    const QVariantList &items,
    const QVariantMap &parent,
    qint64 fetchedAtMs)
{
    const QString normalized = normalizedKind(kind);
    MediaQueryModel *model = ensureQueryModel(normalized, scopeId);
    QVector<MediaQueryRow> rows;
    rows.reserve(items.size());
    QHash<QString, int> occurrences;
    occurrences.reserve(items.size());
    if (m_entities.isEmpty())
        m_entities.reserve(items.size());
    beginEntityNotificationBatch();
    for (const QVariant &value : items) {
        const QVariantMap item = value.toMap();
        const QString entityId = item.value(QStringLiteral("id")).toString();
        if (entityId.isEmpty())
            continue;
        QString rowKey = item.value(QStringLiteral("playlistEntryId")).toString();
        if (rowKey.isEmpty()) {
            const int occurrence = occurrences[entityId]++;
            rowKey = occurrence == 0 ? entityId
                                     : QStringLiteral("%1#%2").arg(entityId).arg(occurrence);
        } else {
            rowKey.prepend(QStringLiteral("playlist:"));
        }
        const QVariantMap decoration = rowDecoration(item, normalized);
        rows.push_back(MediaQueryRow{
            rowKey,
            entityId,
            decoration,
        });
        patchEntity(canonicalFields(item, normalized));
    }
    QString parentId;
    if (!parent.isEmpty()) {
        parentId = parent.value(QStringLiteral("id")).toString();
        patchEntity(canonicalFields(parent, collectionKind));
    }
    endEntityNotificationBatch();
    model->synchronizeRows(std::move(rows));
    const qint64 committedAt = fetchedAtMs > 0
        ? fetchedAtMs : QDateTime::currentMSecsSinceEpoch();
    const QVariantMap parentDecoration = rowDecoration(parent, collectionKind);
    const bool stateChanged = model->m_parentId != parentId
        || model->m_parentDecoration != parentDecoration
        || model->m_fetchedAtMs != committedAt || model->m_stale;
    model->m_parentId = parentId;
    model->m_parentDecoration = parentDecoration;
    model->m_fetchedAtMs = committedAt;
    model->m_stale = false;
    if (stateChanged)
        emit model->stateChanged();
    emit queryChanged(normalized, scopeId);
}

void MediaStore::patchEntity(const QVariantMap &patch, const QSet<QString> &removedFields)
{
    const QString entityId = patch.value(QStringLiteral("id")).toString();
    if (entityId.isEmpty())
        return;
    EntityRecord &record = m_entities[entityId];
    if (removedFields.isEmpty() && !record.fields.isEmpty()) {
        if (record.fields.isSharedWith(patch))
            return;
        bool unchanged = true;
        for (auto iterator = patch.cbegin(); iterator != patch.cend(); ++iterator) {
            const auto existing = record.fields.constFind(iterator.key());
            if (existing == record.fields.cend() || existing.value() != iterator.value()) {
                unchanged = false;
                break;
            }
        }
        if (unchanged)
            return;
    }
    if (record.fields.isEmpty() && removedFields.isEmpty()
        && !m_refreshSourceBaselines.contains(entityId)) {
        record.fields = patch;
        ++record.revision;
        notifyEntityChanged(entityId);
        return;
    }
    QVariantMap acceptedPatch = patch;
    const QString currentUpdatedAt =
        record.fields.value(QStringLiteral("sourceUpdatedAt")).toString();
    const QString incomingUpdatedAt =
        patch.value(QStringLiteral("sourceUpdatedAt")).toString();
    const QDateTime currentTimestamp = QDateTime::fromString(currentUpdatedAt, Qt::ISODate);
    const QDateTime incomingTimestamp = QDateTime::fromString(incomingUpdatedAt, Qt::ISODate);
    bool rejectVersionedMetadata = currentTimestamp.isValid()
        && incomingTimestamp.isValid() && incomingTimestamp < currentTimestamp;

    const auto protectedBaseline = m_refreshSourceBaselines.constFind(entityId);
    if (protectedBaseline != m_refreshSourceBaselines.cend()) {
        const QString incomingVersion =
            patch.value(QStringLiteral("sourceVersion")).toString();
        if (incomingVersion.isEmpty() || incomingVersion == protectedBaseline.value()) {
            rejectVersionedMetadata = true;
        } else if (!rejectVersionedMetadata) {
            m_refreshSourceBaselines.remove(entityId);
        }
    }
    if (rejectVersionedMetadata) {
        for (const QString &field : versionedMetadataKeys)
            acceptedPatch.remove(field);
    }
    QVariantMap merged = record.fields;
    for (auto iterator = acceptedPatch.cbegin(); iterator != acceptedPatch.cend(); ++iterator)
        merged.insert(iterator.key(), iterator.value());
    for (const QString &field : removedFields)
        merged.remove(field);
    if (merged == record.fields)
        return;
    record.fields = std::move(merged);
    ++record.revision;
    notifyEntityChanged(entityId);
}

void MediaStore::patchEntities(const QVariantList &patches)
{
    beginEntityNotificationBatch();
    for (const QVariant &value : patches)
        patchEntity(value.toMap());
    endEntityNotificationBatch();
}

void MediaStore::beginRefreshProtection(const QString &entityId)
{
    if (entityId.isEmpty())
        return;
    m_refreshSourceBaselines.insert(
        entityId,
        m_entities.value(entityId).fields.value(QStringLiteral("sourceVersion")).toString());
}

void MediaStore::endRefreshProtection(const QString &entityId)
{
    m_refreshSourceBaselines.remove(entityId);
}

void MediaStore::markQueryStale(const QString &kind, const QString &scopeId)
{
    MediaQueryModel *model = ensureQueryModel(kind, scopeId);
    if (model->m_stale)
        return;
    model->m_stale = true;
    emit model->stateChanged();
    emit queryChanged(normalizedKind(kind), scopeId);
}

void MediaStore::markAllQueriesStale()
{
    for (MediaQueryModel *model : std::as_const(m_queries)) {
        if (model->m_stale)
            continue;
        model->m_stale = true;
        emit model->stateChanged();
        emit queryChanged(model->kind(), model->scopeId());
    }
}

bool MediaStore::removeQueryRow(
    const QString &kind,
    const QString &scopeId,
    const QString &entityId,
    const QString &rowKey,
    bool journalRemoval)
{
    MediaQueryModel *model = ensureQueryModel(kind, scopeId);
    int index = -1;
    MediaQueryRow removed;
    if (!model->removeFirst(entityId, rowKey, &index, &removed))
        return false;
    if (journalRemoval && m_mutations.contains(m_activeMutationId)) {
        m_mutations[m_activeMutationId].removedRows.push_back(RemovedRow{
            makeQueryKey(kind, scopeId), index, removed, model->contentRevision(),
        });
    }
    emit queryChanged(normalizedKind(kind), scopeId);
    return true;
}

void MediaStore::beginMutation(const QString &mutationId, const QString &entityId)
{
    if (mutationId.isEmpty() || entityId.isEmpty())
        return;
    MutationJournal journal;
    journal.entityId = entityId;
    journal.previousOverlay = m_entities.value(entityId).overlay;
    m_mutations.insert(mutationId, std::move(journal));
    m_activeMutationId = mutationId;
}

void MediaStore::applyMutationPatch(const QVariantMap &patch)
{
    const QString entityId = patch.value(QStringLiteral("id")).toString();
    if (entityId.isEmpty())
        return;
    EntityRecord &record = m_entities[entityId];
    QVariantMap overlay = record.overlay;
    for (auto iterator = patch.cbegin(); iterator != patch.cend(); ++iterator)
        overlay.insert(iterator.key(), iterator.value());
    if (record.overlay == overlay)
        return;
    record.overlay = std::move(overlay);
    notifyEntityChanged(entityId);
}

void MediaStore::commitMutation(const QString &mutationId)
{
    const auto found = m_mutations.find(mutationId);
    if (found == m_mutations.end())
        return;
    EntityRecord &record = m_entities[found->entityId];
    if (!record.overlay.isEmpty()) {
        for (auto iterator = record.overlay.cbegin(); iterator != record.overlay.cend(); ++iterator)
            record.fields.insert(iterator.key(), iterator.value());
        record.overlay.clear();
        ++record.revision;
        notifyEntityChanged(found->entityId);
    }
    m_mutations.erase(found);
    if (m_activeMutationId == mutationId)
        m_activeMutationId.clear();
}

void MediaStore::rollbackMutation(const QString &mutationId)
{
    const auto found = m_mutations.find(mutationId);
    if (found == m_mutations.end())
        return;
    const MutationJournal journal = found.value();
    m_mutations.erase(found);
    if (m_activeMutationId == mutationId)
        m_activeMutationId.clear();
    EntityRecord &record = m_entities[journal.entityId];
    if (record.overlay != journal.previousOverlay) {
        record.overlay = journal.previousOverlay;
        notifyEntityChanged(journal.entityId);
    }
    for (const RemovedRow &removal : journal.removedRows) {
        MediaQueryModel *model = m_queries.value(removal.queryKey);
        if (!model || model->contentRevision() != removal.expectedRevision)
            continue;
        model->restoreRow(removal.index, removal.row);
        emit queryChanged(model->kind(), model->scopeId());
    }
}

void MediaStore::clear()
{
    m_entities.clear();
    m_mutations.clear();
    m_refreshSourceBaselines.clear();
    m_activeMutationId.clear();
    for (MediaQueryModel *model : std::as_const(m_queries))
        model->clearRows();
    emit storeReset();
}

QJsonObject MediaStore::toCacheJson(const QSet<QString> &queryKinds) const
{
    QJsonObject entities;
    QSet<QString> referencedIds;
    QJsonArray queries;
    for (MediaQueryModel *model : m_queries) {
        if (!queryKinds.contains(model->kind()) || !model->scopeId().isEmpty())
            continue;
        QJsonArray rows;
        for (const MediaQueryRow &row : model->m_rows) {
            referencedIds.insert(row.entityId);
            rows.push_back(QJsonObject{
                {QStringLiteral("rowKey"), row.rowKey},
                {QStringLiteral("entityId"), row.entityId},
                {QStringLiteral("decoration"), QJsonObject::fromVariantMap(row.decoration)},
            });
        }
        queries.push_back(QJsonObject{
            {QStringLiteral("kind"), model->kind()},
            {QStringLiteral("scopeId"), model->scopeId()},
            {QStringLiteral("parentId"), model->parentId()},
            {QStringLiteral("parentDecoration"),
             QJsonObject::fromVariantMap(model->m_parentDecoration)},
            {QStringLiteral("fetchedAtMs"), model->fetchedAtMs()},
            {QStringLiteral("stale"), model->stale()},
            {QStringLiteral("rows"), rows},
        });
    }
    for (const QString &entityId : referencedIds) {
        const auto found = m_entities.constFind(entityId);
        if (found != m_entities.cend())
            entities.insert(entityId, QJsonObject::fromVariantMap(found->fields));
    }
    return QJsonObject{
        {QStringLiteral("entities"), entities},
        {QStringLiteral("queries"), queries},
    };
}

bool MediaStore::restoreCacheJson(const QJsonObject &object)
{
    if (!object.value(QStringLiteral("entities")).isObject()
        || !object.value(QStringLiteral("queries")).isArray()) {
        return false;
    }
    const QJsonObject entities = object.value(QStringLiteral("entities")).toObject();
    beginEntityNotificationBatch();
    for (auto iterator = entities.begin(); iterator != entities.end(); ++iterator) {
        if (!iterator.value().isObject())
            continue;
        QVariantMap fields = iterator.value().toObject().toVariantMap();
        fields.insert(QStringLiteral("id"), iterator.key());
        patchEntity(fields);
    }
    endEntityNotificationBatch();
    for (const QJsonValue &queryValue : object.value(QStringLiteral("queries")).toArray()) {
        if (!queryValue.isObject())
            continue;
        const QJsonObject query = queryValue.toObject();
        const QString kind = query.value(QStringLiteral("kind")).toString();
        const QString scopeId = query.value(QStringLiteral("scopeId")).toString();
        MediaQueryModel *model = ensureQueryModel(kind, scopeId);
        QVector<MediaQueryRow> rows;
        for (const QJsonValue &rowValue : query.value(QStringLiteral("rows")).toArray()) {
            const QJsonObject row = rowValue.toObject();
            const QString entityId = row.value(QStringLiteral("entityId")).toString();
            if (entityId.isEmpty() || !m_entities.contains(entityId))
                continue;
            const QVariantMap decoration = row.value(QStringLiteral("decoration"))
                .toObject().toVariantMap();
            rows.push_back(MediaQueryRow{
                row.value(QStringLiteral("rowKey")).toString(),
                entityId,
                decoration,
            });
        }
        model->synchronizeRows(std::move(rows));
        model->m_parentId = query.value(QStringLiteral("parentId")).toString();
        model->m_parentDecoration = query.value(QStringLiteral("parentDecoration"))
            .toObject().toVariantMap();
        model->m_fetchedAtMs = query.value(QStringLiteral("fetchedAtMs")).toVariant().toLongLong();
        model->m_stale = query.value(QStringLiteral("stale")).toBool(true);
        emit model->stateChanged();
        emit queryChanged(model->kind(), model->scopeId());
    }
    return true;
}

QString MediaStore::normalizedKind(const QString &kind)
{
    return kind.trimmed().toLower();
}

QString MediaStore::makeQueryKey(const QString &kind, const QString &scopeId)
{
    return normalizedKind(kind) + QChar(0x1f) + scopeId;
}

QVariantMap MediaStore::canonicalFields(const QVariantMap &item, const QString &kind)
{
    bool hasDecoration = false;
    for (const QString &key : decorationKeys) {
        if (item.contains(key)) {
            hasDecoration = true;
            break;
        }
    }
    const bool hasContextualTitleFlag =
        item.contains(QStringLiteral("titleIsContextual"));
    if (kind == libraryKind && !hasDecoration && !hasContextualTitleFlag)
        return item;

    const bool contextualTitle = hasContextualTitleFlag
        && item.value(QStringLiteral("titleIsContextual")).toBool();
    const bool episodeContextTitle = (kind == resumeKind || kind == recentKind)
        && item.value(QStringLiteral("itemType")).toString()
            == QStringLiteral("Episode");
    const bool removeContextualTitle = contextualTitle || episodeContextTitle;
    const bool removeNullLatestEpisodeSubtitle = kind != libraryKind
        && item.contains(QStringLiteral("latestEpisodeSubtitle"))
        && item.value(QStringLiteral("latestEpisodeSubtitle")).isNull();

    if (!hasDecoration && !hasContextualTitleFlag
        && !removeNullLatestEpisodeSubtitle && !removeContextualTitle) {
        return item;
    }

    QVariantMap fields = item;
    for (const QString &key : decorationKeys) {
        if (fields.contains(key))
            fields.remove(key);
    }
    fields.remove(QStringLiteral("titleIsContextual"));
    if (removeNullLatestEpisodeSubtitle)
        fields.remove(QStringLiteral("latestEpisodeSubtitle"));
    if (removeContextualTitle)
        fields.remove(QStringLiteral("title"));
    return fields;
}

QVariantMap MediaStore::rowDecoration(const QVariantMap &item, const QString &kind)
{
    QVariantMap decoration;
    for (const QString &key : decorationKeys) {
        if (item.contains(key))
            decoration.insert(key, item.value(key));
    }
    const bool hasContextualTitleFlag =
        item.contains(QStringLiteral("titleIsContextual"));
    const bool mayUseEpisodeContext = kind == resumeKind || kind == recentKind;
    if (!hasContextualTitleFlag && !mayUseEpisodeContext)
        return decoration;
    const bool contextualTitle = hasContextualTitleFlag
        && item.value(QStringLiteral("titleIsContextual")).toBool();
    const bool episodeContextTitle = mayUseEpisodeContext
        && item.value(QStringLiteral("itemType")).toString()
            == QStringLiteral("Episode");
    if ((contextualTitle || episodeContextTitle)
        && item.contains(QStringLiteral("title"))) {
        decoration.insert(QStringLiteral("title"), item.value(QStringLiteral("title")));
    }
    return decoration;
}

QString MediaStore::normalizedSearchText(const QString &value)
{
    // This folded form is only a conservative candidate index. Final
    // acceptance still uses QString::contains(..., Qt::CaseInsensitive) on
    // the materialized fields, preserving the established search semantics.
    return value.toCaseFolded();
}

QString MediaStore::searchableText(const QVariantMap &values)
{
    QString result;
    const auto appendValue = [&result](const QString &value) {
        if (value.isEmpty())
            return;
        if (!result.isEmpty())
            result.append(QChar(0x1f));
        result.append(value);
    };
    for (const QString &key : {
             QStringLiteral("title"),
             QStringLiteral("subtitle"),
             QStringLiteral("seriesTitle"),
         }) {
        appendValue(values.value(key).toString());
    }
    return normalizedSearchText(result);
}

MediaQueryModel *MediaStore::ensureQueryModel(const QString &kind, const QString &scopeId)
{
    const QString normalized = normalizedKind(kind);
    const QString key = makeQueryKey(normalized, scopeId);
    auto found = m_queries.find(key);
    if (found != m_queries.end())
        return found.value();
    auto *model = new MediaQueryModel(this, normalized, scopeId, this);
    m_queries.insert(key, model);
    return model;
}

QVariantMap MediaStore::materialize(const MediaQueryRow &row, const QString &kind) const
{
    QVariantMap result = entity(row.entityId);
    for (auto iterator = row.decoration.cbegin(); iterator != row.decoration.cend(); ++iterator)
        result.insert(iterator.key(), iterator.value());
    if ((kind == libraryKind || kind == collectionKind)
        && !result.value(QStringLiteral("latestEpisodeSubtitle")).toString().isEmpty()) {
        result.insert(
            QStringLiteral("subtitle"),
            result.value(QStringLiteral("latestEpisodeSubtitle")));
        result.insert(QStringLiteral("hasLatestEpisode"), true);
    }
    return result;
}

bool MediaStore::filterAccepts(
    const MediaQueryRow &row,
    const QString &kind,
    const QString &needle,
    const QString &category) const
{
    const QVariantMap item = materialize(row, kind);
    if (item.isEmpty())
        return false;
    if (!category.isEmpty() && itemCategory(item) != category)
        return false;
    if (needle.isEmpty())
        return true;
    return item.value(QStringLiteral("title")).toString().contains(
               needle, Qt::CaseInsensitive)
        || item.value(QStringLiteral("subtitle")).toString().contains(
               needle, Qt::CaseInsensitive)
        || item.value(QStringLiteral("seriesTitle")).toString().contains(
               needle, Qt::CaseInsensitive);
}

QPair<QString, QString> MediaStore::filterTexts(
    const MediaQueryRow &row,
    const QString &kind) const
{
    return qMakePair(searchableText(materialize(row, kind)), QString{});
}

void MediaStore::beginEntityNotificationBatch()
{
    ++m_entityNotificationBatchDepth;
}

void MediaStore::endEntityNotificationBatch()
{
    Q_ASSERT(m_entityNotificationBatchDepth > 0);
    if (--m_entityNotificationBatchDepth > 0)
        return;

    const QSet<QString> entityIds = std::exchange(
        m_pendingEntityNotifications, {});
    const QVector<QString> entityOrder = std::exchange(
        m_pendingEntityNotificationOrder, {});
    if (entityIds.isEmpty())
        return;
    for (MediaQueryModel *model : std::as_const(m_queries))
        model->notifyEntitiesChanged(entityIds);
    for (const QString &entityId : entityOrder)
        emit entityChanged(entityId);
}

void MediaStore::notifyEntityChanged(const QString &entityId)
{
    if (m_entityNotificationBatchDepth > 0) {
        if (!m_pendingEntityNotifications.contains(entityId)) {
            m_pendingEntityNotifications.insert(entityId);
            m_pendingEntityNotificationOrder.push_back(entityId);
        }
        return;
    }
    for (MediaQueryModel *model : std::as_const(m_queries))
        model->notifyEntityChanged(entityId);
    emit entityChanged(entityId);
}

MediaQueryProxyModel::MediaQueryProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    m_collator.setLocale(QLocale(m_sortLocale));
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    m_collator.setNumericMode(true);
    setDynamicSortFilter(true);
    setFilterRole(MediaQueryModel::ModelDataRole);
    setSortRole(MediaQueryModel::ModelDataRole);
    connect(this, &QAbstractItemModel::rowsInserted, this, &MediaQueryProxyModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &MediaQueryProxyModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &MediaQueryProxyModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &MediaQueryProxyModel::countChanged);
}

void MediaQueryProxyModel::setSearchText(const QString &value)
{
    if (m_searchText == value)
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    m_searchText = value;
    m_searchNeedle = value.trimmed();
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
    emit searchTextChanged();
}

void MediaQueryProxyModel::setCategory(const QString &value)
{
    if (m_category == value)
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    m_category = value;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
    emit categoryChanged();
}

void MediaQueryProxyModel::setRequireSearchText(bool value)
{
    if (m_requireSearchText == value)
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    m_requireSearchText = value;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
    emit requireSearchTextChanged();
}

void MediaQueryProxyModel::setSortMode(int value)
{
    if (m_sortMode == value)
        return;
    m_sortMode = value;
    updateSorting();
    emit sortModeChanged();
}

void MediaQueryProxyModel::setSortLocale(const QString &value)
{
    if (m_sortLocale == value)
        return;
    m_sortLocale = value;
    m_collator.setLocale(QLocale(m_sortLocale));
    invalidate();
    emit sortLocaleChanged();
}

QVariantMap MediaQueryProxyModel::get(int row) const
{
    return data(index(row, 0), MediaQueryModel::ModelDataRole).toMap();
}

bool MediaQueryProxyModel::filterAcceptsRow(
    int sourceRow,
    const QModelIndex &sourceParent) const
{
    if (!sourceModel())
        return false;
    if (const auto *model = qobject_cast<const MediaQueryModel *>(sourceModel())) {
        if (m_requireSearchText && m_searchNeedle.isEmpty())
            return false;
        return model->filterAccepts(sourceRow, m_searchNeedle, m_category);
    }
    const QVariantMap item = sourceModel()
        ->data(sourceModel()->index(sourceRow, 0, sourceParent), MediaQueryModel::ModelDataRole)
        .toMap();
    if (!m_category.isEmpty() && itemCategory(item) != m_category)
        return false;
    const QString needle = m_searchText.trimmed();
    if (m_requireSearchText && needle.isEmpty())
        return false;
    if (needle.isEmpty())
        return true;
    return item.value(QStringLiteral("title")).toString().contains(needle, Qt::CaseInsensitive)
        || item.value(QStringLiteral("subtitle")).toString().contains(needle, Qt::CaseInsensitive)
        || item.value(QStringLiteral("seriesTitle")).toString().contains(needle, Qt::CaseInsensitive);
}

bool MediaQueryProxyModel::lessThan(
    const QModelIndex &left,
    const QModelIndex &right) const
{
    const QVariantMap first = sourceItem(left);
    const QVariantMap second = sourceItem(right);
    if (m_sortMode == 1)
        return dateValue(first.value(QStringLiteral("updatedAt")))
            > dateValue(second.value(QStringLiteral("updatedAt")));
    if (m_sortMode == 2) {
        const qint64 firstRelease = std::max(
            dateValue(first.value(QStringLiteral("releaseDate"))),
            static_cast<qint64>(first.value(QStringLiteral("productionYear")).toInt()) * 366LL * 86400000LL);
        const qint64 secondRelease = std::max(
            dateValue(second.value(QStringLiteral("releaseDate"))),
            static_cast<qint64>(second.value(QStringLiteral("productionYear")).toInt()) * 366LL * 86400000LL);
        return firstRelease > secondRelease;
    }
    if (m_sortMode == 3)
        return dateValue(first.value(QStringLiteral("dateCreated")))
            > dateValue(second.value(QStringLiteral("dateCreated")));
    if (m_sortMode == 4) {
        const int firstUnplayed = first.value(QStringLiteral("unplayedCount")).toInt();
        const int secondUnplayed = second.value(QStringLiteral("unplayedCount")).toInt();
        if (firstUnplayed != secondUnplayed)
            return firstUnplayed > secondUnplayed;
        return dateValue(first.value(QStringLiteral("updatedAt")))
            > dateValue(second.value(QStringLiteral("updatedAt")));
    }
    return m_collator.compare(
        first.value(QStringLiteral("title")).toString(),
        second.value(QStringLiteral("title")).toString()) < 0;
}

QVariantMap MediaQueryProxyModel::sourceItem(const QModelIndex &index) const
{
    return sourceModel() ? sourceModel()->data(index, MediaQueryModel::ModelDataRole).toMap()
                         : QVariantMap {};
}

void MediaQueryProxyModel::updateSorting()
{
    if (m_sortMode < 0)
        sort(-1);
    else
        sort(0, Qt::AscendingOrder);
    invalidate();
}

MediaSearchModel::MediaSearchModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int MediaSearchModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_sourceRows.size();
}

QVariant MediaSearchModel::data(const QModelIndex &index, int role) const
{
    if (!m_sourceModel || !index.isValid()
        || index.row() < 0 || index.row() >= m_sourceRows.size()) {
        return {};
    }
    return m_sourceModel->data(m_sourceModel->index(m_sourceRows.at(index.row()), 0), role);
}

QHash<int, QByteArray> MediaSearchModel::roleNames() const
{
    return m_sourceModel ? m_sourceModel->roleNames()
                         : QHash<int, QByteArray>{
                               {MediaQueryModel::ModelDataRole, QByteArrayLiteral("modelData")},
                               {MediaQueryModel::EntityIdRole, QByteArrayLiteral("entityId")},
                               {MediaQueryModel::RowKeyRole, QByteArrayLiteral("rowKey")},
                           };
}

void MediaSearchModel::setSourceModel(MediaQueryModel *value)
{
    if (m_sourceModel == value)
        return;
    if (m_sourceModel)
        disconnect(m_sourceModel, nullptr, this, nullptr);
    m_sourceModel = value;
    if (m_sourceModel) {
        connect(m_sourceModel, &QAbstractItemModel::modelReset,
                this, &MediaSearchModel::sourceStructureChanged);
        connect(m_sourceModel, &QAbstractItemModel::rowsInserted,
                this, &MediaSearchModel::sourceStructureChanged);
        connect(m_sourceModel, &QAbstractItemModel::rowsRemoved,
                this, &MediaSearchModel::sourceStructureChanged);
        connect(m_sourceModel, &QAbstractItemModel::rowsMoved,
                this, &MediaSearchModel::sourceStructureChanged);
        connect(m_sourceModel, &QAbstractItemModel::dataChanged,
                this, &MediaSearchModel::sourceDataChanged);
        connect(m_sourceModel, &MediaQueryModel::rowsSynchronized,
                this, &MediaSearchModel::sourceChanged);
        connect(m_sourceModel, &QObject::destroyed, this, [this] {
            m_sourceModel = nullptr;
            sourceChanged();
            emit sourceModelChanged();
        });
    }
    sourceChanged();
    emit sourceModelChanged();
}

void MediaSearchModel::setSearchText(const QString &value)
{
    if (m_searchText == value)
        return;
    m_searchText = value;
    m_searchNeedle = value.trimmed();
    m_indexSearchText = MediaStore::normalizedSearchText(m_searchNeedle);
    rebuild();
    emit searchTextChanged();
}

void MediaSearchModel::setRequireSearchText(bool value)
{
    if (m_requireSearchText == value)
        return;
    m_requireSearchText = value;
    rebuild();
    emit requireSearchTextChanged();
}

QVariantMap MediaSearchModel::get(int row) const
{
    return data(index(row, 0), MediaQueryModel::ModelDataRole).toMap();
}

void MediaSearchModel::sourceChanged()
{
    rebuildIndex();
    if (!rebuild(true) && !m_sourceRows.isEmpty()) {
        emit dataChanged(
            index(0, 0),
            index(m_sourceRows.size() - 1, 0));
    }
}

void MediaSearchModel::sourceStructureChanged()
{
    if (m_sourceModel && m_sourceModel->m_synchronizingRows)
        return;
    sourceChanged();
}

void MediaSearchModel::sourceDataChanged(
    const QModelIndex &topLeft,
    const QModelIndex &bottomRight,
    const QList<int> &roles)
{
    if (m_sourceModel && m_sourceModel->m_synchronizingRows)
        return;
    bool requiresFullRebuild = m_indexedTexts.size()
        != (m_sourceModel ? m_sourceModel->rowCount() : 0);
    int changedTextRows = 0;
    if (!requiresFullRebuild && m_sourceModel) {
        const int firstSourceRow = std::max(0, topLeft.row());
        const int lastSourceRow = std::min(
            bottomRight.row(), m_sourceModel->rowCount() - 1);
        for (int sourceRow = firstSourceRow;
             sourceRow <= lastSourceRow;
             ++sourceRow) {
            const auto updatedTexts = m_sourceModel->filterTexts(sourceRow);
            if (updatedTexts == m_indexedTexts.at(sourceRow))
                continue;

            const QVector<quint64> oldKeys = searchGramKeys(
                m_indexedTexts.at(sourceRow));
            const QVector<quint64> newKeys = searchGramKeys(updatedTexts);
            for (const quint64 key : newKeys) {
                if (!std::binary_search(oldKeys.cbegin(), oldKeys.cend(), key))
                    m_postings[key].push_back(sourceRow);
            }
            m_indexedTexts[sourceRow] = updatedTexts;
            ++changedTextRows;
        }
    }
    if (changedTextRows > 0) {
        m_hasStalePostings = true;
        m_incrementalTextChanges += changedTextRows;
        const int rebuildThreshold = std::max(
            64, m_sourceModel->rowCount() / 10);
        requiresFullRebuild = m_incrementalTextChanges > rebuildThreshold;
    }
    if (requiresFullRebuild) {
        rebuildIndex();
    }
    if ((requiresFullRebuild || changedTextRows > 0) && rebuild())
        return;

    int firstProxyRow = -1;
    int lastProxyRow = -1;
    for (int proxyRow = 0; proxyRow < m_sourceRows.size(); ++proxyRow) {
        const int sourceRow = m_sourceRows.at(proxyRow);
        if (sourceRow < topLeft.row() || sourceRow > bottomRight.row())
            continue;
        if (firstProxyRow < 0)
            firstProxyRow = proxyRow;
        lastProxyRow = proxyRow;
    }
    if (firstProxyRow >= 0) {
        emit dataChanged(
            index(firstProxyRow, 0),
            index(lastProxyRow, 0),
            roles);
    }
}

void MediaSearchModel::rebuildIndex()
{
    m_postings.clear();
    m_indexedTexts.clear();
    m_hasStalePostings = false;
    m_incrementalTextChanges = 0;
    if (!m_sourceModel)
        return;
    const int sourceCount = m_sourceModel->rowCount();
    m_indexedTexts.resize(sourceCount);
    m_postings.reserve(std::min(sourceCount * 4, 500'000));
    for (int row = 0; row < sourceCount; ++row) {
        const auto texts = m_sourceModel->filterTexts(row);
        m_indexedTexts[row] = texts;
        for (const quint64 key : searchGramKeys(texts))
            m_postings[key].push_back(row);
    }
}

bool MediaSearchModel::rebuild(bool preserveSourceViewState)
{
    QVector<int> sourceRows;
    if (m_sourceModel && (!m_requireSearchText || !m_searchNeedle.isEmpty())) {
        const int sourceCount = m_sourceModel->rowCount();
        if (m_searchNeedle.isEmpty()) {
            sourceRows.resize(sourceCount);
            std::iota(sourceRows.begin(), sourceRows.end(), 0);
        } else {
            const QStringView query(m_indexSearchText);
            if (query.isEmpty()) {
                sourceRows.reserve(sourceCount);
                for (int row = 0; row < sourceCount; ++row) {
                    if (m_sourceModel->filterAccepts(
                            row, m_searchNeedle, {})) {
                        sourceRows.push_back(row);
                    }
                }
            } else {
                const qsizetype gramLength = std::min<qsizetype>(3, query.size());
                const QVector<int> *candidates = nullptr;
                for (qsizetype offset = 0;
                     offset <= query.size() - gramLength;
                     ++offset) {
                    const auto found = m_postings.constFind(
                        searchGramKey(query, offset, gramLength));
                    if (found == m_postings.cend()) {
                        candidates = nullptr;
                        break;
                    }
                    if (!candidates || found->size() < candidates->size())
                        candidates = &found.value();
                }
                if (candidates) {
                    // The gram index only narrows candidates. Exact verification
                    // preserves the previous materialized-field and Qt case
                    // insensitive matching behavior, including short queries.
                    sourceRows.reserve(candidates->size());
                    QBitArray seenRows;
                    if (m_hasStalePostings)
                        seenRows.resize(sourceCount);
                    for (const int row : *candidates) {
                        if (row < 0 || row >= sourceCount)
                            continue;
                        if (m_hasStalePostings) {
                            if (seenRows.testBit(row))
                                continue;
                            seenRows.setBit(row);
                        }
                        if (m_sourceModel->filterAccepts(
                                row, m_searchNeedle, {})) {
                            sourceRows.push_back(row);
                        }
                    }
                    if (m_hasStalePostings)
                        std::sort(sourceRows.begin(), sourceRows.end());
                }
            }
        }
    }
    QVector<QString> rowKeys;
    rowKeys.reserve(sourceRows.size());
    for (const int sourceRow : sourceRows) {
        rowKeys.push_back(
            m_sourceModel && sourceRow >= 0
                    && sourceRow < m_sourceModel->m_rows.size()
                ? m_sourceModel->m_rows.at(sourceRow).rowKey
                : QString{});
    }
    if (m_sourceRows == sourceRows && m_rowKeys == rowKeys)
        return false;

    if (preserveSourceViewState && m_rowKeys.size() == rowKeys.size()) {
        const QSet<QString> previousKeys(m_rowKeys.cbegin(), m_rowKeys.cend());
        const QSet<QString> nextKeys(rowKeys.cbegin(), rowKeys.cend());
        if (previousKeys == nextKeys) {
            QHash<QString, int> newRows;
            newRows.reserve(rowKeys.size());
            for (int row = 0; row < rowKeys.size(); ++row)
                newRows.insert(rowKeys.at(row), row);

            emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
            const QModelIndexList from = persistentIndexList();
            m_sourceRows = std::move(sourceRows);
            const QVector<QString> oldRowKeys = std::exchange(
                m_rowKeys, std::move(rowKeys));
            QModelIndexList to;
            to.reserve(from.size());
            for (const QModelIndex &oldIndex : from) {
                const QString key = oldIndex.row() >= 0
                        && oldIndex.row() < oldRowKeys.size()
                    ? oldRowKeys.at(oldIndex.row())
                    : QString{};
                const auto found = newRows.constFind(key);
                to.push_back(found == newRows.cend()
                        ? QModelIndex{}
                        : createIndex(found.value(), oldIndex.column()));
            }
            changePersistentIndexList(from, to);
            emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
            return true;
        }
    }

    beginResetModel();
    m_sourceRows = std::move(sourceRows);
    m_rowKeys = std::move(rowKeys);
    endResetModel();
    emit countChanged();
    return true;
}
