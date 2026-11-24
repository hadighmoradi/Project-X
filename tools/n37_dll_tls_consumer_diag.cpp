// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stddef.h>
#include <stdint.h>

using HMODULE = void*;
using HANDLE = void*;
using FARPROC = intptr_t(*)();
using BOOL = int;
using DWORD = unsigned long;
using SIZE_T = size_t;
using TlsGetter = DWORD (__stdcall*)();
using TlsSetter = void (__stdcall*)(DWORD);

extern "C" __declspec(dllimport) void* __stdcall VirtualAlloc(void*, SIZE_T, DWORD, DWORD);
extern "C" __declspec(dllimport) BOOL __stdcall VirtualFree(void*, SIZE_T, DWORD);
extern "C" __declspec(dllimport) HMODULE __stdcall LoadLibraryA(const char*);
extern "C" __declspec(dllimport) FARPROC __stdcall GetProcAddress(HMODULE, const char*);
extern "C" __declspec(dllimport) BOOL __stdcall FreeLibrary(HMODULE);
extern "C" __declspec(dllimport) HANDLE __stdcall CreateThread(void*, SIZE_T, DWORD (__stdcall*)(void*), void*, DWORD, DWORD*);
extern "C" __declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE, DWORD);
extern "C" __declspec(dllimport) BOOL __stdcall GetExitCodeThread(HANDLE, DWORD*);
extern "C" __declspec(dllimport) BOOL __stdcall CloseHandle(HANDLE);
extern "C" __declspec(dllimport) void __stdcall ExitProcess(unsigned int);

namespace {

constexpr DWORD kInitialTlsValue = 0x12345678UL;
constexpr DWORD kMainTlsValue = 0x11112222UL;
constexpr DWORD kWorkerTlsValue = 0x0badf00dUL;

struct ThreadContext {
    TlsGetter get_value;
    TlsSetter set_value;
};

DWORD __stdcall n37_worker(void* parameter) {
    const auto* context = static_cast<const ThreadContext*>(parameter);
    if (context == nullptr || context->get_value == nullptr || context->set_value == nullptr) return 51U;
    if (context->get_value() != kInitialTlsValue) return 52U;
    context->set_value(kWorkerTlsValue);
    return context->get_value() == kWorkerTlsValue ? 0U : 53U;
}

} // namespace

extern "C" void n37_start() {
    constexpr SIZE_T reserve_size = 0x10000U;
    constexpr DWORD mem_reserve = 0x2000U;
    constexpr DWORD mem_release = 0x8000U;
    constexpr DWORD page_noaccess = 0x01U;
    constexpr DWORD infinite = 0xffffffffUL;

    const auto preferred_base = reinterpret_cast<void*>(0x180000000ULL);
    void* reserved = VirtualAlloc(preferred_base, reserve_size, mem_reserve, page_noaccess);
    if (reserved != preferred_base) ExitProcess(40U);

    HMODULE module = LoadLibraryA("n37_tls_native.dll");
    if (module == nullptr || module == preferred_base) {
        if (module != nullptr) (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(41U);
    }

    const FARPROC raw_get = GetProcAddress(module, "N37GetTlsValue");
    const FARPROC raw_set = GetProcAddress(module, "N37SetTlsValue");
    const FARPROC raw_counter = GetProcAddress(module, "N37TlsCallbackCount");
    const FARPROC raw_index = GetProcAddress(module, "N37TlsIndex");
    if (raw_get == nullptr || raw_set == nullptr || raw_counter == nullptr || raw_index == nullptr) {
        (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(47U);
    }

    const auto get_value = reinterpret_cast<TlsGetter>(reinterpret_cast<uintptr_t>(raw_get));
    const auto set_value = reinterpret_cast<TlsSetter>(reinterpret_cast<uintptr_t>(raw_set));
    const auto* callback_counter = reinterpret_cast<const volatile DWORD*>(reinterpret_cast<uintptr_t>(raw_counter));
    const auto* tls_index = reinterpret_cast<const volatile DWORD*>(reinterpret_cast<uintptr_t>(raw_index));
    if (*callback_counter != 1U || *tls_index == 0xffffffffUL || get_value() != kInitialTlsValue) {
        (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(43U);
    }

    set_value(kMainTlsValue);
    if (get_value() != kMainTlsValue) {
        (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(44U);
    }

    ThreadContext context{get_value, set_value};
    DWORD thread_id = 0U;
    HANDLE thread = CreateThread(nullptr, 0U, &n37_worker, &context, 0U, &thread_id);
    if (thread == nullptr || WaitForSingleObject(thread, infinite) != 0U) {
        if (thread != nullptr) (void)CloseHandle(thread);
        (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(45U);
    }

    DWORD worker_exit = 0xffffffffUL;
    const BOOL got_exit = GetExitCodeThread(thread, &worker_exit);
    (void)CloseHandle(thread);

    // Snapshot all shared/ per-thread state before unloading the DLL.
    const DWORD callback_count = *callback_counter;
    const DWORD main_tls_after_worker = get_value();

    (void)FreeLibrary(module);
    (void)VirtualFree(reserved, 0U, mem_release);

    // Diagnostic exit codes:
    // 42: all checks passed.
    // 60: GetExitCodeThread failed.
    // 100 + worker_exit: worker TLS assertion failed (151/152/153 are expected cases).
    // 71: worker TLS write leaked into the main thread.
    // 80 + callback_count: fewer than three TLS callback notifications observed.
    if (got_exit == 0) ExitProcess(60U);
    if (worker_exit != 0U) ExitProcess(100U + worker_exit);
    if (main_tls_after_worker != kMainTlsValue) ExitProcess(71U);
    if (callback_count < 3U) ExitProcess(80U + callback_count);
    ExitProcess(42U);
}
