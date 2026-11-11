// ultramodern RendererContext on top of RT64 (Metal on macOS, D3D12 or Vulkan on Windows, Vulkan on Linux).
// Portions adapted from Zelda64Recomp (GPL-3.0), https://github.com/Zelda64Recomp/Zelda64Recomp
// (src/ui/ui_renderer.cpp / rt64_render_context.cpp, as carried in RecompFrontend b1a1477's
// recompui/src/renderer/rt64_render_context.cpp).
// THPS additions: display-list inspector tap, RDRAM frame dumps, present capture, occlusion
// handling, our graphics settings; recompui's render hooks only in the frontend build.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <chrono>
#include <algorithm>
#include <string>
#include <vector>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "hle/rt64_application.h"
#ifdef THPS_MENU
#include "ui/ui_host.h"
#endif
#ifdef THPS_FRONTEND
#include "recompui/recompui.h"
#include "recompui/config.h"
#endif

#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#include "ultramodern/renderer_context.hpp"
#ifdef __EMSCRIPTEN__
#include "ultramodern/displaylists.hpp"
#endif

#include "host.h"
#include "draw_distance.h"
#include "hfr.h"
#include "platform.h"

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

static unsigned int MI_INTR_REG = 0;
static unsigned int DPC_START_REG = 0;
static unsigned int DPC_END_REG = 0;
static unsigned int DPC_CURRENT_REG = 0;
static unsigned int DPC_STATUS_REG = 0;
static unsigned int DPC_CLOCK_REG = 0;
static unsigned int DPC_BUFBUSY_REG = 0;
static unsigned int DPC_PIPEBUSY_REG = 0;
static unsigned int DPC_TMEM_REG = 0;

static void dummy_check_interrupts() {}


namespace {

thps::GfxConfig g_applied_gfx;   // CLI build: the graphics settings RT64 was last given (in-game menu changes are diffed against it)
std::atomic<RT64::Application*> g_app{nullptr};   // the live application, for thps::toggle_fullscreen

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult r) {
    switch (r) {
        case RT64::Application::SetupResult::Success: return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI: return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound: return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound: return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
}

ultramodern::renderer::GraphicsApi map_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12: return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan: return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal: return ultramodern::renderer::GraphicsApi::Metal;
        default: return ultramodern::renderer::GraphicsApi::Auto;
    }
}

const char* api_name(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12: return "D3D12";
        case RT64::UserConfiguration::GraphicsAPI::Vulkan: return "Vulkan";
        case RT64::UserConfiguration::GraphicsAPI::Metal: return "Metal";
        case RT64::UserConfiguration::GraphicsAPI::Automatic: return "Automatic";
        default: return "?";
    }
}


// Present captures (rt64 patch: PresentQueue::captureCallback) arrive on RT64's present thread.
// Copy the pixels and leave the PNG encoding to this writer thread, so a dump never stalls presents.
class PresentDumpWriter {
public:
    struct Job {
        uint64_t index;
        int w, h;
        bool shown;
        std::string path;
        std::vector<uint32_t> px;
        // --present-pair: the same frame's native framebuffer from RDRAM (RT64's copy-back).
        std::string native_path;
        int nw = 0, nh = 0;
        std::vector<uint32_t> native_px;
    };
    void push(Job&& job) {
        {
            std::lock_guard<std::mutex> lk(mutex);
            // The writers can't keep up: drop rather than grow without bound. Sequences of every
            // composited frame (--control pcap 1, src/interpolation.cpp) need a deep queue: ~512 MB.
            if (queued_bytes + job.px.size() * 4 > (size_t(512) << 20)) {
                dropped++;
                return;
            }
            queued_bytes += job.px.size() * 4;
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
        std::call_once(started, [this] {
            for (int i = 0; i < 3; i++) std::thread(&PresentDumpWriter::loop, this).detach();
        });
    }
    uint64_t dropped = 0;

private:
    void loop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mutex);
                cv.wait(lk, [this] { return !jobs.empty(); });
                job = std::move(jobs.front());
                jobs.pop_front();
                queued_bytes -= job.px.size() * 4;
            }
            thps::write_png_rgba(job.path, job.px, job.w, job.h);
            if (!job.native_px.empty()) thps::write_png_rgba(job.native_path, job.native_px, job.nw, job.nh);
            thps::stats.present_dumps++;
            std::printf("[rt64] present %llu: %dx%d%s -> %s\n", (unsigned long long)job.index, job.w, job.h,
                        job.shown ? "" : " (not shown: window occluded)", job.path.c_str());
            std::fflush(stdout);
        }
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> jobs;
    size_t queued_bytes = 0;
    std::once_flag started;
};
PresentDumpWriter g_present_writer;
const uint8_t* g_rdram = nullptr;

