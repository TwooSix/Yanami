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
