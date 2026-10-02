#include <windows.h>
#include "input_device.h"
#include "scs_input_api.h"
#include "scs_logging.h"

namespace scs_logging {

std::atomic<scs_log_t> g_log{nullptr};

void initialize(scs_log_t log) {
    g_log.store(log, std::memory_order_release);
}

void shutdown() {
    g_log.store(nullptr, std::memory_order_release);
}

void write(scs_log_type_t type, const char* message) {
    scs_log_t log = g_log.load(std::memory_order_acquire);
    if (log && message) {
        char prefixedMessage[512];
        sprintf_s(prefixedMessage, sizeof(prefixedMessage), "[ets2la_controller] %s", message);
        log(type, prefixedMessage);
    }
}

void writef(scs_log_type_t type, const char* format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
    va_end(args);
    write(type, message);
}

}

extern "C" __declspec(dllexport) scs_result_t SCSAPIFUNC scs_input_init(
    const scs_u32_t version,
    const scs_input_init_params_t* const params) {
    if (version != SCS_INPUT_VERSION_1_00 || !params) {
        return SCS_RESULT_unsupported;
    }

    const auto* versionedParams = static_cast<const scs_input_init_params_v100_t*>(params);
    scs_logging::initialize(versionedParams->common.log);

    if (!StartInputDevice(*versionedParams)) {
        scs_logging::shutdown();
        return SCS_RESULT_generic_error;
    }
    return SCS_RESULT_ok;
}

extern "C" __declspec(dllexport) void SCSAPIFUNC scs_input_shutdown() {
    StopInputDevice();
    scs_logging::shutdown();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            StopInputDevice();
            scs_logging::shutdown();
            break;
    }
    return TRUE;
}