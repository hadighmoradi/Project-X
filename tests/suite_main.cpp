#include "test_report.hpp"

#include <iostream>
#include <string>

void run_n54_register_codegen_tests(int& passed, int& failed);
void run_n55_n59_backend_optimization_tests(int& passed, int& failed);
void run_n72_n77_register_codegen_tests(int& passed, int& failed);
void run_n78_n83_cfg_phi_unwind_tests(int& passed, int& failed);
void run_n108_flow_emitter_anchor_tests(int& passed, int& failed);
void run_n109a_cfg_edge_anchor_tests(int& passed, int& failed);
void run_n109b_n110a_machine_transfer_tests(int& passed, int& failed);
void run_n111_machine_location_plan_tests(int& passed, int& failed);
void run_n112_flow_segment_plan_tests(int& passed, int& failed);
void run_n113_flow_spill_reload_resolution_tests(int& passed, int& failed);

#if defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>

namespace {

void write_wide_text(HANDLE file, const wchar_t* value) {
    if (value == nullptr) value = L"<null>";
    std::size_t length = 0U;
    while (value[length] != L'\0' && length < 4096U) ++length;
    if (length == 0U) return;
    DWORD written = 0U;
    const DWORD byte_count = static_cast<DWORD>(length * sizeof(wchar_t));
    (void)::WriteFile(file, value, byte_count, &written, nullptr);
}

void write_labeled_wide_text(HANDLE file, const wchar_t* label, const wchar_t* value) {
    write_wide_text(file, label);
    write_wide_text(file, value);
    write_wide_text(file, L"\r\n");
}

void write_line_number(HANDLE file, unsigned int line) {
    wchar_t digits[16]{};
    std::size_t position = 15U;
    digits[position] = L'\0';
    do {
        --position;
        digits[position] = static_cast<wchar_t>(L'0' + (line % 10U));
        line /= 10U;
    } while (line != 0U && position > 0U);
    write_wide_text(file, L"Line: ");
    write_wide_text(file, &digits[position]);
    write_wide_text(file, L"\r\n");
}

void __cdecl xfrontend_invalid_parameter_handler(
    const wchar_t* expression,
    const wchar_t* function,
    const wchar_t* file_name,
    unsigned int line,
    uintptr_t reserved) {
    (void)reserved;
    HANDLE file = ::CreateFileW(
        L"build\\reports\\n35-invalid-parameter.log",
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        write_wide_text(file, L"MSVC CRT invalid-parameter handler invoked.\r\n");
        write_labeled_wide_text(file, L"Expression: ", expression);
        write_labeled_wide_text(file, L"Function: ", function);
        write_labeled_wide_text(file, L"Source: ", file_name);
        write_line_number(file, line);
        (void)::FlushFileBuffers(file);
        (void)::CloseHandle(file);
    }
    // Do not let the CRT display a generic crash dialog or continue after an
    // invalid-parameter condition. The diagnostic above is durable first.
    ::ExitProcess(0xE5U);
}

} // namespace
#endif

