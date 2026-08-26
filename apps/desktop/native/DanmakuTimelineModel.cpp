#include "DanmakuTimelineModel.hpp"

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace {

constexpr int MaximumLanes = 64;
constexpr qreal FixedCommentDuration = 4.5;

enum class DanmakuMode : quint8 {
    Scroll,
    Top,
    Bottom,
};

struct TimelineEntry
{
    QString commentId;
    qreal start = 0;
    DanmakuMode mode = DanmakuMode::Scroll;
    QString textColor;
    QString commentText;
    int lane = 0;
    int densityRank = 0;
    int sourceOrdinal = 0;
};

using Timeline = QVector<TimelineEntry>;

struct BuildRequest
{
    quint64 generation = 0;
    quint64 sessionGeneration = 0;
    QVariantList rawComments;
    std::shared_ptr<const Timeline> normalizedComments;
    qreal viewportWidth = 0;
    qreal fontSize = 42;
    qreal scrollDuration = 9;
};

struct BuildResult
{
    quint64 generation = 0;
    quint64 sessionGeneration = 0;
    std::shared_ptr<const Timeline> timeline;
};

DanmakuMode normalizedMode(const QVariant &value)
{
    const QString mode = value.toString().toLower();
    if (mode == QStringLiteral("top"))
        return DanmakuMode::Top;
    if (mode == QStringLiteral("bottom"))
        return DanmakuMode::Bottom;
    return DanmakuMode::Scroll;
}

QString modeName(DanmakuMode mode)
{
    switch (mode) {
    case DanmakuMode::Top:
        return QStringLiteral("top");
    case DanmakuMode::Bottom:
        return QStringLiteral("bottom");
    case DanmakuMode::Scroll:
        return QStringLiteral("scroll");
    }
    return QStringLiteral("scroll");
}

QString normalizedColor(const QVariant &value)
{
    // Preserve the QML expression Number(rgb || 0xffffff): numeric zero is
    // treated as the default white rather than black.
    bool ok = false;
    const double converted = value.toDouble(&ok);
    const double source = !ok || converted == 0.0
        ? static_cast<double>(0xffffff)
        : converted;
    const qint64 rgb = qRound64(std::clamp(source, 0.0, 16777215.0));
    return QStringLiteral("#%1").arg(rgb, 6, 16, QLatin1Char('0'));
}

qreal normalizedFinite(qreal value, qreal fallback)
{
    return std::isfinite(value) ? value : fallback;
}

qreal estimatedWidth(const QString &text, qreal fontSize)
{
    qreal units = 0;
    for (const QChar character : text)
        units += character.unicode() > 255 ? 1.0 : 0.56;
    return std::max(fontSize, units * fontSize);
}

int modeIndex(DanmakuMode mode)
{
    return static_cast<int>(mode);
}

Timeline parseComments(const QVariantList &comments)
{
    Timeline ordered;
    ordered.reserve(comments.size());
    for (qsizetype index = 0; index < comments.size(); ++index) {
        const QVariantMap comment = comments[index].toMap();
        const QString text = comment.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty())
            continue;

        bool timeOk = false;
        double start = comment.value(QStringLiteral("time")).toDouble(&timeOk);
        if (!timeOk || !std::isfinite(start))
            start = 0;

        TimelineEntry entry;
        entry.commentId = comment.contains(QStringLiteral("id"))
            ? comment.value(QStringLiteral("id")).toString()
            : QString::number(index);
        entry.start = std::max(0.0, start);
        entry.mode = normalizedMode(comment.value(QStringLiteral("mode")));
        entry.textColor = normalizedColor(comment.value(QStringLiteral("color")));
        entry.commentText = text;
        entry.sourceOrdinal = static_cast<int>(index);
        ordered.push_back(std::move(entry));
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const TimelineEntry &left, const TimelineEntry &right) {
            if (left.start != right.start)
                return left.start < right.start;
            return left.sourceOrdinal < right.sourceOrdinal;
        });
    return ordered;
}

