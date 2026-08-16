#include "RustBridgeError.hpp"

#include <QJsonDocument>
#include <QJsonObject>

YanamiBridgeErrorEnvelope parseRustBridgeError(const QByteArray &encoded)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        return {
            QStringLiteral("bridge_protocol"),
            QStringLiteral(
                "The Rust backend returned an invalid error response."),
        };
    }
    const QJsonObject object = document.object();
    const QString code = object.value(QStringLiteral("code"))
        .toString().trimmed();
    const QString message = object.value(QStringLiteral("message"))
        .toString().trimmed();
    return {
        code.isEmpty() ? QStringLiteral("unknown") : code,
        message.isEmpty()
            ? QStringLiteral("The Rust backend reported an unknown error.")
            : message,
    };
}
