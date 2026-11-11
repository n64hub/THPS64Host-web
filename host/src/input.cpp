// Controller input for the THPS hosts: SDL game controllers + keyboard through remappable
// bindings, plus the scripted --press input (and --control presses, control.cpp) used by
// headless tests.
//
// Bindings (up to four per N64 input; keyboard ones count on port 1 only):
//   CLI build (default): <config>/controls.cfg, lines "INPUT = FIELD, FIELD ..." (written with
//     the defaults on first run; see write_controls_cfg for the names).
//   THPS_FRONTEND build: RecompFrontend's recompinput profiles, edited in the Controls tab and
//     saved in <config>/controls.json.
// Defaults follow THPS1's own in-game layout (OPTIONS > PLAYER 1 CONTROLS): the four tricks are on
// the C buttons (C-down = ollie, C-left = flip, C-right = grab, C-up = grind; the game reads A and B
// only in menus). So a controller's face buttons are the trick buttons, as in the PlayStation and
// Xbox Tony Hawk games, and A / B also send N64 A / B for the menus:
//   south (Xbox A) = N64 A + C-down (accept / ollie)   east (Xbox B) = N64 B + C-right (back / grab)
//   west (Xbox X)  = C-left (flip)                      north (Xbox Y) = C-up (grind)
//   LB = L, RB + RT = R (spin), LT = Z, Start = START, D-pad + left stick = move, right stick = C.
// Keyboard: arrows = stick, Space = A + C-down, D = B, I/K/J/L = C up/down/left/right, Q = L,
// E = R, S = Z, Return = START. That is the "thps-xbox" preset (the default). The "n64" preset is
// the plain N64 layout (A south, B west, C buttons on the right stick / Y / B / RB / RS), selectable
// with --controls-preset / --control preset. A controls.cfg that is still exactly a layout an earlier
// build generated (the n64 one, written by builds before Q19) is moved to thps-xbox once, with a
// backup (input_load_bindings); one the user edited, or that says "preset = n64", is left alone.
//
// Ports (THPS1 is a one- or two-player game): port 1 has the keyboard and the first controller
// that presses a button, port 2 the next controller that presses one. Every controller uses the
// same bindings. Adapted from CBFD-Recompiled host/src/frontend.cpp (MIT), which does the same
// for four ports.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <vector>

#define SDL_MAIN_HANDLED
#include "sdl_include.h"

#include "librecomp/game.hpp"
#ifdef THPS_FRONTEND
#include "recompinput/recompinput.h"
#include "recompinput/input_mapping.h"
#include "recompinput/profiles.h"
#include "recompinput/input_state.h"
#endif

#include "host.h"
#ifdef THPS_MENU
#include "ui/ui_host.h"
#endif

namespace {

constexpr int k_ports = 2;
constexpr float k_axis_on = 0.5f;     // analog -> digital (recompinput's threshold)
constexpr float k_deadzone = 0.15f;   // CLI build stick deadzone

// N64 inputs in recompinput's order (GameInput::A .. DPAD_RIGHT, then the stick directions).
enum In { A, B, Z, L, R, START, CU, CD, CL, CR, DU, DD, DL, DR, XNEG, XPOS, YPOS, YNEG, IN_COUNT };
const char* k_in_names[IN_COUNT] = {"a", "b", "z", "l", "r", "start", "c_up", "c_down", "c_left", "c_right",
                                    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
                                    "stick_left", "stick_right", "stick_up", "stick_down"};
const uint16_t k_in_bits[14] = {0x8000, 0x4000, 0x2000, 0x0020, 0x0010, 0x1000, 0x0008,
                                0x0004, 0x0002, 0x0001, 0x0800, 0x0400, 0x0200, 0x0100};

struct Field {
    enum Type { None, Key, Button, Axis } type = None;
    int id = 0;  // scancode | SDL_GameControllerButton | +/-(SDL_GameControllerAxis + 1)
};
using Bindings = std::array<std::vector<Field>, IN_COUNT>;

Field key(SDL_Scancode s) { return {Field::Key, int(s)}; }
Field btn(SDL_GameControllerButton b) { return {Field::Button, int(b)}; }
Field axis(SDL_GameControllerAxis a, bool positive) { return {Field::Axis, positive ? int(a) + 1 : -(int(a) + 1)}; }

enum class Preset { ThpsXbox, N64 };
const char* preset_name(Preset p) { return p == Preset::N64 ? "n64" : "thps-xbox"; }
bool parse_preset(const std::string& s, Preset& p) {
    if (s == "thps-xbox" || s == "xbox" || s == "thps") { p = Preset::ThpsXbox; return true; }
    if (s == "n64" || s == "raw") { p = Preset::N64; return true; }
    return false;
}

Bindings thps_xbox_bindings() {
    Bindings b;
    b[A] = {key(SDL_SCANCODE_SPACE), btn(SDL_CONTROLLER_BUTTON_A)};
    b[B] = {key(SDL_SCANCODE_D), btn(SDL_CONTROLLER_BUTTON_B)};
    b[Z] = {key(SDL_SCANCODE_S), axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT, true)};
    b[L] = {key(SDL_SCANCODE_Q), btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)};
    b[R] = {key(SDL_SCANCODE_E), btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true)};
    b[START] = {key(SDL_SCANCODE_RETURN), key(SDL_SCANCODE_KP_ENTER), btn(SDL_CONTROLLER_BUTTON_START)};
    b[CU] = {key(SDL_SCANCODE_I), btn(SDL_CONTROLLER_BUTTON_Y), axis(SDL_CONTROLLER_AXIS_RIGHTY, false)};
    b[CD] = {key(SDL_SCANCODE_K), key(SDL_SCANCODE_SPACE), btn(SDL_CONTROLLER_BUTTON_A), axis(SDL_CONTROLLER_AXIS_RIGHTY, true)};
    b[CL] = {key(SDL_SCANCODE_J), btn(SDL_CONTROLLER_BUTTON_X), axis(SDL_CONTROLLER_AXIS_RIGHTX, false)};
    b[CR] = {key(SDL_SCANCODE_L), btn(SDL_CONTROLLER_BUTTON_B), axis(SDL_CONTROLLER_AXIS_RIGHTX, true)};
    b[DU] = {btn(SDL_CONTROLLER_BUTTON_DPAD_UP)};
    b[DD] = {btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)};
    b[DL] = {btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)};
    b[DR] = {btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)};
    b[XNEG] = {key(SDL_SCANCODE_LEFT), axis(SDL_CONTROLLER_AXIS_LEFTX, false)};
    b[XPOS] = {key(SDL_SCANCODE_RIGHT), axis(SDL_CONTROLLER_AXIS_LEFTX, true)};
    b[YPOS] = {key(SDL_SCANCODE_UP), axis(SDL_CONTROLLER_AXIS_LEFTY, false)};
    b[YNEG] = {key(SDL_SCANCODE_DOWN), axis(SDL_CONTROLLER_AXIS_LEFTY, true)};
    return b;
}

// The plain N64 layout (recompinput's generic one, and the defaults before Q19). THPS1 reads no A/B
// in game, so with this the south/west buttons do nothing on the skateboard: the tricks are on
// the right stick / Y / B / RB / RS (the C buttons).
Bindings n64_bindings() {
    Bindings b = thps_xbox_bindings();
    b[B] = {key(SDL_SCANCODE_D), btn(SDL_CONTROLLER_BUTTON_X)};
    b[R] = {key(SDL_SCANCODE_E), axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true)};
    b[CU] = {key(SDL_SCANCODE_I), axis(SDL_CONTROLLER_AXIS_RIGHTY, false), btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK)};
    b[CD] = {key(SDL_SCANCODE_K), axis(SDL_CONTROLLER_AXIS_RIGHTY, true), btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)};
    b[CL] = {key(SDL_SCANCODE_J), axis(SDL_CONTROLLER_AXIS_RIGHTX, false), btn(SDL_CONTROLLER_BUTTON_Y)};
    b[CR] = {key(SDL_SCANCODE_L), axis(SDL_CONTROLLER_AXIS_RIGHTX, true), btn(SDL_CONTROLLER_BUTTON_B)};
    return b;
}

Bindings preset_bindings(Preset p) { return p == Preset::N64 ? n64_bindings() : thps_xbox_bindings(); }
Bindings default_bindings() { return thps_xbox_bindings(); }

// Layouts earlier builds wrote into controls.cfg on their own (not edited by the user). A file
// that still is one of these is moved to the current default on load. Add a layout here when the
// generated default changes again.
struct GeneratedLayout { const char* name; Bindings (*make)(); };
const GeneratedLayout k_generated_layouts[] = {
    {"pre-Q19 N64 layout (Q17/Q18 builds)", n64_bindings},
};

bool same_field(const Field& a, const Field& b) { return a.type == b.type && a.id == b.id; }

