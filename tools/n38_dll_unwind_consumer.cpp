// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <stdint.h>

using HMODULE = void*;
using BOOL = int;
using DWORD = unsigned long;
using SIZE_T = size_t;
using FARPROC = intptr_t(*)();
using Return42 = int (__stdcall*)();

struct RuntimeFunction {
    uint32_t begin_address;
    uint32_t end_address;
    uint32_t unwind_data;
};
using LookupRuntimeFunction = RuntimeFunction* (__stdcall*)(uint64_t, uint64_t*, void*);

extern "C" __declspec(dllimport) void* __stdcall VirtualAlloc(void*, SIZE_T, DWORD, DWORD);
extern "C" __declspec(dllimport) BOOL __stdcall VirtualFree(void*, SIZE_T, DWORD);
extern "C" __declspec(dllimport) HMODULE __stdcall LoadLibraryA(const char*);
extern "C" __declspec(dllimport) FARPROC __stdcall GetProcAddress(HMODULE, const char*);
extern "C" __declspec(dllimport) HMODULE __stdcall GetModuleHandleA(const char*);
extern "C" __declspec(dllimport) BOOL __stdcall FreeLibrary(HMODULE);
extern "C" __declspec(dllimport) void __stdcall ExitProcess(unsigned int);

namespace {

void finish(unsigned int code, void* reserved, HMODULE module) {
    if (module != nullptr) (void)FreeLibrary(module);
    if (reserved != nullptr) (void)VirtualFree(reserved, 0U, 0x8000U);
    ExitProcess(code);
}

} // namespace

extern "C" void n38_start() {
    constexpr uint64_t preferred_base = 0x180000000ULL;
    constexpr SIZE_T reserve_size = 0x10000U;
    void* reserved = VirtualAlloc(reinterpret_cast<void*>(preferred_base), reserve_size, 0x2000U, 0x01U);
    if (reserved != reinterpret_cast<void*>(preferred_base)) finish(40U, reserved, nullptr);

    HMODULE module = LoadLibraryA("n38_exception_native.dll");
    if (module == nullptr || module == reserved) finish(41U, reserved, module);

    const FARPROC raw_function = GetProcAddress(module, "N38Return42");
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    const FARPROC raw_lookup = ntdll == nullptr ? nullptr : GetProcAddress(ntdll, "RtlLookupFunctionEntry");
    if (raw_function == nullptr || raw_lookup == nullptr) finish(43U, reserved, module);

    const auto function = reinterpret_cast<Return42>(reinterpret_cast<uintptr_t>(raw_function));
    const auto lookup = reinterpret_cast<LookupRuntimeFunction>(reinterpret_cast<uintptr_t>(raw_lookup));
    uint64_t image_base = 0U;
    const auto* runtime = lookup(reinterpret_cast<uint64_t>(raw_function), &image_base, nullptr);
    if (runtime == nullptr || image_base != reinterpret_cast<uint64_t>(module)) finish(44U, reserved, module);

    const uint64_t function_rva = reinterpret_cast<uint64_t>(raw_function) - image_base;
    if (runtime->begin_address != function_rva || runtime->end_address <= runtime->begin_address ||
        runtime->unwind_data == 0U || (runtime->unwind_data & 3U) != 0U) finish(45U, reserved, module);

    const auto* unwind = reinterpret_cast<const volatile uint8_t*>(image_base + runtime->unwind_data);
    if (unwind[0U] != 1U || unwind[1U] != 4U || unwind[2U] != 1U || unwind[3U] != 0U ||
        unwind[4U] != 4U || unwind[5U] != 0x42U) finish(46U, reserved, module);

    const int value = function();
    finish(value == 42 ? 42U : 47U, reserved, module);
}
