#include "MediaStore.hpp"
#include "LibraryCacheContract.hpp"
#include "RequestCoordinator.hpp"

#include <QJsonObject>
#include <QPersistentModelIndex>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThreadPool>
#include <QtTest>

namespace {

QVariantMap item(
    const QString &id,
    const QString &title,
    const QString &imageUrl,
    const QString &type = QStringLiteral("Series"),
    const QString &subtitle = QString())
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("title"), title},
        {QStringLiteral("imageUrl"), imageUrl},
        {QStringLiteral("itemType"), type},
        {QStringLiteral("subtitle"), subtitle},
    };
}

} // namespace

class MediaStoreTests : public QObject
{
    Q_OBJECT

private slots:
    void cacheEnvelopeRejectsSchemaOrScopeBeforeRestore()
    {
        const QJsonObject valid {
            {QStringLiteral("cacheSchemaVersion"), 1},
            {QStringLiteral("bridgeSchemaVersion"), 8},
            {QStringLiteral("cacheScope"), QStringLiteral("account-A")},
            {QStringLiteral("entities"), QJsonObject{}},
            {QStringLiteral("queries"), QJsonArray{}},
        };
        QVERIFY(YanamiCache::acceptsEnvelope(
            valid, 1, 8, QStringLiteral("account-A")));

        QJsonObject incompatibleBridge = valid;
        incompatibleBridge.insert(QStringLiteral("bridgeSchemaVersion"), 7);
        QVERIFY(!YanamiCache::acceptsEnvelope(
            incompatibleBridge, 1, 8, QStringLiteral("account-A")));

        QJsonObject incompatibleCache = valid;
        incompatibleCache.insert(QStringLiteral("cacheSchemaVersion"), 2);
        QVERIFY(!YanamiCache::acceptsEnvelope(
            incompatibleCache, 1, 8, QStringLiteral("account-A")));
        QVERIFY(!YanamiCache::acceptsEnvelope(
            valid, 1, 8, QStringLiteral("account-B")));
    }

    void entityPatchUpdatesEveryReferencingQueryWithoutReset()
    {
        MediaStore store;
        store.setQuery(QStringLiteral("library"), {}, {
            item(QStringLiteral("series-1"), QStringLiteral("Old title"), QStringLiteral("old.jpg")),
        });
        store.setQuery(QStringLiteral("favorites"), {}, {
            item(QStringLiteral("series-1"), QStringLiteral("Old title"), QStringLiteral("old.jpg")),
        });

        QSignalSpy libraryChanged(store.libraryModel(), &QAbstractItemModel::dataChanged);
        QSignalSpy favoritesChanged(store.favoritesModel(), &QAbstractItemModel::dataChanged);
        QSignalSpy libraryReset(store.libraryModel(), &QAbstractItemModel::modelReset);

        store.patchEntity({
            {QStringLiteral("id"), QStringLiteral("series-1")},
            {QStringLiteral("title"), QStringLiteral("New title")},
            {QStringLiteral("imageUrl"), QStringLiteral("new.jpg")},
        });

        QCOMPARE(store.libraryModel()->get(0).value(QStringLiteral("title")).toString(),
                 QStringLiteral("New title"));
        QCOMPARE(store.favoritesModel()->get(0).value(QStringLiteral("imageUrl")).toString(),
                 QStringLiteral("new.jpg"));
        QCOMPARE(libraryChanged.size(), 1);
        QCOMPARE(favoritesChanged.size(), 1);
        QCOMPARE(libraryReset.size(), 0);
    }

    void queryDecorationDoesNotOverwriteCanonicalMetadata()
    {
        MediaStore store;
        store.setQuery(QStringLiteral("library"), {}, {
            item(QStringLiteral("episode-1"), QStringLiteral("Episode title"),
                 QStringLiteral("poster.jpg"), QStringLiteral("Episode"),
                 QStringLiteral("S01E01 · Episode title")),
        });
        store.setQuery(QStringLiteral("recent"), {}, {
            item(QStringLiteral("episode-1"), QStringLiteral("Series title"),
                 QStringLiteral("still.jpg"), QStringLiteral("Episode"),
                 QStringLiteral("S01E01 · Episode title")),
        });

        QCOMPARE(store.libraryModel()->get(0).value(QStringLiteral("title")).toString(),
                 QStringLiteral("Episode title"));
        QCOMPARE(store.recentModel()->get(0).value(QStringLiteral("title")).toString(),
                 QStringLiteral("Series title"));

        store.patchEntity({
            {QStringLiteral("id"), QStringLiteral("episode-1")},
            {QStringLiteral("imageUrl"), QStringLiteral("fresh.jpg")},
        });
        QCOMPARE(store.libraryModel()->get(0).value(QStringLiteral("imageUrl")).toString(),
                 QStringLiteral("fresh.jpg"));
        QCOMPARE(store.recentModel()->get(0).value(QStringLiteral("imageUrl")).toString(),
                 QStringLiteral("fresh.jpg"));
    }