// RGBA5551 / RGBA8888 framebuffer in RDRAM -> 0xAABBGGRR pixels.
void read_rdram_fb(uint32_t addr, uint32_t width, uint32_t height, uint32_t bpp, std::vector<uint32_t>& px) {
    px.resize(size_t(width) * height);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t a = addr + (y * width + x) * bpp;
            uint32_t c;
            if (bpp == 2) {
                uint16_t v = *reinterpret_cast<const uint16_t*>(g_rdram + ((a & 0x7FFFFE) ^ 2));
                uint32_t r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
                c = (r * 255 / 31) | ((g * 255 / 31) << 8) | ((b * 255 / 31) << 16) | 0xFF000000u;
            } else {
                uint32_t v = *reinterpret_cast<const uint32_t*>(g_rdram + (a & 0x7FFFFC));
                c = (v >> 24) | (((v >> 16) & 0xFF) << 8) | (((v >> 8) & 0xFF) << 16) | 0xFF000000u;
            }
            px[size_t(y) * width + x] = c;
        }
    }
}

class ThpsRT64Context final : public ultramodern::renderer::RendererContext {
public:
    ThpsRT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
        static unsigned char dummy_rom_header[0x40];

        RT64::Application::Core core{};
#if defined(_WIN32)
        core.window = window_handle.window;                // HWND
#elif defined(__linux__)
        core.window = window_handle;                       // SDL_Window* (RT64_SDL_WINDOW_VULKAN)
#elif defined(__EMSCRIPTEN__)
        (void)window_handle;
        core.window = "#canvas";                           // the page's canvas, which SDL's window is
#elif defined(__APPLE__)
        core.window.window = window_handle.window;
        core.window.view = window_handle.view;
#endif
        core.checkInterrupts = dummy_check_interrupts;
        core.HEADER = dummy_rom_header;
        core.RDRAM = rdram;
        core.DMEM = DMEM;
        core.IMEM = IMEM;
        core.MI_INTR_REG = &MI_INTR_REG;
        core.DPC_START_REG = &DPC_START_REG;
        core.DPC_END_REG = &DPC_END_REG;
        core.DPC_CURRENT_REG = &DPC_CURRENT_REG;
        core.DPC_STATUS_REG = &DPC_STATUS_REG;
        core.DPC_CLOCK_REG = &DPC_CLOCK_REG;
        core.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
        core.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
        core.DPC_TMEM_REG = &DPC_TMEM_REG;

        auto* vi = ultramodern::renderer::get_vi_regs();
        core.VI_STATUS_REG = &vi->VI_STATUS_REG;
        core.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
        core.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
        core.VI_INTR_REG = &vi->VI_INTR_REG;
        core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
        core.VI_TIMING_REG = &vi->VI_TIMING_REG;
        core.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
        core.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
        core.VI_LEAP_REG = &vi->VI_LEAP_REG;
        core.VI_H_START_REG = &vi->VI_H_START_REG;
        core.VI_V_START_REG = &vi->VI_V_START_REG;
        core.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
        core.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
        core.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;

        RT64::ApplicationConfiguration app_config;
        app_config.useConfigurationFile = false;

        app = std::make_unique<RT64::Application>(core, app_config);
        app->userConfig.developerMode = developer_mode;
        app->userConfig.graphicsAPI = pick_graphics_api();
        app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
        app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        apply_gfx_config(app->userConfig);
#ifdef THPS_FRONTEND
        // recompui (launcher, settings menu, FPS counter) draws through RT64's render hooks.
        recompui::set_render_hooks();
#endif
#ifdef THPS_MENU
        thps::ui::set_render_hooks();   // src/ui: the in-game settings menu draws through the same RT64 hooks
#endif

        uint32_t thread_id = 0;
#ifdef _WIN32
        thread_id = window_handle.thread_id;   // RT64 hooks the window's message queue by thread
#endif
        auto result = app->setup(thread_id);
        setup_result = map_setup_result(result);
        chosen_api = map_api(app->chosenGraphicsAPI);
        thps::stats.renderer_api.store(int(app->chosenGraphicsAPI));
        if (thps::verbose() || result != RT64::Application::SetupResult::Success) std::printf("[thps] RT64 setup result=%d chosen API=%s\n", int(result), api_name(app->chosenGraphicsAPI));
        std::fflush(stdout);
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            app = nullptr;
            return;
        }
        setup_present_capture();
