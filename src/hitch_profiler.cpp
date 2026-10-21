// Hitch profiler (test aid, docs/hitch_profiler.md).
//
// RUSH2_HITCH_LOG=<file> starts a sampler thread that, about once a millisecond, suspends each thread of the process
// that ran since the last tick (and always the game, graphics and present threads, so a blocked one shows what it waits on),
// walks its stack and resumes it. The log also gets frame markers: the game's display list going to RT64 (ultramodern's
// gfx thread) and RT64's swap chain present. tools/hitch_profile.py finds the long frames and names the code each thread
// ran during them, from the stacks and the PDB.
//
// The stack walk runs while the target is suspended, so it must not take any lock the target could hold: no heap, no
// loader lock, no RtlLookupFunctionEntry (it shares a lock with module loading). Unwind tables are read straight from
// each module's exception directory instead, and modules are listed only while nothing is suspended. Reads stay inside
// the thread's stack (its TEB's bounds), so a walk never raises an exception: dispatching one looks up function tables
// and calls vectored handlers, which take locks the suspended thread may hold. In case the sampler still gets stuck
// with a thread suspended, the scanner thread resumes any thread held more than 100 ms (a run of 2026-10-21 stopped
// logging at 57 s that way, with a background thread left suspended).
//
// Log lines (times in microseconds since the profiler started):
//   F <qpc frequency>                     header
//   M <base hex> <size hex> <path>        a loaded module
//   T <tid> <name>                        a thread's name (repeated when it changes)
//   S <id> <pc hex> ...                   a distinct stack, innermost first
//   s <us> <tid> <stack id> <cycles>      a sample; cycles = CPU cycles the thread used since its previous tick (0 = waiting)
//   E <us> <tag>                          a marker: D/d display list begin/end, P/p present begin/end, or a named mark

#include "hitch_profiler.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace RT64 { extern void (*presentProfileMark)(int begin); }
namespace ultramodern { extern void (*displaylist_profile_mark)(int begin); }

namespace {
    constexpr int max_frames = 96;

    struct Module {
        uintptr_t base;
        uintptr_t end;
        const RUNTIME_FUNCTION* table;
        DWORD count;
    };

    struct ThreadInfo {
        DWORD tid;
        HANDLE handle;
        ULONG64 cycles;
        bool watched;
        bool alive;
        std::wstring name;
        uintptr_t teb = 0;      // its TEB: NT_TIB StackBase at +8, StackLimit at +16
    };

    // The thread the sampler has suspended now, for the scanner's watchdog.
    std::mutex watch_mutex;
    HANDLE watch_handle = nullptr;
    int64_t watch_since = 0;

    FILE* out = nullptr;
    LARGE_INTEGER qpc_start;
    double us_per_tick;
    std::vector<Module> modules; // Sorted by base. Only the sampler thread touches it.
    bool modules_stale = true;

    std::mutex marks_mutex;
    std::vector<std::pair<int64_t, std::string>> pending_marks;

    int64_t now_us() {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return int64_t((t.QuadPart - qpc_start.QuadPart) * us_per_tick);
    }

    void push_mark(const char* tag) {
        int64_t t = now_us();
        std::lock_guard lock{ marks_mutex };
        pending_marks.emplace_back(t, tag);
    }

    void refresh_modules() {
        HMODULE handles[1024];
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) {
            return;
        }
        size_t n = std::min<size_t>(needed / sizeof(HMODULE), 1024);
        // Modules unloaded since are dropped: their unwind tables are gone.
        modules.erase(std::remove_if(modules.begin(), modules.end(), [&](const Module& m) {
            return std::none_of(handles, handles + n, [&](HMODULE h) { return reinterpret_cast<uintptr_t>(h) == m.base; });
        }), modules.end());
        for (size_t i = 0; i < n; i++) {
            uintptr_t base = reinterpret_cast<uintptr_t>(handles[i]);
            bool known = std::any_of(modules.begin(), modules.end(), [&](const Module& m) { return m.base == base; });
            if (known) {
                continue;
            }
            auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
            Module m{ base, base + nt->OptionalHeader.SizeOfImage,
                reinterpret_cast<const RUNTIME_FUNCTION*>(base + dir.VirtualAddress), DWORD(dir.Size / sizeof(RUNTIME_FUNCTION)) };
            if (dir.VirtualAddress == 0) {
                m.count = 0;
            }
            modules.push_back(m);
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(handles[i], path, MAX_PATH);
            fprintf(out, "M %llx %llx %ls\n", (unsigned long long)m.base, (unsigned long long)(m.end - m.base), path);
        }
        std::sort(modules.begin(), modules.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
        modules_stale = false;
    }

    const Module* find_module(uintptr_t pc) {
        auto it = std::upper_bound(modules.begin(), modules.end(), pc, [](uintptr_t p, const Module& m) { return p < m.base; });
        if (it == modules.begin()) {
            return nullptr;
        }
        --it;
        return pc < it->end ? &*it : nullptr;
    }