    void optimisticMutationRollsBackOnlyTouchedEntityAndRow()
    {
        MediaStore store;
        store.setQuery(QStringLiteral("resume"), {}, {
            item(QStringLiteral("episode-1"), QStringLiteral("One"), QStringLiteral("1.jpg"),
                 QStringLiteral("Episode")),
            item(QStringLiteral("episode-2"), QStringLiteral("Two"), QStringLiteral("2.jpg"),
                 QStringLiteral("Episode")),
        });
        store.beginMutation(QStringLiteral("mutation-1"), QStringLiteral("episode-1"));
        store.applyMutationPatch({
            {QStringLiteral("id"), QStringLiteral("episode-1")},
            {QStringLiteral("played"), true},
        });
        QVERIFY(store.removeQueryRow(
            QStringLiteral("resume"), {}, QStringLiteral("episode-1"), {}, true));
        QCOMPARE(store.resumeModel()->rowCount(), 1);

        store.patchEntity({
            {QStringLiteral("id"), QStringLiteral("episode-2")},
            {QStringLiteral("imageUrl"), QStringLiteral("2-updated.jpg")},
        });
        store.rollbackMutation(QStringLiteral("mutation-1"));

        QCOMPARE(store.resumeModel()->rowCount(), 2);
        QCOMPARE(store.resumeModel()->get(0).value(QStringLiteral("played")).toBool(), false);
        QCOMPARE(store.resumeModel()->get(1).value(QStringLiteral("imageUrl")).toString(),
                 QStringLiteral("2-updated.jpg"));
    }

    void normalizedCacheRoundTripKeepsRowsAndSharedEntities()
    {
        MediaStore original;
        original.setQuery(QStringLiteral("library"), {}, {
            item(QStringLiteral("series-1"), QStringLiteral("Series"), QStringLiteral("cover.jpg")),
        });
        original.setQuery(QStringLiteral("favorites"), {}, {
            item(QStringLiteral("series-1"), QStringLiteral("Series"), QStringLiteral("cover.jpg")),
        });
        const QJsonObject cache = original.toCacheJson({
            QStringLiteral("library"), QStringLiteral("favorites"),
        });

        MediaStore restored;
        QVERIFY(restored.restoreCacheJson(cache));
        QCOMPARE(restored.libraryModel()->rowCount(), 1);
        QCOMPARE(restored.favoritesModel()->rowCount(), 1);
        QCOMPARE(cache.value(QStringLiteral("entities")).toObject().size(), 1);

        restored.patchEntity({
            {QStringLiteral("id"), QStringLiteral("series-1")},
            {QStringLiteral("imageUrl"), QStringLiteral("fresh.jpg")},
        });
        QCOMPARE(restored.libraryModel()->get(0).value(QStringLiteral("imageUrl")).toString(),
                 QStringLiteral("fresh.jpg"));
        QCOMPARE(restored.favoritesModel()->get(0).value(QStringLiteral("imageUrl")).toString(),
                 QStringLiteral("fresh.jpg"));
    }

