#include "RuntimeLogger.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

constexpr qint64 expectedMaximumLogBytes = 10LL * 1024LL * 1024LL;
constexpr int expectedArchiveRetentionDays = 14;
constexpr int expectedMaximumArchiveCount = 30;

Q_LOGGING_CATEGORY(runtimeLoggerTestLog, "yanami.runtime.logger.tests")

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

bool replaceFile(
    const QString &path,
    const QByteArray &contents,
    const QDateTime &modified = {})
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (file.write(contents) != contents.size())
        return false;
    if (!file.flush())
        return false;
    if (modified.isValid()
        && !file.setFileTime(modified, QFileDevice::FileModificationTime)) {
        return false;
    }
    file.close();
    return true;
}

bool resizeFile(
    const QString &path,
    qint64 bytes,
    const QDateTime &modified = {})
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (!file.resize(bytes))
        return false;
    if (!file.flush())
        return false;
    if (modified.isValid()
        && !file.setFileTime(modified, QFileDevice::FileModificationTime)) {
        return false;
    }
    file.close();
    return true;
}

QStringList archivePaths(const QString &activePath)
{
    const QFileInfo activeFile(activePath);
    const QString suffix = activeFile.completeSuffix();
    const QString pattern = activeFile.completeBaseName()
        + QStringLiteral(".archive-*")
        + (suffix.isEmpty() ? QString() : u'.' + suffix);
    QDir directory(activeFile.absolutePath());
    const QStringList names = directory.entryList(
        {pattern},
        QDir::Files | QDir::NoDotAndDotDot,
        QDir::Name);
    QStringList paths;
    paths.reserve(names.size());
    for (const QString &name : names)
        paths.append(directory.absoluteFilePath(name));
    return paths;
}

QString archivePath(const QString &activePath, const QString &tag)
{
    const QFileInfo activeFile(activePath);
    const QString suffix = activeFile.completeSuffix();
    return QDir(activeFile.absolutePath()).absoluteFilePath(
        activeFile.completeBaseName()
        + QStringLiteral(".archive-")
        + tag
        + (suffix.isEmpty() ? QString() : u'.' + suffix));
}

} // namespace

class RuntimeLoggerTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCoreApplication::setApplicationName(QStringLiteral("YanamiRuntimeLoggerTests"));
        QCoreApplication::setOrganizationName(QStringLiteral("Yanami"));
        m_hadOriginalLogPath = qEnvironmentVariableIsSet("YANAMI_DEV_LOG_PATH");
        m_originalLogPath = qgetenv("YANAMI_DEV_LOG_PATH");
    }

    void init()
    {
        RuntimeLogger::shutdown();
        m_temporaryDirectory = std::make_unique<QTemporaryDir>();
        QVERIFY2(m_temporaryDirectory->isValid(), "Could not create a temporary log directory");
        const QByteArray requestedPath = QFile::encodeName(
            m_temporaryDirectory->filePath(QStringLiteral("yanami.log")));
        QVERIFY(qputenv("YANAMI_DEV_LOG_PATH", requestedPath));
    }

    void cleanup()
    {
        RuntimeLogger::shutdown();
        if (m_hadOriginalLogPath)
            qputenv("YANAMI_DEV_LOG_PATH", m_originalLogPath);
        else
            qunsetenv("YANAMI_DEV_LOG_PATH");
        m_temporaryDirectory.reset();
    }

    void defaultFormatIncludesContext()
    {
        QVERIFY(RuntimeLogger::install());
        const QString activePath = RuntimeLogger::currentLogPath();
        QVERIFY(QFileInfo(activePath).isAbsolute());
        QVERIFY(activePath.contains(
            QStringLiteral("-p%1").arg(QCoreApplication::applicationPid())));

        qCInfo(runtimeLoggerTestLog).noquote() << "runtime_logger_format_probe";
        RuntimeLogger::shutdown();

        const QString log = QString::fromUtf8(readFile(activePath));
        const qsizetype marker = log.indexOf(QStringLiteral("runtime_logger_format_probe"));
        QVERIFY2(marker >= 0, qPrintable(log));
        const qsizetype lineStart = marker > 0 ? log.lastIndexOf(u'\n', marker - 1) + 1 : 0;
        qsizetype lineEnd = log.indexOf(u'\n', marker);
        if (lineEnd < 0)
            lineEnd = log.size();
        const QString line = log.mid(lineStart, lineEnd - lineStart);

        const QRegularExpression timestamp(
            QStringLiteral(
                "^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}"
                "(?:Z|[+-]\\d{2}:\\d{2}) "));
        QVERIFY2(timestamp.match(line).hasMatch(), qPrintable(line));
        QVERIFY(line.contains(
            QStringLiteral(" pid=%1 ").arg(QCoreApplication::applicationPid())));
        QVERIFY(line.contains(QStringLiteral(" thread=0x")));
        QVERIFY(line.contains(QStringLiteral(" level=INFO")));
        QVERIFY(line.contains(
            QStringLiteral(" category=\"yanami.runtime.logger.tests\"")));
        QVERIFY(line.contains(
            QStringLiteral(" message=\"runtime_logger_format_probe\"")));
        QVERIFY2(
            QRegularExpression(QStringLiteral(
                "source=\\\"[^\\\"]*RuntimeLoggerTests\\.cpp:\\d+\\\""))
                .match(line)
                .hasMatch(),
            qPrintable(line));
        QVERIFY(line.contains(QStringLiteral(" function=\"")));
        QVERIFY(line.contains(QStringLiteral("defaultFormatIncludesContext")));
    }

    void sensitiveDataIsRedacted()
    {
        QVERIFY(RuntimeLogger::install());
        const QString activePath = RuntimeLogger::currentLogPath();
        qCWarning(runtimeLoggerTestLog).noquote()
            << "endpoint=https://user:password@example.com:8443/private/path?api_key=url-secret"
               " token=plain-secret Authorization=Bearer bearer-secret"
               " Bearer loose-secret";
        RuntimeLogger::shutdown();

        const QString log = QString::fromUtf8(readFile(activePath));
        QVERIFY(log.contains(QStringLiteral("https://example.com:8443/<redacted>")));
        QVERIFY(log.contains(QStringLiteral("token=<redacted>")));
        QVERIFY(log.contains(QStringLiteral("Authorization=<redacted>")));
        QVERIFY(log.contains(QStringLiteral("Bearer <redacted>")));
        QVERIFY(!log.contains(QStringLiteral("user:password")));
        QVERIFY(!log.contains(QStringLiteral("private/path")));
        QVERIFY(!log.contains(QStringLiteral("url-secret")));
        QVERIFY(!log.contains(QStringLiteral("plain-secret")));
        QVERIFY(!log.contains(QStringLiteral("bearer-secret")));
        QVERIFY(!log.contains(QStringLiteral("loose-secret")));
    }

    void oversizedActiveFileRotatesOnInstall()
    {
        QVERIFY(RuntimeLogger::install());
        const QString activePath = RuntimeLogger::currentLogPath();
        RuntimeLogger::shutdown();
        QVERIFY(resizeFile(activePath, expectedMaximumLogBytes));
        QCOMPARE(QFileInfo(activePath).size(), expectedMaximumLogBytes);

        QVERIFY(RuntimeLogger::install());
        qCInfo(runtimeLoggerTestLog) << "size_rotation_probe";
        RuntimeLogger::shutdown();

        const QStringList archives = archivePaths(activePath);
        QCOMPARE(archives.size(), 1);
        QCOMPARE(QFileInfo(archives.constFirst()).size(), expectedMaximumLogBytes);
        const QByteArray current = readFile(activePath);
        QVERIFY(current.contains("size_rotation_probe"));
        QVERIFY(QFileInfo(activePath).size() < expectedMaximumLogBytes);
    }

    void previousDateActiveFileRotatesOnInstall()
    {
        QVERIFY(RuntimeLogger::install());
        const QString activePath = RuntimeLogger::currentLogPath();
        RuntimeLogger::shutdown();
        const QDateTime previousDate = QDateTime::currentDateTime().addDays(-2);
        QVERIFY(replaceFile(activePath, QByteArrayLiteral("previous_date_payload\n"), previousDate));
        QCOMPARE(QFileInfo(activePath).lastModified().toLocalTime().date(), previousDate.date());

        QVERIFY(RuntimeLogger::install());
        qCInfo(runtimeLoggerTestLog) << "date_rotation_probe";
        RuntimeLogger::shutdown();

        const QStringList archives = archivePaths(activePath);
        QCOMPARE(archives.size(), 1);
        QVERIFY(readFile(archives.constFirst()).contains("previous_date_payload"));
        const QByteArray current = readFile(activePath);
        QVERIFY(current.contains("date_rotation_probe"));
        QVERIFY(!current.contains("previous_date_payload"));
    }

    void expiredAndExcessArchivesAreRemoved()
    {
        QVERIFY(RuntimeLogger::install());
        const QString activePath = RuntimeLogger::currentLogPath();
        RuntimeLogger::shutdown();

        const QString expired = archivePath(activePath, QStringLiteral("expired"));
        QVERIFY(replaceFile(
            expired,
            QByteArrayLiteral("expired\n"),
            QDateTime::currentDateTime().addDays(-expectedArchiveRetentionDays - 2)));

        QString newest;
        QString oldest;
        for (int index = 0; index < expectedMaximumArchiveCount + 5; ++index) {
            const QString path = archivePath(
                activePath,
                QStringLiteral("fresh-%1").arg(index, 3, 10, QLatin1Char('0')));
            QVERIFY(replaceFile(
                path,
                QByteArrayLiteral("fresh\n"),
                QDateTime::currentDateTime().addSecs(-index * 60)));
            if (index == 0)
                newest = path;
            if (index == expectedMaximumArchiveCount + 4)
                oldest = path;
        }
        QCOMPARE(archivePaths(activePath).size(), expectedMaximumArchiveCount + 6);

        QVERIFY(RuntimeLogger::install());
        RuntimeLogger::shutdown();

        const QStringList retained = archivePaths(activePath);
        QCOMPARE(retained.size(), expectedMaximumArchiveCount);
        QVERIFY(!QFileInfo::exists(expired));
        QVERIFY(QFileInfo::exists(newest));
        QVERIFY(!QFileInfo::exists(oldest));
    }

private:
    std::unique_ptr<QTemporaryDir> m_temporaryDirectory;
    QByteArray m_originalLogPath;
    bool m_hadOriginalLogPath = false;
};

QTEST_GUILESS_MAIN(RuntimeLoggerTests)

#include "RuntimeLoggerTests.moc"