void layoutTimeline(
    Timeline &timeline,
    qreal viewportWidth,
    qreal fontSize,
    qreal scrollDuration)
{
    viewportWidth = normalizedFinite(viewportWidth, 0);
    fontSize = normalizedFinite(fontSize, 42);
    scrollDuration = normalizedFinite(scrollDuration, 9);

    std::array<std::array<qreal, MaximumLanes>, 3> available = {};
    std::array<qint64, 3> densitySeconds = {-1, -1, -1};
    std::array<int, 3> densityRanks = {};

    for (TimelineEntry &entry : timeline) {
        const int mode = modeIndex(entry.mode);
        const qint64 second = static_cast<qint64>(std::floor(entry.start));
        if (second != densitySeconds[mode]) {
            densitySeconds[mode] = second;
            densityRanks[mode] = 0;
        }
        entry.densityRank = densityRanks[mode]++;

        std::array<qreal, MaximumLanes> &lanes = available[mode];
        int lane = -1;
        int earliestLane = 0;
        for (int candidate = 0; candidate < MaximumLanes; ++candidate) {
            if (lanes[candidate] < lanes[earliestLane])
                earliestLane = candidate;
            if (lane < 0 && lanes[candidate] <= entry.start)
                lane = candidate;
        }
        if (lane < 0)
            lane = earliestLane;
        entry.lane = lane;

        if (entry.mode == DanmakuMode::Scroll) {
            const qreal speed = std::max<qreal>(1, viewportWidth)
                / std::max<qreal>(3, scrollDuration);
            lanes[lane] = entry.start
                + estimatedWidth(entry.commentText, fontSize) / speed
                + 0.12;
        } else {
            lanes[lane] = entry.start + FixedCommentDuration;
        }
    }
}

BuildResult buildTimeline(BuildRequest request)
{
    Timeline timeline = request.normalizedComments
        ? *request.normalizedComments
        : parseComments(request.rawComments);
    layoutTimeline(
        timeline,
        request.viewportWidth,
        request.fontSize,
        request.scrollDuration);
    return {
        request.generation,
        request.sessionGeneration,
        std::make_shared<const Timeline>(std::move(timeline)),
    };
}

QString entryKey(
    quint64 sessionGeneration,
    quint64 sourceGeneration,
    int sourceOrdinal)
{
    return QStringLiteral("%1:%2:%3")
        .arg(sessionGeneration)
        .arg(sourceGeneration)
        .arg(sourceOrdinal);
}

QVariantMap entryMap(
    const TimelineEntry &entry,
    int timelineIndex,
    quint64 sessionGeneration,
    quint64 sourceGeneration)
{
    return {
        {QStringLiteral("commentId"), entry.commentId},
        {QStringLiteral("start"), entry.start},
        {QStringLiteral("mode"), modeName(entry.mode)},
        {QStringLiteral("textColor"), entry.textColor},
        {QStringLiteral("commentText"), entry.commentText},
        {QStringLiteral("lane"), entry.lane},
        {QStringLiteral("densityRank"), entry.densityRank},
        {QStringLiteral("sourceOrdinal"), entry.sourceOrdinal},
        {QStringLiteral("timelineIndex"), timelineIndex},
        {QStringLiteral("entryKey"),
         entryKey(sessionGeneration, sourceGeneration, entry.sourceOrdinal)},
    };
}

struct BuildCompletionGate
{
    QMutex mutex;
    DanmakuTimelineModel *receiver = nullptr;
};

} // namespace

class DanmakuTimelineModel::Private final
{
public:
    explicit Private(DanmakuTimelineModel *owner)
        : q(owner)
        , completionGate(std::make_shared<BuildCompletionGate>())
    {
        completionGate->receiver = owner;
    }

    void detach()
    {
        const QMutexLocker lock(&completionGate->mutex);
        completionGate->receiver = nullptr;
    }

    qreal durationFor(DanmakuMode mode) const
    {
        return mode == DanmakuMode::Scroll
            ? std::max<qreal>(3, normalizedFinite(scrollDuration, 9))
            : FixedCommentDuration;
    }

    bool modeEnabled(DanmakuMode mode) const
    {
        switch (mode) {
        case DanmakuMode::Top:
            return showTop;
        case DanmakuMode::Bottom:
            return showBottom;
        case DanmakuMode::Scroll:
            return showScroll;
        }
        return showScroll;
    }

