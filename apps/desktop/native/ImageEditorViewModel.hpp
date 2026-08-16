#pragma once

#include "AsyncResourceState.hpp"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class MediaPort;

class ImageEditorViewModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(AsyncResourceState *initial READ initial CONSTANT)
    Q_PROPERTY(AsyncResourceState *searchState READ searchState CONSTANT)
    Q_PROPERTY(bool opened READ opened NOTIFY openedChanged)
    Q_PROPERTY(QString itemId READ itemId NOTIFY identityChanged)
    Q_PROPERTY(QString resourceKey READ resourceKey NOTIFY identityChanged)
    Q_PROPERTY(quint64 sessionGeneration READ sessionGeneration WRITE setSessionGeneration
            NOTIFY sessionGenerationChanged)
    Q_PROPERTY(quint64 viewGeneration READ viewGeneration NOTIFY identityChanged)
    Q_PROPERTY(quint64 searchGeneration READ searchGeneration NOTIFY searchGenerationChanged)
    Q_PROPERTY(QVariantMap editor READ editor NOTIFY editorChanged)
    Q_PROPERTY(QVariantList slotsModel READ slotsModel NOTIFY modelsChanged)
    Q_PROPERTY(QVariantList backdropsModel READ backdropsModel NOTIFY modelsChanged)
    Q_PROPERTY(int nextBackdropIndex READ nextBackdropIndex NOTIFY modelsChanged)
    Q_PROPERTY(QVariantMap pendingContext READ pendingContext WRITE setPendingContext
            NOTIFY pendingContextChanged)
    Q_PROPERTY(QVariantMap mutationStates READ mutationStates NOTIFY mutationStatesChanged)

public:
    explicit ImageEditorViewModel(QObject *parent = nullptr);
    ImageEditorViewModel(MediaPort *port, QObject *parent);

    AsyncResourceState *initial() { return &m_initial; }
    AsyncResourceState *searchState() { return &m_search; }
    bool opened() const { return m_opened; }
    QString itemId() const { return m_itemId; }
    QString resourceKey() const { return m_resourceKey; }
    quint64 sessionGeneration() const { return m_sessionGeneration; }
    quint64 viewGeneration() const { return m_viewGeneration; }
    quint64 searchGeneration() const { return m_searchGeneration; }
    QVariantMap editor() const { return m_editor; }
    QVariantList slotsModel() const { return m_slotsModel; }
    QVariantList backdropsModel() const { return m_backdropsModel; }
    int nextBackdropIndex() const { return m_nextBackdropIndex; }
    QVariantMap pendingContext() const { return m_pendingContext; }
    QVariantMap mutationStates() const { return m_mutationStates; }

    void setSessionGeneration(quint64 generation);
    void setPendingContext(const QVariantMap &context);

    // Opens only the synchronous shell. The returned map is the complete
    // request identity which must accompany the eventual editor response.
    Q_INVOKABLE QVariantMap open(const QVariantMap &item);
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void retry();
    Q_INVOKABLE bool applyEditor(
        const QVariantMap &editor,
        const QVariantMap &requestContext);
    Q_INVOKABLE bool failInitial(
        const QString &message,
        const QVariantMap &requestContext);

    Q_INVOKABLE QVariantMap selectTarget(
        const QString &imageType,
        const QVariant &imageIndex,
        const QString &mode);
    Q_INVOKABLE void clearPendingContext();

    // Every beginSearch call supersedes the preceding search for this view,
    // including searches which used a different provider or filter set.
    Q_INVOKABLE QVariantMap beginSearch(const QVariantMap &filters);
    Q_INVOKABLE QVariantMap search(const QVariantMap &filters);
    Q_INVOKABLE bool applySearch(
        const QVariantMap &result,
        const QVariantMap &requestContext);
    Q_INVOKABLE bool failSearch(
        const QString &message,
        const QVariantMap &requestContext);
    Q_INVOKABLE void cancelSearch();

    // Mutations are independent per exact image card. Each typed intent
    // applies its optimistic image (or delete) and retains only the affected
    // card snapshot, so another card can mutate concurrently.
    Q_INVOKABLE QVariantMap applyRemote(
        const QVariantMap &targetContext,
        const QVariantMap &remoteImage);
    Q_INVOKABLE QVariantMap upload(
        const QVariantMap &targetContext,
        const QString &fileUrl);
    Q_INVOKABLE QVariantMap remove(const QVariantMap &targetContext);
    Q_INVOKABLE bool mutationSucceeded(const QVariantMap &requestContext);
    Q_INVOKABLE bool mutationFailed(
        const QVariantMap &requestContext,
        const QString &message);
    Q_INVOKABLE void mutationReconciled(const QVariantMap &requestContext);
    Q_INVOKABLE void clearMutationState(const QVariantMap &targetContext);
    Q_INVOKABLE QString cardKey(const QVariantMap &targetContext) const;

