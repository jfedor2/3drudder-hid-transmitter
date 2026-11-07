// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "rudder.h"

#include <hidsdi.h>
#include <setupapi.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "sender.h"
#include "util.h"

const char* const RUDDER_STATUS_NAMES[RUDDER_STATUS_COUNT] = {
    "No status",
    "Take your feet off and keep it still",
    "Initializing",
    "Put your feet on",
    "Put your second foot on",
    "Stay still",
    "In use",
    "In use (extended mode)",
};

const char* rudder_status_name(int status) {
    if (status >= 0 && status < RUDDER_STATUS_COUNT) {
        return RUDDER_STATUS_NAMES[status];
    }
    return "Unknown status";
}

template <typename T>
static T read_le(const uint8_t* p) {
    T v;
    memcpy(&v, p, sizeof(v));  // the device and x86 are both little-endian
    return v;
}

bool parse_rudder_frame(const uint8_t* r, size_t len, RudderFrame& f) {
    if (len < 50 || r[0] != 1 || r[2] != (uint8_t) RudderMessage::Data) {
        return false;
    }
    f.counter = r[3];
    f.status = r[4];
    for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
        f.axes[a] = read_le<float>(r + 6 + a * 4);
        if (!std::isfinite(f.axes[a])) {
            f.axes[a] = 0.0f;
        }
    }
    f.unknown = read_le<float>(r + 22);
    for (int i = 0; i < RUDDER_SENSOR_COUNT; i++) {
        f.sensors[i] = read_le<uint16_t>(r + 26 + i * 2);
    }
    for (int i = 0; i < 3; i++) {
        f.accel[i] = read_le<int16_t>(r + 38 + i * 2);
        f.gyro[i] = read_le<int16_t>(r + 44 + i * 2);
    }
    return true;
}

bool parse_rudder_text(const uint8_t* r, size_t len, std::string& text) {
    if (len < 5 || r[0] != 1 || r[2] != (uint8_t) RudderMessage::Text) {
        return false;
    }
    text.clear();
    for (size_t i = 4; i < len && r[i] != 0; i++) {
        if (r[i] >= 0x20 && r[i] < 0x7f) {
            text += (char) r[i];
        }
    }
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return true;
}

// ---------------------------------------------------------------------------

void RudderMapper::reset() {
    memset(buttons_, 0, sizeof(buttons_));
    clear_neutral();
    was_in_use_ = false;
    last_ = RudderFrame();
}

bool RudderMapper::recenter() {
    if (!rudder_in_use(last_.status)) {
        return false;
    }
    for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
        neutral_[a] = last_.axes[a];
    }
    recentered_ = true;
    return true;
}

void RudderMapper::clear_neutral() {
    memset(neutral_, 0, sizeof(neutral_));
    recentered_ = false;
}

float RudderMapper::relative(const RudderFrame& frame, int a) const {
    float v = frame.axes[a] - neutral_[a];
    if (a == (int) RudderAxis::Rotation) {
        v = std::remainder(v, 360.0f);
    }
    return v;
}

void RudderMapper::map(const RudderFrame& frame, const MappingSettings& settings, GamepadState& state,
    float* levels) {
    last_ = frame;
    state = GamepadState();
    const bool in_use = rudder_in_use(frame.status);
    if (in_use && !was_in_use_) {
        // The device zeroes itself when the feet go on, so an old neutral
        // no longer means anything.
        clear_neutral();
    }
    was_in_use_ = in_use;
    if (!in_use) {
        memset(buttons_, 0, sizeof(buttons_));
        if (levels) {
            for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
                levels[a] = 0.0f;
            }
        }
        return;
    }
    for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
        const AxisMapping& m = settings.axes[a];
        const float v = relative(frame, a);
        const float level = axis_level(m, v);
        apply_axis(m, axis_output(m, v), state);
        buttons_[a][0] = button_pressed(buttons_[a][0], level, m.button_level);
        buttons_[a][1] = button_pressed(buttons_[a][1], -level, m.button_level);
        if (buttons_[a][0] && m.button_pos != NO_BUTTON) {
            state.buttons |= button_mask((Button) m.button_pos);
        }
        if (buttons_[a][1] && m.button_neg != NO_BUTTON) {
            state.buttons |= button_mask((Button) m.button_neg);
        }
        if (levels) {
            levels[a] = level;
        }
    }
    clamp_state(state);
}