    bool blocked(const QString &text) const
    {
        return std::any_of(
            blockedTermList.cbegin(),
            blockedTermList.cend(),
            [&text](const QString &term) { return text.contains(term); });
    }

    bool alive(const TimelineEntry &entry, qreal position) const
    {
        const qreal effectiveStart = entry.start + timeOffset;
        return effectiveStart <= position
            && effectiveStart + durationFor(entry.mode) >= position;
    }

    bool eligible(const TimelineEntry &entry, qreal position) const
    {
        return alive(entry, position)
            && modeEnabled(entry.mode)
            && entry.lane < laneCount
            && entry.densityRank < density
            && !blocked(entry.commentText);
    }

    void setPreparing(bool value)
    {
        if (preparing == value)
            return;
        preparing = value;
        emit q->preparingChanged();
    }

    void clearActive()
    {
        if (activeRows.isEmpty())
            return;
        q->beginResetModel();
        activeRows.clear();
        q->endResetModel();
        emit q->countChanged();
    }

    void invalidatePublishedTimeline(bool retainLayoutSource)
    {
        clearActive();
        const int previousCount = timeline ? timeline->size() : 0;
        timeline.reset();
        if (!retainLayoutSource)
            layoutSource.reset();
        nextTimelineIndex = 0;
        lastPosition = std::numeric_limits<qreal>::quiet_NaN();
        if (previousCount != 0)
            emit q->timelineCountChanged();
    }

    void replacePending(std::optional<BuildRequest> request)
    {
        const int previousCount = pendingRequest.has_value() ? 1 : 0;
        pendingRequest = std::move(request);
        const int currentCount = pendingRequest.has_value() ? 1 : 0;
        if (previousCount != currentCount)
            emit q->pendingCountChanged();
    }

    void enqueue(BuildRequest request)
    {
        setPreparing(true);
        if (buildRunning) {
            replacePending(std::move(request));
            return;
        }
        start(std::move(request));
    }

    void start(BuildRequest request)
    {
        buildRunning = true;
        const std::shared_ptr<BuildCompletionGate> gate = completionGate;
        QThreadPool::globalInstance()->start(
            [gate, request = std::move(request)]() mutable {
            BuildResult result = buildTimeline(std::move(request));
            const QMutexLocker lock(&gate->mutex);
            DanmakuTimelineModel *const receiver = gate->receiver;
            if (!receiver)
                return;
            QMetaObject::invokeMethod(
                receiver,
                [receiver, result = std::move(result)]() mutable {
                    receiver->d->finished(std::move(result));
                },
                Qt::QueuedConnection);
        });
    }

    void finished(BuildResult result)
    {
        buildRunning = false;
        if (result.generation == generation
                && result.sessionGeneration == sessionGeneration) {
            const int previousCount = timeline ? timeline->size() : 0;
            timeline = std::move(result.timeline);
            layoutSource = timeline;
            const int currentCount = timeline ? timeline->size() : 0;
            if (previousCount != currentCount)
                emit q->timelineCountChanged();
            setPreparing(false);
            if (std::isfinite(lastRequestedPosition))
                rebuildActive(lastRequestedPosition);
        }

        if (pendingRequest) {
            BuildRequest next = std::move(*pendingRequest);
            replacePending(std::nullopt);
            start(std::move(next));
        }
    }

    void rebuildActive(qreal position)
    {
        position = std::max<qreal>(0, normalizedFinite(position, 0));
        QVector<int> replacement;
        int next = 0;
        if (timeline && !timeline->isEmpty()) {
            const qreal lookBack = std::max<qreal>(20, durationFor(DanmakuMode::Scroll) + 1);
            const qreal threshold = position - timeOffset - lookBack;
            const auto first = std::lower_bound(
                timeline->cbegin(),
                timeline->cend(),
                threshold,
                [](const TimelineEntry &entry, qreal value) {
                    return entry.start < value;
                });
            next = static_cast<int>(std::distance(timeline->cbegin(), first));
            while (next < timeline->size()
                   && timeline->at(next).start + timeOffset <= position) {
                if (eligible(timeline->at(next), position))
                    replacement.push_back(next);
                ++next;
            }
        }

        const bool countChanged = activeRows.size() != replacement.size();
        q->beginResetModel();
        activeRows = std::move(replacement);
        q->endResetModel();
        if (countChanged)
            emit q->countChanged();
        nextTimelineIndex = next;
        lastPosition = position;
    }

