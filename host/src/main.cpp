// THPS (N64) recomp host (THPS1-3, one source; the game profile is game/game.toml): N64ModernRuntime (ultramodern + librecomp) + RT64 (Metal, D3D12, Vulkan) + the
// N64Recomp output of the boot segment and the inflated main image. ROM versions (USA v1.1 and
// others, src/versions.h) share this binary: the ROM is identified by hash, its code class's
// payload is activated, and the host reads game addresses by name (thps::sym()).
//
// Boot path: librecomp mimics IPL3 (1 MiB ROM 0x1000 -> 0x80000400) and calls our
// entrypoint wrapper, which re-registers the main-image sections at their real VRAM and
// runs the recompiled Boot_Entry -> Boot_Main. Boot_Main's PIO copies and ERZ inflation
// are hooked (src/hooks.cpp), so the game's own loader unpacks the main image from the
// user's ROM, then jumps to GameEntry.
//
// Default build: no UI, the ROM path is given on the command line once (a copy is kept in the
// config dir); settings are files in the config dir. With -DTHPS_WITH_RECOMPFRONTEND=ON
// (THPS_FRONTEND), RecompFrontend's launcher and settings menu are added (src/frontend.cpp).
// Usage: thps1_recomp [--rom PATH] [--seconds N (0 = until closed)] [--config DIR] [--verbose]
//                     [--dl-log N] [--dl-every N] [--dump-dir DIR] [--dump-frames a,b,c] [--dump-every N]
//                     [--press SEC:BUTTONS:DUR ...] [--no-keyboard] [--no-pads] [--no-edge-fix] [--mute] [--wav PATH] [--null-audio]
//                     [--no-pak] [--rumble-pak] [--si-requeue] [--save-dir DIR] [--rt64-dump-every N] [--rt64-dump-frames a,b]
//                     [--present-dump-every N] [--present-size WxH] [--present-pair] [--hidden]
//                     [--aspect original|16:9|21:9|expand] [--window WxH] [--res-scale N] [--msaa N] [--fullscreen]
//                     [--hud original|16:9|expand] [--split wide|original] [--preset original|enhanced] [--border original|fill]
//                     [--fog original|soft|off]
//                     [--draw-distance original|2x|4x|full] [--dd-buffers bigger|original]
//                     [--skip-intro|--no-skip-intro] [--show-fps|--no-show-fps] [--volume N] [--check-rom PATH]
//                     [--version ID] [--tv pal|ntsc|mpal] [--ride-record FILE|--ride-play FILE] [--ride-step 1/30]
//                     cheats (test aids, src/cheats.cpp; the ones the game has): [--unlock-levels] [--perfect-balance] [--slow-motion] [--skip-to-restart] [--always-special]
//                     [--no-launcher|--launcher] (frontend build: start the game at once / show the
//                     launcher even with --rom, --control or --seconds)
//                     [--api auto|d3d12|vulkan|metal]
//                     split build: [--no-ui] (console only: no first-run window; --control implies it) [--build-game] [--rebuild-game] [--game-lib PATH]
//                     (env: THPS_UI=0|1, THPS_GAME_LIB, THPS_BUILD_JOBS, THPS_FAKE_NO_COMPILER, THPS_FAKE_FREE_MB; tests: THPS_UI_ACTIONS, THPS_UI_SHOT_DIR)
// Config dir: --config DIR; else, with portable.txt in the working directory or next to the
// executable, that directory; else the per-user folder (platform.h): ~/Library/Application Support/<config_dir>,
// %LOCALAPPDATA%\<config_dir> or $XDG_CONFIG_HOME (~/.config)/<config_dir>.
// Windows and Linux: Alt+Enter toggles fullscreen. OS-specific code: platform.cpp, crash.cpp, create_window() below.
// Input: SDL game controllers + keyboard (src/input.cpp); --press adds scripted input on port 1.
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include "sdl_include.h"
#if defined(_WIN32) || defined(__APPLE__)
#include "sdl_syswm_include.h"   // native window handle (HWND / NSWindow); Linux hands RT64 the SDL_Window itself
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"

#include "recomp.h"
#include "host.h"
#include "input_events.h"
#include "hfr.h"
#include "platform.h"
#include "versions.h"
#ifdef __EMSCRIPTEN__
#include "file_picker.h"   // N64Recomp-browser's frontend/: the browser's file picker
#endif
#ifdef THPS_MENU
#include "ui/ui_host.h"
#endif

#include <thread>
#ifdef THPS_FRONTEND
// librecomp's flag set by ultramodern::quit() (launcher Exit, the menu's quit prompt).
extern std::atomic_bool exited;
#endif

#ifdef THPS_SPLIT_GAME_LIB
#include "game_split.h"
#include "firstrun.h"
// Split build: the audio microcode lives in the game library (game_split.h).
static RspExitReason n_aspMain(uint8_t* rdram, uint32_t ucode_addr) { return RspExitReason(thps::game::run_aspmain(rdram, ucode_addr)); }
#else
// build/gen/rsp/n_aspMain.cpp (RSPRecomp output, see config/thps1.aspMain.toml)
RspExitReason n_aspMain(uint8_t* rdram, uint32_t ucode_addr);
#endif

thps::Stats thps::stats;
thps::Options thps::options;
std::atomic<uint8_t*> thps::rdram_base{nullptr};

