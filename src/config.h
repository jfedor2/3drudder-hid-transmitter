// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#pragma once

#include <string>

#include "mapping.h"

struct WindowRect {
    bool valid = false;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool maximized = false;

    bool operator==(const WindowRect& o) const {
        return valid == o.valid && x == o.x && y == o.y && w == o.w && h == o.h && maximized == o.maximized;
    }
};

struct Config {
    std::string receiver_ip;
    AxisMapping axes[RUDDER_AXIS_COUNT];
    WindowRect window;

    Config();
};

// The defaults, also used by "Reset to defaults".
void default_axes(AxisMapping (&axes)[RUDDER_AXIS_COUNT]);

// %APPDATA%\3drudder-hid-transmitter\3drudder-hid-transmitter, created if needed.
std::wstring app_data_dir();

bool load_config(const std::wstring& path, Config& config);
bool save_config(const std::wstring& path, const Config& config);