    const RUNTIME_FUNCTION* find_function(const Module* m, uintptr_t pc) {
        DWORD rva = DWORD(pc - m->base);
        size_t lo = 0, hi = m->count;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            const RUNTIME_FUNCTION& f = m->table[mid];
            if (rva < f.BeginAddress) {
                hi = mid;
            }
            else if (rva >= f.EndAddress) {
                lo = mid + 1;
            }
            else {
                return &f;
            }
        }
        return nullptr;
    }

    // Walks the stack of a suspended thread. Lock free and allocation free; a bad read ends the walk.
    int walk(CONTEXT ctx, uintptr_t* frames, uintptr_t stack_low, uintptr_t stack_high) {
        int n = 0;
        __try {
            while (n < max_frames && ctx.Rip != 0) {
                frames[n++] = ctx.Rip;
                if (ctx.Rsp < stack_low || ctx.Rsp + 8 > stack_high) {
                    break;
                }
                const Module* m = find_module(ctx.Rip);
                if (m == nullptr) {
                    modules_stale = true;
                    break;
                }
                const RUNTIME_FUNCTION* f = find_function(m, ctx.Rip);
                DWORD64 prev_sp = ctx.Rsp;
                if (f == nullptr) {
                    // A leaf function has no unwind data: the return address is on top of the stack.
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                }
                else {
                    void* handler_data;
                    DWORD64 frame;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, ctx.Rip, const_cast<RUNTIME_FUNCTION*>(f), &ctx,
                        &handler_data, &frame, nullptr);
                }
                if (ctx.Rsp <= prev_sp) {
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return n;
    }

    // The process's threads and their names, listed by a thread of its own: a snapshot of every thread on the system
    // takes tens of milliseconds, in which the sampler would see nothing.
    std::atomic<int> stuck_resumes = 0;    // threads the watchdog resumed (logged as marks "watchdog")
    std::mutex scan_mutex;
    std::vector<std::pair<DWORD, std::wstring>> scan;   // the latest list, under scan_mutex
    bool scan_new = false;

    void scanner(DWORD sampler_tid) {
        SetThreadDescription(GetCurrentThread(), L"Hitch Profiler Scan");
        DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
        while (true) {
            std::vector<std::pair<DWORD, std::wstring>> list;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te{ sizeof(te) };
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != pid || te.th32ThreadID == sampler_tid || te.th32ThreadID == self) {
                        continue;
                    }
                    std::wstring name;
                    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                    if (h != nullptr) {
                        PWSTR desc = nullptr;
                        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc != nullptr) {
                            name = desc;
                            LocalFree(desc);
                        }
                        CloseHandle(h);
                    }
                    list.emplace_back(te.th32ThreadID, std::move(name));
                }
                CloseHandle(snap);
                std::lock_guard lock{ scan_mutex };
                scan.swap(list);
                scan_new = true;
            }
            // The watchdog, every 20 ms between snapshots.
            for (int i = 0; i < 10; i++) {
                Sleep(20);
                std::lock_guard lock{ watch_mutex };
                if (watch_handle != nullptr && now_us() - watch_since > 100'000) {
                    ResumeThread(watch_handle);
                    watch_handle = nullptr;
                    stuck_resumes++;
                }
            }
        }
    }

    uintptr_t thread_teb(HANDLE h) {
        struct BasicInformation {
            LONG exit_status;
            PVOID teb;
            PVOID client_id[2];
            ULONG_PTR affinity;
            LONG priority, base_priority;
        } info{};
        using Query = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
        static Query query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        if (query == nullptr || query(h, 0 /* ThreadBasicInformation */, &info, sizeof(info), nullptr) < 0) {
            return 0;
        }
        return reinterpret_cast<uintptr_t>(info.teb);
    }

    // Takes the scanner's latest list, if there is a new one: opens the new threads, renames, drops the gone ones.
    void refresh_threads(std::vector<ThreadInfo>& threads) {
        std::vector<std::pair<DWORD, std::wstring>> list;
        {
            std::lock_guard lock{ scan_mutex };
            if (!scan_new) {
                return;
            }
            list.swap(scan);
            scan_new = false;
        }
        for (auto& t : threads) {
            t.alive = false;
        }
        for (auto& [tid, name] : list) {
            auto it = std::find_if(threads.begin(), threads.end(), [&](const ThreadInfo& t) { return t.tid == tid; });
            if (it == threads.end()) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
                if (h == nullptr) {
                    continue;
                }
                threads.push_back({ tid, h, 0, false, true, L"" });
                threads.back().teb = thread_teb(h);
                QueryThreadCycleTime(h, &threads.back().cycles);
                it = threads.end() - 1;
            }
            it->alive = true;
            if (!name.empty() && it->name != name) {
                it->name = name;
                it->watched = it->name.starts_with(L"[Game]") || it->name == L"Gfx Thread" || it->name == L"VI Thread" ||
                    it->name == L"SP Task Thread" || it->name == L"RT64 Present" || it->name == L"RT64 Workload";
                fprintf(out, "T %lu %ls\n", it->tid, name.c_str());
            }
        }
        for (auto& t : threads) {
            if (!t.alive && t.handle != nullptr) {
                CloseHandle(t.handle);
                t.handle = nullptr;
            }
        }
        threads.erase(std::remove_if(threads.begin(), threads.end(), [](const ThreadInfo& t) { return t.handle == nullptr; }), threads.end());
    }

    void sampler() {
        SetThreadDescription(GetCurrentThread(), L"Hitch Profiler");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        DWORD self = GetCurrentThreadId();
        std::thread{ scanner, self }.detach();
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        std::vector<ThreadInfo> threads;
        std::unordered_map<uint64_t, uint32_t> stack_ids;
        uintptr_t frames[max_frames];
        int64_t last_refresh = -1'000'000, last_flush = 0;
        std::vector<std::pair<int64_t, std::string>> marks;

        while (true) {
            int64_t t = now_us();
            // Modules are listed when a stack ran into one not listed yet, and every few seconds.
            if (t - last_refresh > 5'000'000 || modules_stale) {
                refresh_modules();
                last_refresh = t;
            }
            refresh_threads(threads);
            {
                std::lock_guard lock{ marks_mutex };
                marks.swap(pending_marks);
            }
            for (static int logged = 0; logged < stuck_resumes; logged++) {
                marks.emplace_back(t, "watchdog");
            }
            for (auto& [mt, tag] : marks) {
                fprintf(out, "E %lld %s\n", (long long)mt, tag.c_str());
            }
            marks.clear();

            for (auto& th : threads) {
                ULONG64 cycles = 0;
                QueryThreadCycleTime(th.handle, &cycles);
                ULONG64 delta = cycles - th.cycles;
                th.cycles = cycles;
                if (delta == 0 && !th.watched) {
                    continue;
                }
                if (th.teb == 0) {
                    continue;
                }
                {
                    std::lock_guard lock{ watch_mutex };
                    watch_handle = th.handle;
                    watch_since = now_us();
                }
                if (SuspendThread(th.handle) == (DWORD)-1) {
                    std::lock_guard lock{ watch_mutex };
                    watch_handle = nullptr;
                    continue;
                }
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                int n = 0;
                int64_t ts = now_us();
                if (GetThreadContext(th.handle, &ctx)) {
                    // The TEB is the thread's own memory and stays while it lives (its handle keeps it).
                    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(th.teb);
                    n = walk(ctx, frames, reinterpret_cast<uintptr_t>(tib->StackLimit), reinterpret_cast<uintptr_t>(tib->StackBase));
                }
                {
                    std::lock_guard lock{ watch_mutex };
                    if (watch_handle == th.handle) {
                        ResumeThread(th.handle);
                    }
                    watch_handle = nullptr;
                }
                if (n == 0) {
                    continue;
                }
                uint64_t hash = 1469598103934665603ull;
                for (int i = 0; i < n; i++) {
                    hash = (hash ^ frames[i]) * 1099511628211ull;
                }
                auto [it, added] = stack_ids.try_emplace(hash, uint32_t(stack_ids.size()));
                if (added) {
                    fprintf(out, "S %u", it->second);
                    for (int i = 0; i < n; i++) {
                        fprintf(out, " %llx", (unsigned long long)frames[i]);
                    }
                    fputc('\n', out);
                }
                fprintf(out, "s %lld %lu %u %llu\n", (long long)ts, th.tid, it->second, (unsigned long long)delta);
            }

            if (t - last_flush > 250'000) {
                fflush(out);
                last_flush = t;
            }
            // Next tick 1 ms after this one started (relative, in 100 ns units), so the walk's own time doesn't stretch it.
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(1000 - (now_us() - t), 100) * 10;
            if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
            }
            else {
                Sleep(1);
            }
        }
    }
}

void rush2::hitch::start() {
    const char* path = std::getenv("RUSH2_HITCH_LOG");
    if (path == nullptr || *path == '\0' || out != nullptr) {
        return;
    }
    out = fopen(path, "w");
    if (out == nullptr) {
        fprintf(stderr, "Hitch profiler: can't open %s\n", path);
        return;
    }
    setvbuf(out, nullptr, _IOFBF, 1 << 20);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&qpc_start);
    us_per_tick = 1e6 / double(freq.QuadPart);
    fprintf(out, "F %lld\n", (long long)freq.QuadPart);

    RT64::presentProfileMark = [](int begin) { push_mark(begin ? "P" : "p"); };
    ultramodern::displaylist_profile_mark = [](int begin) { push_mark(begin ? "D" : "d"); };
    std::thread{ sampler }.detach();
    printf("Hitch profiler: logging to %s\n", path);
}

void rush2::hitch::mark(const char* tag) {
    if (out != nullptr) {
        push_mark(tag);
    }
}

#else
void rush2::hitch::start() {}
void rush2::hitch::mark(const char*) {}
#endif
