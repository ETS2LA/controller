#pragma once

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "controls.h"
#include "protocol.h"

namespace ets2la_controller {

inline uint64_t qpc_now() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return uint64_t(q.QuadPart);
}

// Snapshot of the plugin state and the latency statistics.
struct ControllerStats {
    bool active = false;                  // The game currently polls the device.
    uint32_t plugin_pid = 0;              // Process id of the game that loaded the plugin.
    uint64_t frame_index = 0;             // Game input frames processed since the plugin loaded.
    uint64_t events_sent = 0;             // Input events handed to the game since the plugin loaded.
    double frame_interval_ms = 0.0;       // Time between the last two game input frames.
    uint64_t latency_samples = 0;         // commit() calls measured since the last reset_stats().
    double latency_last_ms = 0.0;         // Delay of the last commit() until the game read it.
    double latency_avg_ms = 0.0;          // Average commit() to game delay.
    double latency_max_ms = 0.0;          // Worst commit() to game delay.
};

// Where the game stands compared to the newest commit().
struct Latency {
    bool applied = false;     // true once the game has read the newest commit().
    double ms = 0.0;          // applied: delay commit() -> game. Otherwise: time the commit() waits so far.
};

// Lock-free writer for the `ets2la_controller.dll` shared memory.
//
// Set axes and buttons with the setters, then call commit() once per control loop iteration.
// Nothing reaches the game before commit(). The game reads the values once per rendered frame,
// so the delay of a command is "time until the next game frame" and is reported by latency()/stats().
class Controller {
public:
    Controller() {
        connect();
    }
    ~Controller() {
        disconnect();
    }
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    bool connected() const {
        return ctl_ != nullptr;
    }
    const std::string& last_error() const {
        return lastError_;
    }

    // Sets an analog axis. Clamped to [-1, 1] by the plugin, NaN counts as 0.
    void set(Axis axis, float value) {
        if (!ensure_connected()) return;
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        in_->values[uint32_t(axis)].store(bits, std::memory_order_relaxed);
    }

    // Sets a digital input. `true` holds it down until you set `false` again.
    // Use this for hold inputs (horn, windows, camera movement, ...).
    void set(Button button, bool down) {
        if (!ensure_connected()) return;
        in_->values[uint32_t(button)].store(down ? 1u : 0u, std::memory_order_relaxed);
    }

    // Triggers a press input (light toggle, gear up, wiper step, ...) for exactly one game frame.
    //
    // Returns immediately, the pulse is sent on the next commit(). Pressing twice before one
    // game frame has passed still counts as a single press.
    void press(Button button) {
        if (!ensure_connected()) return;
        in_->pulses[uint32_t(button)].fetch_add(1, std::memory_order_relaxed);
    }

    float get(Axis axis) const {
        if (!ctl_) return 0.0f;
        const uint32_t bits = in_->values[uint32_t(axis)].load(std::memory_order_relaxed);
        float f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }
    bool get(Button button) const {
        return ctl_ && in_->values[uint32_t(button)].load(std::memory_order_relaxed) != 0;
    }

    // Sets every axis to 0 and releases every button.
    void release_all() {
        if (!ensure_connected()) return;
        for (uint32_t i = 0; i < kInputCount; ++i) {
            in_->values[i].store(0, std::memory_order_relaxed);
        }
    }

    // Publishes everything set since the last commit() and stamps it for the latency measurement.
    //
    // Also acts as heartbeat: without commit() or heartbeat() for 3 seconds the plugin releases all inputs.
    void commit() {
        if (!ensure_connected()) return;
        const uint64_t now = qpc_now();
        in_->write_qpc.store(now, std::memory_order_relaxed);
        in_->write_seq.fetch_add(1, std::memory_order_release);
        beat(now);
    }

    // Keeps the connection alive without writing (only needed if you pause commit() for > 1 s).
    void heartbeat() {
        if (ctl_) beat(GetTickCount64(), true);
    }

    // Blocks until the game starts its next input frame (or timeout).
    //
    // Call commit() right after it to get the lowest possible delay: your values are then used
    // in the very next frame. Returns false on timeout or while not connected.
    bool wait_frame(uint32_t timeout_ms = 1000) {
        if (!ensure_connected()) {
            Sleep(timeout_ms < 50 ? timeout_ms : 50);
            return false;
        }
        const uint64_t deadline = GetTickCount64() + timeout_ms;
        while (true) {
            if (ctl_->frame_index.load(std::memory_order_acquire) > lastFrame_) {
                lastFrame_ = ctl_->frame_index.load(std::memory_order_acquire);
                return true;
            }
            const uint64_t now = GetTickCount64();
            if (now >= deadline) return false;
            const uint64_t left = deadline - now;
            WaitForSingleObject(events_[(lastFrame_ + 1) & 1], DWORD(left < 5 ? left : 5));
            beat(now, true);
        }
    }

    // Status of the newest commit(): applied by the game yet, and how long it took / has waited.
    Latency latency() const {
        Latency l;
        if (!ctl_) return l;
        const uint64_t seq = in_->write_seq.load(std::memory_order_acquire);
        const uint64_t wrote = in_->write_qpc.load(std::memory_order_relaxed);
        if (ctl_->applied_write_seq.load(std::memory_order_acquire) >= seq) {
            l.applied = true;
            l.ms = ticks_to_ms(ctl_->latency_last_ticks.load(std::memory_order_relaxed));
        } else {
            l.ms = ticks_to_ms(qpc_now() - wrote);
        }
        return l;
    }