// Same fields for every input, in any order. `seen` = which inputs the file had a line for; an
// input without a line counts as equal (a missing line takes the default on load anyway).
bool same_bindings(const Bindings& a, const Bindings& b, const std::array<bool, IN_COUNT>* seen = nullptr) {
    for (int i = 0; i < IN_COUNT; i++) {
        if (seen && !(*seen)[i]) continue;
        if (a[i].size() != b[i].size()) return false;
        for (const Field& f : a[i]) {
            if (std::none_of(b[i].begin(), b[i].end(), [&](const Field& g) { return same_field(f, g); })) return false;
        }
    }
    return true;
}

std::string field_name(const Field& f) {
    switch (f.type) {
        case Field::Key: return std::string("key:") + SDL_GetScancodeName(SDL_Scancode(f.id));
        case Field::Button: return std::string("button:") + SDL_GameControllerGetStringForButton(SDL_GameControllerButton(f.id));
        case Field::Axis:
            return std::string("axis:") + SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis(std::abs(f.id) - 1)) + (f.id > 0 ? "+" : "-");
        default: return "none";
    }
}

bool parse_field(std::string s, Field& f) {
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
    if (s == "none" || s.empty()) { f = {}; return true; }
    if (s.rfind("key:", 0) == 0) {
        SDL_Scancode c = SDL_GetScancodeFromName(s.substr(4).c_str());
        if (c == SDL_SCANCODE_UNKNOWN) return false;
        f = key(c);
        return true;
    }
    if (s.rfind("button:", 0) == 0) {
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(s.substr(7).c_str());
        if (b == SDL_CONTROLLER_BUTTON_INVALID) return false;
        f = btn(b);
        return true;
    }
    if (s.rfind("axis:", 0) == 0 && s.size() > 6 && (s.back() == '+' || s.back() == '-')) {
        SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(s.substr(5, s.size() - 6).c_str());
        if (a == SDL_CONTROLLER_AXIS_INVALID) return false;
        f = axis(a, s.back() == '+');
        return true;
    }
    return false;
}

std::mutex g_bind_mutex;
Bindings g_bindings = default_bindings();   // CLI build
std::filesystem::path g_controls_cfg;
std::string g_preset = "thps-xbox";          // controls.cfg "preset = ..." line: thps-xbox, n64, or custom (edited)
Preset g_base = Preset::ThpsXbox;            // what a row's "Default" in the Controls tab resets to: the preset loaded or picked last
bool g_ui_backup_done = false;               // the first rebind of a session keeps the file it replaces as controls.cfg.bak
// THPS2/3's split of the east button (input_get): on when some field drives both N64 B and C-right, as thps-xbox's east button does.
// (It used to follow the preset name, so any edit turned it off, and the east button flipped and grabbed at once.)
std::atomic<bool> g_pair_split{true};
std::atomic<uint64_t> g_bind_version{1};     // +1 on every change of the bindings (the Controls tab redraws its rows)

// After any change of g_bindings (g_bind_mutex held).
void bindings_changed_locked() {
    bool shared = false;
    for (const Field& f : g_bindings[B])
        for (const Field& g : g_bindings[CR]) shared = shared || same_field(f, g);
    g_pair_split = shared;
    g_bind_version++;
}

// The game action of each input, as the Controls tab names it. THPS1 reads N64 A / B in menus only; THPS2/3 also read A as ollie and
// B as flip while skating (game.toml b_is_flip_in_game).
const char* input_label_of(int in) {
    constexpr bool flip = thps::game::b_is_flip_in_game;
    const bool thps3 = std::string_view(thps::game::short_name) == "THPS3";
    switch (in) {
        case A: return flip ? "Ollie + menu accept (N64 A)" : "Menu accept (N64 A)";
        case B: return flip ? "Flip + menu back (N64 B)" : "Menu back (N64 B)";
        case Z: return "Z (unused)";
        case L: return flip ? "Spin left / nollie (L)" : "Spin (L)";
        case R: return flip ? (thps3 ? "Spin right / switch / revert (R)" : "Spin right / switch (R)") : "Spin (R)";
        case START: return "Pause (Start)";
        case CU: return "Grind (C-up)";
        case CD: return "Ollie (C-down)";
        case CL: return "Flip trick (C-left)";
        case CR: return "Grab trick (C-right)";
        case DU: return "D-pad up";
        case DD: return "D-pad down";
        case DL: return "D-pad left";
        case DR: return "D-pad right";
        case XNEG: return "Stick left";
        case XPOS: return "Stick right";
        case YPOS: return "Stick up";
        case YNEG: return "Stick down";
        default: return "?";
    }
}

// The menu inputs (N64 A / B: accept and back) may share a button with the play inputs, as the presets do (A = accept + ollie,
// B = back + grab; THPS1 reads A / B in menus only, THPS2/3 split the east button in input_get). Within either group, a button that
// one input takes is taken from the other (a swap).
bool menu_input(int in) { return in == A || in == B; }
bool may_share(int a, int b) { return menu_input(a) != menu_input(b); }

// A field as the Controls tab shows it: "A", "LB", "RT", "L-stick left", "Space".
std::string field_label(const Field& f) {
    switch (f.type) {
        case Field::Key: {
            const char* n = SDL_GetScancodeName(SDL_Scancode(f.id));
            return n && *n ? n : "key " + std::to_string(f.id);
        }
        case Field::Button: {
            static const char* const names[] = {"A", "B", "X", "Y", "Back", "Guide", "Start", "L3", "R3", "LB", "RB", "D-pad up", "D-pad down",
                                                "D-pad left", "D-pad right", "Share", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad"};
            if (f.id >= 0 && f.id < int(sizeof names / sizeof *names)) return names[f.id];
            return "button " + std::to_string(f.id);
        }
        case Field::Axis: {
            const int a = std::abs(f.id) - 1;
            const bool pos = f.id > 0;
            switch (a) {
                case SDL_CONTROLLER_AXIS_LEFTX: return pos ? "L-stick right" : "L-stick left";
                case SDL_CONTROLLER_AXIS_LEFTY: return pos ? "L-stick down" : "L-stick up";
                case SDL_CONTROLLER_AXIS_RIGHTX: return pos ? "R-stick right" : "R-stick left";
                case SDL_CONTROLLER_AXIS_RIGHTY: return pos ? "R-stick down" : "R-stick up";
                case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return pos ? "LT" : "LT (released)";
                case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return pos ? "RT" : "RT (released)";
                default: return "axis " + std::to_string(f.id);
            }
        }
        default: return "";
    }
}

// Copy `path` to `bak` (replacing it) if `path` exists. Returns whether a copy was made.
bool backup_file(const std::filesystem::path& path, const std::filesystem::path& bak, bool replace) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return false;
    if (!replace && std::filesystem::exists(bak, ec)) return false;
    std::filesystem::copy_file(path, bak, std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

// Writes g_bindings / g_preset (a copy taken under g_bind_mutex: call it without the lock).
void write_controls_cfg() {
    Bindings bindings;
    std::string preset;
    Preset base;
    {
        std::lock_guard lock{g_bind_mutex};
        bindings = g_bindings;
        preset = g_preset;
        base = g_base;
    }
    std::filesystem::path tmp = g_controls_cfg;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        out << "# " << thps::game::short_name << " recomp controls: INPUT = FIELD, FIELD ... (keyboard fields count on port 1 only)\n"
               "# FIELD: key:NAME (SDL key name: Space, Return, Left, A ..), button:NAME (a b x y back start\n"
               "# leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright), axis:NAME+ or\n"
               "# axis:NAME- (leftx lefty rightx righty lefttrigger righttrigger), none. a/b/x/y = south/east/west/north.\n"
               "# preset: thps-xbox (tricks on the face buttons, the default), n64 (C buttons on the right stick) or custom.\n"
               "# base (custom only): the layout the bindings were changed from; Default and Reset all in the Controls tab go back to it.\n"
               "# Change it in the settings menu (Esc > Controls) or with --controls-preset NAME; a file without this line that still has the old generated N64\n"
               "# layout is moved to thps-xbox once (the old file is kept as controls.cfg.pre-xbox.bak).\n"
               "preset = " << preset << "\n";
        // A customised layout keeps the layout it was made from: what the Controls tab's marks, a row's Default and Reset all go back to
        // (review 2026-10-26, finding 5: without it a restart turned an edited N64 layout's baseline into the face-button one).
        if (preset == "custom") out << "base = " << preset_name(base) << "\n";
        for (int i = 0; i < IN_COUNT; i++) {
            out << k_in_names[i] << " =";
            for (size_t j = 0; j < bindings[i].size(); j++) out << (j ? ", " : " ") << field_name(bindings[i][j]);
            out << "\n";
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, g_controls_cfg, ec);
}

#ifdef THPS_FRONTEND
using recompinput::GameInput;
GameInput game_input(int in) {
    switch (in) {
        case XNEG: return GameInput::X_AXIS_NEG;
        case XPOS: return GameInput::X_AXIS_POS;
        case YPOS: return GameInput::Y_AXIS_POS;
        case YNEG: return GameInput::Y_AXIS_NEG;
        default: return GameInput(size_t(GameInput::N64_BUTTON_START) + size_t(in));
    }
}
Field from_recomp(const recompinput::InputField& f) {
    switch (f.input_type) {
        case recompinput::InputType::Keyboard: return {Field::Key, f.input_id};
        case recompinput::InputType::ControllerDigital: return {Field::Button, f.input_id};
        case recompinput::InputType::ControllerAnalog: return {Field::Axis, f.input_id};
        default: return {};
    }
}
#endif

// The bindings of one input: our table (CLI build) or recompinput's single-player keyboard and
// controller profiles (frontend build).
void fields_for(int in, std::vector<Field>& out) {
    out.clear();
#ifdef THPS_FRONTEND
    const int profiles[2] = {recompinput::profiles::get_sp_keyboard_profile_index(), recompinput::profiles::get_sp_controller_profile_index()};
    for (int p : profiles) {
        if (p < 0) continue;
        for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
            Field f = from_recomp(recompinput::profiles::get_input_binding(p, game_input(in), i));
            if (f.type != Field::None) out.push_back(f);
        }
    }
#else
    std::lock_guard lock{g_bind_mutex};
    out = g_bindings[in];
#endif
}

std::mutex g_port_mutex;
// The controllers holding ports 1-2 (-1 = none). A controller takes the first free port when it presses a button; the settings menu's
// Controllers list can also assign or swap them (g_port_reqs, applied on the main thread).
std::array<SDL_JoystickID, k_ports> g_ports{-1, -1};
struct PortReq {
    enum Op { Assign, Swap } op;
    SDL_JoystickID id;
    int port;
};
std::vector<PortReq> g_port_reqs;   // g_port_mutex
std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> g_open;   // main thread: the open controllers (port_controllers)

struct Press {
    double at, dur;
    uint16_t buttons;
    float x, y;
};
std::vector<Press> g_presses;
std::mutex g_script_mutex;
uint16_t g_script_buttons = 0;
float g_script_x = 0, g_script_y = 0;

bool controller_any_button(SDL_GameController* c) {
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        if (SDL_GameControllerGetButton(c, SDL_GameControllerButton(b))) return true;
    }
    // Triggers are axes; sticks are left out, so a drifting stick doesn't claim a port.
    return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384 ||
           SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384;
}