    void staleOrUnchangedSourceVersionCannotRevertMetadata()
    {
        MediaStore store;
        QVariantMap original = item(
            QStringLiteral("series-1"), QStringLiteral("Original"), QStringLiteral("old.jpg"));
        original.insert(QStringLiteral("sourceVersion"), QStringLiteral("v1"));
        original.insert(
            QStringLiteral("sourceUpdatedAt"), QStringLiteral("2026-08-15T00:00:00Z"));
        original.insert(QStringLiteral("imageItemId"), QStringLiteral("series-owner-v1"));
        original.insert(QStringLiteral("imageItemType"), QStringLiteral("Series"));
        original.insert(QStringLiteral("imageTag"), QStringLiteral("poster-v1"));
        original.insert(QStringLiteral("primaryImageAspectRatio"), 0.67);
        store.setQuery(QStringLiteral("library"), {}, {original});
        store.beginRefreshProtection(QStringLiteral("series-1"));

        QVariantMap unchanged = original;
        unchanged.insert(QStringLiteral("title"), QStringLiteral("Stale response"));
        unchanged.insert(QStringLiteral("imageItemId"), QStringLiteral("stale-owner"));
        unchanged.insert(QStringLiteral("imageItemType"), QStringLiteral("Episode"));
        unchanged.insert(QStringLiteral("imageTag"), QStringLiteral("stale-poster"));
        unchanged.insert(QStringLiteral("primaryImageAspectRatio"), 1.78);
        store.setQuery(QStringLiteral("favorites"), {}, {unchanged});
        QCOMPARE(store.entity(QStringLiteral("series-1"))
                     .value(QStringLiteral("title")).toString(),
                 QStringLiteral("Original"));
        QCOMPARE(store.entity(QStringLiteral("series-1"))
                     .value(QStringLiteral("imageItemId")).toString(),
                 QStringLiteral("series-owner-v1"));
        QCOMPARE(store.entity(QStringLiteral("series-1"))
                     .value(QStringLiteral("imageTag")).toString(),
                 QStringLiteral("poster-v1"));

        QVariantMap fresh = original;
        fresh.insert(QStringLiteral("title"), QStringLiteral("Refreshed"));
        fresh.insert(QStringLiteral("imageUrl"), QStringLiteral("new.jpg"));
        fresh.insert(QStringLiteral("imageItemId"), QStringLiteral("series-owner-v2"));
        fresh.insert(QStringLiteral("imageItemType"), QStringLiteral("Series"));
        fresh.insert(QStringLiteral("imageTag"), QStringLiteral("poster-v2"));
        fresh.insert(QStringLiteral("primaryImageAspectRatio"), 0.71);
        fresh.insert(QStringLiteral("sourceVersion"), QStringLiteral("v2"));
        fresh.insert(
            QStringLiteral("sourceUpdatedAt"), QStringLiteral("2026-08-15T00:01:00Z"));
        store.setQuery(QStringLiteral("collection"), QStringLiteral("series-1"), {fresh});
        QCOMPARE(store.entity(QStringLiteral("series-1"))
                     .value(QStringLiteral("imageUrl")).toString(),
                 QStringLiteral("new.jpg"));
        QCOMPARE(store.entity(QStringLiteral("series-1"))
                     .value(QStringLiteral("imageItemId")).toString(),
                 QStringLiteral("series-owner-v2"));
        QCOMPARE(store.entity(QStringLiteral("series-1"))
                     .value(QStringLiteral("imageTag")).toString(),
                 QStringLiteral("poster-v2"));

        store.patchEntity(original);
        QCOMPARE(store.entity(QStringLiteral("series-1"))
                     .value(QStringLiteral("title")).toString(),
                 QStringLiteral("Refreshed"));
        QCOMPARE(store.entity(QStringLiteral("series-1"))
                     .value(QStringLiteral("imageItemId")).toString(),
                 QStringLiteral("series-owner-v2"));
    }

    void proxyFiltersSearchAndFavoriteCategories()
    {
        MediaStore store;
        store.setQuery(QStringLiteral("library"), {}, {
            item(QStringLiteral("series-1"), QStringLiteral("Solo Camping"), QStringLiteral("1.jpg")),
            item(QStringLiteral("movie-1"), QStringLiteral("Another Story"), QStringLiteral("2.jpg"),
                 QStringLiteral("Movie")),
        });
        MediaSearchModel search;
        search.setSourceModel(store.libraryModel());
        search.setRequireSearchText(true);
        QCOMPARE(search.rowCount(), 0);
        search.setSearchText(QStringLiteral("camp"));
        QCOMPARE(search.rowCount(), 1);
        QCOMPARE(search.get(0).value(QStringLiteral("id")).toString(),
                 QStringLiteral("series-1"));

        MediaQueryProxyModel movies;
        movies.setSourceModel(store.libraryModel());
        movies.setCategory(QStringLiteral("movies"));
        QCOMPARE(movies.rowCount(), 1);
        QCOMPARE(movies.get(0).value(QStringLiteral("id")).toString(),
                 QStringLiteral("movie-1"));
    }

