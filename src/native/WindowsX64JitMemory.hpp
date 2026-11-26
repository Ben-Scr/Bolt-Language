// Keep the image-relative COFF unwind relocations and REL32 references in range.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace bolt::native_backend {
class WindowsX64JitMemory {
    struct Section {
        std::uint8_t* start;
        std::size_t length;
        DWORD protection;
        std::size_t pdata_bytes;
    };
    std::uint8_t* base = nullptr;
    std::uint8_t* image_start = nullptr;
    std::size_t reserved = std::size_t{1} << 31;
    std::size_t used = 0;
    std::size_t page = 0;
    std::vector<Section> pending;
    std::vector<std::vector<RUNTIME_FUNCTION>> unwind_tables;
    char* (*message)(const char*);

    std::uint8_t* allocate(std::uintptr_t requested, unsigned alignment, DWORD protection, bool pdata = false) noexcept {
        // Different permissions never share a page. Alignment is relative to the actual address.
        const auto align = std::max<std::size_t>(page, alignment);
        if ((align & (align - 1)) != 0 || requested > reserved) return nullptr;
        const auto address = reinterpret_cast<std::uintptr_t>(base) + used;
        const std::size_t padding = (-address) & (align - 1);
        const auto length = (std::max<std::size_t>(requested, 1) + page - 1) & ~(page - 1);
        if (padding > reserved - used || length > reserved - used - padding) return nullptr;
        auto* start = base + used + padding;
        if (!VirtualAlloc(start, length, MEM_COMMIT, PAGE_READWRITE)) return nullptr;
        try {
            pending.push_back({start, length, protection, pdata ? requested : 0});
        } catch (...) {
            VirtualFree(start, length, MEM_DECOMMIT);
            return nullptr;
        }
        if (!image_start) image_start = start;
        used += padding + length;
        return start;
    }

    const char* register_unwind() {
        static_assert(sizeof(RUNTIME_FUNCTION) == 12);
        std::vector<RUNTIME_FUNCTION> entries;
        const auto extent = image_start ? static_cast<std::size_t>(base + used - image_start) : 0;
        for (const auto& section : pending) {
            // RuntimeDyld may reserve a zero-filled stub tail in addition to COFF's
            // actual .pdata. These records contain only local code and unwind RVAs.
            const auto* at = section.start;
            const auto* end = at + section.pdata_bytes;
            while (end - at >= static_cast<std::ptrdiff_t>(sizeof(RUNTIME_FUNCTION))) {
                RUNTIME_FUNCTION fn;
                std::memcpy(&fn, at, sizeof(fn));
                if (!fn.BeginAddress && !fn.EndAddress && !fn.UnwindData) break;
                if (fn.BeginAddress >= fn.EndAddress || fn.EndAddress > extent || fn.UnwindData >= extent)
                    return "Invalid Windows x64 JIT unwind entry";
                entries.push_back(fn);
                at += sizeof(fn);
            }
            if (std::any_of(at, end, [](auto byte) { return byte != 0; }))
                return "Unexpected suffix in the Windows x64 JIT unwind table";
        }
        if (entries.empty()) return nullptr;
        std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.BeginAddress < rhs.BeginAddress;
        });
        for (std::size_t i = 1; i < entries.size(); ++i)
            if (entries[i].BeginAddress < entries[i - 1].EndAddress)
                return "Overlapping Windows x64 JIT unwind entries";
        // The OS borrows the table storage until RtlDeleteFunctionTable. Moving
        // an outer vector element leaves the inner vector's allocation intact.
        auto& table = unwind_tables.emplace_back(std::move(entries));
        if (!RtlAddFunctionTable(table.data(), static_cast<DWORD>(table.size()),
                                 reinterpret_cast<DWORD64>(image_start))) {
            unwind_tables.pop_back();
            return "RtlAddFunctionTable failed for Windows x64 JIT code";
        }
        return nullptr;
    }

    int finalize(char** error) noexcept {
        for (const auto& section : pending) {
            DWORD previous;
            if (!VirtualProtect(section.start, section.length, section.protection, &previous)) {
                if (error) *error = message("VirtualProtect failed for a Windows x64 JIT section");
                return 1;
            }
            if (section.protection == PAGE_EXECUTE_READ &&
                !FlushInstructionCache(GetCurrentProcess(), section.start, section.length)) {
                if (error) *error = message("FlushInstructionCache failed for Windows x64 JIT code");
                return 1;
            }
        }
        const char* failure = nullptr;
        try { failure = register_unwind(); }
        catch (...) { failure = "Could not allocate the Windows x64 JIT unwind table"; }
        if (failure) {
            if (error) *error = message(failure);
            return 1;
        }
        pending.clear();
        return 0;
    }

public:
    explicit WindowsX64JitMemory(char* (*create_message)(const char*)) : message(create_message) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        page = info.dwPageSize;
        // Reserve address space once per live image, committing only requested pages.
        // Under fragmentation a smaller contiguous range can still hold a small module.
        for (; reserved >= info.dwAllocationGranularity; reserved >>= 1) {
            base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, reserved, MEM_RESERVE, PAGE_NOACCESS));
            if (base) return;
        }
        throw std::runtime_error("Could not reserve a contiguous Windows x64 JIT image");
    }
    WindowsX64JitMemory(const WindowsX64JitMemory&) = delete;
    WindowsX64JitMemory& operator=(const WindowsX64JitMemory&) = delete;
    ~WindowsX64JitMemory() {
        for (auto& table : unwind_tables) RtlDeleteFunctionTable(table.data());
        VirtualFree(base, 0, MEM_RELEASE);
    }

    static std::uint8_t* code(void* context, std::uintptr_t size, unsigned alignment, unsigned, const char*) {
        return static_cast<WindowsX64JitMemory*>(context)->allocate(size, alignment, PAGE_EXECUTE_READ);
    }
    static std::uint8_t* data(void* context, std::uintptr_t size, unsigned alignment, unsigned, const char* name, int read_only) {
        const std::string_view section = name ? name : "";
        return static_cast<WindowsX64JitMemory*>(context)->allocate(size, alignment,
            read_only ? PAGE_READONLY : PAGE_READWRITE, section == ".pdata" || section.starts_with(".pdata$"));
    }
    static int finish(void* context, char** error) {
        return static_cast<WindowsX64JitMemory*>(context)->finalize(error);
    }
    static void destroy(void* context) { delete static_cast<WindowsX64JitMemory*>(context); }
};
}