// Main thread. Updates the ports; out = the controller of each port (nullptr = none). *connected = how many controllers are open
// (capped at k_ports).
void port_controllers(std::array<SDL_GameController*, k_ports>& out, int* connected = nullptr) {
    std::lock_guard lock{g_port_mutex};
    g_open.clear();
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;
        if (thps::options.no_pads && !SDL_JoystickIsVirtual(i)) continue;  // headless tests: only --control vpad
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        SDL_GameController* c = SDL_GameControllerFromInstanceID(id);
        if (c) g_open.emplace_back(id, c);
    }
    auto find_open = [&](SDL_JoystickID id) -> SDL_GameController* {
        if (id < 0) return nullptr;
        for (auto& [oid, c] : g_open) if (oid == id) return c;
        return nullptr;
    };
    auto say = [&](SDL_JoystickID id, const char* what) {
        SDL_GameController* c = find_open(id);
        std::printf("[thps] input: %s %s\n", c ? SDL_GameControllerName(c) : "(controller)", what);
        std::fflush(stdout);
    };
    // Unplugged: the port is free again. If player 1's controller goes, player 2's becomes player 1 (as before the Controllers list).
    const bool lost0 = g_ports[0] >= 0 && !find_open(g_ports[0]);
    for (auto& p : g_ports) if (p >= 0 && !find_open(p)) p = -1;
    if (lost0 && g_ports[1] >= 0) {
        std::swap(g_ports[0], g_ports[1]);
        say(g_ports[0], "is player 1 now (player 1's controller was disconnected)");
    }
    // The settings menu's requests.
    for (const PortReq& r : g_port_reqs) {
        if (r.op == PortReq::Swap) {
            std::swap(g_ports[0], g_ports[1]);
            std::printf("[thps] input: players swapped (settings menu): port 1 = %s, port 2 = %s\n",
                        find_open(g_ports[0]) ? SDL_GameControllerName(find_open(g_ports[0])) : "keyboard only",
                        find_open(g_ports[1]) ? SDL_GameControllerName(find_open(g_ports[1])) : "none");
            std::fflush(stdout);
            continue;
        }
        if (!find_open(r.id) || r.port < 0 || r.port >= k_ports) continue;
        const auto cur = std::find(g_ports.begin(), g_ports.end(), r.id);
        if (cur != g_ports.end()) {
            if (cur - g_ports.begin() == r.port) continue;
            std::swap(*cur, g_ports[size_t(r.port)]);   // a controller moving to the other port swaps with whatever holds it
        } else {
            g_ports[size_t(r.port)] = r.id;             // the one there (if any) has no port now; it takes a free one when it presses a button
        }
        say(r.id, r.port == 0 ? "is player 1 (settings menu)" : "is player 2 (settings menu)");
    }
    g_port_reqs.clear();
    // The first button press claims the first free port.
    for (auto& [id, c] : g_open) {
        if (std::find(g_ports.begin(), g_ports.end(), id) != g_ports.end() || !controller_any_button(c)) continue;
        for (int p = 0; p < k_ports; p++) {
            if (g_ports[size_t(p)] >= 0) continue;
            g_ports[size_t(p)] = id;
            std::printf("[thps] input: %s takes port %d\n", SDL_GameControllerName(c), p + 1);
            std::fflush(stdout);
            break;
        }
    }
    for (int p = 0; p < k_ports; p++) out[size_t(p)] = find_open(g_ports[size_t(p)]);
    if (connected) *connected = std::min(int(g_open.size()), k_ports);
}

float field_value(SDL_GameController* c, const Uint8* keys, int nkeys, const Field& f) {
    switch (f.type) {
        case Field::Key: return keys && f.id >= 0 && f.id < nkeys && keys[f.id] ? 1.0f : 0.0f;
        case Field::Button:
            return c && f.id >= 0 && f.id < SDL_CONTROLLER_BUTTON_MAX && SDL_GameControllerGetButton(c, SDL_GameControllerButton(f.id)) ? 1.0f : 0.0f;
        case Field::Axis: {
            int a = std::abs(f.id) - 1;
            if (!c || a < 0 || a >= SDL_CONTROLLER_AXIS_MAX) return 0.0f;
            float v = SDL_GameControllerGetAxis(c, SDL_GameControllerAxis(a)) * (1.0f / 32768.0f);
            return std::clamp(f.id < 0 ? -v : v, 0.0f, 1.0f);
        }
        default: return 0.0f;
    }
}

float deadzone(float v) {
    if (std::fabs(v) < k_deadzone) return 0;
    return (v - std::copysign(k_deadzone, v)) / (1.0f - k_deadzone);
}

// Key-downs latched until the game has read them (CLI build). The keyboard is read as current state once per main-thread
// update, so a tap whose down and up both fall between two updates was never seen (menus lost taps under ~0.15 s on Windows).
// Main thread only: `fresh` = key-downs since the last snapshot, `published` = in a snapshot the game has not read yet.
struct KeyLatch {
    std::array<bool, SDL_NUM_SCANCODES> fresh{}, published{};
    bool any_fresh = false, any_published = false;
    uint64_t reads_at_publish = 0;
};
KeyLatch g_key_latch;
std::atomic<uint64_t> g_port0_reads{0};   // input_get(port 1) calls: the game thread has read the newest snapshot

// One controller (or none) and/or the keyboard.
void read_port(SDL_GameController* controller, bool keyboard, uint16_t* buttons, float* x, float* y) {
    int nkeys = 0;
    const Uint8* keys = keyboard ? SDL_GetKeyboardState(&nkeys) : nullptr;
    std::array<Uint8, SDL_NUM_SCANCODES> latched{};
    if (keys && (g_key_latch.any_fresh || g_key_latch.any_published)) {
        nkeys = std::min(nkeys, int(SDL_NUM_SCANCODES));
        for (int k = 0; k < nkeys; k++) latched[k] = keys[k] || g_key_latch.fresh[k] || g_key_latch.published[k];
        keys = latched.data();
    }
    std::vector<Field> fields;
    uint16_t b = 0;
    float cv[IN_COUNT] = {}, kv[IN_COUNT] = {};
    for (int in = 0; in < IN_COUNT; in++) {
        fields_for(in, fields);
        for (const Field& f : fields) {
            float v = field_value(f.type == Field::Key ? nullptr : controller, f.type == Field::Key ? keys : nullptr, nkeys, f);
            (f.type == Field::Key ? kv : cv)[in] += v;
            if (in < 14 && (f.type == Field::Axis ? v >= k_axis_on : v > 0)) b |= k_in_bits[in];
        }
    }
    float cx = std::min(cv[XPOS], 1.0f) - std::min(cv[XNEG], 1.0f);
    float cy = std::min(cv[YPOS], 1.0f) - std::min(cv[YNEG], 1.0f);
#ifdef THPS_FRONTEND
    recompinput::apply_joystick_deadzone(cx, cy, &cx, &cy);
#else
    cx = deadzone(cx), cy = deadzone(cy);
#endif
    float kx = std::min(kv[XPOS], 1.0f) - std::min(kv[XNEG], 1.0f);
    float ky = std::min(kv[YPOS], 1.0f) - std::min(kv[YNEG], 1.0f);
    if (kx != 0 && ky != 0) { kx *= 0.7071f; ky *= 0.7071f; }  // keep diagonals on the unit circle
    *buttons = b;
    *x = std::clamp(cx + kx, -1.0f, 1.0f);
    *y = std::clamp(cy + ky, -1.0f, 1.0f);
}