#ifdef __EMSCRIPTEN__
        // RT64 marks where it reads each display list's commands from, so a load from ROM can wait for a list it would
        // land on (N64ModernRuntime-web's displaylists.cpp).
        app->state->displayListReads = ultramodern::displaylist_reads();
#endif
        thps::latency_set_app(app.get());
        // RT64 turns MSAA off when the GPU lacks the sample count (Apple GPUs: 2x/4x, no 8x).
        // Quiet terminal (Q19): always shown when MSAA was turned off, otherwise with --verbose.
        const unsigned msaa_eff = app->userConfig.msaaSampleCount();
        if (thps::verbose() || (thps::gfx.msaa > 1 && msaa_eff <= 1))
        std::printf("[thps] gfx effective: msaa=%u refresh=%s\n", msaa_eff,
                    app->userConfig.refreshRate == RT64::UserConfiguration::RefreshRate::Original ? "original"
                    : app->userConfig.refreshRate == RT64::UserConfiguration::RefreshRate::Display ? "display" : "manual");
        g_app.store(app.get());
#ifndef THPS_FRONTEND
        g_applied_gfx = thps::gfx;
#endif
        app->setFullScreen(thps::gfx.fullscreen);
#ifdef THPS_FRONTEND
        // MSAA needs programmable sample positions; recompui greys the option out without them.
        if (!app->device->getCapabilities().sampleLocations) recompui::config::graphics::update_msaa_supported(false);
