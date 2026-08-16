#pragma once

#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QWindow>

class WindowController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool fullScreen READ fullScreen NOTIFY fullScreenChanged)
    Q_PROPERTY(qreal devicePixelRatio READ devicePixelRatio NOTIFY devicePixelRatioChanged)

public:
    explicit WindowController(QObject *parent = nullptr);

    bool fullScreen() const { return m_fullScreen; }
    qreal devicePixelRatio() const;
    void configureWindow(QWindow *window);
    Q_INVOKABLE void enterFullScreen(QWindow *window);
    Q_INVOKABLE void exitFullScreen();
    Q_INVOKABLE void toggleFullScreen(QWindow *window);
    Q_INVOKABLE void setCursorHidden(bool hidden);

signals:
    void fullScreenChanged();
    void devicePixelRatioChanged();

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void notePointerActivity();
    void setFullScreen(bool fullScreen);
    static void setRoundedCorners(QWindow *window, bool rounded);
    static void applyBorderlessFullScreen(QWindow *window);

    QPointer<QWindow> m_window;
    QRect m_previousGeometry;
    QWindow::Visibility m_previousVisibility = QWindow::Windowed;
    bool m_fullScreen = false;
    bool m_transitioning = false;
    bool m_cursorHidden = false;
    QTimer m_cursorIdleTimer;
};
