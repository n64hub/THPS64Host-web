// Crash log: on a fatal signal (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT; on Windows an unhandled
// exception or abort()) write a report to <config>/crash.log (appended) and to stderr, then exit with
// 128 + signal (Windows: the exception code).
//
// The recompiled game functions are ordinary C functions named after the game's symbols
// (func_8002A588, Front_IntroUpdate, Panel_Display ...), so a host backtrace names the guest
// functions that were running. The report lists every frame with its symbol, marks the
// game ones with their N64 address (from the section table), names the innermost one, and gives the fault address as an N64 address
// when it lies in the game's RDRAM. No game data is written: names and addresses only.
// --control "crash" (control.cpp) makes the game thread write to a null pointer, to test this.
//
// Two implementations behind one interface (host.h):
//   POSIX (macOS, Linux): sigaction + backtrace() + dladdr(). Linux builds link with -rdynamic so dladdr
//     can name the (non-static) recompiled functions.
//   Windows: SetUnhandledExceptionFilter + signal(SIGABRT), frames unwound with RtlLookupFunctionEntry /
//     RtlVirtualUnwind (the .pdata of the exe, no PDB and no DbgHelp needed). The function start the unwinder
//     reports is looked up in the same guest table; host frames print module+offset.
// A web page (Emscripten) installs neither: it has no signals, and a fault is a wasm trap the page reports in its
// console with the wasm stack, the recompiled functions named.
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#ifndef __EMSCRIPTEN__
#include <execinfo.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#endif

#include "host.h"
#include "platform.h"

namespace {
std::atomic<bool> g_in_handler{false};

// Recompiled game functions: host address -> guest VRAM, sorted (from the section table, versions.cpp).
std::vector<std::pair<uintptr_t, uint32_t>> g_guest;
// LiveRecomp mode (game_live.cpp): the JIT code has no symbols and no unwind info, so a frame inside [begin, end) belongs to the
// guest function with the greatest start <= pc (the functions are contiguous in the one code object).
std::pair<uintptr_t, uintptr_t> g_jit_range{0, 0};

// The guest function whose recompiled C function starts at `start` (dladdr's symbol address, or the
// unwinder's function start), or 0 for host code.
uint32_t guest_vram(uintptr_t start) {
    auto it = std::lower_bound(g_guest.begin(), g_guest.end(), std::make_pair(start, uint32_t(0)));
    return it != g_guest.end() && it->first == start ? it->second : 0;
}

// What the report is about, filled by the platform handler.
struct Fault {
    const char* what = "signal";   // "SIGSEGV", "EXCEPTION_ACCESS_VIOLATION", ...
    long code = 0;
    const void* address = nullptr; // faulting data address (access violation) or instruction
};

#ifdef _WIN32
wchar_t g_log_path[1024];
HANDLE g_log = INVALID_HANDLE_VALUE;

void put(const char* s) {
    DWORD n = 0;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, s, DWORD(std::strlen(s)), &n, nullptr);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, s, DWORD(std::strlen(s)), &n, nullptr);
}
void close_log() {
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
}
#else
char g_log_path[1024];
int g_log = -1;

void put(const char* s) {
    if (g_log >= 0) (void)!write(g_log, s, std::strlen(s));
    (void)!write(STDERR_FILENO, s, std::strlen(s));
}
void close_log() {
    if (g_log >= 0) close(g_log);
}
#endif

// Header line, RDRAM address, frame counters. `frames` are printed by the platform code.
void report_header(const Fault& f) {
    char buf[512];
    time_t now = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tmv);
    std::snprintf(buf, sizeof buf, "\n=== THPS recomp crash %s: %s (%ld) at address %p ===\n", when, f.what, f.code, f.address);
    put(buf);
    uint8_t* ram = thps::rdram_base.load();
    if (ram && f.address) {
        uintptr_t a = uintptr_t(f.address), base = uintptr_t(ram);
        if (a >= base && a < base + 0x800000) {
            std::snprintf(buf, sizeof buf, "fault address = N64 address 0x%08X (in RDRAM)\n", uint32_t(a - base + 0x80000000u));
            put(buf);
        }
    }
    std::snprintf(buf, sizeof buf, "game frames displayed %llu, gfx tasks %llu\n", (unsigned long long)thps::stats.vi_origin_changes.load(),
                  (unsigned long long)thps::stats.gfx_tasks.load());
    put(buf);
    put(thps::cheats::crash_note());   // "cheats on (test aids, ...): ..." while any is on (src/cheats.cpp), else nothing
}

