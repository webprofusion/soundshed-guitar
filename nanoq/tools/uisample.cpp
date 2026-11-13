// A sampling profiler for one thread of a running process, for when ETW kernel sampling isn't
// available (it needs an elevated prompt; this doesn't). By default it takes the thread that owns
// the process's ElementsView window, Nano Q's UI thread.
//
//   uisample --pid <pid> --seconds <s> --out <file> [--tid <tid>] [--symbols <path>]
//
// About once a millisecond it looks at the thread's cycle counter. If the thread ran for more than
// kMinCycles since the last look, it suspends it, walks its stack, resumes it and charges those
// cycles to that stack, so a thread that spends most of its time waiting costs nothing and isn't
// sampled there. The last slice of a burst lands on the wait the thread went into; those samples
// are easy to spot (the leaf is a wait call). The floor matters: suspending a waiting thread makes
// it run for about 40 us, so without it each sample would cause the next. Slices under it go
// uncharged and the header's skippedPct says how much that was.
//
// The cycle counter is exact, unlike GetThreadTimes (and so Task Manager and Process.Threads),
// which charges a whole clock tick to whoever is running when the tick lands. The header gives both
// for the sampled window.
//
// Output, after "#" header lines: "<samples>\t<cycles>\t<root;...;leaf>", heaviest first.
//
// Build from a vcvars prompt:
//   cl /nologo /O2 /EHsc /std:c++20 uisample.cpp dbghelp.lib winmm.lib user32.lib
// Put dbghelp.dll and symsrv.dll from the Windows Debuggers beside it to get Microsoft symbols.
#include <windows.h>

#include <dbghelp.h>
#include <intrin.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
struct FoundWindow
{
    DWORD pid = 0;
    DWORD tid = 0;
};

BOOL CALLBACK CheckChild(HWND window, LPARAM param)
{
    auto* found = reinterpret_cast<FoundWindow*>(param);
    char name[256]{};
    GetClassNameA(window, name, sizeof(name));

    if (std::strstr(name, "ElementsView") != nullptr)
    {
        found->tid = GetWindowThreadProcessId(window, nullptr);
        return FALSE;
    }

    return TRUE;
}

BOOL CALLBACK CheckTop(HWND window, LPARAM param)
{
    auto* found = reinterpret_cast<FoundWindow*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);

    if (pid == found->pid)
    {
        EnumChildWindows(window, CheckChild, param);
    }

    return found->tid == 0;
}

double TscPerSecond()
{
    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&a);
    const unsigned __int64 t0 = __rdtsc();
    Sleep(100);
    QueryPerformanceCounter(&b);
    const unsigned __int64 t1 = __rdtsc();
    return static_cast<double>(t1 - t0) * freq.QuadPart / (b.QuadPart - a.QuadPart);
}

unsigned __int64 Ticks100ns(const FILETIME& t)
{
    return (static_cast<unsigned __int64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

constexpr unsigned __int64 kMinCycles = 300000;

struct Weight
{
    unsigned __int64 samples = 0;
    unsigned __int64 cycles = 0;
};

std::string Name(HANDLE process, DWORD64 address, std::unordered_map<DWORD64, std::string>& cache)
{
    if (const auto it = cache.find(address); it != cache.end())
    {
        return it->second;
    }

    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    std::string text = SymGetModuleInfo64(process, address, &module) ? module.ModuleName : "?";

    char buffer[sizeof(SYMBOL_INFO) + 1024]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 1000;
    DWORD64 displacement = 0;

    if (SymFromAddr(process, address, &displacement, symbol))
    {
        text += "!";
        text += symbol->Name;
    }
    else
    {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "+0x%llx", address - module.BaseOfImage);
        text += hex;
    }

    for (char& c : text)
    {
        if (c == ';' || c == '\t' || c == '\n')
        {
            c = ' ';
        }
    }

    cache.emplace(address, text);
    return text;
}
} // namespace

