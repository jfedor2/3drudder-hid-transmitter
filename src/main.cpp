// Copyright (c) 2026 Jacek Fedorynski
// SPDX-License-Identifier: MIT

#include <winsock2.h>
#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "config.h"
#include "mapping.h"
#include "rudder.h"
#include "sender.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_device_context = nullptr;
static IDXGISwapChain* g_swap_chain = nullptr;
static ID3D11RenderTargetView* g_render_target_view = nullptr;
static bool g_swap_chain_occluded = false;
static UINT g_resize_width = 0;
static UINT g_resize_height = 0;
static float g_pending_dpi_scale = 0.0f;

static const ImVec4 COLOR_ERROR(1.0f, 0.42f, 0.42f, 1.0f);
static const ImVec4 COLOR_OK(0.45f, 0.9f, 0.55f, 1.0f);
static const ImVec4 COLOR_WARN(1.0f, 0.8f, 0.35f, 1.0f);

static const ImU32 COLOR_MARKER = IM_COL32(255, 200, 80, 255);
static const ImU32 COLOR_FILL = IM_COL32(60, 140, 220, 200);
static const ImU32 COLOR_BUTTON_MARK = IM_COL32(240, 120, 60, 255);

// Application state, owned by the UI thread.
struct App {
    Config config;
    std::wstring config_path;
    bool running = false;

    bool config_dirty = false;
    ULONGLONG config_dirty_since = 0;
    HWND main_hwnd = nullptr;

    Sender sender;
    RudderReader reader;
    std::shared_ptr<const RudderSnapshot> snapshot;

    void mark_dirty() {
        if (!config_dirty) {
            config_dirty_since = GetTickCount64();
        }
        config_dirty = true;
    }

    // Call after any change to the mapping.
    void mapping_changed() {
        MappingSettings s;
        for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
            s.axes[a] = config.axes[a];
        }
        reader.set_settings(s);
        mark_dirty();
    }

    bool in_use() const {
        return snapshot && snapshot->have_frame && rudder_in_use(snapshot->frame.status);
    }
};

static App g_app;

// ---------------------------------------------------------------------------
// D3D11 boilerplate (from the Dear ImGui example_win32_directx11)

static void create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (back_buffer != nullptr) {
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target_view);
        back_buffer->Release();
    }
}

static void cleanup_render_target() {
    if (g_render_target_view != nullptr) {
        g_render_target_view->Release();
        g_render_target_view = nullptr;
    }
}

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device, &level, &g_device_context);
    }
    if (FAILED(hr)) {
        return false;
    }
    create_render_target();
    return true;
}

static void cleanup_device() {
    cleanup_render_target();
    if (g_swap_chain) {
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_device_context) {
        g_device_context->Release();
        g_device_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------------------

static void apply_style(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    ImGui::StyleColorsDark();
    style.Colors[ImGuiCol_PopupBg].w = 1.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

static bool g_track_placement = false;  // set once the saved placement has been applied

static void capture_window_placement() {
    if (!g_track_placement) {
        return;
    }
    WINDOWPLACEMENT wp = { sizeof(wp) };
    if (!GetWindowPlacement(g_app.main_hwnd, &wp)) {
        return;
    }
    WindowRect w;
    w.valid = true;
    w.x = wp.rcNormalPosition.left;
    w.y = wp.rcNormalPosition.top;
    w.w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
    w.h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    w.maximized = wp.showCmd == SW_SHOWMAXIMIZED ||
                  (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    if (!(w == g_app.config.window)) {
        g_app.config.window = w;
        g_app.mark_dirty();
    }
}

static void save_now() {
    save_config(g_app.config_path, g_app.config);
    g_app.config_dirty = false;
}

// ---------------------------------------------------------------------------
// Window icon: a circle showing the state. The left half is the 3dRudder:
// yellow = connected but not in use (feet not on it), green = in use. The
// right half is red when stopped, green when running. No 3dRudder at all
// makes the whole circle red.

constexpr uint32_t ICON_RED = 0xd94040, ICON_YELLOW = 0xe0b830, ICON_GREEN = 0x3cb85a;

static HICON make_status_icon(uint32_t left_rgb, uint32_t right_rgb, int size) {
    BITMAPV5HEADER bh = {};
    bh.bV5Size = sizeof(bh);
    bh.bV5Width = size;
    bh.bV5Height = -size;  // top-down
    bh.bV5Planes = 1;
    bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00ff0000;
    bh.bV5GreenMask = 0x0000ff00;
    bh.bV5BlueMask = 0x000000ff;
    bh.bV5AlphaMask = 0xff000000;
    uint32_t* pixels = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*) &bh, DIB_RGB_COLORS, (void**) &pixels, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) {
        return nullptr;
    }
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);

    const float c = size / 2.0f, r = size / 2.0f - std::max(size / 32.0f, 0.5f);
    constexpr int SS = 4;  // supersampling, for smooth edges
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int inside = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    const float dx = x + (sx + 0.5f) / SS - c, dy = y + (sy + 0.5f) / SS - c;
                    inside += dx * dx + dy * dy <= r * r;
                }
            }
            const uint32_t rgb = x < size / 2 ? left_rgb : right_rgb;
            const uint32_t alpha = inside * 255 / (SS * SS);
            pixels[y * size + x] = (alpha << 24) | rgb;
        }
    }

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// Sets the window's icons if the state (or the DPI) changed.
static void update_icon(HWND hwnd) {
    static uint32_t current_left = 0, current_right = 0;
    static UINT current_dpi = 0;
    static HICON big_icon = nullptr, small_icon = nullptr;

    const RudderSnapshot* snap = g_app.snapshot.get();
    uint32_t left = ICON_RED, right = ICON_RED;
    if (snap && snap->connected) {
        left = g_app.in_use() ? ICON_GREEN : ICON_YELLOW;
        right = g_app.running ? ICON_GREEN : ICON_RED;
    }
    const UINT dpi = GetDpiForWindow(hwnd);
    if (left == current_left && right == current_right && dpi == current_dpi) {
        return;
    }
    current_left = left;
    current_right = right;
    current_dpi = dpi;
    HICON new_big = make_status_icon(left, right, GetSystemMetricsForDpi(SM_CXICON, dpi));
    HICON new_small = make_status_icon(left, right, GetSystemMetricsForDpi(SM_CXSMICON, dpi));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM) new_big);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM) new_small);
    if (big_icon) {
        DestroyIcon(big_icon);
    }
    if (small_icon) {
        DestroyIcon(small_icon);
    }
    big_icon = new_big;
    small_icon = new_small;
}

