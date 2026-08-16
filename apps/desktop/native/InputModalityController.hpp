#pragma once

#include <QObject>
#include <QPoint>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

class QEvent;
class ControllerNavigationSource;
class InputModalityControllerTests;
class InputModalityService;

class InputModalityController final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(InputModality)
    QML_SINGLETON
    Q_PROPERTY(Modality modality READ modality NOTIFY modalityChanged FINAL)
    Q_PROPERTY(bool focusNavigationActive READ focusNavigationActive
                   NOTIFY modalityChanged FINAL)

public:
    enum class Modality {
        Pointer,
        Keyboard,
        Controller,
    };
    Q_ENUM(Modality)

    explicit InputModalityController(QObject *parent = nullptr);
    ~InputModalityController() override;

    Q_DISABLE_COPY_MOVE(InputModalityController)

    [[nodiscard]] Modality modality() const;
    [[nodiscard]] bool focusNavigationActive() const;

    Q_INVOKABLE void notePointerInput();
    Q_INVOKABLE void noteKeyboardNavigation();
    Q_INVOKABLE void noteControllerNavigation();

signals:
    void modalityChanged();

private:
    friend class InputModalityControllerTests;

    void dispatchControllerNavigationKeyTo(QObject *target, int key);
};

class InputModalityService final : public QObject
{
    Q_OBJECT

public:
    static InputModalityService &instance();
    ~InputModalityService() override;

    Q_DISABLE_COPY_MOVE(InputModalityService)

    [[nodiscard]] InputModalityController::Modality modality() const;
    [[nodiscard]] bool focusNavigationActive() const;

    void notePointerInput();
    void noteKeyboardNavigation();
    void noteControllerNavigation();
    void dispatchControllerNavigationKeyTo(QObject *target, int key);

signals:
    void modalityChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit InputModalityService(QObject *parent = nullptr);

    static bool isModifierOnlyKey(int key);
    void dispatchControllerNavigationKey(int key);
    void notePointerPosition(const QPoint &globalPosition);
    void setModality(InputModalityController::Modality modality);

    std::unique_ptr<ControllerNavigationSource> m_controllerNavigation;
    InputModalityController::Modality m_modality =
        InputModalityController::Modality::Pointer;
    QPoint m_lastPointerPosition;
    bool m_pointerPositionKnown = false;
    bool m_dispatchingControllerKey = false;
};
