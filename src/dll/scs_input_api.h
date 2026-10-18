#pragma once

#include "scs_logging.h"

using scs_context_t = void*;
using scs_value_type_t = scs_u32_t;

constexpr scs_u32_t SCS_INPUT_VERSION_1_00 = (1u << 16) | 0u;
constexpr scs_value_type_t SCS_VALUE_TYPE_bool = 1;
constexpr scs_value_type_t SCS_VALUE_TYPE_float = 5;
constexpr scs_u32_t SCS_INPUT_DEVICE_TYPE_semantical = 2;
constexpr scs_u32_t SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame = 0x00000001;
constexpr scs_u32_t SCS_INPUT_EVENT_CALLBACK_FLAG_first_after_activation = 0x00000002;

struct scs_input_device_input_t {
    scs_string_t name;
    scs_string_t display_name;
    scs_value_type_t value_type;
#if defined(_WIN64)
    scs_u32_t _padding;
#endif
};

struct scs_input_event_t {
    scs_u32_t input_index;
    union {
        struct { scs_u8_t value; } value_bool;
        struct { float value; } value_float;
        float _sizing_for_future_extensions[6];
    };
};

using scs_input_active_callback_t = void (SCSAPIFUNC*)(
    const scs_u8_t active,
    const scs_context_t context
);

using scs_input_event_callback_t = scs_result_t (SCSAPIFUNC*)(
    scs_input_event_t* const event_info,
    const scs_u32_t flags,
    const scs_context_t context
);

struct scs_input_device_t {
    scs_string_t name;
    scs_string_t display_name;
    scs_u32_t type;
    scs_u32_t input_count;
    const scs_input_device_input_t* inputs;
    scs_context_t callback_context;
    scs_input_active_callback_t input_active_callback;
    scs_input_event_callback_t input_event_callback;
};

using scs_input_register_device_t = scs_result_t (SCSAPIFUNC*)(const scs_input_device_t* const device_info);

struct scs_input_init_params_t {
    void method_indicating_this_is_not_a_c_struct(void);
};

struct scs_input_init_params_v100_t : scs_input_init_params_t {
    scs_sdk_init_params_v100_t common;
    scs_input_register_device_t register_device;
};

#if defined(_WIN64)
static_assert(sizeof(scs_input_device_input_t) == 24, "SDK ABI: scs_input_device_input_t");
static_assert(sizeof(scs_input_event_t) == 28, "SDK ABI: scs_input_event_t");
static_assert(sizeof(scs_input_device_t) == 56, "SDK ABI: scs_input_device_t");
static_assert(sizeof(scs_input_init_params_v100_t) == 40, "SDK ABI: scs_input_init_params_v100_t");
#endif