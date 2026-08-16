#include "UpdateChecker.hpp"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVersionNumber>

#include <algorithm>

namespace {
Q_LOGGING_CATEGORY(updateLog, "yanami.update")

const QUrl defaultReleaseEndpoint(QStringLiteral(
    "https://api.github.com/repos/TwooSix/Yanami/releases/latest"));

struct SemanticVersion
{
    QVersionNumber core;
    QStringList prerelease;
    bool valid = false;
};

SemanticVersion parseVersion(QString value)
{
    value = value.trimmed();
    if (value.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        value.remove(0, 1);

    static const QRegularExpression pattern(QStringLiteral(
        R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)"));
    const QRegularExpressionMatch match = pattern.match(value);
    if (!match.hasMatch())
        return {};

    SemanticVersion result;
    result.core = QVersionNumber(
        match.captured(1).toInt(),
        match.captured(2).toInt(),
        match.captured(3).toInt());
    if (!match.captured(4).isEmpty()) {
        result.prerelease = match.captured(4).split(QLatin1Char('.'));
        for (const QString &identifier : result.prerelease) {
            if (identifier.size() > 1 && identifier.front() == QLatin1Char('0')
                && std::ranges::all_of(identifier, [](const QChar character) {
                    return character.isDigit();
                })) {
                return {};
            }
        }
    }
    result.valid = true;
    return result;
}

bool isNumericIdentifier(const QString &identifier)
{
    return !identifier.isEmpty()
        && std::ranges::all_of(identifier, [](const QChar character) {
               return character.isDigit();
           });
}

int compareNumericIdentifiers(const QString &left, const QString &right)
{
    if (left.size() != right.size())
        return left.size() < right.size() ? -1 : 1;
    return QString::compare(left, right, Qt::CaseSensitive);
}

int compareVersions(const SemanticVersion &left, const SemanticVersion &right)
{
    const int coreComparison = QVersionNumber::compare(left.core, right.core);
    if (coreComparison != 0)
        return coreComparison;
    if (left.prerelease.isEmpty() || right.prerelease.isEmpty()) {
        if (left.prerelease.isEmpty() == right.prerelease.isEmpty())
            return 0;
        return left.prerelease.isEmpty() ? 1 : -1;
    }

    const qsizetype commonLength = std::min(
        left.prerelease.size(), right.prerelease.size());
    for (qsizetype index = 0; index < commonLength; ++index) {
        const QString &leftIdentifier = left.prerelease.at(index);
        const QString &rightIdentifier = right.prerelease.at(index);
        const bool leftNumeric = isNumericIdentifier(leftIdentifier);
        const bool rightNumeric = isNumericIdentifier(rightIdentifier);
        if (leftNumeric != rightNumeric)
            return leftNumeric ? -1 : 1;
        const int identifierComparison = leftNumeric
            ? compareNumericIdentifiers(leftIdentifier, rightIdentifier)
            : QString::compare(
                  leftIdentifier, rightIdentifier, Qt::CaseSensitive);
        if (identifierComparison != 0)
            return identifierComparison;
    }
    if (left.prerelease.size() == right.prerelease.size())
        return 0;
    return left.prerelease.size() < right.prerelease.size() ? -1 : 1;
}
}

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_endpoint(defaultReleaseEndpoint)
    , m_currentVersion(QCoreApplication::applicationVersion())
{
}

UpdateChecker::UpdateChecker(
    QNetworkAccessManager *network,
    const QUrl &endpoint,
    const QString &currentVersion,
    QObject *parent)
    : QObject(parent)
    , m_network(network)
    , m_endpoint(endpoint)
    , m_currentVersion(currentVersion)
{
}

UpdateChecker::~UpdateChecker()
{
    if (m_reply)
        m_reply->abort();
}

void UpdateChecker::check()
{
    if (m_checking)
        return;
    if (!m_network || !m_endpoint.isValid()) {
        finishWithError(tr("Could not check for updates. Please try again later."));
        return;
    }

    m_checking = true;
    m_errorMessage.clear();
    emit stateChanged();

    QNetworkRequest request(m_endpoint);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2026-03-10");
    request.setRawHeader(
        "User-Agent",
        QByteArrayLiteral("Yanami/") + m_currentVersion.toUtf8());
    request.setTransferTimeout(10000);
    request.setMaximumRedirectsAllowed(3);
    request.setAttribute(
        QNetworkRequest::RedirectPolicyAttribute,
        QNetworkRequest::NoLessSafeRedirectPolicy);

    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::finished,
        this, &UpdateChecker::finishRequest);
}

void UpdateChecker::finishRequest()
{
    QNetworkReply *reply = m_reply.data();
    if (!reply)
        return;
    m_reply.clear();

    const int httpStatus = reply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QByteArray payload = reply->readAll();
    reply->deleteLater();

    if (httpStatus == 404) {
        qCInfo(updateLog) << "update_check_complete" << "result=no_release";
        finishWithoutRelease();
        return;
    }
    if (networkError != QNetworkReply::NoError || httpStatus != 200) {
        qCWarning(updateLog)
            << "update_check_failed"
            << "httpStatus=" << httpStatus
            << "networkError=" << static_cast<int>(networkError);
        finishWithError(tr("Could not check for updates. Please try again later."));
        return;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qCWarning(updateLog) << "update_check_failed" << "reason=invalid_json";
        finishWithError(tr("The update service returned an invalid response."));
        return;
    }

    const QJsonObject object = document.object();
    const QString tag = object.value(QStringLiteral("tag_name")).toString();
    const QUrl releaseUrl(object.value(QStringLiteral("html_url")).toString());
    const SemanticVersion current = parseVersion(m_currentVersion);
    const SemanticVersion latest = parseVersion(tag);
    if (!current.valid || !latest.valid
        || releaseUrl.scheme() != QStringLiteral("https")
        || releaseUrl.host().compare(
               QStringLiteral("github.com"), Qt::CaseInsensitive) != 0) {
        qCWarning(updateLog) << "update_check_failed" << "reason=invalid_release";
        finishWithError(tr("The update service returned an invalid response."));
        return;
    }

    m_checking = false;
    m_hasChecked = true;
    m_releaseFound = true;
    m_latestVersion = tag;
    m_releaseUrl = releaseUrl;
    m_errorMessage.clear();
    m_updateAvailable = compareVersions(latest, current) > 0;
    qCInfo(updateLog)
        << "update_check_complete"
        << "currentVersion=" << m_currentVersion
        << "latestVersion=" << m_latestVersion
        << "updateAvailable=" << m_updateAvailable;
    emit stateChanged();
}

void UpdateChecker::finishWithError(const QString &message)
{
    m_checking = false;
    m_hasChecked = true;
    m_releaseFound = false;
    m_updateAvailable = false;
    m_latestVersion.clear();
    m_releaseUrl.clear();
    m_errorMessage = message;
    emit stateChanged();
}

void UpdateChecker::finishWithoutRelease()
{
    m_checking = false;
    m_hasChecked = true;
    m_releaseFound = false;
    m_updateAvailable = false;
    m_latestVersion.clear();
    m_releaseUrl.clear();
    m_errorMessage.clear();
    emit stateChanged();
}
