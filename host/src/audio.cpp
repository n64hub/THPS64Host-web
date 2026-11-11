// Audio output for the THPS hosts. The game's n_aspMain audio tasks run as RSPRecomp'd code
// (config/thps1.aspMain.toml); libultra's osAiSetNextBuffer hands each finished buffer to
// ultramodern, which calls queue_samples() here.
//
// Samples arrive as interleaved s16 stereo at the game's rate (22050 Hz for THPS1), with L/R
// swapped by the RDRAM halfword address XOR (same fix as Zelda64Recomp's queue_samples).
// Output: an SDL callback-mode device opened at the game's rate (SDL converts to the hardware
// rate), fed from our own ring buffer. The ring lets the callback see an underrun (the game
// thread stalled for longer than the queue holds: 50-150 ms at level loads, unpausing, a busy
// machine) and declick it: the last sample is ramped to zero instead of SDL's hard cut to silence
// (a step = a click), and the audio fades back in when buffers arrive again, crossfading from the
// held frame when the underrun was shorter than its fade-out (2026-10-15 play-test
// "popping"; the private development notes). --mute keeps a virtual device that drains in real time so
// the game's pacing (via osAiGetStatus/osAiGetLength -> get_frames_remaining) is the same without
// sound. --wav PATH also writes everything queued to a 16-bit stereo WAV (ROM-derived: keep it
// local). THPS_AUDIO_LOG=FILE writes one CSV row per queued buffer (queue depth, underrun counts).
//
// On a page (__EMSCRIPTEN__) the sound plays through N64Recomp-browser's n64recomp_browser::audio, an AudioWorklet on
// the browser's audio thread reading a ring in wasm memory, in place of the SDL device: SDL2's port plays from the
// page's thread, which WebGPU keeps busy. Each buffer, swapped and at the volume, goes through an SDL_AudioStream from
// the game's rate to the page's, and the frames still to play are counted back at the game's rate. The page's output
// has no declick: an underrun there is silence.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#define SDL_MAIN_HANDLED
#include "sdl_include.h"

#include "host.h"
#include "platform.h"
#include "thps_hooks.h"
#ifdef __EMSCRIPTEN__
#include "audio_output.h"   // N64Recomp-browser's frontend/
#endif

