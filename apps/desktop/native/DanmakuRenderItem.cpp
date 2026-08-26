#include "DanmakuRenderItem.hpp"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QMatrix4x4>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>
#include <QSet>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextOption>
#include <QtMath>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace {
constexpr double fixedCommentDuration = 4.5;
constexpr double textOutlinePadding = 2.0;
constexpr int maximumRasterDimension = 16'384;
constexpr qint64 maximumTextureRasterPixels = 4'194'304;
constexpr qint64 maximumTotalRasterPixels = 8'388'608;
constexpr double minimumRasterDevicePixelRatio = 0.025;

struct RenderEntry
{
    QString key;
    QString commentId;
    QString mode;
    QString text;
    QColor color;
    double start = 0.0;
    int lane = 0;
    bool containsCjk = false;
};

struct RenderSnapshot
{
    quint64 revision = 0;
    quint64 generation = 0;
    QVector<RenderEntry> entries;
};

using SharedSnapshot = std::shared_ptr<const RenderSnapshot>;

int roleForName(const QHash<int, QByteArray> &roles, QByteArrayView name)
{
    for (auto it = roles.cbegin(); it != roles.cend(); ++it) {
        if (it.value() == name)
            return it.key();
    }
    return -1;
}

QColor colorFromModelValue(const QVariant &value)
{
    if (value.canConvert<QColor>()) {
        const QColor converted = value.value<QColor>();
        if (converted.isValid())
            return converted;
    }
    const QColor parsed(value.toString());
    return parsed.isValid() ? parsed : QColor(Qt::white);
}

bool containsCjk(const QString &text)
{
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        const ushort value = character.unicode();
        if ((value >= 0x2e80 && value <= 0x9fff)
            || (value >= 0xf900 && value <= 0xfaff)) {
            return true;
        }
    }
    return false;
}

double durationForMode(const QString &mode, double scrollDuration)
{
    return mode == QStringLiteral("scroll")
        ? std::max(3.0, scrollDuration)
        : fixedCommentDuration;
}

enum class RasterStatus {
    Empty,
    Committed,
    BudgetDeferred,
    PermanentRejected,
    TransientFailure,
};

struct RenderStats
{
    std::atomic<qint64> estimatedRasterBytes = 0;
    std::atomic<int> committedTextures = 0;
    std::atomic<int> budgetScaledTextures = 0;
    std::atomic<int> uncommittedTextures = 0;
    std::atomic<int> emptyTextures = 0;
    std::atomic<int> budgetDeferredTextures = 0;
    std::atomic<int> transientFailureTextures = 0;
    std::atomic<int> permanentRejectedTextures = 0;
    QMutex committedVisibleMutex;
    QStringList committedVisibleIds;
};

using SharedRenderStats = std::shared_ptr<RenderStats>;

void publishCommittedVisibleIds(
    const SharedRenderStats &stats,
    QStringList ids = {})
{
    ids.sort();
    const QMutexLocker lock(&stats->committedVisibleMutex);
    stats->committedVisibleIds = std::move(ids);
}

struct TextNodeState
{
    QSGOpacityNode *opacity = nullptr;
    QSGTransformNode *transform = nullptr;
    QSGSimpleTextureNode *textureNode = nullptr;
    QSGTexture *texture = nullptr;
    bool attached = false;
    QString text;
    QString family;
    QColor color;
    int pixelSize = 0;
    double devicePixelRatio = 0.0;
    double rasterDevicePixelRatio = 0.0;
    double logicalWidth = 0.0;
    double logicalHeight = 0.0;
    double textWidth = 0.0;
    qint64 rasterPixels = 0;
    bool budgetScaled = false;
    RasterStatus rasterStatus = RasterStatus::Empty;
    int transientFailures = 0;
};

class RenderRootNode final : public QSGClipNode
{
public:
    explicit RenderRootNode(SharedRenderStats renderStats)
        : stats(std::move(renderStats))
    {
    }

    ~RenderRootNode() override
    {
        for (TextNodeState &state : nodes) {
            if (state.opacity) {
                if (state.attached)
                    removeChildNode(state.opacity);
                delete state.opacity;
                state.opacity = nullptr;
                state.transform = nullptr;
                state.textureNode = nullptr;
                state.attached = false;
            }
            delete state.texture;
            state.texture = nullptr;
        }
        stats->estimatedRasterBytes.store(0, std::memory_order_relaxed);
        stats->committedTextures.store(0, std::memory_order_relaxed);
        stats->budgetScaledTextures.store(0, std::memory_order_relaxed);
        stats->uncommittedTextures.store(0, std::memory_order_relaxed);
        stats->emptyTextures.store(0, std::memory_order_relaxed);
        stats->budgetDeferredTextures.store(0, std::memory_order_relaxed);
        stats->transientFailureTextures.store(0, std::memory_order_relaxed);
        stats->permanentRejectedTextures.store(0, std::memory_order_relaxed);
        // The scene-graph root owns the only authoritative committed set.
        // Never leave the GUI-side diagnostic holding IDs after its teardown.
        publishCommittedVisibleIds(stats);
    }

    quint64 snapshotRevision = std::numeric_limits<quint64>::max();
    qint64 rasterPixels = 0;
    QHash<QString, TextNodeState> nodes;
    SharedRenderStats stats;
};