static void push_button_colors(ImVec4 base) {
    ImGui::PushStyleColor(ImGuiCol_Button, base);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(base.x * 1.2f, base.y * 1.2f, base.z * 1.2f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(base.x * 1.4f, base.y * 1.4f, base.z * 1.4f, 1));
}

// "12.0°" or "0.50", depending on the axis.
static void format_value(int a, float v, char* buf, size_t size, bool sign = true) {
    if (axis_in_degrees((RudderAxis) a)) {
        snprintf(buf, size, sign ? "%+.1f°" : "%.1f°", v);
    } else {
        snprintf(buf, size, sign ? "%+.2f" : "%.2f", v);
    }
}

// ---------------------------------------------------------------------------
// Device view

// Top-down view: the feet turned by the rotation, the tilt as a marker.
static void draw_rudder_view(float size) {
    const RudderSnapshot* snap = g_app.snapshot.get();
    const bool in_use = g_app.in_use();
    const float em = ImGui::GetFontSize();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = size / 2;
    const ImVec2 c(p0.x + r, p0.y + r);

    dl->AddCircleFilled(c, r, IM_COL32(32, 34, 40, 255), 64);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 60), 64, 1.5f);
    dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), IM_COL32(255, 255, 255, 25));
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), IM_COL32(255, 255, 255, 25));

    const float rotation = in_use ? snap->relative[(int) RudderAxis::Rotation] : 0.0f;
    const float tilt_x = in_use ? snap->relative[(int) RudderAxis::LeftRight] : 0.0f;
    const float tilt_y = in_use ? snap->relative[(int) RudderAxis::ForwardBack] : 0.0f;

    // Feet, rotated (clockwise = +, screen y is down).
    const float th = rotation * 3.14159265f / 180.0f;
    const float cs = std::cos(th), sn = std::sin(th);
    auto rot = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
    // Opaque, so the toes and the soles don't add up where they overlap.
    const ImU32 foot_col = in_use ? IM_COL32(97, 102, 117, 255) : IM_COL32(56, 58, 66, 255);
    for (float side : { -1.0f, 1.0f }) {
        const float fx = side * r * 0.32f, fw = r * 0.16f, fh = r * 0.58f;
        const ImVec2 q[4] = { rot(fx - fw, -fh), rot(fx + fw, -fh), rot(fx + fw, fh), rot(fx - fw, fh) };
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], foot_col);
        // toes
        dl->AddCircleFilled(rot(fx, -fh), fw, foot_col, 16);
    }
    // Which way is forward.
    dl->AddTriangleFilled(rot(0, -r * 0.95f), rot(-em * 0.4f, -r * 0.95f + em * 0.6f),
        rot(em * 0.4f, -r * 0.95f + em * 0.6f), in_use ? COLOR_MARKER : IM_COL32(110, 110, 120, 120));

    if (in_use) {
        // Tilt, with the bar scales at the edge.
        const float sx = AXIS_LIMITS[(int) RudderAxis::LeftRight].display;
        const float sy = AXIS_LIMITS[(int) RudderAxis::ForwardBack].display;
        const ImVec2 dot(c.x + std::clamp(tilt_x / sx, -1.0f, 1.0f) * (r - em * 0.4f),
            c.y - std::clamp(tilt_y / sy, -1.0f, 1.0f) * (r - em * 0.4f));
        dl->AddLine(c, dot, IM_COL32(255, 200, 80, 120), 2.0f);
        dl->AddCircleFilled(dot, em * 0.35f, COLOR_MARKER);
    } else {
        const char* text = !snap || !snap->connected ? "Not connected"
                           : !snap->have_frame       ? "No data"
                                                     : rudder_status_name(snap->frame.status);
        const float wrap = size - em * 2;
        const ImVec2 ts = ImGui::CalcTextSize(text, nullptr, false, wrap);
        const ImVec2 tp(c.x - ts.x / 2, c.y - ts.y / 2);
        dl->AddRectFilled(ImVec2(tp.x - em * 0.4f, tp.y - em * 0.2f), ImVec2(tp.x + ts.x + em * 0.4f, tp.y + ts.y + em * 0.2f),
            IM_COL32(0, 0, 0, 170), 4.0f);
        dl->AddText(nullptr, 0.0f, tp, ImGui::GetColorU32(COLOR_WARN), text, nullptr, wrap);
    }
}