struct FrameTally {
    char innermost[64] = {};  // a copy: the Windows walker passes names in a loop-local buffer
    uint32_t innermost_vram = 0;
};

void report_frame(int i, const void* pc, const char* name, uintptr_t off, uint32_t vram, FrameTally& t, const char* module = nullptr) {
    char buf[512];
    if (vram && !t.innermost_vram) {
        std::snprintf(t.innermost, sizeof t.innermost, "%s", name ? name : "?");
        t.innermost_vram = vram;
    }
    if (vram) {
        std::snprintf(buf, sizeof buf, "  #%02d %p [game 0x%08X] %s+0x%llx\n", i, pc, vram, name ? name : "?", (unsigned long long)off);
    } else if (module) {
        std::snprintf(buf, sizeof buf, "  #%02d %p %s+0x%llx\n", i, pc, module, (unsigned long long)off);
    } else {
        std::snprintf(buf, sizeof buf, "  #%02d %p %s+0x%llx\n", i, pc, name ? name : "?", (unsigned long long)off);
    }
    put(buf);
}

void report_footer(const FrameTally& t, const char* path_utf8) {
    char buf[1200];
    if (t.innermost_vram) std::snprintf(buf, sizeof buf, "innermost game function: %s (0x%08X)\n", t.innermost, t.innermost_vram);
    else std::snprintf(buf, sizeof buf, "innermost game function: (none: host code only)\n");
    put(buf);
    std::snprintf(buf, sizeof buf, "(report appended to %s)\n", path_utf8);
    put(buf);
}

char g_log_path_utf8[1200];

#ifdef _WIN32
const char* exception_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
        default: return "exception";
    }
}

#if defined(_M_X64) || defined(__x86_64__)
#define THPS_PC(c) ((c).Rip)
#define THPS_SP(c) ((c).Rsp)
#define THPS_HAVE_UNWIND 1
#elif defined(_M_ARM64) || defined(__aarch64__)
#define THPS_PC(c) ((c).Pc)
#define THPS_SP(c) ((c).Sp)
#define THPS_HAVE_UNWIND 1
#endif

// Walk the stack from `ctx` with the exe's unwind tables. A frame without a table entry is a leaf function
// (x64: the return address is at [rsp]); the walk stops at anything it cannot step.
void walk_stack(CONTEXT ctx, FrameTally& tally) {
#ifdef THPS_HAVE_UNWIND
    for (int i = 0; i < 64; i++) {
        DWORD64 pc = THPS_PC(ctx);
        if (!pc) break;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &image, nullptr);
        uintptr_t start = rf ? uintptr_t(image + rf->BeginAddress) : 0;
        uint32_t vram = start ? guest_vram(start) : 0;
        char module[MAX_PATH] = {};
        const char* mod = nullptr;
        uintptr_t off = start ? uintptr_t(pc - start) : 0;
        if (!vram) {
            HMODULE hm = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(pc), &hm) && hm) {
                GetModuleFileNameA(hm, module, MAX_PATH);
                const char* slash = std::strrchr(module, '\\');
                mod = slash ? slash + 1 : module;
                off = uintptr_t(pc - DWORD64(hm));
            }
        }
        // No symbol names without a PDB: the guest address is the key into the decomp's syms.toml
        // (unnamed functions are literally func_<VRAM>).
        char name[32];
        if (vram) std::snprintf(name, sizeof name, "func_%08X", vram);
        report_frame(i, reinterpret_cast<const void*>(pc), vram ? name : "?", off, vram, tally, mod);
        if (!rf) {
            // Leaf frame.
            ULONG64* sp = reinterpret_cast<ULONG64*>(THPS_SP(ctx));
#if defined(_M_X64) || defined(__x86_64__)
            THPS_PC(ctx) = *sp;
            THPS_SP(ctx) += 8;
#else
            break;
#endif
        } else {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, rf, &ctx, &handler_data, &establisher, nullptr);
        }
        if (THPS_SP(ctx) == 0) break;
    }
#else
    (void)ctx, (void)tally;
    put("  (no stack walk on this architecture)\n");
