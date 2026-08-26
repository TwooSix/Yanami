#include "SearchResultRowsModel.hpp"

#include "MediaStore.hpp"

#include <algorithm>
#include <utility>

namespace {
const QString headerRowType = QStringLiteral("header");
const QString cardsRowType = QStringLiteral("cards");
const QString titlesSection = QStringLiteral("titles");
const QString episodesSection = QStringLiteral("episodes");
}

SearchResultRowsModel::SearchResultRowsModel(
    MediaQueryModel *titleResults,
    MediaQueryModel *episodeResults,
    QObject *parent)
    : QAbstractListModel(parent)
    , m_titleResults(titleResults)
    , m_episodeResults(episodeResults)
{
    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_rebuildTimer, &QTimer::timeout,
        this, &SearchResultRowsModel::rebuild);

    const auto observe = [this](
                             MediaQueryModel *model,
                             MediaSection section) {
        if (!model)
            return;
        connect(model, &MediaQueryModel::countChanged,
            this, &SearchResultRowsModel::scheduleRebuild);
        connect(model, &MediaQueryModel::rowsSynchronized,
            this, &SearchResultRowsModel::scheduleRebuild);
        connect(model, &QAbstractItemModel::dataChanged,
            this, [this, section](
                      const QModelIndex &topLeft,
                      const QModelIndex &bottomRight) {
                sourceDataChanged(
                    section, topLeft.row(), bottomRight.row());
            });
        connect(model, &QAbstractItemModel::modelReset,
            this, &SearchResultRowsModel::scheduleRebuild);
        connect(model, &QAbstractItemModel::layoutChanged,
            this, &SearchResultRowsModel::scheduleRebuild);
    };
    observe(titleResults, MediaSection::Titles);
    observe(episodeResults, MediaSection::Episodes);
    rebuild();
}

int SearchResultRowsModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant SearchResultRowsModel::data(
    const QModelIndex &index,
    int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case RowTypeRole:
        return row.type == RowType::Header ? headerRowType : cardsRowType;
    case MediaSectionRole:
        return sectionName(row.section);
    case ItemsRole:
        return itemsForRow(row);
    case StartIndexRole:
        return row.startIndex;
    case ItemCountRole:
        return row.itemCount;
    case RowKeyRole:
        return row.rowKey;
    default:
        return {};
    }
}

QHash<int, QByteArray> SearchResultRowsModel::roleNames() const
{
    return {
        {RowTypeRole, "rowType"},
        {MediaSectionRole, "mediaSection"},
        {ItemsRole, "items"},
        {StartIndexRole, "startIndex"},
        {ItemCountRole, "itemCount"},
        {RowKeyRole, "rowKey"},
    };
}

void SearchResultRowsModel::setColumns(int columns)
{
    columns = std::max(1, columns);
    if (m_columns == columns)
        return;
    m_columns = columns;
    emit columnsChanged();
    scheduleRebuild();
}

int SearchResultRowsModel::rowFor(
    const QString &mediaSection,
    int sourceIndex) const
{
    const std::optional<MediaSection> section = parseSection(mediaSection);
    if (!section.has_value() || sourceIndex < 0)
        return -1;
    for (int rowIndex = 0; rowIndex < m_rows.size(); ++rowIndex) {
        const Row &row = m_rows.at(rowIndex);
        if (row.type != RowType::Cards || row.section != *section)
            continue;
        if (sourceIndex >= row.startIndex
            && sourceIndex < row.startIndex + row.itemCount) {
            return rowIndex;
        }
    }
    return -1;
}

int SearchResultRowsModel::columnFor(
    const QString &mediaSection,
    int sourceIndex) const
{
    const int rowIndex = rowFor(mediaSection, sourceIndex);
    return rowIndex < 0 ? -1 : sourceIndex - m_rows.at(rowIndex).startIndex;
}

int SearchResultRowsModel::sourceIndexForId(
    const QString &mediaSection,
    const QString &itemId) const
{
    const std::optional<MediaSection> section = parseSection(mediaSection);
    MediaQueryModel *const model = section.has_value()
        ? sourceModel(*section) : nullptr;
    if (!model || itemId.isEmpty())
        return -1;
    for (int index = 0; index < model->rowCount(); ++index) {
        if (model->get(index).value(QStringLiteral("id")).toString() == itemId)
            return index;
    }
    return -1;
}

int SearchResultRowsModel::rowForId(
    const QString &mediaSection,
    const QString &itemId) const
{
    return rowFor(
        mediaSection,
        sourceIndexForId(mediaSection, itemId));
}

