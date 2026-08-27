#pragma once

#include <QString>

#include <atomic>
#include <memory>

#include <mpv/client.h>
#include <mpv/render.h>

class QLibrary;
class QMutex;

struct MpvFunctions final
{
    decltype(&::mpv_create) create = nullptr;
    decltype(&::mpv_terminate_destroy) terminateDestroy = nullptr;
    decltype(&::mpv_initialize) initialize = nullptr;
    decltype(&::mpv_set_option_string) setOptionString = nullptr;
    decltype(&::mpv_error_string) errorString = nullptr;
    decltype(&::mpv_request_log_messages) requestLogMessages = nullptr;
    decltype(&::mpv_observe_property) observeProperty = nullptr;
    decltype(&::mpv_set_wakeup_callback) setWakeupCallback = nullptr;
    decltype(&::mpv_get_property) getProperty = nullptr;
    decltype(&::mpv_free) free = nullptr;
    decltype(&::mpv_set_property) setProperty = nullptr;
    decltype(&::mpv_set_property_async) setPropertyAsync = nullptr;
    decltype(&::mpv_set_property_string) setPropertyString = nullptr;
    decltype(&::mpv_command_async) commandAsync = nullptr;
    decltype(&::mpv_wait_event) waitEvent = nullptr;
    decltype(&::mpv_render_context_create) renderContextCreate = nullptr;
    decltype(&::mpv_render_context_set_update_callback) renderContextSetUpdateCallback = nullptr;
    decltype(&::mpv_render_context_render) renderContextRender = nullptr;
    decltype(&::mpv_render_context_free) renderContextFree = nullptr;
};

// Resolves libmpv on first player construction instead of linking its import
// library into the desktop executable. Keeping the library object alive for
// the process lifetime also keeps callbacks and render-thread function
// pointers valid until every MpvVideoItem has been destroyed.
class MpvApi final
{
public:
    static MpvApi &instance();

    const MpvFunctions *load(QString *errorMessage = nullptr);
    const MpvFunctions *functions() const noexcept;
    bool isLoaded() const noexcept;
    QString loadedFileName() const;

    MpvApi(const MpvApi &) = delete;
    MpvApi &operator=(const MpvApi &) = delete;

private:
    MpvApi();
    ~MpvApi();

    std::unique_ptr<QLibrary> m_library;
    std::unique_ptr<QMutex> m_mutex;
    MpvFunctions m_functions;
    QString m_error;
    QString m_loadedFileName;
    bool m_loadAttempted = false;
    std::atomic_bool m_loaded{false};
};