// --control vpad: one virtual SDL game controller (SDL_JoystickAttachVirtual).
// Two slots: "vpad" and "vpad2" (a second player / hotplug tests).
constexpr int k_vpads = 2;
std::array<SDL_Joystick*, k_vpads> g_vpads{};
std::array<int, k_vpads> g_vpad_indices{-1, -1};

std::array<bool, k_ports> g_rumble{};

// Controller state, read on the main thread (input_update) and handed to the game thread
// (input_get / input_connected). SDL's joystick calls must stay on one thread: with sdl2-compat
// on SDL3, SDL_NumJoysticks from the game thread raced the main thread's event pump and freed
// the same joystick list twice (SIGABRT "pointer being freed was not allocated", Q19).
struct PortSnap {
    bool present = false;
    uint16_t buttons = 0;
    float x = 0, y = 0;
};
std::mutex g_snap_mutex;
std::array<PortSnap, k_ports> g_snap{PortSnap{true}};  // port 1 has a controller from the start
int g_snap_connected = 0;

// The settings menu's Controllers list (main thread writes, the render thread reads).
std::mutex g_padinfo_mutex;
std::vector<thps::InputPad> g_padinfo;

// What a controller is pressing right now, as labels ("A + LB", "L-stick left"); "" = nothing. Main thread (SDL calls).
std::string pressed_now(SDL_GameController* c) {
    std::string s;
    auto add = [&](const Field& f) { s += (s.empty() ? "" : " + ") + field_label(f); };
    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
        if (SDL_GameControllerGetButton(c, SDL_GameControllerButton(b))) add(btn(SDL_GameControllerButton(b)));
    for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) {
        const int v = SDL_GameControllerGetAxis(c, SDL_GameControllerAxis(a));
        if (v > 16384) add(axis(SDL_GameControllerAxis(a), true));
        else if (v < -16384 && a != SDL_CONTROLLER_AXIS_TRIGGERLEFT && a != SDL_CONTROLLER_AXIS_TRIGGERRIGHT) add(axis(SDL_GameControllerAxis(a), false));
    }
    return s;
}

void refresh_padinfo(const std::array<SDL_GameController*, k_ports>& pads) {
    std::vector<thps::InputPad> info;
    for (auto& [id, c] : g_open) {
        thps::InputPad p;
        p.id = int(id);
        const char* name = SDL_GameControllerName(c);
        p.name = name && *name ? name : "Controller";
        p.player = 0;
        for (int i = 0; i < k_ports; i++) if (pads[size_t(i)] == c) p.player = i + 1;
        p.pressed = pressed_now(c);
        info.push_back(std::move(p));
    }
    std::lock_guard lock{g_padinfo_mutex};
    g_padinfo = std::move(info);
}

void refresh_snapshot() {
    std::array<SDL_GameController*, k_ports> pads{};
    int connected = 0;
    port_controllers(pads, &connected);
    refresh_padinfo(pads);
#ifdef THPS_FRONTEND
    bool disabled = recompinput::game_input_disabled();  // a menu is open
#elif defined(THPS_MENU)
    bool disabled = thps::ui::input_blocked();  // the in-game settings menu is open
#else
    bool disabled = false;
#endif
    KeyLatch& kl = g_key_latch;
    if (kl.any_published && g_port0_reads.load() != kl.reads_at_publish) kl.published.fill(false), kl.any_published = false;
    if (disabled) kl.fresh.fill(false), kl.published.fill(false), kl.any_fresh = kl.any_published = false;
    std::array<PortSnap, k_ports> snap{};
    for (int port = 0; port < k_ports; port++) {
        // Port 1 is always there (the keyboard); port 2 when two controllers are open or one was given port 2 (the Controllers list).
        if (port > 0 && connected <= port && !pads[size_t(port)]) continue;
        snap[port].present = true;
        if (!disabled)
            read_port(pads[size_t(port)], port == 0 && !thps::options.no_keyboard, &snap[port].buttons, &snap[port].x, &snap[port].y);
    }
    std::lock_guard lock{g_snap_mutex};
    g_snap = snap;
    g_snap_connected = connected;
    if (kl.any_fresh) {
        for (size_t k = 0; k < kl.fresh.size(); k++) kl.published[k] = kl.published[k] || kl.fresh[k];
        kl.fresh.fill(false);
        kl.any_fresh = false, kl.any_published = true;
        kl.reads_at_publish = g_port0_reads.load();   // under the snapshot lock: a later read saw these keys
    }
}

}  // namespace

void thps::input_load_bindings(const std::filesystem::path& config_dir) {
#ifdef THPS_FRONTEND
    // recompinput's defaults are set before the Controls tab exists (frontend.cpp).
    using recompinput::InputField;
    const Bindings defaults = default_bindings();
    for (int in = 0; in < IN_COUNT; in++) {
        std::vector<InputField> keyboard, controller;
        for (const Field& f : defaults[in]) {
            if (f.type == Field::Key) keyboard.push_back(InputField::keyboard(SDL_Scancode(f.id)));
            else if (f.type == Field::Button) controller.push_back(InputField::controller_digital(SDL_GameControllerButton(f.id)));
            else if (f.type == Field::Axis) controller.push_back(InputField::controller_analog(SDL_GameControllerAxis(std::abs(f.id) - 1), f.id > 0));
        }
        keyboard.resize(std::min(keyboard.size(), recompinput::num_bindings_per_input));
        controller.resize(std::min(controller.size(), recompinput::num_bindings_per_input));
        recompinput::set_default_mapping_for_keyboard(game_input(in), keyboard);
        recompinput::set_default_mapping_for_controller(game_input(in), controller);
    }
    (void)config_dir;
#else
    g_controls_cfg = config_dir / "controls.cfg";
    std::ifstream in(g_controls_cfg);
    if (!in) {
        g_preset = preset_name(Preset::ThpsXbox);
        {
            std::lock_guard lock{g_bind_mutex};
            bindings_changed_locked();
        }
        write_controls_cfg();
        std::printf("[thps] input: wrote default bindings (preset thps-xbox) to %s\n", g_controls_cfg.string().c_str());
        return;
    }
    Bindings b = default_bindings();
    std::array<bool, IN_COUNT> seen{};
    std::string marker, base_marker, line;
    int lines = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        std::string name = line.substr(0, eq == std::string::npos ? 0 : eq);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (name == "preset" || name == "base") {
            std::string& m = name == "preset" ? marker : base_marker;
            m = line.substr(eq + 1);
            m.erase(0, m.find_first_not_of(' '));
            while (!m.empty() && (m.back() == ' ' || m.back() == '\r')) m.pop_back();
            continue;
        }
        int idx = -1;
        for (int i = 0; i < IN_COUNT; i++) if (name == k_in_names[i]) idx = i;
        if (idx < 0) { std::fprintf(stderr, "[thps] controls.cfg: ignoring '%s'\n", line.c_str()); continue; }
        b[idx].clear();
        seen[idx] = true;
        std::stringstream ss(line.substr(eq + 1));
        std::string item;
        while (std::getline(ss, item, ',')) {
            Field f;
            if (!parse_field(item, f)) std::fprintf(stderr, "[thps] controls.cfg: bad binding '%s' for %s\n", item.c_str(), k_in_names[idx]);
            else if (f.type != Field::None) b[idx].push_back(f);
        }
        lines++;
    }
    in.close();
    // A file that is still exactly a layout an earlier build generated (any field order, lines may be
    // missing) is moved to the current default, after a backup. A file with a "preset" line was
    // written by a build that knows presets (the user chose it); one the user edited matches none.
    const GeneratedLayout* old_layout = nullptr;
    if (marker.empty()) {
        for (const GeneratedLayout& g : k_generated_layouts) {
            if (same_bindings(b, g.make(), &seen)) { old_layout = &g; break; }
        }
    }
    // The preset the file names, if its bindings still are that preset's (an edit by hand makes it custom: the Controls tab then
    // checks neither layout); a file without the line is thps-xbox when it has those bindings.
    Preset named = Preset::ThpsXbox, based = Preset::ThpsXbox;
    const bool named_ok = parse_preset(marker, named);
    const bool based_ok = !named_ok && parse_preset(base_marker, based);   // `base = ...`: the layout a custom one was made from
    g_base = named_ok ? named : based_ok ? based : Preset::ThpsXbox;
    if (marker.empty()) g_preset = same_bindings(b, thps_xbox_bindings()) ? preset_name(Preset::ThpsXbox) : "custom";
    else g_preset = named_ok && same_bindings(b, preset_bindings(named)) ? preset_name(named) : "custom";
    if (old_layout) {
        std::filesystem::path bak = g_controls_cfg;
        bak += ".pre-xbox.bak";
        bool kept = backup_file(g_controls_cfg, bak, false);
        b = default_bindings();
        g_preset = preset_name(Preset::ThpsXbox);
        g_base = Preset::ThpsXbox;
        {
            std::lock_guard lock{g_bind_mutex};
            g_bindings = b;
            bindings_changed_locked();
        }
        write_controls_cfg();
        std::printf("[thps] input: controls.cfg had the generated %s; moved to preset thps-xbox "
                    "(A = ollie + accept, B = grab + back, X = flip, Y = grind); old file %s %s\n", old_layout->name,
                    kept ? "saved as" : "already backed up as", bak.string().c_str());
    } else {
        std::lock_guard lock{g_bind_mutex};
        g_bindings = b;
        bindings_changed_locked();
    }
    if (thps::verbose()) std::printf("[thps] input: %d binding line(s) from %s\n", lines, g_controls_cfg.string().c_str());