namespace {


std::atomic<int> g_entry_state{0};  // 1 = entered Boot_Entry, 2 = returned (should not happen)
// The version this process runs (src/versions.h). Chosen from --rom / --version / the stored ROMs before
// the game starts; in the launcher build, when Start Game is pressed.
const thps::VersionInfo* g_chosen = nullptr;
std::u8string game_id_of(const thps::VersionInfo& v) { return std::u8string(reinterpret_cast<const char8_t*>(v.game_id)); }

// librecomp's IPL3 step registers every section whose (codeblob) rom lies in
// [0x1000, 0x101000) at rom-relative addresses. That is right for the boot segment
// but wrong for the main image, which the codeblob stores right after the boot segment
// (version().main_blob_rom). Move the main sections to their real VRAM before running the game.
void thps_entrypoint(uint8_t* rdram, recomp_context* ctx) {
    thps::rdram_base = rdram;
    const thps::VersionInfo& v = thps::version();
#ifdef THPS_SPLIT_GAME_LIB
    thps::game::bind_library();   // librecomp's init_overlays has run: the game library gets section_addresses
#endif
    unload_overlays((int32_t)v.main_vram, v.main_size);
    load_overlays(v.main_blob_rom, (int32_t)v.main_vram, v.main_size);
    if (thps::verbose()) std::printf("[thps] entrypoint (%s, code class %s): main sections registered at 0x%08X; running Boot_Entry\n", v.id, v.code_class, v.main_vram);
    std::fflush(stdout);
    g_entry_state = 1;
    thps::payload_for(v.code_class)->entrypoint(rdram, ctx);
    g_entry_state = 2;
    if (thps::verbose()) std::printf("[thps] boot thread returned from Boot_Entry (game threads keep running)\n");
    std::fflush(stdout);
}

// After librecomp's IPL3 stand-in (which stores osTvType = NTSC): the version's own TV type, or --tv.
void thps_on_init(uint8_t* rdram, recomp_context*) {
    MEM_W(0, (gpr)(int32_t)0x80000300) = thps::versions::tv_type();   // osTvType
    if (thps::verbose()) std::printf("[thps] osTvType = %d\n", thps::versions::tv_type());
}

SDL_Window* g_window = nullptr;
void* g_native_window = nullptr;   // NSWindow* / HWND / SDL_Window* (thps::platform::window_visible)
double g_run_seconds = 0;  // 0 = run until the window is closed
std::chrono::steady_clock::time_point g_start;
bool g_quit_requested = false;
bool g_game_start_sent = false;
bool g_direct_start = true;   // start the game at once (frontend build: false = the launcher starts it)



ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
#ifdef THPS_FRONTEND
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    const Uint32 subsystems = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC;
#else
    const Uint32 subsystems = SDL_INIT_VIDEO;
#endif
    if (SDL_Init(subsystems) != 0) {
        std::fprintf(stderr, "[thps] SDL_Init failed: %s\n", SDL_GetError());
        std::exit(2);
    }
    thps::input_init();
    thps::audio_init();
#ifdef THPS_FRONTEND
    thps::frontend::after_sdl_init();
#endif
    return nullptr;
}

// The one place the window and renderer handle differ per OS (what RT64 wants, ultramodern::renderer::WindowHandle):
//   macOS    {NSWindow*, CAMetalLayer*}  window created with SDL_WINDOW_METAL
//   Windows  {HWND, creating thread id}  plain window; RT64 picks D3D12 (Vulkan as fallback)
//   Linux    SDL_Window*                 SDL_WINDOW_VULKAN (RT64 is built with RT64_SDL_WINDOW_VULKAN)
ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    Uint32 flags = SDL_WINDOW_RESIZABLE | (thps::options.hidden ? SDL_WINDOW_HIDDEN : 0);
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#elif defined(__linux__)
    flags |= SDL_WINDOW_VULKAN;
#endif
    // The default size follows the display scaling (Windows: 1920x1080 at 150 %); --window / a saved WxH is physical pixels.
    // A hidden window (the tests) keeps 1280x720, so results do not depend on the test machine's scaling.
    int win_w = thps::gfx.window_w, win_h = thps::gfx.window_h;
    if (thps::gfx.window_auto && !thps::options.hidden) thps::platform::scale_window_size(win_w, win_h);
    g_window = SDL_CreateWindow(thps::game::window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, flags);
    if (g_window == nullptr) {
        std::fprintf(stderr, "[thps] SDL_CreateWindow failed: %s\n", SDL_GetError());
        std::exit(3);
    }
    if (thps::verbose())
        std::printf("[thps] window %dx%d (%s, display scaling %.2f)\n", win_w, win_h, thps::gfx.window_auto ? "default size" : "set size",
                    double(thps::platform::window_scale()));
#ifdef THPS_FRONTEND
    thps::frontend::set_window(g_window);
#endif
#ifdef THPS_MENU
    thps::ui::set_window(g_window);
#endif
    g_start = std::chrono::steady_clock::now();
#if defined(_WIN32) || defined(__APPLE__)
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(g_window, &wm)) {
        std::fprintf(stderr, "[thps] SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        std::exit(4);
    }
#endif
#if defined(_WIN32)
    g_native_window = wm.info.win.window;
    return ultramodern::renderer::WindowHandle{wm.info.win.window, GetCurrentThreadId()};
#elif defined(__APPLE__)
    g_native_window = wm.info.cocoa.window;
    SDL_MetalView view = SDL_Metal_CreateView(g_window);
    return ultramodern::renderer::WindowHandle{wm.info.cocoa.window, SDL_Metal_GetLayer(view)};
#else
    g_native_window = g_window;
    return ultramodern::renderer::WindowHandle{g_window};
#endif
}

double elapsed() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }

void print_status(const char* tag, double t) {
    auto& s = thps::stats;
    std::printf("[thps] %s t=%.1fs presented=%" PRIu64 " gfx_tasks=%" PRIu64 " audio_tasks=%" PRIu64 " dummy=%" PRIu64
                " pio=%" PRIu64 " erz=%" PRIu64 "(err %" PRIu64 ") raw_dma=%" PRIu64 " (%" PRIu64 " KiB) input=%" PRIu64 "/%" PRIu64 " entry=%d\n",
                tag, t, s.frames_presented.load(), s.gfx_tasks.load(), s.audio_tasks.load(), s.dummy_workloads.load(),
                s.pio_copies.load(), s.erz_unpacks.load(), s.erz_errors.load(), s.raw_dmas.load(), s.raw_dma_bytes.load() / 1024,
                s.input_nonzero.load(), s.input_polls.load(),
                g_entry_state.load());
    std::fflush(stdout);
}