namespace {

std::mutex g_mutex;
SDL_AudioDeviceID g_dev = 0;
uint32_t g_freq = 0;
bool g_sdl_ok = false;
// Virtual device (--mute, or SDL audio unavailable): frames queued minus frames played.
double g_virtual_frames = 0;
std::chrono::steady_clock::time_point g_virtual_t;
std::vector<int16_t> g_buf;

std::atomic<bool> g_have_last{false};  // a buffer has been queued (underruns count only after the first)

// SDL device ring (callback mode). Its own mutex: the SDL audio thread takes only this one, so
// SDL_CloseAudioDevice under g_mutex cannot deadlock with the callback.
std::mutex g_ring_mutex;
std::vector<int16_t> g_ring;     // interleaved stereo, capacity kRingFrames
size_t g_ring_read = 0, g_ring_fill = 0;  // in frames
constexpr size_t kRingFrames = 1 << 16;   // ~3 s at 22050 Hz; the FIFO pacing keeps it < 100 ms
constexpr int kFadeFrames = 64;           // declick ramps: ~2.9 ms at 22050 Hz
bool g_dry = true;                // the last callback ran out of samples
int g_fade_in = kFadeFrames;      // frames into the fade-in after an underrun (kFadeFrames = done)
int g_fade_out = kFadeFrames;     // frames into the fade-out of the held frame (kFadeFrames = done)
int16_t g_last_l = 0, g_last_r = 0;  // last frame output (before the volume): what an underrun holds
int16_t g_hold_l = 0, g_hold_r = 0;  // the frame held at the last underrun, fading out

bool g_declick = true;            // THPS_AUDIO_DECLICK=0: hard cuts as SDL's own underrun silence (A/B tests)
int16_t g_prev_l = 0, g_prev_r = 0;  // last frame output (edge step measurement)
constexpr int kEdgeFrames = 4;    // edge_step looks at the first frames after each edge (the step the edge adds, not the music's own)
int g_edge_left = 0;              // frames left to measure after an underrun edge

// The held frame's remaining gain (out of kFadeFrames) and the next step of its fade-out.
#ifdef __EMSCRIPTEN__
uint32_t g_web_rate = 0;               // the page's output rate (its audio context's)
SDL_AudioStream* g_web_stream = nullptr;   // the game's rate, s16, to the page's, float
#endif

int hold_gain() { return kFadeFrames - std::min(g_fade_out, kFadeFrames); }
void hold_step() { if (g_fade_out < kFadeFrames) g_fade_out++; }

void SDLCALL device_callback(void*, Uint8* stream, int len) {
    auto* out = reinterpret_cast<int16_t*>(stream);
    size_t frames = size_t(len) / 4;
    std::lock_guard lock{g_ring_mutex};
    const bool counting = g_have_last.load();
    const int gain = int(thps::settings::volume() * 256.0f + 0.5f);   // master volume (also in queue_samples for the WAV/stats)
    // --control "audiogap N": N frames in the middle of this callback play as an underrun even though samples are
    // queued, so the output resumes within the same callback (a short underrun, deterministic; declick test).
    size_t gap0 = frames, gap1 = frames;
    if (uint32_t g = thps::stats.audio_gap_frames.exchange(0)) {
        gap0 = frames / 4;
        gap1 = std::min(frames, gap0 + g);
    }
    size_t used = 0;   // ring frames consumed
    uint64_t edge_step = 0, edge_raw = 0, starved = 0;
    for (size_t i = 0; i < frames; i++) {
        int l, r;
        if (used < g_ring_fill && (i < gap0 || i >= gap1)) {
            size_t k = (g_ring_read + used) % kRingFrames;
            used++;
            l = g_ring[2 * k];
            r = g_ring[2 * k + 1];
            if (g_dry) {   // the first frame after an underrun
                g_dry = false;
                g_fade_in = g_declick ? 0 : kFadeFrames;
                if (!g_declick) g_fade_out = kFadeFrames;
                g_edge_left = kEdgeFrames;
                edge_raw = std::max<uint64_t>(edge_raw, uint64_t(std::max(std::abs(l), std::abs(r))));
            }
            if (g_fade_in < kFadeFrames) {
                // Resuming: ramp the new samples up from silence and keep fading the held frame out under them. A short
                // underrun can resume before that fade-out ends: the output then blends continuously from the held,
                // partly faded value into the music instead of jumping to the ramp's zero (Astra review C1).
                int h = hold_gain();
                l = (l * g_fade_in + g_hold_l * h) / kFadeFrames;
                r = (r * g_fade_in + g_hold_r * h) / kFadeFrames;
                g_fade_in++;
                hold_step();
            }
            g_last_l = int16_t(std::clamp(l, -32768, 32767));
            g_last_r = int16_t(std::clamp(r, -32768, 32767));
            l = g_last_l;
            r = g_last_r;
        } else {
            starved++;
            if (!g_dry) {   // underrun: the game has not queued the next buffer in time
                g_dry = true;
                if (counting) thps::stats.audio_underruns++;
                // Hold what was just played (mid fade-in, that already includes any older held frame) and fade it out.
                g_hold_l = g_last_l;
                g_hold_r = g_last_r;
                g_fade_out = g_declick ? 0 : kFadeFrames;
                g_edge_left = kEdgeFrames;
                edge_raw = std::max<uint64_t>(edge_raw, uint64_t(std::max(std::abs(int(g_last_l)), std::abs(int(g_last_r)))));
            }
            // hold the last frame and ramp it to zero (declick), then silence
            int h = hold_gain();
            l = g_hold_l * h / kFadeFrames;
            r = g_hold_r * h / kFadeFrames;
            hold_step();
            g_last_l = int16_t(l);
            g_last_r = int16_t(r);
        }
        if (g_edge_left > 0) {
            g_edge_left--;
            edge_step = std::max<uint64_t>(edge_step, uint64_t(std::max(std::abs(l - g_prev_l), std::abs(r - g_prev_r))));
        }
        g_prev_l = int16_t(l);
        g_prev_r = int16_t(r);
        if (gain != 256) {
            l = l * gain / 256;
            r = r * gain / 256;
        }
        out[2 * i] = int16_t(l);
        out[2 * i + 1] = int16_t(r);
    }
    g_ring_read = (g_ring_read + used) % kRingFrames;
    g_ring_fill -= used;
    if (counting) thps::stats.audio_starved_frames += starved;
    if (counting) {
        if (edge_step > thps::stats.audio_edge_step_max.load()) thps::stats.audio_edge_step_max.store(edge_step);
        if (edge_raw > thps::stats.audio_edge_raw_max.load()) thps::stats.audio_edge_raw_max.store(edge_raw);
    }
}

FILE* g_alog = nullptr;  // THPS_AUDIO_LOG=FILE: one CSV row per buffer (debugging)

FILE* g_wav = nullptr;
uint64_t g_wav_frames = 0;
uint32_t g_wav_freq = 0;

void wav_header(FILE* f, uint32_t freq, uint64_t frames) {
    uint32_t data = uint32_t(std::min<uint64_t>(frames * 4, 0xFFFFFFF0ull));
    auto u32 = [&](uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); };
    auto u16 = [&](uint16_t v) { uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); };
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f); u32(36 + data); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(freq); u32(freq * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(data);
}

