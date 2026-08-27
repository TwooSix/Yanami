#include "BootstrapReadySignal.hpp"

#include <QStringView>

#include <limits>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr auto BootstrapReadyHandleOption =
    "--yanami-bootstrap-ready-handle";
}

BootstrapReadySignal bootstrapReadySignalFromArguments(
    const QStringList &arguments)
{
    BootstrapReadySignal signal;
    const QString option = QString::fromLatin1(BootstrapReadyHandleOption);
    const QString prefix = option + QLatin1Char('=');
    QStringList values;
    for (const QString &argument : arguments) {
        if (argument == option) {
            signal.supplied = true;
            values.push_back(QString());
        } else if (argument.startsWith(prefix)) {
            signal.supplied = true;
            values.push_back(argument.mid(prefix.size()));
        }
    }
    if (!signal.supplied)
        return signal;
    if (values.size() != 1) {
        signal.valid = false;
        signal.rejectionReason = QStringLiteral("ready_handle_option_count");
        return signal;
    }

#ifdef Q_OS_WIN
    bool converted = false;
    const qulonglong encoded = values.constFirst().toULongLong(
        &converted, 10);
    if (!converted || encoded == 0
        || encoded > std::numeric_limits<std::uintptr_t>::max()) {
        signal.valid = false;
        signal.rejectionReason = QStringLiteral("ready_handle_invalid");
        return signal;
    }
    signal.nativeHandle = static_cast<std::uintptr_t>(encoded);
    DWORD flags = 0;
    if (!GetHandleInformation(
            reinterpret_cast<HANDLE>(signal.nativeHandle), &flags)) {
        signal.valid = false;
        signal.rejectionReason = QStringLiteral("ready_handle_not_inherited");
    }
#else
    signal.valid = false;
    signal.rejectionReason = QStringLiteral("ready_handle_unsupported_platform");
#endif
    return signal;
}

bool signalBootstrapReady(
    const BootstrapReadySignal &signal,
    QString *errorCode)
{
    if (!signal.supplied)
        return true;
    if (!signal.valid || signal.nativeHandle == 0) {
        if (errorCode)
            *errorCode = signal.rejectionReason.isEmpty()
                ? QStringLiteral("ready_handle_invalid")
                : signal.rejectionReason;
        return false;
    }
#ifdef Q_OS_WIN
    if (!SetEvent(reinterpret_cast<HANDLE>(signal.nativeHandle))) {
        if (errorCode)
            *errorCode = QStringLiteral("ready_event_signal_failed");
        return false;
    }
    return true;
#else
    if (errorCode)
        *errorCode = QStringLiteral("ready_handle_unsupported_platform");
    return false;
#endif
}
