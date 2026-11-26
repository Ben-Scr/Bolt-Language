// Sample the main thread of a newly launched x64 benchmark. The child inherits
// stdout so the caller can check its normal output independently of the samples.
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <mmsystem.h>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) return 2;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\"";
    if (!CreateProcessW(argv[1], command.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        nullptr, &startup, &child)) return 3;

    struct Module { std::string name; std::uintptr_t base; };
    std::map<std::uintptr_t, Module> pages;
    std::map<std::pair<std::string, std::uintptr_t>, unsigned> samples;
    // Optional return PCs distinguish callers even when the CRT aliases
    // memcpy and memmove. Unwind the suspended thread using its PE unwind data.
    std::map<std::vector<std::uintptr_t>, unsigned> stacks;
    MODULEINFO image{};
    bool symbols = false;
    const auto timer = timeBeginPeriod(1);
    while (WaitForSingleObject(child.hProcess, 1) == WAIT_TIMEOUT) {
        if (SuspendThread(child.hThread) == DWORD(-1)) break;
        CONTEXT context{};
        context.ContextFlags = argc == 4 ? CONTEXT_FULL : CONTEXT_CONTROL;
        const bool valid = GetThreadContext(child.hThread, &context) != FALSE;
        std::vector<std::uintptr_t> trace;
        if (valid && argc == 4) {
            if (!symbols) {
                HMODULE module{};
                DWORD needed{};
                if (K32EnumProcessModules(child.hProcess, &module, sizeof(module), &needed) && module &&
                    K32GetModuleInformation(child.hProcess, module, &image, sizeof(image))) {
                    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
                    symbols = SymInitialize(child.hProcess, ".", TRUE) != FALSE;
                }
            }
            if (symbols) {
                auto copy = context;
                STACKFRAME64 frame{};
                frame.AddrPC = {context.Rip, 0, AddrModeFlat};
                frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
                frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
                const auto base = reinterpret_cast<std::uintptr_t>(image.lpBaseOfDll);
                const auto image_rva = [&](DWORD64 pc) { return pc-base < image.SizeOfImage ? pc-base : 0; };
                trace.push_back(image_rva(context.Rip));
                for (unsigned depth=0; depth<12 && StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                     child.hProcess, child.hThread, &frame, &copy, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++depth) {
                    if (!frame.AddrReturn.Offset) break;
                    trace.push_back(image_rva(frame.AddrReturn.Offset));
                }
            }
        }
        ResumeThread(child.hThread);
        if (!valid) continue;
        const auto pc = static_cast<std::uintptr_t>(context.Rip);
        const auto page = pc >> 12;
        auto cached = pages.find(page);
        if (cached == pages.end()) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQueryEx(child.hProcess, reinterpret_cast<void*>(pc), &region, sizeof(region))) continue;
            wchar_t path[MAX_PATH]{};
            if (!K32GetModuleFileNameExW(child.hProcess, static_cast<HMODULE>(region.AllocationBase),
                                         path, MAX_PATH)) continue;
            Module module{std::filesystem::path(path).filename().string(),
                          reinterpret_cast<std::uintptr_t>(region.AllocationBase)};
            cached = pages.emplace(page, module).first;
        }
        ++samples[{cached->second.name, pc - cached->second.base}];
        if (!trace.empty()) ++stacks[trace];
    }
    if (timer == TIMERR_NOERROR) timeEndPeriod(1);
    if (symbols) SymCleanup(child.hProcess);
    WaitForSingleObject(child.hProcess, INFINITE);
    DWORD status{};
    GetExitCodeProcess(child.hProcess, &status);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    std::ofstream out{std::filesystem::path(argv[2])};
    for (const auto& [location, count] : samples)
        out << location.first << '\t' << location.second << '\t' << count << '\n';
    if (argc == 4) {
        std::ofstream frames{std::filesystem::path(argv[3])};
        for (const auto& [trace, count] : stacks) {
            frames << count;
            for (auto rva : trace) frames << '\t' << rva;
            frames << '\n';
        }
        if (!frames || stacks.empty()) return 5;
    }
    return out ? static_cast<int>(status) : 4;
}