#if defined(XFRONTEND_SUITE_N30)
void run_pe64_image_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N31)
void run_archive_linker_adapter_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N32)
void run_coff_import_library_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N33)
void run_pe64_exports_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N34)
void run_pe64_dll_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N35)
void run_pe64_dll_relocation_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N36)
void run_pe64_dll_import_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N37)
void run_pe64_dll_tls_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N38)
void run_pe64_dll_unwind_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N39_N43)
void run_pe64_n39_n43_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N44)
void run_pe64_n44_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N45)
void run_pe64_n45_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N46)
void run_linker_dce_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N47)
void run_native_object_emitter_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N48)
void run_x64_codegen_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N49_N53)
void run_n49_n53_native_backend_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N54)
void run_n54_register_codegen_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N55_N59)
void run_n55_n59_backend_optimization_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N60_N65)
void run_n60_n65_register_allocation_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N66_N71)
void run_n66_n71_register_analysis_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N72_N77)
void run_n72_n77_register_codegen_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N78_N83)
void run_n78_n83_cfg_phi_unwind_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N84_N89)
void run_n84_n89_multiblock_codegen_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N90_N95)
void run_n90_n95_machine_home_codegen_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N96_N101)
void run_n96_n101_persistent_register_codegen_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N102_N107)
void run_n102_n107_allocator_hardening_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_N108)
// N108 test entry point is declared with the other test functions above.
#elif defined(XFRONTEND_SUITE_N109A)
// N109A test entry point is declared with the other test functions above.
#elif defined(XFRONTEND_SUITE_N109B_N110A)
// N109B/N110A test entry point is declared with the other test functions above.
#elif defined(XFRONTEND_SUITE_N111)
// N111 test entry point is declared with the other test functions above.
#elif defined(XFRONTEND_SUITE_N112)
// N112 test entry point is declared with the other test functions above.
#elif defined(XFRONTEND_SUITE_N113)
// N113 test entry point is declared with the other test functions above.
#elif defined(XFRONTEND_SUITE_PE64)
void run_pe64_n44_tests(int& passed, int& failed);
void run_pe64_n45_tests(int& passed, int& failed);
void run_pe64_dll_relocation_tests(int& passed, int& failed);
void run_pe64_dll_import_tests(int& passed, int& failed);
void run_pe64_dll_tls_tests(int& passed, int& failed);
void run_pe64_dll_unwind_tests(int& passed, int& failed);
void run_pe64_image_tests(int& passed, int& failed);
void run_archive_linker_adapter_tests(int& passed, int& failed);
void run_coff_import_library_tests(int& passed, int& failed);
void run_pe64_exports_tests(int& passed, int& failed);
void run_pe64_dll_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_ELF64)
void run_linker_tests(int& passed, int& failed);
void run_executable_image_tests(int& passed, int& failed);
void run_elf64_shared_object_tests(int& passed, int& failed);
void run_elf64_dynamic_import_tests(int& passed, int& failed);
void run_elf64_plt_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_NATIVE_CORE)
void run_target_abi_tests(int& passed, int& failed);
void run_native_symbol_tests(int& passed, int& failed);
void run_object_file_tests(int& passed, int& failed);
void run_binary_object_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_LEXER)
void run_lexer_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_OWNERSHIP)
void run_lifetime_tests(int& passed, int& failed);
void run_ownership_matrix_tests(int& passed, int& failed);
void run_ownership_semantics_tests(int& passed, int& failed);
void run_ownership_flow_tests(int& passed, int& failed);
void run_ownership_deep_matrix_tests(int& passed, int& failed);
void run_ownership_acceptance_matrix_tests(int& passed, int& failed);
void run_ownership_lowering_tests(int& passed, int& failed);
void run_drop_contract_tests(int& passed, int& failed);
void run_drop_abi_tests(int& passed, int& failed);
void run_target_abi_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_PARSER)
void run_parser_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_SEMANTIC)
void run_semantic_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_MODULES)
void run_module_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_SOURCES)
void run_source_manager_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_CONTROL_FLOW)
void run_control_flow_tests(int& passed, int& failed);
void run_reachability_tests(int& passed, int& failed);
#elif defined(XFRONTEND_SUITE_LIFETIME)
void run_lifetime_tests(int& passed, int& failed);
#else
#error A supported XFRONTEND_SUITE_* compile definition is required.
#endif