void deleteTextState(RenderRootNode *root, TextNodeState &state)
{
    if (!state.opacity)
        return;
    if (state.attached)
        root->removeChildNode(state.opacity);
    delete state.opacity;
    delete state.texture;
    root->rasterPixels = std::max<qint64>(
        0, root->rasterPixels - state.rasterPixels);
    state.rasterPixels = 0;
    state.texture = nullptr;
    state.opacity = nullptr;
    state.transform = nullptr;
    state.textureNode = nullptr;
    state.attached = false;
}

void releaseTextTexture(RenderRootNode *root, TextNodeState &state)
{
    if (state.attached) {
        root->removeChildNode(state.opacity);
        state.attached = false;
    }
    if (state.textureNode && state.transform) {
        state.transform->removeChildNode(state.textureNode);
        delete state.textureNode;
        state.textureNode = new QSGSimpleTextureNode;
        state.textureNode->setOwnsTexture(false);
        state.transform->appendChildNode(state.textureNode);
    }
    delete state.texture;
    state.texture = nullptr;
    root->rasterPixels = std::max<qint64>(
        0, root->rasterPixels - state.rasterPixels);
    state.rasterPixels = 0;
    state.rasterDevicePixelRatio = 0.0;
    state.budgetScaled = false;
}

RasterStatus rebuildTextTexture(
    TextNodeState &state,
    const RenderEntry &entry,
    const QString &family,
    int pixelSize,
    double devicePixelRatio,
    qint64 rasterPixelBudget,
    QQuickWindow *quickWindow)
{
    const QString &rasterText = entry.text;

    QFont font(family);
    font.setPixelSize(pixelSize);
    font.setWeight(QFont::DemiBold);

    QTextLayout layout(rasterText, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    double naturalWidth = 0.0;
    double laidOutHeight = 0.0;
    int lineCount = 0;
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(std::numeric_limits<qreal>::max() / 4.0);
        line.setPosition(QPointF(0.0, laidOutHeight));
        naturalWidth = std::max(
            naturalWidth,
            static_cast<double>(line.naturalTextWidth()));
        laidOutHeight += std::max(1.0, static_cast<double>(line.height()));
        ++lineCount;
    }
    layout.endLayout();

    naturalWidth = lineCount > 0
        ? std::max(1.0, naturalWidth)
        : static_cast<double>(pixelSize);
    const double lineHeight = lineCount > 0
        ? std::max(1.0, laidOutHeight)
        : static_cast<double>(pixelSize);
    const QRectF layoutBounds = layout.boundingRect();
    const double contentLeft = std::min(0.0, static_cast<double>(layoutBounds.left()));
    const double contentTop = std::min(0.0, static_cast<double>(layoutBounds.top()));
    const double contentRight = std::max(
        naturalWidth,
        static_cast<double>(layoutBounds.right()));
    const double contentBottom = std::max(
        lineHeight,
        static_cast<double>(layoutBounds.bottom()));
    const QSizeF logicalSize(
        std::ceil(contentRight - contentLeft + textOutlinePadding * 2.0),
        std::ceil(contentBottom - contentTop + textOutlinePadding * 2.0));
    devicePixelRatio = std::max(1.0, devicePixelRatio);
    const auto rejectRaster = [&](RasterStatus status) {
        // Remember the attempted signature. Budget-deferred entries may be
        // retried in a bounded second pass or after a model revision;
        // transient failures use a bounded retry count; permanently
        // unrepresentable entries do not repeat layout work on every frame.
        state.text = entry.text;
        state.family = family;
        state.color = entry.color;
        state.pixelSize = pixelSize;
        state.devicePixelRatio = devicePixelRatio;
        state.textWidth = naturalWidth;
        state.logicalWidth = logicalSize.width();
        state.logicalHeight = logicalSize.height();
        return status;
    };
    const double logicalWidth = logicalSize.width();
    const double logicalHeight = logicalSize.height();
    const double logicalPixels = logicalWidth * logicalHeight;
    if (!std::isfinite(logicalPixels) || logicalWidth <= 0.0
        || logicalHeight <= 0.0) {
        return rejectRaster(RasterStatus::PermanentRejected);
    }

    const double hardRasterDevicePixelRatio = std::min({
        devicePixelRatio,
        static_cast<double>(maximumRasterDimension) / logicalWidth,
        static_cast<double>(maximumRasterDimension) / logicalHeight,
        std::sqrt(static_cast<double>(maximumTextureRasterPixels)
            / logicalPixels),
    });
    if (!std::isfinite(hardRasterDevicePixelRatio)
        || hardRasterDevicePixelRatio < minimumRasterDevicePixelRatio) {
        return rejectRaster(RasterStatus::PermanentRejected);
    }
    const qint64 allowedPixels = std::min(
        maximumTextureRasterPixels,
        std::max<qint64>(0, rasterPixelBudget));
    if (allowedPixels <= 0)
        return rejectRaster(RasterStatus::BudgetDeferred);

    // Preserve active entries that remain representable at the safety scale
    // under adversarial long-text density by reducing only their cached raster
    // resolution. Geometry, timing, font, color, and outline stay unchanged.
    // Ordinary comments remain at the window DPR; only entries that would
    // breach the per-text or aggregate 32 MiB RGBA payload budget use this
    // fallback, leaving headroom for replacement and scene-graph overhead.
    double rasterDevicePixelRatio = std::min({
        hardRasterDevicePixelRatio,
        std::sqrt(static_cast<double>(allowedPixels) / logicalPixels),
    });
    if (!std::isfinite(rasterDevicePixelRatio)
        || rasterDevicePixelRatio < minimumRasterDevicePixelRatio) {
        return rejectRaster(RasterStatus::BudgetDeferred);
    }
    const auto physicalSizeAt = [logicalWidth, logicalHeight](double ratio) {
        return std::pair {
            static_cast<qint64>(std::ceil(logicalWidth * ratio)),
            static_cast<qint64>(std::ceil(logicalHeight * ratio)),
        };
    };
    const auto fitsPixelBudget = [allowedPixels](
                                     const std::pair<qint64, qint64> &size) {
        return size.first > 0 && size.second > 0
            && size.first <= maximumRasterDimension
            && size.second <= maximumRasterDimension
            && size.first <= allowedPixels / size.second;
    };
    auto physicalSize = physicalSizeAt(rasterDevicePixelRatio);
    if (!fitsPixelBudget(physicalSize)) {
        const auto minimumPhysicalSize = physicalSizeAt(
            minimumRasterDevicePixelRatio);
        if (!fitsPixelBudget(minimumPhysicalSize))
            return rejectRaster(RasterStatus::BudgetDeferred);

        // ceil(width * ratio) * ceil(height * ratio) is monotonic but its
        // integer steps can remain above the budget after a fixed number of
        // proportional corrections. Search the bounded interval instead so
        // every representable minimum-scale texture obtains the largest
        // feasible raster ratio independent of platform font metrics.
        double feasibleRatio = minimumRasterDevicePixelRatio;
        double infeasibleRatio = rasterDevicePixelRatio;
        for (int iteration = 0; iteration < 48; ++iteration) {
            const double candidateRatio = feasibleRatio
                + (infeasibleRatio - feasibleRatio) / 2.0;
            const auto candidateSize = physicalSizeAt(candidateRatio);
            if (fitsPixelBudget(candidateSize))
                feasibleRatio = candidateRatio;
            else
                infeasibleRatio = candidateRatio;
        }
        rasterDevicePixelRatio = feasibleRatio;
        physicalSize = physicalSizeAt(rasterDevicePixelRatio);
    }
    const qint64 physicalWidth = physicalSize.first;
    const qint64 physicalHeight = physicalSize.second;
    if (physicalWidth <= 0 || physicalHeight <= 0
        || physicalWidth > maximumRasterDimension
        || physicalHeight > maximumRasterDimension
        || rasterDevicePixelRatio < minimumRasterDevicePixelRatio) {
        return rejectRaster(RasterStatus::BudgetDeferred);
    }
    const qint64 physicalPixels = static_cast<qint64>(physicalWidth)
        * static_cast<qint64>(physicalHeight);
    if (physicalPixels <= 0 || physicalPixels > allowedPixels)
        return rejectRaster(RasterStatus::BudgetDeferred);
    QImage image(
        QSize(
            static_cast<int>(physicalWidth),
            static_cast<int>(physicalHeight)),
        QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return rejectRaster(RasterStatus::TransientFailure);
    image.setDevicePixelRatio(rasterDevicePixelRatio);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QPointF textOrigin(
        textOutlinePadding - contentLeft,
        textOutlinePadding - contentTop);

    // QTextCharFormat::setTextOutline() asks the raster engine to stroke every
    // glyph path and is prohibitively expensive on a cold dense seek. Build a
    // one-pixel union mask from the already-shaped QTextLayout instead, tint it
    // once with the same #B0000000 style color, then paint the foreground. The
    // result is cached in the scene-graph texture; steady frames only update
    // opacity and geometry.
    QTextLayout::FormatRange outlineRange;
    outlineRange.start = 0;
    outlineRange.length = rasterText.size();
    outlineRange.format.setForeground(Qt::white);
    const QList<QTextLayout::FormatRange> outlineFormats {outlineRange};
    static const QPointF outlineOffsets[] = {
        {-1.0, -1.0}, {0.0, -1.0}, {1.0, -1.0},
        {-1.0, 0.0},                {1.0, 0.0},
        {-1.0, 1.0},  {0.0, 1.0},  {1.0, 1.0},
    };
    for (const QPointF &offset : outlineOffsets)
        layout.draw(&painter, textOrigin + offset, outlineFormats);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(
        QRectF(QPointF(0.0, 0.0), logicalSize),
        QColor(0, 0, 0, 176));

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    QTextLayout::FormatRange foregroundRange;
    foregroundRange.start = 0;
    foregroundRange.length = rasterText.size();
    foregroundRange.format.setForeground(entry.color);
    layout.draw(&painter, textOrigin, {foregroundRange});
    painter.end();

    QSGTexture *const previousTexture = state.texture;
    QSGTexture *const newTexture = quickWindow->createTextureFromImage(
        image,
        QQuickWindow::TextureHasAlphaChannel);
    if (!newTexture)
        return rejectRaster(RasterStatus::TransientFailure);
    state.textureNode->setTexture(newTexture);
    delete previousTexture;
    state.texture = newTexture;
    state.textureNode->setFiltering(QSGTexture::Linear);
    state.textureNode->setRect(QRectF(
        contentLeft - textOutlinePadding,
        contentTop - textOutlinePadding,
        logicalSize.width(),
        logicalSize.height()));

    state.text = entry.text;
    state.family = family;
    state.color = entry.color;
    state.pixelSize = pixelSize;
    state.devicePixelRatio = devicePixelRatio;
    state.rasterDevicePixelRatio = rasterDevicePixelRatio;
    state.logicalWidth = logicalWidth;
    state.logicalHeight = logicalHeight;
    state.textWidth = naturalWidth;
    state.rasterPixels = physicalPixels;
    state.budgetScaled = rasterDevicePixelRatio + 0.001 < devicePixelRatio;
    return RasterStatus::Committed;
}
} // namespace