int main(int argc, char** argv)
{
    DWORD pid = 0, tid = 0;
    double seconds = 10.0;
    std::string out, symbols;

    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string key = argv[i];

        if (key == "--pid")
        {
            pid = std::strtoul(argv[i + 1], nullptr, 10);
        }
        else if (key == "--tid")
        {
            tid = std::strtoul(argv[i + 1], nullptr, 10);
        }
        else if (key == "--seconds")
        {
            seconds = std::atof(argv[i + 1]);
        }
        else if (key == "--out")
        {
            out = argv[i + 1];
        }
        else if (key == "--symbols")
        {
            symbols = argv[i + 1];
        }
    }

    if (pid == 0 || out.empty())
    {
        std::fprintf(stderr, "uisample --pid <pid> --seconds <s> --out <file> [--tid <tid>] [--symbols <path>]\n");
        return 2;
    }

    if (tid == 0)
    {
        FoundWindow found;
        found.pid = pid;
        EnumWindows(CheckTop, reinterpret_cast<LPARAM>(&found));
        tid = found.tid;
    }

    if (tid == 0)
    {
        std::fprintf(stderr, "no ElementsView window in pid %lu\n", pid);
        return 1;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);

    if (process == nullptr || thread == nullptr)
    {
        std::fprintf(stderr, "can't open the process or thread: %lu\n", GetLastError());
        return 1;
    }

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);

    // No symbols while sampling: the walk needs only each module's unwind tables, and fetching a
    // PDB would leave the thread suspended for as long as that takes. Names come at the end.
    if (!SymInitialize(process, "\\.\nosymbols", TRUE))
    {
        std::fprintf(stderr, "SymInitialize failed: %lu\n", GetLastError());
        return 1;
    }

    const double tscHz = TscPerSecond();
    timeBeginPeriod(1);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    std::map<std::vector<DWORD64>, Weight> stacks;
    unsigned __int64 last = 0, first = 0, tries = 0, skipped = 0, charged = 0, charges = 0;
    QueryThreadCycleTime(thread, &first);
    last = first;

    FILETIME created, exited, kernel0, user0, kernel1, user1;
    GetThreadTimes(thread, &created, &exited, &kernel0, &user0);

    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    double elapsed = 0.0;
    double walkSeconds = 0.0;

    alignas(16) CONTEXT context;
    std::vector<DWORD64> frames;
    frames.reserve(128);

    while (elapsed < seconds)
    {
        Sleep(1);
        ++tries;
        unsigned __int64 cycles = 0;
        QueryThreadCycleTime(thread, &cycles);

        if (cycles - last < kMinCycles)
        {
            skipped += cycles - last;
            last = cycles;
        }
        else
        {
            LARGE_INTEGER walkStart, walkEnd;
            QueryPerformanceCounter(&walkStart);

            if (SuspendThread(thread) != static_cast<DWORD>(-1))
            {
                std::memset(&context, 0, sizeof(context));
                context.ContextFlags = CONTEXT_FULL;

                if (GetThreadContext(thread, &context))
                {
                    frames.clear();
                    STACKFRAME64 frame{};
                    frame.AddrPC.Offset = context.Rip;
                    frame.AddrPC.Mode = AddrModeFlat;
                    frame.AddrFrame.Offset = context.Rbp;
                    frame.AddrFrame.Mode = AddrModeFlat;
                    frame.AddrStack.Offset = context.Rsp;
                    frame.AddrStack.Mode = AddrModeFlat;

                    while (frames.size() < 96 &&
                           StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                                       SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
                           frame.AddrPC.Offset != 0)
                    {
                        frames.push_back(frame.AddrPC.Offset);
                    }

                    QueryThreadCycleTime(thread, &cycles);
                    Weight& weight = stacks[frames];
                    ++weight.samples;
                    weight.cycles += cycles - last;
                    charged += cycles - last;
                    ++charges;
                    last = cycles;
                }

                ResumeThread(thread);
            }

            QueryPerformanceCounter(&walkEnd);
            walkSeconds += static_cast<double>(walkEnd.QuadPart - walkStart.QuadPart) / freq.QuadPart;
        }

        QueryPerformanceCounter(&now);
        elapsed = static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart;
    }

    unsigned __int64 cycles = 0;
    QueryThreadCycleTime(thread, &cycles);
    GetThreadTimes(thread, &created, &exited, &kernel1, &user1);
    timeEndPeriod(1);

    const double busySeconds = static_cast<double>(cycles - first) / tscHz;
    const double kernelSeconds = (Ticks100ns(kernel1) - Ticks100ns(kernel0)) * 1e-7;
    const double userSeconds = (Ticks100ns(user1) - Ticks100ns(user0)) * 1e-7;

    std::FILE* file = std::fopen(out.c_str(), "wb");

    if (file == nullptr)
    {
        std::fprintf(stderr, "can't write %s\n", out.c_str());
        return 1;
    }

    std::fprintf(file, "# seconds %.3f tries %llu tscGHz %.3f\n", elapsed, tries, tscHz / 1e9);
    std::fprintf(file, "# cyclePct %.3f tickUserPct %.3f tickKernelPct %.3f walkPct %.3f\n",
                 100.0 * busySeconds / elapsed, 100.0 * userSeconds / elapsed, 100.0 * kernelSeconds / elapsed,
                 100.0 * walkSeconds / elapsed);

    SymCleanup(process);
    SymInitialize(process, symbols.empty() ? nullptr : symbols.c_str(), TRUE);

    std::vector<std::pair<std::string, Weight>> lines;
    std::unordered_map<DWORD64, std::string> names;

    for (const auto& [stack, weight] : stacks)
    {
        std::string text;

        for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        {
            if (!text.empty())
            {
                text += ';';
            }

            text += Name(process, *it, names);
        }

        lines.emplace_back(std::move(text), weight);
    }

    std::sort(lines.begin(), lines.end(),
              [](const auto& a, const auto& b) { return a.second.cycles > b.second.cycles; });

    for (const auto& [text, weight] : lines)
    {
        std::fprintf(file, "%llu\t%llu\t%s\n", weight.samples, weight.cycles, text.c_str());
    }

    std::fclose(file);
    std::printf("cyclePct %.2f tickPct %.2f stacks %zu\n", 100.0 * busySeconds / elapsed,
                100.0 * (userSeconds + kernelSeconds) / elapsed, lines.size());
    return 0;
}
