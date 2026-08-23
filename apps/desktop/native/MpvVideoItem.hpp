#pragma once

#include "PlaybackCompletionGate.hpp"
#include "PlaybackStallWatchdog.hpp"

#include <QPointer>
#include <QElapsedTimer>
#include <QQuickFramebufferObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <mpv/client.h>

#include <memory>

struct MpvRenderState;

class MpvVideoItem : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(double bufferedPosition READ bufferedPosition NOTIFY bufferedPositionChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(double rate READ rate WRITE setRate NOTIFY rateChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY seekableChanged)
    Q_PROPERTY(PlaybackState playbackState READ playbackState NOTIFY playbackStateChanged)
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList subtitleTracks READ subtitleTracks NOTIFY tracksChanged)
    Q_PROPERTY(qint64 selectedAudioTrack READ selectedAudioTrack NOTIFY tracksChanged)
    Q_PROPERTY(qint64 selectedSubtitleTrack READ selectedSubtitleTrack NOTIFY tracksChanged)

public:
    enum class PlaybackState {
        Idle,
        Loading,
        Playing,
        Paused,
        Buffering,
        Ended,
    };
    Q_ENUM(PlaybackState)

    explicit MpvVideoItem(QQuickItem *parent = nullptr);
    ~MpvVideoItem() override;

    Renderer *createRenderer() const override;

    bool paused() const { return m_paused; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    double bufferedPosition() const { return m_bufferedPosition; }
    double volume() const { return m_volume; }
    bool muted() const { return m_muted; }
    double rate() const { return m_rate; }
    bool seekable() const { return m_seekable; }
    PlaybackState playbackState() const { return m_playbackState; }
    QVariantList audioTracks() const { return m_audioTracks; }
    QVariantList subtitleTracks() const { return m_subtitleTracks; }
    qint64 selectedAudioTrack() const { return m_selectedAudioTrack; }
    qint64 selectedSubtitleTrack() const { return m_selectedSubtitleTrack; }

    Q_INVOKABLE void open(const QUrl &url, const QVariantMap &headers = {});
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void setVolume(double volume);
    Q_INVOKABLE void setMuted(bool muted);
    Q_INVOKABLE void setRate(double rate);
    Q_INVOKABLE void addSubtitle(const QUrl &url, const QString &title, bool selected = false);
    Q_INVOKABLE void selectAudioTrack(qint64 trackId);
    Q_INVOKABLE void selectSubtitleTrack(qint64 trackId);
    Q_INVOKABLE void disableSubtitles();

public slots:
    void setPaused(bool paused);

protected:
    bool event(QEvent *event) override;

signals:
    void pausedChanged();
    void positionChanged();
    void durationChanged();
    void bufferedPositionChanged();
    void volumeChanged();
    void mutedChanged();
    void rateChanged();
    void seekableChanged();
    void playbackStateChanged();
    void playbackError(const QString &message);
    // A recoverable media-read timeout. libmpv keeps retrying, and
    // playbackRecovered() is emitted once time-pos advances again.
    void playbackTimedOut(const QString &message);
    void playbackRecovered();
    void tracksChanged();
    void fileLoaded();
    void fileEnded();
    // Emitted once per load generation when libmpv reaches natural EOF.
    // With keep-open enabled this can happen without fileEnded(), because the
    // file remains loaded so the final frame can stay visible.
    void playbackCompleted();
    void seekRequested(double positionSeconds);

private slots:
    void drainEvents();

private:
    friend class MpvRenderer;

    static void wakeup(void *context);
    void setPlaybackState(PlaybackState state);
    void command(const QList<QByteArray> &arguments, quint64 replyUserdata = 0);
    void setHeaders(const QVariantMap &headers);
    void refreshTracks();
    void refreshPlaybackState();
    void updatePlaybackPauseMonitoring(bool paused);
    void pollPlaybackStall();
    void handlePlaybackStallEvent(YanamiPlayback::PlaybackStallEvent event);
    void resetPlaybackStall();
    qint64 playbackStallNow() const;

    std::shared_ptr<mpv_handle> m_mpvOwner;
    mpv_handle *m_mpv = nullptr;
    std::shared_ptr<MpvRenderState> m_renderState;
    bool m_paused = false;
    bool m_pauseRequested = false;
    // Native libmpv paused-for-cache and the synthetic no-progress watchdog
    // remain separate so either source can hold the UI in Buffering.
    bool m_buffering = false;
    bool m_watchdogBuffering = false;
    bool m_timeoutReported = false;
    bool m_fileLoaded = false;
    quint64 m_loadGeneration = 0;
    YanamiPlayback::PlaybackCompletionGate m_completionGate;
    quint64 m_bufferingTransitions = 0;
    qint64 m_totalBufferingMs = 0;
    QElapsedTimer m_loadTimer;
    QElapsedTimer m_bufferingTimer;
    QElapsedTimer m_playbackStallClock;
    QTimer m_playbackStallTimer;
    YanamiPlayback::PlaybackStallWatchdog m_playbackStallWatchdog;
    qint64 m_startupWatchStartedMs = 0;
    qint64 m_lastPlaybackStallPollMs = 0;
    double m_position = 0.0;
    double m_duration = 0.0;
    double m_bufferedPosition = 0.0;
    double m_volume = 100.0;
    bool m_muted = false;
    double m_rate = 1.0;
    bool m_seekable = false;
    PlaybackState m_playbackState = PlaybackState::Idle;
    QVariantList m_audioTracks;
    QVariantList m_subtitleTracks;
    qint64 m_selectedAudioTrack = -1;
    qint64 m_selectedSubtitleTrack = -1;
};