class DanmakuRenderItem::Private
{
public:
    struct FontClassCacheEntry
    {
        QString text;
        bool containsCjk = false;
    };

    mutable QMutex snapshotMutex;
    SharedSnapshot snapshot = std::make_shared<RenderSnapshot>();
    quint64 nextRevision = 1;
    SharedRenderStats renderStats = std::make_shared<RenderStats>();
    QHash<QString, FontClassCacheEntry> fontClassCache;
};

DanmakuRenderItem::DanmakuRenderItem(QQuickItem *parent)
    : QQuickItem(parent)
    , d(std::make_unique<Private>())
{
    setFlag(ItemHasContents, true);
}

DanmakuRenderItem::~DanmakuRenderItem()
{
    disconnectModel();
    publishCommittedVisibleIds(d->renderStats);
}

void DanmakuRenderItem::setModel(QAbstractItemModel *model)
{
    if (m_model == model)
        return;
    disconnectModel();
    m_model = model;
    if (m_model) {
        const auto rebuild = [this] { rebuildSnapshot(); };
        const auto scheduleRebuild = [this] { scheduleSnapshotRebuild(); };
        m_modelConnections.push_back(connect(
            m_model, &QAbstractItemModel::modelReset, this, rebuild));
        m_modelConnections.push_back(connect(
            m_model, &QAbstractItemModel::layoutChanged, this, scheduleRebuild));
        m_modelConnections.push_back(connect(
            m_model, &QAbstractItemModel::dataChanged, this, scheduleRebuild));
        m_modelConnections.push_back(connect(
            m_model, &QAbstractItemModel::rowsInserted, this, scheduleRebuild));
        m_modelConnections.push_back(connect(
            m_model, &QAbstractItemModel::rowsRemoved, this, scheduleRebuild));
        m_modelConnections.push_back(connect(
            m_model, &QAbstractItemModel::rowsMoved, this, scheduleRebuild));
        m_modelConnections.push_back(connect(
            m_model, &QObject::destroyed, this, [this] {
                m_model = nullptr;
                m_modelConnections.clear();
                clearSnapshot();
                emit modelChanged();
            }));
        rebuildSnapshot();
    } else {
        clearSnapshot();
    }
    emit modelChanged();
}

