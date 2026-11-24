// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

bool range_fits(std::size_t total, std::size_t offset, std::size_t length) {
    return offset <= total && length <= total - offset;
}

bool verify_codeview(HMODULE module, const IMAGE_NT_HEADERS64* nt) {
    const auto& debug = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const std::size_t image_size = nt->OptionalHeader.SizeOfImage;
    if (debug.VirtualAddress == 0U || debug.Size < sizeof(IMAGE_DEBUG_DIRECTORY) ||
        debug.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0U ||
        !range_fits(image_size, debug.VirtualAddress, debug.Size)) {
        return false;
    }

    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + debug.VirtualAddress);
    const std::size_t count = debug.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    bool found = false;
    for (std::size_t i = 0U; i < count; ++i) {
        const IMAGE_DEBUG_DIRECTORY& entry = entries[i];
        if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        if (found || entry.AddressOfRawData == 0U || entry.PointerToRawData == 0U ||
            entry.SizeOfData < 25U || !range_fits(image_size, entry.AddressOfRawData, entry.SizeOfData)) {
            return false;
        }
        found = true;
        const auto* record = base + entry.AddressOfRawData;
        if (record[0] != 'R' || record[1] != 'S' || record[2] != 'D' || record[3] != 'S') return false;
        std::uint32_t age = 0U;
        std::memcpy(&age, record + 20U, sizeof(age));
        if (age == 0U) return false;
        const char* path = reinterpret_cast<const char*>(record + 24U);
        const std::size_t path_capacity = entry.SizeOfData - 24U;
        const void* terminator = std::memchr(path, '\0', path_capacity);
        if (terminator == nullptr) return false;
        const auto path_length_signed = static_cast<const char*>(terminator) - path;
        if (path_length_signed < 0) return false;
        const std::size_t path_length = static_cast<std::size_t>(path_length_signed);
        if (std::string(path, path_length) != "n44_integrated.pdb") return false;
    }
    return found;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 10;
    HMODULE module = ::LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::wcerr << L"LoadLibraryW failed: " << ::GetLastError() << L'\n';
        return 11;
    }

    const LANGID language = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
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
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) { ::FreeLibrary(module); return 17; }
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
    if (!verify_codeview(module, nt)) { ::FreeLibrary(module); return 20; }

    std::wcout << L"Windows Loader: Debug-enabled DLL loaded successfully\n";
    std::wcout << L"Resource: verified\n";
    std::wcout << L"COFF-linked export: returned 42\n";
    std::wcout << L"RtlLookupFunctionEntry: .pdata/.xdata registered\n";
    std::wcout << L"CodeView: mapped IMAGE_DEBUG_DIRECTORY and RSDS record verified\n";
    std::wcout << L"PDB reference: n44_integrated.pdb (external PDB not synthesized)\n";
    ::FreeLibrary(module);
    return 42;
}