// Per-interval frame pacing: displayed frames (VI origin changes), gfx/audio tasks, and how long
// the gfx thread spent in send_dl / update_screen.
void print_perf(double t) {
    auto& s = thps::stats;
    static double last_t = 0;
    static uint64_t l_frames = 0, l_gfx = 0, l_aud = 0, l_vi = 0, l_dl = 0, l_us = 0;
    double dt = t - last_t;
    if (dt <= 0) return;
    uint64_t frames = s.vi_origin_changes, gfx = s.gfx_tasks, aud = s.audio_tasks, vi = s.frames_presented;
    uint64_t dl = s.send_dl_us, us = s.update_screen_us;
    uint64_t dgfx = gfx - l_gfx, dvi = vi - l_vi;
    static uint64_t l_skip = 0, l_comp = 0, l_win = 0;
    uint64_t skip = s.presents_skipped, comp = s.rt64_composited, win = s.rt64_window_presents;
    static uint64_t l_af = 0, l_nz = 0;
    uint64_t af = s.audio_frames_queued, nz = s.audio_nonsilent_buffers;
    std::printf("[thps] audio t=%.1fs frames/s=%.0f nonsilent_buffers=%" PRIu64 " peak=%" PRIu64 " queued=%" PRIu64 " fifo_full=%" PRIu64 " ucode_errors=%" PRIu64
                " underruns=%" PRIu64 " starved_frames=%" PRIu64 "\n",
                t, (af - l_af) / dt, nz - l_nz, s.audio_peak.exchange(0), s.audio_frames_remaining_last.load(), s.ai_fifo_full.exchange(0), s.audio_ucode_errors.load(),
                s.audio_underruns.load(), s.audio_starved_frames.load());
    l_af = af, l_nz = nz;
    std::printf("[thps] perf t=%.1fs skipped_presents=%" PRIu64 " fps=%.1f presented/s=%.1f window/s=%.1f presented_total=%" PRIu64
                " present_dumps=%" PRIu64 " gfx/s=%.1f audio/s=%.1f vi/s=%.1f send_dl avg %.2f ms max %.2f | update_screen avg %.2f ms max %.2f\n",
                t, skip - l_skip, (frames - l_frames) / dt, (comp - l_comp) / dt, (win - l_win) / dt, comp, s.present_dumps.load(),
                dgfx / dt, (aud - l_aud) / dt, dvi / dt,
                dgfx ? (dl - l_dl) / 1000.0 / dgfx : 0.0, s.send_dl_us_max.exchange(0) / 1000.0,
                dvi ? (us - l_us) / 1000.0 / dvi : 0.0, s.update_screen_us_max.exchange(0) / 1000.0);
    std::string hfr = thps::hfr_perf_text();
    if (!hfr.empty()) std::printf("%s\n", hfr.c_str());
    std::fflush(stdout);
    last_t = t;
    l_skip = skip, l_comp = comp, l_win = win;
    l_frames = frames, l_gfx = gfx, l_aud = aud, l_vi = vi, l_dl = dl, l_us = us;
}

void update_gfx(void*) {
#ifdef THPS_FRONTEND
    thps::frontend::handle_events();  // recompinput: SDL events -> menus, controllers, quit prompt
#else
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (!thps::controller_action_allowed(ev, thps::options.no_pads)) continue;
        if (ev.type == SDL_QUIT) g_quit_requested = true;
#ifndef __APPLE__
        // Alt+Enter: windowed <-> fullscreen (macOS has its own full-screen button / Ctrl+Cmd+F).
        if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT) && !ev.key.repeat) {
            thps::toggle_fullscreen();
            continue;
        }
#endif
#ifdef THPS_MENU
        if (thps::ui::handle_event(ev)) continue;   // the settings menu (Esc) takes the event while it is open
#endif
        thps::input_event(ev);
    }
#endif
    double t = elapsed();
    if (thps::options.control) thps::control_main_thread();
    thps::input_update(t);
    thps::fps_update(g_window);
#ifdef THPS_FRONTEND
    thps::frontend::update();
#endif
    static double last_vis_check = -1;
    if (t - last_vis_check >= 0.1) {
        last_vis_check = t;
        bool vis = thps::platform::window_visible(g_native_window);
        int ww = 0, wh = 0;
        SDL_GetWindowSize(g_window, &ww, &wh);
        if (ww > 0 && wh > 0) thps::window_aspect = float(ww) / float(wh);
        if (vis != thps::stats.window_visible.exchange(vis)) {
            if (thps::verbose()) std::printf("[thps] window %s: %s presents\n", vis ? "visible" : "occluded", vis ? "resuming" : "skipping");
            std::fflush(stdout);
        }
    }
    // Per-second status/audio/perf lines: --verbose, or --control (the test runner reads them).
    static int last_report = -1;
    if (int(t) != last_report && (thps::options.verbose || thps::options.control)) {
        last_report = int(t);
        print_status("status", t);
        print_perf(t);
    }
    if (!g_game_start_sent && t >= 0.5 && g_direct_start) {
        g_game_start_sent = true;
        if (thps::verbose()) std::printf("[thps] start_game\n");
        std::fflush(stdout);
        recomp::start_game(game_id_of(*g_chosen), "");
    }
    if ((g_run_seconds > 0 && t >= g_run_seconds) || g_quit_requested || thps::ctl_quit) {
        static bool quitting = false;
        if (!quitting) {
            quitting = true;
            print_status("quit", t);
            thps::ride_quit();
            thps::audio_shutdown();
            // ultramodern::quit() lets recomp::start munmap RDRAM while game threads can still
            // wake up and touch it (SIGSEGV in wait_for_resumed, ~1 in 5 runs). Nothing needs
            // an orderly teardown here (no save files yet, WAV is closed), so leave now.
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(thps::stats.gfx_tasks.load() > 0 ? 0 : 1);
        }
    }
}

// Audio: the game's n_aspMain, recompiled by RSPRecomp (config/thps1.aspMain.toml ->
// build/gen/rsp/n_aspMain.cpp). --null-audio completes audio tasks without running them.
RspExitReason null_audio_ucode(uint8_t*, uint32_t) { return RspExitReason::Broke; }
RspExitReason audio_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    const auto t0 = std::chrono::steady_clock::now();
    RspExitReason r = n_aspMain(rdram, ucode_addr);
    thps::ftime_span("audio", t0);
    if (r != RspExitReason::Broke && thps::stats.audio_ucode_errors.fetch_add(1) < 5) {
        std::fprintf(stderr, "[thps] n_aspMain exited with reason %d\n", int(r));
    }
    return RspExitReason::Broke;  // keep the runtime going; errors are counted
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    if (task->t.type == M_AUDTASK) {
        if (thps::stats.audio_tasks.fetch_add(1) == 0 && thps::verbose()) {
            std::printf("[thps] first audio task: flags 0x%X ucode_boot 0x%08X/0x%X ucode 0x%08X/0x%X ucode_data 0x%08X/0x%X dram_stack 0x%08X/0x%X output_buff 0x%08X data 0x%08X/0x%X yield 0x%08X/0x%X\n",
                        task->t.flags, task->t.ucode_boot, task->t.ucode_boot_size, task->t.ucode, task->t.ucode_size,
                        task->t.ucode_data, task->t.ucode_data_size, task->t.dram_stack, task->t.dram_stack_size,
                        task->t.output_buff, task->t.data_ptr, task->t.data_size, task->t.yield_data_ptr, task->t.yield_data_size);
            std::fflush(stdout);
        }
        return thps::options.null_audio ? null_audio_ucode : audio_ucode;
    }
    thps::stats.other_tasks++;
    std::fprintf(stderr, "[thps] unexpected RSP task type %" PRIu32 " ucode 0x%08X\n", task->t.type, task->t.ucode);
    return null_audio_ucode;
}

