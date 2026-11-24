// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "test_report.hpp"

#include <iostream>
#include <string>

void run_lexer_tests(int& passed, int& failed);
void run_parser_tests(int& passed, int& failed);
void run_source_manager_tests(int& passed, int& failed);
void run_semantic_tests(int& passed, int& failed);
void run_module_tests(int& passed, int& failed);
void run_control_flow_tests(int& passed, int& failed);
void run_reachability_tests(int& passed, int& failed);
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
void run_native_symbol_tests(int& passed, int& failed);
void run_object_file_tests(int& passed, int& failed);
void run_binary_object_tests(int& passed, int& failed);
void run_linker_tests(int& passed, int& failed);
void run_executable_image_tests(int& passed, int& failed);
void run_elf64_shared_object_tests(int& passed, int& failed);
void run_elf64_dynamic_import_tests(int& passed, int& failed);
void run_elf64_plt_tests(int& passed, int& failed);
void run_pe64_image_tests(int& passed, int& failed);
void run_archive_linker_adapter_tests(int& passed, int& failed);
void run_coff_import_library_tests(int& passed, int& failed);
void run_pe64_exports_tests(int& passed, int& failed);
void run_pe64_dll_tests(int& passed, int& failed);
void run_pe64_dll_relocation_tests(int& passed, int& failed);
void run_pe64_dll_import_tests(int& passed, int& failed);
void run_pe64_dll_tls_tests(int& passed, int& failed);
void run_pe64_dll_unwind_tests(int& passed, int& failed);
void run_pe64_n39_n43_tests(int& passed, int& failed);
void run_pe64_n44_tests(int& passed, int& failed);
void run_pe64_n45_tests(int& passed, int& failed);
void run_linker_dce_tests(int& passed, int& failed);
void run_native_object_emitter_tests(int& passed, int& failed);
void run_x64_codegen_tests(int& passed, int& failed);
void run_n49_n53_native_backend_tests(int& passed, int& failed);
void run_n54_register_codegen_tests(int& passed, int& failed);
void run_n55_n59_backend_optimization_tests(int& passed, int& failed);
void run_n60_n65_register_allocation_tests(int& passed, int& failed);
void run_n66_n71_register_analysis_tests(int& passed, int& failed);
void run_n72_n77_register_codegen_tests(int& passed, int& failed);
void run_n78_n83_cfg_phi_unwind_tests(int& passed, int& failed);
void run_n84_n89_multiblock_codegen_tests(int& passed, int& failed);
void run_n90_n95_machine_home_codegen_tests(int& passed, int& failed);
void run_n96_n101_persistent_register_codegen_tests(int& passed, int& failed);

int main() {
    int passed = 0;
    int failed = 0;

    run_lexer_tests(passed, failed);
    run_parser_tests(passed, failed);
    run_source_manager_tests(passed, failed);
    run_semantic_tests(passed, failed);
    run_module_tests(passed, failed);
    run_control_flow_tests(passed, failed);
    run_reachability_tests(passed, failed);
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
    run_native_symbol_tests(passed, failed);
    run_object_file_tests(passed, failed);
    run_binary_object_tests(passed, failed);
    run_linker_tests(passed, failed);
    run_executable_image_tests(passed, failed);
    run_elf64_shared_object_tests(passed, failed);
    run_elf64_dynamic_import_tests(passed, failed);
    run_elf64_plt_tests(passed, failed);
    run_pe64_image_tests(passed, failed);
    run_archive_linker_adapter_tests(passed, failed);
    run_coff_import_library_tests(passed, failed);
    run_pe64_exports_tests(passed, failed);
    run_pe64_dll_tests(passed, failed);
    run_pe64_dll_relocation_tests(passed, failed);
    run_pe64_dll_import_tests(passed, failed);
    run_pe64_dll_tls_tests(passed, failed);
    run_pe64_dll_unwind_tests(passed, failed);
    run_pe64_n39_n43_tests(passed, failed);
    run_pe64_n44_tests(passed, failed);
    run_pe64_n45_tests(passed, failed);
    run_linker_dce_tests(passed, failed);
    run_native_object_emitter_tests(passed, failed);
    run_x64_codegen_tests(passed, failed);
    run_n49_n53_native_backend_tests(passed, failed);
    run_n54_register_codegen_tests(passed, failed);
    run_n55_n59_backend_optimization_tests(passed, failed);
    run_n60_n65_register_allocation_tests(passed, failed);
    run_n66_n71_register_analysis_tests(passed, failed);
    run_n72_n77_register_codegen_tests(passed, failed);
    run_n78_n83_cfg_phi_unwind_tests(passed, failed);
    run_n84_n89_multiblock_codegen_tests(passed, failed);
    run_n90_n95_machine_home_codegen_tests(passed, failed);
    run_n96_n101_persistent_register_codegen_tests(passed, failed);

    const std::string report_dir = xfrontend_test_report::env_or(
        "X_TEST_REPORT_DIR", "build\\reports");
    xfrontend_test_report::write_reports(report_dir, passed, failed);
    if (passed + failed == 0) {
        std::cerr << "No test IDs matched the selected filter. Check X_TEST_FILTER.\n";
        return 2;
    }
    std::cout << "X Native Frontend full cumulative regression suite: " << passed
              << " passed, " << failed << " failed, " << (passed + failed)
              << " executed\n";
    std::cout << "Test reports: " << report_dir
              << "\\test-report.json and test-report.txt\n";
    return failed == 0 ? 0 : 1;
}
