#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include "../native/AsyncImageKey.hpp"

class QmlArchitectureTests final : public QObject
{
    Q_OBJECT

private:
    static QStringList qmlFiles()
    {
        QStringList files;
        QDirIterator iterator(
            QStringLiteral(YANAMI_QML_SOURCE_DIR),
            {QStringLiteral("*.qml")},
            QDir::Files,
            QDirIterator::Subdirectories);
        while (iterator.hasNext())
            files.push_back(iterator.next());
        return files;
    }

    static QString source(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {};
        return QString::fromUtf8(file.readAll());
    }

    static QString withoutLineComment(QString line)
    {
        const qsizetype comment = line.indexOf(QStringLiteral("//"));
        if (comment >= 0)
            line.truncate(comment);
        return line;
    }

private slots:
    void customPopupsUseTheSharedBase()
    {
        const QRegularExpression rawPopup(QStringLiteral(R"(\bPopup\s*\{)"));
        const QRegularExpression rawMenu(QStringLiteral(R"(\bMenu\s*\{)"));
        for (const QString &path : qmlFiles()) {
            if (path.endsWith(QStringLiteral("AppPopup.qml"))
                || path.endsWith(QStringLiteral("AppMenu.qml"))) {
                continue;
            }
            const QString content = source(path);
            QVERIFY2(!content.isEmpty(), qPrintable(path));
            const QStringList lines = content.split(QLatin1Char('\n'));
            for (int index = 0; index < lines.size(); ++index) {
                const QString code = withoutLineComment(lines.at(index));
                QVERIFY2(!rawPopup.match(code).hasMatch(),
                    qPrintable(QStringLiteral("%1:%2 contains a raw Popup")
                        .arg(path).arg(index + 1)));
                QVERIFY2(!rawMenu.match(code).hasMatch(),
                    qPrintable(QStringLiteral("%1:%2 contains a raw Menu")
                        .arg(path).arg(index + 1)));
            }
        }
    }

    void popupRootsDoNotOverridePolicyOrLayer()
    {
        const QRegularExpression rootDeclaration(
            QStringLiteral(R"(^\s*([A-Za-z_][A-Za-z0-9_.]*)\s*\{)"));
        const QRegularExpression forbiddenRootProperty(
            QStringLiteral(R"(^\s*(?:closePolicy|z)\s*:)"));

        for (const QString &path : qmlFiles()) {
            if (path.endsWith(QStringLiteral("AppPopup.qml"))
                || path.endsWith(QStringLiteral("AppMenu.qml"))) {
                continue;
            }
            const QStringList lines = source(path).split(QLatin1Char('\n'));
            bool popupRoot = false;
            bool rootFound = false;
            int depth = 0;
            for (int index = 0; index < lines.size(); ++index) {
                const QString code = withoutLineComment(lines.at(index));
                if (!rootFound) {
                    const auto match = rootDeclaration.match(code);
                    if (!match.hasMatch())
                        continue;
                    rootFound = true;
                    const QString rootType = match.captured(1);
                    popupRoot = rootType.endsWith(QStringLiteral("Popup"))
                        || rootType.endsWith(QStringLiteral("Menu"));
                } else if (popupRoot && depth == 1) {
                    QVERIFY2(!forbiddenRootProperty.match(code).hasMatch(),
                        qPrintable(QStringLiteral("%1:%2 overrides shared popup policy")
                            .arg(path).arg(index + 1)));
                }
                depth += code.count(QLatin1Char('{'));
                depth -= code.count(QLatin1Char('}'));
            }
        }
    }

