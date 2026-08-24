#include "BackendInfrastructure.hpp"
#include "SearchCoordinator.hpp"
#include "SearchResultRowsModel.hpp"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTest>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace {

YanamiOperationResult searchResponse(
    const QString &query,
    const QString &catalogRevision,
    int itemCount,
    bool complete = true,
    bool syncing = false,
    bool indexError = false)
{
    QJsonObject entities;
    QJsonArray rows;
    for (int index = 0; index < itemCount; ++index) {
        const bool episode = index % 2 != 0;
        const QString itemId = QStringLiteral("%1-item-%2")
                                   .arg(query)
                                   .arg(index);
        QJsonObject entity {
            {QStringLiteral("id"), itemId},
            {QStringLiteral("title"), QStringLiteral("%1-%2-title-%3")
                 .arg(query, catalogRevision).arg(index)},
            {QStringLiteral("itemType"), episode
                 ? QStringLiteral("Episode") : QStringLiteral("Movie")},
            {QStringLiteral("imageTag"), QStringLiteral("%1-%2-poster")
                 .arg(itemId, catalogRevision)},
            {QStringLiteral("sourceVersion"), catalogRevision},
        };
        if (episode) {
            entity.insert(QStringLiteral("seriesTitle"),
                QStringLiteral("%1-series").arg(query));
            entity.insert(QStringLiteral("subtitle"),
                QStringLiteral("S01E01 · episode"));
            entity.insert(QStringLiteral("titleIsContextual"), true);
            entity.insert(QStringLiteral("imageItemId"),
                QStringLiteral("%1-series-owner").arg(query));
            entity.insert(QStringLiteral("imageItemType"),
                QStringLiteral("Series"));
        }
        entities.insert(itemId, entity);
        rows.push_back(QJsonObject {
            {QStringLiteral("rowKey"), itemId},
            {QStringLiteral("entityId"), itemId},
            {QStringLiteral("decoration"), QJsonObject {}},
        });
    }

    QJsonObject searchStatus {
        {QStringLiteral("query"), query},
        {QStringLiteral("catalogRevision"), catalogRevision},
        {QStringLiteral("cachedCount"), 100},
        {QStringLiteral("totalCount"), 100},
        {QStringLiteral("totalMatches"), itemCount},
        {QStringLiteral("hasMore"), false},
        {QStringLiteral("complete"), complete},
        {QStringLiteral("syncing"), syncing},
        {QStringLiteral("indexError"), indexError},
    };
    if (indexError) {
        searchStatus.insert(QStringLiteral("indexErrorDetail"),
            QStringLiteral("controlled index failure"));
    }

    const QJsonObject document {
        {QStringLiteral("schemaVersion"), 8},
        {QStringLiteral("entities"), entities},
        {QStringLiteral("queries"), QJsonObject {
             {QStringLiteral("search"), QJsonObject {
                  {QStringLiteral("scopeId"), query},
                  {QStringLiteral("parentId"), QString()},
                  {QStringLiteral("parentDecoration"), QJsonObject {}},
                  {QStringLiteral("rows"), rows},
              }},
         }},
        {QStringLiteral("searchStatus"), searchStatus},
    };
    YanamiOperationResult result;
    result.status = 0;
    result.payload = QJsonDocument(document).toJson(QJsonDocument::Compact);
    return result;
}

class TestStatusSink final : public StatusSink
{
public:
    void publishStatus(const QString &, bool) override {}

    QString userFacingBackendError(
        const QString &,
        const QString & = {}) const override
    {
        return QStringLiteral("controlled backend failure");
    }

    QString userFacingDanmakuError(
        const QString &,
        const QString & = {}) const override
    {
        return QStringLiteral("controlled danmaku failure");
    }
};