// ---------------------------------------------------------------------------

static constexpr uint64_t RECONNECT_MS = 1000;
// Without data messages for this long, the output goes neutral.
static constexpr uint64_t STALE_MS = 500;
static constexpr size_t LOG_LINES = 100;

RudderReader::~RudderReader() {
    stop();
}

void RudderReader::start(Sender* sender) {
    sender_ = sender;
    quit_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread(&RudderReader::thread_main, this);
}

void RudderReader::stop() {
    if (thread_.joinable()) {
        SetEvent(quit_event_);
        thread_.join();
    }
    if (quit_event_) {
        CloseHandle(quit_event_);
        quit_event_ = nullptr;
    }
}

void RudderReader::set_settings(const MappingSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = settings;
}

void RudderReader::recenter() {
    std::lock_guard<std::mutex> lock(mutex_);
    recenter_requested_ = true;
}

void RudderReader::clear_neutral() {
    std::lock_guard<std::mutex> lock(mutex_);
    clear_neutral_requested_ = true;
}

std::shared_ptr<const RudderSnapshot> RudderReader::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void RudderReader::publish() {
    auto snap = std::make_shared<const RudderSnapshot>(current_);
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = std::move(snap);
}

void RudderReader::set_state(const GamepadState& state) {
    current_.state = state;
    if (sender_) {
        sender_->set_state(state);
    }
}

bool RudderReader::open_device() {
    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);
    HDEVINFO info = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        current_.error = "Can't list HID devices.";
        return false;
    }
    current_.error = "3dRudder not found. Is it plugged in?";
    std::vector<uint8_t> detail_buf;
    for (DWORD i = 0; device_ == INVALID_HANDLE_VALUE; i++) {
        SP_DEVICE_INTERFACE_DATA ifd = { sizeof(ifd) };
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &hid_guid, i, &ifd)) {
            break;
        }
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifd, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }
        detail_buf.assign(size, 0);
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*) detail_buf.data();
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &ifd, detail, size, nullptr, nullptr)) {
            continue;
        }

        // Check what it is without asking for read access first.
        HANDLE probe = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            continue;
        }
        HIDD_ATTRIBUTES attr = { sizeof(attr) };
        HIDP_CAPS caps = {};
        bool match = HidD_GetAttributes(probe, &attr) && attr.VendorID == RUDDER_VID;
        if (match) {
            PHIDP_PREPARSED_DATA pp = nullptr;
            match = HidD_GetPreparsedData(probe, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == 0xff00 && caps.InputReportByteLength >= RUDDER_REPORT_SIZE;
            if (pp) {
                HidD_FreePreparsedData(pp);
            }
        }
        wchar_t name[128] = {};
        if (match && !HidD_GetProductString(probe, name, sizeof(name))) {
            name[0] = 0;
        }
        CloseHandle(probe);
        if (!match) {
            continue;
        }

        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            current_.error = "Found the 3dRudder, but can't open it.";
            continue;
        }
        device_ = h;
        report_len_ = caps.InputReportByteLength;
        current_.connected = true;
        current_.error.clear();
        current_.device_name = name[0] ? to_utf8(name) : "3dRudder";
        char ids[32];
        snprintf(ids, sizeof(ids), " (%04x:%04x)", attr.VendorID, attr.ProductID);
        current_.device_name += ids;
    }
    SetupDiDestroyDeviceInfoList(info);
    return device_ != INVALID_HANDLE_VALUE;
}

