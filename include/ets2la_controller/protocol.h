#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "controls.h"

namespace ets2la_controller {

constexpr uint32_t kMagic = 0x434C5445;      // "ETLC"

// Bump whenever ControlBlock/InputBlock layout or semantics change in any way.
constexpr uint32_t kProtocolVersion = 1;

constexpr uint32_t kMaxInputs = 400;         // capacity of the value tables
constexpr uint32_t kMaxClients = 32;         // heartbeat table size
constexpr uint32_t kClientTimeoutMs = 3000;  // client considered gone after this, inputs are then released
constexpr uint32_t kHeartbeatIntervalMs = 250;

constexpr uint64_t kControlBytes = 4096;
constexpr uint64_t kInputBytes = 4096;
constexpr uint64_t kTotalBytes = kControlBytes + kInputBytes;

inline constexpr const wchar_t* kMappingName = L"Local\\ETS2LA_Controller";
inline constexpr const wchar_t* kEventNames[2] = {
    L"Local\\ETS2LA_Controller_Frame0",
    L"Local\\ETS2LA_Controller_Frame1",
};

enum class DeliveryMode : uint32_t {
    Changes = 0,
    EveryFrame = 1,
};

struct ClientEntry {                            // 64 bytes, one cache line per client
    std::atomic<uint64_t> id;                   // 0 = free, else (pid << 32) | counter
    std::atomic<uint64_t> heartbeat_ms;         // GetTickCount64() at last sign of life
    uint8_t pad[48];
};

struct ControlBlock {
    // cache line 0: written once by the DLL
    uint32_t magic;                             //   0
    uint32_t version;                           //   4
    uint32_t input_count;                       //   8
    uint32_t max_clients;                       //  12
    uint64_t qpc_frequency;                     //  16
    uint32_t dll_pid;                           //  24
    uint32_t pad0;                              //  28
    uint8_t pad1[32];                           //  32
    // cache line 1: written by clients, read by the DLL
    std::atomic<uint32_t> delivery_mode;        //  64  DeliveryMode
    std::atomic<uint32_t> stats_epoch;          //  68  increment to reset the latency statistics
    uint8_t pad2[56];                           //  72
    // cache line 2: per frame status, written by the DLL
    std::atomic<uint32_t> active;               // 128  1 while the game asks the device for events
    uint32_t pad3;                              // 132
    std::atomic<uint64_t> frame_index;          // 136  game input frames processed, 0 = none yet
    std::atomic<uint64_t> events_sent;          // 144  events handed to the game since load
    std::atomic<uint64_t> last_frame_qpc;       // 152  QPC at the start of the last game input frame
    std::atomic<uint64_t> frame_interval_ticks; // 160 QPC ticks between the last two game input frames
    std::atomic<uint64_t> applied_write_seq;    // 168  newest commit() sequence the game has seen
    std::atomic<uint64_t> applied_qpc;          // 176  QPC when applied_write_seq was picked up
    uint8_t pad4[8];                            // 184
    // cache line 3: commit() -> game latency statistics, written by the DLL
    std::atomic<uint64_t> latency_last_ticks;   // 192
    std::atomic<uint64_t> latency_max_ticks;    // 200
    std::atomic<uint64_t> latency_sum_ticks;    // 208
    std::atomic<uint64_t> latency_samples;      // 216
    std::atomic<uint32_t> applied_stats_epoch;  // 224 stats_epoch the DLL last honoured
    uint8_t pad5[28];                           // 228
    ClientEntry clients[kMaxClients];           // 256
};

struct InputBlock {
    std::atomic<uint64_t> write_qpc;            //    0  QPC of the newest commit()
    std::atomic<uint64_t> write_seq;            //    8  number of commit() calls
    uint8_t pad[48];                            //   16
    std::atomic<uint32_t> values[kMaxInputs];   //   64
    std::atomic<uint32_t> pulses[kMaxInputs];   // 1664
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "need lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "need lock-free 32-bit atomics");
static_assert(sizeof(ClientEntry) == 64, "ClientEntry layout");
static_assert(offsetof(ControlBlock, version) == 4, "ControlBlock layout");
static_assert(offsetof(ControlBlock, input_count) == 8, "ControlBlock layout");
static_assert(offsetof(ControlBlock, max_clients) == 12, "ControlBlock layout");
static_assert(offsetof(ControlBlock, qpc_frequency) == 16, "ControlBlock layout");
static_assert(offsetof(ControlBlock, dll_pid) == 24, "ControlBlock layout");
static_assert(offsetof(ControlBlock, delivery_mode) == 64, "ControlBlock layout");
static_assert(offsetof(ControlBlock, stats_epoch) == 68, "ControlBlock layout");
static_assert(offsetof(ControlBlock, active) == 128, "ControlBlock layout");
static_assert(offsetof(ControlBlock, frame_index) == 136, "ControlBlock layout");
static_assert(offsetof(ControlBlock, events_sent) == 144, "ControlBlock layout");
static_assert(offsetof(ControlBlock, last_frame_qpc) == 152, "ControlBlock layout");
static_assert(offsetof(ControlBlock, frame_interval_ticks) == 160, "ControlBlock layout");
static_assert(offsetof(ControlBlock, applied_write_seq) == 168, "ControlBlock layout");
static_assert(offsetof(ControlBlock, applied_qpc) == 176, "ControlBlock layout");
static_assert(offsetof(ControlBlock, latency_last_ticks) == 192, "ControlBlock layout");
static_assert(offsetof(ControlBlock, latency_max_ticks) == 200, "ControlBlock layout");
static_assert(offsetof(ControlBlock, latency_sum_ticks) == 208, "ControlBlock layout");
static_assert(offsetof(ControlBlock, latency_samples) == 216, "ControlBlock layout");
static_assert(offsetof(ControlBlock, applied_stats_epoch) == 224, "ControlBlock layout");
static_assert(offsetof(ControlBlock, clients) == 256, "ControlBlock layout");
static_assert(sizeof(ControlBlock) <= kControlBytes, "ControlBlock too large");
static_assert(offsetof(InputBlock, write_seq) == 8, "InputBlock layout");
static_assert(offsetof(InputBlock, values) == 64, "InputBlock layout");
static_assert(offsetof(InputBlock, pulses) == 64 + 4 * kMaxInputs, "InputBlock layout");
static_assert(sizeof(InputBlock) <= kInputBytes, "InputBlock too large");
static_assert(kInputCount <= kMaxInputs, "too many inputs for the value tables");

}