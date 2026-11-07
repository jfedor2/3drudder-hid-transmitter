// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include "gamepad.h"

// The four axes the 3dRudder reports.
enum class RudderAxis : int {
    Rotation,
    ForwardBack,
    LeftRight,
    UpDown,
    Count
};

constexpr int RUDDER_AXIS_COUNT = (int) RudderAxis::Count;
extern const char* const RUDDER_AXIS_NAMES[RUDDER_AXIS_COUNT];         // "Rotation"
extern const char* const RUDDER_AXIS_KEYS[RUDDER_AXIS_COUNT];          // "rotation" (config)
extern const char* const RUDDER_AXIS_DESCRIPTIONS[RUDDER_AXIS_COUNT];  // "turn clockwise = +"

// Rotation and the tilts are in degrees, up / down is -1..1.
inline bool axis_in_degrees(RudderAxis a) {
    return a != RudderAxis::UpDown;
}

// Slider limits for each axis, in its units.
struct AxisLimits {
    float range_min, range_max;  // full output at
    float deadzone_max;
    float display;               // the bar shows +/- this much
};
extern const AxisLimits AXIS_LIMITS[RUDDER_AXIS_COUNT];

// What an axis drives. For sticks, the + side (clockwise, forward, right, up)
// moves the stick right / up. Triggers: the + side drives R2, the - side L2.
// L2 / R2 alone are driven by the + side only.
enum class AxisTarget : int {
    None,
    LeftStickX,
    LeftStickY,
    RightStickX,
    RightStickY,
    Triggers,
    L2,
    R2,
    Count
};

constexpr int AXIS_TARGET_COUNT = (int) AxisTarget::Count;
extern const char* const AXIS_TARGET_NAMES[AXIS_TARGET_COUNT];

constexpr int NO_BUTTON = -1;

struct AxisMapping {
    AxisTarget target = AxisTarget::None;
    bool invert = false;
    float range = 10.0f;    // from neutral for full output
    float deadzone = 1.0f;  // from neutral with no output
    float curve = 1.0f;     // response exponent, 1 = linear
    // Buttons pressed past a level (0..1 of the way from deadzone to full)
    // on either side, independent of the analog target.
    int button_pos = NO_BUTTON;
    int button_neg = NO_BUTTON;
    float button_level = 0.5f;
};

// Linear level (-1..1) for a value relative to neutral: 0 inside the
// deadzone, 1 at the range. Invert applied, curve not.
float axis_level(const AxisMapping& m, float value);

// The analog output (-1..1): the level through the response curve.
float axis_output(const AxisMapping& m, float value);

// Whether a level presses a button, with a little hysteresis so it doesn't
// chatter at the threshold.
bool button_pressed(bool was_pressed, float level, float threshold);

// Adds the output of one axis to the gamepad state. Values add up when several
// axes drive the same output; clamp_state() caps them.
void apply_axis(const AxisMapping& m, float output, GamepadState& state);
void clamp_state(GamepadState& state);
