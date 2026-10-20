#pragma once
// Fail closed on executables with incompatible absolute addresses.
#include <windows.h>
#include <winnt.h>
#include <cstdint>

namespace SAVersionGuard {
    inline bool IsExecutableAddress(std::uintptr_t address) noexcept {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))
            || mbi.State != MEM_COMMIT) return false;
        const DWORD access = mbi.Protect & 0xFFu;
        return access == PAGE_EXECUTE || access == PAGE_EXECUTE_READ ||
               access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
    }

    inline bool Supported() noexcept {
        auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!base || reinterpret_cast<std::uintptr_t>(base) != 0x400000u) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew < 0x40 || dos->e_lfanew > 0x1000) return false;
        const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (pe->Signature != IMAGE_NT_SIGNATURE ||
            pe->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            pe->OptionalHeader.ImageBase != 0x400000 ||
            pe->FileHeader.TimeDateStamp != 0x427101CAu)
            return false;
        // At least these entrypoints must still resolve to executable memory.
        return IsExecutableAddress(0x4087E0) &&
               IsExecutableAddress(0x5B3C60) &&
               IsExecutableAddress(0x5B3DE0) &&
               IsExecutableAddress(0x533150);
    }
}