signals:
    void openedChanged();
    void identityChanged();
    void sessionGenerationChanged();
    void searchGenerationChanged();
    void editorChanged();
    void modelsChanged();
    void pendingContextChanged();
    void mutationStatesChanged();
    void editorReady(const QVariantMap &editor, quint64 viewGeneration);
    void providersReady(const QVariantMap &providers, quint64 viewGeneration);
    void searchReady(const QVariantMap &result, quint64 searchGeneration);
    void mutationCommitted(const QVariantMap &requestContext);
    void editorRequestFailed(
        const QString &itemId,
        const QString &message,
        bool nonModal);
    void providersRequestFailed(
        const QString &itemId,
        const QString &message,
        bool nonModal);
    void searchRequestFailed(
        const QString &itemId,
        const QString &message,
        bool nonModal);
    void mutationRequestFailed(
        const QString &itemId,
        const QString &message,
        bool nonModal);
    void reconciliationRequestFailed(
        const QString &itemId,
        const QString &message,
        bool nonModal);

private:
    struct MutationRecord
    {
        quint64 mutationId = 0;
        QString resourceKey;
        QString cardKey;
        QString kind;
        QVariantMap context;
        bool hadPreviousImage = false;
        QVariantMap previousImage;
        int previousPosition = -1;
        bool insertedOptimisticImage = false;
        QVariantMap optimisticImage;
    };

    static const QStringList &ordinaryImageTypes();
    static QVariant normalizedImageIndex(const QVariant &value);
    static bool sameImageIndex(const QVariant &left, const QVariant &right);
    static bool imageComesBefore(const QVariantMap &left, const QVariantMap &right);
    static QVariantMap normalizedTargetContext(const QVariantMap &context);
    static bool sameTargetContext(const QVariantMap &left, const QVariantMap &right);
    static QVariantList editorImages(const QVariantMap &editor);
    static QVariantMap requestIdentity(
        quint64 requestId,
        const QString &resourceKey,
        quint64 sessionGeneration,
        quint64 viewGeneration);

    bool responseMatchesCurrentView(const QVariantMap &requestContext) const;
    bool searchResponseMatches(
        const QVariantMap &requestContext,
        const QVariantMap &result) const;
    bool mutationResponseMatches(
        const MutationRecord &record,
        const QVariantMap &requestContext) const;
    int findTargetImage(const QVariantList &images, const QVariantMap &context) const;
    int findImageIdentity(const QVariantList &images, const QVariantMap &image) const;
    QString searchResourceKey(const QVariantMap &filters) const;
    QVariantMap mutationState(
        const MutationRecord &record,
        const QString &phase,
        const QString &errorMessage = {}) const;
    QVariantMap beginMutation(
        const QString &kind,
        const QVariantMap &targetContext,
        const QVariantMap &optimisticImage);
    void setMutationState(const QString &key, const QVariantMap &state);
    void rebuildModels();
    void requestInitial(bool preserveData);
    void requestProviders();
    void reconcileMutations();
    void handleCompleted(
        const QString &requestId,
        const QString &itemId,
        int operationValue,
        const QVariantMap &result);
    void handleFailed(
        const QString &requestId,
        const QString &itemId,
        int operationValue,
        const QString &message,
        bool nonModal);

    QPointer<MediaPort> m_port;
    AsyncResourceState m_initial;
    AsyncResourceState m_search;
    bool m_opened = false;
    QString m_itemId;
    QString m_resourceKey;
    quint64 m_sessionGeneration = 0;
    quint64 m_viewGeneration = 0;
    quint64 m_searchGeneration = 0;
    quint64 m_nextProviderRequestId = 0;
    quint64 m_nextMutationId = 0;
    QVariantMap m_editor;
    QVariantList m_slotsModel;
    QVariantList m_backdropsModel;
    int m_nextBackdropIndex = 0;
    QVariantMap m_pendingContext;
    QVariantMap m_mutationStates;
    QHash<QString, MutationRecord> m_mutations;
    QVariantMap m_initialRequestContext;
    QVariantMap m_searchRequestContext;
    QString m_initialTransportRequestId;
    QString m_providerRequestId;
    QString m_searchTransportRequestId;
    QList<QVariantMap> m_reconciliationContexts;
    QHash<QString, QVariantMap> m_pendingMutationContexts;
    int m_transportDispatchDepth = 0;
};