    void searchIndexPreservesExistingCaseAndFieldSemanticsWithLivePatches()
    {
        MediaStore store;
        QVariantMap sourceItem = item(
            QStringLiteral("series-1"),
            QStringLiteral("ＡＢＣ Café"),
            QStringLiteral("1.jpg"),
            QStringLiteral("Series"),
            QStringLiteral("Finale Special"));
        sourceItem.insert(
            QStringLiteral("aliases"),
            QVariantList{QStringLiteral("hidden alias")});
        store.setQuery(QStringLiteral("library"), {}, {sourceItem});

        MediaSearchModel search;
        search.setSourceModel(store.libraryModel());
        search.setRequireSearchText(true);

        search.setSearchText(QStringLiteral("ＡＢＣ CAFÉ"));
        QCOMPARE(search.rowCount(), 1);
        search.setSearchText(QStringLiteral("abc CAFÉ"));
        QCOMPARE(search.rowCount(), 0);
        search.setSearchText(QStringLiteral("finale"));
        QCOMPARE(search.rowCount(), 1);
        search.setSearchText(QStringLiteral("hidden alias"));
        QCOMPARE(search.rowCount(), 0);

        store.patchEntity({
            {QStringLiteral("id"), QStringLiteral("series-1")},
            {QStringLiteral("title"), QStringLiteral("Renamed Series")},
        });
        QSignalSpy searchChanged(&search, &QAbstractItemModel::dataChanged);
        search.setSearchText(QStringLiteral("renamed"));
        QCOMPARE(search.rowCount(), 1);
        store.patchEntity({
            {QStringLiteral("id"), QStringLiteral("series-1")},
            {QStringLiteral("imageUrl"), QStringLiteral("updated.jpg")},
        });
        QCOMPARE(searchChanged.count(), 1);
        QCOMPARE(search.get(0).value(QStringLiteral("imageUrl")).toString(),
                 QStringLiteral("updated.jpg"));
        search.setSearchText(QStringLiteral("abc"));
        QCOMPARE(search.rowCount(), 0);
    }

    void indexedSearchMatchesLegacyCaseInsensitiveFieldPredicate()
    {
        const QVariantList items {
            item(QStringLiteral("1"), QStringLiteral("Straße"), {},
                 QStringLiteral("Series"), QStringLiteral("Finale")),
            item(QStringLiteral("2"), QStringLiteral("STRASSE"), {},
                 QStringLiteral("Series"), QStringLiteral("İstanbul")),
            item(QStringLiteral("3"), QStringLiteral("ＡＢＣ Café"), {},
                 QStringLiteral("Series"), QStringLiteral("银河列车")),
            item(QStringLiteral("4"), QStringLiteral("Cafe\u0301"), {},
                 QStringLiteral("Series"), QStringLiteral("Emoji 😀")),
        };
        MediaStore store;
        store.setQuery(QStringLiteral("library"), {}, items);
        MediaSearchModel search;
        search.setRequireSearchText(true);
        search.setSourceModel(store.libraryModel());

        const QStringList queries {
            QStringLiteral("straße"),
            QStringLiteral("STRASSE"),
            QStringLiteral("istanbul"),
            QStringLiteral("İST"),
            QStringLiteral("abc"),
            QStringLiteral("ＡＢＣ"),
            QStringLiteral("CAFÉ"),
            QStringLiteral("cafe\u0301"),
            QStringLiteral("银河"),
            QStringLiteral("😀"),
            QStringLiteral("missing"),
        };
        for (const QString &query : queries) {
            int expected = 0;
            for (const QVariant &value : items) {
                const QVariantMap sourceItem = value.toMap();
                if (sourceItem.value(QStringLiteral("title")).toString().contains(
                        query, Qt::CaseInsensitive)
                    || sourceItem.value(QStringLiteral("subtitle")).toString().contains(
                        query, Qt::CaseInsensitive)
                    || sourceItem.value(QStringLiteral("seriesTitle")).toString().contains(
                        query, Qt::CaseInsensitive)) {
                    ++expected;
                }
            }
            search.setSearchText(query);
            QCOMPARE(search.rowCount(), expected);
        }
    }

