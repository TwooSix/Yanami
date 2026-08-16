#include "CatalogCoordinator.hpp"

#include <QDebug>
#include <QTimer>

#include <array>

namespace {

const QString libraryRequestKey = QStringLiteral("library");
const QString activityRequestKey = QStringLiteral("activity");
const QString favoritesRequestKey = QStringLiteral("favorites");
const QString playlistsViewId = QStringLiteral("__yanami_playlists__");

QString collectionRequestKey(const QString &parentId)
{
    return QStringLiteral("collection:") + parentId;
}

int operationCode(MediaPort::Operation operation)
{
    return static_cast<int>(operation);
}

QVariantMap coherentItemPatch(const QVariantMap &item)
{
    // Route-specific decoration must not leak into other mounted queries.
    // Keep only fields whose meaning is stable in every catalog surface.
    static const std::array<const char *, 13> fields {
        "title",
        "overview",
        "imageUrl",
        "backdropUrl",
        "productionYear",
        "itemType",
        "seriesId",
        "seasonId",
        "childCount",
        "unplayedCount",
        "played",
        "favorite",
        "resumeTicks",
    };
    QVariantMap patch {{QStringLiteral("id"),
                        item.value(QStringLiteral("id"))}};
    for (const char *field : fields) {
        const QString key = QString::fromLatin1(field);
        if (item.contains(key))
            patch.insert(key, item.value(key));
    }
    return patch;
}

} // namespace

QString CatalogCoordinator::beginOptimisticStateMutation(
    const QString &itemId,
    MediaPort::Operation operation,
    const QVariantMap &payload,
    quint64 submissionSequence)
{
    if (!activeSession()
        || (operation != MediaPort::Operation::SetPlayed
            && operation != MediaPort::Operation::SetFavorite)
        || m_mediaStore->entity(itemId).isEmpty()) {
        return {};
    }

    const QString mutationId = itemId + QChar(0x1f)
        + QString::number(operationCode(operation)) + QChar(0x1f)
        + QString::number(submissionSequence);
    m_mediaStore->beginMutation(mutationId, itemId);

    QVariantMap patch {{QStringLiteral("id"), itemId}};
    if (operation == MediaPort::Operation::SetPlayed) {
        const bool played =
            payload.value(QStringLiteral("played")).toBool();
        patch.insert(QStringLiteral("played"), played);
        patch.insert(QStringLiteral("progress"), played ? 100.0 : 0.0);
        patch.insert(QStringLiteral("resumeTicks"), 0);
        if (played) {
            m_mediaStore->removeQueryRow(
                QStringLiteral("resume"), {}, itemId, {}, true);
        }
    } else {
        const bool favorite =
            payload.value(QStringLiteral("favorite")).toBool();
        patch.insert(QStringLiteral("favorite"), favorite);
        if (!favorite) {
            m_mediaStore->removeQueryRow(
                QStringLiteral("favorites"), {}, itemId, {}, true);
        }
    }
    m_mediaStore->applyMutationPatch(patch);
    m_requests.invalidate({activityRequestKey, favoritesRequestKey});
    qInfo().noquote()
        << "optimistic_media_mutation"
        << "operation=" << operationCode(operation)
        << "item=" << itemId
        << "journal=incremental"
        << "blocking=false";
    return mutationId;
}

bool CatalogCoordinator::applyAuthoritativeStates(
    const QVariantList &states)
{
    bool changed = false;
    for (const QVariant &value : states) {
        const QVariantMap state = value.toMap();
        const QString id = state.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        const QVariantMap before = m_mediaStore->entity(id);
        m_mediaStore->patchEntity(state);
        changed = changed || before != m_mediaStore->entity(id);
    }
    if (!changed)
        return false;
    m_requests.invalidate(activityRequestKey);
    saveLibraryCache();
    return true;
}

void CatalogCoordinator::rollbackOptimisticStateMutation(
    const QString &mutationId)
{
    if (mutationId.isEmpty())
        return;
    // Rollback is deliberately allowed while the session is fenced. Media is
    // fenced after Catalog, and must still be able to unwind an old overlay.
    m_mediaStore->rollbackMutation(mutationId);
    m_requests.invalidate(activityRequestKey);
    saveLibraryCache();
}

