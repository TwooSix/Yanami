#pragma once

#include "BackendPorts.hpp"

#include <QObject>

#include <memory>

// Process-local composition root for the desktop backend. Concrete runtime,
// executor and feature coordinator types are intentionally hidden so neither
// QML nor presentation view models can depend on infrastructure details.
class DesktopBackendServices final : public QObject
{
public:
    explicit DesktopBackendServices(QObject *parent = nullptr);
    ~DesktopBackendServices() override;

    BackendPortSet portSet() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
