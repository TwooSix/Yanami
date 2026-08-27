#pragma once

#include "BackendPorts.hpp"
#include "RustBridgeRuntime.hpp"

#include <QObject>
#include <QString>
#include <QThreadPool>

#include <memory>

struct RuntimeInitializationResult
{
    bool ready = false;
    QString errorCode;
    QString errorMessage;
    QString dataDirectory;
    QString cacheDirectory;
};

// Owns the dynamic Rust bridge and its process-wide backend handle. Session
// observes the host lifecycle; other feature coordinators receive a typed
// runtime reference. None of them resolve C symbols or own bridge strings.
class RuntimeHost final : public QObject
{
    Q_OBJECT

public:
    explicit RuntimeHost(QObject *parent = nullptr);
    ~RuntimeHost() override;

    RuntimeInitializationResult initialize(
        const QString &applicationDirectory = {},
        const QString &dataDirectory = {},
        bool isolatedCredentials = false);
    void shutdown();

    bool ready() const;
    RustBridgeRuntime *runtime() const { return m_runtime.get(); }
    QString dataDirectory() const { return m_dataDirectory; }
    QString cacheDirectory() const { return m_cacheDirectory; }

    static QString platformBridgeFileName();

signals:
    void readyChanged();

private:
    std::unique_ptr<RustBridgeRuntime> m_runtime;
    QString m_dataDirectory;
    QString m_cacheDirectory;
};

// A single composition-owned pool set. Each coordinator gets a dedicated lane
// so a slow catalog/media request cannot starve authentication or playback.
class WorkerPools final
{
public:
    WorkerPools();
    ~WorkerPools();

    WorkerPools(const WorkerPools &) = delete;
    WorkerPools &operator=(const WorkerPools &) = delete;

    QThreadPool &sessionControl() { return m_sessionControl; }
    QThreadPool &danmakuControl() { return m_danmakuControl; }
    QThreadPool &catalog() { return m_catalog; }
    QThreadPool &search() { return m_search; }
    QThreadPool &searchHydration() { return m_searchHydration; }
    QThreadPool &mediaRead() { return m_mediaRead; }
    QThreadPool &mediaMutation() { return m_mediaMutation; }
    QThreadPool &playbackPrepare() { return m_playbackPrepare; }
    QThreadPool &playbackReport() { return m_playbackReport; }

    void drain();

private:
    static void configure(QThreadPool &pool, int maximumThreads);

    QThreadPool m_sessionControl;
    QThreadPool m_danmakuControl;
    QThreadPool m_catalog;
    QThreadPool m_search;
    QThreadPool m_searchHydration;
    QThreadPool m_mediaRead;
    QThreadPool m_mediaMutation;
    QThreadPool m_playbackPrepare;
    QThreadPool m_playbackReport;
};

// Narrow dependency used by coordinators. Stable error codes choose localized
// user copy; transport detail remains diagnostic-only.
class StatusSink
{
public:
    virtual ~StatusSink() = default;
    virtual void publishStatus(const QString &message, bool error) = 0;
    virtual QString userFacingBackendError(
        const QString &code,
        const QString &diagnosticMessage = {}) const = 0;
    virtual QString userFacingDanmakuError(
        const QString &code,
        const QString &diagnosticMessage = {}) const = 0;
};

class ApplicationStatusService final
    : public ApplicationStatusPort
    , public StatusSink
{
    Q_OBJECT

public:
    explicit ApplicationStatusService(
        RuntimeHost &runtimeHost,
        QObject *parent = nullptr);

    bool ready() const override;
    QString message() const override { return m_message; }
    bool error() const override { return m_error; }
    void clear() override;

    void publishStatus(const QString &message, bool error) override;
    QString userFacingBackendError(
        const QString &code,
        const QString &diagnosticMessage = {}) const override;
    QString userFacingDanmakuError(
        const QString &code,
        const QString &diagnosticMessage = {}) const override;

private:
    RuntimeHost &m_runtimeHost;
    QString m_message;
    bool m_error = false;
};
