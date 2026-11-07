// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "gamepad.h"
#include "mapping.h"

class Sender;

// ---------------------------------------------------------------------------
// Protocol
//
// The 3dRudder (PS4 version, VID 2dfa PID 0002) has one vendor-defined HID
// interface (usage page ff00) with 64-byte input reports, report ID 1. Byte 2
// is the message type:
//
//   f5  sensor data, about 60 per second:
//       3      counter
//       4      status (see RUDDER_STATUS_NAMES)
//       6..21  rotation, forward/back, left/right (degrees), up/down (-1..1),
//              little-endian floats. Relative to where the feet were when
//              the device finished initializing; only meaningful in use.
//       22..25 a float that drifts slowly, unknown
//       26..37 six force sensors, uint16
//       38..43 accelerometer x/y/z, int16 (16384 = 1 g)
//       44..49 gyro x/y/z, int16
//   f4  status change events
//   fd  debug text from the firmware: byte 3 counter, text from byte 4

constexpr uint16_t RUDDER_VID = 0x2dfa;
constexpr size_t RUDDER_REPORT_SIZE = 64;
constexpr int RUDDER_SENSOR_COUNT = 6;

enum class RudderMessage : uint8_t {
    Status = 0xf4,
    Data = 0xf5,
    Text = 0xfd,
};

// Status codes, as in the 3dRudder SDK.
constexpr int RUDDER_STATUS_COUNT = 8;
extern const char* const RUDDER_STATUS_NAMES[RUDDER_STATUS_COUNT];
const char* rudder_status_name(int status);

// 6 = in use, 7 = in use with extended mode.
inline bool rudder_in_use(int status) {
    return status == 6 || status == 7;
}

struct RudderFrame {
    uint8_t counter = 0;
    uint8_t status = 0;
    float axes[RUDDER_AXIS_COUNT] = {};
    float unknown = 0.0f;
    uint16_t sensors[RUDDER_SENSOR_COUNT] = {};
    int16_t accel[3] = {};
    int16_t gyro[3] = {};
};

// Decodes a data message (report ID included). False if it isn't one.
bool parse_rudder_frame(const uint8_t* report, size_t len, RudderFrame& frame);

// The text of a debug message, without the line ending. False if it isn't one.
bool parse_rudder_text(const uint8_t* report, size_t len, std::string& text);

// ---------------------------------------------------------------------------
// Mapping the frames to a gamepad state

struct MappingSettings {
    AxisMapping axes[RUDDER_AXIS_COUNT];
};

// Turns rudder frames into gamepad states. Keeps the neutral position and
// button states between frames.
class RudderMapper {
public:
    // Fills state and, if not null, levels (each axis's linear level).
    void map(const RudderFrame& frame, const MappingSettings& settings, GamepadState& state,
        float* levels = nullptr);
    void reset();

    // Takes the last frame's position as neutral (only while in use).
    bool recenter();
    void clear_neutral();
    bool recentered() const { return recentered_; }
    // Position relative to neutral.
    float relative(const RudderFrame& frame, int axis) const;

private:
    bool buttons_[RUDDER_AXIS_COUNT][2] = {};
    float neutral_[RUDDER_AXIS_COUNT] = {};
    bool recentered_ = false;
    bool was_in_use_ = false;
    RudderFrame last_;
};

// ---------------------------------------------------------------------------
// Reading the device

// What the UI shows.
struct RudderSnapshot {
    bool connected = false;
    std::string device_name;
    std::string error;
    bool have_frame = false;  // a data message arrived recently
    RudderFrame frame;
    float relative[RUDDER_AXIS_COUNT] = {};  // to neutral
    float levels[RUDDER_AXIS_COUNT] = {};
    bool recentered = false;
    float rate = 0.0f;  // data messages per second
    GamepadState state;
    std::deque<std::string> log;  // firmware messages, newest last
};

// Finds the 3dRudder, reads it on its own thread (reconnecting if it goes
// away), maps it and passes the result to the sender.
class RudderReader {
public:
    ~RudderReader();
    void start(Sender* sender);
    void stop();

    void set_settings(const MappingSettings& settings);
    void recenter();
    void clear_neutral();
    std::shared_ptr<const RudderSnapshot> snapshot();

private:
    void thread_main();
    bool open_device();
    void close_device();
    void handle_report(const uint8_t* data, size_t len);
    void publish();
    void set_state(const GamepadState& state);

    std::thread thread_;
    HANDLE quit_event_ = nullptr;
    Sender* sender_ = nullptr;

    std::mutex mutex_;
    MappingSettings settings_;
    bool recenter_requested_ = false;
    bool clear_neutral_requested_ = false;
    std::shared_ptr<const RudderSnapshot> snapshot_;

    // Reader thread only.
    HANDLE device_ = INVALID_HANDLE_VALUE;
    size_t report_len_ = 0;
    RudderSnapshot current_;
    RudderMapper mapper_;
    uint64_t last_frame_ms_ = 0;
    uint64_t rate_start_ms_ = 0;
    int rate_count_ = 0;
};
