#include "MpvApi.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLibrary>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <utility>

namespace {

Q_LOGGING_CATEGORY(mpvRuntimeLog, "yanami.playback.mpv.runtime")

template<typename Function>
bool resolveFunction(
    QLibrary &library,
    Function &function,
    const char *symbol,
    QStringList &missingSymbols)
{
    function = reinterpret_cast<Function>(library.resolve(symbol));
    if (function)
        return true;
    missingSymbols.append(QString::fromLatin1(symbol));
    return false;
}

bool resolveFunctions(
    QLibrary &library,
    MpvFunctions &functions,
    QStringList &missingSymbols)
{
    bool complete = true;
#define YANAMI_RESOLVE_MPV(member, symbol) \
    complete = resolveFunction( \
        library, functions.member, symbol, missingSymbols) && complete
    YANAMI_RESOLVE_MPV(create, "mpv_create");
    YANAMI_RESOLVE_MPV(terminateDestroy, "mpv_terminate_destroy");
    YANAMI_RESOLVE_MPV(initialize, "mpv_initialize");
    YANAMI_RESOLVE_MPV(setOptionString, "mpv_set_option_string");
    YANAMI_RESOLVE_MPV(errorString, "mpv_error_string");
    YANAMI_RESOLVE_MPV(requestLogMessages, "mpv_request_log_messages");
    YANAMI_RESOLVE_MPV(observeProperty, "mpv_observe_property");
    YANAMI_RESOLVE_MPV(setWakeupCallback, "mpv_set_wakeup_callback");
    YANAMI_RESOLVE_MPV(getProperty, "mpv_get_property");
    YANAMI_RESOLVE_MPV(free, "mpv_free");
    YANAMI_RESOLVE_MPV(setProperty, "mpv_set_property");
    YANAMI_RESOLVE_MPV(setPropertyAsync, "mpv_set_property_async");
    YANAMI_RESOLVE_MPV(setPropertyString, "mpv_set_property_string");
    YANAMI_RESOLVE_MPV(commandAsync, "mpv_command_async");
    YANAMI_RESOLVE_MPV(waitEvent, "mpv_wait_event");
    YANAMI_RESOLVE_MPV(renderContextCreate, "mpv_render_context_create");
    YANAMI_RESOLVE_MPV(
        renderContextSetUpdateCallback,
        "mpv_render_context_set_update_callback");
    YANAMI_RESOLVE_MPV(renderContextRender, "mpv_render_context_render");
    YANAMI_RESOLVE_MPV(renderContextFree, "mpv_render_context_free");
#undef YANAMI_RESOLVE_MPV
    return complete;
}

QStringList libraryCandidates()
{
#ifdef YANAMI_MPV_RUNTIME_TEST_MISSING
    return {
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
            QStringLiteral("__yanami_missing_libmpv_runtime__"))
    };
#else
    QStringList names;
#ifdef YANAMI_MPV_RUNTIME_FILENAME
    names.append(QStringLiteral(YANAMI_MPV_RUNTIME_FILENAME));
#endif
#ifdef Q_OS_WIN
    names.append(QStringLiteral("libmpv-2.dll"));
    names.append(QStringLiteral("mpv-2.dll"));
#elif defined(Q_OS_MACOS)
    names.append(QStringLiteral("libmpv.2.dylib"));
    names.append(QStringLiteral("libmpv.dylib"));
#else
    names.append(QStringLiteral("libmpv.so.2"));
    names.append(QStringLiteral("libmpv.so"));
#endif
    names.removeDuplicates();

    QStringList candidates;
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    for (const QString &name : std::as_const(names)) {
#ifdef Q_OS_MACOS
        if (!applicationDirectory.isEmpty()) {
            candidates.append(QDir(applicationDirectory).absoluteFilePath(
                QStringLiteral("../Frameworks/") + name));
        }
#endif
        if (!applicationDirectory.isEmpty())
            candidates.append(QDir(applicationDirectory).absoluteFilePath(name));
    }
#ifdef Q_OS_MACOS
#ifdef YANAMI_MPV_DEVELOPMENT_RUNTIME_PATH
    // Installed bundles resolve their private Frameworks copy first. Build-tree
    // executables and tests need the exact library discovered by CMake because
    // dyld does not search Homebrew prefixes by default.
    candidates.append(
        QStringLiteral(YANAMI_MPV_DEVELOPMENT_RUNTIME_PATH));
#endif
#endif
#ifdef Q_OS_MACOS
    // Let QLibrary add the platform prefix and suffix for loader-path lookup.
    candidates.append(QStringLiteral("mpv"));
#elif !defined(Q_OS_WIN)
    // Unix packages normally provide libmpv through the system loader.
    candidates.append(names);
#endif
    candidates.removeDuplicates();
    return candidates;
#endif
}

} // namespace

MpvApi &MpvApi::instance()
{
    static MpvApi api;
    return api;
}

MpvApi::MpvApi()
    : m_mutex(std::make_unique<QMutex>())
{
}

MpvApi::~MpvApi() = default;

const MpvFunctions *MpvApi::load(QString *errorMessage)
{
    if (const MpvFunctions *loaded = functions())
        return loaded;

    const QMutexLocker locker(m_mutex.get());
    if (m_loaded.load(std::memory_order_acquire))
        return &m_functions;
    if (m_loadAttempted) {
        if (errorMessage)
            *errorMessage = m_error;
        return nullptr;
    }
    m_loadAttempted = true;

    QElapsedTimer timer;
    timer.start();
    QStringList failures;
    for (const QString &candidate : libraryCandidates()) {
        auto library = std::make_unique<QLibrary>(candidate);
        if (!library->load()) {
            failures.append(QStringLiteral("%1: %2")
                                .arg(QDir::toNativeSeparators(candidate),
                                     library->errorString()));
            continue;
        }

        MpvFunctions resolved;
        QStringList missingSymbols;
        if (!resolveFunctions(*library, resolved, missingSymbols)) {
            failures.append(QStringLiteral("%1: missing symbols %2")
                                .arg(QDir::toNativeSeparators(candidate),
                                     missingSymbols.join(QStringLiteral(", "))));
            library->unload();
            continue;
        }

        const QFileInfo loadedFile(library->fileName());
        // QLibrary preserves a bare name when the Unix system loader resolves
        // it through its configured search paths. Do not reinterpret that
        // identifier as a nonexistent file in the process working directory.
        m_loadedFileName = loadedFile.isAbsolute()
            ? loadedFile.absoluteFilePath()
            : library->fileName();
        m_functions = resolved;
        m_library = std::move(library);
        m_loaded.store(true, std::memory_order_release);
        qCInfo(mpvRuntimeLog).noquote()
            << "mpv_runtime_loaded"
            << "path=" << QDir::toNativeSeparators(m_loadedFileName)
            << "elapsedMs=" << timer.elapsed();
        return &m_functions;
    }

    m_error = QStringLiteral("Unable to load the packaged libmpv runtime. %1")
                  .arg(failures.join(QStringLiteral(" | ")));
    qCCritical(mpvRuntimeLog).noquote() << "mpv_runtime_load_failed" << m_error;
    if (errorMessage)
        *errorMessage = m_error;
    return nullptr;
}

const MpvFunctions *MpvApi::functions() const noexcept
{
    return m_loaded.load(std::memory_order_acquire) ? &m_functions : nullptr;
}

bool MpvApi::isLoaded() const noexcept
{
    return m_loaded.load(std::memory_order_acquire);
}

QString MpvApi::loadedFileName() const
{
    const QMutexLocker locker(m_mutex.get());
    return m_loadedFileName;
}
