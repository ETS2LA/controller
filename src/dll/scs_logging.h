#pragma once

#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <windows.h>

using scs_u8_t = unsigned __int8;
using scs_u32_t = unsigned __int32;
using scs_s32_t = signed __int32;
using scs_string_t = const char*;
using scs_log_type_t = scs_s32_t;
using scs_result_t = scs_s32_t;

constexpr scs_result_t SCS_RESULT_ok = 0;
constexpr scs_result_t SCS_RESULT_unsupported = -1;
constexpr scs_result_t SCS_RESULT_not_found = -4;
constexpr scs_result_t SCS_RESULT_generic_error = -7;
constexpr scs_log_type_t SCS_LOG_TYPE_message = 0;
constexpr scs_log_type_t SCS_LOG_TYPE_warning = 1;
constexpr scs_log_type_t SCS_LOG_TYPE_error = 2;

#define SCSAPIFUNC __stdcall
using scs_log_t = void (SCSAPIFUNC*)(scs_log_type_t type, scs_string_t message);

struct scs_sdk_init_params_v100_t {
    scs_string_t game_name;
    scs_string_t game_id;
    scs_u32_t game_version;
#if defined(_WIN64)
    scs_u32_t _padding;
#endif
    scs_log_t log;
};

namespace scs_logging {

extern std::atomic<scs_log_t> g_log;

void initialize(scs_log_t log);
void shutdown();
void write(scs_log_type_t type, const char* message);
void writef(scs_log_type_t type, const char* format, ...);

}