    void removeIneligibleRows(qreal position)
    {
        int row = activeRows.size() - 1;
        while (row >= 0) {
            if (alive(timeline->at(activeRows[row]), position)) {
                --row;
                continue;
            }
            const int last = row;
            while (row >= 0
                   && !alive(timeline->at(activeRows[row]), position)) {
                --row;
            }
            const int first = row + 1;
            q->beginRemoveRows({}, first, last);
            activeRows.erase(
                activeRows.begin() + first,
                activeRows.begin() + last + 1);
            q->endRemoveRows();
        }
    }

    void appendDueRows(qreal position)
    {
        QVector<int> appended;
        while (nextTimelineIndex < timeline->size()
               && timeline->at(nextTimelineIndex).start + timeOffset <= position) {
            if (eligible(timeline->at(nextTimelineIndex), position))
                appended.push_back(nextTimelineIndex);
            ++nextTimelineIndex;
        }
        if (appended.isEmpty())
            return;
        const int first = activeRows.size();
        const int last = first + appended.size() - 1;
        q->beginInsertRows({}, first, last);
        activeRows += appended;
        q->endInsertRows();
    }

    void sync(qreal position, bool force)
    {
        position = std::max<qreal>(0, normalizedFinite(position, 0));
        lastRequestedPosition = position;
        if (!timeline) {
            clearActive();
            return;
        }
        if (force
                || !std::isfinite(lastPosition)
                || position < lastPosition
                || position > lastPosition + 1.2) {
            rebuildActive(position);
            return;
        }

        const int previousCount = activeRows.size();
        removeIneligibleRows(position);
        appendDueRows(position);
        if (previousCount != activeRows.size())
            emit q->countChanged();
        lastPosition = position;
    }

    void refreshActive()
    {
        if (timeline && std::isfinite(lastRequestedPosition))
            rebuildActive(lastRequestedPosition);
    }

    BuildRequest requestForCurrentSource() const
    {
        BuildRequest request;
        request.generation = generation;
        request.sessionGeneration = sessionGeneration;
        request.viewportWidth = viewportWidth;
        request.fontSize = fontSize;
        request.scrollDuration = scrollDuration;
        if (layoutSource)
            request.normalizedComments = layoutSource;
        else
            request.rawComments = comments;
        return request;
    }

    DanmakuTimelineModel *q = nullptr;
    std::shared_ptr<BuildCompletionGate> completionGate;
    QVariantList comments;
    std::shared_ptr<const Timeline> timeline;
    std::shared_ptr<const Timeline> layoutSource;
    QVector<int> activeRows;
    QString blockedTerms;
    QStringList blockedTermList;
    std::optional<BuildRequest> pendingRequest;
    qreal viewportWidth = 0;
    qreal fontSize = 42;
    qreal scrollDuration = 9;
    qreal timeOffset = 0;
    qreal lastPosition = std::numeric_limits<qreal>::quiet_NaN();
    qreal lastRequestedPosition = std::numeric_limits<qreal>::quiet_NaN();
    quint64 sessionGeneration = 0;
    quint64 sourceGeneration = 0;
    quint64 generation = 0;
    int nextTimelineIndex = 0;
    int density = 14;
    int laneCount = 1;
    bool showScroll = true;
    bool showTop = true;
    bool showBottom = true;
    bool preparing = false;
    bool buildRunning = false;
};

DanmakuTimelineModel::DanmakuTimelineModel(QObject *parent)
    : QAbstractListModel(parent)
    , d(std::make_unique<Private>(this))
{
}

DanmakuTimelineModel::~DanmakuTimelineModel()
{
    d->detach();
}

int DanmakuTimelineModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : d->activeRows.size();
}

