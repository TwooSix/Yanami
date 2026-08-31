#include "UpdateHelperProcess.hpp"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QLoggingCategory>
#include <QPointer>
#include <QProcess>
#include <QPromise>
#include <QThread>
#include <QtConcurrentRun>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <utility>

namespace {
Q_LOGGING_CATEGORY(updateTransportLog, "yanami.update.transport")

constexpr int pollIntervalMs = 50;
constexpr int terminateWaitMs = 1000;
constexpr qsizetype maximumOutputBytes = 4 * 1024 * 1024;

struct WorkState
{
    std::atomic_bool cancelled = false;
};

struct ProcessEvent
{
    QByteArray output;
    int exitCode = -1;
    bool crashed = true;
    bool terminal = false;
};

void runHelper(
    QPromise<ProcessEvent> &promise,
    const std::shared_ptr<WorkState> &state,
    const QString &program,
    const QStringList &arguments,
    int timeoutMs,
    const std::function<void(QProcess &)> &starter)
{
    QElapsedTimer elapsed;
    elapsed.start();
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *creation) {
            creation->flags |= CREATE_NO_WINDOW;
        });
#endif

    qsizetype outputBytes = 0;
    bool outputLimitReached = false;
    const auto drainOutput = [&] {
        const QByteArray output = process.readAllStandardOutput();
        // stderr is diagnostic noise, not the updater's JSON protocol. Drain
        // it throughout the operation so a full pipe cannot stall the helper.
        process.readAllStandardError();
        if (output.isEmpty())
            return;
        if (output.size() > maximumOutputBytes - outputBytes) {
            outputLimitReached = true;
            return;
        }
        outputBytes += output.size();
        promise.addResult(ProcessEvent{output});
    };
    const auto expired = [&] {
        return timeoutMs > 0 && elapsed.elapsed() >= timeoutMs;
    };
    const auto interrupted = [&] {
        return state->cancelled.load(std::memory_order_relaxed)
            || expired() || outputLimitReached;
    };
    const auto complete = [&](int exitCode, bool crashed) {
        qCInfo(updateTransportLog)
            << "helper completed elapsed_ms=" << elapsed.elapsed()
            << "exit_code=" << exitCode << "crashed=" << crashed
            << "cancelled=" << state->cancelled.load(std::memory_order_relaxed)
            << "timed_out=" << expired()
            << "output_limit=" << outputLimitReached;
        promise.addResult(ProcessEvent{{}, exitCode, crashed, true});
    };

    if (interrupted()) {
        complete(-1, true);
        return;
    }

    qCInfo(updateTransportLog) << "helper launch started";
    starter(process);
    qCInfo(updateTransportLog)
        << "helper launch returned elapsed_ms=" << elapsed.elapsed();
    process.closeWriteChannel();

    while (process.state() != QProcess::NotRunning) {
        drainOutput();
        if (interrupted()) {
            // No GUI object owns this QProcess, and cancellation never waits
            // for it. Only this worker kills/reaps the exact helper process;
            // an updater successfully handed off by it remains independent.
            process.kill();
            process.waitForFinished(terminateWaitMs);
            drainOutput();
            complete(-1, true);
            return;
        }
        const int waitMs = timeoutMs > 0
            ? std::min(pollIntervalMs,
                  std::max(1, timeoutMs - int(elapsed.elapsed())))
            : pollIntervalMs;
        // The blocking process APIs are intentionally confined to this worker.
        // waitForReadyRead also services process startup/exit and both pipes.
        process.waitForReadyRead(waitMs);
    }

    // An exit can race the final readyRead notification. Publish its remaining
    // bytes before the terminal event, including a final line without '\n'.
    drainOutput();
    if (interrupted() || process.error() == QProcess::FailedToStart) {
        complete(-1, true);
        return;
    }
    complete(process.exitCode(), process.exitStatus() == QProcess::CrashExit);
}
}

struct UpdateHelperProcess::Private
{
    QFutureWatcher<ProcessEvent> watcher;
    std::shared_ptr<WorkState> state = std::make_shared<WorkState>();
    ProcessStarter starter;
    int deliveredResults = 0;
    bool started = false;
    bool terminalDelivered = false;

    void deliverAvailable(UpdateHelperProcess *owner)
    {
        const QFuture<ProcessEvent> future = watcher.future();
        QPointer<UpdateHelperProcess> guard(owner);
        while (deliveredResults < future.resultCount()) {
            const ProcessEvent event = future.resultAt(deliveredResults++);
            if (event.terminal) {
                if (!terminalDelivered) {
                    terminalDelivered = true;
                    emit owner->finished(event.exitCode, event.crashed);
                }
                return;
            }
            emit owner->outputReady(event.output);
            // A receiver can cancel or destroy this facade synchronously.
            if (!guard)
                return;
        }
    }
};

UpdateHelperProcess::UpdateHelperProcess(QObject *parent)
    : UpdateHelperProcess([](QProcess &process) { process.start(); }, parent)
{
}

UpdateHelperProcess::UpdateHelperProcess(ProcessStarter starter, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
    d->starter = std::move(starter);
    connect(&d->watcher, &QFutureWatcher<ProcessEvent>::resultsReadyAt, this,
        [this](int, int) { d->deliverAvailable(this); });
    connect(&d->watcher, &QFutureWatcher<ProcessEvent>::finished, this, [this] {
        // Do not rely on separate queued readyRead/finished notification order.
        d->deliverAvailable(this);
    });
}

UpdateHelperProcess::~UpdateHelperProcess()
{
    // QFutureWatcher destruction detaches notifications without waiting for
    // the task. The task only captures shared state, never this QObject.
    cancel();
}

void UpdateHelperProcess::start(
    const QString &program, const QStringList &arguments, int timeoutMs)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->started)
        return;
    d->started = true;
    d->watcher.setFuture(QtConcurrent::run(
        runHelper, d->state, program, arguments, std::max(0, timeoutMs),
        d->starter));
}

void UpdateHelperProcess::cancel()
{
    d->state->cancelled.store(true, std::memory_order_relaxed);
}