int SearchResultRowsModel::rowForKey(const QString &rowKey) const
{
    if (rowKey.isEmpty())
        return -1;
    for (int row = 0; row < m_rows.size(); ++row) {
        if (m_rows.at(row).rowKey == rowKey)
            return row;
    }
    return -1;
}

QString SearchResultRowsModel::sectionName(MediaSection section)
{
    return section == MediaSection::Titles ? titlesSection : episodesSection;
}

std::optional<SearchResultRowsModel::MediaSection>
SearchResultRowsModel::parseSection(const QString &section)
{
    if (section == titlesSection)
        return MediaSection::Titles;
    if (section == episodesSection)
        return MediaSection::Episodes;
    return std::nullopt;
}

MediaQueryModel *SearchResultRowsModel::sourceModel(MediaSection section) const
{
    return section == MediaSection::Titles
        ? m_titleResults.data() : m_episodeResults.data();
}

QVariantList SearchResultRowsModel::itemsForRow(const Row &row) const
{
    if (row.type != RowType::Cards)
        return {};
    MediaQueryModel *const model = sourceModel(row.section);
    if (!model)
        return {};
    const int end = std::min(
        model->rowCount(), row.startIndex + row.itemCount);
    QVariantList items;
    items.reserve(std::max(0, end - row.startIndex));
    for (int index = row.startIndex; index < end; ++index)
        items.push_back(model->get(index));
    return items;
}

QVector<SearchResultRowsModel::Row> SearchResultRowsModel::buildRows() const
{
    QVector<Row> rows;
    const auto appendSection = [this, &rows](
                                   MediaSection section,
                                   MediaQueryModel *model) {
        const int count = model ? model->rowCount() : 0;
        if (count <= 0)
            return;
        const QString name = sectionName(section);
        rows.push_back(Row {
            .type = RowType::Header,
            .section = section,
            .startIndex = 0,
            .itemCount = 0,
            .rowKey = name + QStringLiteral(":header"),
        });
        int visualRow = 0;
        for (int start = 0; start < count; start += m_columns) {
            rows.push_back(Row {
                .type = RowType::Cards,
                .section = section,
                .startIndex = start,
                .itemCount = std::min(m_columns, count - start),
                .rowKey = QStringLiteral("%1:cards:%2")
                              .arg(name)
                              .arg(visualRow++),
            });
        }
    };
    appendSection(MediaSection::Titles, m_titleResults);
    appendSection(MediaSection::Episodes, m_episodeResults);
    return rows;
}

void SearchResultRowsModel::sourceDataChanged(
    MediaSection section,
    int firstSourceRow,
    int lastSourceRow)
{
    if (firstSourceRow < 0 || lastSourceRow < firstSourceRow)
        return;
    const int firstVisualRow = rowFor(
        sectionName(section), firstSourceRow);
    const int lastVisualRow = rowFor(
        sectionName(section), lastSourceRow);
    if (firstVisualRow < 0 || lastVisualRow < firstVisualRow)
        return;
    emit dataChanged(
        index(firstVisualRow), index(lastVisualRow), {ItemsRole});
}

void SearchResultRowsModel::scheduleRebuild()
{
    m_rebuildTimer.start();
}

void SearchResultRowsModel::rebuild()
{
    emit rowsAboutToBeRebuilt();
    synchronizeRows(buildRows());
    if (!m_rows.isEmpty()) {
        emit dataChanged(
            index(0), index(m_rows.size() - 1),
            {RowTypeRole, MediaSectionRole, ItemsRole,
             StartIndexRole, ItemCountRole, RowKeyRole});
    }
    emit rowsRebuilt();
}

void SearchResultRowsModel::synchronizeRows(QVector<Row> rows)
{
    bool rowCountChanged = false;
    int target = 0;
    while (target < rows.size()) {
        const Row &wanted = rows.at(target);
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
            rowCountChanged = true;
        } else {
            if (found != target) {
                beginMoveRows({}, found, found, {}, target);
                m_rows.move(found, target);
                endMoveRows();
            }
            m_rows[target] = wanted;
        }
        ++target;
    }
    if (m_rows.size() > rows.size()) {
        beginRemoveRows({}, rows.size(), m_rows.size() - 1);
        m_rows.erase(m_rows.begin() + rows.size(), m_rows.end());
        endRemoveRows();
        rowCountChanged = true;
    }
    if (rowCountChanged)
        emit countChanged();
}
