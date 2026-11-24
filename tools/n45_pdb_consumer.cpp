// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    HMODULE module = ::LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::cerr << "LoadLibraryW failed: " << ::GetLastError() << '\n';
        return 3;
    }
    using Entry = int (__cdecl*)();
    const FARPROC procedure = ::GetProcAddress(module, "DebugEntry");
    Entry entry = nullptr;
    static_assert(sizeof(entry) == sizeof(procedure), "Windows function pointer representation is unexpected");
    std::memcpy(&entry, &procedure, sizeof(entry));
    if (entry == nullptr) {
        std::cerr << "GetProcAddress failed: " << ::GetLastError() << '\n';
        (void)::FreeLibrary(module);
        return 4;
    }
    const int value = entry();
    (void)::FreeLibrary(module);
    std::cout << "SAMPLE_EXIT=" << value << '\n';
    return value == 42 ? 0 : 5;
}