void queue_samples(int16_t* samples, size_t count) { thps::audio_queue_samples(samples, count); }
size_t get_frames_remaining() { return thps::audio_frames_remaining(); }
void set_frequency(uint32_t freq) { thps::audio_set_frequency(freq); }

void poll_input() {
    thps::input_poll();
    thps::cheats::tick();   // game thread: the game's own cheat flags (game/cheats.cpp)
}
bool get_input(int controller, uint16_t* buttons, float* x, float* y) {
    bool ok = thps::input_get(controller, buttons, x, y);
    if (controller == 0) {
        thps::skip_intro_input(buttons);
        if (thps::options.control) thps::control_input(buttons, x, y);
        if (thps::options.control) thps::latency_on_poll(buttons);
        thps::ride_on_read(&ok, buttons, x, y);   // a ride records what the game gets, or replaces it
        thps::stats.input_polls++;
        if (*buttons || *x != 0 || *y != 0) thps::stats.input_nonzero++;
    }
    return ok;
}
void set_rumble(int controller, bool on) { thps::input_set_rumble(controller, on); }
ultramodern::input::connected_device_info_t get_connected_device_info(int controller) {
    if (thps::input_connected(controller)) {
        // The Controller Pak (src/pak.cpp) is answered through osPfs*; ultramodern only models
        // "none" or a Rumble Pak here, and osMotorInit uses this.
        return {ultramodern::input::Device::Controller, thps::options.rumble_pak ? ultramodern::input::Pak::RumblePak : ultramodern::input::Pak::None};
    }
    return {ultramodern::input::Device::None, ultramodern::input::Pak::None};
}

void message_box(const char* msg) { std::fprintf(stderr, "[thps] message_box: %s\n", msg); }

void vi_callback() { thps::input_vi(); }

// N64 button bits (CONT_*): A 0x8000 B 0x4000 Z 0x2000 START 0x1000 U 0x0800 D 0x0400 L 0x0200 R 0x0100
uint16_t parse_buttons(const std::string& s, float& sx, float& sy) {
    uint16_t b = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('+', i);
        std::string k = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (k == "A") b |= 0x8000;
        else if (k == "B") b |= 0x4000;
        else if (k == "Z") b |= 0x2000;
        else if (k == "START") b |= 0x1000;
        else if (k == "UP") b |= 0x0800;
        else if (k == "DOWN") b |= 0x0400;
        else if (k == "LEFT") b |= 0x0200;
        else if (k == "RIGHT") b |= 0x0100;
        else if (k == "L") b |= 0x0020;
        else if (k == "R") b |= 0x0010;
        else if (k == "CU") b |= 0x0008;
        else if (k == "CD") b |= 0x0004;
        else if (k == "CL") b |= 0x0002;
        else if (k == "CR") b |= 0x0001;
        else if (k == "SUP") sy = 1;
        else if (k == "SDOWN") sy = -1;
        else if (k == "SLEFT") sx = -1;
        else if (k == "SRIGHT") sx = 1;
        else std::fprintf(stderr, "[thps] unknown button %s\n", k.c_str());
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return b;
}

}  // namespace

uint16_t thps_parse_buttons(const std::string& s, float& sx, float& sy) { return parse_buttons(s, sx, sy); }