#endif
}

void thps::input_init() {
#ifndef THPS_FRONTEND
    // Buttons by position, not by printed label: SDL's default swaps A/B and X/Y on Nintendo controllers (Switch Pro, Joy-Con,
    // GameCube adapters) to match their labels, which put ollie on the Switch's east button. The bindings and the presets
    // describe an Xbox-style layout (A = south), so every pad follows it. Must be set before the subsystem opens a controller.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef __EMSCRIPTEN__
    // Emscripten's SDL2 has no haptic subsystem, and asking for it fails the whole init: rumble goes through
    // SDL_GameControllerRumble, which the port plays on the Gamepad API.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
#else
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0) {
#endif
        std::fprintf(stderr, "[thps] input: SDL game controller init failed: %s (keyboard only)\n", SDL_GetError());
    }
#endif
    int pads = 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) continue;
        pads++;
#ifndef THPS_FRONTEND
        SDL_GameControllerOpen(i);
#endif
    }
    if (thps::verbose()) std::printf("[thps] input: %d controller(s); keyboard %s; bindings: %s\n", pads, options.no_keyboard ? "off (--no-keyboard)" : "on",
#ifdef THPS_FRONTEND
                "Controls tab (controls.json)"
#else
                g_controls_cfg.string().c_str()
#endif
    );
    std::fflush(stdout);
}

// CLI build: open/close controllers as they come and go (recompinput does it in the frontend build).
void thps::input_event(const SDL_Event& ev) {
#ifndef THPS_FRONTEND
    if (ev.type == SDL_KEYDOWN && !ev.key.repeat && !thps::options.no_keyboard) {
        int sc = int(ev.key.keysym.scancode);
        if (sc > 0 && sc < SDL_NUM_SCANCODES) g_key_latch.fresh[size_t(sc)] = true, g_key_latch.any_fresh = true;
    } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
        // input_init already opened the pads that were attached at start, and SDL sends an ADDED event for those too: opening twice
        // would take a second reference, so the one close on removal would leave the unplugged pad open (a leak).
        if (SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(ev.cdevice.which))) return;
        if (SDL_GameController* c = SDL_GameControllerOpen(ev.cdevice.which)) {
            std::printf("[thps] input: controller connected: %s\n", SDL_GameControllerName(c));
            std::fflush(stdout);
        }
    } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
        if (SDL_GameController* c = SDL_GameControllerFromInstanceID(ev.cdevice.which)) {
            std::printf("[thps] input: controller disconnected: %s\n", SDL_GameControllerName(c));
            SDL_GameControllerClose(c);
        }
    }
#else
    (void)ev;
#endif
}

void thps::input_add_press(double at, double dur, uint16_t buttons, float x, float y) {
    g_presses.push_back({at, dur, buttons, x, y});
}

void thps::input_update(double t) {
    uint16_t b = 0;
    float x = 0, y = 0;
    for (const auto& p : g_presses) {
        if (t >= p.at && t < p.at + p.dur) {
            b |= p.buttons;
            if (p.x != 0) x = p.x;
            if (p.y != 0) y = p.y;
        }
    }
    {
        std::lock_guard lock{g_script_mutex};
        g_script_buttons = b, g_script_x = x, g_script_y = y;
    }
    refresh_snapshot();
#ifndef THPS_FRONTEND
    // Rumble (CLI build), on the main thread like the other SDL calls.
    static std::array<bool, k_ports> last{};
    std::array<SDL_GameController*, k_ports> pads{};
    port_controllers(pads);
    for (int i = 0; i < k_ports; i++) {
        if (!pads[size_t(i)]) continue;
        bool on = g_rumble[i];
        if (on != last[i]) SDL_GameControllerRumble(pads[i], on ? 0xC000 : 0, on ? 0xC000 : 0, on ? 10000 : 0);
        last[i] = on;
    }
#endif
}

void thps::input_poll() {
#ifdef THPS_FRONTEND
    recompinput::poll_inputs();
#endif
}

namespace {
std::atomic<uint64_t> g_b_dropped{0};     // reads where the thps-xbox filter dropped N64 B / C-right (tests: control "inputstate")
std::atomic<uint64_t> g_cr_dropped{0};
std::atomic<uint32_t> g_last_read{0};     // port 1 buttons as the game thread last read them (before scripted input)
std::atomic<uint32_t> g_last_read2{0};    // port 2 buttons as the game thread last read them (tests: two pads)
}

// Game thread: the main thread's latest snapshot plus the scripted input (port 1).
bool thps::input_get(int port, uint16_t* buttons, float* x, float* y) {
    *buttons = 0;
    *x = *y = 0;
    if (port < 0 || port >= k_ports) return false;
    // THPS1's OPTIONS > PLAYER 2 CONTROLS screen reads only port 2, so on a machine with one controller (or just the keyboard) it could
    // never be left (hand play 2026-10-14, "the controls menu soft locks"). Its front-end menus take either pad (two pads pressing the
    // same button move the cursor once), so while no second controller is attached and no 3D scene is drawn (menus, not a level or the
    // skater previews), port 2 simply carries port 1's input. In a level port 2 stays empty.
    // THPS2/3 (b_is_flip_in_game, which have the run flag gRunActive): 2 PLAYER > (a game) > PLAYER 2 CHOOSE SKATER waits for port 2
    // only, and so does opening OPTIONS > CONTROLLER 2 / TWO. Their menus draw 3D (THPS3: every menu has a live 3D background; the
    // skater select screens show the skater), so "no 3D drawn" is never true there: "in a menu" is the game's own flag instead,
    // gRunActive == 0 (front end, pause, every screen over the level). During a run (also a 2 PLAYER split-screen run) port 2 stays empty.
    if constexpr (thps::game::p2_menu_mirror) {
        bool in_menu;
        if constexpr (thps::game::b_is_flip_in_game) in_menu = !thps::skater_in_control();
        else in_menu = !thps::in_gameplay_3d();
        if (port == 1 && in_menu) {
            bool second_pad;
            {
                std::lock_guard lock{g_snap_mutex};
                second_pad = g_snap[1].present;
            }
            if (!second_pad) {
                uint16_t b0 = 0;
                float x0 = 0, y0 = 0;
                input_get(0, &b0, &x0, &y0);
                *buttons = b0, *x = x0, *y = y0;
                g_last_read2.store(b0);
                return true;
            }
        }
    }
    {
        std::lock_guard lock{g_snap_mutex};
        if (!g_snap[port].present) {
            if (port == 1) g_last_read2.store(0);   // (else `inputstate` kept the mirror's last value once the mirror stopped)
            return false;
        }
        *buttons = g_snap[port].buttons, *x = g_snap[port].x, *y = g_snap[port].y;
        if (port == 0) g_port0_reads.fetch_add(1);
    }
    // THPS2/3 read N64 B as FLIP as well as back (their pad layer: A = ollie, B = flip, the C buttons the four tricks, LOG H3), so
    // the east face button (N64 B + C-right: back / grab) would flip and grab at once while skating. With the thps-xbox preset the
    // pair is split by what the game is doing:
    //  - the skater is under control (a level run being played: skater_in_control, the game's own flag, and a 3D view is drawn):
    //    drop N64 B, leaving C-right = grab. (Xbox X, west, is still flip.)
    //  - anywhere else (menus, including the ones that draw a 3D scene or skater and the pause / end-of-run / name-entry screens
    //    over the level): drop C-right, leaving B = back. The earlier rule dropped B whenever a 3D view was drawn, so "B = back"
    //    never arrived in Create-a-Skater or the pause menu; and where C-right does something (the skater roster: C-right = LOAD,
    //    C-left = DELETE) it would have fired together with the back.
    // A scripted/explicit B (or C-right) is untouched. (THPS1 reads A/B in menus only: thps::game::b_is_flip_in_game is false.)
    if constexpr (thps::game::b_is_flip_in_game) {
        if (g_pair_split.load() && (*buttons & 0x4000) && (*buttons & 0x0001)) {
            if (thps::in_gameplay_3d() && thps::skater_in_control()) {
                *buttons &= ~uint16_t(0x4000);
                if (port == 0) g_b_dropped.fetch_add(1);
            } else {
                *buttons &= ~uint16_t(0x0001);
                if (port == 0) g_cr_dropped.fetch_add(1);
            }
        }
    }
    if (port == 0) g_last_read.store(*buttons);
    else if (port == 1) g_last_read2.store(*buttons);
    if (port == 0) {
        std::lock_guard lock{g_script_mutex};
        *buttons |= g_script_buttons;
        if (g_script_x != 0) *x = g_script_x;
        if (g_script_y != 0) *y = g_script_y;
    }
    // THPS1's name entry only reads the D-pad: while that screen runs, the left stick / arrow keys move its cursor and letter too.
    // The game's own press detection (button now, not on the previous poll) gives one step per push of the stick.
    if (thps::name_entry_active()) {
        constexpr float k_on = 0.5f;
        if (*y >= k_on) *buttons |= 0x0800;      // D-up
        else if (*y <= -k_on) *buttons |= 0x0400;  // D-down
        if (*x >= k_on) *buttons |= 0x0100;      // D-right
        else if (*x <= -k_on) *buttons |= 0x0200;  // D-left
    }
    return true;
}

