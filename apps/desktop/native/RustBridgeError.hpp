#pragma once

#include <QByteArray>
#include <QString>

struct YanamiBridgeErrorEnvelope
{
    QString code;
    QString message;
};

YanamiBridgeErrorEnvelope parseRustBridgeError(const QByteArray &encoded);