int main(int argc_in, char** argv_in) {
    thps::platform::early_init();   // Windows: 1 ms timer, UTF-8 console, WASAPI default; DPI-awareness hint
    // The command line as UTF-8 (Windows argv is the ANSI code page); paths below go through path_from_utf8.
    std::vector<std::string> arg_strings = thps::platform::command_line(argc_in, argv_in);
    std::vector<char*> arg_ptrs;
    for (auto& a : arg_strings) arg_ptrs.push_back(a.data());
    arg_ptrs.push_back(nullptr);
    const int argc = int(arg_strings.size());
    char** argv = arg_ptrs.data();
    using thps::platform::path_from_utf8;
    std::filesystem::path config_dir;
    std::filesystem::path rom_path, check_rom;
    std::string version_arg;
    std::vector<std::pair<std::string, std::string>> gfx_overrides;
    std::string controls_preset;
    bool no_launcher = false, force_launcher = false, no_ui = false;
#ifdef THPS_SPLIT_GAME_LIB
    thps::game::Options game_opt;
    bool build_game_only = false;
    std::filesystem::path blob_hash_rom, dump_blob_out;
#endif
    // Discover the opt-in before any immediate --ride-play / --check-rom action,
    // preserving legacy parser exit behavior and making flag order independent.
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i],"--ride-step")) {
        if (++i >= argc || !thps::ride_set_step(argv[i])) {
            std::fprintf(stderr,"[thps] ride: --ride-step supports only 1/30\n"); return 2;
        }
    }
    for (int i = 1; i < argc; i++) {
        auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        auto flag = [&](const char* name) { return !std::strcmp(argv[i], name); };
        if (arg("--seconds")) g_run_seconds = std::atof(argv[++i]);
        else if (arg("--config")) config_dir = path_from_utf8(argv[++i]);
        else if (arg("--rom")) rom_path = path_from_utf8(argv[++i]);
        else if (arg("--check-rom")) check_rom = path_from_utf8(argv[++i]);
        else if (arg("--version")) version_arg = argv[++i];
        else if (arg("--api")) {
            thps::options.graphics_api = argv[++i];
            if (thps::options.graphics_api != "auto" && thps::options.graphics_api != "d3d12" && thps::options.graphics_api != "vulkan" &&
                thps::options.graphics_api != "metal") {
                std::fprintf(stderr, "bad --api %s (auto|d3d12|vulkan|metal)\n", thps::options.graphics_api.c_str());
                return 2;
            }
        }
        else if (arg("--tv")) {
            std::string t = argv[++i];
            if (t == "pal") thps::versions::tv_override = 0;
            else if (t == "ntsc") thps::versions::tv_override = 1;
            else if (t == "mpal") thps::versions::tv_override = 2;
            else { std::fprintf(stderr, "bad --tv %s (pal|ntsc|mpal)\n", t.c_str()); return 2; }
        }
        else if (arg("--dl-log")) thps::options.dl_log = std::atoi(argv[++i]);
        else if (arg("--dl-every")) thps::options.dl_log_every = std::atoi(argv[++i]);
        else if (arg("--dump-dir")) thps::options.dump_dir = path_from_utf8(argv[++i]);
        else if (arg("--dump-frames")) thps::options.dump_frames = argv[++i];
        else if (arg("--dump-every")) thps::options.dump_every = std::atoi(argv[++i]);
        else if (arg("--rt64-dump-every")) thps::options.rt64_dump_every = std::atoi(argv[++i]);
        else if (arg("--rt64-dump-frames")) thps::options.rt64_dump_frames = argv[++i];
        else if (arg("--dl-trace")) thps::options.dl_trace = std::atoi(argv[++i]);
        else if (arg("--press")) {
            // SEC:BUTTONS:DUR, e.g. 12:START:0.2
            std::string s = argv[++i];
            size_t a = s.find(':'), b = s.rfind(':');
            if (a == std::string::npos || b == a) { std::fprintf(stderr, "bad --press %s\n", s.c_str()); return 2; }
            float sx = 0, sy = 0;
            uint16_t buttons = parse_buttons(s.substr(a + 1, b - a - 1), sx, sy);
            thps::input_add_press(std::atof(s.substr(0, a).c_str()), std::atof(s.substr(b + 1).c_str()), buttons, sx, sy);
        }
        else if (flag("--verbose")) thps::options.verbose = true;
        else if (flag("--no-keyboard")) thps::options.no_keyboard = true;
        else if (flag("--keyboard")) thps::options.no_keyboard = false;   // tests: the suite passes --no-keyboard first; keyboard sessions turn it back on
        else if (flag("--no-pads")) thps::options.no_pads = true;
        else if (flag("--no-edge-fix")) thps::options.edge_fix = false;
        else if (flag("--always-present")) thps::options.always_present = true;
        else if (flag("--mute")) thps::options.mute = true;
        else if (flag("--no-pak")) thps::options.no_pak = true;
        else if (flag("--si-requeue")) thps::options.si_requeue = true;
        else if (flag("--rumble-pak")) thps::options.rumble_pak = true;
        else if (arg("--save-dir")) thps::options.save_dir = path_from_utf8(argv[++i]);
        else if (flag("--null-audio")) thps::options.null_audio = true;
        else if (flag("--hidden")) thps::options.hidden = true;
        else if (flag("--present-pair")) thps::options.present_pair = true;
        else if (arg("--cull-extra")) thps::options.cull_extra = float(std::atof(argv[++i]));
        else if (arg("--present-dump-every")) thps::options.present_dump_every = std::atoi(argv[++i]);
        else if (arg("--present-size")) {
            std::string v = argv[++i];
            size_t x = v.find('x');
            thps::options.present_w = x == std::string::npos ? 0 : std::atoi(v.substr(0, x).c_str());
            thps::options.present_h = x == std::string::npos ? 0 : std::atoi(v.substr(x + 1).c_str());
        }
        else if (arg("--aspect")) gfx_overrides.emplace_back("aspect", argv[++i]);
        else if (arg("--window")) gfx_overrides.emplace_back("window", argv[++i]);
        else if (arg("--res-scale")) gfx_overrides.emplace_back("res_scale", argv[++i]);
        else if (arg("--msaa")) gfx_overrides.emplace_back("msaa", argv[++i]);
        else if (arg("--refresh")) gfx_overrides.emplace_back("refresh", argv[++i]);
        else if (arg("--hud")) gfx_overrides.emplace_back("hud", argv[++i]);
        else if (arg("--split")) gfx_overrides.emplace_back("split", argv[++i]);
        else if (arg("--preset")) gfx_overrides.emplace_back("preset", argv[++i]);   // the seven rows of a graphics preset; flags after it change single rows
        else if (arg("--border")) gfx_overrides.emplace_back("border", argv[++i]);
        else if (arg("--fog")) gfx_overrides.emplace_back("fog", argv[++i]);
        else if (arg("--draw-distance")) gfx_overrides.emplace_back("draw_distance", argv[++i]);
        else if (arg("--dd-buffers")) gfx_overrides.emplace_back("dd_buffers", argv[++i]);
        else if (arg("--controls-preset")) controls_preset = argv[++i];
        else if (flag("--fullscreen")) gfx_overrides.emplace_back("fullscreen", "1");
        else if (flag("--windowed")) gfx_overrides.emplace_back("fullscreen", "0");
        else if (arg("--wav")) thps::options.wav_path = path_from_utf8(argv[++i]);
        else if (arg("--ride-step")) ++i; // validated before immediate ride actions above
        else if (arg("--ride-record")) { if (!thps::ride_record(argv[++i])) {
            std::fprintf(stderr,"[thps] ride: %s\n",thps::ride_error().c_str()); return 2; } }
        else if (arg("--ride-play")) { const char* f = argv[++i]; if (!thps::ride_play(f)) { std::fprintf(stderr, "[thps] ride: can't read %s (%s)\n", f, thps::ride_error().c_str()); return 2; } }
        else if (arg("--audio-fifo")) thps::options.audio_fifo_frames = std::atoi(argv[++i]);
        else if (flag("--control")) thps::options.control = true;
        else if (flag("--skip-intro")) thps::skip_intro_override = 1;
        else if (flag("--no-skip-intro")) thps::skip_intro_override = 0;
        else if (flag("--show-fps")) thps::show_fps_override = 1;
        else if (flag("--no-show-fps")) thps::show_fps_override = 0;
        else if (arg("--volume")) thps::volume_override = std::clamp(std::atoi(argv[++i]), 0, 100);
        else if (flag("--no-launcher")) no_launcher = true;
        else if (flag("--launcher")) force_launcher = true;
        else if (flag("--no-ui")) no_ui = true;
#ifdef THPS_SPLIT_GAME_LIB
        else if (flag("--build-game")) build_game_only = true;                  // translate + compile + link, then exit
        else if (flag("--rebuild-game")) game_opt.force_rebuild = true;
        else if (flag("--live-game")) game_opt.live = true;
        else if (arg("--blob-hash")) blob_hash_rom = path_from_utf8(argv[++i]);   // release tooling: fingerprint of the code blob of a .z64
        else if (arg("--dump-blob")) dump_blob_out = path_from_utf8(argv[++i]);     // with --blob-hash: also write the blob (local comparison)
        else if (flag("--keep-game-work")) game_opt.keep_work = true;
        else if (arg("--game-lib")) game_opt.library = path_from_utf8(argv[++i]);   // use this library, never build
#endif
        else if (flag("--features")) {
#ifdef THPS_FRONTEND
            std::puts("frontend=1");
#else
            std::puts("frontend=0");
#endif
#ifdef THPS_MENU
            std::puts("menu=1");
#else
            std::puts("menu=0");
#endif
            std::printf("ride_step=%s\n",!std::strcmp(thps::game::family,"thps2") ? "thps2-us-static-split-experimental" : "unsupported");
            return 0;
        }
        else if (thps::hfr_arg(argc, argv, i)) {}  // --hfr-* (src/interpolation.cpp)
        else if (thps::cheats::parse_flag(argv[i])) {}   // --unlock-levels, --perfect-balance, ... (test aids, src/cheats.cpp)
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    // [dl] inspector summaries: only with --verbose unless asked for (--dl-log / --dl-every).
    if (thps::options.dl_log < 0) thps::options.dl_log = thps::options.verbose ? 8 : 0;
    if (thps::options.dl_log_every < 0) thps::options.dl_log_every = thps::options.verbose ? 60 : 0;
    if (config_dir.empty()) config_dir = thps::platform::default_config_dir(argv[0], thps::game::config_dir_name);
    config_dir = std::filesystem::absolute(config_dir);
    {
        std::error_code ec;
        std::filesystem::create_directories(config_dir, ec);
        if (ec) {
            // Read-only disk, a file in the way, no permission: say so instead of dying on an exception.
            const std::string msg = "The folder where the app keeps its settings, saves and game cannot be created:\n  " + thps::platform::to_utf8(config_dir) + "\n(" + ec.message() +
                                    ")\n\nMake sure you may write there, or start the app with --config FOLDER to use another place.";
#ifdef THPS_SPLIT_GAME_LIB
            const char* ui_env = std::getenv("THPS_UI");
            const bool ui_on = ui_env ? *ui_env == '1' : !thps::options.control && !no_ui;
            std::fflush(stdout);
            std::_Exit(thps::firstrun::show_fatal("The settings folder cannot be created", msg, ui_on, thps::options.hidden, thps::firstrun::kFolderNotWritable));
#else
            std::fprintf(stderr, "[thps] %s\n", msg.c_str());
            return 7;
#endif
        }
    }
    thps::crash_install(config_dir);
    if (thps::options.save_dir.empty()) thps::options.save_dir = config_dir / "saves";
    if (!thps::options.dump_dir.empty()) std::filesystem::create_directories(thps::options.dump_dir);

    recomp::register_config_path(config_dir);
#ifdef THPS_SPLIT_GAME_LIB
    if (const char* lib = std::getenv("THPS_GAME_LIB"); lib && *lib && game_opt.library.empty()) game_opt.library = path_from_utf8(lib);
    if (const char* live = std::getenv("THPS_LIVE"); live && *live == '1') game_opt.live = true;
    thps::ride_engine_live(game_opt.live);
    thps::game::init(thps::platform::resource_dir(argv[0]), !game_opt.library.empty());
#endif

    // One GameEntry per ROM version this build can run (own hash, header name, game_id and stored ROM
    // <game_id>.z64); they share the entrypoint wrapper, which uses the active version. Recognised-only
    // versions get none: src/rom_check.cpp names them when a user offers such a ROM.
    std::vector<recomp::GameEntry> games;
    for (const thps::VersionInfo& v : thps::all_versions) {
        if (!thps::versions::runnable(v)) continue;
        recomp::GameEntry game{};
        game.rom_hash = v.xxh3;
        game.internal_name = v.internal_name;
        game.display_name = v.display_name;
        game.game_id = game_id_of(v);
#ifdef THPS_FRONTEND
        game.mod_game_id = thps::game::mod_game_id;   // also the name librecomp's --game takes
#else
        game.mod_game_id = "";
#endif
        game.save_type = recomp::SaveType::None;  // saves are Controller Pak only (src/pak.cpp, .mpk files)
        game.is_enabled = true;
        game.entrypoint_address = (gpr)(int32_t)v.boot_vram;
        game.entrypoint = thps_entrypoint;
        game.on_init_callback = thps_on_init;
        games.push_back(game);
    }
    if (games.empty()) {
        std::fprintf(stderr, "[thps] this build has no runnable ROM version (generate a payload: tools/gen.sh)\n");
        return 1;
    }

    // Settings: files in the config dir (CLI build) or recompui's tabs (frontend build).
    thps::input_load_bindings(config_dir);
#ifdef THPS_FRONTEND
    thps::frontend::init(&games);  // tabs + finalize() loads <config>/*.json
    thps::input_after_settings(config_dir);
    for (auto& [k, v] : gfx_overrides) {
        if (!thps::gfx_override(k, v)) { std::fprintf(stderr, "bad --%s %s\n", k.c_str(), v.c_str()); return 2; }
    }
    thps::gfx_from_settings(ultramodern::renderer::get_graphics_config(), thps::settings::widescreen(), true);
#else
    thps::settings::load(config_dir);
    thps::gfx_config_load(config_dir / "graphics.cfg");
    for (auto& [k, v] : gfx_overrides) {
        if (!thps::gfx_config_set(k, v)) { std::fprintf(stderr, "bad --%s %s\n", k.c_str(), v.c_str()); return 2; }
    }
    if (!gfx_overrides.empty() || !std::filesystem::exists(config_dir / "graphics.cfg")) thps::gfx_config_save(config_dir / "graphics.cfg");
#ifdef THPS_MENU
    thps::ui::init(thps::platform::resource_dir(argv[0]) / "ui", config_dir);
#endif
#endif
    if (!controls_preset.empty()) {
        std::string r = thps::input_set_preset(controls_preset);
        std::printf("[thps] input: --controls-preset %s: %s\n", controls_preset.c_str(), r.c_str());
        if (r.rfind("err", 0) == 0) return 2;
    }
    std::printf("[thps] start: seconds=%.1f config=%s gfx: %s skip_intro=%d show_fps=%d volume=%.0f%% %s\n", g_run_seconds,
                config_dir.string().c_str(), thps::gfx_describe().c_str(), int(thps::settings::skip_intro()),
                int(thps::settings::show_fps()), thps::settings::volume() * 100.0f, thps::gfx_live().c_str());
    thps::cheats::load(config_dir);   // cheats.cfg + the command-line flags; one log line when any is on

    for (const recomp::GameEntry& game : games) {
        if (!recomp::register_game(game)) {
            std::fprintf(stderr, "[thps] register_game failed\n");
            return 1;
        }
    }
    if (!check_rom.empty()) {
        // ROM check only (tests and users): the result and the message the launcher would show. Nothing is stored.
        thps::RomCheck c = thps::check_rom_file(check_rom);
        std::printf("[thps] check-rom %s: %d%s%s\n%s\n", check_rom.string().c_str(), c.error, c.version ? " " : "", c.version ? c.version->id : "",
                    c.text.c_str());
        std::fflush(stdout);
        std::_Exit(c.error == int(recomp::RomValidationError::Good) ? 0 : 3);
    }
#ifdef THPS_SPLIT_GAME_LIB
    if (!blob_hash_rom.empty()) {
        thps::RomCheck c = thps::check_rom_file(blob_hash_rom);
        std::string h, err;
        if (!c.version || !thps::game::blob_hash(*c.version, blob_hash_rom, h, err)) {
            std::fprintf(stderr, "[thps] --blob-hash: %s\n", c.version ? err.c_str() : c.text.c_str());
            std::_Exit(3);
        }
        if (!dump_blob_out.empty() && !thps::game::dump_blob(*c.version, blob_hash_rom, dump_blob_out, err)) { std::fprintf(stderr, "[thps] %s\n", err.c_str()); std::_Exit(3); }
        std::printf("%s %s %s\n", h.c_str(), c.version->id, c.version->code_class);
        std::fflush(stdout);
        std::_Exit(0);
    }
#endif
    if (!version_arg.empty()) {
        // --version ID: run this version (its stored ROM, or --rom; a --rom of another version is refused below).
        g_chosen = thps::versions::find(version_arg);
        if (!g_chosen || !thps::versions::runnable(*g_chosen)) {
            std::fprintf(stderr, "[thps] --version %s: not a version this build runs. Runnable:%s\n", version_arg.c_str(), thps::versions::supported_list().c_str());
            return 2;
        }
    }
#if defined(THPS_SPLIT_GAME_LIB) && !defined(THPS_FRONTEND)
    // Split build: the ROM (stored, --rom, the file picker, a dropped file), the hash check and the first-run build all live in
    // src/firstrun.cpp. It shows a window when something needs the user and the console otherwise.
    (void)no_launcher, (void)force_launcher;
    {
        thps::firstrun::Params fp;
        fp.config_dir = config_dir;
        fp.rom_arg = rom_path;
        fp.version_arg = version_arg;
        fp.game_opt = game_opt;
        fp.ui = !thps::options.control && !no_ui && !build_game_only;
        if (const char* ui_env = std::getenv("THPS_UI"); ui_env && *ui_env) fp.ui = *ui_env == '1';
        fp.hidden = thps::options.hidden;
        g_chosen = nullptr;
        int rc = thps::firstrun::prepare(fp, g_chosen);
        if (rc != 0 || !g_chosen) {
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(rc ? rc : 1);
        }
        if (build_game_only) { std::printf("[thps] game library ready\n"); return 0; }
    }
#else
    if (!rom_path.empty()) {
        thps::RomCheck c = thps::check_rom_file(rom_path);
        if (c.error == int(recomp::RomValidationError::Good) && g_chosen && g_chosen != c.version) {
            std::fprintf(stderr, "[thps] --rom is %s, but --version %s was given\n", c.version->id, g_chosen->id);
            return 1;
        }
        auto v = c.version ? recomp::select_rom(rom_path, game_id_of(*c.version)) : recomp::RomValidationError(c.error);   // select_rom stores it
        if (thps::verbose()) std::printf("[thps] select_rom(%s) = %d (0 = Good)%s%s\n", rom_path.string().c_str(), int(v), c.version ? " " : "", c.version ? c.version->id : "");
        if (v != recomp::RomValidationError::Good) {
            std::fprintf(stderr, "[thps] %s\n", c.text.c_str());
            return 1;
        }
        g_chosen = c.version;
    }
    recomp::check_all_stored_roms();  // is_rom_valid() reads what this finds (also a ROM stored just now)
#ifdef __EMSCRIPTEN__
    // A page has no command line: until the browser's storage holds a dump (select_rom keeps it in the config folder,
    // which the page keeps), the player picks theirs in the browser's file picker, which the page shows as a button.
    // A file that isn't a dump this build runs is said why and asked for again.
    if (!g_chosen) {
        auto stored = [] {
            for (const thps::VersionInfo& v : thps::all_versions) {
                if (thps::versions::runnable(v) && recomp::is_rom_valid(game_id_of(v))) return true;
            }
            return false;
        };
        while (!stored()) {
            std::printf("[thps] Load ROM: pick your %s dump (.z64, .n64 or .v64)\n", thps::game::family);
            std::fflush(stdout);
            std::vector<std::filesystem::path> picked = n64recomp_browser::pick_files(false, ".z64,.n64,.v64");
            if (picked.empty()) continue;
            thps::RomCheck c = thps::check_rom_file(picked[0]);
            auto v = c.version ? recomp::select_rom(picked[0], game_id_of(*c.version)) : recomp::RomValidationError(c.error);
            std::error_code ec;
            std::filesystem::remove(picked[0], ec);
            if (v != recomp::RomValidationError::Good) {
                std::fprintf(stderr, "[thps] %s\n", c.text.c_str());
                continue;
            }
            recomp::check_all_stored_roms();
        }
    }
#endif
    if (!g_chosen) {
        // The first runnable version (table order) with a stored ROM.
        for (const thps::VersionInfo& v : thps::all_versions) {
            if (thps::versions::runnable(v) && recomp::is_rom_valid(game_id_of(v))) { g_chosen = &v; break; }
        }
    } else if (!recomp::is_rom_valid(game_id_of(*g_chosen))) {
        std::fprintf(stderr, "[thps] no stored ROM for %s; pass --rom /path/to/rom.z64 (.n64/.v64 also accepted)\n", g_chosen->display_name);
        return 1;
    }
#ifdef THPS_FRONTEND
    // The launcher unless the game is started from the command line (--no-launcher, --rom,
    // --control, --seconds: tests and scripts).
    g_direct_start = !force_launcher && (no_launcher || !rom_path.empty() || thps::options.control || g_run_seconds > 0);
    thps::frontend::set_launcher_enabled(!g_direct_start);
    if (g_direct_start && !g_chosen) {
        std::fprintf(stderr, "[thps] no stored ROM; pass --rom /path/to/%s.z64 (.n64/.v64 also accepted), or start without --no-launcher\n"
                             "[thps] this build runs:%s\n", thps::game::family, thps::versions::supported_list().c_str());
        return 1;
    }
#else
    (void)no_launcher, (void)force_launcher;
    if (!g_chosen) {
        std::fprintf(stderr, "[thps] no stored ROM; pass --rom /path/to/%s.z64 (.n64/.v64 also accepted)\n"
                             "[thps] this build runs:%s\n", thps::game::family, thps::versions::supported_list().c_str());
        return 1;
    }
#endif
    if (thps::ride_needs_step() && (!g_direct_start || !g_chosen || !thps::ride_validate_version(*g_chosen))) {
        std::fprintf(stderr,"[thps] ride: configured timing needs direct THPS2 US native/split startup (%s)\n",thps::ride_error().c_str());
        return 2;
    }
#ifdef THPS_SPLIT_GAME_LIB
    if (g_chosen) {
        // The stored ROM is <game_id>.z64 in the config folder (select_rom stored it, big-endian).
        std::string err;
        if (!thps::game::ensure(*g_chosen, config_dir / (std::string(g_chosen->game_id) + ".z64"), config_dir, game_opt, err)) {
            std::fprintf(stderr, "[thps] cannot prepare the game for %s:\n%s\n", g_chosen->display_name, err.c_str());
            return 4;
        }
        if (build_game_only) { std::printf("[thps] game library ready\n"); return 0; }
    }
#endif
#endif
    if (g_chosen && g_direct_start) {   // the launcher activates the version when Start Game is pressed
        thps::versions::activate(*g_chosen);
        std::printf("[thps] version: %s (%s), code class %s, osTvType %d\n", g_chosen->display_name, g_chosen->id, g_chosen->code_class, thps::versions::tv_type());
    }
    if (thps::options.control) thps::control_start();

    recomp::Configuration cfg{};
    std::vector<char*> runtime_argv{argv[0]};
#ifdef THPS_FRONTEND
    // Started from the command line: librecomp starts the game before the first frame, so the
    // launcher never shows (recompui shows it whenever no game is running and no menu is open).
    if (g_direct_start) {
        runtime_argv.push_back((char*)"--game");
        runtime_argv.push_back((char*)thps::game::mod_game_id);
        g_game_start_sent = true;
    }
#endif
    cfg.argc = int(runtime_argv.size());
    cfg.argv = runtime_argv.data();
    cfg.project_version = recomp::Version(0, 2, 0);
    cfg.window_handle = {};
    cfg.rsp_callbacks = {.get_rsp_microcode = get_rsp_microcode};
    cfg.renderer_callbacks = {.create_render_context = thps::create_render_context};
    cfg.audio_callbacks = {.queue_samples = queue_samples, .get_frames_remaining = get_frames_remaining, .set_frequency = set_frequency};
    cfg.input_callbacks = {.poll_input = poll_input, .get_input = get_input, .set_rumble = set_rumble, .get_connected_device_info = get_connected_device_info};
    cfg.gfx_callbacks = {.create_gfx = create_gfx, .create_window = create_window, .update_gfx = update_gfx};
    cfg.events_callbacks = {.vi_callback = vi_callback, .gfx_init_callback = nullptr};
    // SI messages are not requeued (the runtime's default is true). The games' pad queue (8 slots) is full of unread SI messages in play (the
    // pad poller sends one per frame, nobody reads them all), so every new one found the queue full, was put back on the external-message
    // queue by the idle thread and tried again, forever: a 100 % busy idle thread, and when more of them pile up (the pak re-initialisation
    // of THPS3's PAUSE > RETRY sends four osContStartQuery messages at once) the same producer's re-queued items starve the VI/SP/DP
    // messages, so no thread ever wakes again (hand play 2026-10-13: "freezes after level retry"). On the console osSendMesg to a full
    // queue just fails, which is what dropping the message does. --si-requeue restores the runtime default (A/B).
    cfg.message_queue_control.requeue_si = thps::options.si_requeue;
    cfg.error_handling_callbacks = {.message_box = message_box};
    cfg.threads_callbacks = {.get_game_thread_name = nullptr};
#ifdef THPS_FRONTEND
    thps::frontend::set_callbacks(&cfg);
    // ultramodern::quit() (launcher Exit, the quit prompt) lets recomp::start unmap RDRAM while
    // game threads may still run (see update_gfx): leave as our own quit path does.
    std::thread([] {
        while (!exited.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        print_status("quit", elapsed());
        thps::audio_shutdown();
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(0);
    }).detach();
#endif

    recomp::start(cfg);

    print_status("exit", elapsed());
    // Don't SDL_DestroyWindow/SDL_Quit here (exp 07 gotcha 6: plume CocoaWindow use-after-free).
    // Static destructors still raced with RT64/Cocoa teardown now and then (SIGSEGV/SIGBUS after
    // "exit"), so leave without running them: everything worth keeping (WAV, saves) is flushed.
    thps::audio_shutdown();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(thps::stats.gfx_tasks.load() > 0 ? 0 : 1);
}
