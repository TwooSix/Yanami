#include "ApplicationPaths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <cstdlib>
#include <string>
#endif

namespace {

bool isWithin(const QString &root, const QString &candidate)
{
    const QString relative = QDir(root).relativeFilePath(candidate);
    return relative != QStringLiteral("..")
        && !relative.startsWith(QStringLiteral("../"))
        && !QFileInfo(relative).isAbsolute();
}

bool setEnvironmentVariable(const char *name, const QString &value)
{
#ifdef Q_OS_WIN
    const std::wstring variableName =
        QString::fromLatin1(name).toStdWString();
    const std::wstring nativeValue = value.toStdWString();
    return ::_wputenv_s(variableName.c_str(), nativeValue.c_str()) == 0;
#else
    return qputenv(name, QFile::encodeName(value));
#endif
}

} // namespace

class ApplicationPathsTests final : public QObject
{
    Q_OBJECT

private slots:
    void validatesProfileRoots()
    {
        QString resolved;
        QString error;
        QVERIFY(!ApplicationPaths::validateIsolatedProfileRoot(
            {}, &resolved, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!ApplicationPaths::validateIsolatedProfileRoot(
            QStringLiteral("relative/profile"), &resolved, &error));
        QVERIFY(!ApplicationPaths::validateIsolatedProfileRoot(
            QDir::rootPath(), &resolved, &error));

        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(ApplicationPaths::validateIsolatedProfileRoot(
            temporary.filePath(QStringLiteral("全新 profile")),
            &resolved,
            &error));
        QVERIFY(QFileInfo(resolved).isAbsolute());
        QVERIFY(error.isEmpty());
    }

    void isolatesEveryApplicationPathAndQSettingsScope()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString profileRoot = temporary.filePath(
            QStringLiteral("cold-profile-全新"));
        QVERIFY(setEnvironmentVariable(
            ApplicationPaths::isolatedProfileEnvironment,
            profileRoot));
        QCOMPARE(
            qEnvironmentVariable(
                ApplicationPaths::isolatedProfileEnvironment),
            profileRoot);

        QCoreApplication::setApplicationName(
            QStringLiteral("YanamiApplicationPathsTests"));
        QCoreApplication::setOrganizationName(
            QStringLiteral("YanamiTests"));
        const ApplicationPaths::ConfigurationResult configured =
            ApplicationPaths::configureFromEnvironment();
        QVERIFY2(configured.succeeded,
            qPrintable(configured.error));
        QVERIFY(configured.isolated);
        QVERIFY(ApplicationPaths::isolated());
        QCOMPARE(ApplicationPaths::profileRoot(), configured.profileRoot);
        QVERIFY(isWithin(configured.profileRoot, ApplicationPaths::dataRoot()));
        QVERIFY(isWithin(configured.profileRoot, ApplicationPaths::cacheRoot()));
        QVERIFY(isWithin(
            configured.profileRoot,
            ApplicationPaths::upscalingAssetRoot()));
        QVERIFY(isWithin(
            configured.profileRoot,
            ApplicationPaths::logFilePath()));
        QCOMPARE(
            qEnvironmentVariable("TEMP"),
            temporary.filePath(QStringLiteral("cold-profile-全新/temp")));

        QCOMPARE(QSettings::defaultFormat(), QSettings::IniFormat);
        QSettings settings;
        QVERIFY(isWithin(configured.profileRoot, settings.fileName()));
        settings.setValue(QStringLiteral("isolation/sentinel"), 17);
        settings.sync();
        QCOMPARE(settings.status(), QSettings::NoError);
        QVERIFY(QFileInfo::exists(settings.fileName()));
        QCOMPARE(
            settings.value(QStringLiteral("isolation/sentinel")).toInt(),
            17);
    }
};

QTEST_GUILESS_MAIN(ApplicationPathsTests)

#include "ApplicationPathsTests.moc"