    void batchPatchCoalescesModelNotificationsAndKeepsSearchRowsLive()
    {
        MediaStore store;
        store.setQuery(QStringLiteral("library"), {}, {
            item(QStringLiteral("series-1"), QStringLiteral("Series One"), QStringLiteral("1.jpg")),
            item(QStringLiteral("series-2"), QStringLiteral("Series Two"), QStringLiteral("2.jpg")),
            item(QStringLiteral("series-3"), QStringLiteral("Series Three"), QStringLiteral("3.jpg")),
        });
        MediaSearchModel search;
        search.setRequireSearchText(true);
        search.setSourceModel(store.libraryModel());
        search.setSearchText(QStringLiteral("series"));
        QCOMPARE(search.rowCount(), 3);

        QSignalSpy sourceChanged(store.libraryModel(), &QAbstractItemModel::dataChanged);
        QSignalSpy searchChanged(&search, &QAbstractItemModel::dataChanged);
        QSignalSpy entityChanged(&store, &MediaStore::entityChanged);
        store.patchEntities({
            QVariantMap{
                {QStringLiteral("id"), QStringLiteral("series-1")},
                {QStringLiteral("title"), QStringLiteral("Series One Updated")},
            },
            QVariantMap{
                {QStringLiteral("id"), QStringLiteral("series-3")},
                {QStringLiteral("imageUrl"), QStringLiteral("updated.jpg")},
            },
        });

        QCOMPARE(sourceChanged.count(), 1);
        QCOMPARE(searchChanged.count(), 1);
        QCOMPARE(entityChanged.count(), 2);
        QCOMPARE(search.rowCount(), 3);
        QCOMPARE(search.get(0).value(QStringLiteral("title")).toString(),
                 QStringLiteral("Series One Updated"));
        QCOMPARE(search.get(2).value(QStringLiteral("imageUrl")).toString(),
                 QStringLiteral("updated.jpg"));
        search.setSearchText(QStringLiteral("updated"));
        QCOMPARE(search.rowCount(), 1);
        QCOMPARE(search.get(0).value(QStringLiteral("id")).toString(),
                 QStringLiteral("series-1"));
    }

    void searchIndexRebuildsOnceAfterSourceReorder()
    {
        MediaStore store;
        const QVariantMap one = item(
            QStringLiteral("series-1"), QStringLiteral("Series One"), QStringLiteral("1.jpg"));
        const QVariantMap two = item(
            QStringLiteral("series-2"), QStringLiteral("Series Two"), QStringLiteral("2.jpg"));
        const QVariantMap three = item(
            QStringLiteral("series-3"), QStringLiteral("Series Three"), QStringLiteral("3.jpg"));
        store.setQuery(QStringLiteral("library"), {}, {one, two, three});
        MediaSearchModel search;
        search.setRequireSearchText(true);
        search.setSourceModel(store.libraryModel());
        search.setSearchText(QStringLiteral("series"));

        QSignalSpy resets(&search, &QAbstractItemModel::modelReset);
        QSignalSpy layouts(&search, &QAbstractItemModel::layoutChanged);
        QSignalSpy sourceLayouts(
            store.libraryModel(), &QAbstractItemModel::layoutChanged);
        QSignalSpy sourceMoves(
            store.libraryModel(), &QAbstractItemModel::rowsMoved);
        const QPersistentModelIndex firstSourceItem(
            store.libraryModel()->index(0, 0));
        const QPersistentModelIndex firstItem(search.index(0, 0));
        store.setQuery(QStringLiteral("library"), {}, {three, one, two});

        QCOMPARE(resets.count(), 0);
        QCOMPARE(layouts.count(), 1);
        QCOMPARE(sourceLayouts.count(), 1);
        QCOMPARE(sourceMoves.count(), 0);
        QCOMPARE(search.rowCount(), 3);
        QCOMPARE(search.get(0).value(QStringLiteral("id")).toString(),
                 QStringLiteral("series-3"));
        QCOMPARE(search.get(1).value(QStringLiteral("id")).toString(),
                 QStringLiteral("series-1"));
        QCOMPARE(firstItem.row(), 1);
        QCOMPARE(firstItem.data(MediaQueryModel::EntityIdRole).toString(),
                 QStringLiteral("series-1"));
        QCOMPARE(firstSourceItem.row(), 1);
        QCOMPARE(firstSourceItem.data(MediaQueryModel::EntityIdRole).toString(),
                 QStringLiteral("series-1"));
    }

