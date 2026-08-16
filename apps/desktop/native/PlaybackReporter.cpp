#include "PlaybackReporter.hpp"

#include "MpvVideoItem.hpp"
#include "PlaybackTrackMapper.hpp"

#include <QTimer>

#include <algorithm>

PlaybackReporter::PlaybackReporter(
    PlaybackPort *port,
    QObject *parent,
    PlaybackReporterTiming timing)
    : QObject(parent)
    , m_port(port)
{
    m_startDelay.setSingleShot(true);
    m_startDelay.setInterval(std::max(0, timing.startDelayMs));
    connect(&m_startDelay, &QTimer::timeout,
        this, &PlaybackReporter::startSession);
    m_heartbeat.setInterval(std::max(1, timing.heartbeatIntervalMs));
    m_heartbeat.setTimerType(Qt::CoarseTimer);
    connect(&m_heartbeat, &QTimer::timeout,
        this, [this]() { reportProgress(); });
}

PlaybackReporter::~PlaybackReporter()
{
    stopSession();
    disconnectPlayer();
}

bool PlaybackReporter::attachPlayer(QObject *player)
{
    auto *mpvPlayer = qobject_cast<MpvVideoItem *>(player);
    if (!mpvPlayer)
        return false;
    if (m_player == mpvPlayer)
        return true;

    if (m_active)
        stopSession();
    disconnectPlayer();
    m_player = mpvPlayer;

    const auto queueCurrentState = [this]() { queueProgress(); };
    m_playerConnections.append(connect(mpvPlayer,
        &MpvVideoItem::pausedChanged, this, queueCurrentState));
    m_playerConnections.append(connect(mpvPlayer,
        &MpvVideoItem::volumeChanged, this, queueCurrentState));
    m_playerConnections.append(connect(mpvPlayer,
        &MpvVideoItem::mutedChanged, this, queueCurrentState));
    m_playerConnections.append(connect(mpvPlayer,
        &MpvVideoItem::rateChanged, this, queueCurrentState));
    m_playerConnections.append(connect(mpvPlayer,
        &MpvVideoItem::seekableChanged, this, queueCurrentState));
    m_playerConnections.append(connect(mpvPlayer,
        &MpvVideoItem::tracksChanged, this,
        [this]() {
            // External subtitle insertion updates mpv's track list
            // asynchronously.  Once that reconciliation arrives we can emit
            // Started immediately; the bounded timer remains the fallback for
            // files whose track list does not change.
            if (m_active && !m_started)
                startSession();
            else
                queueProgress();
        }));
    m_playerConnections.append(connect(mpvPlayer,
        &MpvVideoItem::seekRequested, this,
        [this](double positionSeconds) {
            if (m_active && m_started)
                reportProgress(positionSeconds);
            else
                m_pendingPosition = positionSeconds;
        }));
    m_playerConnections.append(connect(mpvPlayer,
        &MpvVideoItem::fileEnded, this,
        [this]() { stopSession(); }));
    m_playerConnections.append(connect(mpvPlayer,
        &QObject::destroyed, this,
        [this]() {
            m_player = nullptr;
            stopSession();
            m_playerConnections.clear();
        }));
    return true;
}

bool PlaybackReporter::beginSession(
    const QString &reportSessionId,
    const QVariantList &embyTracks)
{
    const QString normalizedId = reportSessionId.trimmed();
    if (!m_port || !m_player || normalizedId.isEmpty())
        return false;

    if (m_active && m_reportSessionId == normalizedId) {
        m_embyTracks = embyTracks;
        return true;
    }
    if (m_active)
        stopSession();

    ++m_sessionGeneration;
    m_reportSessionId = normalizedId;
    m_embyTracks = embyTracks;
    m_active = true;
    m_started = false;
    m_startDelay.start();
    return true;
}

void PlaybackReporter::stopSession()
{
    if (!m_active)
        return;

    if (!m_started)
        startSession();
    m_startDelay.stop();
    m_heartbeat.stop();
    m_queuedProgressGeneration.reset();
    m_lastSnapshot = snapshot();
    if (m_port)
        m_port->report(PlaybackPort::Event::Stopped, m_lastSnapshot);
    m_active = false;
    m_started = false;
    m_reportSessionId.clear();
    m_embyTracks.clear();
    m_pendingPosition.reset();
    ++m_sessionGeneration;
}

void PlaybackReporter::abandonSessionForTransition()
{
    m_startDelay.stop();
    m_heartbeat.stop();
    m_queuedProgressGeneration.reset();
    m_active = false;
    m_started = false;
    m_reportSessionId.clear();
    m_embyTracks.clear();
    m_pendingPosition.reset();
    ++m_sessionGeneration;
}

void PlaybackReporter::startSession()
{
    if (!m_active || m_started || !m_port)
        return;
    m_lastSnapshot = snapshot();
    if (m_pendingPosition)
        m_lastSnapshot.positionSeconds = *m_pendingPosition;
    m_pendingPosition.reset();
    m_started = true;
    m_port->report(PlaybackPort::Event::Started, m_lastSnapshot);
    m_heartbeat.start();
}

PlaybackPort::Snapshot PlaybackReporter::snapshot() const
{
    PlaybackPort::Snapshot value = m_lastSnapshot;
    value.reportSessionId = m_reportSessionId;
    if (!m_player)
        return value;

    value.positionSeconds = m_player->position();
    value.paused = m_player->paused();
    value.muted = m_player->muted();
    value.volume = m_player->volume();
    value.rate = m_player->rate();
    value.seekable = m_player->seekable();
    value.audioStreamIndex = YanamiPlayback::selectedStreamIndex(
        m_player->audioTracks(), m_player->selectedAudioTrack(),
        m_embyTracks, QStringLiteral("audio"))
                                 .value_or(-1);
    value.subtitleStreamIndex = YanamiPlayback::selectedStreamIndex(
        m_player->subtitleTracks(), m_player->selectedSubtitleTrack(),
        m_embyTracks, QStringLiteral("subtitle"))
                                    .value_or(-1);
    return value;
}

void PlaybackReporter::reportProgress(
    std::optional<double> positionSeconds)
{
    if (!m_active || !m_started || !m_port)
        return;

    m_lastSnapshot = snapshot();
    if (positionSeconds)
        m_lastSnapshot.positionSeconds = *positionSeconds;
    m_port->report(PlaybackPort::Event::Progress, m_lastSnapshot);
}

void PlaybackReporter::queueProgress()
{
    if (!m_active
        || (m_queuedProgressGeneration.has_value()
            && *m_queuedProgressGeneration == m_sessionGeneration)) {
        return;
    }
    const quint64 generation = m_sessionGeneration;
    m_queuedProgressGeneration = generation;
    QTimer::singleShot(0, this, [this, generation]() {
        if (!m_queuedProgressGeneration.has_value()
            || *m_queuedProgressGeneration != generation) {
            return;
        }
        m_queuedProgressGeneration.reset();
        if (generation != m_sessionGeneration)
            return;
        reportProgress();
    });
}

void PlaybackReporter::disconnectPlayer()
{
    for (const QMetaObject::Connection &connection : m_playerConnections)
        disconnect(connection);
    m_playerConnections.clear();
    m_player = nullptr;
}