void CatalogCoordinator::commitOptimisticStateMutation(
    const QString &mutationId)
{
    if (mutationId.isEmpty())
        return;
    m_mediaStore->commitMutation(mutationId);
    // Optimistic overlays are intentionally absent from the persisted cache.
    // Once promoted, persist the committed fields even if a following
    // authoritative patch reports the same effective value.
    saveLibraryCache();
}

void CatalogCoordinator::applyEntityPatch(const QVariantMap &item)
{
    const QVariantMap patch = coherentItemPatch(item);
    if (!patch.value(QStringLiteral("id")).toString().isEmpty())
        m_mediaStore->patchEntity(patch);
}

void CatalogCoordinator::applyInvalidationEvent(const QVariantMap &event)
{
    QStringList queryKinds;
    for (const QVariant &value :
         event.value(QStringLiteral("queryKinds")).toList()) {
        const QString kind = value.toString();
        if (!kind.isEmpty())
            queryKinds.push_back(kind);
    }

    QStringList requestKeys;
    for (const QString &kind : queryKinds) {
        if (kind == QStringLiteral("library")) {
            requestKeys.push_back(libraryRequestKey);
            m_mediaStore->markQueryStale(QStringLiteral("library"));
            m_mediaStore->markQueryStale(QStringLiteral("views"));
        } else if (kind == QStringLiteral("activity")) {
            requestKeys.push_back(activityRequestKey);
            m_mediaStore->markQueryStale(QStringLiteral("resume"));
            m_mediaStore->markQueryStale(QStringLiteral("recent"));
        } else if (kind == QStringLiteral("favorites")) {
            requestKeys.push_back(favoritesRequestKey);
            m_mediaStore->markQueryStale(QStringLiteral("favorites"));
        } else if (kind == QStringLiteral("collection")
                   && !m_collectionTargetId.isEmpty()) {
            requestKeys.push_back(
                collectionRequestKey(m_collectionTargetId));
            m_mediaStore->markQueryStale(
                QStringLiteral("collection"), m_collectionTargetId);
        }
    }
    requestKeys.removeDuplicates();
    m_requests.invalidate(requestKeys);
    if (!queryKinds.isEmpty()) {
        qInfo().noquote()
            << "cache_invalidation"
            << "operation="
            << event.value(QStringLiteral("operationId")).toString()
            << "queries=" << queryKinds.join(u',')
            << "entities="
            << event.value(QStringLiteral("entityIds")).toList().size();
    }
}

void CatalogCoordinator::applyPendingMetadataPatch(
    const QString &itemId,
    const QVariantMap &payload)
{
    QVariantMap patch {{QStringLiteral("id"), itemId}};
    static const std::array<const char *, 3> fields {
        "title",
        "overview",
        "productionYear",
    };
    for (const char *field : fields) {
        const QString key = QString::fromLatin1(field);
        if (payload.contains(key))
            patch.insert(key, payload.value(key));
    }
    applyEntityPatch(patch);
    saveLibraryCache();
}