#endif
    }

    // --api (default auto): macOS has only Metal; elsewhere Automatic = D3D12 on Windows, falling back to Vulkan.
    static RT64::UserConfiguration::GraphicsAPI pick_graphics_api() {
        using API = RT64::UserConfiguration::GraphicsAPI;
        const std::string& a = thps::options.graphics_api;
        if (a == "d3d12") return API::D3D12;
        if (a == "vulkan") return API::Vulkan;
        if (a == "metal") return API::Metal;
#ifdef __APPLE__
        return API::Metal;
#else
        return API::Automatic;
#endif
    }

    // thps::gfx (src/gfx_config.cpp) -> RT64 user configuration.
    static void apply_gfx_config(RT64::UserConfiguration& uc) {
        const auto& g = thps::gfx;
        using AR = RT64::UserConfiguration::AspectRatio;
        if (g.aspect == "expand") {
            uc.aspectRatio = AR::Expand;
        } else if (g.aspect_ratio() > 0) {
            uc.aspectRatio = AR::Manual;
            uc.aspectTarget = g.aspect_ratio();
        } else {
            uc.aspectRatio = AR::Original;
        }
        // HUD placement (src/hud_anchor.cpp): RT64 moves the extended-alignment (LEFT / RIGHT origin) draws to the edges of
        // this aspect; draws without alignment (everything the host doesn't anchor) are unaffected. `original` aligns nothing for one
        // player, but places a two-player split screen's HUD at the edges of the widened views, like `expand`: the window's picture.
        if (thps::hud_supported() && (thps::gfx.hud == "expand" || thps::gfx.hud == "original")) {
            uc.extAspectRatio = AR::Expand;
        } else if (thps::hud_supported() && thps::gfx.hud == "16:9") {
            uc.extAspectRatio = AR::Manual;
            uc.extAspectTarget = 16.0 / 9.0;
        } else {
            uc.extAspectRatio = AR::Original;
        }
        if (g.res_scale > 0) {
            uc.resolution = RT64::UserConfiguration::Resolution::Manual;
            uc.resolutionMultiplier = g.res_scale;
            uc.downsampleMultiplier = std::max(g.downsample, 1);
        } else {
            uc.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            uc.downsampleMultiplier = 1;
        }
        using AA = RT64::UserConfiguration::Antialiasing;
        uc.antialiasing = g.msaa == 8 ? AA::MSAA8X : g.msaa == 4 ? AA::MSAA4X : g.msaa == 2 ? AA::MSAA2X : AA::None;
        // Frame interpolation (src/interpolation.cpp): RT64 renders in-between frames up to this rate.
        using RR = RT64::UserConfiguration::RefreshRate;
        // The settings give Original (30) or 60. Tests and measurements only: THPS_TEST_REFRESH=display|N overrides it for the run (not saved;
        // 120 Hz is not offered: it paced badly on a ProMotion display, 2026-10-23).
        int rate = g.refresh;
        if (const char* t = std::getenv("THPS_TEST_REFRESH")) rate = std::string(t) == "display" ? -1 : std::max(0, std::atoi(t));
#ifdef __EMSCRIPTEN__
        // A web page interpolates to the display's rate (N64Recomp-browser's rule for every page): its choice other than Original is Display.
        else if (rate > 0) rate = -1;
#endif
        uc.refreshRate = rate < 0 ? RR::Display : rate > 0 ? RR::Manual : RR::Original;
        if (rate > 0) uc.refreshRateTarget = rate;
        // Renderer setup and settings updates can apply the same values again.
        // Keep useful changes in the log without repeating the current values.
        static std::mutex log_mutex;
        static std::string last_description;
        const std::string description = thps::gfx_describe();
        std::lock_guard<std::mutex> lock(log_mutex);
        if (description != last_description) {
            std::printf("[thps] gfx: %s\n", description.c_str());
            std::fflush(stdout);
            last_description = description;
        }
    }

    // --control "edges N": compares the border bands (outer 1/24 of the width on each side) of
    // consecutive composited frames, on the present thread, without writing PNGs. A flickering
    // border (stale or uninitialised pixels that differ between framebuffers) shows up as a large
    // frame-to-frame difference there while the screen itself stands still.
    // The top and bottom bands (outer 1/24 of the height, all columns) are measured the same way: a 2D screen whose picture is
    // framed by black bars (THPS1's skater / course select) flashed stale picture rows in those bands on some frames.
    struct EdgeMeter {
        std::vector<uint8_t> prev, prev_rows;  // luma of both column bands / of both row bands, previous frame
        int w = 0, h = 0, frames = 0;
        double worst_left = 0, worst_right = 0, worst_col = 0, sum_left = 0, sum_right = 0;
        double worst_top = 0, worst_bottom = 0, worst_row = 0, sum_top = 0, sum_bottom = 0;
        int worst_col_x = -1, worst_row_y = -1;
    };
    static inline std::atomic<uint32_t> g_edges_saved_every{0};
    static inline std::atomic<bool> g_edges_active{false};
    static EdgeMeter& edge_meter() { static EdgeMeter m; return m; }

    static void measure_edges(const RT64::PresentQueue::Capture& c, RT64::PresentQueue* pq) {
        EdgeMeter& m = edge_meter();
        const int w = int(c.width), h = int(c.height), band = std::max(1, w / 24);
        if (m.w != w || m.h != h) { m = EdgeMeter{}; m.w = w; m.h = h; }
        std::vector<uint8_t> cur(size_t(2 * band) * h);
        for (int y = 0; y < h; y++) {
            const uint8_t* row = c.bgra + size_t(y) * c.rowPitch;
            for (int i = 0; i < 2 * band; i++) {
                const int x = i < band ? i : w - 2 * band + i;
                const uint8_t* p = row + x * 4;  // BGRA
                cur[size_t(y) * 2 * band + i] = uint8_t((29 * p[0] + 150 * p[1] + 77 * p[2]) >> 8);
            }
        }
        const int bandh = std::max(1, h / 24);
        std::vector<uint8_t> cur_rows(size_t(2 * bandh) * w);
        for (int i = 0; i < 2 * bandh; i++) {
            const int y = i < bandh ? i : h - 2 * bandh + i;
            const uint8_t* row = c.bgra + size_t(y) * c.rowPitch;
            for (int x = 0; x < w; x++) {
                const uint8_t* p = row + x * 4;
                cur_rows[size_t(i) * w + x] = uint8_t((29 * p[0] + 150 * p[1] + 77 * p[2]) >> 8);
            }
        }
        if (!m.prev_rows.empty()) {
            double t = 0, b = 0;
            for (int i = 0; i < 2 * bandh; i++) {
                double d = 0;
                for (int x = 0; x < w; x++) d += std::abs(int(cur_rows[size_t(i) * w + x]) - int(m.prev_rows[size_t(i) * w + x]));
                d /= w;
                (i < bandh ? t : b) += d;
                if (d > m.worst_row) { m.worst_row = d; m.worst_row_y = i < bandh ? i : h - 2 * bandh + i; }
            }
            t /= bandh, b /= bandh;
            m.worst_top = std::max(m.worst_top, t), m.worst_bottom = std::max(m.worst_bottom, b);
            m.sum_top += t, m.sum_bottom += b;
        }
        m.prev_rows.swap(cur_rows);
        if (!m.prev.empty()) {
            std::vector<double> col(2 * band, 0.0);
            for (int y = 0; y < h; y++)
                for (int i = 0; i < 2 * band; i++)
                    col[i] += std::abs(int(cur[size_t(y) * 2 * band + i]) - int(m.prev[size_t(y) * 2 * band + i]));
            double l = 0, r = 0;
            for (int i = 0; i < 2 * band; i++) {
                col[i] /= h;
                (i < band ? l : r) += col[i];
                if (col[i] > m.worst_col) { m.worst_col = col[i]; m.worst_col_x = i < band ? i : w - 2 * band + i; }
            }
            l /= band, r /= band;
            m.worst_left = std::max(m.worst_left, l), m.worst_right = std::max(m.worst_right, r);
            m.sum_left += l, m.sum_right += r;
            m.frames++;
        }
        m.prev.swap(cur);
        if (--thps::edges_frames_left <= 0) {
            char buf[480];
            std::snprintf(buf, sizeof buf, "ok edges frames=%d size=%dx%d band=%d left_mean=%.2f left_max=%.2f right_mean=%.2f right_max=%.2f col_max=%.2f col_x=%d "
                          "top_mean=%.2f top_max=%.2f bottom_mean=%.2f bottom_max=%.2f row_max=%.2f row_y=%d",
                          m.frames, w, h, band, m.frames ? m.sum_left / m.frames : 0.0, m.worst_left, m.frames ? m.sum_right / m.frames : 0.0,
                          m.worst_right, m.worst_col, m.worst_col_x, m.frames ? m.sum_top / m.frames : 0.0, m.worst_top,
                          m.frames ? m.sum_bottom / m.frames : 0.0, m.worst_bottom, m.worst_row, m.worst_row_y);
            pq->captureEvery = g_edges_saved_every.load();
            g_edges_active = false;
            m = EdgeMeter{};
            thps::control_reply(buf);
        }
    }

    void setup_present_capture() {
        g_rdram = app->core.RDRAM;
        auto* pq = app->presentQueue.get();
        pq->captureWidth = uint32_t(thps::options.present_w);
        pq->captureHeight = uint32_t(thps::options.present_h);
        pq->captureCallback = [pq](const RT64::PresentQueue::Capture& c) {
            if (thps::latency_on_present(c.bgra, c.width, c.height, c.rowPitch, c.presentIndex)) return;  // src/latency.cpp
            if (thps::edges_frames_left.load() > 0) {
                static int edge_shots = 0;
                const bool first = edge_meter().w == 0;
                if (first) edge_shots = 0;
                measure_edges(c, pq);
                if (!thps::edges_dump_dir.empty() && edge_shots < 6) {
                    char name[96];
                    std::snprintf(name, sizeof name, "edges_%06llu_%ux%u.png", (unsigned long long)c.presentIndex, c.width, c.height);
                    // Written by the PNG writer threads: encoding here (~50 ms per 1280x720 frame) stalls
                    // RT64's present thread, and a stalled present thread itself makes the widescreen side
                    // bands of the pause menu flicker (Q21: the measurement disturbed what it measured).
                    PresentDumpWriter::Job ej;
                    ej.index = c.presentIndex;
                    ej.w = int(c.width);
                    ej.h = int(c.height);
                    ej.shown = c.shownInWindow;
                    ej.path = thps::platform::to_utf8(std::filesystem::path(thps::edges_dump_dir) / name);
                    ej.px.resize(size_t(c.width) * c.height);
                    for (uint32_t y = 0; y < c.height; y++)
                        for (uint32_t x = 0; x < c.width; x++) {
                            const uint8_t* q = c.bgra + size_t(y) * c.rowPitch + x * 4;
                            ej.px[size_t(y) * c.width + x] = uint32_t(q[2]) | (uint32_t(q[1]) << 8) | (uint32_t(q[0]) << 16) | 0xFF000000u;
                        }
                    g_present_writer.push(std::move(ej));
                    edge_shots++;
                }
                // Only the regular --present-dump-every frames go on to be written as PNGs.
                const uint32_t every = uint32_t(std::max(0, thps::options.present_dump_every));
                if (thps::options.dump_dir.empty() || every == 0 || (c.presentIndex % every) != 0) return;
            }
            PresentDumpWriter::Job job;
            job.index = c.presentIndex;
            job.w = int(c.width);
            job.h = int(c.height);
            job.shown = c.shownInWindow;
            char name[96];
            std::snprintf(name, sizeof name, "present_%06llu_%ux%u.png", (unsigned long long)c.presentIndex, c.width, c.height);
            job.path = thps::platform::to_utf8(thps::options.dump_dir / name);
            job.px.resize(size_t(c.width) * c.height);
            for (uint32_t y = 0; y < c.height; y++) {
                const uint8_t* row = c.bgra + size_t(y) * c.rowPitch;
                uint32_t* out = job.px.data() + size_t(y) * c.width;
                for (uint32_t x = 0; x < c.width; x++) {
                    const uint8_t* p = row + x * 4;
                    out[x] = uint32_t(p[2]) | (uint32_t(p[1]) << 8) | (uint32_t(p[0]) << 16) | 0xFF000000u;
                }
            }
            if (thps::options.present_pair && g_rdram && (c.fbSiz == 2 || c.fbSiz == 3) && c.fbWidth >= 64 && c.fbWidth <= 1024 &&
                c.fbHeight > 0 && c.fbHeight <= 1024) {
                job.nw = int(c.fbWidth);
                job.nh = int(c.fbHeight);
                read_rdram_fb(c.fbAddress, c.fbWidth, c.fbHeight, c.fbSiz == 2 ? 2 : 4, job.native_px);
                std::snprintf(name, sizeof name, "present_%06llu_native_%ux%u.png", (unsigned long long)c.presentIndex, c.fbWidth, c.fbHeight);
                job.native_path = thps::platform::to_utf8(thps::options.dump_dir / name);
            }
            g_present_writer.push(std::move(job));
        };
        if (!thps::options.dump_dir.empty() && thps::options.present_dump_every > 0) {
            pq->captureEvery = uint32_t(thps::options.present_dump_every);
        }
    }

    bool valid() override { return static_cast<bool>(app); }

    // The settings menu changed (recompui Graphics tab -> ultramodern GraphicsConfig, or one of
    // our own options, frontend.cpp): apply it now. Command-line overrides stop applying.
    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) override {
        bool ours = thps::settings::take_changed();
        if (old_config == new_config && !ours) return false;
        thps::GfxConfig before = thps::gfx;
#ifdef THPS_FRONTEND
        thps::gfx_from_settings(new_config, thps::settings::widescreen(), false);
#else
        // CLI build with the in-game menu (THPS_MENU): the menu already changed thps::gfx (and graphics.cfg);
        // compare with what RT64 was last given.
        before = g_applied_gfx;
#endif
        const thps::GfxConfig& g = thps::gfx;
        if (g.fullscreen != before.fullscreen) app->setFullScreen(g.fullscreen);
        apply_gfx_config(app->userConfig);
        bool discard = g.res_scale != before.res_scale || g.downsample != before.downsample || g.aspect != before.aspect || g.msaa != before.msaa;
        app->updateUserConfig(discard);
        if (g.msaa != before.msaa) app->updateMultisampling();
#ifndef THPS_FRONTEND
        g_applied_gfx = g;
#endif
        return true;
    }

    void enable_instant_present() override {
        app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        app->updateEnhancementConfig();
    }

    static void note_time(std::atomic<uint64_t>& total, std::atomic<uint64_t>& max, std::chrono::steady_clock::time_point t0) {
        uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        total += us;
        uint64_t m = max.load();
        while (us > m && !max.compare_exchange_weak(m, us)) {}
    }

    void send_dl(const OSTask* task) override {
        auto t0 = std::chrono::steady_clock::now();
        uint64_t n = thps::stats.gfx_tasks.fetch_add(1);
        thps::fix_display_list(app->core.RDRAM, task->t.data_ptr);
        thps::crop_display_list(app->core.RDRAM, task->t.data_ptr);   // graphics setting border=fill (src/border_crop.cpp)
        thps::fog_display_list(app->core.RDRAM, task->t.data_ptr);    // graphics setting fog=soft|off (src/fog.cpp)
        thps::inspect_display_list(app->core.RDRAM, uint32_t(n), task->t.ucode, task->t.data_ptr, task->t.data_size);
        thps::dd::note_task(app->core.RDRAM, task->t.data_ptr, task->t.data_size);   // src/draw_distance.cpp: buffer high-water marks
        app->state->rsp->reset();
        app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
        thps::hfr_task_begin(app->state.get(), app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF);  // src/interpolation.cpp
        const bool canary = thps::dl_canary_enabled();
        if (canary) thps::dl_canary_begin(app->core.RDRAM, task->t.data_ptr);
        {   // Debug (THPS_DL_STALL_MS=N): hold the gfx thread N ms before it parses every THPS_DL_STALL_EVERY-th (default every) list, as a loaded machine or a full RT64 workload ring does.
            static const int stall_ms = std::getenv("THPS_DL_STALL_MS") ? std::atoi(std::getenv("THPS_DL_STALL_MS")) : 0;
            static const int every = std::getenv("THPS_DL_STALL_EVERY") ? std::max(1, std::atoi(std::getenv("THPS_DL_STALL_EVERY"))) : 1;
            if (stall_ms > 0 && (n % every) == (uint64_t(every) > 3 ? 3 : 0)) std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
        }
        app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
#ifdef __EMSCRIPTEN__
        ultramodern::displaylist_read();   // N64ModernRuntime-web: a load from ROM waits for the lists that read where it lands
#endif
        if (canary) thps::dl_canary_end(app->core.RDRAM, n);
        thps::hfr_task_end(app->state.get());
        note_time(thps::stats.send_dl_us, thps::stats.send_dl_us_max, t0);
        thps::ftime_span("send_dl", t0);
    }

    // Called by ultramodern's VI thread every frame while no game is running.
    void send_dummy_workload(uint32_t fb_address) override {
        thps::stats.dummy_workloads.fetch_add(1);
        app->state->listProcessBegin();
        app->state->rdp->setColorImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, fb_address);
        app->state->rdp->setOtherMode(0x382C30, 0); // G_CYC_FILL etc. (same as recompui)
        app->state->rdp->setFillColor(0);  // black behind the launcher / menus (no game running)
        app->state->rdp->fillRect(0, 0, 320 << 2, 240 << 2);