std::string thps::input_state() {
    char buf[200];
    int in_control = 0;
    if constexpr (thps::game::b_is_flip_in_game) in_control = int(thps::skater_in_control());
    std::snprintf(buf, sizeof buf, "in3d=%d in_control=%d b_dropped=%llu cr_dropped=%llu last=0x%04X last2=0x%04X", int(thps::in_gameplay_3d()),
                  in_control, (unsigned long long)g_b_dropped.load(),
                  (unsigned long long)g_cr_dropped.load(), g_last_read.load(), g_last_read2.load());
    return buf;
}

namespace {
std::atomic<uint64_t> g_name_entry_frame{0};
}
void thps::name_entry_tick() {
    if (g_name_entry_frame.exchange(thps::stats.vi_origin_changes.load() + 1) == 0) {
        std::printf("[thps] name entry screen is running: the left stick / arrow keys act as the D-pad there\n");
        std::fflush(stdout);
    }
}
bool thps::name_entry_active() {
    uint64_t last = g_name_entry_frame.load();
    return last != 0 && thps::stats.vi_origin_changes.load() <= last + 4;
}

bool thps::input_connected(int port) {
    if (port == 0) return true;
    if (port < 0 || port >= k_ports) return false;
    std::lock_guard lock{g_snap_mutex};
    return g_snap[size_t(port)].present;
}

// Rumble (only with --rumble-pak).
void thps::input_set_rumble(int port, bool on) {
    if (port < 0 || port >= k_ports) return;
#ifdef THPS_FRONTEND
    recompinput::set_rumble(port, on);  // ramped like an N64 motor, at the General tab's strength
#else
    g_rumble[port] = on;
#endif
}

// VI callback (VI thread): recompinput's rumble ramp (frontend build).
void thps::input_vi() {
#ifdef THPS_FRONTEND
    recompinput::update_rumble();
#endif
}

// vpad attach | vpad detach | vpad button NAME 0|1 | vpad axis NAME VALUE(-32768..32767); "vpad2 ..." = a second virtual
// controller (slot 1: a second player, hotplug tests).
// NAME = SDL game controller names (a b x y start back dpup dpdown dpleft dpright leftshoulder
// rightshoulder leftstick rightstick; leftx lefty rightx righty lefttrigger righttrigger).
std::string thps::input_vpad(const std::string& args, int slot) {
    const std::string label = slot == 0 ? "vpad" : "vpad2";
    if (slot < 0 || slot >= k_vpads) return "err " + label + ": no such slot";
    SDL_Joystick*& vpad = g_vpads[size_t(slot)];
    int& vindex = g_vpad_indices[size_t(slot)];
    std::istringstream in(args);
    std::string op, name;
    in >> op >> name;
    if (op == "attach") {
        if (vpad) return "ok " + label + " attach (already)";
        vindex = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        if (vindex < 0) return "err " + label + " attach: " + SDL_GetError();
        vpad = SDL_JoystickOpen(vindex);
        if (!vpad) return "err " + label + " open: " + SDL_GetError();
        for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) SDL_JoystickSetVirtualAxis(vpad, a, 0);
        char buf[160];
        std::snprintf(buf, sizeof buf, "ok %s attach index=%d gamecontroller=%d", label.c_str(), vindex, int(SDL_IsGameController(vindex)));
        return buf;
    }
    if (!vpad) return "err " + label + ": not attached";
    if (op == "detach") {
        SDL_JoystickClose(vpad);
        SDL_JoystickDetachVirtual(vindex);
        vpad = nullptr;
        return "ok " + label + " detach";
    }
    if (op == "button") {
        int v = 0;
        in >> v;
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name.c_str());
        if (b == SDL_CONTROLLER_BUTTON_INVALID) return "err " + label + " button " + name;
        SDL_JoystickSetVirtualButton(vpad, int(b), Uint8(v ? 1 : 0));
        return "ok " + label + " button " + name + " " + std::to_string(v);
    }
    if (op == "axis") {
        int v = 0;
        in >> v;
        SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name.c_str());
        if (a == SDL_CONTROLLER_AXIS_INVALID) return "err " + label + " axis " + name;
        SDL_JoystickSetVirtualAxis(vpad, int(a), Sint16(std::clamp(v, -32768, 32767)));
        return "ok " + label + " axis " + name + " " + std::to_string(v);
    }
    return "err " + label + " usage: " + label + " attach|detach|button NAME 0|1|axis NAME VALUE";
}

// --control "key NAME": a keyboard tap (SDL key name: Space, Return, D, Left ...) as SDL events, down + up in one batch: the quickest tap a
// player can make. It reaches the game through the key latch (the SDL keyboard state is not touched).
std::string thps::input_key(const std::string& args) {
    std::istringstream in(args);
    std::string name;
    std::getline(in >> std::ws, name);
    SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) return "err key " + name;
    SDL_Event d{};
    d.type = SDL_KEYDOWN;
    d.key.keysym.scancode = sc;
    d.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    d.key.state = SDL_PRESSED;
    SDL_PushEvent(&d);
    SDL_Event u = d;
    u.type = SDL_KEYUP;
    u.key.state = SDL_RELEASED;
    SDL_PushEvent(&u);
    return "ok key " + name;
}

#ifdef THPS_FRONTEND
namespace {

// Replaces one input's bindings in recompinput's single-player keyboard and controller profiles
// (keyboard fields go to the keyboard profile, the rest to the controller profile).
void frontend_assign(int idx, const std::vector<Field>& fields) {
    using recompinput::InputField;
    int kp = recompinput::profiles::get_sp_keyboard_profile_index(), cp = recompinput::profiles::get_sp_controller_profile_index();
    size_t ki = 0, ci = 0;
    for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
        recompinput::profiles::set_input_binding(kp, game_input(idx), i, InputField{});
        recompinput::profiles::set_input_binding(cp, game_input(idx), i, InputField{});
    }
    for (const Field& f : fields) {
        if (f.type == Field::Key && ki < recompinput::num_bindings_per_input)
            recompinput::profiles::set_input_binding(kp, game_input(idx), ki++, InputField::keyboard(SDL_Scancode(f.id)));
        else if (f.type == Field::Button && ci < recompinput::num_bindings_per_input)
            recompinput::profiles::set_input_binding(cp, game_input(idx), ci++, InputField::controller_digital(SDL_GameControllerButton(f.id)));
        else if (f.type == Field::Axis && ci < recompinput::num_bindings_per_input)
            recompinput::profiles::set_input_binding(cp, game_input(idx), ci++,
                                                     InputField::controller_analog(SDL_GameControllerAxis(std::abs(f.id) - 1), f.id > 0));
    }
}

bool frontend_save() { return recompinput::profiles::save_controls_config(recomp::get_config_path() / "controls.json"); }

// Does profile p (a keyboard or a controller profile) hold exactly `layout`'s fields for its device,
// in any order? Like the defaults, at most num_bindings_per_input per input.
bool profile_is(int p, const Bindings& layout, bool keyboard) {
    if (p < 0) return false;
    for (int in = 0; in < IN_COUNT; in++) {
        std::vector<Field> want, have;
        for (const Field& f : layout[in]) if ((f.type == Field::Key) == keyboard) want.push_back(f);
        want.resize(std::min(want.size(), recompinput::num_bindings_per_input));
        for (size_t i = 0; i < recompinput::num_bindings_per_input; i++) {
            Field f = from_recomp(recompinput::profiles::get_input_binding(p, game_input(in), i));
            if (f.type != Field::None) have.push_back(f);
        }
        if (want.size() != have.size()) return false;
        for (const Field& f : have)
            if (std::none_of(want.begin(), want.end(), [&](const Field& g) { return same_field(f, g); })) return false;
    }
    return true;
}

}  // namespace
#endif

