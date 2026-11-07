// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include "config.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

void default_axes(AxisMapping (&axes)[RUDDER_AXIS_COUNT]) {
    // Like the HID Remapper "3dRudder: analog stick" example: rotation steers,
    // tilting forward / back moves forward / back.
    axes[(int) RudderAxis::Rotation] = { AxisTarget::LeftStickX, false, 12.0f, 1.3f };
    axes[(int) RudderAxis::ForwardBack] = { AxisTarget::LeftStickY, false, 7.2f, 0.8f };
    axes[(int) RudderAxis::LeftRight] = { AxisTarget::None, false, 10.0f, 1.0f };
    axes[(int) RudderAxis::UpDown] = { AxisTarget::None, false, 0.8f, 0.1f };
}

Config::Config() {
    default_axes(axes);
}

// The same layout SDL_GetPrefPath() gives the other HID Forwarder tools.
std::wstring app_data_dir() {
    static std::wstring dir = []() {
        PWSTR appdata = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appdata))) {
            CoTaskMemFree(appdata);
            return std::wstring(L".");
        }
        std::wstring path = appdata;
        CoTaskMemFree(appdata);
        for (const wchar_t* part : { L"\\3drudder-hid-transmitter", L"\\3drudder-hid-transmitter" }) {
            path += part;
            CreateDirectoryW(path.c_str(), nullptr);
        }
        return path;
    }();
    return dir;
}

template <typename E, int N>
static bool enum_from_name(const std::string& name, const char* const (&names)[N], E& out) {
    for (int i = 0; i < N; i++) {
        if (name == names[i]) {
            out = (E) i;
            return true;
        }
    }
    return false;
}

static int button_from_name(const std::string& name) {
    int b = NO_BUTTON;
    enum_from_name(name, BUTTON_NAMES, b);
    return b;
}

static std::string button_name(int b) {
    return b >= 0 && b < BUTTON_COUNT ? BUTTON_NAMES[b] : "";
}

bool load_config(const std::wstring& path, Config& config) {
    std::ifstream f(path);
    if (!f) {
        return false;
    }
    try {
        json j = json::parse(f);
        config.receiver_ip = j.value("receiver_ip", "");
        if (j.contains("axes") && j["axes"].is_object()) {
            for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
                const json& ja = j["axes"].value(RUDDER_AXIS_KEYS[a], json::object());
                const AxisLimits& lim = AXIS_LIMITS[a];
                AxisMapping& m = config.axes[a];
                enum_from_name(ja.value("output", ""), AXIS_TARGET_NAMES, m.target);
                m.invert = ja.value("invert", m.invert);
                m.range = std::clamp(ja.value("range", m.range), lim.range_min, lim.range_max);
                m.deadzone = std::clamp(ja.value("deadzone", m.deadzone), 0.0f, lim.deadzone_max);
                m.curve = std::clamp(ja.value("curve", m.curve), 1.0f, 3.0f);
                m.button_pos = button_from_name(ja.value("button_plus", ""));
                m.button_neg = button_from_name(ja.value("button_minus", ""));
                m.button_level = std::clamp(ja.value("button_level", m.button_level), 0.0f, 1.0f);
            }
        }
        if (j.contains("window") && j["window"].is_object()) {
            const json& jw = j["window"];
            config.window.x = jw.value("x", 0);
            config.window.y = jw.value("y", 0);
            config.window.w = jw.value("w", 0);
            config.window.h = jw.value("h", 0);
            config.window.maximized = jw.value("maximized", false);
            config.window.valid = config.window.w > 0 && config.window.h > 0;
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool save_config(const std::wstring& path, const Config& config) {
    json j;
    j["receiver_ip"] = config.receiver_ip;
    json axes = json::object();
    for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
        const AxisMapping& m = config.axes[a];
        axes[RUDDER_AXIS_KEYS[a]] = {
            { "output", AXIS_TARGET_NAMES[(int) m.target] },
            { "invert", m.invert },
            { "range", m.range },
            { "deadzone", m.deadzone },
            { "curve", m.curve },
            { "button_plus", button_name(m.button_pos) },
            { "button_minus", button_name(m.button_neg) },
            { "button_level", m.button_level },
        };
    }
    j["axes"] = axes;
    if (config.window.valid) {
        j["window"] = {
            { "x", config.window.x },
            { "y", config.window.y },
            { "w", config.window.w },
            { "h", config.window.h },
            { "maximized", config.window.maximized },
        };
    }

    std::wstring tmp_path = path + L".tmp";
    {
        std::ofstream f(tmp_path, std::ios::trunc);
        if (!f) {
            return false;
        }
        f << j.dump(2);
        if (!f) {
            return false;
        }
    }
    return MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
