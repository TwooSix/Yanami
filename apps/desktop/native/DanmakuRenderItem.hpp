#pragma once

#include <QAbstractItemModel>
#include <QMetaObject>
#include <QQuickItem>
#include <QStringList>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include <memory>

class DanmakuRenderItem : public QQuickItem
{
    Q_OBJECT
    QML_NAMED_ELEMENT(DanmakuRenderItem)

    Q_PROPERTY(QAbstractItemModel *model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(double renderTime READ renderTime WRITE setRenderTime NOTIFY renderTimeChanged)
    Q_PROPERTY(double timeOffset READ timeOffset WRITE setTimeOffset NOTIFY timeOffsetChanged)
    Q_PROPERTY(double fontSize READ fontSize WRITE setFontSize NOTIFY fontSizeChanged)
    Q_PROPERTY(double scrollDuration READ scrollDuration WRITE setScrollDuration NOTIFY scrollDurationChanged)
    Q_PROPERTY(double displayArea READ displayArea WRITE setDisplayArea NOTIFY displayAreaChanged)
    Q_PROPERTY(double topMargin READ topMargin WRITE setTopMargin NOTIFY topMarginChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontFamilyChanged)
    Q_PROPERTY(QString cjkFontFamily READ cjkFontFamily WRITE setCjkFontFamily NOTIFY cjkFontFamilyChanged)
    Q_PROPERTY(int snapshotCount READ snapshotCount NOTIFY snapshotChanged)
    Q_PROPERTY(qint64 estimatedCachedRasterBytes READ estimatedCachedRasterBytes)
    Q_PROPERTY(qint64 texturePayloadBudgetBytes READ texturePayloadBudgetBytes CONSTANT)
    Q_PROPERTY(int committedTextureCount READ committedTextureCount)
    Q_PROPERTY(int budgetScaledTextureCount READ budgetScaledTextureCount)
    Q_PROPERTY(int uncommittedTextureCount READ uncommittedTextureCount)
    Q_PROPERTY(int emptyTextureCount READ emptyTextureCount)
    Q_PROPERTY(int budgetDeferredTextureCount READ budgetDeferredTextureCount)
    Q_PROPERTY(int transientFailureTextureCount READ transientFailureTextureCount)
    Q_PROPERTY(int permanentRejectedTextureCount READ permanentRejectedTextureCount)

public:
    explicit DanmakuRenderItem(QQuickItem *parent = nullptr);
    ~DanmakuRenderItem() override;

    QAbstractItemModel *model() const { return m_model; }
    void setModel(QAbstractItemModel *model);

    double renderTime() const { return m_renderTime; }
    void setRenderTime(double value);

    double timeOffset() const { return m_timeOffset; }
    void setTimeOffset(double value);

    double fontSize() const { return m_fontSize; }
    void setFontSize(double value);

    double scrollDuration() const { return m_scrollDuration; }
    void setScrollDuration(double value);

    double displayArea() const { return m_displayArea; }
    void setDisplayArea(double value);

    double topMargin() const { return m_topMargin; }
    void setTopMargin(double value);

    QString fontFamily() const { return m_fontFamily; }
    void setFontFamily(const QString &value);

    QString cjkFontFamily() const { return m_cjkFontFamily; }
    void setCjkFontFamily(const QString &value);

    int snapshotCount() const;
    qint64 estimatedCachedRasterBytes() const;
    qint64 texturePayloadBudgetBytes() const;
    int committedTextureCount() const;
    int budgetScaledTextureCount() const;
    int uncommittedTextureCount() const;
    int emptyTextureCount() const;
    int budgetDeferredTextureCount() const;
    int transientFailureTextureCount() const;
    int permanentRejectedTextureCount() const;

    // Hosted probes can compare the renderer's exact time-window candidates
    // with an independent fixture oracle without walking 100k source rows.
    Q_INVOKABLE QStringList eligibleCandidateIds() const;

    // Unlike eligibleCandidateIds(), this is render-thread evidence: an ID is
    // published only after its texture is committed and visible in the scene
    // graph. The mutex-protected copy is safe to inspect from the GUI thread.
    Q_INVOKABLE QStringList committedVisibleCandidateIds() const;

signals:
    void modelChanged();
    void renderTimeChanged();
    void timeOffsetChanged();
    void fontSizeChanged();
    void scrollDurationChanged();
    void displayAreaChanged();
    void topMarginChanged();
    void fontFamilyChanged();
    void cjkFontFamilyChanged();
    void snapshotChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    class Private;

    void disconnectModel();
    void scheduleSnapshotRebuild();
    void rebuildSnapshot();
    void clearSnapshot();
    void scheduleVisualUpdate();

    std::unique_ptr<Private> d;
    QAbstractItemModel *m_model = nullptr;
    QVector<QMetaObject::Connection> m_modelConnections;
    bool m_snapshotRebuildPending = false;
    double m_renderTime = 0.0;
    double m_timeOffset = 0.0;
    double m_fontSize = 42.0;
    double m_scrollDuration = 9.0;
    double m_displayArea = 0.70;
    double m_topMargin = 0.0;
    QString m_fontFamily;
    QString m_cjkFontFamily;
};