// After the settings (controls.json) are loaded: a profile that still holds a generated layout of an
// earlier build (the pre-Q19 N64 one) is reset to the current defaults (thps-xbox), after a backup.
// Not when `controls.preset` says the user picked n64, and never for a profile the user changed.
void thps::input_after_settings(const std::filesystem::path& config_dir) {
#ifdef THPS_FRONTEND
    std::filesystem::path sidecar = config_dir / "controls.preset", json = config_dir / "controls.json";
    std::error_code ec;
    if (std::filesystem::exists(sidecar, ec)) return;
    int kp = recompinput::profiles::get_sp_keyboard_profile_index(), cp = recompinput::profiles::get_sp_controller_profile_index();
    const Bindings old_layout = n64_bindings();
    bool kb = profile_is(kp, old_layout, true), pad = profile_is(cp, old_layout, false);
    if (!kb && !pad) return;
    backup_file(json, config_dir / "controls.json.pre-xbox.bak", false);
    if (kb) recompinput::profiles::reset_profile_bindings(kp, recompinput::InputDevice::Keyboard);
    if (pad) recompinput::profiles::reset_profile_bindings(cp, recompinput::InputDevice::Controller);
    frontend_save();
    std::printf("[thps] input: controls.json had the generated pre-Q19 N64 layout (%s%s%s); moved to preset thps-xbox, old file "
                "kept as controls.json.pre-xbox.bak\n", pad ? "controller" : "", pad && kb ? " + " : "", kb ? "keyboard" : "");
#else
    (void)config_dir;
#endif
}

// --controls-preset NAME / --control "preset NAME": replaces every binding with a preset and saves it.
std::string thps::input_set_preset(const std::string& name) {
    Preset p;
    if (!parse_preset(name, p)) return "err preset '" + name + "': thps-xbox | n64";
    const Bindings b = preset_bindings(p);
#ifdef THPS_FRONTEND
    for (int in = 0; in < IN_COUNT; in++) frontend_assign(in, b[in]);
    bool saved = frontend_save();
    std::error_code ec;
    std::filesystem::path sidecar = recomp::get_config_path() / "controls.preset";  // stops the stale-layout migration
    if (p == Preset::N64) {
        std::ofstream(sidecar) << "n64\n";
    } else {
        std::filesystem::remove(sidecar, ec);
    }
#else
    {
        std::lock_guard lock{g_bind_mutex};
        if (!same_bindings(g_bindings, b)) backup_file(g_controls_cfg, g_controls_cfg.string() + ".bak", true);
        g_bindings = b;
        g_preset = preset_name(p);
        g_base = p;
        bindings_changed_locked();
    }
    write_controls_cfg();
    bool saved = std::filesystem::exists(g_controls_cfg);
#endif
    return std::string("ok preset ") + preset_name(p) + " saved=" + (saved ? "1" : "0");
}

std::string thps::input_preset_name() {
#ifdef THPS_FRONTEND
    return "";
#else
    std::lock_guard lock{g_bind_mutex};
    return g_preset;
#endif
}

// --control "bind INPUT FIELD[,FIELD..]": replaces an input's bindings and saves them, as the
// Controls tab / an edit of controls.cfg would. INPUT = a controls.cfg name (a, b, start,
// c_up, stick_left, ...); FIELD as in controls.cfg.
std::string thps::input_bind(const std::string& args) {
    std::istringstream in(args);
    std::string name, list;
    in >> name;
    std::getline(in >> std::ws, list);
    int idx = -1;
    for (int i = 0; i < IN_COUNT; i++) if (name == k_in_names[i]) idx = i;
    std::vector<Field> fields;
    std::stringstream ss(list);
    std::string item;
    while (std::getline(ss, item, ',')) {
        Field f;
        if (!parse_field(item, f)) return "err bind field " + item;
        if (f.type != Field::None) fields.push_back(f);
    }
    if (idx < 0) return "err bind usage: bind INPUT FIELD[,FIELD..]";
#ifdef THPS_FRONTEND
    frontend_assign(idx, fields);
    bool saved = frontend_save();
#else
    {
        std::lock_guard lock{g_bind_mutex};
        g_bindings[idx] = fields;
        g_preset = "custom";
        bindings_changed_locked();
    }
    write_controls_cfg();
    bool saved = std::filesystem::exists(g_controls_cfg);
#endif
    return "ok bind " + name + " saved=" + (saved ? "1" : "0");
}

// ---- The settings menu's Controls tab: rebinding and the Controllers list (CLI build) ----------------------------------------------
// Each input (a controls.cfg line) shows two controller slots and two keyboard slots: the first and second field of that kind on
// the line (a hand-edited line may have more; they are kept, and the tab shows "+N").

int thps::input_count() { return IN_COUNT; }
std::string thps::input_id(int in) { return in >= 0 && in < IN_COUNT ? k_in_names[in] : ""; }
std::string thps::input_label(int in) { return in >= 0 && in < IN_COUNT ? input_label_of(in) : ""; }
uint64_t thps::input_bindings_version() { return g_bind_version.load(); }

int thps::input_find(const std::string& id) {
    for (int i = 0; i < IN_COUNT; i++) if (id == k_in_names[i]) return i;
    return -1;
}

std::vector<std::string> thps::input_slots(int in, bool keyboard) {
    std::vector<std::string> out;
    if (in < 0 || in >= IN_COUNT) return out;
    std::lock_guard lock{g_bind_mutex};
    for (const Field& f : g_bindings[in]) if ((f.type == Field::Key) == keyboard) out.push_back(field_name(f));
    return out;
}

std::string thps::input_field_label(const std::string& field) {
    Field f;
    if (!parse_field(field, f)) return field;
    return field_label(f);
}

// Puts `field` ("button:y", "axis:lefttrigger+", "key:U", or "none" = clear the slot) into slot 0/1 of input `in`'s controller or
// keyboard fields. A field another input of the same group (may_share) already has moves here: that input gets this slot's old
// field instead (a swap), or loses it when the slot was empty. Sharing with the other group is allowed and named in the message.
thps::RebindResult thps::input_rebind(int in, bool keyboard, int slot, const std::string& field) {
    RebindResult r;
#ifdef THPS_FRONTEND
    (void)in, (void)keyboard, (void)slot, (void)field;
    r.reply = "err rebind: the launcher build has its own Controls tab";
    return r;
#else
    Field f;
    if (in < 0 || in >= IN_COUNT || slot < 0 || slot > 1) { r.reply = "err rebind: no such input or slot"; return r; }
    if (!parse_field(field, f)) { r.reply = "err rebind: bad field " + field; return r; }
    if (f.type != Field::None && (f.type == Field::Key) != keyboard) {
        r.reply = "err rebind: " + field + " is not a " + (keyboard ? "key" : "controller input");
        return r;
    }
    const std::string who = std::string(k_in_names[in]) + (keyboard ? " key " : " pad ") + std::to_string(slot);
    const std::string label = input_label_of(in);
    std::vector<std::string> swapped;
    std::string message;
    bool backup = false;
    {
        std::lock_guard lock{g_bind_mutex};
        std::vector<Field>& list = g_bindings[size_t(in)];
        auto device_pos = [&] {   // indices of this device's fields in `list`
            std::vector<size_t> pos;
            for (size_t i = 0; i < list.size(); i++) if ((list[i].type == Field::Key) == keyboard) pos.push_back(i);
            return pos;
        };
        auto has = [](const std::vector<Field>& l, const Field& g) {
            return std::any_of(l.begin(), l.end(), [&](const Field& h) { return same_field(g, h); });
        };
        std::vector<size_t> pos = device_pos();
        const Field old = size_t(slot) < pos.size() ? list[pos[size_t(slot)]] : Field{};
        if (f.type != Field::None && old.type != Field::None && same_field(f, old)) {
            r.ok = true;
            r.reply = "ok rebind " + who + " " + field_name(f) + " unchanged";
            r.message = label + ": " + field_label(f) + " (unchanged)";
            return r;
        }
        if (f.type == Field::None) {
            if (old.type != Field::None) list.erase(list.begin() + std::ptrdiff_t(pos[size_t(slot)]));
            message = label + ": " + (keyboard ? "key " : "controller button ") + std::to_string(slot + 1) + " cleared";
        } else {
            std::string notes;
            if (has(list, f)) {
                // Already in this input's other slot: the two slots trade places (or it moves to the empty slot).
                auto here = std::find_if(list.begin(), list.end(), [&](const Field& g) { return same_field(f, g); });
                if (old.type != Field::None) {
                    *here = old;
                    list[pos[size_t(slot)]] = f;
                } else {
                    list.erase(here);
                    pos = device_pos();
                    list.insert(pos.empty() ? list.end() : list.begin() + std::ptrdiff_t(pos.back() + 1), f);
                }
            } else {
                for (int j = 0; j < IN_COUNT; j++) {
                    if (j == in) continue;
                    std::vector<Field>& other = g_bindings[size_t(j)];
                    const auto hit = std::find_if(other.begin(), other.end(), [&](const Field& g) { return same_field(f, g); });
                    if (hit == other.end()) continue;
                    if (may_share(in, j)) {
                        notes += std::string(notes.empty() ? "" : "; ") + "also " + input_label_of(j);
                        continue;
                    }
                    const bool give_old = old.type != Field::None && !has(other, old);
                    if (give_old) *hit = old;
                    else other.erase(hit);
                    swapped.push_back(k_in_names[j]);
                    notes += std::string(notes.empty() ? "" : "; ") + input_label_of(j) + " " +
                             (give_old ? "is now " + field_label(old) : "lost it");
                }
                if (size_t(slot) < pos.size()) list[pos[size_t(slot)]] = f;
                else list.insert(pos.empty() ? list.end() : list.begin() + std::ptrdiff_t(pos.back() + 1), f);
            }
            message = label + ": " + field_label(f) + (notes.empty() ? "" : " (" + notes + ")");
        }
        g_preset = same_bindings(g_bindings, thps_xbox_bindings()) ? preset_name(Preset::ThpsXbox)
                 : same_bindings(g_bindings, n64_bindings())       ? preset_name(Preset::N64)
                                                                    : "custom";
        bindings_changed_locked();
        backup = !g_ui_backup_done;
        g_ui_backup_done = true;
    }
    if (backup) backup_file(g_controls_cfg, g_controls_cfg.string() + ".bak", true);   // once per session: the file as it was before
    write_controls_cfg();
    r.ok = true;
    r.reply = "ok rebind " + who + " " + (f.type == Field::None ? std::string("none") : field_name(f)) + " swapped=";
    for (size_t i = 0; i < swapped.size(); i++) r.reply += (i ? "," : "") + swapped[i];
    if (swapped.empty()) r.reply += "-";
    r.reply += " preset=" + thps::input_preset_name() + " saved=" + (std::filesystem::exists(g_controls_cfg) ? "1" : "0");
    r.message = message;
    std::printf("[thps] input: %s\n", r.reply.c_str() + 3);
    std::fflush(stdout);
    return r;
#endif
}

