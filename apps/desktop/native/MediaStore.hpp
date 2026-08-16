#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class MediaStore;

struct MediaQueryRow
{
    QString rowKey;
    QString entityId;
    QVariantMap decoration;

    bool operator==(const MediaQueryRow &) const = default;
};

class MediaQueryModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        ModelDataRole = Qt::UserRole + 1,
        EntityIdRole,
        RowKeyRole,
    };

    explicit MediaQueryModel(
        MediaStore *store,
        QString kind,
        QString scopeId,
        QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QVariantMap get(int row) const;
    QString kind() const { return m_kind; }
    QString scopeId() const { return m_scopeId; }
    qint64 fetchedAtMs() const { return m_fetchedAtMs; }
    quint64 contentRevision() const { return m_contentRevision; }
    bool stale() const { return m_stale; }
    QString parentId() const { return m_parentId; }

signals:
    void countChanged();
    void stateChanged();

private:
    friend class MediaStore;

    void synchronizeRows(QVector<MediaQueryRow> rows);
    void notifyEntityChanged(const QString &entityId);
    bool removeFirst(const QString &entityId, const QString &rowKey, int *removedIndex = nullptr,
                     MediaQueryRow *removedRow = nullptr);
    void restoreRow(int index, const MediaQueryRow &row);
    void clearRows();

    MediaStore *m_store = nullptr;
    QString m_kind;
    QString m_scopeId;
    QVector<MediaQueryRow> m_rows;
    QString m_parentId;
    QVariantMap m_parentDecoration;
    qint64 m_fetchedAtMs = 0;
    quint64 m_contentRevision = 0;
    bool m_stale = true;
};

class MediaStore final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(MediaQueryModel *libraryModel READ libraryModel CONSTANT)
    Q_PROPERTY(MediaQueryModel *libraryViewsModel READ libraryViewsModel CONSTANT)
    Q_PROPERTY(MediaQueryModel *resumeModel READ resumeModel CONSTANT)
    Q_PROPERTY(MediaQueryModel *recentModel READ recentModel CONSTANT)
    Q_PROPERTY(MediaQueryModel *favoritesModel READ favoritesModel CONSTANT)

public:
    explicit MediaStore(QObject *parent = nullptr);

    MediaQueryModel *libraryModel() const;
    MediaQueryModel *libraryViewsModel() const;
    MediaQueryModel *resumeModel() const;
    MediaQueryModel *recentModel() const;
    MediaQueryModel *favoritesModel() const;

    Q_INVOKABLE MediaQueryModel *queryModel(
        const QString &kind,
        const QString &scopeId = QString());
    Q_INVOKABLE QVariantMap entity(const QString &entityId) const;
    Q_INVOKABLE QVariantMap queryParent(
        const QString &kind,
        const QString &scopeId = QString()) const;
    Q_INVOKABLE bool hasQuery(
        const QString &kind,
        const QString &scopeId = QString()) const;

    QVariantList queryItems(
        const QString &kind,
        const QString &scopeId = QString()) const;
    qint64 queryFetchedAtMs(
        const QString &kind,
        const QString &scopeId = QString()) const;
    bool queryStale(
        const QString &kind,
        const QString &scopeId = QString()) const;

    void setQuery(
        const QString &kind,
        const QString &scopeId,
        const QVariantList &items,
        const QVariantMap &parent = {},
        qint64 fetchedAtMs = 0);
    void patchEntity(const QVariantMap &patch, const QSet<QString> &removedFields = {});
    void patchEntities(const QVariantList &patches);
    void beginRefreshProtection(const QString &entityId);
    void endRefreshProtection(const QString &entityId);
    void markQueryStale(const QString &kind, const QString &scopeId = QString());
    void markAllQueriesStale();
    bool removeQueryRow(
        const QString &kind,
        const QString &scopeId,
        const QString &entityId,
        const QString &rowKey = QString(),
        bool journalRemoval = false);

    void beginMutation(const QString &mutationId, const QString &entityId);
    void applyMutationPatch(const QVariantMap &patch);
    void commitMutation(const QString &mutationId);
    void rollbackMutation(const QString &mutationId);
    void clear();

    QJsonObject toCacheJson(const QSet<QString> &queryKinds) const;
    bool restoreCacheJson(const QJsonObject &object);

signals:
    void entityChanged(const QString &entityId);
    void queryChanged(const QString &kind, const QString &scopeId);
    void storeReset();

private:
    friend class MediaQueryModel;
    struct EntityRecord {
        QVariantMap fields;
        QVariantMap overlay;
        quint64 revision = 0;
    };
    struct RemovedRow {
        QString queryKey;
        int index = -1;
        MediaQueryRow row;
        quint64 expectedRevision = 0;
    };
    struct MutationJournal {
        QString entityId;
        QVariantMap previousOverlay;
        QVector<RemovedRow> removedRows;
    };

    static QString normalizedKind(const QString &kind);
    static QString makeQueryKey(const QString &kind, const QString &scopeId);
    static QVariantMap canonicalFields(const QVariantMap &item, const QString &kind);
    static QVariantMap rowDecoration(const QVariantMap &item, const QString &kind);
    static QVector<MediaQueryRow> rowsFromItems(
        const QVariantList &items,
        const QString &kind);
    MediaQueryModel *ensureQueryModel(const QString &kind, const QString &scopeId);
    QVariantMap materialize(const MediaQueryRow &row, const QString &kind) const;
    void notifyEntityChanged(const QString &entityId);

    QHash<QString, EntityRecord> m_entities;
    QHash<QString, MediaQueryModel *> m_queries;
    QHash<QString, MutationJournal> m_mutations;
    QHash<QString, QString> m_refreshSourceBaselines;
    QString m_activeMutationId;
};

class MediaQueryProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString category READ category WRITE setCategory NOTIFY categoryChanged)
    Q_PROPERTY(bool requireSearchText READ requireSearchText WRITE setRequireSearchText NOTIFY requireSearchTextChanged)
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    Q_PROPERTY(QString sortLocale READ sortLocale WRITE setSortLocale NOTIFY sortLocaleChanged)

public:
    explicit MediaQueryProxyModel(QObject *parent = nullptr);

    QString searchText() const { return m_searchText; }
    QString category() const { return m_category; }
    bool requireSearchText() const { return m_requireSearchText; }
    int sortMode() const { return m_sortMode; }
    QString sortLocale() const { return m_sortLocale; }

    void setSearchText(const QString &value);
    void setCategory(const QString &value);
    void setRequireSearchText(bool value);
    void setSortMode(int value);
    void setSortLocale(const QString &value);
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void countChanged();
    void searchTextChanged();
    void categoryChanged();
    void requireSearchTextChanged();
    void sortModeChanged();
    void sortLocaleChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override;

private:
    QVariantMap sourceItem(const QModelIndex &index) const;
    void updateSorting();

    QString m_searchText;
    QString m_category;
    bool m_requireSearchText = false;
    QString m_sortLocale = QStringLiteral("en");
    int m_sortMode = -1;
};