// A vertical -1..1 bar for up / down.
static void draw_updown_bar(float height) {
    const RudderSnapshot* snap = g_app.snapshot.get();
    const bool in_use = g_app.in_use();
    const float em = ImGui::GetFontSize();
    const float w = em * 1.2f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + height);
    ImGui::Dummy(ImVec2(w, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(32, 34, 40, 255), 3.0f);
    const float cy = (p0.y + p1.y) / 2;
    dl->AddLine(ImVec2(p0.x, cy), ImVec2(p1.x, cy), IM_COL32(255, 255, 255, 50));
    if (in_use) {
        const float v = std::clamp(snap->relative[(int) RudderAxis::UpDown], -1.0f, 1.0f);
        const float y = cy - v * height / 2;
        dl->AddRectFilled(ImVec2(p0.x + 2, std::min(y, cy)), ImVec2(p1.x - 2, std::max(y, cy)), COLOR_FILL, 2.0f);
        dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), COLOR_MARKER, 2.0f);
    }
    ImGui::SetItemTooltip("Up / down");
}

static void draw_sensors() {
    const RudderSnapshot* snap = g_app.snapshot.get();
    const bool have = snap && snap->have_frame;
    const float em = ImGui::GetFontSize();
    const float h = em * 3.0f, w = em * 1.4f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < RUDDER_SENSOR_COUNT; i++) {
        if (i > 0) {
            ImGui::SameLine(0, em * 0.4f);
        }
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1(p0.x + w, p0.y + h);
        ImGui::Dummy(ImVec2(w, h));
        dl->AddRectFilled(p0, p1, IM_COL32(32, 34, 40, 255), 3.0f);
        const int v = have ? snap->frame.sensors[i] : 0;
        const float f = std::clamp(v / 8000.0f, 0.0f, 1.0f);
        if (f > 0) {
            dl->AddRectFilled(ImVec2(p0.x + 2, p1.y - 2 - f * (h - 4)), ImVec2(p1.x - 2, p1.y - 2), COLOR_FILL, 2.0f);
        }
        ImGui::SetItemTooltip("Force sensor %d: %d", i + 1, v);
    }
}

