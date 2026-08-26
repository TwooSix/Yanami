#include "CatalogCoordinator.hpp"

#include "LibraryCacheContract.hpp"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

namespace {

constexpr int desktopSchemaVersion = 8;
constexpr int libraryCacheSchemaVersion = 2;

void discardInvalidCache(const QString &path, const char *reason)
{
    qWarning().noquote()
        << "library_cache"
        << "outcome=discarded"
        << "reason=" << reason;
    if (!path.isEmpty() && QFile::exists(path) && !QFile::remove(path)) {
        qWarning().noquote()
            << "library_cache"
            << "outcome=remove_failed"
            << "path=" << path;
    }
}

QSet<QString> latestMediaScopes(const QJsonObject &object)
{
    QSet<QString> availableScopes;
    QSet<QString> sectionScopes;
    for (const QJsonValue &queryValue :
         object.value(QStringLiteral("queries")).toArray()) {
        const QJsonObject query = queryValue.toObject();
        const QString kind = query.value(QStringLiteral("kind"))
            .toString().trimmed().toLower();
        if (kind == QStringLiteral("latest")) {
            const QString scopeId = query.value(QStringLiteral("scopeId"))
                .toString();
            if (!scopeId.isEmpty())
                availableScopes.insert(scopeId);
            continue;
        }
        if (kind != QStringLiteral("latestsections")
            || !query.value(QStringLiteral("scopeId")).toString().isEmpty()) {
            continue;
        }
        for (const QJsonValue &rowValue :
             query.value(QStringLiteral("rows")).toArray()) {
            const QString scopeId = rowValue.toObject()
                .value(QStringLiteral("entityId")).toString();
            if (!scopeId.isEmpty())
                sectionScopes.insert(scopeId);
        }
    }
    return availableScopes & sectionScopes;
}

} // namespace

bool CatalogCoordinator::loadLibraryCache()
{
    if (!activeSession() || m_cachePath.isEmpty())
        return false;

    QFile cache(m_cachePath);
    if (!cache.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = cache.readAll();
    cache.close();

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        discardInvalidCache(m_cachePath, "invalid_json");
        return false;
    }

    const QJsonObject object = document.object();
    if (!YanamiCache::acceptsEnvelope(
            object,
            libraryCacheSchemaVersion,
            desktopSchemaVersion,
            m_cacheScope)) {
        discardInvalidCache(m_cachePath, "incompatible_envelope");
        return false;
    }
    if (!m_mediaStore->restoreCacheJson(object)) {
        m_mediaStore->clear();
        discardInvalidCache(m_cachePath, "invalid_store");
        return false;
    }
    m_latestMediaScopeIds = latestMediaScopes(object);

    m_lastFullLibraryRefreshMs =
        object.value(QStringLiteral("fullRefreshedAtMs"))
            .toVariant().toLongLong();
    m_lastFavoritesRefreshMs =
        object.value(QStringLiteral("favoritesRefreshedAtMs"))
            .toVariant().toLongLong();
    const QJsonObject capabilities =
        object.value(QStringLiteral("userCapabilities")).toObject();
    m_capabilities = {
        .administrator = capabilities.value(
            QStringLiteral("isAdministrator")).toBool(),
        .canDownload = capabilities.value(
            QStringLiteral("canDownload")).toBool(),
        .canDelete = capabilities.value(
            QStringLiteral("canDelete")).toBool(),
    };
    publishCapabilities();
    qInfo().noquote()
        << "library_cache"
        << "outcome=restored"
        << "scope=" << m_cacheScope;
    return true;
}

void CatalogCoordinator::saveLibraryCache() const
{
    // The committed store remains writable while a speculative session
    // transition is fenced so Media can persist rollback of optimistic rows.
    if (!m_initialized || m_shuttingDown || m_cachePath.isEmpty()
        || !m_committedSession.connected) {
        return;
    }

    QJsonObject object = m_mediaStore->toCacheJson(QSet<QString> {
        QStringLiteral("library"),
        QStringLiteral("views"),
        QStringLiteral("resume"),
        QStringLiteral("latestsections"),
        QStringLiteral("latest"),
        QStringLiteral("favorites"),
    });
    object.insert(QStringLiteral("userCapabilities"), QJsonObject {
        {QStringLiteral("isAdministrator"),
         m_capabilities.administrator},
        {QStringLiteral("canDownload"), m_capabilities.canDownload},
        {QStringLiteral("canDelete"), m_capabilities.canDelete},
    });
    object.insert(
        QStringLiteral("cacheSchemaVersion"),
        libraryCacheSchemaVersion);
    object.insert(
        QStringLiteral("bridgeSchemaVersion"), desktopSchemaVersion);
    object.insert(QStringLiteral("cacheScope"), m_cacheScope);
    object.insert(
        QStringLiteral("fullRefreshedAtMs"),
        m_lastFullLibraryRefreshMs);
    object.insert(
        QStringLiteral("favoritesRefreshedAtMs"),
        m_lastFavoritesRefreshMs);

    QSaveFile cache(m_cachePath);
    if (!cache.open(QIODevice::WriteOnly)) {
        qWarning().noquote()
            << "library_cache"
            << "outcome=open_failed"
            << "path=" << m_cachePath;
        return;
    }
    const QByteArray bytes =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (cache.write(bytes) != bytes.size()) {
        cache.cancelWriting();
        qWarning().noquote()
            << "library_cache"
            << "outcome=write_failed"
            << "path=" << m_cachePath;
        return;
    }
    if (!cache.commit()) {
        qWarning().noquote()
            << "library_cache"
            << "outcome=commit_failed"
            << "path=" << m_cachePath;
    }
}

void CatalogCoordinator::clearLibraryCache()
{
    if (m_cachePath.isEmpty() || !QFile::exists(m_cachePath))
        return;
    if (!QFile::remove(m_cachePath)) {
        qWarning().noquote()
            << "library_cache"
            << "outcome=remove_failed"
            << "path=" << m_cachePath;
    }
}

void CatalogCoordinator::updateLibraryCachePath()
{
    const QString serverIdentity =
        !m_committedSession.serverUrl.trimmed().isEmpty()
        ? m_committedSession.serverUrl.trimmed()
        : m_committedSession.serverDomain.trimmed();
    const QString userName = m_committedSession.userName.trimmed();
    if (m_cacheDirectory.isEmpty() || serverIdentity.isEmpty()
        || userName.isEmpty()) {
        m_cacheScope.clear();
        m_cachePath.clear();
        return;
    }

    QByteArray identity = serverIdentity.toUtf8();
    identity.append('\0');
    identity.append(userName.toUtf8());
    m_cacheScope = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
            .toHex().left(20));
    m_cachePath = QDir(m_cacheDirectory).filePath(
        QStringLiteral("home-library-%1.json").arg(m_cacheScope));
}
