// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 10;
    HMODULE module = ::LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::wcerr << L"LoadLibraryW failed: " << ::GetLastError() << L'\n';
        return 11;
    }

    const LANGID language = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    // Use the explicitly wide resource-type ordinal: RT_RCDATA expands through
    // MAKEINTRESOURCEA when UNICODE is not defined, which is incompatible with
    // the explicitly wide FindResourceExW API.
    constexpr WORD kRtRcDataId = 10U;
    const LPCWSTR resource_type = MAKEINTRESOURCEW(kRtRcDataId);
    const LPCWSTR resource_name = MAKEINTRESOURCEW(101U);
    HRSRC resource = ::FindResourceExW(module, resource_type, resource_name, language);
    if (resource == nullptr) { ::FreeLibrary(module); return 12; }
    const DWORD resource_size = ::SizeofResource(module, resource);
    HGLOBAL loaded_resource = ::LoadResource(module, resource);
    const void* resource_data = loaded_resource == nullptr ? nullptr : ::LockResource(loaded_resource);
    static constexpr unsigned char expected[] = {'X', '3', '9', 'N', '4', '3'};
    if (resource_size != sizeof(expected) || resource_data == nullptr ||
        std::memcmp(resource_data, expected, sizeof(expected)) != 0) {
        ::FreeLibrary(module); return 13;
    }

    using MarkerFunction = int (__cdecl*)();
    const FARPROC marker_address = ::GetProcAddress(module, "EntryPoint");
    MarkerFunction marker = nullptr;
    static_assert(sizeof(marker) == sizeof(marker_address), "function pointer representation mismatch");
    std::memcpy(&marker, &marker_address, sizeof(marker));
    if (marker == nullptr || marker() != 42) { ::FreeLibrary(module); return 14; }

    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    using LookupFunction = PRUNTIME_FUNCTION (WINAPI*)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);
    const FARPROC lookup_address = ntdll == nullptr ? nullptr : ::GetProcAddress(ntdll, "RtlLookupFunctionEntry");
    LookupFunction lookup = nullptr;
    static_assert(sizeof(lookup) == sizeof(lookup_address), "function pointer representation mismatch");
    std::memcpy(&lookup, &lookup_address, sizeof(lookup));
    if (lookup == nullptr) { ::FreeLibrary(module); return 15; }
    DWORD64 image_base = 0U;
    const auto control_pc = static_cast<DWORD64>(reinterpret_cast<ULONG_PTR>(marker));
    PRUNTIME_FUNCTION runtime_function = lookup(control_pc, &image_base, nullptr);
    if (runtime_function == nullptr || image_base != static_cast<DWORD64>(reinterpret_cast<ULONG_PTR>(module))) {
        ::FreeLibrary(module); return 16;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { ::FreeLibrary(module); return 17; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        reinterpret_cast<const unsigned char*>(module) + static_cast<std::size_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        ::FreeLibrary(module); return 18;
    }
    const auto& directories = nt->OptionalHeader.DataDirectory;
    if (directories[IMAGE_DIRECTORY_ENTRY_RESOURCE].VirtualAddress == 0U ||
        directories[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress == 0U ||
        directories[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG].VirtualAddress == 0U ||
        directories[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress == 0U ||
        directories[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].VirtualAddress == 0U ||
        directories[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress == 0U ||
        directories[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress == 0U) {
        ::FreeLibrary(module); return 19;
    }

    std::wcout << L"Windows Loader: DLL loaded successfully\n";
    std::wcout << L"Resource: verified\n";
    std::wcout << L"COFF-linked export: returned 42\n";
    std::wcout << L"RtlLookupFunctionEntry: .pdata/.xdata registered\n";
    std::wcout << L"PE directories: import, delay import, bound import, resource, load config, reloc, exception\n";
    ::FreeLibrary(module);
    return 42;
}
