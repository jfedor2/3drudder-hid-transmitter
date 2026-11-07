// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

// Command line tool for checking the protocol decoding and the mapping.
//
//   rht-test             run the self-tests
//   rht-test dump        print what the connected 3dRudder sends
//   rht-test replay FILE map a recording (lines of "seconds hex hex ...")

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "config.h"
#include "mapping.h"
#include "rudder.h"

static int g_failures = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            g_failures++;                                              \
        }                                                              \
    } while (0)

static bool close_to(float a, float b, float eps = 0.01f) {
    return std::fabs(a - b) <= eps;
}

static std::vector<uint8_t> from_hex(const std::string& s) {
    std::vector<uint8_t> out;
    std::istringstream in(s);
    std::string byte;
    while (in >> byte) {
        out.push_back((uint8_t) strtoul(byte.c_str(), nullptr, 16));
    }
    return out;
}

static MappingSettings default_settings() {
    Config c;
    MappingSettings s;
    for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
        s.axes[a] = c.axes[a];
    }
    return s;
}

static void run_tests() {
    // Recorded while rotating clockwise.
    const std::vector<uint8_t> report = from_hex(
        "01 00 f5 b6 06 00 a0 77 b5 41 1a 70 0d c0 59 07 2e c0 91 65 ac 3d b6 a8 24 43 5c 00 9f 04 ff 0a "
        "f3 07 bf 0c 6e 01 e8 f7 10 fd 50 c0 d4 ff dc ff e2 ff 7f 00 6d 00 7f 01 00 00 00 00 00 00 00 00");
    RudderFrame f;
    CHECK(parse_rudder_frame(report.data(), report.size(), f));
    CHECK(f.status == 6);
    CHECK(f.counter == 0xb6);
    CHECK(close_to(f.axes[(int) RudderAxis::Rotation], 22.68f));
    CHECK(close_to(f.axes[(int) RudderAxis::ForwardBack], -2.21f));
    CHECK(close_to(f.axes[(int) RudderAxis::LeftRight], -2.72f));
    CHECK(close_to(f.axes[(int) RudderAxis::UpDown], 0.084f, 0.001f));
    CHECK(f.sensors[0] == 92 && f.sensors[5] == 366);
    CHECK(f.accel[2] == -16304);

    std::string text;
    const std::vector<uint8_t> msg = from_hex(
        "01 00 fd 1b 49 6e 69 74 20 73 74 65 70 20 63 6f 6d 70 6c 65 74 65 64 20 36 44 4f 46 0d 0a 00 00");
    CHECK(!parse_rudder_frame(msg.data(), msg.size(), f));
    CHECK(parse_rudder_text(msg.data(), msg.size(), text));
    CHECK(text == "Init step completed 6DOF");

    // Mapping
    AxisMapping m;
    m.range = 10.0f;
    m.deadzone = 2.0f;
    CHECK(axis_level(m, 1.0f) == 0.0f);
    CHECK(close_to(axis_level(m, 6.0f), 0.5f));
    CHECK(close_to(axis_level(m, -6.0f), -0.5f));
    CHECK(axis_level(m, 20.0f) == 1.0f);
    m.curve = 2.0f;
    CHECK(close_to(axis_output(m, 6.0f), 0.25f));
    CHECK(close_to(axis_output(m, -6.0f), -0.25f));
    m.invert = true;
    CHECK(close_to(axis_output(m, 6.0f), -0.25f));

    CHECK(!button_pressed(false, 0.49f, 0.5f));
    CHECK(button_pressed(false, 0.5f, 0.5f));
    CHECK(button_pressed(true, 0.47f, 0.5f));
    CHECK(!button_pressed(true, 0.44f, 0.5f));
    CHECK(!button_pressed(false, 0.0f, 0.0f));
    CHECK(button_pressed(false, 0.01f, 0.0f));

    // Defaults: clockwise past full steers the left stick right.
    MappingSettings s = default_settings();
    RudderMapper mapper;
    GamepadState state;
    mapper.map(f, s, state);
    CHECK(close_to(state.sticks[(int) Stick::Left][0], 1.0f));
    // Forward / back of -2.2 is past the 0.8 deadzone: back = stick down (+y).
    CHECK(state.sticks[(int) Stick::Left][1] > 0.1f);

    // A button on the + side of rotation.
    s.axes[(int) RudderAxis::Rotation].button_pos = (int) Button::A;
    mapper.map(f, s, state);
    CHECK(state.buttons == button_mask(Button::A));

    // Recentering makes the current position neutral.
    CHECK(mapper.recenter());
    mapper.map(f, s, state);
    CHECK(state.sticks[(int) Stick::Left][0] == 0.0f);
    CHECK(state.buttons == 0);

    // Not in use: nothing, and the neutral is dropped when it's in use again.
    RudderFrame idle = f;
    idle.status = 3;
    mapper.map(idle, s, state);
    CHECK(state == GamepadState());
    mapper.map(f, s, state);
    CHECK(!mapper.recentered());
    CHECK(close_to(state.sticks[(int) Stick::Left][0], 1.0f));

    // Rotation wraps around.
    RudderFrame wrap = f;
    wrap.axes[(int) RudderAxis::Rotation] = 179.0f;
    mapper.map(wrap, s, state);
    CHECK(mapper.recenter());
    wrap.axes[(int) RudderAxis::Rotation] = -179.0f;
    mapper.map(wrap, s, state);
    CHECK(close_to(mapper.relative(wrap, (int) RudderAxis::Rotation), 2.0f));
}