    void searchForwardsDecorationOnlyRefreshAfterRowSynchronization()
    {
        MediaStore store;
        store.setQuery(QStringLiteral("library"), {}, {
            item(QStringLiteral("series-1"), QStringLiteral("Series One"),
                 QStringLiteral("1.jpg"), QStringLiteral("Series"),
                 QStringLiteral("Old subtitle")),
        });
        MediaSearchModel search;
        search.setRequireSearchText(true);
        search.setSourceModel(store.libraryModel());
        search.setSearchText(QStringLiteral("series"));
        QCOMPARE(search.rowCount(), 1);

        QSignalSpy changed(&search, &QAbstractItemModel::dataChanged);
        QSignalSpy resets(&search, &QAbstractItemModel::modelReset);
        store.setQuery(QStringLiteral("library"), {}, {
            item(QStringLiteral("series-1"), QStringLiteral("Series One"),
                 QStringLiteral("1.jpg"), QStringLiteral("Series"),
                 QStringLiteral("New subtitle")),
        });

        QCOMPARE(resets.count(), 0);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(search.get(0).value(QStringLiteral("subtitle")).toString(),
                 QStringLiteral("New subtitle"));
    }

    void latestRequestSupersedesOnlyItsLane()
    {
        RequestCoordinator requests;
        constexpr quint64 session = 7;
        const LatestRequestToken firstLoad = requests.beginLatest(
            QStringLiteral("image-editor.load"),
            QStringLiteral("item-a"),
            session);
        const LatestRequestToken firstSearch = requests.beginLatest(
            QStringLiteral("image-editor.search"),
            QStringLiteral("item-a:primary"),
            session);
        QVERIFY(requests.acceptsLatest(firstLoad, session));
        QVERIFY(requests.acceptsLatest(firstSearch, session));

        const LatestRequestToken secondLoad = requests.beginLatest(
            QStringLiteral("image-editor.load"),
            QStringLiteral("item-b"),
            session);
        QVERIFY(!requests.acceptsLatest(firstLoad, session));
        QVERIFY(requests.acceptsLatest(secondLoad, session));
        QVERIFY(requests.acceptsLatest(firstSearch, session));
        QVERIFY(secondLoad.requestId > firstSearch.requestId);
    }

    void equivalentLatestRequestCanBeCoalesced()
    {
        RequestCoordinator requests;
        constexpr quint64 session = 11;
        const LatestRequestToken request = requests.beginLatest(
            QStringLiteral("image-editor.search"),
            QStringLiteral("item-a:backdrop:cats"),
            session);
        QVERIFY(requests.representsLatest(
            request,
            QStringLiteral("image-editor.search"),
            QStringLiteral("item-a:backdrop:cats"),
            session));
        QVERIFY(!requests.representsLatest(
            request,
            QStringLiteral("image-editor.search"),
            QStringLiteral("item-a:backdrop:dogs"),
            session));

        requests.beginLatest(
            QStringLiteral("image-editor.search"),
            QStringLiteral("item-a:backdrop:dogs"),
            session);
        QVERIFY(!requests.representsLatest(
            request,
            QStringLiteral("image-editor.search"),
            QStringLiteral("item-a:backdrop:cats"),
            session));
    }

