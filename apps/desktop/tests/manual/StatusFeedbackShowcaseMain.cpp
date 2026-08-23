#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

#include <cstdio>

int main(int argc, char *argv[])
{
    QGuiApplication::setApplicationName(
        QStringLiteral("Yanami Status Feedback Showcase"));
    QGuiApplication::setOrganizationName(QStringLiteral("Yanami"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QQuickWindow::setDefaultAlphaBuffer(false);

    QGuiApplication app(argc, argv);
    app.setWindowIcon(QIcon(QStringLiteral(
        ":/qt/qml/Yanami/Ui/qml/assets/yanami-logo.png")));

    QQmlApplicationEngine engine;
    engine.addImportPath(
        QCoreApplication::applicationDirPath() + QStringLiteral("/qml"));
    QObject::connect(
        &engine, &QQmlEngine::warnings,
        [](const QList<QQmlError> &warnings) {
            for (const QQmlError &warning : warnings) {
                std::fprintf(stderr, "%s\n",
                             warning.toString().toLocal8Bit().constData());
            }
            std::fflush(stderr);
        });
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, [] { QCoreApplication::exit(EXIT_FAILURE); },
        Qt::QueuedConnection);
    engine.loadFromModule(
        QStringLiteral("Yanami.StatusFeedbackShowcase"),
        QStringLiteral("StatusFeedbackShowcase"));

    if (app.arguments().contains(QStringLiteral("--smoke-test")))
        QTimer::singleShot(500, &app, &QCoreApplication::quit);

    return app.exec();
}
