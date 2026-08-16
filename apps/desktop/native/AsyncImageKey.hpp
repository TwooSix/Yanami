#pragma once

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringConverter>

namespace YanamiAsyncImageKey {
inline Qt::CaseSensitivity pathCaseSensitivity()
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

inline QString normalizedCacheRoot(const QString &path)
{
    if (path.trimmed().isEmpty())
        return {};

    const QFileInfo info(QDir::cleanPath(path));
    QString normalized = info.canonicalFilePath();
    if (normalized.isEmpty())
        normalized = info.absoluteFilePath();
    return QDir::fromNativeSeparators(QDir::cleanPath(normalized));
}

inline bool isStrictDescendant(const QString &path, const QString &root)
{
    if (path.isEmpty() || root.isEmpty())
        return false;
    const QString rootPrefix = root.endsWith(QLatin1Char('/'))
        ? root
        : root + QLatin1Char('/');
    return path.startsWith(rootPrefix, pathCaseSensitivity());
}

inline QString decode(const QString &id, const QString &cacheRoot)
{
    // Version the identifier so a future key representation cannot
    // accidentally be interpreted as an absolute path. Only a cache-relative
    // UTF-8 path is accepted; the Rust bridge emits this exact form.
    constexpr QLatin1StringView keyPrefix("v1-");
    if (!id.startsWith(keyPrefix))
        return {};

    const QByteArray encoded = id.sliced(keyPrefix.size()).toLatin1();
    static const QRegularExpression hexadecimal(QStringLiteral("^[0-9a-fA-F]+$"));
    if (encoded.isEmpty() || encoded.size() % 2 != 0
        || !hexadecimal.match(QString::fromLatin1(encoded)).hasMatch()) {
        return {};
    }

    const QByteArray bytes = QByteArray::fromHex(encoded);
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString relativePath = decoder.decode(bytes);
    if (decoder.hasError() || relativePath.isEmpty()
        || relativePath.contains(QChar::Null)) {
        return {};
    }

    relativePath = QDir::fromNativeSeparators(relativePath);
    if (QDir::isAbsolutePath(relativePath))
        return {};

    const QStringList components = relativePath.split(
        QLatin1Char('/'), Qt::KeepEmptyParts);
    if (components.isEmpty())
        return {};
    for (const QString &component : components) {
        // Reject rather than clean traversal and ambiguous path spellings.
        // Colons also exclude Windows drive-relative paths and NTFS streams.
        if (component.isEmpty() || component == QLatin1String(".")
            || component == QLatin1String("..")
            || component.contains(QLatin1Char(':'))) {
            return {};
        }
    }

    const QString candidate = QDir::fromNativeSeparators(QDir::cleanPath(
        QDir(cacheRoot).absoluteFilePath(relativePath)));
    return isStrictDescendant(candidate, cacheRoot) ? candidate : QString{};
}
}