#endif
}

void windows_report(Fault f, const CONTEXT* ctx) {
    if (g_in_handler.exchange(true)) TerminateProcess(GetCurrentProcess(), 3);
    g_log = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    report_header(f);
    FrameTally tally;
    CONTEXT local;
    if (!ctx) {
        RtlCaptureContext(&local);
        ctx = &local;
    }
    walk_stack(*ctx, tally);
    report_footer(tally, g_log_path_utf8);
    close_log();
}

LONG WINAPI exception_filter(EXCEPTION_POINTERS* ep) {
    Fault f;
    f.what = exception_name(ep->ExceptionRecord->ExceptionCode);
    f.code = long(ep->ExceptionRecord->ExceptionCode);
    f.address = reinterpret_cast<const void*>(ep->ExceptionRecord->ExceptionAddress);
    // Access violations: ExceptionInformation[0] = 0 read / 1 write / 8 DEP, [1] = the data address.
    if ((ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        f.address = reinterpret_cast<const void*>(ep->ExceptionRecord->ExceptionInformation[1]);
    }
    windows_report(f, ep->ContextRecord);
    TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

void abort_handler(int sig) {
    Fault f;
    f.what = "SIGABRT";
    f.code = sig;
    windows_report(f, nullptr);
    TerminateProcess(GetCurrentProcess(), 128 + sig);
}
#elif !defined(__EMSCRIPTEN__)  // POSIX

const char* sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default: return "signal";
    }
}

void handler(int sig, siginfo_t* info, void*) {
    if (g_in_handler.exchange(true)) _exit(128 + sig);
    g_log = open(g_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    Fault f;
    f.what = sig_name(sig);
    f.code = sig;
    f.address = info ? info->si_addr : nullptr;
    report_header(f);
    void* frames[64];
    int n = backtrace(frames, 64);
    FrameTally tally;
    for (int i = 0; i < n; i++) {
        Dl_info di{};
        const char* name = nullptr;
        uintptr_t off = 0;
        if (dladdr(frames[i], &di) && di.dli_sname) {
            name = di.dli_sname;
            off = uintptr_t(frames[i]) - uintptr_t(di.dli_saddr);
        }
        uint32_t vram = name ? guest_vram(uintptr_t(di.dli_saddr)) : 0;
        char jit_name[32];
        if (!name && g_jit_range.second && uintptr_t(frames[i]) >= g_jit_range.first && uintptr_t(frames[i]) < g_jit_range.second) {
            auto it = std::upper_bound(g_guest.begin(), g_guest.end(), std::make_pair(uintptr_t(frames[i]), UINT32_MAX));
            if (it != g_guest.begin()) {
                --it;
                vram = it->second;
                off = uintptr_t(frames[i]) - it->first;
                std::snprintf(jit_name, sizeof jit_name, "func_%08X", vram);
                name = jit_name;
            }
        }
        report_frame(i, frames[i], name, off, vram, tally);
    }
    report_footer(tally, g_log_path);
    close_log();
    _exit(128 + sig);
}
#endif
}  // namespace

void thps::crash_add_guest_function(const void* host, uint32_t vram) { g_guest.emplace_back(uintptr_t(host), vram); }

void thps::crash_set_jit_range(const void* begin, const void* end) {
    std::sort(g_guest.begin(), g_guest.end());
    g_jit_range = {uintptr_t(begin), uintptr_t(end)};
}

void thps::crash_install(const std::filesystem::path& config_dir) {
    std::sort(g_guest.begin(), g_guest.end());
    const std::filesystem::path log = config_dir / "crash.log";
#ifdef _WIN32
    std::snprintf(g_log_path_utf8, sizeof g_log_path_utf8, "%s", thps::platform::to_utf8(log).c_str());
    std::wcsncpy(g_log_path, log.c_str(), sizeof g_log_path / sizeof g_log_path[0] - 1);
    SetUnhandledExceptionFilter(exception_filter);
    // No Windows Error Reporting dialog on a crash: our report is the record.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::signal(SIGABRT, abort_handler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#elif defined(__EMSCRIPTEN__)
    (void)log;
#else
    std::snprintf(g_log_path, sizeof g_log_path, "%s", log.string().c_str());
    struct sigaction sa{};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
#endif
}
