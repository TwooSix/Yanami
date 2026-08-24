#pragma once

#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

class DanmakuTimelineModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged FINAL)
    Q_PROPERTY(QVariantList comments READ comments WRITE setComments
                   NOTIFY commentsChanged FINAL)
    Q_PROPERTY(quint64 sessionGeneration READ sessionGeneration
                   WRITE setSessionGeneration NOTIFY sessionGenerationChanged FINAL)
    Q_PROPERTY(qreal viewportWidth READ viewportWidth WRITE setViewportWidth
                   NOTIFY viewportWidthChanged FINAL)
    Q_PROPERTY(qreal fontSize READ fontSize WRITE setFontSize
                   NOTIFY fontSizeChanged FINAL)
    Q_PROPERTY(qreal scrollDuration READ scrollDuration WRITE setScrollDuration
                   NOTIFY scrollDurationChanged FINAL)
    Q_PROPERTY(qreal timeOffset READ timeOffset WRITE setTimeOffset
                   NOTIFY timeOffsetChanged FINAL)
    Q_PROPERTY(QString blockedTerms READ blockedTerms WRITE setBlockedTerms
                   NOTIFY blockedTermsChanged FINAL)
    Q_PROPERTY(bool showScroll READ showScroll WRITE setShowScroll
                   NOTIFY showScrollChanged FINAL)
    Q_PROPERTY(bool showTop READ showTop WRITE setShowTop
                   NOTIFY showTopChanged FINAL)
    Q_PROPERTY(bool showBottom READ showBottom WRITE setShowBottom
                   NOTIFY showBottomChanged FINAL)
    Q_PROPERTY(int density READ density WRITE setDensity
                   NOTIFY densityChanged FINAL)
    Q_PROPERTY(int laneCount READ laneCount WRITE setLaneCount
                   NOTIFY laneCountChanged FINAL)
    Q_PROPERTY(bool preparing READ preparing NOTIFY preparingChanged FINAL)
    Q_PROPERTY(int timelineCount READ timelineCount NOTIFY timelineCountChanged FINAL)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY pendingCountChanged FINAL)
    Q_PROPERTY(quint64 generation READ generation NOTIFY generationChanged FINAL)

public:
    enum Role {
        CommentIdRole = Qt::UserRole + 1,
        StartRole,
        ModeRole,
        TextColorRole,
        CommentTextRole,
        LaneRole,
        DensityRankRole,
        SourceOrdinalRole,
        TimelineIndexRole,
        EntryKeyRole,
    };
    Q_ENUM(Role)

    explicit DanmakuTimelineModel(QObject *parent = nullptr);
    ~DanmakuTimelineModel() override;

    Q_DISABLE_COPY_MOVE(DanmakuTimelineModel)

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QVariantList comments() const;
    void setComments(const QVariantList &comments);

    quint64 sessionGeneration() const;
    void setSessionGeneration(quint64 generation);

    qreal viewportWidth() const;
    void setViewportWidth(qreal width);

    qreal fontSize() const;
    void setFontSize(qreal size);

    qreal scrollDuration() const;
    void setScrollDuration(qreal seconds);

    qreal timeOffset() const;
    void setTimeOffset(qreal seconds);

    QString blockedTerms() const;
    void setBlockedTerms(const QString &terms);

    bool showScroll() const;
    void setShowScroll(bool show);

    bool showTop() const;
    void setShowTop(bool show);

    bool showBottom() const;
    void setShowBottom(bool show);

    int density() const;
    void setDensity(int density);

    int laneCount() const;
    void setLaneCount(int count);

    bool preparing() const;
    int timelineCount() const;
    int pendingCount() const;
    quint64 generation() const;

    // This atomic form is the safe episode/session transition API. It avoids
    // depending on the relative order of two QML property-binding updates.
    Q_INVOKABLE void replaceComments(
        const QVariantList &comments,
        quint64 sessionGeneration);
    Q_INVOKABLE void syncActive(qreal position, bool force = false);

    // entryAt() intentionally inspects the compact full timeline without
    // exposing it as the model. Only the active window creates QML delegates.
    Q_INVOKABLE QVariantMap entryAt(int timelineIndex) const;
    Q_INVOKABLE QVariantMap get(int activeRow) const;
    Q_INVOKABLE int activeTimelineIndexAt(int activeRow) const;

signals:
    void countChanged();
    void commentsChanged();
    void sessionGenerationChanged();
    void viewportWidthChanged();
    void fontSizeChanged();
    void scrollDurationChanged();
    void timeOffsetChanged();
    void blockedTermsChanged();
    void showScrollChanged();
    void showTopChanged();
    void showBottomChanged();
    void densityChanged();
    void laneCountChanged();
    void preparingChanged();
    void timelineCountChanged();
    void pendingCountChanged();
    void generationChanged();

private:
    class Private;
    std::unique_ptr<Private> d;
};
