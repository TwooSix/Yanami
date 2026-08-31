#include "UpdateChecker.hpp"
#include "UpdateHelperProcess.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QVersionNumber>

#include <algorithm>
#include <optional>

namespace {
Q_LOGGING_CATEGORY(updateLog, "yanami.update")

const QUrl defaultReleaseEndpoint(QStringLiteral(
    "https://api.github.com/repos/TwooSix/Yanami/releases?per_page=30"));
const QString releaseTagPrefix(QStringLiteral(
    "https://github.com/TwooSix/Yanami/releases/tag/v"));

struct SemanticVersion
{
    QVersionNumber core;
    QStringList prerelease;
    bool valid = false;
};

struct ReleaseCandidate
{
    QString tag;
    QUrl url;
    SemanticVersion version;
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

bool isTrustedReleaseUrl(const QUrl &url)
{
    return url.scheme() == QStringLiteral("https")
        && url.host().compare(
               QStringLiteral("github.com"), Qt::CaseInsensitive) == 0;
}
}

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_endpoint(defaultReleaseEndpoint)
    , m_currentVersion(QCoreApplication::applicationVersion())
{
#ifdef Q_OS_WIN
    const QString candidate = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("yanami-updater.exe"));
    if (QFileInfo(candidate).isFile())
        m_helperPath = candidate;
#endif
}

UpdateChecker::UpdateChecker(
    QNetworkAccessManager *network,
    const QUrl &endpoint,
    const QString &currentVersion,
    QObject *parent)
    : UpdateChecker(network, endpoint, currentVersion, QString(), parent)
{
}

UpdateChecker::UpdateChecker(
    QNetworkAccessManager *network,
    const QUrl &endpoint,
    const QString &currentVersion,
    const QString &helperPath,
    QObject *parent)
    : QObject(parent)
    , m_network(network)
    , m_endpoint(endpoint)
    , m_helperPath(helperPath)
    , m_currentVersion(currentVersion)
{
}

UpdateChecker::~UpdateChecker()
{
    if (m_reply)
        m_reply->abort();
    if (m_process)
        m_process->cancel();
}

void UpdateChecker::resetReleaseState()
{
    m_hasChecked = false;
    m_releaseFound = false;
    m_updateAvailable = false;
    m_latestVersion.clear();
    m_releaseUrl.clear();
    m_errorMessage.clear();
    m_directUpdateSupported = false;
    m_incrementalUpdate = false;
    m_downloadSize = 0;
    m_downloadProgress = 0;
    m_updateReady = false;
}

void UpdateChecker::check()
{
    if (m_checking || m_downloading || m_applying || m_process)
        return;
    if (!m_network || !m_endpoint.isValid()) {
        finishWithError(tr("Could not check for updates. Please try again later."));
        return;
    }

    resetReleaseState();
    m_checking = true;
    emit stateChanged();

    if (!m_helperPath.isEmpty()) {
        startHelper(HelperOperation::Check, {QStringLiteral("check")});
        return;
    }
    startNetworkCheck();
}

