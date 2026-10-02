#pragma once

#include <windows.h>
#include <sddl.h>

#include <atomic>
#include <cstring>
#include <string>

#include "../../include/ets2la_controller/protocol.h"

namespace ets2la_controller {

class SecurityAttrs {
public:
    SecurityAttrs() {
        std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)";
        std::wstring sid = current_user_sid();
        if (!sid.empty()) {
            sddl += L"(A;;GA;;;" + sid + L")";
        }
        sddl += L"S:(ML;;NW;;;ME)";
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd_, nullptr)) {
            sa_.nLength = sizeof(sa_);
            sa_.lpSecurityDescriptor = sd_;
            sa_.bInheritHandle = FALSE;
            ok_ = true;
        }
    }
    ~SecurityAttrs() {
        if (sd_) {
            LocalFree(sd_);
        }
    }
    SecurityAttrs(const SecurityAttrs&) = delete;
    SecurityAttrs& operator=(const SecurityAttrs&) = delete;
    SECURITY_ATTRIBUTES* get() {
        return ok_ ? &sa_ : nullptr;
    }

private:
    static std::wstring current_user_sid() {
        std::wstring result;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            return result;
        }
        DWORD needed = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        if (needed) {
            std::string buf(needed, '\0');
            if (GetTokenInformation(token, TokenUser, &buf[0], needed, &needed)) {
                auto* user = reinterpret_cast<TOKEN_USER*>(&buf[0]);
                LPWSTR str = nullptr;
                if (ConvertSidToStringSidW(user->User.Sid, &str)) {
                    result = str;
                    LocalFree(str);
                }
            }
        }
        CloseHandle(token);
        return result;
    }

    SECURITY_ATTRIBUTES sa_{};
    PSECURITY_DESCRIPTOR sd_ = nullptr;
    bool ok_ = false;
};

class SharedControls {
public:
    ~SharedControls() {
        shutdown();
    }

    const std::string& last_error() const {
        return lastError_;
    }

    ControlBlock* control() {
        return ctl_;
    }
    InputBlock* inputs() {
        return in_;
    }

    bool init() {
        if (ctl_) {
            return true;
        }

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);

        bool existed = false;
        map_ = create_mapping(&existed);
        if (!map_) {
            return fail("CreateFileMapping failed");
        }
        base_ = static_cast<uint8_t*>(MapViewOfFile(map_, FILE_MAP_WRITE, 0, 0, kTotalBytes));
        if (!base_) {
            return fail("MapViewOfFile failed");
        }
        ctl_ = reinterpret_cast<ControlBlock*>(base_);
        in_ = reinterpret_cast<InputBlock*>(base_ + kControlBytes);

        for (int i = 0; i < 2; ++i) {
            events_[i] = create_event(kEventNames[i]);
            if (!events_[i]) {
                return fail("CreateEvent failed");
            }
        }

        if (!existed || ctl_->magic != kMagic || ctl_->version != kProtocolVersion) {
            std::memset(base_, 0, kTotalBytes);
        }
        ctl_->version = kProtocolVersion;
        ctl_->input_count = kInputCount;
        ctl_->max_clients = kMaxClients;
        ctl_->qpc_frequency = uint64_t(freq.QuadPart);
        ctl_->dll_pid = GetCurrentProcessId();
        ctl_->active.store(0, std::memory_order_relaxed);
        ctl_->last_frame_qpc.store(0, std::memory_order_relaxed);
        ctl_->frame_interval_ticks.store(0, std::memory_order_relaxed);
        ctl_->magic = kMagic;

        const uint64_t latest = ctl_->frame_index.load(std::memory_order_relaxed);
        ResetEvent(events_[(latest + 1) & 1]);
        if (latest > 0) {
            SetEvent(events_[latest & 1]);
        } else {
            ResetEvent(events_[latest & 1]);
        }
        return true;
    }

    void signal_frame(uint64_t index) {
        ResetEvent(events_[(index + 1) & 1]);
        ctl_->frame_index.store(index, std::memory_order_release);
        SetEvent(events_[index & 1]);
    }

    bool has_active_clients() const {
        if (!ctl_) {
            return false;
        }
        const uint64_t now = GetTickCount64();
        for (uint32_t i = 0; i < kMaxClients; ++i) {
            const auto& c = ctl_->clients[i];
            if (c.id.load(std::memory_order_relaxed) != 0 &&
                now - c.heartbeat_ms.load(std::memory_order_relaxed) < kClientTimeoutMs) {
                return true;
            }
        }
        return false;
    }

    void shutdown() {
        if (ctl_) {
            ctl_->active.store(0, std::memory_order_relaxed);
        }
        if (base_) {
            UnmapViewOfFile(base_);
            base_ = nullptr;
            ctl_ = nullptr;
            in_ = nullptr;
        }
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

private:
    bool fail(const char* msg) {
        lastError_ = msg;
        return false;
    }

    HANDLE create_mapping(bool* existed) {
        const DWORD hi = DWORD(kTotalBytes >> 32), lo = DWORD(kTotalBytes & 0xFFFFFFFFu);
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, sa_.get(), PAGE_READWRITE, hi, lo, kMappingName);
        if (!h && sa_.get()) {
            h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, hi, lo, kMappingName);
        }
        *existed = (h != nullptr) && GetLastError() == ERROR_ALREADY_EXISTS;
        return h;
    }

    HANDLE create_event(const wchar_t* name) {
        HANDLE h = CreateEventW(sa_.get(), TRUE, FALSE, name);
        if (!h && sa_.get()) {
            h = CreateEventW(nullptr, TRUE, FALSE, name);
        }
        return h;
    }

    SecurityAttrs sa_;
    std::string lastError_;
    HANDLE map_ = nullptr;
    uint8_t* base_ = nullptr;
    ControlBlock* ctl_ = nullptr;
    InputBlock* in_ = nullptr;
    HANDLE events_[2] = {nullptr, nullptr};
};

}