// The fields input `in` has in the preset loaded or picked last, as input_slots lists them (the Controls tab marks a slot that differs).
std::vector<std::string> thps::input_default_slots(int in, bool keyboard) {
    std::vector<std::string> out;
#ifndef THPS_FRONTEND
    if (in < 0 || in >= IN_COUNT) return out;
    std::lock_guard lock{g_bind_mutex};
    const Bindings base = preset_bindings(g_base);   // a named copy: a range-for over a temporary's element would dangle
    for (const Field& f : base[size_t(in)]) if ((f.type == Field::Key) == keyboard) out.push_back(field_name(f));
#else
    (void)in, (void)keyboard;
#endif
    return out;
}

std::string thps::input_base_preset() {
#ifdef THPS_FRONTEND
    return "";
#else
    std::lock_guard lock{g_bind_mutex};
    return preset_name(g_base);
#endif
}

// One input back to the preset loaded or picked last (thps-xbox for a file edited by hand).
thps::RebindResult thps::input_reset_input(int in) {
    RebindResult r;
#ifdef THPS_FRONTEND
    (void)in;
    r.reply = "err reset: the launcher build has its own Controls tab";
#else
    if (in < 0 || in >= IN_COUNT) { r.reply = "err reset: no such input"; return r; }
    std::string base, notes;
    std::vector<std::string> swapped;
    {
        std::lock_guard lock{g_bind_mutex};
        base = preset_name(g_base);
        const Bindings def = preset_bindings(g_base);
        const std::vector<Field>& want = def[size_t(in)];
        std::vector<Field>& list = g_bindings[size_t(in)];
        auto has = [](const std::vector<Field>& l, const Field& g) {
            return std::any_of(l.begin(), l.end(), [&](const Field& h) { return same_field(g, h); });
        };
        // The fields this row gives up (it has them, its default does not): what another input whose field moves back here gets in return.
        std::vector<Field> freed;
        for (const Field& f : list) if (!has(want, f)) freed.push_back(f);
        // As a rebind (input_rebind): a default field another input of the same group holds now is taken from it, and that input gets a freed
        // field of the same kind (its own default first), or loses it. Without this, Default on one row of a swap left the button on two
        // inputs (review 2026-10-26, finding 4: Flip X -> Y swaps Grind onto X; Default on Flip alone gave X to both).
        for (const Field& f : want) {
            if (has(list, f)) continue;
            for (int j = 0; j < IN_COUNT; j++) {
                if (j == in || may_share(in, j)) continue;
                std::vector<Field>& other = g_bindings[size_t(j)];
                const auto hit = std::find_if(other.begin(), other.end(), [&](const Field& g) { return same_field(f, g); });
                if (hit == other.end()) continue;
                auto pick = freed.end();
                for (auto it = freed.begin(); it != freed.end(); ++it) {
                    if ((it->type == Field::Key) != (f.type == Field::Key) || has(other, *it)) continue;
                    if (pick == freed.end() || (has(def[size_t(j)], *it) && !has(def[size_t(j)], *pick))) pick = it;
                }
                if (pick != freed.end()) {
                    *hit = *pick;
                    notes += std::string(notes.empty() ? "" : "; ") + input_label_of(j) + " is now " + field_label(*pick);
                    freed.erase(pick);
                } else {
                    other.erase(hit);
                    notes += std::string(notes.empty() ? "" : "; ") + input_label_of(j) + " lost " + field_label(f);
                }
                swapped.push_back(k_in_names[j]);
            }
        }
        list = want;
        g_preset = same_bindings(g_bindings, thps_xbox_bindings()) ? preset_name(Preset::ThpsXbox)
                 : same_bindings(g_bindings, n64_bindings())       ? preset_name(Preset::N64)
                                                                    : "custom";
        bindings_changed_locked();
    }
    write_controls_cfg();
    r.ok = true;
    r.reply = "ok reset " + std::string(k_in_names[in]) + " to " + base + " swapped=";
    for (size_t i = 0; i < swapped.size(); i++) r.reply += (i ? "," : "") + swapped[i];
    if (swapped.empty()) r.reply += "-";
    r.reply += " preset=" + thps::input_preset_name();
    r.message = std::string(input_label_of(in)) + ": back to the " + (base == "n64" ? "N64 layout" : "face-button layout") +
                (notes.empty() ? "" : " (" + notes + ")");
    std::printf("[thps] input: %s\n", r.reply.c_str() + 3);
    std::fflush(stdout);
#endif
    return r;
}

std::vector<thps::InputPad> thps::input_pads() {
    std::lock_guard lock{g_padinfo_mutex};
    return g_padinfo;
}

// Any thread: applied by the main thread's next update.
void thps::input_assign_port(int id, int port) {
    std::lock_guard lock{g_port_mutex};
    g_port_reqs.push_back({PortReq::Assign, SDL_JoystickID(id), port});
}
void thps::input_swap_ports() {
    std::lock_guard lock{g_port_mutex};
    g_port_reqs.push_back({PortReq::Swap, -1, 0});
}

// --control "ports": the open controllers and their ports; "ports swap"; "ports assign N P" (N = index in that list, P = 1 | 2). Main
// thread: the request is applied before the reply.
std::string thps::input_ports(const std::string& args) {
    std::istringstream in(args);
    std::string op;
    in >> op;
    std::vector<InputPad> pads = input_pads();
    if (op == "swap") input_swap_ports();
    else if (op == "assign") {
        int n = -1, p = 0;
        in >> n >> p;
        if (n < 0 || n >= int(pads.size()) || p < 1 || p > k_ports) return "err ports usage: ports assign INDEX 1|2 (INDEX in 0.." + std::to_string(pads.size()) + ")";
        input_assign_port(pads[size_t(n)].id, p - 1);
    } else if (!op.empty()) return "err ports usage: ports | ports swap | ports assign INDEX 1|2";
    std::array<SDL_GameController*, k_ports> ports{};
    port_controllers(ports);
    refresh_padinfo(ports);
    pads = input_pads();
    std::string s = "ok ports n=" + std::to_string(pads.size());
    for (int p = 0; p < k_ports; p++) {
        int idx = -1;
        for (size_t i = 0; i < pads.size(); i++) if (pads[i].player == p + 1) idx = int(i);
        s += " p" + std::to_string(p + 1) + "=" + (idx < 0 ? std::string("-") : std::to_string(idx));
    }
    for (size_t i = 0; i < pads.size(); i++) {   // padN=NAME#ID:pPLAYER:PRESSED (spaces as _; PRESSED "-" = nothing)
        std::string name = pads[i].name, pressed = pads[i].pressed.empty() ? "-" : pads[i].pressed;
        std::replace(name.begin(), name.end(), ' ', '_');
        std::replace(pressed.begin(), pressed.end(), ' ', '_');
        s += " pad" + std::to_string(i) + "=" + name + "#" + std::to_string(pads[i].id) + ":p" + std::to_string(pads[i].player) + ":" + pressed;
    }
    return s;
}