static void draw_device_panel() {
    App& app = g_app;
    const RudderSnapshot* snap = app.snapshot.get();
    const float em = ImGui::GetFontSize();
    const bool in_use = app.in_use();

    const float bar_w = em * 1.2f + ImGui::GetStyle().ItemSpacing.x;
    const float size = std::clamp(ImGui::GetContentRegionAvail().x - bar_w, em * 10, em * 20);
    draw_rudder_view(size);
    ImGui::SameLine();
    draw_updown_bar(size);

    ImGui::Spacing();
    ImGui::BeginDisabled(!in_use);
    push_button_colors(ImVec4(0.18f, 0.36f, 0.58f, 1));
    if (ImGui::Button("Recenter", ImVec2(em * 7, ImGui::GetFrameHeight() * 1.5f))) {
        app.reader.recenter();
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Take the current position as neutral (no output).\n"
                          "The 3dRudder zeroes itself when you put your feet on it,\n"
                          "this is for when it drifts or you want a different neutral.");
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (snap && snap->recentered) {
        ImGui::TextUnformatted("Neutral: recentered.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            app.reader.clear_neutral();
        }
        ImGui::SetItemTooltip("Go back to the neutral the 3dRudder set when your feet went on.");
    } else {
        ImGui::TextUnformatted("Neutral: where your feet were when it started.");
    }
    ImGui::TextDisabled(in_use ? "Hold your feet where they should give no output and click."
                               : "Can be set while the 3dRudder is in use.");
    ImGui::EndGroup();

    ImGui::SeparatorText("Sensors");
    draw_sensors();
    // Right-aligned columns, the font isn't monospaced.
    const bool have = snap && snap->have_frame;
    const float col_w = ImGui::CalcTextSize("-00000").x + em * 0.6f;
    const float x0 = ImGui::GetCursorPosX() + ImGui::CalcTextSize("Accelerometer").x + em * 0.5f;
    auto imu_row = [&](const char* label, const int16_t (&v)[3]) {
        ImGui::TextDisabled("%s", label);
        for (int i = 0; i < 3; i++) {
            char num[16];
            snprintf(num, sizeof(num), have ? "%d" : "--", v[i]);
            ImGui::SameLine(x0 + col_w * (i + 1) - ImGui::CalcTextSize(num).x);
            ImGui::TextDisabled("%s", num);
        }
    };
    const RudderFrame none;
    imu_row("Accelerometer", have ? snap->frame.accel : none.accel);
    imu_row("Gyro", have ? snap->frame.gyro : none.gyro);

    ImGui::SeparatorText("Device messages");
    if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_FrameStyle)) {
        if (snap) {
            for (const std::string& line : snap->log) {
                ImGui::TextUnformatted(line.c_str());
            }
            // Follow new lines unless scrolled up.
            static size_t last_count = 0;
            if (snap->log.size() != last_count && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - em) {
                ImGui::SetScrollHereY(1.0f);
            }
            last_count = snap->log.size();
        }
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Output

static void draw_stick_indicator(Stick stick, const float (&value)[2]) {
    const float em = ImGui::GetFontSize();
    const float radius = em * 1.3f;
    const ImVec2 size(radius * 2, radius * 2);
    ImGui::BeginGroup();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + radius, p.y + radius);
    const Color3& col = STICK_COLORS[(int) stick];
    const bool centered = stick_byte(value[0]) == 128 && stick_byte(value[1]) == 128;
    dl->AddCircleFilled(c, radius, IM_COL32(40, 42, 48, 255));
    dl->AddCircle(c, radius, ImGui::GetColorU32(ImVec4(col.r, col.g, col.b, 1)), 0, 1.5f);
    dl->AddLine(ImVec2(c.x - radius, c.y), ImVec2(c.x + radius, c.y), IM_COL32(255, 255, 255, 30));
    dl->AddLine(ImVec2(c.x, c.y - radius), ImVec2(c.x, c.y + radius), IM_COL32(255, 255, 255, 30));
    const ImVec2 dot(c.x + value[0] * (radius - 3), c.y + value[1] * (radius - 3));
    dl->AddCircleFilled(dot, em * 0.22f,
        centered ? IM_COL32(120, 120, 130, 255)
                 : ImGui::GetColorU32(ImVec4(col.r + (1 - col.r) * 0.4f, col.g + (1 - col.g) * 0.4f, col.b + (1 - col.b) * 0.4f, 1)));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(STICK_NAMES[(int) stick]);
    ImGui::TextDisabled("%+.2f, %+.2f", value[0], -value[1]);
    ImGui::EndGroup();
    ImGui::EndGroup();
}

static void draw_output() {
    const GamepadState state = g_app.snapshot ? g_app.snapshot->state : GamepadState();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const float slot_w = em * 2.6f + style.ItemSpacing.x +
                         std::max(ImGui::CalcTextSize(STICK_NAMES[(int) Stick::Right]).x,
                             ImGui::CalcTextSize("-0.00, -0.00").x * 1.15f);
    const float x0 = ImGui::GetCursorPosX();
    draw_stick_indicator(Stick::Left, state.sticks[(int) Stick::Left]);
    ImGui::SameLine(x0 + slot_w + em);
    draw_stick_indicator(Stick::Right, state.sticks[(int) Stick::Right]);
    for (int t = 0; t < TRIGGER_COUNT; t++) {
        const Color3& c = BUTTON_COLORS[(int) trigger_button((Trigger) t)];
        char overlay[16];
        snprintf(overlay, sizeof(overlay), "%.2f", state.triggers[t]);
        if (t > 0) {
            ImGui::SameLine(0, em);
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TRIGGER_NAMES[t]);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(c.r, c.g, c.b, 1));
        ImGui::ProgressBar(state.triggers[t], ImVec2(em * 6, em * 1.1f), overlay);
        ImGui::PopStyleColor();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Buttons");
    bool any = false;
    for (int b = 0; b < BUTTON_COUNT; b++) {
        if (state.buttons & button_mask((Button) b)) {
            const Color3& c = BUTTON_COLORS[b];
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c.r, c.g, c.b, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c.r, c.g, c.b, 1));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c.r, c.g, c.b, 1));
            ImGui::SmallButton(BUTTON_NAMES[b]);
            ImGui::PopStyleColor(3);
            any = true;
        }
    }
    if (!any) {
        ImGui::SameLine();
        ImGui::TextDisabled("none");
    }
    if (!g_app.running) {
        ImGui::TextDisabled("Stopped: the receiver gets nothing pressed.");
    }
}

