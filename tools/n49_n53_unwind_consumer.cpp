#include <windows.h>

#include <cstring>
#include <iostream>

namespace {
using ProbeFunction = int (*)();
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::wcerr << L"usage: n49_n53_unwind_consumer <dll-path>\n";
        return 2;
    }
    const HMODULE module = LoadLibraryW(argv[1]);
    if (module == nullptr) {
        std::wcerr << L"N49_UNWIND_LOADER | RESULT=FAILED | ERROR=LOAD_LIBRARY | CODE=" << GetLastError() << L"\n";
        return 3;
    }
    const FARPROC exported = GetProcAddress(module, "XUnwindProbe");
    if (exported == nullptr) {
        std::wcerr << L"N49_UNWIND_LOADER | RESULT=FAILED | ERROR=GET_PROC_ADDRESS | CODE=" << GetLastError() << L"\n";
        FreeLibrary(module);
        return 4;
    }
    static_assert(sizeof(ProbeFunction) == sizeof(FARPROC), "Win64 function pointer size mismatch");
    ProbeFunction probe = nullptr;
    std::memcpy(&probe, &exported, sizeof(probe));

    DWORD64 image_base = 0U;
    const DWORD64 function_address = reinterpret_cast<DWORD64>(probe);
    const PRUNTIME_FUNCTION runtime_function = RtlLookupFunctionEntry(function_address, &image_base, nullptr);
    if (runtime_function == nullptr || runtime_function->UnwindData == 0U) {
        std::wcerr << L"N49_UNWIND_LOADER | RESULT=FAILED | ERROR=RUNTIME_FUNCTION_NOT_FOUND\n";
        FreeLibrary(module);
        return 5;
    }
    const int result = probe();
    FreeLibrary(module);
    std::wcout << L"N49_UNWIND_LOADER | RESULT=" << (result == 42 ? L"PASSED" : L"FAILED")
               << L" | RUNTIME_FUNCTION=FOUND | SAMPLE_EXIT=" << result << L" | EXIT=" << result << L"\n";
    return result;
}
