#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class UpdateHelperProcess;

class UpdateChecker final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY stateChanged)
    Q_PROPERTY(QUrl releaseUrl READ releaseUrl NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
    Q_PROPERTY(bool hasChecked READ hasChecked NOTIFY stateChanged)
    Q_PROPERTY(bool releaseFound READ releaseFound NOTIFY stateChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool directUpdateSupported READ directUpdateSupported NOTIFY stateChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY stateChanged)
    Q_PROPERTY(bool updateReady READ updateReady NOTIFY stateChanged)
    Q_PROPERTY(bool applying READ applying NOTIFY stateChanged)
    Q_PROPERTY(int downloadProgress READ downloadProgress NOTIFY stateChanged)
    Q_PROPERTY(bool incrementalUpdate READ incrementalUpdate NOTIFY stateChanged)
    Q_PROPERTY(qint64 downloadSize READ downloadSize NOTIFY stateChanged)

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // Deterministic transport seam used by unit tests. Production owns its
    // QNetworkAccessManager and first asks the installed update helper.
    UpdateChecker(
        QNetworkAccessManager *network,
        const QUrl &endpoint,
        const QString &currentVersion,
        QObject *parent = nullptr);
    UpdateChecker(
        QNetworkAccessManager *network,
        const QUrl &endpoint,
        const QString &currentVersion,
        const QString &helperPath,
        QObject *parent = nullptr);
    ~UpdateChecker() override;

    QString currentVersion() const { return m_currentVersion; }
    QString latestVersion() const { return m_latestVersion; }
    QUrl releaseUrl() const { return m_releaseUrl; }
    QString errorMessage() const { return m_errorMessage; }
    bool checking() const { return m_checking; }
    bool hasChecked() const { return m_hasChecked; }
    bool releaseFound() const { return m_releaseFound; }
    bool updateAvailable() const { return m_updateAvailable; }
    bool directUpdateSupported() const { return m_directUpdateSupported; }
    bool downloading() const { return m_downloading; }
    bool updateReady() const { return m_updateReady; }
    bool applying() const { return m_applying; }
    int downloadProgress() const { return m_downloadProgress; }
    bool incrementalUpdate() const { return m_incrementalUpdate; }
    qint64 downloadSize() const { return m_downloadSize; }

    Q_INVOKABLE void check();
    Q_INVOKABLE void downloadUpdate();
    Q_INVOKABLE void applyUpdate();
    Q_INVOKABLE void cancelDownload();

signals:
    void stateChanged();

private:
    enum class HelperOperation {
        None,
        Check,
        Download,
        Apply,
    };

    void startNetworkCheck();
    void startHelper(HelperOperation operation, const QStringList &arguments);
    void readHelperOutput(const QByteArray &output);
    void handleHelperLine(const QByteArray &line);
    void finishHelper(int exitCode, bool crashed);
    void finishRequest();
    void finishWithError(const QString &message);
    void finishWithoutRelease();
    void resetReleaseState();

    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QPointer<UpdateHelperProcess> m_process;
    QUrl m_endpoint;
    QString m_helperPath;
    QString m_currentVersion;
    QString m_latestVersion;
    QUrl m_releaseUrl;
    QString m_errorMessage;
    QByteArray m_helperOutput;
    HelperOperation m_helperOperation = HelperOperation::None;
    bool m_helperProducedResult = false;
    bool m_cancelled = false;
    bool m_checking = false;
    bool m_hasChecked = false;
    bool m_releaseFound = false;
    bool m_updateAvailable = false;
    bool m_directUpdateSupported = false;
    bool m_downloading = false;
    bool m_updateReady = false;
    bool m_applying = false;
    int m_downloadProgress = 0;
    bool m_incrementalUpdate = false;
    qint64 m_downloadSize = 0;
};
