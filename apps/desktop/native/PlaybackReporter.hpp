#pragma once

#include "BackendPorts.hpp"

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>

#include <optional>

class MpvVideoItem;

struct PlaybackReporterTiming
{
    int startDelayMs = 180;
    int heartbeatIntervalMs = 10'000;
};

// Owns the lifetime and cadence of Emby playback telemetry. QML only attaches
// the player and starts/stops a reporting session; it never constructs events.
class PlaybackReporter final : public QObject, public PlaybackReporterPort
{
    Q_OBJECT

public:
    explicit PlaybackReporter(
        PlaybackPort *port,
        QObject *parent = nullptr,
        PlaybackReporterTiming timing = {});
    ~PlaybackReporter() override;

    bool attachPlayer(QObject *player) override;
    bool beginSession(
        const QString &reportSessionId,
        const QVariantList &embyTracks) override;
    void stopSession() override;

    // A login/logout transition changes the Rust backend's global session.
    // Retire the current telemetry identity without sending it through a
    // transport that may already belong to another account.
    void abandonSessionForTransition();

private:
    PlaybackPort::Snapshot snapshot() const;
    void startSession();
    void reportProgress(std::optional<double> positionSeconds = std::nullopt);
    void queueProgress();
    void disconnectPlayer();

    QPointer<PlaybackPort> m_port;
    QPointer<MpvVideoItem> m_player;
    QTimer m_startDelay;
    QTimer m_heartbeat;
    QString m_reportSessionId;
    QVariantList m_embyTracks;
    PlaybackPort::Snapshot m_lastSnapshot;
    QList<QMetaObject::Connection> m_playerConnections;
    std::optional<double> m_pendingPosition;
    std::optional<quint64> m_queuedProgressGeneration;
    quint64 m_sessionGeneration = 0;
    bool m_active = false;
    bool m_started = false;
};
