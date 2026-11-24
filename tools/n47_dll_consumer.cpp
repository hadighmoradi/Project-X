// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <iostream>

int main() {
    HMODULE library = LoadLibraryW(L"n47-source-dce.dll");
    if (library == nullptr) {
        std::cerr << "N47_WINDOWS_LOADER | RESULT=FAILED | ERROR=LoadLibraryW | WIN32_ERROR="
                  << GetLastError() << " | EXIT=2\n";
        return 2;
    }
    const FARPROC exported = GetProcAddress(library, "XMain");
    if (exported == nullptr) {
        const DWORD error = GetLastError();
        FreeLibrary(library);
        std::cerr << "N47_WINDOWS_LOADER | RESULT=FAILED | ERROR=GetProcAddress | WIN32_ERROR="
                  << error << " | EXIT=3\n";
        return 3;
    }
    using EntryFunction = int (__cdecl*)();
    EntryFunction entry = nullptr;
    static_assert(sizeof(entry) == sizeof(exported), "function and data pointer sizes differ on this target");
    std::memcpy(&entry, &exported, sizeof(entry));
    const int result = entry();
    FreeLibrary(library);
    const bool passed = result == 42;
    std::cout << "N47_WINDOWS_LOADER | RESULT=" << (passed ? "PASSED" : "FAILED")
              << " | SAMPLE_EXIT=" << result << " | EXIT=" << (passed ? 0 : 4) << '\n';
    return passed ? 0 : 4;
}