    void interactivePresentationLanesRemainIndependent()
    {
        RequestCoordinator requests;
        constexpr quint64 session = 13;
        const LatestRequestToken metadata = requests.beginLatest(
            QStringLiteral("metadata-editor.load"),
            QStringLiteral("item-a"),
            session);
        const LatestRequestToken state = requests.beginLatest(
            QStringLiteral("media-state.load"),
            QStringLiteral("item-a"),
            session);
        const LatestRequestToken targets = requests.beginLatest(
            QStringLiteral("media-targets.load"),
            QStringLiteral("item-a"),
            session);
        const LatestRequestToken danmakuSearch = requests.beginLatest(
            QStringLiteral("danmaku.search"),
            QStringLiteral("item-a:anime-title"),
            session);
        const LatestRequestToken images = requests.beginLatest(
            QStringLiteral("image-editor.load"),
            QStringLiteral("item-a"),
            session);
        const LatestRequestToken imageProviders = requests.beginLatest(
            QStringLiteral("image-editor.providers"),
            QStringLiteral("item-a"),
            session);
        const LatestRequestToken search = requests.beginLatest(
            QStringLiteral("image-editor.search"),
            QStringLiteral("item-a:primary"),
            session);

        QVERIFY(requests.acceptsLatest(metadata, session));
        QVERIFY(requests.acceptsLatest(state, session));
        QVERIFY(requests.acceptsLatest(targets, session));
        QVERIFY(requests.acceptsLatest(danmakuSearch, session));
        QVERIFY(requests.acceptsLatest(images, session));
        QVERIFY(requests.acceptsLatest(imageProviders, session));
        QVERIFY(requests.acceptsLatest(search, session));

        const LatestRequestToken newerMetadata = requests.beginLatest(
            QStringLiteral("metadata-editor.load"),
            QStringLiteral("item-b"),
            session);
        QVERIFY(!requests.acceptsLatest(metadata, session));
        QVERIFY(requests.acceptsLatest(newerMetadata, session));
        QVERIFY(requests.acceptsLatest(state, session));
        QVERIFY(requests.acceptsLatest(targets, session));
        QVERIFY(requests.acceptsLatest(danmakuSearch, session));
        QVERIFY(requests.acceptsLatest(images, session));
        QVERIFY(requests.acceptsLatest(imageProviders, session));
        QVERIFY(requests.acceptsLatest(search, session));
        QVERIFY(newerMetadata.requestId > search.requestId);
    }

    void latestRequestIsFencedBySessionAndInvalidation()
    {
        RequestCoordinator requests;
        const LatestRequestToken laneRequest = requests.beginLatest(
            QStringLiteral("image-editor.load"),
            QStringLiteral("item-a"),
            3);
        QVERIFY(!requests.acceptsLatest(laneRequest, 4));
        QVERIFY(requests.acceptsLatest(laneRequest, 3));
        requests.invalidateLatestLane(QStringLiteral("image-editor.load"));
        QVERIFY(!requests.acceptsLatest(laneRequest, 3));

        const LatestRequestToken resetRequest = requests.beginLatest(
            QStringLiteral("image-editor.search"),
            QStringLiteral("item-a:primary"),
            3);
        QVERIFY(requests.acceptsLatest(resetRequest, 3));
        requests.invalidateAll();
        QVERIFY(!requests.acceptsLatest(resetRequest, 3));
    }

    void playbackLaneStartsWhileBackgroundLaneIsBlocked()
    {
        QThreadPool backgroundPool;
        backgroundPool.setMaxThreadCount(1);
        QThreadPool playbackPool;
        playbackPool.setMaxThreadCount(1);
        QSemaphore backgroundStarted;
        QSemaphore releaseBackground;
        QSemaphore playbackStarted;

        backgroundPool.start([&] {
            backgroundStarted.release();
            releaseBackground.acquire();
        });
        QVERIFY2(backgroundStarted.tryAcquire(1, 1000),
                 "the controlled background request never reached its barrier");

        playbackPool.start([&] { playbackStarted.release(); });
        QVERIFY2(playbackStarted.tryAcquire(1, 1000),
                 "playback was queued behind an unrelated background request");

        releaseBackground.release();
        QVERIFY(playbackPool.waitForDone(1000));
        QVERIFY(backgroundPool.waitForDone(1000));
    }

    void interactiveLaneStartsWhileBackgroundLaneIsBlocked()
    {
        QThreadPool backgroundPool;
        backgroundPool.setMaxThreadCount(1);
        QThreadPool interactivePool;
        interactivePool.setMaxThreadCount(4);
        QSemaphore backgroundStarted;
        QSemaphore releaseBackground;
        QSemaphore interactiveStarted;

        backgroundPool.start([&] {
            backgroundStarted.release();
            releaseBackground.acquire();
        });
        QVERIFY(backgroundStarted.tryAcquire(1, 1000));

        interactivePool.start([&] { interactiveStarted.release(); });
        QVERIFY2(interactiveStarted.tryAcquire(1, 1000),
                 "interactive query was queued behind a background refresh");

        releaseBackground.release();
        QVERIFY(backgroundPool.waitForDone(1000));
        QVERIFY(interactivePool.waitForDone(1000));
    }

