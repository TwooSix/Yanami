#pragma once

#include <QPointer>
#include <QQuickFramebufferObject>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <mpv/client.h>

#include <atomic>

class MpvVideoItem : public QQuickFramebufferObject
{
    Q_OBJECT
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(double bufferedPosition READ bufferedPosition NOTIFY bufferedPositionChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(QString playbackState READ playbackState NOTIFY playbackStateChanged)
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList subtitleTracks READ subtitleTracks NOTIFY tracksChanged)
    Q_PROPERTY(qint64 selectedAudioTrack READ selectedAudioTrack NOTIFY tracksChanged)
    Q_PROPERTY(qint64 selectedSubtitleTrack READ selectedSubtitleTrack NOTIFY tracksChanged)

public:
    explicit MpvVideoItem(QQuickItem *parent = nullptr);
    ~MpvVideoItem() override;

    Renderer *createRenderer() const override;

    bool paused() const { return m_paused; }
    double position() const { return m_position; }
    double duration() const { return m_duration; }
    double bufferedPosition() const { return m_bufferedPosition; }
    double volume() const { return m_volume; }
    QString playbackState() const { return m_playbackState; }
    QVariantList audioTracks() const { return m_audioTracks; }
    QVariantList subtitleTracks() const { return m_subtitleTracks; }
    qint64 selectedAudioTrack() const { return m_selectedAudioTrack; }
    qint64 selectedSubtitleTrack() const { return m_selectedSubtitleTrack; }

    Q_INVOKABLE void open(const QUrl &url, const QVariantMap &headers = {});
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(double seconds);
    Q_INVOKABLE void setVolume(double volume);
    Q_INVOKABLE void setRate(double rate);
    Q_INVOKABLE void addSubtitle(const QUrl &url, const QString &title, bool selected = false);
    Q_INVOKABLE void setDanmakuFile(const QUrl &url);
    Q_INVOKABLE void setDanmakuVisible(bool visible);
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
    void playbackStateChanged();
    void playbackError(const QString &message);
    void tracksChanged();
    void fileLoaded();
    void fileEnded();

private slots:
    void drainEvents();
    void selectDanmakuTrack();

private:
    friend class MpvRenderer;

    static void wakeup(void *context);
    void setPlaybackState(const QString &state);
    void command(const QList<QByteArray> &arguments);
    void setHeaders(const QVariantMap &headers);
    void refreshTracks();

    mpv_handle *m_mpv = nullptr;
    bool m_paused = false;
    bool m_buffering = false;
    bool m_fileLoaded = false;
    double m_position = 0.0;
    double m_duration = 0.0;
    double m_bufferedPosition = 0.0;
    double m_volume = 100.0;
    QString m_playbackState = QStringLiteral("idle");
    QString m_pendingDanmaku;
    QVariantList m_audioTracks;
    QVariantList m_subtitleTracks;
    qint64 m_selectedAudioTrack = -1;
    qint64 m_selectedSubtitleTrack = -1;
    std::atomic_uint64_t m_renderCallbackCount{0};
    std::atomic_uint64_t m_renderCount{0};
    std::atomic_uint64_t m_renderTotalNanoseconds{0};
    std::atomic_uint64_t m_renderMaximumNanoseconds{0};
};