#ifdef __EMSCRIPTEN__
        // Not copied to RDRAM: the dummy framebuffer is game memory once the game starts, and on a page these lists
        // can fall seconds behind the VIs that queue them, so the copies would land on what the game has loaded there.
        app->state->setRenderToRAM(0);
#endif
        app->state->fullSync();
        app->state->listProcessEnd();
    }

    void update_screen() override {
        auto t0 = std::chrono::steady_clock::now();
        uint32_t origin = ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG;
        bool new_frame = origin != last_origin;
        if (new_frame) {
            last_origin = origin;
            uint64_t frame = thps::stats.vi_origin_changes++;
            if (want_rt64_dump(frame)) dump_vi_frame(frame);
        }
        if (thps::options.control) {
            uint64_t frame = thps::stats.vi_origin_changes.load();
            if (frame > 0) {
                std::string shot = thps::control_take_shot(new_frame);
                if (!shot.empty()) dump_vi_frame(frame - 1, shot);
                if (new_frame) thps::control_frame(frame - 1);
            }
            if (thps::edges_frames_left.load() > 0 && !g_edges_active.exchange(true)) {
                g_edges_saved_every = app->presentQueue->captureEvery.load();
                app->presentQueue->captureEvery = 1;  // capture every composited frame while measuring
            }
        }
        // While the window is occluded, CAMetalLayer.nextDrawable blocks ~1 s per present, and that
        // stall would reach the game through RT64's workload -> present chain. So RT64 keeps
        // compositing (VI history, present captures) but leaves the swap chain alone (rt64 patch:
        // PresentQueue::swapChainEnabled).
        bool show = thps::stats.window_visible.load() || thps::options.always_present;
        app->presentQueue->swapChainEnabled = show;
        // While the window isn't shown, RT64 draws the UI offscreen (rt64 patch) when there is
        // UI to draw, so menus keep working and present captures include them.
#ifdef THPS_FRONTEND
        app->presentQueue->captureUi = thps::frontend::ui_wanted();
#elif defined(THPS_MENU)
        app->presentQueue->captureUi = thps::ui::wanted();
#endif
        app->updateScreen();
        if (!show) thps::stats.presents_skipped++;
        if (thps::verbose()) {
            static int n = 0;
            if ((n++ % 60) == 0) {
                auto* sr = app->presentQueue->ext.sharedResources;
                std::printf("[rt64] resolutionScale %.3f x %.3f swapchain %ux%u aspect mode %d target %.3f\n", float(sr->resolutionScale.x),
                            float(sr->resolutionScale.y), sr->swapChainWidth, sr->swapChainHeight, int(sr->userConfig.aspectRatio),
                            float(sr->userConfig.aspectTarget));
            }
        }
        thps::stats.rt64_composited.store(app->presentQueue->presentedFrameCount.load());
        thps::stats.rt64_window_presents.store(app->presentQueue->windowPresentCount.load());
        thps::stats.frames_presented.fetch_add(1);
        note_time(thps::stats.update_screen_us, thps::stats.update_screen_us_max, t0);
        thps::ftime_span("update_screen", t0);
    }

    void shutdown() override {
        if (app) {
            app->end();
        }
    }

    uint32_t get_display_framerate() const override {
        return app->presentQueue->ext.sharedResources->swapChainRate;
    }

    float get_resolution_scale() const override { return 1.0f; }