void DanmakuRenderItem::setRenderTime(double value)
{
    if (!std::isfinite(value))
        value = 0.0;
    value = std::max(0.0, value);
    if (qFuzzyCompare(m_renderTime, value))
        return;
    m_renderTime = value;
    scheduleVisualUpdate();
    emit renderTimeChanged();
}

void DanmakuRenderItem::setTimeOffset(double value)
{
    if (!std::isfinite(value))
        value = 0.0;
    if (qFuzzyCompare(m_timeOffset, value))
        return;
    m_timeOffset = value;
    scheduleVisualUpdate();
    emit timeOffsetChanged();
}

void DanmakuRenderItem::setFontSize(double value)
{
    value = std::clamp(std::isfinite(value) ? value : 42.0, 1.0, 512.0);
    if (qFuzzyCompare(m_fontSize, value))
        return;
    m_fontSize = value;
    scheduleVisualUpdate();
    emit fontSizeChanged();
}

void DanmakuRenderItem::setScrollDuration(double value)
{
    value = std::max(3.0, std::isfinite(value) ? value : 9.0);
    if (qFuzzyCompare(m_scrollDuration, value))
        return;
    m_scrollDuration = value;
    scheduleVisualUpdate();
    emit scrollDurationChanged();
}

void DanmakuRenderItem::setDisplayArea(double value)
{
    value = std::clamp(std::isfinite(value) ? value : 0.70, 0.0, 1.0);
    if (qFuzzyCompare(m_displayArea, value))
        return;
    m_displayArea = value;
    scheduleVisualUpdate();
    emit displayAreaChanged();
}

void DanmakuRenderItem::setTopMargin(double value)
{
    value = std::max(0.0, std::isfinite(value) ? value : 0.0);
    if (qFuzzyCompare(m_topMargin, value))
        return;
    m_topMargin = value;
    scheduleVisualUpdate();
    emit topMarginChanged();
}

void DanmakuRenderItem::setFontFamily(const QString &value)
{
    if (m_fontFamily == value)
        return;
    m_fontFamily = value;
    scheduleVisualUpdate();
    emit fontFamilyChanged();
}

void DanmakuRenderItem::setCjkFontFamily(const QString &value)
{
    if (m_cjkFontFamily == value)
        return;
    m_cjkFontFamily = value;
    scheduleVisualUpdate();
    emit cjkFontFamilyChanged();
}

int DanmakuRenderItem::snapshotCount() const
{
    const QMutexLocker lock(&d->snapshotMutex);
    return d->snapshot ? d->snapshot->entries.size() : 0;
}

qint64 DanmakuRenderItem::estimatedCachedRasterBytes() const
{
    return d->renderStats->estimatedRasterBytes.load(
        std::memory_order_relaxed);
}