void UpdateChecker::startNetworkCheck()
{
    QNetworkRequest request(m_endpoint);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
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

void UpdateChecker::startHelper(
    HelperOperation operation, const QStringList &arguments)
{
    if (m_process)
        return;

    auto *process = new UpdateHelperProcess(this);
    m_process = process;
    m_helperOperation = operation;
    m_helperOutput.clear();
    m_helperProducedResult = false;
    m_cancelled = false;
    connect(process, &UpdateHelperProcess::outputReady,
        this, &UpdateChecker::readHelperOutput);
    connect(process, &UpdateHelperProcess::finished,
        this, &UpdateChecker::finishHelper);
    // QProcess::start() itself can spend time inside Windows CreateProcess.
    // The transport owns even process creation off-thread, so the checking
    // state can reach the render loop immediately after this method returns.
    process->start(m_helperPath, arguments,
        operation == HelperOperation::Check ? 15000 : 0);
}

void UpdateChecker::readHelperOutput(const QByteArray &output)
{
    if (!m_process)
        return;
    m_helperOutput += output;
    while (true) {
        const qsizetype newline = m_helperOutput.indexOf('\n');
        if (newline < 0)
            break;
        const QByteArray line = m_helperOutput.left(newline).trimmed();
        m_helperOutput.remove(0, newline + 1);
        if (!line.isEmpty())
            handleHelperLine(line);
    }
}

void UpdateChecker::handleHelperLine(const QByteArray &line)
{
    if (m_cancelled && m_helperOperation == HelperOperation::Download)
        return;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return;
    const QJsonObject object = document.object();
    const QString event = object.value(QStringLiteral("event")).toString();

    if (event == QStringLiteral("check")) {
        const QString status = object.value(QStringLiteral("status")).toString();
        if (status != QStringLiteral("available")
            && status != QStringLiteral("current")
            && status != QStringLiteral("empty")
            && status != QStringLiteral("ready")) {
            return;
        }
        m_helperProducedResult = true;
        m_directUpdateSupported = true;
        m_hasChecked = true;
        m_errorMessage.clear();
        if (status == QStringLiteral("available")
            || status == QStringLiteral("ready")) {
            const QString version = object.value(QStringLiteral("version"))
                .toString().trimmed();
            const SemanticVersion parsed = parseVersion(version);
            if (!parsed.valid) {
                finishWithError(tr("The update service returned an invalid response."));
                return;
            }
            m_releaseFound = true;
            m_updateAvailable = true;
            m_updateReady = status == QStringLiteral("ready");
            m_downloadProgress = m_updateReady ? 100 : 0;
            m_latestVersion = version.startsWith(QLatin1Char('v'))
                ? version : QStringLiteral("v") + version;
            m_releaseUrl = QUrl(releaseTagPrefix +
                (version.startsWith(QLatin1Char('v'))
                    ? version.sliced(1) : version));
            m_incrementalUpdate = object.value(QStringLiteral("delta_count"))
                .toInteger() > 0;
            m_downloadSize = std::max<qint64>(0,
                object.value(QStringLiteral("download_size")).toInteger());
        } else {
            m_releaseFound = status == QStringLiteral("current");
            m_updateAvailable = false;
            m_downloadProgress = 0;
        }
        emit stateChanged();
        return;
    }

    if (event == QStringLiteral("progress")
        && m_helperOperation == HelperOperation::Download) {
        m_helperProducedResult = true;
        m_downloadProgress = std::clamp(
            object.value(QStringLiteral("percent")).toInt(), 0, 100);
        emit stateChanged();
        return;
    }

    if (event == QStringLiteral("ready")
        && m_helperOperation == HelperOperation::Download) {
        m_helperProducedResult = true;
        m_updateReady = true;
        m_downloadProgress = 100;
        m_errorMessage.clear();
        emit stateChanged();
        return;
    }

    if (event == QStringLiteral("handed_off")
        && m_helperOperation == HelperOperation::Apply) {
        m_helperProducedResult = true;
        m_applying = false;
        emit stateChanged();
        QMetaObject::invokeMethod(
            QCoreApplication::instance(), &QCoreApplication::quit,
            Qt::QueuedConnection);
        return;
    }

    if (event == QStringLiteral("error")) {
        m_helperProducedResult = true;
        if (m_helperOperation == HelperOperation::Check)
            return;
        if (m_helperOperation == HelperOperation::Download) {
            m_errorMessage = tr(
                "Could not download the update. Please try again later.");
        } else if (m_helperOperation == HelperOperation::Apply) {
            m_errorMessage = tr(
                "Could not start the installer. Please try again.");
        }
        emit stateChanged();
    }
}

void UpdateChecker::finishHelper(int exitCode, bool crashed)
{
    UpdateHelperProcess *process = m_process.data();
    if (!process)
        return;
    // The transport drains stdout before its finished notification.
    if (!m_helperOutput.trimmed().isEmpty())
        handleHelperLine(m_helperOutput.trimmed());

    const HelperOperation operation = m_helperOperation;
    const bool producedResult = m_helperProducedResult;
    const bool cancelled = m_cancelled;
    m_process.clear();
    m_helperOperation = HelperOperation::None;
    m_helperOutput.clear();
    process->deleteLater();

    if (operation == HelperOperation::Check) {
        if (!producedResult || crashed || exitCode != 0) {
            // Portable builds and damaged updater installations retain the
            // read-only GitHub check instead of hiding update information.
            m_directUpdateSupported = false;
            startNetworkCheck();
        } else {
            m_checking = false;
            emit stateChanged();
        }
        return;
    }
    if (operation == HelperOperation::Download) {
        // A helper result can arrive just before QProcess::finished. Keep the
        // operation busy until this point so a fast follow-up click cannot be
        // lost while m_process still refers to the previous helper.
        m_downloading = false;
        if (cancelled) {
            m_downloadProgress = 0;
            m_updateReady = false;
            m_errorMessage.clear();
            emit stateChanged();
        } else if (!m_updateReady
            && (!producedResult || crashed || exitCode != 0)) {
            m_downloading = false;
            m_errorMessage = tr(
                "Could not download the update. Please try again later.");
            emit stateChanged();
        } else {
            emit stateChanged();
        }
        return;
    }
    if (operation == HelperOperation::Apply) {
        if (!producedResult || crashed || exitCode != 0) {
            m_applying = false;
            if (m_errorMessage.isEmpty()) {
                m_errorMessage = tr(
                    "Could not start the installer. Please try again.");
            }
            emit stateChanged();
        }
    }
}

void UpdateChecker::downloadUpdate()
{
    if (!m_directUpdateSupported || !m_updateAvailable
        || m_checking || m_downloading || m_applying || m_process
        || m_helperPath.isEmpty()) {
        return;
    }
    m_downloading = true;
    m_updateReady = false;
    m_downloadProgress = 0;
    m_errorMessage.clear();
    emit stateChanged();
    startHelper(HelperOperation::Download, {QStringLiteral("download")});
}

void UpdateChecker::cancelDownload()
{
    if (!m_downloading || !m_process
        || m_helperOperation != HelperOperation::Download) {
        return;
    }
    m_cancelled = true;
    m_process->cancel();
}

void UpdateChecker::applyUpdate()
{
    if (!m_updateReady || m_downloading || m_applying
        || m_checking || m_process || m_helperPath.isEmpty()) {
        return;
    }
    m_applying = true;
    m_errorMessage.clear();
    emit stateChanged();
    startHelper(HelperOperation::Apply, {
        QStringLiteral("apply"), QStringLiteral("--wait-pid"),
        QString::number(QCoreApplication::applicationPid()),
    });
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
    if (parseError.error != QJsonParseError::NoError
        || (!document.isArray() && !document.isObject())) {
        qCWarning(updateLog) << "update_check_failed" << "reason=invalid_json";
        finishWithError(tr("The update service returned an invalid response."));
        return;
    }

    const SemanticVersion current = parseVersion(m_currentVersion);
    if (!current.valid) {
        finishWithError(tr("The update service returned an invalid response."));
        return;
    }
    const bool includePrereleases = !current.prerelease.isEmpty();
    QJsonArray releases = document.isArray()
        ? document.array() : QJsonArray {document.object()};
    std::optional<ReleaseCandidate> selected;
    bool invalidRelevantRelease = false;
    for (const QJsonValue &value : releases) {
        if (!value.isObject()) {
            invalidRelevantRelease = true;
            continue;
        }
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("draft")).toBool())
            continue;
        if (!includePrereleases
            && object.value(QStringLiteral("prerelease")).toBool()) {
            continue;
        }
        ReleaseCandidate candidate;
        candidate.tag = object.value(QStringLiteral("tag_name")).toString();
        candidate.url = QUrl(
            object.value(QStringLiteral("html_url")).toString());
        candidate.version = parseVersion(candidate.tag);
        if (!candidate.version.valid || !isTrustedReleaseUrl(candidate.url)) {
            invalidRelevantRelease = true;
            continue;
        }
        if (!includePrereleases && !candidate.version.prerelease.isEmpty())
            continue;
        if (!selected
            || compareVersions(candidate.version, selected->version) > 0) {
            selected = std::move(candidate);
        }
    }

    if (!selected) {
        if (invalidRelevantRelease && !releases.isEmpty())
            finishWithError(tr("The update service returned an invalid response."));
        else
            finishWithoutRelease();
        return;
    }

    m_checking = false;
    m_hasChecked = true;
    m_releaseFound = true;
    m_latestVersion = selected->tag;
    m_releaseUrl = selected->url;
    m_errorMessage.clear();
    m_updateAvailable = compareVersions(selected->version, current) > 0;
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
