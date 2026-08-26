#pragma once

#include "UpscalingCatalog.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

#include <memory>

class QCryptographicHash;
class QLockFile;
class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
class QUrl;

class UpscalingAssetManager final : public QObject
{
    Q_OBJECT

public:
    explicit UpscalingAssetManager(
        const QString &assetRoot,
        QObject *parent = nullptr);
    UpscalingAssetManager(
        QNetworkAccessManager *network,
        const QString &assetRoot,
        QObject *parent = nullptr);
    ~UpscalingAssetManager() override;

    QString assetRoot() const { return m_assetRoot; }
    bool hasState(const QString &assetSetId) const
    { return m_states.contains(assetSetId); }
    QVariantMap stateFor(const QString &assetSetId) const;
    QString absolutePath(
        const YanamiUpscaling::ShaderArtifact &artifact) const;

    void verify(
        const QString &assetSetId,
        const QVector<YanamiUpscaling::ShaderArtifact> &artifacts);
    void download(
        const QString &assetSetId,
        const QVector<YanamiUpscaling::ShaderArtifact> &artifacts);
    void cancel(const QString &assetSetId);

signals:
    void stateChanged(const QString &assetSetId);

private:
    struct AssetState {
        QString phase = QStringLiteral("missing");
        QString version;
        QString errorCode;
        QString errorMessage;
        qint64 bytesReceived = 0;
        qint64 bytesTotal = 0;
        quint64 generation = 0;
        QVector<YanamiUpscaling::ShaderArtifact> artifacts;
        QVector<YanamiUpscaling::ShaderArtifact> downloadArtifacts;
    };

    struct VerificationResult {
        QVector<YanamiUpscaling::ShaderArtifact> missingOrInvalid;
        bool hadInvalidFile = false;
    };

    struct DownloadTask {
        QString assetSetId;
        quint64 generation = 0;
        QVector<YanamiUpscaling::ShaderArtifact> artifacts;
        qsizetype artifactIndex = 0;
        qint64 cachedBytes = 0;
        qint64 completedDownloadBytes = 0;
        qint64 currentFileBytes = 0;
    };

    void beginVerification(
        const QString &assetSetId,
        const QVector<YanamiUpscaling::ShaderArtifact> &artifacts,
        bool downloadWhenMissing,
        const QString &phase);
    void reverifyStatesSharingArtifacts(
        const QString &readyAssetSetId,
        const QVector<YanamiUpscaling::ShaderArtifact> &readyArtifacts);
    void enqueueDownload(
        const QString &assetSetId,
        quint64 generation,
        const QVector<YanamiUpscaling::ShaderArtifact> &missingArtifacts);
    void startNextDownload();
    void startCurrentArtifact();
    void drainCurrentReply(bool drainAll = false);
    void finishCurrentReply();
    void finishCurrentArtifact();
    void finishDownloadTask();
    void failCurrentTask(
        const QString &errorCode,
        const QString &message);
    void publishProgress(bool force = false);
    void resetTransport();
    bool isAllowedDownloadUrl(const QUrl &url) const;
    void publishState(const QString &assetSetId);

    QString m_assetRoot;
    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QHash<QString, AssetState> m_states;
    QStringList m_downloadQueue;
    std::unique_ptr<DownloadTask> m_currentTask;
    std::unique_ptr<QSaveFile> m_saveFile;
    std::unique_ptr<QCryptographicHash> m_hash;
    std::unique_ptr<QLockFile> m_lock;
    QElapsedTimer m_progressPublishTimer;
};