private:
    static bool want_rt64_dump(uint64_t frame) {
        const auto& o = thps::options;
        if (o.dump_dir.empty()) return false;
        if (o.rt64_dump_every > 0 && frame % uint64_t(o.rt64_dump_every) == 0) return true;
        size_t p = 0;
        const std::string& l = o.rt64_dump_frames;
        while (p < l.size()) {
            size_t q = l.find(',', p);
            if (q == std::string::npos) q = l.size();
            if (q > p && std::strtoull(l.substr(p, q - p).c_str(), nullptr, 10) == frame) return true;
            p = q + 1;
        }
        return false;
    }

    // RT64 copies every finished framebuffer back to RDRAM at native resolution (State::fullSync:
    // copyRenderTargetToNative + copyNativeToRAM), so the frame the VI is about to scan out is
    // RT64's GPU (Metal) result, not the game's or dl_inspect's. Write it as a PNG (local only).
    void dump_vi_frame(uint64_t frame, const std::string& ctl_path = {}) {
        const auto* vi = ultramodern::renderer::get_vi_regs();
        uint32_t width = vi->VI_WIDTH_REG & 0xFFF;
        uint32_t type = vi->VI_STATUS_REG & 3;  // 2 = 16-bit, 3 = 32-bit
        uint32_t origin = vi->VI_ORIGIN_REG & 0xFFFFFF;
        // Visible lines from the VI timing: (vEnd - vStart) / 2 half-lines, scaled by yScale (2.10).
        uint32_t v_start = (vi->VI_V_START_REG >> 16) & 0x3FF, v_end = vi->VI_V_START_REG & 0x3FF;
        uint32_t y_scale = vi->VI_Y_SCALE_REG & 0xFFF;
        uint32_t lines = (v_end > v_start ? (v_end - v_start) / 2 : 240);
        uint32_t height = y_scale ? std::min<uint32_t>(lines * y_scale / 1024, 1024) : lines;
        if (width < 64 || width > 1024 || height == 0 || (type != 2 && type != 3)) {
            std::printf("[rt64] frame %llu: VI width %u type %u height %u not dumpable\n", (unsigned long long)frame, width, type, height);
            if (!ctl_path.empty()) thps::control_shot_done(ctl_path, frame, 0, 0, 0.0);
            return;
        }
        const uint8_t* rdram = app->core.RDRAM;
        uint32_t bpp = type == 2 ? 2 : 4;
        // VI_ORIGIN points at the first scanned-out pixel. libultra's modes put it one line into
        // the framebuffer (THPS: cimg 0x569FC0, origin 0x56A4C0 = +640*2), so the image is the
        // visible lines only (474 of 480, 237 of 240).
        uint32_t base = origin;
        std::vector<uint32_t> px(size_t(width) * height);
        uint64_t sum = 0;
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t a = base + (y * width + x) * bpp;
                uint32_t c;
                if (bpp == 2) {
                    uint16_t v = *reinterpret_cast<const uint16_t*>(rdram + (((a) & 0x7FFFFE) ^ 2));
                    uint32_t r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
                    c = (r * 255 / 31) | ((g * 255 / 31) << 8) | ((b * 255 / 31) << 16) | 0xFF000000u;
                } else {
                    uint32_t v = *reinterpret_cast<const uint32_t*>(rdram + (a & 0x7FFFFC));
                    c = (v >> 24) | (((v >> 16) & 0xFF) << 8) | (((v >> 8) & 0xFF) << 16) | 0xFF000000u;
                }
                px[size_t(y) * width + x] = c;
                sum += (c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF);
            }
        }
        if (!ctl_path.empty()) {
            thps::write_png_rgba(ctl_path, px, int(width), int(height));
            thps::control_shot_done(ctl_path, frame, int(width), int(height), double(sum) / (3.0 * px.size()));
            return;
        }
        char name[96];
        std::snprintf(name, sizeof name, "rt64_frame%06llu_%ux%u_%06X.png", (unsigned long long)frame, width, height, origin);
        thps::write_png_rgba(thps::platform::to_utf8(thps::options.dump_dir / name), px, int(width), int(height));
        std::printf("[rt64] frame %llu (gfx task %llu): VI origin 0x%06X %ux%u %s-bit, mean level %.1f -> %s\n", (unsigned long long)frame,
                    (unsigned long long)thps::stats.gfx_tasks.load(), origin, width, height, bpp == 2 ? "16" : "32",
                    double(sum) / (3.0 * px.size()), name);
        std::fflush(stdout);
    }

    std::unique_ptr<RT64::Application> app;
    uint32_t last_origin = 0;
};

} // namespace

void thps::toggle_fullscreen() {
    if (RT64::Application* a = g_app.load()) {
        gfx.fullscreen = !gfx.fullscreen;
        a->setFullScreen(gfx.fullscreen);
        if (verbose()) std::printf("[thps] fullscreen %s\n", gfx.fullscreen ? "on" : "off");
    }
}

std::unique_ptr<ultramodern::renderer::RendererContext> thps::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<ThpsRT64Context>(rdram, window_handle, developer_mode);
}