void RudderReader::close_device() {
    if (device_ != INVALID_HANDLE_VALUE) {
        CloseHandle(device_);
        device_ = INVALID_HANDLE_VALUE;
    }
    current_.connected = false;
    current_.have_frame = false;
    current_.rate = 0.0f;
    current_.error = "3dRudder disconnected.";
    mapper_.reset();
    for (float& l : current_.levels) {
        l = 0.0f;
    }
    set_state(GamepadState());
}

void RudderReader::handle_report(const uint8_t* data, size_t len) {
    RudderFrame frame;
    std::string text;
    if (parse_rudder_frame(data, len, frame)) {
        MappingSettings settings;
        bool recenter, clear;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            settings = settings_;
            recenter = recenter_requested_;
            clear = clear_neutral_requested_;
            recenter_requested_ = clear_neutral_requested_ = false;
        }
        GamepadState state;
        mapper_.map(frame, settings, state, current_.levels);
        if (recenter && mapper_.recenter()) {
            mapper_.map(frame, settings, state, current_.levels);
        }
        if (clear) {
            mapper_.clear_neutral();
            mapper_.map(frame, settings, state, current_.levels);
        }
        current_.frame = frame;
        current_.have_frame = true;
        current_.recentered = mapper_.recentered();
        for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
            current_.relative[a] = mapper_.relative(frame, a);
        }
        const uint64_t now = GetTickCount64();
        last_frame_ms_ = now;
        rate_count_++;
        if (now - rate_start_ms_ >= 1000) {
            current_.rate = rate_count_ * 1000.0f / (float) (now - rate_start_ms_);
            rate_start_ms_ = now;
            rate_count_ = 0;
        }
        set_state(state);
        publish();
    } else if (parse_rudder_text(data, len, text)) {
        if (!text.empty()) {
            current_.log.push_back(text);
            while (current_.log.size() > LOG_LINES) {
                current_.log.pop_front();
            }
            publish();
        }
    }
}

void RudderReader::thread_main() {
    HANDLE read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<uint8_t> buf;
    bool quit = false;
    while (!quit) {
        if (device_ == INVALID_HANDLE_VALUE) {
            if (!open_device()) {
                publish();
                quit = WaitForSingleObject(quit_event_, (DWORD) RECONNECT_MS) == WAIT_OBJECT_0;
                continue;
            }
            rate_start_ms_ = GetTickCount64();
            rate_count_ = 0;
            buf.assign(report_len_, 0);
            publish();
        }

        OVERLAPPED ov = {};
        ov.hEvent = read_event;
        ResetEvent(read_event);
        if (!ReadFile(device_, buf.data(), (DWORD) buf.size(), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
            close_device();
            publish();
            continue;
        }
        for (;;) {
            const HANDLE handles[2] = { quit_event_, read_event };
            const DWORD r = WaitForMultipleObjects(2, handles, FALSE, 100);
            if (r == WAIT_OBJECT_0) {
                CancelIoEx(device_, &ov);
                DWORD n;
                GetOverlappedResult(device_, &ov, &n, TRUE);
                quit = true;
                break;
            }
            if (r == WAIT_OBJECT_0 + 1) {
                DWORD n = 0;
                if (GetOverlappedResult(device_, &ov, &n, FALSE)) {
                    handle_report(buf.data(), n);
                } else {
                    close_device();
                    publish();
                }
                break;
            }
            // Timeout: the device is there but quiet.
            if (current_.have_frame && GetTickCount64() - last_frame_ms_ > STALE_MS) {
                current_.have_frame = false;
                current_.rate = 0.0f;
                for (float& l : current_.levels) {
                    l = 0.0f;
                }
                set_state(GamepadState());
                publish();
            }
        }
    }
    if (device_ != INVALID_HANDLE_VALUE) {
        CloseHandle(device_);
        device_ = INVALID_HANDLE_VALUE;
    }
    CloseHandle(read_event);
}