    ControllerStats stats() const {
        ControllerStats s;
        if (!ctl_) return s;
        s.active = ctl_->active.load(std::memory_order_relaxed) != 0;
        s.plugin_pid = ctl_->dll_pid;
        s.frame_index = ctl_->frame_index.load(std::memory_order_relaxed);
        s.events_sent = ctl_->events_sent.load(std::memory_order_relaxed);
        s.frame_interval_ms = ticks_to_ms(ctl_->frame_interval_ticks.load(std::memory_order_relaxed));
        s.latency_samples = ctl_->latency_samples.load(std::memory_order_relaxed);
        s.latency_last_ms = ticks_to_ms(ctl_->latency_last_ticks.load(std::memory_order_relaxed));
        s.latency_max_ms = ticks_to_ms(ctl_->latency_max_ticks.load(std::memory_order_relaxed));
        if (s.latency_samples) {
            s.latency_avg_ms = ticks_to_ms(ctl_->latency_sum_ticks.load(std::memory_order_relaxed)) /
                               double(s.latency_samples);
        }
        return s;
    }

    // Resets the min/avg/max latency statistics.
    void reset_stats() {
        if (ctl_) ctl_->stats_epoch.fetch_add(1, std::memory_order_relaxed);
    }

    // Choose how the plugin hands values to the game (default: DeliveryMode::Changes).
    void set_delivery_mode(DeliveryMode mode) {
        if (ctl_) ctl_->delivery_mode.store(uint32_t(mode), std::memory_order_relaxed);
    }

    double ticks_to_ms(uint64_t ticks) const {
        return freq_ ? double(ticks) * 1000.0 / double(freq_) : 0.0;
    }

    bool connect() {
        disconnect();
        map_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kMappingName);
        if (!map_) {
            return fail("controller not running (OpenFileMapping failed). Is ets2la_controller.dll loaded?");
        }
        base_ = static_cast<uint8_t*>(MapViewOfFile(map_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kTotalBytes));
        if (!base_) {
            return fail("MapViewOfFile failed");
        }
        auto* ctl = reinterpret_cast<ControlBlock*>(base_);
        if (ctl->magic != kMagic) {
            disconnect();
            return fail("controller DLL not ready");
        }
        if (ctl->version != kProtocolVersion) {
            disconnect();
            return fail("controller DLL protocol version mismatch");
        }
        for (int i = 0; i < 2; ++i) {
            events_[i] = OpenEventW(SYNCHRONIZE, FALSE, kEventNames[i]);
            if (!events_[i]) {
                disconnect();
                return fail("OpenEvent failed");
            }
        }
        ctl_ = ctl;
        in_ = reinterpret_cast<InputBlock*>(base_ + kControlBytes);
        freq_ = ctl_->qpc_frequency;
        register_client();
        lastFrame_ = ctl_->frame_index.load(std::memory_order_acquire);
        lastError_.clear();
        return true;
    }

private:
    bool fail(const char* msg) {
        lastError_ = msg;
        return false;
    }

    bool ensure_connected() {
        if (ctl_) return true;
        const DWORD now = GetTickCount();
        if (now - lastReconnectTick_ < 250) return false;
        lastReconnectTick_ = now;
        return connect();
    }

    void disconnect() {
        unregister_client();
        if (base_) {
            UnmapViewOfFile(base_);
            base_ = nullptr;
        }
        ctl_ = nullptr;
        in_ = nullptr;
        if (map_) {
            CloseHandle(map_);
            map_ = nullptr;
        }
        for (auto& e : events_) {
            if (e) {
                CloseHandle(e);
                e = nullptr;
            }
        }
    }

    void register_client() {
        static std::atomic<uint32_t> counter{0};
        myId_ = (uint64_t(GetCurrentProcessId()) << 32) | (counter.fetch_add(1) + 1);
        const uint64_t now = GetTickCount64();
        const uint32_t start = GetCurrentProcessId() % kMaxClients;
        entry_ = &ctl_->clients[start];
        for (uint32_t i = 0; i < kMaxClients; ++i) {
            ClientEntry* e = &ctl_->clients[(start + i) % kMaxClients];
            const uint64_t id = e->id.load(std::memory_order_relaxed);
            if (id == 0 || now - e->heartbeat_ms.load(std::memory_order_relaxed) > kClientTimeoutMs * 2) {
                entry_ = e;
                break;
            }
        }
        beat(now, true);
    }

    void beat(uint64_t, bool force = false) {
        if (!entry_) return;
        const DWORD tick = GetTickCount();
        if (!force && tick - lastBeatTick_ < kHeartbeatIntervalMs) return;
        lastBeatTick_ = tick;
        entry_->id.store(myId_, std::memory_order_relaxed);
        entry_->heartbeat_ms.store(GetTickCount64(), std::memory_order_release);
    }

    void unregister_client() {
        if (entry_ && ctl_ && entry_->id.load(std::memory_order_relaxed) == myId_) {
            entry_->heartbeat_ms.store(0, std::memory_order_relaxed);
            entry_->id.store(0, std::memory_order_release);
        }
        entry_ = nullptr;
    }

    std::string lastError_;
    HANDLE map_ = nullptr;
    uint8_t* base_ = nullptr;
    ControlBlock* ctl_ = nullptr;
    InputBlock* in_ = nullptr;
    HANDLE events_[2] = {nullptr, nullptr};
    ClientEntry* entry_ = nullptr;
    uint64_t myId_ = 0;
    uint64_t freq_ = 0;
    uint64_t lastFrame_ = 0;
    DWORD lastReconnectTick_ = 0;
    DWORD lastBeatTick_ = 0;
};

}