void virtual_drain() {
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - g_virtual_t).count();
    g_virtual_t = now;
    double played = dt * (g_freq ? g_freq : 22050);
    if (played > g_virtual_frames && g_have_last) {
        if (g_virtual_frames > 0) thps::stats.audio_underruns++;
        thps::stats.audio_starved_frames += uint64_t(played - g_virtual_frames);
    }
    g_virtual_frames = std::max(0.0, g_virtual_frames - played);
}

#ifdef __EMSCRIPTEN__
// The frames the page has yet to play and the stream still holds, in the game's frames.
size_t web_frames_remaining() {
    const uint64_t page = uint64_t(n64recomp_browser::audio::queued_frames()) +
        uint64_t(SDL_AudioStreamAvailable(g_web_stream)) / (sizeof(float) * 2);
    return size_t(page * g_freq / g_web_rate);
}
#endif

void open_device_locked(uint32_t freq) {
    if (g_dev) {
#ifndef __EMSCRIPTEN__
        SDL_CloseAudioDevice(g_dev);
#endif
        g_dev = 0;
    }
    {
        std::lock_guard rl{g_ring_mutex};
        g_ring.assign(kRingFrames * 2, 0);
        g_ring_read = g_ring_fill = 0;
        g_dry = true;
        g_fade_in = g_fade_out = kFadeFrames;
        g_last_l = g_last_r = g_hold_l = g_hold_r = g_prev_l = g_prev_r = 0;
        g_edge_left = 0;
    }
    g_freq = freq;
    g_virtual_frames = 0;
    g_virtual_t = std::chrono::steady_clock::now();
    if (thps::options.mute || !g_sdl_ok) return;
#ifdef __EMSCRIPTEN__
    if (g_web_rate == 0) g_web_rate = n64recomp_browser::audio::open(2);
    if (g_web_stream) SDL_FreeAudioStream(g_web_stream);
    g_web_stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, int(freq), AUDIO_F32SYS, 2, int(g_web_rate));
    if (!g_web_stream) {
        std::fprintf(stderr, "[thps] audio: can't convert %u Hz to the page's %u Hz: %s (muted)\n", freq, g_web_rate, SDL_GetError());
        return;
    }
    g_dev = 1;   // open: the page's output, not an SDL device
    std::printf("[thps] audio: the page's output at %u Hz, from %u Hz stereo s16\n", g_web_rate, freq);
    std::fflush(stdout);
    return;
