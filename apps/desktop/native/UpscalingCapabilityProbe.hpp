#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QSGRendererInterface>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <memory>

class QQuickWindow;

class UpscalingCapabilityProbe final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap result READ result NOTIFY resultChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY resultChanged)

public:
    explicit UpscalingCapabilityProbe(QObject *parent = nullptr);
    ~UpscalingCapabilityProbe() override;

    QVariantMap result() const { return m_result; }
    bool ready() const { return m_ready; }

    // Must be called from this object's (normally GUI) thread. The actual
    // graphics query is deferred to the observed window's scene-graph thread,
    // where Qt has made the renderer's OpenGL context current.
    Q_INVOKABLE void observe(QQuickWindow *window);

    // Pure support policy used by both the render-thread probe and unit tests.
    // Preset selection is intentionally user-controlled and is not inferred
    // from GPU identity here.
    static QVariantMap evaluate(
        QSGRendererInterface::GraphicsApi graphicsApi,
        int glMajor,
        int glMinor,
        const QString &vendor,
        const QString &renderer,
        int maximumTextureSize);

signals:
    void resultChanged();

private:
    struct ObservationFence;

    void disconnectWindow();
    void publish(const QVariantMap &result, bool ready);

    QPointer<QQuickWindow> m_window;
    std::shared_ptr<ObservationFence> m_fence;
    QVector<QMetaObject::Connection> m_connections;
    QVariantMap m_result;
    bool m_ready = false;
};
