#pragma once

#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

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

public:
    explicit UpdateChecker(QObject *parent = nullptr);

    // Deterministic transport seam used by unit tests. Production owns its
    // QNetworkAccessManager and uses the public GitHub Releases endpoint.
    UpdateChecker(
        QNetworkAccessManager *network,
        const QUrl &endpoint,
        const QString &currentVersion,
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

    Q_INVOKABLE void check();

signals:
    void stateChanged();

private:
    void finishRequest();
    void finishWithError(const QString &message);
    void finishWithoutRelease();

    QPointer<QNetworkAccessManager> m_network;
    QPointer<QNetworkReply> m_reply;
    QUrl m_endpoint;
    QString m_currentVersion;
    QString m_latestVersion;
    QUrl m_releaseUrl;
    QString m_errorMessage;
    bool m_checking = false;
    bool m_hasChecked = false;
    bool m_releaseFound = false;
    bool m_updateAvailable = false;
};