#endif
    SDL_AudioSpec want{}, have{};
    want.freq = int(freq);
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = device_callback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        std::fprintf(stderr, "[thps] audio: SDL_OpenAudioDevice(%u Hz) failed: %s (muted)\n", freq, SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    std::printf("[thps] audio: SDL device %u at %u Hz stereo s16 (%s)\n", g_dev, freq, SDL_GetCurrentAudioDriver());
    std::fflush(stdout);
}

}  // namespace

void thps::audio_init() {
    std::lock_guard lock{g_mutex};
    if (!options.mute) {
#ifdef __EMSCRIPTEN__
        g_sdl_ok = true;   // the page's output needs no SDL audio (n64recomp_browser::audio)
#else
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) g_sdl_ok = true;
        else std::fprintf(stderr, "[thps] audio: SDL audio init failed: %s (muted)\n", SDL_GetError());
#endif
    }
    if (!options.wav_path.empty()) {
        g_wav = platform::fopen_path(options.wav_path, "wb");
        if (!g_wav) std::fprintf(stderr, "[thps] audio: can't write %s\n", options.wav_path.string().c_str());
        else wav_header(g_wav, 22050, 0);
    }
    if (const char* p = std::getenv("THPS_AUDIO_DECLICK"); p && std::strcmp(p, "0") == 0) g_declick = false;
    if (const char* p = std::getenv("THPS_AUDIO_LOG"); p && *p) {
        g_alog = std::fopen(p, "w");
        if (g_alog) std::fprintf(g_alog, "t_ms,queued_before,frames,peak,first_l,last_l,underruns,starved_frames\n");
    }
}

void thps::audio_set_frequency(uint32_t freq) {
    std::lock_guard lock{g_mutex};
    if (freq == g_freq) return;
    if (thps::verbose()) std::printf("[thps] audio set_frequency %u\n", freq);
    std::fflush(stdout);
    open_device_locked(freq);
    if (g_wav && g_wav_frames == 0) g_wav_freq = freq;
}