    void playbackLaneIsLatestWinsWhilePriorTransportIsBlocked()
    {
        RequestCoordinator requests;
        constexpr quint64 session = 23;
        QThreadPool playbackPool;
        playbackPool.setMaxThreadCount(1);
        QSemaphore firstStarted;
        QSemaphore releaseFirst;

        const LatestRequestToken first = requests.beginLatest(
            QStringLiteral("playback.prepare"),
            QStringLiteral("episode-a:context-a"),
            session);
        playbackPool.start([&] {
            firstStarted.release();
            releaseFirst.acquire();
        });
        QVERIFY2(firstStarted.tryAcquire(1, 1000),
                 "the first playback transport never reached its barrier");
        QVERIFY(requests.acceptsLatest(first, session));

        const LatestRequestToken second = requests.beginLatest(
            QStringLiteral("playback.prepare"),
            QStringLiteral("episode-b:context-b"),
            session);
        QVERIFY(!requests.acceptsLatest(first, session));
        QVERIFY(requests.acceptsLatest(second, session));
        QVERIFY(second.requestId > first.requestId);

        releaseFirst.release();
        QVERIFY(playbackPool.waitForDone(1000));
    }

    void playbackLaneRejectsPreviousSessionResults()
    {
        RequestCoordinator requests;
        const LatestRequestToken request = requests.beginLatest(
            QStringLiteral("playback.prepare"),
            QStringLiteral("episode-a:context-a"),
            31);
        QVERIFY(requests.acceptsLatest(request, 31));
        QVERIFY(!requests.acceptsLatest(request, 32));

        requests.invalidateLatestLane(QStringLiteral("playback.prepare"));
        QVERIFY(!requests.acceptsLatest(request, 31));
    }

    void navigationPoolStartsCollectionWhileLibraryIsBlocked()
    {
        QThreadPool navigationPool;
        navigationPool.setMaxThreadCount(2);
        QSemaphore libraryStarted;
        QSemaphore releaseLibrary;
        QSemaphore collectionStarted;
        QSemaphore collectionFinished;

        navigationPool.start([&] {
            libraryStarted.release();
            releaseLibrary.acquire();
        });
        QVERIFY2(libraryStarted.tryAcquire(1, 1000),
                 "the controlled library transport never reached its barrier");

        navigationPool.start([&] {
            collectionStarted.release();
            collectionFinished.release();
        });
        QVERIFY2(collectionStarted.tryAcquire(1, 1000),
                 "collection transport was queued behind the blocked library");
        QVERIFY2(collectionFinished.tryAcquire(1, 1000),
                 "collection transport did not complete independently");

        releaseLibrary.release();
        QVERIFY(navigationPool.waitForDone(1000));
    }

    void navigationPresentationIsLatestWinsAndResourceIsSessionFenced()
    {
        RequestCoordinator requests;
        constexpr quint64 session = 41;
        const RequestCommitToken collectionAResource = requests.begin(
            QStringLiteral("collection:a"), session);
        const LatestRequestToken collectionA = requests.beginLatest(
            QStringLiteral("navigation.collection"),
            QStringLiteral("collection:a"),
            session);
        const LatestRequestToken library = requests.beginLatest(
            QStringLiteral("navigation.library"),
            QStringLiteral("library"),
            session);

        const LatestRequestToken collectionB = requests.beginLatest(
            QStringLiteral("navigation.collection"),
            QStringLiteral("collection:b"),
            session);
        QVERIFY(!requests.acceptsLatest(collectionA, session));
        QVERIFY(requests.acceptsLatest(collectionB, session));
        QVERIFY(requests.acceptsLatest(library, session));
        QVERIFY(requests.accepts(collectionAResource, session));

        // The superseded A response is still safe to cache until the resource
        // itself is invalidated. It must never commit after a session switch.
        requests.invalidate(QStringLiteral("collection:a"));
        QVERIFY(!requests.accepts(collectionAResource, session));
        QVERIFY(!requests.acceptsLatest(collectionB, session + 1));
        QVERIFY(!requests.acceptsLatest(library, session + 1));
    }
};

QTEST_GUILESS_MAIN(MediaStoreTests)

#include "MediaStoreTests.moc"
