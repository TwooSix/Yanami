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

enum class BootstrapHandoffParentValidation {
    Allowed,
    CanonicalPathUnavailable,
    OutsideTemporaryRoots,
};

QStringList bootstrapHandoffTemporaryRoots();

BootstrapHandoffParentValidation validateBootstrapHandoffParent(
    const QString &parentPath,
    const QStringList &temporaryRoots);

BootstrapReadySignal bootstrapReadySignalFromArguments(
    const QStringList &arguments);

bool signalBootstrapReady(
    const BootstrapReadySignal &signal,
    QString *errorCode = nullptr);
