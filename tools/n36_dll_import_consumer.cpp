// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <stdint.h>

using HMODULE = void*;
using FARPROC = intptr_t(*)();
using BOOL = int;
using DWORD = unsigned long;
using SIZE_T = size_t;

extern "C" __declspec(dllimport) void* __stdcall VirtualAlloc(void*, SIZE_T, DWORD, DWORD);
extern "C" __declspec(dllimport) BOOL __stdcall VirtualFree(void*, SIZE_T, DWORD);
extern "C" __declspec(dllimport) HMODULE __stdcall LoadLibraryA(const char*);
extern "C" __declspec(dllimport) FARPROC __stdcall GetProcAddress(HMODULE, const char*);
extern "C" __declspec(dllimport) BOOL __stdcall FreeLibrary(HMODULE);
extern "C" __declspec(dllimport) DWORD __stdcall GetCurrentProcessId();
extern "C" __declspec(dllimport) void __stdcall ExitProcess(unsigned int);

using ImportedProcessId = DWORD (__stdcall*)();

extern "C" void n36_start() {
    const auto preferred_base = reinterpret_cast<void*>(0x180000000ULL);
    constexpr SIZE_T reserve_size = 0x10000U;
    constexpr DWORD mem_reserve = 0x2000U;
    constexpr DWORD mem_release = 0x8000U;
    constexpr DWORD page_noaccess = 0x01U;

    void* reserved = VirtualAlloc(preferred_base, reserve_size, mem_reserve, page_noaccess);
    if (reserved != preferred_base) ExitProcess(40U);

    HMODULE module = LoadLibraryA("n36_import_native.dll");
    if (module == nullptr) {
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(41U);
    }
    if (module == preferred_base) {
        (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(43U);
    }

    FARPROC raw_function = GetProcAddress(module, "N36CurrentProcessId");
    FARPROC raw_pointer = GetProcAddress(module, "N36FunctionPointer");
    if (raw_function == nullptr || raw_pointer == nullptr) {
        (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(44U);
    }

    const auto function = reinterpret_cast<ImportedProcessId>(
        reinterpret_cast<uintptr_t>(raw_function));
    const uintptr_t pointer_slot_address = reinterpret_cast<uintptr_t>(raw_pointer);
    const auto* pointer_slot = reinterpret_cast<const uintptr_t*>(pointer_slot_address);
    const uintptr_t relocated_pointer = *pointer_slot;
    const uintptr_t relocated_function = reinterpret_cast<uintptr_t>(raw_function);
    if (relocated_pointer != relocated_function) {
        (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(45U);
    }

    const DWORD imported_pid = function(); // DLL tail-jumps through its resolved IAT.
    const DWORD native_pid = GetCurrentProcessId();
    (void)FreeLibrary(module);
    (void)VirtualFree(reserved, 0U, mem_release);
    ExitProcess(imported_pid != 0U && imported_pid == native_pid ? 42U : 46U);
}