static void print_frame(const RudderFrame& f, const GamepadState& st) {
    printf("st=%d rot=%7.2f fb=%7.2f lr=%7.2f ud=%5.2f  LS=(%+.2f,%+.2f) RS=(%+.2f,%+.2f) L2=%.2f R2=%.2f btn=%05x\n",
        f.status, f.axes[0], f.axes[1], f.axes[2], f.axes[3], st.sticks[0][0], -st.sticks[0][1], st.sticks[1][0],
        -st.sticks[1][1], st.triggers[0], st.triggers[1], st.buttons);
}

static int dump() {
    RudderReader reader;
    reader.set_settings(default_settings());
    reader.start(nullptr);
    size_t log_seen = 0;
    for (;;) {
        Sleep(200);
        auto snap = reader.snapshot();
        if (!snap) {
            continue;
        }
        if (snap->log.size() < log_seen) {
            log_seen = 0;
        }
        for (size_t i = log_seen; i < snap->log.size(); i++) {
            printf("  [device] %s\n", snap->log[i].c_str());
        }
        log_seen = snap->log.size();
        if (!snap->connected) {
            printf("%s\n", snap->error.c_str());
        } else if (!snap->have_frame) {
            printf("%s: no data\n", snap->device_name.c_str());
        } else {
            print_frame(snap->frame, snap->state);
        }
        fflush(stdout);
    }
}

static int replay(const char* path) {
    std::ifstream in(path);
    if (!in) {
        printf("can't open %s\n", path);
        return 1;
    }
    MappingSettings s = default_settings();
    RudderMapper mapper;
    std::string line;
    double last_print = -1;
    while (std::getline(in, line)) {
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) {
            continue;
        }
        const double t = atof(line.substr(0, sp).c_str());
        const std::vector<uint8_t> r = from_hex(line.substr(sp + 1));
        RudderFrame f;
        std::string text;
        if (parse_rudder_frame(r.data(), r.size(), f)) {
            GamepadState st;
            mapper.map(f, s, st);
            if (t - last_print >= 0.25) {
                printf("%7.2f ", t);
                print_frame(f, st);
                last_print = t;
            }
        } else if (parse_rudder_text(r.data(), r.size(), text) && !text.empty()) {
            printf("%7.2f [device] %s\n", t, text.c_str());
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "dump") == 0) {
        return dump();
    }
    if (argc >= 3 && strcmp(argv[1], "replay") == 0) {
        return replay(argv[2]);
    }
    run_tests();
    printf(g_failures ? "%d check(s) failed\n" : "all tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
