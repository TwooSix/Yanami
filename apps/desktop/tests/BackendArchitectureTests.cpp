#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTest>

namespace {

bool isNativeSourceRoot(const QString &path)
{
    const QDir directory(path);
    return directory.exists(QStringLiteral("main.cpp"))
        && directory.exists(QStringLiteral("BackendPorts.hpp"))
        && directory.exists(QStringLiteral("DesktopBackendServices.cpp"));
}

QString locateNativeSourceRoot()
{
#ifdef YANAMI_NATIVE_SOURCE_DIR
    const QString configured = QDir::cleanPath(
        QStringLiteral(YANAMI_NATIVE_SOURCE_DIR));
    if (isNativeSourceRoot(configured))
        return configured;
#endif

    // __FILE__ is absolute in CMake builds. The current-directory walk keeps
    // an ad-hoc test binary runnable from either the workspace or its build
    // tree without baking a machine-specific path into the test.
    const QDir testSourceDirectory(
        QFileInfo(QString::fromUtf8(__FILE__)).absolutePath());
    const QString adjacentNative = QDir::cleanPath(
        testSourceDirectory.filePath(QStringLiteral("../native")));
    if (isNativeSourceRoot(adjacentNative))
        return adjacentNative;

    QDir cursor = QDir::current();
    for (int depth = 0; depth < 10; ++depth) {
        const QString desktopNative = cursor.filePath(
            QStringLiteral("apps/desktop/native"));
        if (isNativeSourceRoot(desktopNative))
            return QDir::cleanPath(desktopNative);

        const QString localNative = cursor.filePath(
            QStringLiteral("native"));
        if (isNativeSourceRoot(localNative))
            return QDir::cleanPath(localNative);
        if (!cursor.cdUp())
            break;
    }
    return {};
}

QString source(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QStringList productionNativeFiles(const QString &nativeRoot)
{
    QStringList files;
    QDirIterator iterator(
        nativeRoot,
        {
            QStringLiteral("*.cpp"),
            QStringLiteral("*.hpp"),
            QStringLiteral("*.h"),
        },
        QDir::Files,
        QDirIterator::Subdirectories);
    while (iterator.hasNext())
        files.push_back(iterator.next());
    files.sort();
    return files;
}

QString diagnosticPath(const QString &nativeRoot, const QString &path)
{
    return QDir(nativeRoot).relativeFilePath(path);
}

} // namespace

class BackendArchitectureTests final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        m_nativeRoot = locateNativeSourceRoot();
        QVERIFY2(!m_nativeRoot.isEmpty(),
            "Could not locate apps/desktop/native");
    }

    void productionNativeHasNoLegacyControllerSurface()
    {
        const QStringList literalMarkers {
            QStringLiteral("BackendController"),
            QStringLiteral("BackendControllerState"),
            QStringLiteral("YanamiItemAction"),
            QStringLiteral("performItemAction"),
            QStringLiteral("optimistic_item_action"),
        };
        const QRegularExpression memberAliasMacro(
            QStringLiteral(R"(#\s*define\s+m_[A-Za-z0-9_]*)"));

        const QStringList files = productionNativeFiles(m_nativeRoot);
        QVERIFY2(!files.isEmpty(), "No production native sources found");
        for (const QString &path : files) {
            const QString relative = diagnosticPath(m_nativeRoot, path);
            QVERIFY2(!QFileInfo(path).fileName().contains(
                         QStringLiteral("BackendController")),
                qPrintable(relative
                    + QStringLiteral(" retains a legacy controller filename")));
            const QString content = source(path);
            QVERIFY2(!content.isEmpty(), qPrintable(relative));
            for (const QString &marker : literalMarkers) {
                QVERIFY2(!content.contains(marker),
                    qPrintable(relative
                        + QStringLiteral(" contains forbidden marker: ")
                        + marker));
            }
            QVERIFY2(!memberAliasMacro.match(content).hasMatch(),
                qPrintable(relative
                    + QStringLiteral(" defines a legacy m_ alias macro")));
        }
    }

    void backendPortsStayRuntimeAgnostic()
    {
        const QString path = QDir(m_nativeRoot).filePath(
            QStringLiteral("BackendPorts.hpp"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));
        const QStringList forbiddenTypes {
            QStringLiteral("DesktopBackendServices"),
            QStringLiteral("RustBridgeRuntime"),
            QStringLiteral("QThreadPool"),
        };
        for (const QString &type : forbiddenTypes) {
            const QRegularExpression token(
                QStringLiteral(R"(\b%1\b)")
                    .arg(QRegularExpression::escape(type)));
            QVERIFY2(!token.match(content).hasMatch(),
                qPrintable(QStringLiteral(
                    "BackendPorts.hpp leaks concrete type: %1").arg(type)));
        }
    }

    void mainUsesOnlyTheBackendCompositionRoot()
    {
        const QString path = QDir(m_nativeRoot).filePath(
            QStringLiteral("main.cpp"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));

        const QRegularExpression quotedInclude(
            QStringLiteral("^\\s*#\\s*include\\s*\"([^\"]+)\""),
            QRegularExpression::MultilineOption);
        QStringList includedHeaders;
        auto match = quotedInclude.globalMatch(content);
        while (match.hasNext())
            includedHeaders.push_back(match.next().captured(1));

        QCOMPARE(
            includedHeaders.count(
                QStringLiteral("DesktopBackendServices.hpp")),
            1);
        for (const QString &header : includedHeaders) {
            if (header == QStringLiteral("DesktopBackendServices.hpp"))
                continue;
            const bool concreteBackendHeader =
                header.contains(QStringLiteral("Backend"))
                || header.endsWith(QStringLiteral("Coordinator.hpp"))
                || header == QStringLiteral("RustBridgeRuntime.hpp")
                || header == QStringLiteral("PlaybackReporter.hpp")
                || header == QStringLiteral("RequestCoordinator.hpp");
            QVERIFY2(!concreteBackendHeader,
                qPrintable(QStringLiteral(
                    "main.cpp bypasses DesktopBackendServices via %1")
                    .arg(header)));
        }
    }

    void featureCoordinatorsDirectlyImplementTheirPorts()
    {
        struct ExpectedBase {
            const char *coordinator;
            const char *port;
        };
        static constexpr ExpectedBase expected[] {
            {"SessionCoordinator", "SessionPort"},
            {"CatalogCoordinator", "CatalogPort"},
            {"SearchCoordinator", "SearchPort"},
            {"MediaCoordinator", "MediaPort"},
            {"PlaybackCoordinator", "PlaybackPort"},
            {"DanmakuCoordinator", "DanmakuPort"},
        };

        for (const ExpectedBase &entry : expected) {
            const QString coordinator = QString::fromLatin1(
                entry.coordinator);
            const QString port = QString::fromLatin1(entry.port);
            const QString fileName = coordinator
                + QStringLiteral(".hpp");
            const QString path = QDir(m_nativeRoot).filePath(fileName);
            const QString content = source(path);
            QVERIFY2(!content.isEmpty(), qPrintable(fileName));
            const QRegularExpression directInheritance(
                QStringLiteral(
                    R"(class\s+%1\s+final\s*:\s*public\s+%2\b)")
                    .arg(QRegularExpression::escape(coordinator),
                         QRegularExpression::escape(port)));
            QVERIFY2(directInheritance.match(content).hasMatch(),
                qPrintable(QStringLiteral(
                    "%1 must directly implement %2")
                    .arg(coordinator, port)));
        }
    }

    void mediaCoordinatorDoesNotOwnDanmaku()
    {
        const QRegularExpression danmaku(
            QStringLiteral("danmaku"),
            QRegularExpression::CaseInsensitiveOption);
        const QStringList files {
            QStringLiteral("MediaCoordinator.hpp"),
            QStringLiteral("MediaCoordinator.cpp"),
        };
        for (const QString &fileName : files) {
            const QString content = source(
                QDir(m_nativeRoot).filePath(fileName));
            QVERIFY2(!content.isEmpty(), qPrintable(fileName));
            QVERIFY2(!danmaku.match(content).hasMatch(),
                qPrintable(fileName
                    + QStringLiteral(" must not depend on Danmaku")));
        }
    }

    void desktopBackendServicesIsACompositionRootNotAnAdapter()
    {
        const QString path = QDir(m_nativeRoot).filePath(
            QStringLiteral("DesktopBackendServices.cpp"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));
        QVERIFY2(!content.contains(QStringLiteral("BackendController")),
            "DesktopBackendServices must not wrap BackendController");
        const QRegularExpression adapter(
            QStringLiteral(R"(\badapter\b)"),
            QRegularExpression::CaseInsensitiveOption);
        QVERIFY2(!adapter.match(content).hasMatch(),
            "DesktopBackendServices must remain a composition root, not an adapter");
    }

    void catalogCoordinatorDoesNotMaterializeQueriesForMetadata()
    {
        const QStringList files {
            QStringLiteral("CatalogCoordinator.cpp"),
            QStringLiteral("CatalogCoordinatorNavigation.cpp"),
            QStringLiteral("CatalogCoordinatorStore.cpp"),
            QStringLiteral("CatalogCoordinatorCache.cpp"),
        };
        const QRegularExpression queryMaterialization(
            QStringLiteral(R"(m_mediaStore\s*->\s*queryItems\s*\()"));

        for (const QString &fileName : files) {
            const QString content = source(
                QDir(m_nativeRoot).filePath(fileName));
            QVERIFY2(!content.isEmpty(), qPrintable(fileName));
            QVERIFY2(!queryMaterialization.match(content).hasMatch(),
                qPrintable(fileName
                    + QStringLiteral(
                        " materializes full query rows for count/existence metadata; "
                        "use the query model's O(1) rowCount instead")));
        }
    }

    void searchPosterHydrationIsDelayedGenerationFencedAndIsolated()
    {
        const QString coordinator = source(
            QDir(m_nativeRoot).filePath(
                QStringLiteral("SearchCoordinator.cpp")));
        const QString infrastructure = source(
            QDir(m_nativeRoot).filePath(
                QStringLiteral("BackendInfrastructure.hpp")));
        const QString composition = source(
            QDir(m_nativeRoot).filePath(
                QStringLiteral("DesktopBackendServices.cpp")));
        QVERIFY(!coordinator.isEmpty());
        QVERIFY(!infrastructure.isEmpty());
        QVERIFY(!composition.isEmpty());

        QVERIFY(coordinator.contains(
            QStringLiteral("constexpr int hydrationDelayMs = 150")));
        QVERIFY(coordinator.contains(
            QStringLiteral("m_hydrationDelay.setSingleShot(true)")));
        QVERIFY(coordinator.contains(
            QStringLiteral("cancelPendingHydration();")));
        QVERIFY(coordinator.contains(
            QStringLiteral("completed->identity.query == m_publishedQuery")));
        QVERIFY(coordinator.contains(
            QStringLiteral("&m_hydrationPool")));
        QVERIFY(coordinator.contains(
            QStringLiteral("m_operations.hydrateCatalogSearchImages")));
        QVERIFY(coordinator.contains(
            QStringLiteral("return hydrationOperation(payload);")));

        QVERIFY(infrastructure.contains(
            QStringLiteral("QThreadPool m_search;")));
        QVERIFY(infrastructure.contains(
            QStringLiteral("QThreadPool m_searchHydration;")));
        QVERIFY(composition.contains(
            QStringLiteral("pools.search(),")));
        QVERIFY(composition.contains(
            QStringLiteral("pools.searchHydration(),")));
    }

    void controllerRuntimeInitializationStaysAfterFirstShellFrame()
    {
        const QString inputModality = source(
            QDir(m_nativeRoot).filePath(
                QStringLiteral("InputModalityController.cpp")));
        const QString main = source(
            QDir(m_nativeRoot).filePath(QStringLiteral("main.cpp")));
        QVERIFY(!inputModality.isEmpty());
        QVERIFY(!main.isEmpty());

        const qsizetype constructorStart = inputModality.indexOf(
            QStringLiteral("InputModalityService::InputModalityService("));
        const qsizetype initializerStart = inputModality.indexOf(
            QStringLiteral(
                "void InputModalityService::initializeControllerNavigation()"));
        const qsizetype destructorStart = inputModality.indexOf(
            QStringLiteral("InputModalityService::~InputModalityService()"));
        QVERIFY(constructorStart >= 0);
        QVERIFY(initializerStart > constructorStart);
        QVERIFY(destructorStart > initializerStart);

        const QString constructor = inputModality.mid(
            constructorStart, initializerStart - constructorStart);
        const QString initializer = inputModality.mid(
            initializerStart, destructorStart - initializerStart);
        QVERIFY2(!constructor.contains(
                     QStringLiteral("ControllerNavigationSource")),
                 "InputModalityService construction must not create the SDL/XInput source");
        QVERIFY(initializer.contains(QStringLiteral(
            "std::make_unique<ControllerNavigationSource>()")));

        const qsizetype firstShellMilestone = main.indexOf(
            QStringLiteral("first_shell_present"));
        const qsizetype controllerBegin = main.indexOf(
            QStringLiteral("controller_navigation_init_begin"));
        const qsizetype frameHook = main.lastIndexOf(
            QStringLiteral("&QQuickWindow::frameSwapped"), controllerBegin);
        const qsizetype queuedInitialization = main.indexOf(
            QStringLiteral("QTimer::singleShot(0"), frameHook);
        const qsizetype initializeCall = main.indexOf(
            QStringLiteral("initializeControllerNavigation()"),
            queuedInitialization);
        const qsizetype controllerEnd = main.indexOf(
            QStringLiteral("controller_navigation_init_end"), initializeCall);
        const qsizetype singleShotConnection = main.indexOf(
            QStringLiteral("Qt::SingleShotConnection"), frameHook);
        QVERIFY(firstShellMilestone >= 0);
        QVERIFY(controllerBegin > firstShellMilestone);
        QVERIFY(frameHook > firstShellMilestone);
        QVERIFY(queuedInitialization > frameHook);
        QVERIFY(controllerBegin > queuedInitialization);
        QVERIFY(initializeCall > controllerBegin);
        QVERIFY(controllerEnd > initializeCall);
        QVERIFY(singleShotConnection > controllerEnd);
        QCOMPARE(main.count(QStringLiteral(
                     "initializeControllerNavigation()")), 1);
        QCOMPARE(main.count(QStringLiteral(
                     "controller_navigation_init_begin")), 1);
        QCOMPARE(main.count(QStringLiteral(
                     "controller_navigation_init_end")), 1);
    }

    void bootstrapHandoffUsesPortableAtomicReadyFileAfterVisibleTransitionFrame()
    {
        const QString main = source(
            QDir(m_nativeRoot).filePath(QStringLiteral("main.cpp")));
        QVERIFY(!main.isEmpty());

        QVERIFY(main.contains(QStringLiteral(
            "--yanami-bootstrap-ready-file")));
        QVERIFY(main.contains(QStringLiteral(
            "QDir::isAbsolutePath(candidate)")));
        QVERIFY(main.contains(QStringLiteral(
            "BootstrapReadyFileName = \"desktop-ready.json\"")));
        QVERIFY(main.contains(QStringLiteral(
            "QStringLiteral(\"YanamiBootstrap-\")")));
        QVERIFY(main.contains(QStringLiteral(
            "QFileInfo(QDir::tempPath()).canonicalFilePath()")));
        QVERIFY(main.contains(QStringLiteral("QSaveFile readyFile(path)")));
        QVERIFY(main.contains(QStringLiteral(
            "readyFile.setDirectWriteFallback(false)")));
        QVERIFY(main.contains(QStringLiteral(
            "QStringLiteral(\"schemaVersion\"), QStringLiteral(\"1.0\")")));
        QVERIFY(main.contains(QStringLiteral(
            "QStringLiteral(\"state\"), QStringLiteral(\"desktop_ready\")")));
        QVERIFY(main.contains(QStringLiteral(
            "QCoreApplication::applicationPid()")));

        const QStringList forbiddenPlatformApis {
            QStringLiteral("windows.h"),
            QStringLiteral("CreateEvent"),
            QStringLiteral("OpenEvent"),
            QStringLiteral("SetForegroundWindow"),
        };
        for (const QString &api : forbiddenPlatformApis) {
            QVERIFY2(!main.contains(api), qPrintable(
                QStringLiteral("main.cpp must keep bootstrap IPC portable: %1")
                    .arg(api)));
        }

        const qsizetype contextProperty = main.indexOf(QStringLiteral(
            "QStringLiteral(\"bootstrapHandoffRequested\")"));
        const qsizetype qmlLoad = main.indexOf(QStringLiteral(
            "engine.loadFromModule(\"Yanami\", \"Main\")"));
        QVERIFY(contextProperty >= 0 && qmlLoad > contextProperty);
        QVERIFY(main.contains(QStringLiteral(
            "bootstrapHandoff.usable() ? 2 : 1")));
        QVERIFY(main.contains(QStringLiteral(
            "startupFrames->swappedFrameCount")));
        QVERIFY(main.contains(QStringLiteral(
            ">= startupFrames->firstShellFrame")));

        const qsizetype handoffState = main.indexOf(QStringLiteral(
            "struct BootstrapHandoffFrameState"));
        const qsizetype transparentGateCleared = main.indexOf(QStringLiteral(
            "\"bootstrapHandoffPending\", false"), handoffState);
        const qsizetype secondFrameRequested = main.indexOf(QStringLiteral(
            "quickWindow->requestUpdate()"), transparentGateCleared);
        const qsizetype secondFrameGuard = main.indexOf(QStringLiteral(
            "handoffFrames->swappedFrames < 2"), secondFrameRequested);
        const qsizetype desktopReady = main.indexOf(QStringLiteral(
            "QStringLiteral(\"desktop_ready\")"), secondFrameGuard);
        const qsizetype readyCommit = main.indexOf(QStringLiteral(
            "publishBootstrapReadyFile("), desktopReady);
        const qsizetype qmlRelease = main.indexOf(QStringLiteral(
            "root->setProperty(\"bootstrapHandoffReady\", true)"),
            readyCommit);
        const qsizetype commitMilestone = main.indexOf(QStringLiteral(
            "QStringLiteral(\"desktop_ready_file_committed\")"),
            qmlRelease);
        QVERIFY(handoffState >= 0);
        QVERIFY(transparentGateCleared > handoffState);
        QVERIFY(secondFrameRequested > transparentGateCleared);
        QVERIFY(secondFrameGuard > secondFrameRequested);
        QVERIFY(desktopReady > secondFrameGuard);
        QVERIFY(readyCommit > desktopReady);
        QVERIFY(qmlRelease > readyCommit);
        QVERIFY(commitMilestone > qmlRelease);
        QVERIFY2(!main.contains(QStringLiteral(
                     "QStringLiteral(\"handoff_complete\")")),
            "Only the launcher may claim handoff_complete after hiding its splash");
    }

private:
    QString m_nativeRoot;
};

QTEST_APPLESS_MAIN(BackendArchitectureTests)
#include "BackendArchitectureTests.moc"