int main() {
#if defined(_MSC_VER)
    (void)::_set_invalid_parameter_handler(&xfrontend_invalid_parameter_handler);
#endif
    int passed = 0;
    int failed = 0;
#if defined(XFRONTEND_SUITE_N30)
    run_pe64_image_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N31)
    run_archive_linker_adapter_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N32)
    run_coff_import_library_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N33)
    run_pe64_exports_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N34)
    run_pe64_dll_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N35)
    run_pe64_dll_relocation_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N36)
    run_pe64_dll_import_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N37)
    run_pe64_dll_tls_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N38)
    run_pe64_dll_unwind_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N39_N43)
    run_pe64_n39_n43_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N44)
    run_pe64_n44_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N45)
    run_pe64_n45_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N46)
    run_linker_dce_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N47)
    run_native_object_emitter_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N48)
    run_x64_codegen_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N49_N53)
    run_n49_n53_native_backend_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N54)
    run_n54_register_codegen_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N55_N59)
    run_n55_n59_backend_optimization_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N60_N65)
    run_n60_n65_register_allocation_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N66_N71)
    run_n66_n71_register_analysis_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N72_N77)
    run_n72_n77_register_codegen_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N78_N83)
    run_n78_n83_cfg_phi_unwind_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N84_N89)
    run_n84_n89_multiblock_codegen_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N90_N95)
    run_n90_n95_machine_home_codegen_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N96_N101)
    run_n96_n101_persistent_register_codegen_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N102_N107)
    run_n102_n107_allocator_hardening_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N108)
    run_n108_flow_emitter_anchor_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N109A)
    run_n109a_cfg_edge_anchor_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N109B_N110A)
    run_n109b_n110a_machine_transfer_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N111)
    run_n111_machine_location_plan_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N112)
    run_n112_flow_segment_plan_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_N113)
    run_n113_flow_spill_reload_resolution_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_PE64)
    run_pe64_n44_tests(passed, failed);
    run_pe64_n45_tests(passed, failed);
    run_pe64_image_tests(passed, failed);
    run_archive_linker_adapter_tests(passed, failed);
    run_coff_import_library_tests(passed, failed);
    run_pe64_exports_tests(passed, failed);
    run_pe64_dll_tests(passed, failed);
    run_pe64_dll_relocation_tests(passed, failed);
    run_pe64_dll_import_tests(passed, failed);
    run_pe64_dll_tls_tests(passed, failed);
    run_pe64_dll_unwind_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_ELF64)
    run_linker_tests(passed, failed);
    run_executable_image_tests(passed, failed);
    run_elf64_shared_object_tests(passed, failed);
    run_elf64_dynamic_import_tests(passed, failed);
    run_elf64_plt_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_NATIVE_CORE)
    run_target_abi_tests(passed, failed);
    run_native_symbol_tests(passed, failed);
    run_object_file_tests(passed, failed);
    run_binary_object_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_LEXER)
    run_lexer_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_OWNERSHIP)
    run_lifetime_tests(passed, failed);
    run_ownership_matrix_tests(passed, failed);
    run_ownership_semantics_tests(passed, failed);
    run_ownership_flow_tests(passed, failed);
    run_ownership_deep_matrix_tests(passed, failed);
    run_ownership_acceptance_matrix_tests(passed, failed);
    run_ownership_lowering_tests(passed, failed);
    run_drop_contract_tests(passed, failed);
    run_drop_abi_tests(passed, failed);
    run_target_abi_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_PARSER)
    run_parser_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_SEMANTIC)
    run_semantic_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_MODULES)
    run_module_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_SOURCES)
    run_source_manager_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_CONTROL_FLOW)
    run_control_flow_tests(passed, failed);
    run_reachability_tests(passed, failed);
#elif defined(XFRONTEND_SUITE_LIFETIME)
    run_lifetime_tests(passed, failed);
#endif

    const std::string report_dir = xfrontend_test_report::env_or(
        "X_TEST_REPORT_DIR", "build\\reports");
    xfrontend_test_report::write_reports(report_dir, passed, failed);
    if (passed + failed == 0) {
        std::cerr << "No test IDs matched the selected suite/filter. Check -Suite and -Filter.\n";
        return 2;
    }
    std::cout << "X Native Frontend selected test suite: " << passed
              << " passed, " << failed << " failed, " << (passed + failed)
              << " executed\n";
    std::cout << "Test reports: " << report_dir
              << "\\test-report.json and test-report.txt\n";
    return failed == 0 ? 0 : 1;
}
