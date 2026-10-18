#include "input_device.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "shared_memory.h"

namespace ets2la_controller {
namespace {

constexpr const char* kDeviceName = "ets2la_controller";
constexpr const char* kDeviceDisplayName = "ETS2LA Controller";

inline uint64_t qpc_now() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return uint64_t(q.QuadPart);
}

inline uint32_t axis_bits(uint32_t raw) {
    float f;
    std::memcpy(&f, &raw, sizeof(f));
    if (!(f == f)) {
        f = 0.0f;
    } else if (f > 1.0f) {
        f = 1.0f;
    } else if (f < -1.0f) {
        f = -1.0f;
    } else if (f == 0.0f) {
        f = 0.0f;
    }
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

class InputDevice {
public:
    bool start(const scs_input_init_params_v100_t& params) {
        if (!shared_.init()) {
            scs_logging::writef(SCS_LOG_TYPE_error, "shared memory failed: %s", shared_.last_error().c_str());
            return false;
        }
        ctl_ = shared_.control();
        in_ = shared_.inputs();

        for (uint32_t i = 0; i < kInputCount; ++i) {
            const ControlInfo& info = kControls[i];
            std::snprintf(displayNames_[i], sizeof(displayNames_[i]), "ETS2LA %s", info.scs_name);
            inputs_[i].name = info.scs_name;
            inputs_[i].display_name = displayNames_[i];
            inputs_[i].value_type = info.is_axis ? SCS_VALUE_TYPE_float : SCS_VALUE_TYPE_bool;
#if defined(_WIN64)
            inputs_[i]._padding = 0;
#endif
            pulseSeen_[i] = in_->pulses[i].load(std::memory_order_relaxed);
            sent_[i] = 0;
        }

        scs_input_device_t device{};
        device.name = kDeviceName;
        device.display_name = kDeviceDisplayName;
        device.type = SCS_INPUT_DEVICE_TYPE_semantical;
        device.input_count = kInputCount;
        device.inputs = inputs_;
        device.callback_context = this;
        device.input_active_callback = &InputDevice::on_active;
        device.input_event_callback = &InputDevice::on_event;

        if (params.register_device(&device) != SCS_RESULT_ok) {
            scs_logging::write(SCS_LOG_TYPE_error, "unable to register the input device");
            shared_.shutdown();
            ctl_ = nullptr;
            in_ = nullptr;
            return false;
        }
        scs_logging::writef(SCS_LOG_TYPE_message, "registered %u inputs (%u axes), protocol v%u",
                            kInputCount, kAxisCount, kProtocolVersion);
        return true;
    }

    void stop() {
        shared_.shutdown();
        ctl_ = nullptr;
        in_ = nullptr;
        pendingCount_ = cursor_ = 0;
    }

private:
    static void SCSAPIFUNC on_active(const scs_u8_t active, const scs_context_t context) {
        auto* self = static_cast<InputDevice*>(context);
        if (self->ctl_) {
            self->ctl_->active.store(active ? 1u : 0u, std::memory_order_relaxed);
        }
    }

    static scs_result_t SCSAPIFUNC on_event(scs_input_event_t* const event, const scs_u32_t flags,
                                            const scs_context_t context) {
        auto* self = static_cast<InputDevice*>(context);
        if (!self->ctl_) {
            return SCS_RESULT_not_found;
        }
        if (flags & SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame) {
            self->begin_frame((flags & SCS_INPUT_EVENT_CALLBACK_FLAG_first_after_activation) != 0);
        }
        if (self->cursor_ >= self->pendingCount_) {
            return SCS_RESULT_not_found;
        }
        const uint32_t index = self->pendingIndex_[self->cursor_];
        const uint32_t value = self->pendingValue_[self->cursor_];
        ++self->cursor_;

        event->input_index = index;
        if (kControls[index].is_axis) {
            std::memcpy(&event->value_float.value, &value, sizeof(float));
        } else {
            event->value_bool.value = value ? 1 : 0;
        }
        return SCS_RESULT_ok;
    }

    void begin_frame(bool afterActivation) {
        const uint64_t now = qpc_now();

        const uint64_t previous = ctl_->last_frame_qpc.load(std::memory_order_relaxed);
        if (previous != 0) {
            ctl_->frame_interval_ticks.store(now - previous, std::memory_order_relaxed);
        }
        ctl_->last_frame_qpc.store(now, std::memory_order_relaxed);

        record_latency(now);

        const bool clients = shared_.has_active_clients();
        const bool everyFrame =
            ctl_->delivery_mode.load(std::memory_order_relaxed) == uint32_t(DeliveryMode::EveryFrame);
        if (afterActivation) {
            std::memset(sent_, 0, sizeof(sent_));
        }
        const bool sendAll = everyFrame;

        uint32_t count = 0;
        for (uint32_t i = 0; i < kInputCount; ++i) {
            uint32_t target = 0;
            const uint32_t raw = in_->values[i].load(std::memory_order_relaxed);
            if (kControls[i].is_axis) {
                if (clients) {
                    target = axis_bits(raw);
                }
            } else {
                const uint32_t pulses = in_->pulses[i].load(std::memory_order_relaxed);
                const bool pulsed = pulses != pulseSeen_[i];
                pulseSeen_[i] = pulses;
                target = (clients && (raw != 0 || pulsed)) ? 1u : 0u;
            }
            if (sendAll || target != sent_[i]) {
                sent_[i] = target;
                pendingIndex_[count] = uint16_t(i);
                pendingValue_[count] = target;
                ++count;
            }
        }
        pendingCount_ = count;
        cursor_ = 0;
        ctl_->events_sent.fetch_add(count, std::memory_order_relaxed);

        if (clients) {
            shared_.signal_frame(ctl_->frame_index.load(std::memory_order_relaxed) + 1);
        }
    }

    void record_latency(uint64_t now) {
        const uint32_t epoch = ctl_->stats_epoch.load(std::memory_order_relaxed);
        if (epoch != ctl_->applied_stats_epoch.load(std::memory_order_relaxed)) {
            ctl_->latency_max_ticks.store(0, std::memory_order_relaxed);
            ctl_->latency_sum_ticks.store(0, std::memory_order_relaxed);
            ctl_->latency_samples.store(0, std::memory_order_relaxed);
            ctl_->applied_stats_epoch.store(epoch, std::memory_order_relaxed);
        }

        const uint64_t seq = in_->write_seq.load(std::memory_order_acquire);
        if (seq == appliedSeq_) {
            return;
        }
        appliedSeq_ = seq;
        const uint64_t written = in_->write_qpc.load(std::memory_order_relaxed);
        const uint64_t ticks = now > written ? now - written : 0;

        ctl_->latency_last_ticks.store(ticks, std::memory_order_relaxed);
        if (ticks > ctl_->latency_max_ticks.load(std::memory_order_relaxed)) {
            ctl_->latency_max_ticks.store(ticks, std::memory_order_relaxed);
        }
        ctl_->latency_sum_ticks.store(ctl_->latency_sum_ticks.load(std::memory_order_relaxed) + ticks,
                                      std::memory_order_relaxed);
        ctl_->latency_samples.store(ctl_->latency_samples.load(std::memory_order_relaxed) + 1,
                                    std::memory_order_relaxed);
        ctl_->applied_qpc.store(now, std::memory_order_relaxed);
        ctl_->applied_write_seq.store(seq, std::memory_order_release);
    }

    SharedControls shared_;
    ControlBlock* ctl_ = nullptr;
    InputBlock* in_ = nullptr;

    scs_input_device_input_t inputs_[kInputCount];
    char displayNames_[kInputCount][48];

    uint32_t sent_[kInputCount];
    uint32_t pulseSeen_[kInputCount];
    uint16_t pendingIndex_[kInputCount];
    uint32_t pendingValue_[kInputCount];
    uint32_t pendingCount_ = 0;
    uint32_t cursor_ = 0;
    uint64_t appliedSeq_ = 0;
};

InputDevice g_device;

}
}

bool StartInputDevice(const scs_input_init_params_v100_t& params) {
    return ets2la_controller::g_device.start(params);
}

void StopInputDevice() {
    ets2la_controller::g_device.stop();
}