    void roundedImagesDoNotPollTheFileSystem()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("components/RoundedImage.qml"));
        const QString content = source(path);
        QVERIFY2(!content.contains(QRegularExpression(QStringLiteral(R"(\bTimer\s*\{)"))),
            "RoundedImage must use the async image provider");
        QVERIFY2(!content.contains(QStringLiteral("localFileExists")),
            "RoundedImage must not poll backend.localFileExists");
    }

    void qmlUsesOnlyTheApplicationFacade()
    {
        const QRegularExpression rawBackend(
            QStringLiteral(R"(\bbackend\b)"));
        for (const QString &path : qmlFiles()) {
            const QStringList lines = source(path).split(QLatin1Char('\n'));
            for (int index = 0; index < lines.size(); ++index) {
                const QString code = withoutLineComment(lines.at(index));
                QVERIFY2(!rawBackend.match(code).hasMatch(),
                    qPrintable(QStringLiteral(
                        "%1:%2 accesses the raw backend instead of app")
                        .arg(path).arg(index + 1)));
            }
        }
    }

    void searchPageKeepsPartitionedResultsNavigable()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("pages/SearchPage.qml"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));

        QVERIFY2(content.contains(QStringLiteral(
                     "model: app.search.resultRows"))
                && content.contains(QStringLiteral(
                    "mediaSection === \"titles\""))
                && content.contains(QStringLiteral(
                    "mediaSection === \"episodes\"")),
            "Search must render title and episode visual rows in one virtualized list");
        QVERIFY2(content.contains(QStringLiteral(
                     "app.search.results.count")),
            "The aggregate search model must remain the total/empty-state authority");
        QVERIFY2(content.contains(QStringLiteral("moveResultFocus"))
                && content.contains(QStringLiteral("Keys.onDownPressed"))
                && content.contains(QStringLiteral("revealResultCard")),
            "Both result sections must retain directional focus and scroll reveal");
        QVERIFY2(content.contains(QStringLiteral(
                     "resultsList.resetScrollPosition()"))
                && content.contains(QStringLiteral(
                    "root.focusedResultId = \"\"")),
            "A new query must reset scroll and stale semantic focus together");
        QVERIFY2(content.contains(QStringLiteral(
                     "running: root.visible && root.effectiveQuery.length > 0"))
                && content.contains(QStringLiteral(
                     "&& root.hostWindowActive && !app.search.searching"))
                && content.contains(QStringLiteral(
                    "&& !searchSubmission.pending"))
                && content.contains(QStringLiteral(
                    "&& !searchField.inputMethodComposing"))
                && !content.contains(QStringLiteral(
                    "running: root.visible && app.search.syncing")),
            "Catalog revision polling must pause during input and IME composition");
        QVERIFY2(content.contains(QStringLiteral(
                     "onVisibleChanged:"))
                && content.contains(QStringLiteral(
                    "function onActiveChanged()"))
                && content.contains(QStringLiteral(
                    "root.refreshCatalogRevision()")),
            "Returning to Search or reactivating its window must request one immediate revision check");
        QCOMPARE(content.count(QStringLiteral("add: Transition")), 0);
        QVERIFY2(content.contains(QStringLiteral(
                     "visible: app.search.results.count > 0"))
                && content.contains(QStringLiteral(
                    "visible: app.search.results.count === 0"))
                && !content.contains(QStringLiteral(
                    "opacity: app.search.results.count")),
            "Search result and empty-state containers must switch deterministically without opacity crossfades");
        QVERIFY2(!content.contains(QStringLiteral(
                     "Layout.preferredHeight: Math.ceil"))
                && !content.contains(QStringLiteral("GridView {"))
                && content.contains(QStringLiteral("SmoothListView {"))
                && content.contains(QStringLiteral("cacheBuffer: cardRowHeight")),
            "Search result cards must be row-virtualized instead of expanding nested grids");
        QVERIFY2(content.contains(QStringLiteral("SearchSubmissionPolicy {"))
                && content.contains(QStringLiteral("delayMs: 100"))
                && content.contains(QStringLiteral("app.search.inputPending()"))
                && content.contains(QStringLiteral(
                    "searchSubmission.pending || app.search.searching")),
            "Search input must use the production trailing policy, fence old work, and expose pending state");
        QVERIFY2(content.contains(QStringLiteral(
                     "function onRowsAboutToBeRebuilt()"))
                && content.contains(QStringLiteral(
                    "root.restoreScrollAnchor()"))
                && content.contains(QStringLiteral(
                    "InputModality.focusNavigationActive")),
            "Incremental results must preserve pointer scroll anchors without stealing keyboard focus");
    }

    void framelessMainWindowRetainsPlatformWindowCapabilities()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("Main.qml"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));

        const QRegularExpression flagsDeclaration(
            QStringLiteral(
                R"(\bflags\s*:\s*Qt\.Window(?:\s*\|\s*Qt\.[A-Za-z0-9_]+)+)"));
        const QRegularExpressionMatch match = flagsDeclaration.match(content);
        QVERIFY2(match.hasMatch(),
            "Main window flags must be one cross-platform capability declaration");

        const QString flags = match.captured(0);
        const QStringList requiredFlags {
            QStringLiteral("Qt.FramelessWindowHint"),
            QStringLiteral("Qt.WindowSystemMenuHint"),
            QStringLiteral("Qt.WindowMinimizeButtonHint"),
            QStringLiteral("Qt.WindowMaximizeButtonHint"),
            QStringLiteral("Qt.WindowCloseButtonHint"),
        };
        for (const QString &requiredFlag : requiredFlags) {
            QVERIFY2(flags.contains(requiredFlag),
                qPrintable(QStringLiteral(
                    "Frameless main window must retain platform capability %1")
                    .arg(requiredFlag)));
        }
    }

    void playerPageFencesLateMpvEventsAfterClose()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("pages/PlayerPage.qml"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));

        const QString closedLoadGuard = QStringLiteral(
            "root.mediaUrl.toString().length === 0\n"
            "                    || root.currentItemId.length === 0");
        QVERIFY2(content.count(closedLoadGuard) >= 2,
            "Both FILE_LOADED and playbackError must reject closed loads");
        QVERIFY2(content.contains(QStringLiteral(
            "mediaUrl.toString().length === 0 || currentItemId.length === 0")),
            "Natural completion must reject a closed load");
    }

    void playbackStallFeedbackRemainsRecoverableAndInteractive()
    {
        const QDir qmlRoot(QStringLiteral(YANAMI_QML_SOURCE_DIR));
        const QString player = source(
            qmlRoot.filePath(QStringLiteral("pages/PlayerPage.qml")));
        const QString loadingOverlay = source(
            qmlRoot.filePath(QStringLiteral("components/LoadingOverlay.qml")));
        const QString statusToast = source(
            qmlRoot.filePath(QStringLiteral("components/StatusToast.qml")));
        const QString mainWindow = source(
            qmlRoot.filePath(QStringLiteral("Main.qml")));

        QVERIFY2(player.contains(QStringLiteral("onPlaybackTimedOut:"))
                && player.contains(QStringLiteral("onPlaybackRecovered:")),
            "PlayerPage must present and clear recoverable media-read timeouts");
        const qsizetype timeoutStart = player.indexOf(
            QStringLiteral("onPlaybackTimedOut:"));
        const qsizetype recoveryStart = player.indexOf(
            QStringLiteral("onPlaybackRecovered:"), timeoutStart);
        QVERIFY(timeoutStart >= 0 && recoveryStart > timeoutStart);
        const QString timeoutHandler = player.mid(
            timeoutStart, recoveryStart - timeoutStart);
        QVERIFY2(timeoutHandler.contains(QStringLiteral(
                     "playerStatusToast.show(message, \"warning\", 5200)"))
                && !timeoutHandler.contains(QStringLiteral("errorOccurred")),
            "Recoverable timeouts must stay in-player and must not leave a stale modal");
        const qsizetype playbackErrorStart = player.indexOf(
            QStringLiteral("onPlaybackError:"));
        QVERIFY(playbackErrorStart >= 0 && timeoutStart > playbackErrorStart);
        const QString playbackErrorHandler = player.mid(
            playbackErrorStart, timeoutStart - playbackErrorStart);
        QVERIFY2(playbackErrorHandler.contains(QStringLiteral(
                     "playerStatusToast.show(message, \"error\", 6500)"))
                && !playbackErrorHandler.contains(QStringLiteral("errorOccurred")),
            "Ordinary playback errors must use the in-player error toast");
        QVERIFY2(player.contains(QStringLiteral(
                     "active: root.playbackBusy"))
                && player.contains(QStringLiteral(
                    "blocksInput: root.blockingPlaybackOperation"))
                && player.contains(QStringLiteral("showPanel: false")),
            "Player loading must keep the centered indicator while hiding its panel");
        const qsizetype overlayStart = player.indexOf(
            QStringLiteral("LoadingOverlay {"));
        const qsizetype toastStart = player.indexOf(
            QStringLiteral("StatusToast {"), overlayStart);
        QVERIFY(overlayStart >= 0 && toastStart > overlayStart);
        const QString playerLoading = player.mid(
            overlayStart, toastStart - overlayStart);
        QVERIFY2(playerLoading.contains(QStringLiteral(
                     "blocksInput: root.blockingPlaybackOperation")),
            "Only preparation and item switching may block player input");
        QVERIFY2(loadingOverlay.contains(QStringLiteral(
                     "property bool blocksInput: true"))
                && loadingOverlay.contains(QStringLiteral(
                    "property bool showPanel: true"))
                && loadingOverlay.contains(QStringLiteral(
                    "enabled: root.active && root.blocksInput")),
            "LoadingOverlay must separate its panel from input blocking");
        QVERIFY2(player.contains(QStringLiteral("dismissible: true"))
                && player.contains(QStringLiteral(
                    "anchors.horizontalCenter: parent.horizontalCenter"))
                && player.contains(QStringLiteral(
                    "globalToastBottom + 8"))
                && mainWindow.contains(QStringLiteral(
                    "item.globalToastBottom = Qt.binding"))
                && mainWindow.contains(QStringLiteral(
                    "return actionToast.visible"))
                && statusToast.contains(QStringLiteral("Theme.surfaceStrong"))
                && statusToast.contains(QStringLiteral("Theme.info"))
                && statusToast.contains(QStringLiteral("color: Theme.text"))
                && statusToast.contains(QStringLiteral(
                    "objectName: \"statusToastCloseButton\""))
                && statusToast.contains(QStringLiteral("controlSize: 28"))
                && statusToast.contains(QStringLiteral(
                    "objectName: \"statusToastCloseHighlight\""))
                && statusToast.contains(QStringLiteral("width: 20"))
                && statusToast.contains(QStringLiteral(
                    "? \"#18FFFFFF\" : \"transparent\""))
                && statusToast.contains(QStringLiteral(
                    "accessibleName: qsTr(\"Close notification\")"))
                && player.contains(QStringLiteral(
                    "enabled: root.globalToastBottom <= 0"))
                && statusToast.contains(QStringLiteral("Behavior on opacity"))
                && statusToast.contains(QStringLiteral("Behavior on scale")),
            "Player status toast must be readable, animated, stacked, and dismissible");
        QVERIFY2(mainWindow.contains(QStringLiteral("StatusToast {"))
                && mainWindow.contains(QStringLiteral("id: actionToast"))
                && mainWindow.contains(QStringLiteral(
                    "function showActionToast(message, tone)")),
            "Main and player feedback must share the same typed toast presentation");
    }

    void queueRefreshDescriptorIsConsumedBeforeOpeningMedia()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("components/PlaybackHost.qml"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));

        const qsizetype refreshBranch = content.indexOf(QStringLiteral(
            "if (page.automaticQueueRefreshPending)"));
        const qsizetype mediaAssignment = content.indexOf(QStringLiteral(
            "page.mediaUrl = descriptor.mediaUrl"));
        QVERIFY2(refreshBranch >= 0 && mediaAssignment > refreshBranch,
            "Queue refresh must branch before normal media assignment");
        const QString branch = content.mid(
            refreshBranch, mediaAssignment - refreshBranch);
        QVERIFY2(branch.contains(QStringLiteral(
                     "page.consumeQueueRefresh(descriptor)"))
                && branch.contains(QStringLiteral("return")),
            "Queue refresh descriptors must be consumed without replaying media");
    }

    void sessionLifecycleDoesNotCloseLoginOnBusyState()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("components/ApplicationLifecycleHost.qml"));
        const QString content = source(path);
        QVERIFY(content.contains(QStringLiteral("observedSessionGeneration")));
        QVERIFY(content.contains(QStringLiteral("sessionChanged || disconnected")));
        QVERIFY(!content.contains(QStringLiteral(
            "if (app.session.connected)\n                return")));
    }

    void sharedButtonsRemainKeyboardAccessible()
    {
        const QStringList componentNames {
            QStringLiteral("AppButton.qml"),
            QStringLiteral("NavButton.qml"),
        };
        for (const QString &componentName : componentNames) {
            const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
                .filePath(QStringLiteral("components/") + componentName);
            const QString content = source(path);
            QVERIFY2(content.contains(QStringLiteral("Qt.StrongFocus")),
                qPrintable(componentName + QStringLiteral(" must accept keyboard focus")));
            QVERIFY2(content.contains(QStringLiteral("Accessible.name:")),
                qPrintable(componentName + QStringLiteral(" must expose an accessible name")));
            QVERIFY2(content.contains(QStringLiteral("visualFocus")),
                qPrintable(componentName + QStringLiteral(" must show keyboard focus")));
        }
    }

    void popupMenusRemainKeyboardAccessible()
    {
        const QRegularExpression mouseArea(QStringLiteral(R"(\bMouseArea\s*\{)"));
        const QString appMenuPath = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("components/AppMenu.qml"));
        const QString appMenu = source(appMenuPath);
        QVERIFY2(appMenu.contains(QStringLiteral("Menu {")),
            "AppMenu must adapt the semantic Qt Menu control");
        QVERIFY2(appMenu.contains(QStringLiteral("popupType: Popup.Item")),
            "AppMenu must remain in the coordinated in-window popup layer");
        QVERIFY2(appMenu.contains(QStringLiteral("Popup.NoAutoClose"))
                && appMenu.contains(QStringLiteral("PopupCoordinator.registerPopup")),
            "AppMenu must use the shared dismissal and stacking protocol");

        const QStringList componentNames {
            QStringLiteral("MediaContextMenu.qml"),
            QStringLiteral("TrackMenu.qml"),
        };
        for (const QString &componentName : componentNames) {
            const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
                .filePath(QStringLiteral("components/") + componentName);
            const QString content = source(path);
            QVERIFY2(content.contains(QStringLiteral("AppMenu {"))
                    && content.contains(QStringLiteral("delegate: MenuItem")),
                qPrintable(componentName
                    + QStringLiteral(" must use semantic Menu/MenuItem controls")));
            QVERIFY2(content.contains(QStringLiteral("Accessible.name:"))
                    && content.contains(QStringLiteral("Accessible.id:"))
                    && content.contains(QStringLiteral("onTriggered:")),
                qPrintable(componentName
                    + QStringLiteral(" must expose named actionable menu items")));
            QVERIFY2(content.contains(QStringLiteral("Keys.onUpPressed"))
                    && content.contains(QStringLiteral("Keys.onDownPressed"))
                    && content.contains(QStringLiteral("Keys.onPressed"))
                    && content.contains(QStringLiteral("Qt.Key_Home"))
                    && content.contains(QStringLiteral("Qt.Key_End"))
                    && !content.contains(QStringLiteral("Keys.onHomePressed"))
                    && !content.contains(QStringLiteral("Keys.onEndPressed")),
                qPrintable(componentName + QStringLiteral(" must support menu navigation keys")));
            QVERIFY2(!content.contains(QStringLiteral("AccessibilityBridge")),
                qPrintable(componentName
                    + QStringLiteral(" must not synthesize platform focus events")));
            QVERIFY2(!mouseArea.match(content).hasMatch(),
                qPrintable(componentName + QStringLiteral(" must not regress to pointer-only rows")));
        }
    }

    void mediaCardsRemainKeyboardAccessible()
    {
        const QStringList componentNames {
            QStringLiteral("LibraryCard.qml"),
            QStringLiteral("PosterCard.qml"),
            QStringLiteral("EpisodeCard.qml"),
            QStringLiteral("RecentEpisodeCard.qml"),
        };
        for (const QString &componentName : componentNames) {
            const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
                .filePath(QStringLiteral("components/") + componentName);
            const QString content = source(path);
            QVERIFY2(content.contains(QStringLiteral("activeFocusOnTab: true")),
                qPrintable(componentName + QStringLiteral(" must be reachable by Tab")));
            QVERIFY2(content.contains(QStringLiteral("Accessible.role: Accessible.Button"))
                    && content.contains(QStringLiteral("Accessible.name:"))
                    && content.contains(QStringLiteral("Accessible.onPressAction")),
                qPrintable(componentName + QStringLiteral(" must expose an actionable card")));
            QVERIFY2(content.contains(QStringLiteral("Keys.onPressed"))
                    && content.contains(QStringLiteral("Qt.Key_Return"))
                    && content.contains(QStringLiteral("Qt.Key_Space"))
                    && content.contains(QStringLiteral("Qt.Key_Menu"))
                    && content.contains(QStringLiteral("Qt.Key_F10"))
                    && content.contains(QStringLiteral("Qt.ShiftModifier")),
                qPrintable(componentName + QStringLiteral(" must support keyboard actions")));
            QVERIFY2(content.contains(QStringLiteral("root.activeFocus")),
                qPrintable(componentName + QStringLiteral(" must show its focus state")));
            if (componentName != QStringLiteral("LibraryCard.qml")) {
                QVERIFY2(content.contains(QStringLiteral("accessibleName: qsTr(\"Play\")"))
                        && !content.contains(QStringLiteral("toolTipText: qsTr(\"Play\")")),
                    qPrintable(componentName
                        + QStringLiteral(" must name its play button without a tooltip")));
            }
        }
    }

    void homePageUsesExplicitLibraryScrollContexts()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("pages/HomePage.qml"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));

        QVERIFY(content.contains(QStringLiteral(
            "property string pendingLibraryScrollResetId")));
        QVERIFY(content.contains(QStringLiteral(
            "onCollectionReadyChanged:")));
        QVERIFY(content.contains(QStringLiteral(
            "onSortModeChanged:")));

        const qsizetype requestStart = content.indexOf(
            QStringLiteral("function requestLibraryScrollReset(targetId)"));
        const qsizetype requestEnd = content.indexOf(
            QStringLiteral("function completeLibraryScrollReset()"), requestStart);
        QVERIFY(requestStart >= 0 && requestEnd > requestStart);
        const QString requestReset = content.mid(
            requestStart, requestEnd - requestStart);
        QVERIFY(requestReset.contains(QStringLiteral(
            "root.pendingLibraryScrollResetId = normalizedId")));
        QVERIFY(requestReset.contains(QStringLiteral(
            "libraryGrid.resetScrollPosition()")));
        QVERIFY(requestReset.contains(QStringLiteral(
            "Qt.callLater(root.completeLibraryScrollReset)")));

        const qsizetype completeEnd = content.indexOf(
            QStringLiteral("function goHome()"), requestEnd);
        QVERIFY(completeEnd > requestEnd);
        const QString completeReset = content.mid(
            requestEnd, completeEnd - requestEnd);
        QVERIFY(completeReset.contains(QStringLiteral(
            "libraryGrid.resetScrollPosition()")));
        QVERIFY(completeReset.contains(QStringLiteral(
            "if (root.collectionReady)")));

        const qsizetype openStart = content.indexOf(
            QStringLiteral("function openLibraryView(item)"));
        const qsizetype openEnd = content.indexOf(
            QStringLiteral("function openLibraryItem(item)"), openStart);
        QVERIFY(openStart >= 0 && openEnd > openStart);
        const QString openLibrary = content.mid(openStart, openEnd - openStart);
        QVERIFY2(openLibrary.contains(QStringLiteral(
                     "root.requestLibraryScrollReset(root.libraryId)")),
            "A fresh library route must reset the real GridView after its context changes");

        const qsizetype backStart = content.indexOf(
            QStringLiteral("function goBack()"));
        const qsizetype backEnd = content.indexOf(
            QStringLiteral("function localizedLibraryTitle(item)"), backStart);
        QVERIFY(backStart >= 0 && backEnd > backStart);
        const QString goBack = content.mid(backStart, backEnd - backStart);
        const qsizetype detailReturnStart = goBack.indexOf(QStringLiteral(
            "else if (root.depth === 2 && root.libraryId.length > 0)"));
        const qsizetype detailReturnEnd = goBack.indexOf(QStringLiteral(
            "else if (root.depth === 2 && root.detailReturnPage >= 0)"),
            detailReturnStart);
        QVERIFY(detailReturnStart >= 0 && detailReturnEnd > detailReturnStart);
        const QString detailReturn = goBack.mid(
            detailReturnStart, detailReturnEnd - detailReturnStart);
        QVERIFY2(!detailReturn.contains(QStringLiteral("requestLibraryScrollReset")),
            "Returning from details must preserve the parent library position");
    }

    void firstShellDefersAuthenticatedHomeAndHeavyDialogs()
    {
        const QDir qmlRoot(QStringLiteral(YANAMI_QML_SOURCE_DIR));
        const QString mainWindow = source(
            qmlRoot.filePath(QStringLiteral("Main.qml")));
        const QString homePage = source(
            qmlRoot.filePath(QStringLiteral("pages/HomePage.qml")));
        QVERIFY2(!mainWindow.isEmpty(), "Main.qml must be readable");
        QVERIFY2(!homePage.isEmpty(), "HomePage.qml must be readable");

        QVERIFY(mainWindow.contains(QStringLiteral(
            "active: app.session.connected")));
        QVERIFY(mainWindow.contains(QStringLiteral(
            "source: Qt.resolvedUrl(\"pages/HomePage.qml\")")));
        QVERIFY(mainWindow.contains(QStringLiteral(
            "text: qsTr(\"Connect your Emby server\")")));
        QVERIFY(!homePage.contains(QStringLiteral(
            "text: qsTr(\"Connect your Emby server\")")));

        const QList<QPair<QString, QString>> deferredDialogs {
            { QStringLiteral("MetadataEditorDialog"),
                QStringLiteral("metadataEditorLoader") },
            { QStringLiteral("ImageEditorDialog"),
                QStringLiteral("imageEditorLoader") },
            { QStringLiteral("RefreshMetadataDialog"),
                QStringLiteral("refreshMetadataLoader") },
            { QStringLiteral("MediaTargetDialog"),
                QStringLiteral("mediaTargetLoader") },
        };
        for (const auto &[dialog, loaderId] : deferredDialogs) {
            const qsizetype idPosition = mainWindow.indexOf(
                QStringLiteral("id: ") + loaderId);
            const qsizetype loaderStart = mainWindow.lastIndexOf(
                QStringLiteral("Loader {"), idPosition);
            const qsizetype loaderEnd = mainWindow.indexOf(
                QLatin1Char('}'), idPosition);
            QVERIFY2(loaderStart >= 0 && idPosition > loaderStart
                    && loaderEnd > idPosition,
                qPrintable(dialog + QStringLiteral(" must be owned by a Loader")));
            const QString loader = mainWindow.mid(
                loaderStart, loaderEnd - loaderStart);
            QVERIFY2(loader.contains(QStringLiteral("active: false"))
                    && loader.contains(dialog + QStringLiteral(".qml")),
                qPrintable(dialog + QStringLiteral(" must start inactive")));
            const QRegularExpression eagerDeclaration(
                QStringLiteral(R"(\b%1\s*\{)").arg(dialog));
            QVERIFY2(!eagerDeclaration.match(mainWindow).hasMatch(),
                qPrintable(dialog + QStringLiteral(" must not be created by the first shell")));
        }
        QVERIFY(mainWindow.contains(QStringLiteral("AppErrorDialog {"))
            && mainWindow.contains(QStringLiteral(
                "errorDialog.show(app.status.message)")));
    }

    void bootstrapHandoffMirrorsBalancedNativeBrandFrameBeforeFading()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("Main.qml"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));

        QVERIFY(content.contains(QStringLiteral(
            "property bool bootstrapHandoffPending: bootstrapHandoffRequested")));
        QVERIFY(content.contains(QStringLiteral(
            "property bool bootstrapHandoffReady: false")));
        QVERIFY(content.contains(QStringLiteral(
            "property bool bootstrapHandoffTransitionVisible: bootstrapHandoffRequested")));
        QVERIFY(content.contains(QStringLiteral(
            "opacity: bootstrapHandoffPending ? 0.0 : 1.0")));
        QVERIFY(content.contains(QStringLiteral(
            "active: window.bootstrapHandoffTransitionVisible")));
        QCOMPARE(content.count(QStringLiteral("sourceComponent:")), 1);
        QVERIFY(content.contains(QStringLiteral(
            "objectName: \"bootstrapHandoffTransition\"")));
        QVERIFY(content.contains(QStringLiteral("color: \"#080D17\"")));
        QVERIFY(content.contains(QStringLiteral(
            "\"qrc:/qt/qml/Yanami/Ui/qml/assets/yanami-logo.png\")")));
        QVERIFY(content.contains(QStringLiteral(
            "objectName: \"bootstrapHandoffLogo\"")));
        QVERIFY(content.contains(QStringLiteral("width: 540")));
        QVERIFY(content.contains(QStringLiteral("height: 320")));
        QVERIFY(content.contains(QStringLiteral("x: 214")));
        QVERIFY(content.contains(QStringLiteral("y: 38")));
        QVERIFY(content.contains(QStringLiteral("width: 112")));
        QVERIFY(content.contains(QStringLiteral("height: 112")));
        QVERIFY(content.contains(QStringLiteral("mipmap: true")));
        QVERIFY(content.contains(QStringLiteral("smooth: true")));
        QVERIFY(content.contains(QStringLiteral(
            "border.color: \"#1B2534\"")));
        QVERIFY(content.contains(QStringLiteral(
            "objectName: \"bootstrapHandoffTitle\"")));
        QVERIFY(content.contains(QStringLiteral(
            "objectName: \"bootstrapHandoffStatus\"")));
        QVERIFY(content.contains(QStringLiteral("text: \"Yanami\"")));
        QVERIFY(content.contains(QStringLiteral(
            "text: qsTr(\"Starting Yanami…\")")));
        QVERIFY(content.contains(QStringLiteral(
            "color: \"#F5F7FA\"")));
        QVERIFY(content.contains(QStringLiteral(
            "color: \"#9AA3B2\"")));
        QVERIFY(content.contains(QStringLiteral(
            "renderType: Text.NativeRendering")));
        QVERIFY(content.contains(QStringLiteral("font.pixelSize: 30")));
        QVERIFY(content.contains(QStringLiteral("font.pixelSize: 16")));
        QVERIFY(content.contains(QStringLiteral(
            "font.weight: Font.DemiBold")));
        QVERIFY2(!content.contains(QStringLiteral(
            "objectName: \"bootstrapHandoffSpinner\"")),
            "QML must not restart the native spinner with a mismatched phase");
        QVERIFY2(!content.contains(QStringLiteral(
            "SequentialAnimation on scale")),
            "the handoff logo must remain on exact device-pixel boundaries");
        QVERIFY(content.contains(QStringLiteral("interval: 200")));
        QVERIFY(content.contains(QStringLiteral(
            "running: window.bootstrapHandoffReady")));
        QVERIFY(content.contains(QStringLiteral("duration: 180")));
        QVERIFY(content.contains(QStringLiteral(
            "window.bootstrapHandoffTransitionVisible = false")));
    }

    void inactivePagesUseUrlLoaders()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("Main.qml"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));

        const QStringList pageTypes {
            QStringLiteral("SearchPage"),
            QStringLiteral("PlayerPage"),
            QStringLiteral("SettingsPage"),
            QStringLiteral("FavoritesPage"),
            QStringLiteral("AboutPage"),
        };
        for (const QString &pageType : pageTypes) {
            const QString pageSource = QStringLiteral("pages/")
                + pageType + QStringLiteral(".qml");
            const qsizetype sourcePosition = content.indexOf(pageSource);
            const qsizetype loaderStart = content.lastIndexOf(
                QStringLiteral("Loader {"), sourcePosition);
            QVERIFY2(sourcePosition >= 0,
                qPrintable(pageType + QStringLiteral(" must have a URL loader")));
            QVERIFY(loaderStart >= 0);
            const QString loader = content.mid(
                loaderStart, sourcePosition - loaderStart);
            QVERIFY2(!loader.contains(QStringLiteral("sourceComponent:")),
                qPrintable(pageType
                    + QStringLiteral(" must defer type parsing through a URL")));
            const QRegularExpression eagerDeclaration(
                QStringLiteral(R"(\b%1\s*\{)").arg(pageType));
            QVERIFY2(!eagerDeclaration.match(content).hasMatch(),
                qPrintable(pageType + QStringLiteral(" must not resolve in the first shell")));
        }
        QVERIFY(content.contains(QStringLiteral(
            "readonly property var playerPage: playerLoader.item")));
    }

    void homeLandingRailsPassVerticalWheelToThePage()
    {
        const QString homePath = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("pages/HomePage.qml"));
        const QString home = source(homePath);
        QVERIFY2(!home.isEmpty(), qPrintable(homePath));
        QCOMPARE(home.count(QStringLiteral("passVerticalWheelToParent: true")), 3);

        const QString listPath = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("components/SmoothHorizontalList.qml"));
        const QString list = source(listPath);
        QVERIFY2(!list.isEmpty(), qPrintable(listPath));
        QVERIFY(list.contains(QStringLiteral(
            "property bool passVerticalWheelToParent: false")));
        QVERIFY(list.contains(QStringLiteral(
            "? Qt.ShiftModifier : Qt.KeyboardModifierMask")));
    }

    void homePageAutoPositionsASeasonAtItsFirstUnplayedEpisode()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("pages/HomePage.qml"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));

        QVERIFY(content.contains(QStringLiteral("EpisodeScrollPolicy {")));
        QVERIFY(content.contains(QStringLiteral(
            "activeScopeId: root.depth === 3 ? root.seasonId : \"\"")));
        QVERIFY(content.contains(QStringLiteral("view: episodesList")));
        QVERIFY(content.contains(QStringLiteral(
            "refreshing: root.depth === 3 && root.collectionRefreshing")));

        const qsizetype requestStart = content.indexOf(
            QStringLiteral("function requestEpisodeScroll(targetId)"));
        const qsizetype requestEnd = content.indexOf(
            QStringLiteral("function goHome()"), requestStart);
        QVERIFY(requestStart >= 0 && requestEnd > requestStart);
        const QString request = content.mid(
            requestStart, requestEnd - requestStart);
        QVERIFY(request.contains(QStringLiteral(
            "episodeScrollPolicy.request(normalizedId)")));
        QVERIFY(!content.contains(QStringLiteral("function positionEpisodeList(")));

        const QString policyPath = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("components/EpisodeScrollPolicy.qml"));
        const QString policy = source(policyPath);
        QVERIFY2(!policy.isEmpty(), qPrintable(policyPath));
        QVERIFY(policy.contains(QStringLiteral("property int generation")));
        QVERIFY(policy.contains(QStringLiteral(
            "expectedGeneration !== root.generation")));
        QVERIFY(policy.contains(QStringLiteral(
            "root.view.positionViewAtIndex(index, ListView.Beginning)")));
        QVERIFY(policy.contains(QStringLiteral(
            "root.view.currentIndex = index")));
        QVERIFY(policy.contains(QStringLiteral(
            "function onUserScrollStarted() { root.notifyUserScroll() }")));

        const QString listPath = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("components/SmoothHorizontalList.qml"));
        const QString list = source(listPath);
        QVERIFY2(!list.isEmpty(), qPrintable(listPath));
        QVERIFY(list.contains(QStringLiteral("signal userScrollStarted()")));
        QVERIFY(list.contains(QStringLiteral("onDraggingChanged:")));
        QVERIFY(list.contains(QStringLiteral("Keys.onPressed: event =>")));
        QVERIFY(list.contains(QStringLiteral("Qt.Key_Left")));
        QVERIFY(list.contains(QStringLiteral("Qt.Key_Right")));
        QVERIFY(list.contains(QStringLiteral("onPressedChanged:")));
        QVERIFY(list.contains(QStringLiteral("onWheel: event =>")));
        QVERIFY(list.count(QStringLiteral("root.userScrollStarted()")) >= 3);

        const qsizetype openSeasonStart = content.indexOf(
            QStringLiteral("function openSeason(item)"));
        const qsizetype openExternalSeasonStart = content.indexOf(
            QStringLiteral("function openExternalSeason(item, returnPage)"),
            openSeasonStart);
        const qsizetype goBackStart = content.indexOf(
            QStringLiteral("function goBack()"), openExternalSeasonStart);
        QVERIFY(openSeasonStart >= 0
            && openExternalSeasonStart > openSeasonStart
            && goBackStart > openExternalSeasonStart);
        QVERIFY(content.mid(openSeasonStart,
                    openExternalSeasonStart - openSeasonStart)
                    .contains(QStringLiteral(
                        "root.requestEpisodeScroll(root.seasonId)")));
        QVERIFY(content.mid(openExternalSeasonStart,
                    goBackStart - openExternalSeasonStart)
                    .contains(QStringLiteral(
                        "root.requestEpisodeScroll(root.seasonId)")));

        const qsizetype depthThreeStart = content.indexOf(
            QStringLiteral("else if (root.depth === 3)"), goBackStart);
        const qsizetype depthTwoStart = content.indexOf(
            QStringLiteral("else if (root.depth === 2"), depthThreeStart);
        QVERIFY(depthThreeStart >= 0 && depthTwoStart > depthThreeStart);
        QVERIFY(content.mid(depthThreeStart, depthTwoStart - depthThreeStart)
                    .contains(QStringLiteral("episodeScrollPolicy.cancel()")));
    }

    void seriesDetailsExposeContinueQueueBeforeSeasons()
    {
        const QString path = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("pages/HomePage.qml"));
        const QString content = source(path);
        QVERIFY2(!content.isEmpty(), qPrintable(path));
        QVERIFY(content.contains(QStringLiteral(
            "queryModel(\"seriesContinue\", root.seriesId)")));
        QVERIFY(content.contains(QStringLiteral(
            "playButtonVisible: root.depth !== 2")));

        const qsizetype continueStart = content.indexOf(
            QStringLiteral("id: seriesContinueList"));
        const qsizetype seasonsStart = content.indexOf(
            QStringLiteral("text: qsTr(\"Seasons and specials\")"));
        QVERIFY(continueStart >= 0 && seasonsStart > continueStart);
        const QString continueSection = content.mid(
            continueStart, seasonsStart - continueStart);
        QVERIFY(continueSection.contains(QStringLiteral(
            "model: animatedSeriesContinueModel")));
        QVERIFY(continueSection.contains(QStringLiteral(
            "delegate: RecentEpisodeCard")));
        QVERIFY(continueSection.contains(QStringLiteral(
            "subtitle: LocaleText.mediaSubtitle(modelData)")));
        QVERIFY(continueSection.contains(QStringLiteral(
            "animatedSeriesContinueModel.count > 0")));
        QVERIFY(continueSection.contains(QStringLiteral(
            "modelData.id, modelData.title")));
        QVERIFY(continueSection.contains(QStringLiteral(
            "root.seriesPlaybackContext()")));

        const QString heroPath = QDir(QStringLiteral(YANAMI_QML_SOURCE_DIR))
            .filePath(QStringLiteral("components/DetailHero.qml"));
        const QString heroContent = source(heroPath);
        QVERIFY2(!heroContent.isEmpty(), qPrintable(heroPath));
        QVERIFY(heroContent.contains(QStringLiteral(
            "property bool playButtonVisible: true")));
        QVERIFY(heroContent.contains(QStringLiteral(
            "visible: root.playButtonVisible")));
    }

    void controllerNavigationUsesOneSemanticFocusGraph()
    {
        const QDir qmlRoot(QStringLiteral(YANAMI_QML_SOURCE_DIR));
        const QString mainWindow = source(
            qmlRoot.filePath(QStringLiteral("Main.qml")));
        const QString navigator = source(qmlRoot.filePath(
            QStringLiteral("components/SpatialFocusNavigator.qml")));
        const QString inputTestScope = source(qmlRoot.filePath(
            QStringLiteral("components/ControllerInputTestScope.qml")));
        const QString settings = source(qmlRoot.filePath(
            QStringLiteral("pages/SettingsPage.qml")));
        const QString player = source(qmlRoot.filePath(
            QStringLiteral("pages/PlayerPage.qml")));
        const QString volumeControl = source(qmlRoot.filePath(
            QStringLiteral("components/VolumeControl.qml")));
        const QString slider = source(qmlRoot.filePath(
            QStringLiteral("components/AppSlider.qml")));
        const QString textField = source(qmlRoot.filePath(
            QStringLiteral("components/AppTextField.qml")));
        const QString search = source(qmlRoot.filePath(
            QStringLiteral("pages/SearchPage.qml")));

        QVERIFY2(!mainWindow.isEmpty(), "Main.qml must be readable");
        QVERIFY2(!navigator.isEmpty(),
            "The shared spatial focus navigator must be packaged");
        QVERIFY2(!inputTestScope.isEmpty(),
            "The controller input-test ownership scope must be packaged");
        QVERIFY(mainWindow.contains(QStringLiteral(
            "SpatialFocusNavigator {")));
        QVERIFY(mainWindow.contains(QStringLiteral(
            "function onActionPressed(action, repeated)")));
        QVERIFY(mainWindow.contains(QStringLiteral(
            "InputModality.PagePrevious"))
            && mainWindow.contains(QStringLiteral(
                "InputModality.ScrollRight"))
            && mainWindow.contains(QStringLiteral(
                "function openControllerMenu()")));
        QVERIFY(mainWindow.contains(QStringLiteral(
            "qsTr(\"Minimize window\")"))
            && mainWindow.contains(QStringLiteral(
                "qsTr(\"Close Yanami\")")));
        QVERIFY(mainWindow.contains(QStringLiteral(
            "window.playerPage.consumeBack()"))
            && player.contains(QStringLiteral(
                "function consumeBack()")));
        QVERIFY(player.contains(QStringLiteral(
            "action === InputModality.Search"))
            && player.contains(QStringLiteral(
                "root.togglePlayerFullScreen()"))
            && player.contains(QStringLiteral(
                "!repeated && !root.blockingPopupOpened"))
            && player.contains(QStringLiteral(
                "KeyNavigation.up: root.canSkipIntro"))
            && player.contains(QStringLiteral(
                "visible: root.canSkipIntro || opacity > 0"))
            && !player.contains(QStringLiteral(
                "id: introOfferTimeout"))
            && player.contains(QStringLiteral(
                "volumeControl.showTransientVolume()"))
            && player.contains(QStringLiteral(
                "popupHost: player"))
            && !player.contains(QStringLiteral(
                "popupHost: root"))
            && player.contains(QStringLiteral(
                "visible: opacity > 0 || root.controlBarFocusMode"))
            && player.contains(QStringLiteral(
                "enabled: opacity > 0.05 || root.controlBarFocusMode")));
        QVERIFY(volumeControl.contains(QStringLiteral(
            "function showTransientVolume()"))
            && volumeControl.contains(QStringLiteral(
                "property Item popupHost: null"))
            && volumeControl.contains(QStringLiteral(
                "takesFocus: false"))
            && volumeControl.contains(QStringLiteral(
                "blocksShortcuts: false")));

        QVERIFY(navigator.contains(QStringLiteral(
            "function moveInVirtualView(current, direction)"))
            && navigator.contains(QStringLiteral(
                "function candidateScore(sourceRect, candidateRect, direction)"))
            && navigator.contains(QStringLiteral(
                "function revealItem(item)"))
            && navigator.contains(QStringLiteral(
                "property var pageBookmarks")));

        QVERIFY(settings.contains(QStringLiteral(
            "objectName: \"controllerDiagnosticsSection\""))
            && settings.contains(QStringLiteral(
                "InputModality.connectedDevices"))
            && settings.contains(QStringLiteral(
                "InputModality.lastActionName")));
        QVERIFY(settings.contains(QStringLiteral(
            "available: root.pageActive && root.activeSection === 4"))
            && settings.contains(QStringLiteral(
                "function onControllerInputTestAction(action, repeated)"))
            && settings.contains(QStringLiteral(
                "controllerInputTestScope.start()"))
            && settings.contains(QStringLiteral(
                "controllerInputTestScope.handleAction(action, repeated)"))
            && settings.contains(QStringLiteral(
                "press B / Back to exit"))
            && !settings.contains(QStringLiteral(
                "function onActionPressed(action, repeated)")));
        QVERIFY(settings.contains(QStringLiteral(
            "border.width: sectionButton.visualFocus"))
            && settings.contains(QStringLiteral(
                "? 2 : (sectionButton.selected ? 1 : 0)"))
            && settings.contains(QStringLiteral(
                "border.color: sectionButton.visualFocus"))
            && settings.contains(QStringLiteral(
                "? Theme.accent : \"#52FF6687\"")));
        QVERIFY(mainWindow.contains(QStringLiteral(
            "item.pageActive = Qt.binding"))
            && mainWindow.contains(QStringLiteral(
                "return window.currentPage === 3"))
            && mainWindow.contains(QStringLiteral(
                "InputModality.controllerInputTestActive")));
        QVERIFY(inputTestScope.contains(QStringLiteral(
            "InputModality.acquireControllerInputTest(root)"))
            && inputTestScope.contains(QStringLiteral(
                "InputModality.releaseControllerInputTest(root)"))
            && inputTestScope.contains(QStringLiteral(
                "action === InputModality.Back"))
            && inputTestScope.contains(QStringLiteral(
                "Component.onDestruction")));
        QVERIFY(slider.contains(QStringLiteral(
            "focusPolicy: Qt.StrongFocus"))
            && slider.contains(QStringLiteral(
                "control.visualFocus")));
        QVERIFY(textField.contains(QStringLiteral(
            "Keys.priority: Keys.BeforeItem"))
            && textField.contains(QStringLiteral(
                "controllerRightHandler"))
            && search.contains(QStringLiteral(
                "controllerExitTarget"))
            && search.contains(QStringLiteral(
                "controllerDownHandler")));
    }

    void aboutPageExportsDiagnosticsAndLinksSupportSafely()
    {
        const QDir qmlRoot(QStringLiteral(YANAMI_QML_SOURCE_DIR));
        const QString about = source(qmlRoot.filePath(
            QStringLiteral("pages/AboutPage.qml")));
        const QString localeText = source(qmlRoot.filePath(
            QStringLiteral("LocaleText.qml")));

        QVERIFY2(!about.isEmpty(), "AboutPage.qml must be readable");
        QVERIFY(about.contains(QStringLiteral(
            "https://afdian.com/a/twooosix")));
        QVERIFY(about.contains(QStringLiteral(
            "app.diagnostics.exportLogs()"))
            && about.contains(QStringLiteral(
                "app.diagnostics.openExportFolder()"))
            && about.contains(QStringLiteral(
                "function openExternalUrl(url, failureMessage)"))
            && about.contains(QStringLiteral(
                "if (!Qt.openUrlExternally(url))")));

        QVERIFY2(!localeText.isEmpty(), "LocaleText.qml must be readable");
        QVERIFY(localeText.contains(QStringLiteral(
            "if (subtitle.length === 0 || subtitle === itemType)"))
            && localeText.contains(QStringLiteral(
                "return itemTypeLabel(itemType)"))
            && localeText.contains(QStringLiteral(
                "case \"MusicVideo\": return qsTr(\"Music video\")")));
    }

    void asyncImageKeysStayInsideTheCacheRoot()
    {
        QTemporaryDir temporaryDirectory;
        QVERIFY(temporaryDirectory.isValid());
        const QString cacheRoot = QDir(temporaryDirectory.path())
            .filePath(QStringLiteral("cache"));
        QVERIFY(QDir().mkpath(cacheRoot));
        const QString normalizedRoot =
            YanamiAsyncImageKey::normalizedCacheRoot(cacheRoot);
        const auto key = [](const QByteArray &path) {
            return QStringLiteral("v1-") + QString::fromLatin1(path.toHex());
        };

        const QString validRelative = QStringLiteral("images/server/poster.jpg");
        QCOMPARE(
            YanamiAsyncImageKey::decode(key(validRelative.toUtf8()), normalizedRoot),
            QDir::fromNativeSeparators(
                QDir(normalizedRoot).filePath(validRelative)));
        const QString unicodeRelative = QStringLiteral("image-editor/海报.jpg");
        QCOMPARE(
            YanamiAsyncImageKey::decode(key(unicodeRelative.toUtf8()), normalizedRoot),
            QDir::fromNativeSeparators(
                QDir(normalizedRoot).filePath(unicodeRelative)));

        QVERIFY(YanamiAsyncImageKey::decode(
            key(QDir(normalizedRoot).filePath(QStringLiteral("outside.jpg")).toUtf8()),
            normalizedRoot).isEmpty());
        QVERIFY(YanamiAsyncImageKey::decode(
            key(QByteArrayLiteral("../outside.jpg")), normalizedRoot).isEmpty());
        QVERIFY(YanamiAsyncImageKey::decode(
            key(QByteArrayLiteral("images//poster.jpg")), normalizedRoot).isEmpty());
        QVERIFY(YanamiAsyncImageKey::decode(
            key(QByteArrayLiteral("images/poster.jpg:stream")), normalizedRoot).isEmpty());
        QVERIFY(YanamiAsyncImageKey::decode(
            QStringLiteral("v1-ff"), normalizedRoot).isEmpty());
        QVERIFY(YanamiAsyncImageKey::decode(
            QString::fromLatin1(QByteArrayLiteral("images/poster.jpg").toHex()),
            normalizedRoot).isEmpty());
        QVERIFY(YanamiAsyncImageKey::decode(
            key(QByteArrayLiteral("images/poster.jpg")), QString{}).isEmpty());
    }
};

QTEST_MAIN(QmlArchitectureTests)
#include "QmlArchitectureTests.moc"
