#include "RustBridgeError.hpp"

#include <QtTest>

class RustBridgeErrorTests final : public QObject
{
    Q_OBJECT

private slots:
    void preservesStableCodeAndMessage()
    {
        const YanamiBridgeErrorEnvelope error = parseRustBridgeError(
            R"({"code":"storage","message":"database unavailable"})");
        QCOMPARE(error.code, QStringLiteral("storage"));
        QCOMPARE(error.message, QStringLiteral("database unavailable"));
    }

    void invalidEnvelopeGetsProtocolCode()
    {
        const YanamiBridgeErrorEnvelope malformed =
            parseRustBridgeError("not-json");
        QCOMPARE(malformed.code, QStringLiteral("bridge_protocol"));
        QVERIFY(!malformed.message.isEmpty());

        const YanamiBridgeErrorEnvelope missingFields =
            parseRustBridgeError("{}");
        QCOMPARE(missingFields.code, QStringLiteral("unknown"));
        QVERIFY(!missingFields.message.isEmpty());
    }
};

QTEST_MAIN(RustBridgeErrorTests)
#include "RustBridgeErrorTests.moc"