qint64 DanmakuRenderItem::texturePayloadBudgetBytes() const
{
    return maximumTotalRasterPixels * 4;
}

int DanmakuRenderItem::committedTextureCount() const
{
    return d->renderStats->committedTextures.load(
        std::memory_order_relaxed);
}

int DanmakuRenderItem::budgetScaledTextureCount() const
{
    return d->renderStats->budgetScaledTextures.load(
        std::memory_order_relaxed);
}

int DanmakuRenderItem::uncommittedTextureCount() const
{
    return d->renderStats->uncommittedTextures.load(
        std::memory_order_relaxed);
}

int DanmakuRenderItem::emptyTextureCount() const
{
    return d->renderStats->emptyTextures.load(std::memory_order_relaxed);
}

int DanmakuRenderItem::budgetDeferredTextureCount() const
{
    return d->renderStats->budgetDeferredTextures.load(
        std::memory_order_relaxed);
}

int DanmakuRenderItem::transientFailureTextureCount() const
{
    return d->renderStats->transientFailureTextures.load(
        std::memory_order_relaxed);
}

int DanmakuRenderItem::permanentRejectedTextureCount() const
{
    return d->renderStats->permanentRejectedTextures.load(
        std::memory_order_relaxed);
}

QStringList DanmakuRenderItem::eligibleCandidateIds() const
{
    SharedSnapshot snapshot;
    {
        const QMutexLocker lock(&d->snapshotMutex);
        snapshot = d->snapshot;
    }
    QStringList ids;
    if (!snapshot)
        return ids;
    ids.reserve(snapshot->entries.size());
    for (const RenderEntry &entry : snapshot->entries) {
        const double life = m_renderTime - (entry.start + m_timeOffset);
        const double duration = durationForMode(entry.mode, m_scrollDuration);
        if (life >= 0.0 && life <= duration)
            ids.push_back(entry.commentId);
    }
    ids.sort();
    return ids;
}

QStringList DanmakuRenderItem::committedVisibleCandidateIds() const
{
    const QMutexLocker lock(&d->renderStats->committedVisibleMutex);
    return d->renderStats->committedVisibleIds;
}