// ---------------------------------------------------------------------------
// Mapping

// Where a button level (0..1) is, in axis units.
static float button_point(const AxisMapping& m, float level) {
    return m.deadzone + level * (m.range - m.deadzone);
}

// The axis value on a scale, with the deadzone shaded, the full output points
// marked, button thresholds ticked and the output filled in from the center.
static void draw_axis_bar(int a, const AxisMapping& m, float value, bool valid, float width) {
    const float em = ImGui::GetFontSize();
    const float h = em * 1.25f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + h);
    ImGui::Dummy(ImVec2(width, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float scale = std::max(AXIS_LIMITS[a].display, m.range * 1.15f);
    const float cx = (p0.x + p1.x) / 2;
    // Invert flips which side is which, so the bar follows the output.
    const float dir = m.invert ? -1.0f : 1.0f;
    auto x_of = [&](float v) { return cx + std::clamp(v / scale, -1.0f, 1.0f) * width / 2; };
    const bool buttons = m.button_pos != NO_BUTTON || m.button_neg != NO_BUTTON;
    const bool used = m.target != AxisTarget::None || buttons;

    dl->AddRectFilled(p0, p1, IM_COL32(32, 34, 40, 255), 3.0f);
    if (used) {
        const float level = valid ? axis_level(m, value) : 0.0f;
        if (level != 0.0f) {
            const float edge = button_point(m, std::abs(level));
            const float sign = level < 0 ? -1.0f : 1.0f;
            const float x0 = x_of(sign * m.deadzone), x1 = x_of(sign * edge);
            dl->AddRectFilled(ImVec2(std::min(x0, x1), p0.y + 2), ImVec2(std::max(x0, x1), p1.y - 2), COLOR_FILL, 2.0f);
        }
        dl->AddRectFilled(ImVec2(x_of(-m.deadzone), p0.y), ImVec2(x_of(m.deadzone), p1.y), IM_COL32(255, 255, 255, 22));
        for (float s : { -1.0f, 1.0f }) {
            const float x = x_of(s * m.range);
            dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 120), 1.5f);
        }
        const float bp = button_point(m, m.button_level);
        if (m.button_pos != NO_BUTTON) {
            const float x = x_of(bp);
            dl->AddTriangleFilled(ImVec2(x - em * 0.3f, p1.y), ImVec2(x + em * 0.3f, p1.y), ImVec2(x, p1.y - em * 0.4f),
                COLOR_BUTTON_MARK);
        }
        if (m.button_neg != NO_BUTTON) {
            const float x = x_of(-bp);
            dl->AddTriangleFilled(ImVec2(x - em * 0.3f, p1.y), ImVec2(x + em * 0.3f, p1.y), ImVec2(x, p1.y - em * 0.4f),
                COLOR_BUTTON_MARK);
        }
    }
    dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p1.y), IM_COL32(255, 255, 255, 50));
    if (valid) {
        const float x = x_of(value * dir);
        dl->AddTriangleFilled(ImVec2(x - em * 0.35f, p0.y), ImVec2(x + em * 0.35f, p0.y), ImVec2(x, p0.y + em * 0.45f),
            COLOR_MARKER);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), COLOR_MARKER, 2.0f);
    }
}