void thps::audio_queue_samples(const int16_t* samples, size_t count) {
    size_t frames = count / 2;
    if (uint32_t ms = stats.audio_stall_ms.exchange(0)) std::this_thread::sleep_for(std::chrono::milliseconds(ms));  // test: a stall
    std::lock_guard lock{g_mutex};
    g_buf.resize(frames * 2);
    uint64_t peak = 0, sumsq = 0;
    // Master volume (settings: general.cfg volume / Sound tab / --volume), applied to what is recorded and
    // measured here; the device callback applies the same gain to what is heard.
    const int gain = int(settings::volume() * 256.0f + 0.5f);
    for (size_t i = 0; i < frames; i++) {
        int16_t l = samples[2 * i + 1], r = samples[2 * i + 0];  // undo the halfword XOR swap
        if (gain != 256) {
            l = int16_t((int(l) * gain) / 256);
            r = int16_t((int(r) * gain) / 256);
        }
        g_buf[2 * i + 0] = l;
        g_buf[2 * i + 1] = r;
        sumsq += uint64_t(int64_t(l) * l) + uint64_t(int64_t(r) * r);
        peak = std::max<uint64_t>(peak, uint64_t(std::abs(int(l))));
        peak = std::max<uint64_t>(peak, uint64_t(std::abs(int(r))));
    }
    stats.audio_frames_queued += frames;
    if (peak > 64) stats.audio_nonsilent_buffers++;
    uint64_t m = stats.audio_peak.load();
    if (peak > m) stats.audio_peak.store(peak);
    stats.audio_sumsq += sumsq;
    stats.audio_samples += frames * 2;
    if (peak > stats.audio_peak_ctl.load()) stats.audio_peak_ctl.store(peak);
    auto now = std::chrono::steady_clock::now();
    double before;
#ifdef __EMSCRIPTEN__
    if (g_dev && g_web_stream) {
        before = double(web_frames_remaining());
        SDL_AudioStreamPut(g_web_stream, g_buf.data(), int(frames * 2 * sizeof(int16_t)));
        float converted[4096];
        int got;
        while ((got = SDL_AudioStreamGet(g_web_stream, converted, sizeof(converted))) > 0) {
            n64recomp_browser::audio::queue(converted, size_t(got) / (sizeof(float) * 2));
        }
    } else
#endif
    if (g_dev) {
        std::lock_guard rl{g_ring_mutex};
        before = double(g_ring_fill);
        size_t n = std::min(frames, kRingFrames - g_ring_fill);   // full ring: drop the excess (never seen; FIFO pacing)
        for (size_t i = 0; i < n; i++) {
            size_t k = (g_ring_read + g_ring_fill + i) % kRingFrames;
            g_ring[2 * k] = samples[2 * i + 1];       // full scale: the callback applies the volume after the declick,
            g_ring[2 * k + 1] = samples[2 * i + 0];   // so edge_step is measured the same at any volume
        }
        g_ring_fill += n;
    } else {
        virtual_drain();
        before = g_virtual_frames;
        g_virtual_frames += double(frames);
    }
    if (g_alog) {
        static auto t0 = now;
        std::fprintf(g_alog, "%.3f,%.0f,%zu,%llu,%d,%d,%llu,%llu\n", std::chrono::duration<double>(now - t0).count() * 1000.0, before, frames,
                     (unsigned long long)peak, frames ? g_buf[0] : 0, frames ? g_buf[2 * frames - 2] : 0,
                     (unsigned long long)stats.audio_underruns.load(), (unsigned long long)stats.audio_starved_frames.load());
    }
    g_have_last = true;
    if (g_wav) {
        if (g_wav_freq == 0) g_wav_freq = g_freq ? g_freq : 22050;
        std::fwrite(g_buf.data(), 4, frames, g_wav);
        g_wav_frames += frames;
    }
}

bool thps::audio_device_open() {
    std::lock_guard lock{g_mutex};
    return g_dev != 0;
}

size_t thps::audio_frames_remaining() {
    std::lock_guard lock{g_mutex};
    size_t r;
#ifdef __EMSCRIPTEN__
    if (g_dev && g_web_stream) {
        r = web_frames_remaining();
    } else
#endif
    if (g_dev) {
        std::lock_guard rl{g_ring_mutex};
        r = g_ring_fill;
    } else {
        virtual_drain();
        r = size_t(g_virtual_frames);
    }
    stats.audio_frames_remaining_last = r;
    return r;
}

void thps::audio_shutdown() {
    std::lock_guard lock{g_mutex};
    if (g_alog) {
        std::fclose(g_alog);
        g_alog = nullptr;
    }
    if (g_wav) {
        wav_header(g_wav, g_wav_freq ? g_wav_freq : 22050, g_wav_frames);
        std::fclose(g_wav);
        g_wav = nullptr;
        std::printf("[thps] audio: wrote %llu frames (%.1f s at %u Hz) to %s\n", (unsigned long long)g_wav_frames,
                    g_wav_freq ? double(g_wav_frames) / g_wav_freq : 0.0, g_wav_freq, options.wav_path.string().c_str());
        std::fflush(stdout);
    }
}

// libultra osAiGetStatus: bit 31 = DMA FIFO full (two buffers queued). Report "full" while
// the host queue holds more than --audio-fifo frames (default 800 = 36 ms at 22050 Hz),
// so the game produces audio at the playback rate and the queue stays ~36-70 ms deep.
extern "C" void thps_ai_get_status(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    size_t remaining = thps::audio_frames_remaining();
    bool full = remaining > size_t(thps::options.audio_fifo_frames);
    if (full) thps::stats.ai_fifo_full++;
    ctx->r2 = full ? (gpr)(int32_t)0x80000000 : 0;
}
