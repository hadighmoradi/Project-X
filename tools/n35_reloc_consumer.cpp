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
extern "C" __declspec(dllimport) void __stdcall ExitProcess(unsigned int);

extern "C" void n35_start() {
    const auto preferred_base = reinterpret_cast<void*>(0x180000000ULL);
    constexpr SIZE_T reserve_size = 0x10000U;
    constexpr DWORD mem_reserve = 0x2000U;
    constexpr DWORD mem_release = 0x8000U;
    constexpr DWORD page_noaccess = 0x01U;

    void* reserved = VirtualAlloc(preferred_base, reserve_size, mem_reserve, page_noaccess);
    if (reserved != preferred_base) ExitProcess(40U);

    HMODULE module = LoadLibraryA("n35_reloc_native.dll");
    if (module == nullptr) {
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(41U);
    }
    if (module == preferred_base) {
        (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(43U);
    }

    FARPROC function_address = GetProcAddress(module, "N35Return42");
    FARPROC pointer_export_address = GetProcAddress(module, "N35FunctionPointer");
    if (function_address == nullptr || pointer_export_address == nullptr) {
        (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(44U);
    }

    const uintptr_t pointer_slot_address = reinterpret_cast<uintptr_t>(pointer_export_address);
    const auto* pointer_slot = reinterpret_cast<const uintptr_t*>(pointer_slot_address);
    const uintptr_t relocated_pointer = *pointer_slot;
    const uintptr_t relocated_function = reinterpret_cast<uintptr_t>(function_address);
    if (relocated_pointer != relocated_function) {
        (void)FreeLibrary(module);
        (void)VirtualFree(reserved, 0U, mem_release);
        ExitProcess(45U);
    }

    const intptr_t returned = function_address();
    (void)FreeLibrary(module);
    (void)VirtualFree(reserved, 0U, mem_release);
    ExitProcess(returned == 42 ? 42U : 46U);
}
