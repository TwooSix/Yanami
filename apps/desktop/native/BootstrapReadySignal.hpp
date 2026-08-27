#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

struct BootstrapReadySignal {
    bool supplied = false;
    bool valid = true;
    std::uintptr_t nativeHandle = 0;
    QString rejectionReason;
};

BootstrapReadySignal bootstrapReadySignalFromArguments(
    const QStringList &arguments);

bool signalBootstrapReady(
    const BootstrapReadySignal &signal,
    QString *errorCode = nullptr);