QVariant DanmakuTimelineModel::data(const QModelIndex &index, int role) const
{
    if (!d->timeline
            || !index.isValid()
            || index.row() < 0
            || index.row() >= d->activeRows.size()) {
        return {};
    }
    const int timelineIndex = d->activeRows[index.row()];
    const TimelineEntry &entry = d->timeline->at(timelineIndex);
    switch (role) {
    case CommentIdRole:
        return entry.commentId;
    case StartRole:
        return entry.start;
    case ModeRole:
        return modeName(entry.mode);
    case TextColorRole:
        return entry.textColor;
    case CommentTextRole:
        return entry.commentText;
    case LaneRole:
        return entry.lane;
    case DensityRankRole:
        return entry.densityRank;
    case SourceOrdinalRole:
        return entry.sourceOrdinal;
    case TimelineIndexRole:
        return timelineIndex;
    case EntryKeyRole:
        return entryKey(
            d->sessionGeneration,
            d->sourceGeneration,
            entry.sourceOrdinal);
    default:
        return {};
    }
}

QHash<int, QByteArray> DanmakuTimelineModel::roleNames() const
{
    return {
        {CommentIdRole, "commentId"},
        {StartRole, "start"},
        {ModeRole, "mode"},
        {TextColorRole, "textColor"},
        {CommentTextRole, "commentText"},
        {LaneRole, "lane"},
        {DensityRankRole, "densityRank"},
        {SourceOrdinalRole, "sourceOrdinal"},
        {TimelineIndexRole, "timelineIndex"},
        {EntryKeyRole, "entryKey"},
    };
}

QVariantList DanmakuTimelineModel::comments() const
{
    return d->comments;
}

void DanmakuTimelineModel::setComments(const QVariantList &comments)
{
    replaceComments(comments, d->sessionGeneration);
}

quint64 DanmakuTimelineModel::sessionGeneration() const
{
    return d->sessionGeneration;
}

void DanmakuTimelineModel::setSessionGeneration(quint64 generation)
{
    if (d->sessionGeneration == generation)
        return;
    d->sessionGeneration = generation;
    ++d->sourceGeneration;
    ++d->generation;
    d->comments.clear();
    d->replacePending(std::nullopt);
    d->invalidatePublishedTimeline(false);
    d->setPreparing(false);
    emit sessionGenerationChanged();
    emit commentsChanged();
    emit generationChanged();
}

qreal DanmakuTimelineModel::viewportWidth() const
{
    return d->viewportWidth;
}

void DanmakuTimelineModel::setViewportWidth(qreal width)
{
    width = normalizedFinite(width, 0);
    if (qFuzzyCompare(d->viewportWidth, width))
        return;
    d->viewportWidth = width;
    emit viewportWidthChanged();
    if (width <= 0 || d->comments.isEmpty())
        return;

    ++d->generation;
    emit generationChanged();
    d->enqueue(d->requestForCurrentSource());
}

qreal DanmakuTimelineModel::fontSize() const
{
    return d->fontSize;
}

void DanmakuTimelineModel::setFontSize(qreal size)
{
    size = normalizedFinite(size, 42);
    if (qFuzzyCompare(d->fontSize, size))
        return;
    d->fontSize = size;
    emit fontSizeChanged();
    // Deliberately no relayout: this matches DanmakuOverlay.qml's existing
    // style-change semantics. A later width/comments rebuild uses the value.
}

qreal DanmakuTimelineModel::scrollDuration() const
{
    return d->scrollDuration;
}

void DanmakuTimelineModel::setScrollDuration(qreal seconds)
{
    seconds = normalizedFinite(seconds, 9);
    if (qFuzzyCompare(d->scrollDuration, seconds))
        return;
    d->scrollDuration = seconds;
    emit scrollDurationChanged();
    // Runtime lifetime changes immediately, but existing lane assignments do
    // not. Width/comments are the only layout rebuild triggers.
    d->refreshActive();
}

qreal DanmakuTimelineModel::timeOffset() const
{
    return d->timeOffset;
}

void DanmakuTimelineModel::setTimeOffset(qreal seconds)
{
    seconds = normalizedFinite(seconds, 0);
    if (qFuzzyCompare(d->timeOffset, seconds))
        return;
    d->timeOffset = seconds;
    emit timeOffsetChanged();
    d->refreshActive();
}

QString DanmakuTimelineModel::blockedTerms() const
{
    return d->blockedTerms;
}