void CatalogCoordinator::applyContainerMutation(
    const QString &itemId,
    MediaPort::Operation operation,
    const QVariantMap &result)
{
    const auto markStale = [this](const QString &parentId) {
        if (!parentId.isEmpty()) {
            m_mediaStore->markQueryStale(
                QStringLiteral("collection"), parentId);
        }
    };
    const quint64 generation = m_committedSession.generation;

    if (operation == MediaPort::Operation::AddToPlaylist) {
        const QString targetId =
            result.value(QStringLiteral("targetId")).toString();
        const bool created = result.value(QStringLiteral("created")).toBool();
        markStale(playlistsViewId);
        markStale(targetId);

        if (created) {
            QTimer::singleShot(0, this, [this, generation] {
                if (acceptsSession(generation))
                    loadLibrary();
            });
        }
        const bool activePageAffected =
            m_collectionDisplayedId == m_collectionTargetId
            && (m_collectionTargetId == playlistsViewId
                || (!targetId.isEmpty()
                    && m_collectionTargetId == targetId));
        if (activePageAffected) {
            const QString parentId = m_collectionTargetId;
            QTimer::singleShot(
                0, this, [this, generation, parentId] {
                    if (acceptsSession(generation))
                        loadCollection(parentId);
                });
        }
        qInfo().noquote()
            << "collection_mutation"
            << "operation=" << operationCode(operation)
            << "item=" << itemId
            << "target=" << targetId
            << "created=" << created
            << "activeRefreshQueued=" << activePageAffected
            << "scope=targeted";
        return;
    }

    if (operation != MediaPort::Operation::RemoveFromPlaylist)
        return;

    const QString playlistId =
        result.value(QStringLiteral("playlistId")).toString();
    const QString entryId = result.value(
        QStringLiteral("removedPlaylistEntryId")).toString();
    const QString removedItemId =
        result.value(QStringLiteral("removedItemId")).toString();
    markStale(playlistsViewId);
    markStale(playlistId);

    const QString rowKey = entryId.isEmpty()
        ? QString()
        : QStringLiteral("playlist:") + entryId;
    QTimer::singleShot(
        170, this,
        [this, generation, playlistId, removedItemId, rowKey, operation] {
            if (!acceptsSession(generation))
                return;
            const bool cacheChanged = m_mediaStore->removeQueryRow(
                QStringLiteral("collection"),
                playlistId,
                removedItemId,
                rowKey);
            qInfo().noquote()
                << "collection_mutation_commit"
                << "operation=" << operationCode(operation)
                << "playlist=" << playlistId
                << "item=" << removedItemId
                << "cacheChanged=" << cacheChanged
                << "visibleChanged="
                << (cacheChanged
                    && m_collectionDisplayedId == playlistId)
                << "scope=targeted";
        });
    qInfo().noquote()
        << "collection_mutation"
        << "operation=" << operationCode(operation)
        << "playlist=" << playlistId
        << "entry=" << entryId
        << "item=" << removedItemId
        << "removalScheduled=true"
        << "scope=targeted";
}

void CatalogCoordinator::beginRefreshProtection(const QString &itemId)
{
    m_mediaStore->beginRefreshProtection(itemId);
}

void CatalogCoordinator::endRefreshProtection(const QString &itemId)
{
    m_mediaStore->endRefreshProtection(itemId);
}

void CatalogCoordinator::invalidatePresentationCache()
{
    QStringList requestKeys {
        libraryRequestKey,
        activityRequestKey,
        favoritesRequestKey,
    };
    if (!m_collectionTargetId.isEmpty()) {
        requestKeys.push_back(
            collectionRequestKey(m_collectionTargetId));
    }
    m_requests.invalidate(requestKeys);
    m_lastFullLibraryRefreshMs = 0;
    m_lastFavoritesRefreshMs = 0;
    m_mediaStore->markAllQueriesStale();
    saveLibraryCache();
}

void CatalogCoordinator::scheduleContentReconciliation(
    const QString &reason)
{
    if (!activeSession())
        return;
    const quint64 generation = m_committedSession.generation;
    const QString collectionId = m_collectionTargetId;
    qInfo().noquote()
        << "catalog_reconciliation"
        << "phase=queued"
        << "reason=" << reason
        << "sessionGeneration=" << generation;

    QTimer::singleShot(0, this, [this, generation] {
        if (acceptsSession(generation))
            loadLibrary();
    });
    if (!collectionId.isEmpty()) {
        QTimer::singleShot(0, this,
            [this, generation, collectionId] {
                if (acceptsSession(generation))
                    loadCollection(collectionId);
            });
    }
    QTimer::singleShot(0, this, [this, generation] {
        if (acceptsSession(generation))
            refreshActivity();
    });
    QTimer::singleShot(0, this, [this, generation] {
        if (acceptsSession(generation))
            refreshFavorites();
    });
}
