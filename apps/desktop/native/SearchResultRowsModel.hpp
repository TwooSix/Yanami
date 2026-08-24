#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

#include <optional>

class MediaQueryModel;

// Adapts the two semantic search result models into a small visual-row model.
// QML can then use one virtualized ListView: header rows remain full-width and
// each card row contains no more than the current number of columns.
class SearchResultRowsModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int columns READ columns WRITE setColumns NOTIFY columnsChanged)

public:
    enum Role {
        RowTypeRole = Qt::UserRole + 1,
        MediaSectionRole,
        ItemsRole,
        StartIndexRole,
        ItemCountRole,
        RowKeyRole,
    };

    explicit SearchResultRowsModel(
        MediaQueryModel *titleResults,
        MediaQueryModel *episodeResults,
        QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(
        const QModelIndex &index,
        int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int columns() const { return m_columns; }
    void setColumns(int columns);

    Q_INVOKABLE int rowFor(
        const QString &mediaSection,
        int sourceIndex) const;
    Q_INVOKABLE int columnFor(
        const QString &mediaSection,
        int sourceIndex) const;
    Q_INVOKABLE int sourceIndexForId(
        const QString &mediaSection,
        const QString &itemId) const;
    Q_INVOKABLE int rowForId(
        const QString &mediaSection,
        const QString &itemId) const;
    Q_INVOKABLE int rowForKey(const QString &rowKey) const;

signals:
    void countChanged();
    void columnsChanged();
    void rowsAboutToBeRebuilt();
    void rowsRebuilt();

private:
    enum class RowType {
        Header,
        Cards,
    };
    enum class MediaSection {
        Titles,
        Episodes,
    };
    struct Row
    {
        RowType type = RowType::Header;
        MediaSection section = MediaSection::Titles;
        int startIndex = 0;
        int itemCount = 0;
        QString rowKey;

        bool operator==(const Row &) const = default;
    };

    static QString sectionName(MediaSection section);
    static std::optional<MediaSection> parseSection(const QString &section);
    MediaQueryModel *sourceModel(MediaSection section) const;
    QVariantList itemsForRow(const Row &row) const;
    QVector<Row> buildRows() const;
    void sourceDataChanged(
        MediaSection section,
        int firstSourceRow,
        int lastSourceRow);
    void scheduleRebuild();
    void rebuild();
    void synchronizeRows(QVector<Row> rows);

    QPointer<MediaQueryModel> m_titleResults;
    QPointer<MediaQueryModel> m_episodeResults;
    QTimer m_rebuildTimer;
    QVector<Row> m_rows;
    int m_columns = 1;
};