static bool button_combo(const char* id, int& button) {
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
    if (ImGui::BeginCombo(id, button == NO_BUTTON ? "None" : BUTTON_NAMES[button])) {
        if (ImGui::Selectable("None", button == NO_BUTTON)) {
            button = NO_BUTTON;
            changed = true;
        }
        for (int b = 0; b < BUTTON_COUNT; b++) {
            if (ImGui::Selectable(BUTTON_NAMES[b], button == b)) {
                button = b;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void draw_axis(int a) {
    App& app = g_app;
    AxisMapping& m = app.config.axes[a];
    const AxisLimits& lim = AXIS_LIMITS[a];
    const float em = ImGui::GetFontSize();
    const RudderSnapshot* snap = app.snapshot.get();
    const bool valid = app.in_use();
    const float value = valid ? snap->relative[a] : 0.0f;
    const bool degrees = axis_in_degrees((RudderAxis) a);
    bool changed = false;

    ImGui::PushID(a);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(RUDDER_AXIS_NAMES[a]);
    if (RUDDER_AXIS_DESCRIPTIONS[a][0]) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", RUDDER_AXIS_DESCRIPTIONS[a]);
    }
    ImGui::SameLine();
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    char text[32];
    if (valid) {
        format_value(a, value, text, sizeof(text));
    } else {
        snprintf(text, sizeof(text), "--");
    }
    ImGui::SetCursorPosX(right - ImGui::CalcTextSize("+00.0°").x);
    ImGui::TextUnformatted(text);

    draw_axis_bar(a, m, value, valid, ImGui::GetContentRegionAvail().x);

    const float label_w = em * 5.5f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Output");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(em * 9);
    if (ImGui::BeginCombo("##target", AXIS_TARGET_NAMES[(int) m.target])) {
        for (int t = 0; t < AXIS_TARGET_COUNT; t++) {
            if (ImGui::Selectable(AXIS_TARGET_NAMES[t], (int) m.target == t)) {
                m.target = (AxisTarget) t;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip(
        "Sticks: the + side (clockwise, toes down, tilt right, up) moves the stick right / up.\n"
        "L2 / R2: the + side drives R2, the - side drives L2.\n"
        "L2 or R2 alone: driven by the + side only (invert for the other side).\n"
        "Several axes can drive the same output; they add up.");
    ImGui::SameLine(0, em);
    changed |= ImGui::Checkbox("Invert", &m.invert);
    ImGui::SetItemTooltip("Swap the + and - sides, for the analog output and the buttons.");
    ImGui::SameLine(0, em);
    const float out = valid && m.target != AxisTarget::None ? axis_output(m, value) : 0.0f;
    ImGui::TextDisabled("output %+.2f", out);

    const float slider_w = ImGui::GetContentRegionAvail().x - label_w;
    char format[32];
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Sensitivity");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(slider_w);
    snprintf(format, sizeof(format), degrees ? "full at %%.1f°" : "full at %%.2f");
    if (ImGui::SliderFloat("##range", &m.range, lim.range_min, lim.range_max, format)) {
        m.range = std::max(m.range, m.deadzone + lim.range_min / 2);
        changed = true;
    }
    ImGui::SetItemTooltip("How far from neutral for full output.\nLower = more sensitive.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Deadzone");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(slider_w);
    snprintf(format, sizeof(format), degrees ? "%%.1f°" : "%%.2f");
    if (ImGui::SliderFloat("##deadzone", &m.deadzone, 0.0f, lim.deadzone_max, format)) {
        m.range = std::max(m.range, m.deadzone + lim.range_min / 2);
        changed = true;
    }
    ImGui::SetItemTooltip("How far from neutral with no output.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Curve");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(slider_w);
    ImGui::BeginDisabled(m.target == AxisTarget::None);
    changed |= ImGui::SliderFloat("##curve", &m.curve, 1.0f, 3.0f, m.curve == 1.0f ? "linear" : "%.1f");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("1 = linear. Higher = finer control near neutral, the same full output.");

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Buttons");
    ImGui::SameLine(label_w);
    ImGui::TextUnformatted("+");
    ImGui::SameLine();
    changed |= button_combo("##bpos", m.button_pos);
    ImGui::SameLine(0, em * 0.7f);
    ImGui::TextUnformatted("-");
    ImGui::SameLine();
    changed |= button_combo("##bneg", m.button_neg);
    ImGui::SameLine(0, em * 0.7f);
    ImGui::BeginDisabled(m.button_pos == NO_BUTTON && m.button_neg == NO_BUTTON);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    changed |= ImGui::SliderFloat("##blevel", &m.button_level, 0.0f, 1.0f, "at %.2f");
    ImGui::SetItemTooltip("Buttons pressed when the axis is this far between the deadzone (0) and full (1),\n"
                          "on the + or - side. Marked under the bar. Works with or without an analog output.");
    ImGui::EndDisabled();

    ImGui::PopID();
    if (changed) {
        app.mapping_changed();
    }
}

// ---------------------------------------------------------------------------

static void draw_top_bar() {
    App& app = g_app;
    Config& cfg = app.config;
    const float em = ImGui::GetFontSize();
    const RudderSnapshot* snap = app.snapshot.get();

    const ImVec2 big(em * 9, ImGui::GetFrameHeight() * 1.7f);
    push_button_colors(app.running ? ImVec4(0.13f, 0.52f, 0.22f, 1) : ImVec4(0.45f, 0.17f, 0.17f, 1));
    if (ImGui::Button(app.running ? "RUNNING" : "STOPPED", big)) {
        app.running = !app.running;
    }
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip(app.running ? "Click to stop: releases everything and stops sending inputs.\n"
                                        "The 3dRudder is still read and the output still shown."
                                      : "Click to start sending inputs to the receiver.");

    ImGui::SameLine(0, em * 1.5f);
    ImGui::BeginGroup();
    const float label_w = em * 6;
    // Receiver
    static char ip_buffer[64] = {};
    static bool ip_init = false;
    if (!ip_init) {
        strncpy_s(ip_buffer, cfg.receiver_ip.c_str(), _TRUNCATE);
        ip_init = true;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Receiver IP");
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(em * 9);
    if (ImGui::InputTextWithHint("##ip", "e.g. 192.168.1.50", ip_buffer, sizeof(ip_buffer),
            ImGuiInputTextFlags_CharsNoBlank)) {
        cfg.receiver_ip = ip_buffer;
        app.sender.set_receiver(cfg.receiver_ip);
        app.mark_dirty();
    }
    ImGui::SetItemTooltip("IP address of the HID Receiver. Inputs are sent as UDP to port 42734.");
    ImGui::SameLine();
    if (cfg.receiver_ip.empty()) {
        ImGui::TextDisabled("not set");
    } else if (!app.sender.address_valid()) {
        ImGui::TextColored(COLOR_ERROR, "invalid IPv4 address");
    } else if (app.sender.send_error()) {
        ImGui::TextColored(COLOR_ERROR, "send failed");
    } else if (app.running) {
        ImGui::TextColored(COLOR_OK, "sending (%llu packets)", (unsigned long long) app.sender.packets_sent());
    } else {
        ImGui::TextDisabled("idle");
    }

    // Device
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("3dRudder");
    ImGui::SameLine(label_w);
    if (!snap) {
        ImGui::TextDisabled("looking...");
    } else if (!snap->connected) {
        ImGui::TextColored(COLOR_ERROR, "%s", snap->error.c_str());
    } else {
        ImGui::TextUnformatted(snap->device_name.c_str());
        ImGui::SameLine();
        if (!snap->have_frame) {
            ImGui::TextColored(COLOR_WARN, "connected, no data");
        } else {
            const int status = snap->frame.status;
            ImGui::TextColored(rudder_in_use(status) ? COLOR_OK : COLOR_WARN, "%s", rudder_status_name(status));
            ImGui::SameLine();
            ImGui::TextDisabled("(status %d, %.0f reports/s)", status, snap->rate);
        }
    }
    ImGui::EndGroup();
}

static void draw_ui() {
    App& app = g_app;
    Config& cfg = app.config;
    ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

    draw_top_bar();
    ImGui::Spacing();

    const float left_w = std::max(em * 22, ImGui::GetContentRegionAvail().x * 0.38f);

    if (ImGui::BeginChild("left", ImVec2(left_w, 0))) {
        ImGui::SeparatorText("3dRudder");
        draw_device_panel();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("right", ImVec2(0, 0))) {
        ImGui::SeparatorText("Axes");
        for (int a = 0; a < RUDDER_AXIS_COUNT; a++) {
            if (a > 0) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }
            draw_axis(a);
        }
        ImGui::Spacing();
        if (ImGui::SmallButton("Reset to defaults")) {
            ImGui::OpenPopup("reset");
        }
        if (ImGui::BeginPopup("reset")) {
            ImGui::TextUnformatted("Reset all the axis settings?");
            if (ImGui::Button("Reset")) {
                default_axes(cfg.axes);
                app.mapping_changed();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SeparatorText("Output");
        draw_output();
    }
    ImGui::EndChild();

    ImGui::End();
}

// ---------------------------------------------------------------------------

static LRESULT WINAPI main_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return true;
    }
    switch (msg) {
        case WM_SIZE:
            capture_window_placement();
            if (wparam == SIZE_MINIMIZED) {
                return 0;
            }
            g_resize_width = (UINT) LOWORD(lparam);
            g_resize_height = (UINT) HIWORD(lparam);
            return 0;
        case WM_MOVE:
            capture_window_placement();
            return 0;
        case WM_CLOSE:
            capture_window_placement();
            break;
        case WM_GETMINMAXINFO: {
            float s = GetDpiForWindow(hwnd) / 96.0f;
            auto* mmi = (MINMAXINFO*) lparam;
            mmi->ptMinTrackSize.x = (LONG) (960 * s);
            mmi->ptMinTrackSize.y = (LONG) (700 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            g_pending_dpi_scale = HIWORD(wparam) / 96.0f;
            const RECT* r = (const RECT*) lparam;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wparam & 0xfff0) == SC_KEYMENU) {  // disable ALT application menu
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_cmd) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    App& app = g_app;
    app.config_path = app_data_dir() + L"\\settings.json";
    load_config(app.config_path, app.config);
    if (wcsstr(GetCommandLineW(), L"--start") != nullptr) {
        app.running = true;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"RudderHidTransmitter";
    RegisterClassExW(&wc);

    // Restore the window where it was last time, but only if its title bar
    // would still be reachable (monitors may have changed since).
    const WindowRect& saved = app.config.window;
    bool restore = false;
    if (saved.valid) {
        RECT title = { saved.x, saved.y, saved.x + saved.w, saved.y + GetSystemMetrics(SM_CYCAPTION) };
        if (HMONITOR mon = MonitorFromRect(&title, MONITOR_DEFAULTTONULL)) {
            MONITORINFO mi = { sizeof(mi) };
            RECT visible;
            restore = GetMonitorInfoW(mon, &mi) && IntersectRect(&visible, &title, &mi.rcWork) &&
                      visible.right - visible.left >= 100;
        }
    }

    const float primary_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    HWND hwnd = restore
        ? CreateWindowW(wc.lpszClassName, L"3dRudder HID Transmitter", WS_OVERLAPPEDWINDOW,
              saved.x, saved.y, saved.w, saved.h, nullptr, nullptr, instance, nullptr)
        : CreateWindowW(wc.lpszClassName, L"3dRudder HID Transmitter", WS_OVERLAPPEDWINDOW,
              (int) (80 * primary_scale), (int) (80 * primary_scale), (int) (1180 * primary_scale),
              (int) (860 * primary_scale), nullptr, nullptr, instance, nullptr);
    app.main_hwnd = hwnd;
    update_icon(hwnd);
    const float scale = GetDpiForWindow(hwnd) / 96.0f;

    if (!create_device(hwnd)) {
        cleanup_device();
        MessageBoxW(nullptr, L"Failed to initialize Direct3D 11.", L"3dRudder HID Transmitter", MB_ICONERROR);
        return 1;
    }

    app.sender.set_receiver(app.config.receiver_ip);
    app.sender.start();
    app.mapping_changed();
    app.config_dirty = false;
    app.reader.start(&app.sender);

    if (restore) {
        WINDOWPLACEMENT wp = { sizeof(wp) };
        GetWindowPlacement(hwnd, &wp);
        wp.rcNormalPosition = { saved.x, saved.y, saved.x + saved.w, saved.y + saved.h };
        wp.showCmd = show_cmd;
        if (saved.maximized && (show_cmd == SW_SHOWNORMAL || show_cmd == SW_SHOW || show_cmd == SW_SHOWDEFAULT)) {
            wp.showCmd = SW_SHOWMAXIMIZED;
        }
        SetWindowPlacement(hwnd, &wp);
    } else {
        ShowWindow(hwnd, show_cmd);
    }
    UpdateWindow(hwnd);
    g_track_placement = true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    apply_style(scale);
    // Latin plus the degree sign.
    static const ImWchar glyph_ranges[] = { 0x0020, 0x00FF, 0 };
    io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, nullptr, glyph_ranges);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_device_context);

    bool done = false;
    while (!done) {
        const bool minimized = IsIconic(hwnd) != 0;
        if (minimized || g_swap_chain_occluded) {
            // Nothing to draw: sleep until something happens.
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) {
            break;
        }

        app.sender.set_active(app.running);
        if (app.config_dirty && GetTickCount64() - app.config_dirty_since >= 1000) {
            save_now();
        }

        // Also while minimized, the taskbar shows the icon.
        app.snapshot = app.reader.snapshot();
        update_icon(hwnd);

        if (minimized) {
            continue;
        }
        if (g_swap_chain_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            continue;
        }
        g_swap_chain_occluded = false;

        if (g_resize_width != 0 && g_resize_height != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_width = g_resize_height = 0;
            create_render_target();
        }
        if (g_pending_dpi_scale > 0) {
            apply_style(g_pending_dpi_scale);
            g_pending_dpi_scale = 0;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_ui();
        ImGui::Render();

        const float clear[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
        g_device_context->OMSetRenderTargets(1, &g_render_target_view, nullptr);
        g_device_context->ClearRenderTargetView(g_render_target_view, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_swap_chain->Present(1, 0);
        g_swap_chain_occluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    app.running = false;
    app.reader.stop();
    app.sender.set_active(false);
    app.sender.stop();
    if (app.config_dirty) {
        save_now();
    }

    app.snapshot.reset();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    cleanup_device();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    WSACleanup();
    CoUninitialize();
    return 0;
}
