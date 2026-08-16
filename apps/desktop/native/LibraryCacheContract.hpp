#pragma once

#include <QJsonObject>
#include <QString>

namespace YanamiCache {

inline bool acceptsEnvelope(
    const QJsonObject &object,
    int cacheSchemaVersion,
    int bridgeSchemaVersion,
    const QString &scope)
{
    return object.value(QStringLiteral("cacheSchemaVersion")).toInt(-1)
            == cacheSchemaVersion
        && object.value(QStringLiteral("bridgeSchemaVersion")).toInt(-1)
            == bridgeSchemaVersion
        && !scope.isEmpty()
        && object.value(QStringLiteral("cacheScope")).toString() == scope
        && object.value(QStringLiteral("entities")).isObject()
        && object.value(QStringLiteral("queries")).isArray();
}

} // namespace YanamiCache
