#pragma once

#include <QByteArray>
#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

class QProcess;

// One helper operation. All process APIs, including the synchronously expensive
// part of QProcess::start on Windows, run outside the object's owning thread.
class UpdateHelperProcess final : public QObject
{
    Q_OBJECT

public:
    explicit UpdateHelperProcess(QObject *parent = nullptr);
    ~UpdateHelperProcess() override;

    void start(
        const QString &program, const QStringList &arguments, int timeoutMs = 0);
    void cancel();

signals:
    void outputReady(QByteArray output);
    void finished(int exitCode, bool crashed);

private:
    // A private launch seam models a slow OS process-creation call in tests,
    // without adding an environment-controlled production behavior.
    using ProcessStarter = std::function<void(QProcess &)>;
    UpdateHelperProcess(ProcessStarter starter, QObject *parent);
    friend class UpdateHelperProcessTests;

    struct Private;
    std::unique_ptr<Private> d;
};
