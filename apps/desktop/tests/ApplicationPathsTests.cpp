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
#include <qt_windows.h>
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
    const bool operatingSystemUpdated = ::SetEnvironmentVariableW(
        variableName.c_str(), nativeValue.c_str()) != FALSE;
    const bool runtimeUpdated =
        ::_wputenv_s(variableName.c_str(), nativeValue.c_str()) == 0;
    return operatingSystemUpdated && runtimeUpdated;
#else
    return qputenv(name, QFile::encodeName(value));
#endif
}

#ifdef Q_OS_WIN
QString nativeEnvironmentVariable(const wchar_t *name)
{
    const DWORD requiredSize = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (requiredSize == 0)
        return {};
    std::wstring value(requiredSize, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(
        name, value.data(), requiredSize);
    if (written == 0 || written >= requiredSize)
        return {};
    return QString::fromWCharArray(value.data(), static_cast<qsizetype>(written));
}
#endif

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
#ifdef Q_OS_WIN
        QCOMPARE(
            nativeEnvironmentVariable(L"YANAMI_ISOLATED_PROFILE_ROOT"),
            profileRoot);
#endif

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
        const QString isolatedTemporaryPath = temporary.filePath(
            QStringLiteral("cold-profile-全新/temp"));
#ifdef Q_OS_WIN
        QCOMPARE(qEnvironmentVariable("TEMP"), isolatedTemporaryPath);
        QCOMPARE(qEnvironmentVariable("TMP"), isolatedTemporaryPath);
        QCOMPARE(
            nativeEnvironmentVariable(L"TEMP"),
            isolatedTemporaryPath);
        QCOMPARE(nativeEnvironmentVariable(L"TMP"), isolatedTemporaryPath);
#else
        QCOMPARE(qEnvironmentVariable("TMPDIR"), isolatedTemporaryPath);
#endif
        QCOMPARE(QDir::tempPath(), isolatedTemporaryPath);

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