QSGNode *DanmakuRenderItem::updatePaintNode(
    QSGNode *oldNode,
    UpdatePaintNodeData *)
{
    auto *root = static_cast<RenderRootNode *>(oldNode);
    if (!root)
        root = new RenderRootNode(d->renderStats);
    root->setIsRectangular(true);
    root->setClipRect(boundingRect());

    SharedSnapshot snapshot;
    {
        const QMutexLocker lock(&d->snapshotMutex);
        snapshot = d->snapshot;
    }
    if (!snapshot) {
        root->stats->estimatedRasterBytes.store(
            root->rasterPixels * 4,
            std::memory_order_relaxed);
        root->stats->committedTextures.store(0, std::memory_order_relaxed);
        root->stats->budgetScaledTextures.store(0, std::memory_order_relaxed);
        root->stats->uncommittedTextures.store(0, std::memory_order_relaxed);
        root->stats->emptyTextures.store(0, std::memory_order_relaxed);
        root->stats->budgetDeferredTextures.store(0, std::memory_order_relaxed);
        root->stats->transientFailureTextures.store(0, std::memory_order_relaxed);
        root->stats->permanentRejectedTextures.store(0, std::memory_order_relaxed);
        publishCommittedVisibleIds(root->stats);
        return root;
    }

    const int pixelSize = std::max(1, qRound(m_fontSize));
    QQuickWindow *const quickWindow = window();
    const double devicePixelRatio = quickWindow
        ? quickWindow->effectiveDevicePixelRatio() : 1.0;
    const bool snapshotChanged = root->snapshotRevision != snapshot->revision;
    if (snapshotChanged) {
        QSet<QString> desiredKeys;
        desiredKeys.reserve(snapshot->entries.size());
        for (const RenderEntry &entry : snapshot->entries)
            desiredKeys.insert(entry.key);

        for (auto it = root->nodes.begin(); it != root->nodes.end();) {
            if (!desiredKeys.contains(it.key())) {
                deleteTextState(root, it.value());
                it = root->nodes.erase(it);
            } else {
                ++it;
            }
        }
        if (QQuickWindow *quickWindow = window()) {
            for (const RenderEntry &entry : snapshot->entries) {
                auto it = root->nodes.find(entry.key);
                if (it == root->nodes.end()) {
                    TextNodeState state;
                    state.opacity = new QSGOpacityNode;
                    state.transform = new QSGTransformNode;
                    state.textureNode = new QSGSimpleTextureNode;
                    state.textureNode->setOwnsTexture(false);
                    state.transform->appendChildNode(state.textureNode);
                    state.opacity->appendChildNode(state.transform);
                    it = root->nodes.insert(entry.key, state);
                }
            }
        }
        root->snapshotRevision = snapshot->revision;
    }

    const double areaBottom = height() * m_displayArea;
    const double effectiveTopMargin = std::max(
        0.0,
        std::min(m_topMargin, areaBottom - m_fontSize - 8.0));

    const auto familyFor = [this](const RenderEntry &entry) {
        return entry.containsCjk ? m_cjkFontFamily : m_fontFamily;
    };
    const auto signatureChanged = [pixelSize, devicePixelRatio](
                                      const TextNodeState &state,
                                      const RenderEntry &entry,
                                      const QString &family) {
        return state.text != entry.text
            || state.family != family
            || state.pixelSize != pixelSize
            || state.color != entry.color
            || !qFuzzyCompare(state.devicePixelRatio, devicePixelRatio);
    };
    const auto canFullyUpgrade = [root](const TextNodeState &state) {
        if (!state.budgetScaled || state.rasterDevicePixelRatio <= 0.0
            || state.logicalWidth <= 0.0 || state.logicalHeight <= 0.0) {
            return false;
        }
        const double physicalWidth = std::ceil(
            state.logicalWidth * state.devicePixelRatio);
        const double physicalHeight = std::ceil(
            state.logicalHeight * state.devicePixelRatio);
        if (!std::isfinite(physicalWidth) || !std::isfinite(physicalHeight)
            || physicalWidth <= 0 || physicalHeight <= 0
            || physicalWidth > maximumRasterDimension
            || physicalHeight > maximumRasterDimension) {
            return false;
        }
        const qint64 desiredPixels = static_cast<qint64>(physicalWidth)
            * static_cast<qint64>(physicalHeight);
        const qint64 pixelsOutsideState = std::max<qint64>(
            0, root->rasterPixels - state.rasterPixels);
        const qint64 availablePixels = std::max<qint64>(
            0, maximumTotalRasterPixels - pixelsOutsideState);
        return desiredPixels <= maximumTextureRasterPixels
            && desiredPixels <= availablePixels;
    };
    const auto shouldBuild = [&](const TextNodeState &state,
                                 const RenderEntry &entry,
                                 const QString &family) {
        if (signatureChanged(state, entry, family))
            return true;
        if (state.rasterStatus == RasterStatus::TransientFailure)
            // Snapshot churn does not make the same failed raster signature a
            // new request. A changed signature is handled by the first branch
            // and resets the counter immediately before its build attempt.
            return state.transientFailures < 3;
        if (state.rasterStatus == RasterStatus::BudgetDeferred)
            return snapshotChanged;
        return state.rasterStatus == RasterStatus::Committed
            && snapshotChanged && canFullyUpgrade(state);
    };

    int untexturedRebuilds = 0;
    for (const RenderEntry &entry : snapshot->entries) {
        const auto it = root->nodes.constFind(entry.key);
        if (it == root->nodes.cend())
            continue;
        const TextNodeState &state = it.value();
        const QString family = familyFor(entry);
        if (shouldBuild(state, entry, family) && state.rasterPixels == 0)
            ++untexturedRebuilds;
    }
    int pendingUntexturedBuilds = untexturedRebuilds;

    const auto isPureQualityUpgrade = [&](const TextNodeState &state,
                                          const RenderEntry &entry,
                                          const QString &family) {
        return !signatureChanged(state, entry, family)
            && state.rasterStatus == RasterStatus::Committed
            && state.budgetScaled
            && snapshotChanged
            && canFullyUpgrade(state);
    };

    bool scheduleTransientRetry = false;
    const auto performBuild = [&](TextNodeState &state,
                                  const RenderEntry &entry,
                                  const QString &family,
                                  qint64 rasterPixelBudget) {
        const qint64 previousPixels = state.rasterPixels;
        const qint64 pixelsOutsideState = std::max<qint64>(
            0, root->rasterPixels - previousPixels);
        const RasterStatus result = rebuildTextTexture(
            state,
            entry,
            family,
            pixelSize,
            devicePixelRatio,
            rasterPixelBudget,
            quickWindow);
        state.rasterStatus = result;
        if (result == RasterStatus::Committed) {
            state.transientFailures = 0;
            root->rasterPixels = pixelsOutsideState + state.rasterPixels;
            Q_ASSERT(root->rasterPixels <= maximumTotalRasterPixels);
            return true;
        }

        if (result == RasterStatus::TransientFailure) {
            ++state.transientFailures;
            scheduleTransientRetry = scheduleTransientRetry
                || state.transientFailures < 3;
        } else {
            state.transientFailures = 0;
        }
        // A stale texture is intentionally not shown for a new signature and
        // must not continue consuming the aggregate budget while hidden.
        releaseTextTexture(root, state);
        Q_ASSERT(root->rasterPixels == pixelsOutsideState);
        return false;
    };

    QSet<QString> firstPassDeferredKeys;
    for (const RenderEntry &entry : snapshot->entries) {
        auto it = root->nodes.find(entry.key);
        if (it == root->nodes.end())
            continue;
        TextNodeState &state = it.value();
        const QString family = familyFor(entry);
        if (!quickWindow || !shouldBuild(state, entry, family))
            continue;
        if (pendingUntexturedBuilds > 0
            && isPureQualityUpgrade(state, entry, family)) {
            // Timeline churn can introduce a new eligible entry while an old
            // committed entry is merely eligible for a higher-resolution
            // texture. Preserve the aggregate remainder for entries that do
            // not have any texture before spending it on a quality upgrade.
            continue;
        }
        if (state.rasterStatus == RasterStatus::TransientFailure
            && signatureChanged(state, entry, family)) {
            state.transientFailures = 0;
        }
        const qint64 previousPixels = state.rasterPixels;
        const bool wasUntextured = previousPixels == 0;
        const qint64 pixelsOutsideState = std::max<qint64>(
            0, root->rasterPixels - previousPixels);
        qint64 rasterPixelBudget = std::max<qint64>(
            0, maximumTotalRasterPixels - pixelsOutsideState);
        if (previousPixels == 0 && untexturedRebuilds > 0) {
            const qint64 unallocatedPixels = std::max<qint64>(
                0, maximumTotalRasterPixels - root->rasterPixels);
            rasterPixelBudget = std::min(
                rasterPixelBudget,
                unallocatedPixels / untexturedRebuilds);
        }
        performBuild(state, entry, family, rasterPixelBudget);
        if (state.rasterStatus == RasterStatus::BudgetDeferred)
            firstPassDeferredKeys.insert(entry.key);
        const bool retainsFairShare = state.rasterStatus
                == RasterStatus::TransientFailure
            && state.transientFailures < 3;
        if (wasUntextured && !retainsFairShare) {
            // A transient texture-creation failure is retried on a following
            // frame. Keep its fair share of the aggregate budget reserved so
            // later entries cannot consume the remainder and turn that retry
            // into BudgetDeferred. True budget deferrals retain the existing
            // second-pass policy below; resolved, permanent, and exhausted
            // attempts no longer participate in this first-pass divisor.
            untexturedRebuilds = std::max(0, untexturedRebuilds - 1);
        }
        if (wasUntextured
            && (state.rasterPixels > 0
                || !shouldBuild(state, entry, family))) {
            pendingUntexturedBuilds = std::max(
                0, pendingUntexturedBuilds - 1);
        }
    }

    // A fair first pass can defer an unusually large early entry even when
    // later short entries leave enough aggregate headroom. Retry only deferred
    // entries once with the actual remainder; persistent insufficiency then
    // waits for a model revision instead of hot-looping every frame.
    if (quickWindow) {
        for (const RenderEntry &entry : snapshot->entries) {
            auto it = root->nodes.find(entry.key);
            if (it == root->nodes.end()
                || it->rasterStatus != RasterStatus::BudgetDeferred
                || !firstPassDeferredKeys.contains(entry.key)) {
                continue;
            }
            const qint64 availablePixels = std::max<qint64>(
                0, maximumTotalRasterPixels - root->rasterPixels);
            if (availablePixels <= 0)
                break;
            performBuild(*it, entry, familyFor(entry), availablePixels);
        }
    }

    QStringList committedVisibleIds;
    committedVisibleIds.reserve(snapshot->entries.size());
    for (const RenderEntry &entry : snapshot->entries) {
        auto it = root->nodes.find(entry.key);
        if (it == root->nodes.end())
            continue;
        TextNodeState &state = it.value();
        if (state.rasterStatus != RasterStatus::Committed || !state.texture) {
            if (state.opacity)
                state.opacity->setOpacity(0.0);
            continue;
        }
        if (!state.attached) {
            root->appendChildNode(state.opacity);
            state.attached = true;
        }

        const double duration = durationForMode(entry.mode, m_scrollDuration);
        const double life = m_renderTime - (entry.start + m_timeOffset);
        const bool visible = life >= 0.0 && life <= duration;
        state.opacity->setOpacity(visible ? 1.0 : 0.0);
        if (!visible)
            continue;
        committedVisibleIds.push_back(entry.commentId);

        const double progress = std::clamp(life / duration, 0.0, 1.0);
        const double x = entry.mode == QStringLiteral("scroll")
            ? width() - progress * (width() + state.textWidth)
            : (width() - state.textWidth) / 2.0;
        const double y = entry.mode == QStringLiteral("bottom")
            ? areaBottom - (entry.lane + 1) * (m_fontSize + 8.0)
            : effectiveTopMargin + entry.lane * (m_fontSize + 8.0);
        QMatrix4x4 matrix;
        matrix.translate(static_cast<float>(x), static_cast<float>(y));
        state.transform->setMatrix(matrix);
    }
    publishCommittedVisibleIds(root->stats, std::move(committedVisibleIds));
    if (snapshotChanged) {
        // Model resets and filter toggles can retain some nodes while
        // reintroducing others. Reorder the reused nodes to the timeline's
        // stable order so overlap stacking remains identical to Repeater.
        for (const RenderEntry &entry : snapshot->entries) {
            auto it = root->nodes.find(entry.key);
            if (it == root->nodes.end() || !it->attached)
                continue;
            root->removeChildNode(it->opacity);
            root->appendChildNode(it->opacity);
        }
    }
    if (scheduleTransientRetry) {
        QMetaObject::invokeMethod(
            this,
            [this] { update(); },
            Qt::QueuedConnection);
    }

    int committedTextureCount = 0;
    int budgetScaledTextureCount = 0;
    int emptyTextureCount = 0;
    int budgetDeferredTextureCount = 0;
    int transientFailureTextureCount = 0;
    int permanentRejectedTextureCount = 0;
    for (const TextNodeState &state : std::as_const(root->nodes)) {
        switch (state.rasterStatus) {
        case RasterStatus::Empty:
            ++emptyTextureCount;
            break;
        case RasterStatus::Committed:
            if (state.texture && state.attached) {
                ++committedTextureCount;
                if (state.budgetScaled)
                    ++budgetScaledTextureCount;
            }
            break;
        case RasterStatus::BudgetDeferred:
            ++budgetDeferredTextureCount;
            break;
        case RasterStatus::PermanentRejected:
            ++permanentRejectedTextureCount;
            break;
        case RasterStatus::TransientFailure:
            ++transientFailureTextureCount;
            break;
        }
    }
    root->stats->estimatedRasterBytes.store(
        root->rasterPixels * 4,
        std::memory_order_relaxed);
    root->stats->committedTextures.store(
        committedTextureCount,
        std::memory_order_relaxed);
    root->stats->budgetScaledTextures.store(
        budgetScaledTextureCount,
        std::memory_order_relaxed);
    root->stats->uncommittedTextures.store(
        root->nodes.size() - committedTextureCount,
        std::memory_order_relaxed);
    root->stats->emptyTextures.store(
        emptyTextureCount,
        std::memory_order_relaxed);
    root->stats->budgetDeferredTextures.store(
        budgetDeferredTextureCount,
        std::memory_order_relaxed);
    root->stats->transientFailureTextures.store(
        transientFailureTextureCount,
        std::memory_order_relaxed);
    root->stats->permanentRejectedTextures.store(
        permanentRejectedTextureCount,
        std::memory_order_relaxed);
    return root;
}