void DanmakuTimelineModel::setBlockedTerms(const QString &terms)
{
    if (d->blockedTerms == terms)
        return;
    d->blockedTerms = terms;
    d->blockedTermList.clear();
    const QStringList candidates = terms.split(
        QRegularExpression(QStringLiteral("[,\\n]")),
        Qt::KeepEmptyParts);
    for (const QString &candidate : candidates) {
        const QString term = candidate.trimmed();
        if (!term.isEmpty())
            d->blockedTermList.push_back(term);
    }
    emit blockedTermsChanged();
    d->refreshActive();
}

bool DanmakuTimelineModel::showScroll() const
{
    return d->showScroll;
}

void DanmakuTimelineModel::setShowScroll(bool show)
{
    if (d->showScroll == show)
        return;
    d->showScroll = show;
    emit showScrollChanged();
    d->refreshActive();
}

bool DanmakuTimelineModel::showTop() const
{
    return d->showTop;
}

void DanmakuTimelineModel::setShowTop(bool show)
{
    if (d->showTop == show)
        return;
    d->showTop = show;
    emit showTopChanged();
    d->refreshActive();
}

bool DanmakuTimelineModel::showBottom() const
{
    return d->showBottom;
}

void DanmakuTimelineModel::setShowBottom(bool show)
{
    if (d->showBottom == show)
        return;
    d->showBottom = show;
    emit showBottomChanged();
    d->refreshActive();
}

int DanmakuTimelineModel::density() const
{
    return d->density;
}

void DanmakuTimelineModel::setDensity(int density)
{
    if (d->density == density)
        return;
    d->density = density;
    emit densityChanged();
    d->refreshActive();
}

int DanmakuTimelineModel::laneCount() const
{
    return d->laneCount;
}

void DanmakuTimelineModel::setLaneCount(int count)
{
    count = std::max(1, count);
    if (d->laneCount == count)
        return;
    d->laneCount = count;
    emit laneCountChanged();
    d->refreshActive();
}

bool DanmakuTimelineModel::preparing() const
{
    return d->preparing;
}

int DanmakuTimelineModel::timelineCount() const
{
    return d->timeline ? d->timeline->size() : 0;
}

int DanmakuTimelineModel::pendingCount() const
{
    return d->pendingRequest ? 1 : 0;
}

quint64 DanmakuTimelineModel::generation() const
{
    return d->generation;
}

void DanmakuTimelineModel::replaceComments(
    const QVariantList &comments,
    quint64 sessionGeneration)
{
    const bool sessionChanged = d->sessionGeneration != sessionGeneration;
    d->sessionGeneration = sessionGeneration;
    d->comments = comments;
    ++d->sourceGeneration;
    ++d->generation;
    d->replacePending(std::nullopt);
    d->invalidatePublishedTimeline(false);
    emit commentsChanged();
    if (sessionChanged)
        emit sessionGenerationChanged();
    emit generationChanged();

    if (comments.isEmpty()) {
        d->setPreparing(false);
        return;
    }
    d->enqueue(d->requestForCurrentSource());
}

void DanmakuTimelineModel::syncActive(qreal position, bool force)
{
    d->sync(position, force);
}

QVariantMap DanmakuTimelineModel::entryAt(int timelineIndex) const
{
    if (!d->timeline
            || timelineIndex < 0
            || timelineIndex >= d->timeline->size()) {
        return {};
    }
    return entryMap(
        d->timeline->at(timelineIndex),
        timelineIndex,
        d->sessionGeneration,
        d->sourceGeneration);
}

QVariantMap DanmakuTimelineModel::get(int activeRow) const
{
    if (!d->timeline || activeRow < 0 || activeRow >= d->activeRows.size())
        return {};
    const int timelineIndex = d->activeRows[activeRow];
    return entryMap(
        d->timeline->at(timelineIndex),
        timelineIndex,
        d->sessionGeneration,
        d->sourceGeneration);
}

int DanmakuTimelineModel::activeTimelineIndexAt(int activeRow) const
{
    if (activeRow < 0 || activeRow >= d->activeRows.size())
        return -1;
    return d->activeRows[activeRow];
}