class ControlledSearchBackend final
{
public:
    YanamiOperationResult search(const QString &query)
    {
        QString revision;
        int itemCount = 0;
        bool failStatus = false;
        bool blockStatus = false;
        bool blockFull = false;
        bool complete = true;
        bool syncing = false;
        bool indexError = false;
        {
            const std::lock_guard lock(m_mutex);
            revision = m_revision;
            itemCount = m_itemCount;
            complete = m_complete;
            syncing = m_syncing;
            indexError = m_indexError;
            if (query.isEmpty()) {
                ++m_statusCalls;
                failStatus = m_failStatus;
                blockStatus = m_blockNextStatus;
                m_blockNextStatus = false;
            } else {
                ++m_fullCalls;
                ++m_fullCallsByQuery[query];
                blockFull = m_blockNextFullQuery == query;
                if (blockFull)
                    m_blockNextFullQuery.clear();
            }
        }

        if (blockFull) {
            m_fullStarted.release();
            if (!m_fullRelease.tryAcquire(1, 5'000)) {
                YanamiOperationResult timeout;
                timeout.status = 1;
                timeout.errorCode = QStringLiteral("controlled_timeout");
                timeout.error = QStringLiteral("full search release timed out");
                return timeout;
            }
        }

        if (query.isEmpty()) {
            m_statusStarted.release();
            if (blockStatus && !m_statusRelease.tryAcquire(1, 5'000)) {
                YanamiOperationResult timeout;
                timeout.status = 1;
                timeout.errorCode = QStringLiteral("controlled_timeout");
                timeout.error = QStringLiteral("status release timed out");
                m_statusCompleted.fetch_add(1, std::memory_order_release);
                return timeout;
            }
            m_statusCompleted.fetch_add(1, std::memory_order_release);
            if (failStatus) {
                YanamiOperationResult failure;
                failure.status = 1;
                failure.errorCode = QStringLiteral("controlled_failure");
                failure.error = QStringLiteral("status poll failed");
                return failure;
            }
            return searchResponse(
                QString(), revision, 0, complete, syncing, indexError);
        }
        return searchResponse(
            query, revision, itemCount, complete, syncing, indexError);
    }

    YanamiOperationResult hydrate(const QVariantMap &)
    {
        m_hydrationCalls.fetch_add(1, std::memory_order_release);
        YanamiOperationResult result;
        result.status = 0;
        return result;
    }

    void setCatalog(QString revision, int itemCount)
    {
        const std::lock_guard lock(m_mutex);
        m_revision = std::move(revision);
        m_itemCount = itemCount;
    }

    void setStatusFailure(bool fail)
    {
        const std::lock_guard lock(m_mutex);
        m_failStatus = fail;
    }

    void setIndexerStatus(bool complete, bool syncing, bool indexError)
    {
        const std::lock_guard lock(m_mutex);
        m_complete = complete;
        m_syncing = syncing;
        m_indexError = indexError;
    }

    void blockNextStatus()
    {
        const std::lock_guard lock(m_mutex);
        m_blockNextStatus = true;
    }

    void releaseStatus() { m_statusRelease.release(); }
    bool waitForStatusStart(int timeoutMs = 2'000)
    {
        return m_statusStarted.tryAcquire(1, timeoutMs);
    }

    void blockNextFull(const QString &query)
    {
        const std::lock_guard lock(m_mutex);
        m_blockNextFullQuery = query;
    }

    void releaseFull() { m_fullRelease.release(); }
    bool waitForFullStart(int timeoutMs = 2'000)
    {
        return m_fullStarted.tryAcquire(1, timeoutMs);
    }

    int statusCalls() const
    {
        const std::lock_guard lock(m_mutex);
        return m_statusCalls;
    }

    int statusCompleted() const
    {
        return m_statusCompleted.load(std::memory_order_acquire);
    }

    int fullCalls() const
    {
        const std::lock_guard lock(m_mutex);
        return m_fullCalls;
    }

    int fullCallsFor(const QString &query) const
    {
        const std::lock_guard lock(m_mutex);
        return m_fullCallsByQuery.value(query);
    }

    int hydrationCalls() const
    {
        return m_hydrationCalls.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex m_mutex;
    QString m_revision = QStringLiteral("revision-1");
    int m_itemCount = 2;
    int m_statusCalls = 0;
    int m_fullCalls = 0;
    QHash<QString, int> m_fullCallsByQuery;
    bool m_failStatus = false;
    bool m_blockNextStatus = false;
    QString m_blockNextFullQuery;
    bool m_complete = true;
    bool m_syncing = false;
    bool m_indexError = false;
    QSemaphore m_statusStarted;
    QSemaphore m_statusRelease;
    QSemaphore m_fullStarted;
    QSemaphore m_fullRelease;
    std::atomic<int> m_statusCompleted {0};
    std::atomic<int> m_hydrationCalls {0};
};

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs = 3'000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() >= timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QTest::qWait(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return true;
}

} // namespace

class SearchCoordinatorTests final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        m_backend = std::make_unique<ControlledSearchBackend>();
        m_queryPool.setMaxThreadCount(2);
        m_hydrationPool.setMaxThreadCount(1);
        m_session = {.generation = 1, .connected = true};
        m_publishCount.store(0, std::memory_order_release);
        SearchBackendOperations operations {
            .ready = [] { return true; },
            .searchCatalog = [this](const QString &query) {
                return m_backend->search(query);
            },
            .hydrateCatalogSearchImages = [this](const QVariantMap &payload) {
                return m_backend->hydrate(payload);
            },
        };
        m_coordinator = std::make_unique<SearchCoordinator>(
            std::move(operations),
            m_queryPool,
            m_hydrationPool,
            [this] { return m_session; },
            m_statusSink,
            [this](const SearchCoordinatorEvent &event) {
                if (event.milestone == QStringLiteral("publish_committed"))
                    m_publishCount.fetch_add(1, std::memory_order_release);
            });
        QVERIFY(m_coordinator->initializeFromSession());
    }

    void cleanup()
    {
        if (m_backend)
            m_backend->releaseStatus();
        if (m_backend)
            m_backend->releaseFull();
        if (m_coordinator) {
            m_coordinator->shutdown();
            m_coordinator->drain();
        }
        m_queryPool.waitForDone();
        m_hydrationPool.waitForDone();
        m_coordinator.reset();
        m_backend.reset();
    }

    void unchangedRevisionDoesNotRepublishOrHydrate()
    {
        MediaQueryModel *const results = m_coordinator->resultsModel();
        MediaQueryModel *const titles = m_coordinator->titleResultsModel();
        MediaQueryModel *const episodes = m_coordinator->episodeResultsModel();
        QSignalSpy rowsSynchronized(results,
            &MediaQueryModel::rowsSynchronized);
        QSignalSpy titleRowsSynchronized(titles,
            &MediaQueryModel::rowsSynchronized);
        QSignalSpy episodeRowsSynchronized(episodes,
            &MediaQueryModel::rowsSynchronized);
        QSignalSpy stateChanged(m_coordinator.get(),
            &SearchCoordinator::stateChanged);
        m_coordinator->requestSearch(QStringLiteral("show"));
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching()
                && results->rowCount() == 2;
        }));
        QVERIFY(waitUntil([&] { return m_backend->hydrationCalls() == 1; }));

        const int baselineSynchronizations = rowsSynchronized.count();
        const int baselineTitleSynchronizations =
            titleRowsSynchronized.count();
        const int baselineEpisodeSynchronizations =
            episodeRowsSynchronized.count();
        const int baselineStateChanges = stateChanged.count();
        QCOMPARE(m_publishCount.load(std::memory_order_acquire), 1);
        m_coordinator->refresh();
        QVERIFY(waitUntil([&] { return m_backend->statusCalls() >= 1; }));

        // Starting a third poll proves that the first two watcher completions
        // ran on the coordinator thread; neither may have published work.
        QVERIFY(waitUntil([&] {
            m_coordinator->refresh();
            return m_backend->statusCalls() >= 3;
        }));
        QCOMPARE(m_backend->fullCalls(), 1);
        QCOMPARE(m_backend->hydrationCalls(), 1);
        QCOMPARE(m_publishCount.load(std::memory_order_acquire), 1);
        QCOMPARE(rowsSynchronized.count(), baselineSynchronizations);
        QCOMPARE(titleRowsSynchronized.count(),
            baselineTitleSynchronizations);
        QCOMPARE(episodeRowsSynchronized.count(),
            baselineEpisodeSynchronizations);
        QCOMPARE(stateChanged.count(), baselineStateChanges);
        QCOMPARE(results->rowCount(), 2);
    }

    void changedRevisionUsesStableDiffAndRefreshesLatestQuery()
    {
        MediaQueryModel *const results = m_coordinator->resultsModel();
        m_coordinator->requestSearch(QStringLiteral("show"));
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching()
                && results->rowCount() == 2;
        }));
        QVERIFY(waitUntil([&] { return m_backend->hydrationCalls() == 1; }));

        QSignalSpy modelReset(results, &QAbstractItemModel::modelReset);
        QSignalSpy rowsInserted(results, &QAbstractItemModel::rowsInserted);
        m_backend->setCatalog(QStringLiteral("revision-2"), 3);
        m_coordinator->refresh();
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching()
                && m_backend->fullCalls() == 2
                && results->rowCount() == 3;
        }));
        QVERIFY(waitUntil([&] { return m_backend->hydrationCalls() == 2; }));

        QCOMPARE(m_publishCount.load(std::memory_order_acquire), 2);
        QCOMPARE(m_backend->fullCallsFor(QStringLiteral("show")), 2);
        QCOMPARE(modelReset.count(), 0);
        QVERIFY(rowsInserted.count() > 0);
        QCOMPARE(results->get(0).value(QStringLiteral("id")).toString(),
            QStringLiteral("show-item-0"));
        QCOMPARE(results->get(0).value(QStringLiteral("title")).toString(),
            QStringLiteral("show-revision-2-title-0"));
    }

    void unchangedRevisionAppliesIndexerStateWithoutRepublishing()
    {
        MediaQueryModel *const results = m_coordinator->resultsModel();
        QSignalSpy rowsSynchronized(results,
            &MediaQueryModel::rowsSynchronized);
        m_coordinator->requestSearch(QStringLiteral("show"));
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching()
                && results->rowCount() == 2;
        }));
        QVERIFY(waitUntil([&] { return m_backend->hydrationCalls() == 1; }));
        const int baselineSynchronizations = rowsSynchronized.count();

        m_backend->setIndexerStatus(false, false, true);
        m_coordinator->refresh();
        QVERIFY(waitUntil([&] {
            return !m_coordinator->complete()
                && m_coordinator->error()
                    == QStringLiteral(
                        "Background catalog indexing was interrupted and will retry automatically.");
        }));
        QCOMPARE(m_backend->fullCalls(), 1);
        QCOMPARE(m_backend->hydrationCalls(), 1);
        QCOMPARE(m_publishCount.load(std::memory_order_acquire), 1);
        QCOMPARE(rowsSynchronized.count(), baselineSynchronizations);
        QCOMPARE(results->rowCount(), 2);

        m_backend->setIndexerStatus(false, true, false);
        m_coordinator->refresh();
        QVERIFY(waitUntil([&] {
            return m_coordinator->syncing()
                && m_coordinator->error().isEmpty();
        }));
        QCOMPARE(m_backend->fullCalls(), 1);
        QCOMPARE(m_backend->hydrationCalls(), 1);
        QCOMPARE(m_publishCount.load(std::memory_order_acquire), 1);
        QCOMPARE(rowsSynchronized.count(), baselineSynchronizations);
    }

    void statusFailurePreservesCommittedResults()
    {
        MediaQueryModel *const results = m_coordinator->resultsModel();
        m_coordinator->requestSearch(QStringLiteral("show"));
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching()
                && results->rowCount() == 2;
        }));
        QVERIFY(waitUntil([&] { return m_backend->hydrationCalls() == 1; }));
        const QVariantMap first = results->get(0);
        const QString baselineError = m_coordinator->error();

        m_backend->setStatusFailure(true);
        m_coordinator->refresh();
        QVERIFY(waitUntil([&] { return m_backend->statusCalls() >= 1; }));
        QVERIFY(waitUntil([&] {
            m_coordinator->refresh();
            return m_backend->statusCalls() >= 2;
        }));

        QCOMPARE(m_backend->fullCalls(), 1);
        QCOMPARE(m_backend->hydrationCalls(), 1);
        QCOMPARE(m_publishCount.load(std::memory_order_acquire), 1);
        QCOMPARE(results->rowCount(), 2);
        QCOMPARE(results->get(0), first);
        QCOMPARE(m_coordinator->error(), baselineError);
    }

    void staleStatusCompletionCannotReplaceNewerQuery()
    {
        MediaQueryModel *const results = m_coordinator->resultsModel();
        m_coordinator->requestSearch(QStringLiteral("show"));
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching()
                && results->rowCount() == 2;
        }));

        m_backend->setCatalog(QStringLiteral("revision-2"), 3);
        m_backend->blockNextStatus();
        m_coordinator->refresh();
        QVERIFY(m_backend->waitForStatusStart());
        m_coordinator->requestSearch(QStringLiteral("new"));
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching()
                && m_coordinator->query() == QStringLiteral("new")
                && results->rowCount() == 3;
        }));
        m_backend->releaseStatus();

        // A second status invocation can start only after the stale watcher
        // completion was handled and rejected by its request generation fence.
        QVERIFY(waitUntil([&] {
            m_coordinator->refresh();
            return m_backend->statusCalls() >= 2;
        }));
        QCOMPARE(m_backend->fullCallsFor(QStringLiteral("show")), 1);
        QCOMPARE(m_backend->fullCallsFor(QStringLiteral("new")), 1);
        QCOMPARE(m_publishCount.load(std::memory_order_acquire), 2);
        QCOMPARE(m_coordinator->query(), QStringLiteral("new"));
        QCOMPARE(results->get(0).value(QStringLiteral("id")).toString(),
            QStringLiteral("new-item-0"));
    }

    void inputPendingFencesInFlightRevisionRefresh()
    {
        MediaQueryModel *const results = m_coordinator->resultsModel();
        m_coordinator->requestSearch(QStringLiteral("show"));
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching()
                && results->rowCount() == 2;
        }));
        const int publishedBeforePending =
            m_publishCount.load(std::memory_order_acquire);

        m_backend->setCatalog(QStringLiteral("revision-2"), 3);
        m_backend->blockNextStatus();
        m_coordinator->refresh();
        QVERIFY(m_backend->waitForStatusStart());

        // This is the native half of the QML debounce window: the input text
        // has changed, but the trailing query has not been submitted yet.
        m_coordinator->inputPending();
        m_backend->releaseStatus();
        QVERIFY(waitUntil([&] { return m_backend->statusCompleted() >= 1; }));
        QTest::qWait(20);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

        QCOMPARE(m_backend->fullCallsFor(QStringLiteral("show")), 1);
        QCOMPARE(m_publishCount.load(std::memory_order_acquire),
            publishedBeforePending);
        QCOMPARE(results->rowCount(), 2);
        QCOMPARE(results->get(0).value(QStringLiteral("id")).toString(),
            QStringLiteral("show-item-0"));

        m_coordinator->requestSearch(QStringLiteral("new"));
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching()
                && m_coordinator->query() == QStringLiteral("new")
                && results->rowCount() == 3;
        }));
        QCOMPARE(results->get(0).value(QStringLiteral("id")).toString(),
            QStringLiteral("new-item-0"));
    }

    void visualRowsKeepSectionsVirtualizableAcrossColumnChanges()
    {
        auto *const visualRows = qobject_cast<SearchResultRowsModel *>(
            m_coordinator->resultRowsModel());
        QVERIFY(visualRows);
        QSignalSpy modelResetSpy(visualRows, &QAbstractItemModel::modelReset);
        visualRows->setColumns(8);
        m_backend->setCatalog(QStringLiteral("revision-rows"), 100);

        m_coordinator->requestSearch(QStringLiteral("show"));
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching()
                && visualRows->rowCount() == 16;
        }));

        const auto roles = visualRows->roleNames();
        const int rowTypeRole = roles.key("rowType", -1);
        const int sectionRole = roles.key("mediaSection", -1);
        const int itemsRole = roles.key("items", -1);
        QVERIFY(rowTypeRole >= 0);
        QVERIFY(sectionRole >= 0);
        QVERIFY(itemsRole >= 0);
        QCOMPARE(visualRows->data(visualRows->index(0), rowTypeRole).toString(),
            QStringLiteral("header"));
        QCOMPARE(visualRows->data(visualRows->index(0), sectionRole).toString(),
            QStringLiteral("titles"));
        QCOMPARE(visualRows->data(visualRows->index(1), itemsRole).toList().size(), 8);
        QCOMPARE(visualRows->data(visualRows->index(7), itemsRole).toList().size(), 2);
        QCOMPARE(visualRows->data(visualRows->index(8), sectionRole).toString(),
            QStringLiteral("episodes"));
        QCOMPARE(visualRows->rowFor(QStringLiteral("titles"), 49), 7);
        QCOMPARE(visualRows->rowFor(QStringLiteral("episodes"), 49), 15);
        QCOMPARE(visualRows->columnFor(QStringLiteral("episodes"), 49), 1);
        QCOMPARE(visualRows->sourceIndexForId(
                     QStringLiteral("titles"), QStringLiteral("show-item-98")),
            49);

        int projectedItemCount = 0;
        for (int row = 0; row < visualRows->rowCount(); ++row) {
            const QVariantList items = visualRows->data(
                visualRows->index(row), itemsRole).toList();
            QVERIFY(items.size() <= 8);
            projectedItemCount += items.size();
        }
        QCOMPARE(projectedItemCount, 100);
        QCOMPARE(modelResetSpy.count(), 0);

        visualRows->setColumns(10);
        QVERIFY(waitUntil([&] { return visualRows->rowCount() == 12; }));
        QCOMPARE(visualRows->rowForId(
                     QStringLiteral("episodes"), QStringLiteral("show-item-99")),
            11);
        QCOMPARE(modelResetSpy.count(), 0);
    }

    void emptyQueryClearsImmediatelyAndFencesActiveCompletion()
    {
        MediaQueryModel *const results = m_coordinator->resultsModel();
        MediaQueryModel *const titles = m_coordinator->titleResultsModel();
        MediaQueryModel *const episodes = m_coordinator->episodeResultsModel();
        m_coordinator->requestSearch(QStringLiteral("show"));
        QVERIFY(waitUntil([&] {
            return !m_coordinator->searching() && results->rowCount() == 2;
        }));
        const int publishesBeforeSlowQuery =
            m_publishCount.load(std::memory_order_acquire);

        m_backend->blockNextFull(QStringLiteral("slow"));
        m_coordinator->requestSearch(QStringLiteral("slow"));
        QVERIFY(m_backend->waitForFullStart());
        m_coordinator->requestSearch(QString());

        QCOMPARE(m_coordinator->query(), QString());
        QCOMPARE(results->rowCount(), 0);
        QCOMPARE(titles->rowCount(), 0);
        QCOMPARE(episodes->rowCount(), 0);

        m_backend->releaseFull();
        QVERIFY(waitUntil([&] {
            return m_backend->statusCalls() >= 1
                && m_publishCount.load(std::memory_order_acquire)
                    == publishesBeforeSlowQuery + 1;
        }));
        QCOMPARE(results->rowCount(), 0);
        QCOMPARE(titles->rowCount(), 0);
        QCOMPARE(episodes->rowCount(), 0);
        QCOMPARE(m_backend->fullCallsFor(QStringLiteral("slow")), 1);
        QCOMPARE(m_publishCount.load(std::memory_order_acquire),
            publishesBeforeSlowQuery + 1);
    }

private:
    TestStatusSink m_statusSink;
    QThreadPool m_queryPool;
    QThreadPool m_hydrationPool;
    SearchSessionState m_session;
    std::unique_ptr<ControlledSearchBackend> m_backend;
    std::unique_ptr<SearchCoordinator> m_coordinator;
    std::atomic<int> m_publishCount {0};
};

QTEST_GUILESS_MAIN(SearchCoordinatorTests)

#include "SearchCoordinatorTests.moc"