void DanmakuRenderItem::geometryChange(
    const QRectF &newGeometry,
    const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    scheduleVisualUpdate();
}

void DanmakuRenderItem::disconnectModel()
{
    for (const QMetaObject::Connection &connection : std::as_const(m_modelConnections))
        disconnect(connection);
    m_modelConnections.clear();
}

void DanmakuRenderItem::scheduleSnapshotRebuild()
{
    if (m_snapshotRebuildPending)
        return;
    m_snapshotRebuildPending = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_snapshotRebuildPending = false;
            rebuildSnapshot();
        },
        Qt::QueuedConnection);
}

void DanmakuRenderItem::rebuildSnapshot()
{
    if (!m_model) {
        clearSnapshot();
        return;
    }
    const QHash<int, QByteArray> roles = m_model->roleNames();
    const int idRole = roleForName(roles, "commentId");
    const int startRole = roleForName(roles, "start");
    const int modeRole = roleForName(roles, "mode");
    const int colorRole = roleForName(roles, "textColor");
    const int textRole = roleForName(roles, "commentText");
    const int laneRole = roleForName(roles, "lane");
    const int keyRole = roleForName(roles, "entryKey");
    if (idRole < 0 || startRole < 0 || modeRole < 0
        || colorRole < 0 || textRole < 0 || laneRole < 0) {
        clearSnapshot();
        return;
    }

    auto snapshot = std::make_shared<RenderSnapshot>();
    snapshot->generation = m_model->property("generation").toULongLong();
    snapshot->revision = d->nextRevision++;
    const int rowCount = m_model->rowCount();
    snapshot->entries.reserve(rowCount);
    QHash<QString, Private::FontClassCacheEntry> nextFontClassCache;
    nextFontClassCache.reserve(rowCount);
    QHash<QString, int> signatureOccurrences;
    for (int row = 0; row < rowCount; ++row) {
        const QModelIndex index = m_model->index(row, 0);
        RenderEntry entry;
        entry.commentId = m_model->data(index, idRole).toString();
        entry.start = m_model->data(index, startRole).toDouble();
        entry.mode = m_model->data(index, modeRole).toString();
        entry.color = colorFromModelValue(m_model->data(index, colorRole));
        entry.text = m_model->data(index, textRole).toString();
        entry.lane = m_model->data(index, laneRole).toInt();
        if (keyRole >= 0)
            entry.key = m_model->data(index, keyRole).toString();
        if (entry.key.isEmpty()) {
            const QString signature = QStringLiteral("%1\x1f%2\x1f%3\x1f%4\x1f%5")
                                          .arg(entry.commentId)
                                          .arg(entry.start, 0, 'g', 17)
                                          .arg(entry.mode)
                                          .arg(entry.lane)
                                          .arg(entry.text);
            const int occurrence = signatureOccurrences[signature]++;
            entry.key = QStringLiteral("%1\x1e%2\x1e%3")
                            .arg(snapshot->generation)
                            .arg(signature)
                            .arg(occurrence);
        }
        const auto cachedFontClass = d->fontClassCache.constFind(entry.key);
        if (cachedFontClass != d->fontClassCache.cend()
            && cachedFontClass->text == entry.text) {
            entry.containsCjk = cachedFontClass->containsCjk;
        } else {
            entry.containsCjk = containsCjk(entry.text);
        }
        nextFontClassCache.insert(
            entry.key,
            Private::FontClassCacheEntry {entry.text, entry.containsCjk});
        snapshot->entries.push_back(std::move(entry));
    }
    d->fontClassCache = std::move(nextFontClassCache);
    {
        const QMutexLocker lock(&d->snapshotMutex);
        d->snapshot = std::move(snapshot);
    }
    scheduleVisualUpdate();
    emit snapshotChanged();
}

void DanmakuRenderItem::clearSnapshot()
{
    d->fontClassCache.clear();
    auto snapshot = std::make_shared<RenderSnapshot>();
    snapshot->revision = d->nextRevision++;
    {
        const QMutexLocker lock(&d->snapshotMutex);
        d->snapshot = std::move(snapshot);
    }
    // Non-empty lists only come from updatePaintNode(); clearing immediately
    // prevents a hidden or detached item from exposing stale committed IDs.
    publishCommittedVisibleIds(d->renderStats);
    scheduleVisualUpdate();
    emit snapshotChanged();
}

void DanmakuRenderItem::scheduleVisualUpdate()
{
    if (window())
        update();
}
