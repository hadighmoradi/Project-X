# SPDX-FileCopyrightText: 2026 Hector Moradi
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)
project(x_native_frontend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Update this cumulative count whenever a milestone adds regression cases.
set(XFRONTEND_TEST_INVENTORY_TOTAL 175976)

add_library(xfrontend
    src/source_manager.cpp
    src/lexer.cpp
    src/parser.cpp
    src/semantic.cpp
    src/module_resolver.cpp
    src/control_flow.cpp
    src/lifetime.cpp
    src/ownership_lowering.cpp
    src/drop_contract.cpp
    src/drop_abi.cpp
    src/target_abi.cpp
    src/native_symbol.cpp
    src/native_object_emitter.cpp
    src/x64_codegen.cpp
    src/win64_unwind_codegen.cpp
    src/linear_scan_allocator.cpp
    src/native_register_analysis.cpp
    src/native_cfg_bridge.cpp
    src/x64_location_transfer.cpp
    src/native_program_pipeline.cpp
    src/object_file.cpp
    src/binary_object.cpp
    src/linker.cpp
    src/executable_image.cpp
    src/elf64_shared_object.cpp
    src/pe64_image.cpp
    src/archive.cpp
    src/system_linker.cpp
    src/coff_import_library.cpp
    src/pe64_exports.cpp
    src/pe64_dll.cpp
    src/pe64_advanced.cpp
    src/pe64_debug_signing.cpp
    src/pe64_pdb.cpp
)
target_include_directories(xfrontend PUBLIC include)

if(MSVC)
    target_compile_options(xfrontend PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(xfrontend PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(xfrontend_tests tests/test_main.cpp tests/lexer_tests.cpp tests/parser_tests.cpp tests/source_manager_tests.cpp tests/semantic_tests.cpp tests/module_tests.cpp tests/control_flow_tests.cpp tests/reachability_tests.cpp tests/lifetime_tests.cpp tests/ownership_matrix_tests.cpp tests/ownership_semantics_tests.cpp tests/ownership_flow_tests.cpp tests/ownership_deep_matrix_tests.cpp tests/ownership_acceptance_matrix_tests.cpp tests/ownership_lowering_tests.cpp tests/drop_contract_tests.cpp tests/drop_abi_tests.cpp tests/target_abi_tests.cpp tests/native_symbol_tests.cpp tests/object_file_tests.cpp tests/binary_object_tests.cpp tests/linker_tests.cpp tests/executable_image_tests.cpp tests/elf64_shared_object_tests.cpp tests/elf64_dynamic_import_tests.cpp tests/elf64_plt_tests.cpp tests/pe64_image_tests.cpp tests/archive_linker_adapter_tests.cpp tests/coff_import_library_tests.cpp tests/pe64_exports_tests.cpp tests/pe64_dll_tests.cpp tests/pe64_dll_relocation_tests.cpp tests/pe64_dll_import_tests.cpp tests/pe64_dll_tls_tests.cpp tests/pe64_dll_unwind_tests.cpp tests/pe64_n39_n43_tests.cpp tests/pe64_n44_tests.cpp tests/pe64_n45_tests.cpp tests/linker_dce_tests.cpp tests/native_object_emitter_tests.cpp tests/x64_codegen_tests.cpp tests/n49_n53_native_backend_tests.cpp tests/n54_register_codegen_tests.cpp tests/n55_n59_backend_optimization_tests.cpp tests/n60_n65_register_allocation_tests.cpp tests/n66_n71_register_analysis_tests.cpp tests/n72_n77_register_codegen_tests.cpp tests/n78_n83_cfg_phi_unwind_tests.cpp tests/n84_n89_multiblock_codegen_tests.cpp tests/n90_n95_machine_home_codegen_tests.cpp tests/n96_n101_persistent_register_codegen_tests.cpp tests/n102_n107_allocator_hardening_tests.cpp)
target_link_libraries(xfrontend_tests PRIVATE xfrontend)
target_compile_definitions(xfrontend_tests PRIVATE XFRONTEND_TEST_INVENTORY_TOTAL=${XFRONTEND_TEST_INVENTORY_TOTAL})

function(add_xfrontend_suite_target target_name suite_define)
    add_executable(${target_name} tests/suite_main.cpp ${ARGN})
    target_compile_definitions(${target_name} PRIVATE
        ${suite_define}
        XFRONTEND_TEST_INVENTORY_TOTAL=${XFRONTEND_TEST_INVENTORY_TOTAL})
    target_link_libraries(${target_name} PRIVATE xfrontend)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /WX /utf-8 /permissive-)
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

# Focused suite executables keep Fast/Subsystem builds from compiling unrelated
# regression translation units. The full xfrontend_tests target remains intact.
add_xfrontend_suite_target(xfrontend_tests_n30 XFRONTEND_SUITE_N30 tests/pe64_image_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n31 XFRONTEND_SUITE_N31 tests/archive_linker_adapter_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n32 XFRONTEND_SUITE_N32 tests/coff_import_library_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n33 XFRONTEND_SUITE_N33 tests/pe64_exports_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n34 XFRONTEND_SUITE_N34 tests/pe64_dll_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n35 XFRONTEND_SUITE_N35 tests/pe64_dll_relocation_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n36 XFRONTEND_SUITE_N36 tests/pe64_dll_import_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n37 XFRONTEND_SUITE_N37 tests/pe64_dll_tls_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n38 XFRONTEND_SUITE_N38 tests/pe64_dll_unwind_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n39_n43 XFRONTEND_SUITE_N39_N43 tests/pe64_n39_n43_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n44 XFRONTEND_SUITE_N44 tests/pe64_n44_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n45 XFRONTEND_SUITE_N45 tests/pe64_n45_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n46 XFRONTEND_SUITE_N46 tests/linker_dce_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n47 XFRONTEND_SUITE_N47 tests/native_object_emitter_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n48 XFRONTEND_SUITE_N48 tests/x64_codegen_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n49_n53 XFRONTEND_SUITE_N49_N53 tests/n49_n53_native_backend_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n54 XFRONTEND_SUITE_N54 tests/n54_register_codegen_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n55_n59 XFRONTEND_SUITE_N55_N59 tests/n55_n59_backend_optimization_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n60_n65 XFRONTEND_SUITE_N60_N65 tests/n60_n65_register_allocation_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n66_n71 XFRONTEND_SUITE_N66_N71 tests/n66_n71_register_analysis_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n72_n77 XFRONTEND_SUITE_N72_N77 tests/n72_n77_register_codegen_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n78_n83 XFRONTEND_SUITE_N78_N83 tests/n78_n83_cfg_phi_unwind_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n84_n89 XFRONTEND_SUITE_N84_N89 tests/n84_n89_multiblock_codegen_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n90_n95 XFRONTEND_SUITE_N90_N95 tests/n90_n95_machine_home_codegen_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n96_n101 XFRONTEND_SUITE_N96_N101 tests/n96_n101_persistent_register_codegen_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n102_n107 XFRONTEND_SUITE_N102_N107 tests/n102_n107_allocator_hardening_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n108 XFRONTEND_SUITE_N108 tests/n108_flow_emitter_anchor_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n109a XFRONTEND_SUITE_N109A tests/n109a_cfg_edge_anchor_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n109b_n110a XFRONTEND_SUITE_N109B_N110A tests/n109b_n110a_machine_transfer_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n111 XFRONTEND_SUITE_N111 tests/n111_machine_location_plan_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n112 XFRONTEND_SUITE_N112 tests/n112_flow_segment_plan_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n113 XFRONTEND_SUITE_N113 tests/n113_flow_spill_reload_resolution_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_n114_b13b XFRONTEND_SUITE_N114_B13B tests/n114_b13b_expression_branch_anchor_tests.cpp)

add_executable(emit_n84_n89_native_exe tools/emit_n84_n89_native_exe.cpp)
target_link_libraries(emit_n84_n89_native_exe PRIVATE xfrontend)
add_executable(emit_n90_n95_native_exe tools/emit_n90_n95_native_exe.cpp)
target_link_libraries(emit_n90_n95_native_exe PRIVATE xfrontend)
add_executable(emit_n96_n101_native_exe tools/emit_n96_n101_native_exe.cpp)
target_link_libraries(emit_n96_n101_native_exe PRIVATE xfrontend)
add_executable(emit_n102_n107_native_exe tools/emit_n102_n107_native_exe.cpp)
target_link_libraries(emit_n102_n107_native_exe PRIVATE xfrontend)
add_executable(emit_n106_stack_probe_native_exe tools/emit_n106_stack_probe_native_exe.cpp)
target_link_libraries(emit_n106_stack_probe_native_exe PRIVATE xfrontend)
add_executable(emit_n113_g3_native_exe tools/emit_n113_g3_native_exe.cpp)
target_link_libraries(emit_n113_g3_native_exe PRIVATE xfrontend)

# N113-G18: read-only FunctionEmitter Flow-edge census exporter for real source inputs.
add_executable(xfrontend_n113_g18_census tools/n113_g18_census_report.cpp)
target_link_libraries(xfrontend_n113_g18_census PRIVATE xfrontend)
if(MSVC)
    target_compile_options(xfrontend_n113_g18_census PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(xfrontend_n113_g18_census PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# N113-G19: per-Value/per-edge endpoint evidence export; preserves the G18 v1 tool.
add_executable(xfrontend_n113_g19_endpoint_census tools/n113_g19_endpoint_census_report.cpp)
target_link_libraries(xfrontend_n113_g19_endpoint_census PRIVATE xfrontend)
if(MSVC)
    target_compile_options(xfrontend_n113_g19_endpoint_census PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(xfrontend_n113_g19_endpoint_census PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# N113-G20: read-only reconciliation of G4/G6 live values, G7 scratch trials,
# G8 readiness attempts and G10/G11 evidence on exact CFG edges.
add_executable(xfrontend_n113_g20_edge_diagnostics tools/n113_g20_edge_diagnostics_report.cpp)
target_link_libraries(xfrontend_n113_g20_edge_diagnostics PRIVATE xfrontend)
if(MSVC)
    target_compile_options(xfrontend_n113_g20_edge_diagnostics PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(xfrontend_n113_g20_edge_diagnostics PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# N113-G21: read-only audit of existing emitter/Flow spill slots and exact G4 endpoint occupancy.
add_executable(xfrontend_n113_g21_frame_scratch tools/n113_g21_frame_scratch_report.cpp)
target_link_libraries(xfrontend_n113_g21_frame_scratch PRIVATE xfrontend)
if(MSVC)
    target_compile_options(xfrontend_n113_g21_frame_scratch PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(xfrontend_n113_g21_frame_scratch PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# N113-G22: isolated audit of the SpillSlot cycle-storage / register expansion contract.
add_executable(xfrontend_n113_g22_memory_cycle_contract tools/n113_g22_memory_cycle_contract_report.cpp)
target_link_libraries(xfrontend_n113_g22_memory_cycle_contract PRIVATE xfrontend)
if(MSVC)
    target_compile_options(xfrontend_n113_g22_memory_cycle_contract PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(xfrontend_n113_g22_memory_cycle_contract PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# N113-G24: GP SpillSlot cycles may use a proven-dead Win64 volatile XMM1-XMM5
# register for bit-preserving MOVQ memory-to-memory expansion. This tool is a
# synthetic contract probe only; edge liveness and active-frame availability
# remain separate prerequisites and no code is inserted.
add_executable(xfrontend_n113_g24_gp_xmm_expansion tools/n113_g24_gp_xmm_expansion_report.cpp)
target_link_libraries(xfrontend_n113_g24_gp_xmm_expansion PRIVATE xfrontend)
if(MSVC)
    target_compile_options(xfrontend_n113_g24_gp_xmm_expansion PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(xfrontend_n113_g24_gp_xmm_expansion PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# N113-G25: exact-edge reconciliation of G4 live-out/live-in Value sets and
# N111 active locations before selecting a Win64 volatile XMM expansion register.
# The report uses a synthetic CFG contract and remains strictly read-only.
add_executable(xfrontend_n113_g25_gp_xmm_edge_liveness tools/n113_g25_gp_xmm_edge_liveness_report.cpp)
target_link_libraries(xfrontend_n113_g25_gp_xmm_edge_liveness PRIVATE xfrontend)
if(MSVC)
    target_compile_options(xfrontend_n113_g25_gp_xmm_edge_liveness PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(xfrontend_n113_g25_gp_xmm_edge_liveness PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# N113-G26: audit ordered multi-cycle transactions and edge-local reuse of one
# dedicated memory cycle SpillSlot under already-proven G25 exact-edge XMM liveness.
# This remains a read-only protocol audit; it does not reserve a frame or insert code.
add_executable(xfrontend_n113_g26_multi_edge_cycle_audit tools/n113_g26_multi_edge_cycle_audit_report.cpp)
target_link_libraries(xfrontend_n113_g26_multi_edge_cycle_audit PRIVATE xfrontend)
if(MSVC)
    target_compile_options(xfrontend_n113_g26_multi_edge_cycle_audit PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(xfrontend_n113_g26_multi_edge_cycle_audit PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(MSVC)
    target_compile_options(emit_n106_stack_probe_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n106_stack_probe_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(MSVC)
    target_compile_options(emit_n113_g3_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n113_g3_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(MSVC)
    target_compile_options(emit_n102_n107_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n102_n107_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(MSVC)
    target_compile_options(emit_n96_n101_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n96_n101_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(MSVC)
    target_compile_options(emit_n90_n95_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n90_n95_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(MSVC)
    target_compile_options(emit_n84_n89_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n84_n89_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_n48_native_exe tools/emit_n48_native_exe.cpp)
target_link_libraries(emit_n48_native_exe PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_n48_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n48_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_n54_native_exe tools/emit_n54_native_exe.cpp)
target_link_libraries(emit_n54_native_exe PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_n54_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n54_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_n55_n59_native_exe tools/emit_n55_n59_native_exe.cpp)
target_link_libraries(emit_n55_n59_native_exe PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_n55_n59_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n55_n59_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_n72_n77_native_exe tools/emit_n72_n77_native_exe.cpp)
target_link_libraries(emit_n72_n77_native_exe PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_n72_n77_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n72_n77_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_n49_n53_native_exe tools/emit_n49_n53_native_exe.cpp)
target_link_libraries(emit_n49_n53_native_exe PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_n49_n53_native_exe PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n49_n53_native_exe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_n46_dce_smoke tools/emit_n46_dce_smoke.cpp)
target_link_libraries(emit_n46_dce_smoke PRIVATE xfrontend)

add_executable(emit_n47_source_object_smoke tools/emit_n47_source_object_smoke.cpp)
target_link_libraries(emit_n47_source_object_smoke PRIVATE xfrontend)

add_executable(verify_n47_linked_pe tools/verify_n47_linked_pe.cpp)
foreach(n47_tool emit_n47_source_object_smoke verify_n47_linked_pe)
    if(MSVC)
        target_compile_options(${n47_tool} PRIVATE /W4 /WX /utf-8 /permissive-)
    else()
        target_compile_options(${n47_tool} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
if(MSVC)
    target_compile_options(emit_n46_dce_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_n46_dce_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_pe64_n45_debug_obj tools/emit_pe64_n45_debug_obj.cpp)
target_link_libraries(emit_pe64_n45_debug_obj PRIVATE xfrontend)
add_executable(verify_pe64_n45_pdb tools/verify_pe64_n45_pdb.cpp)
target_link_libraries(verify_pe64_n45_pdb PRIVATE xfrontend)
foreach(n45_tool emit_pe64_n45_debug_obj verify_pe64_n45_pdb)
    if(MSVC)
        target_compile_options(${n45_tool} PRIVATE /W4 /WX /utf-8 /permissive-)
    else()
        target_compile_options(${n45_tool} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endforeach()
add_xfrontend_suite_target(xfrontend_tests_pe64 XFRONTEND_SUITE_PE64
    tests/pe64_image_tests.cpp
    tests/archive_linker_adapter_tests.cpp
    tests/coff_import_library_tests.cpp
    tests/pe64_exports_tests.cpp
    tests/pe64_dll_tests.cpp
    tests/pe64_dll_relocation_tests.cpp
    tests/pe64_dll_import_tests.cpp
    tests/pe64_dll_tls_tests.cpp
    tests/pe64_dll_unwind_tests.cpp tests/pe64_n39_n43_tests.cpp tests/pe64_n44_tests.cpp tests/pe64_n45_tests.cpp tests/linker_dce_tests.cpp tests/native_object_emitter_tests.cpp tests/x64_codegen_tests.cpp tests/n49_n53_native_backend_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_elf64 XFRONTEND_SUITE_ELF64
    tests/linker_tests.cpp
    tests/executable_image_tests.cpp
    tests/elf64_shared_object_tests.cpp
    tests/elf64_dynamic_import_tests.cpp
    tests/elf64_plt_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_native_core XFRONTEND_SUITE_NATIVE_CORE
    tests/target_abi_tests.cpp
    tests/native_symbol_tests.cpp
    tests/object_file_tests.cpp
    tests/binary_object_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_lexer XFRONTEND_SUITE_LEXER tests/lexer_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_ownership XFRONTEND_SUITE_OWNERSHIP
    tests/lifetime_tests.cpp
    tests/ownership_matrix_tests.cpp
    tests/ownership_semantics_tests.cpp
    tests/ownership_flow_tests.cpp
    tests/ownership_deep_matrix_tests.cpp
    tests/ownership_acceptance_matrix_tests.cpp
    tests/ownership_lowering_tests.cpp
    tests/drop_contract_tests.cpp
    tests/drop_abi_tests.cpp
    tests/target_abi_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_parser XFRONTEND_SUITE_PARSER tests/parser_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_semantic XFRONTEND_SUITE_SEMANTIC tests/semantic_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_modules XFRONTEND_SUITE_MODULES tests/module_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_sources XFRONTEND_SUITE_SOURCES tests/source_manager_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_control_flow XFRONTEND_SUITE_CONTROL_FLOW
    tests/control_flow_tests.cpp tests/reachability_tests.cpp)
add_xfrontend_suite_target(xfrontend_tests_lifetime XFRONTEND_SUITE_LIFETIME tests/lifetime_tests.cpp)

if(MSVC)
    target_compile_options(xfrontend_tests PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(xfrontend_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME xfrontend_tests COMMAND xfrontend_tests)

add_executable(emit_elf64_shared_smoke tools/emit_elf64_shared_smoke.cpp)
target_link_libraries(emit_elf64_shared_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_elf64_shared_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_elf64_shared_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_elf64_dynamic_import_smoke tools/emit_elf64_dynamic_import_smoke.cpp)
target_link_libraries(emit_elf64_dynamic_import_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_elf64_dynamic_import_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_elf64_dynamic_import_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_elf64_plt_smoke tools/emit_elf64_plt_smoke.cpp)
target_link_libraries(emit_elf64_plt_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_elf64_plt_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_elf64_plt_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_pe64_smoke tools/emit_pe64_smoke.cpp)
target_link_libraries(emit_pe64_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_pe64_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_pe64_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_archive_smoke tools/emit_archive_smoke.cpp)
target_link_libraries(emit_archive_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_archive_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_archive_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_coff_import_smoke tools/emit_coff_import_smoke.cpp)
target_link_libraries(emit_coff_import_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_coff_import_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_coff_import_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_pe64_export_import_smoke tools/emit_pe64_export_import_smoke.cpp)
target_link_libraries(emit_pe64_export_import_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_pe64_export_import_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_pe64_export_import_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_pe64_dll_smoke tools/emit_pe64_dll_smoke.cpp)
target_link_libraries(emit_pe64_dll_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_pe64_dll_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_pe64_dll_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()


add_executable(emit_pe64_dll_reloc_smoke tools/emit_pe64_dll_reloc_smoke.cpp)
target_link_libraries(emit_pe64_dll_reloc_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_pe64_dll_reloc_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_pe64_dll_reloc_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_pe64_dll_tls_smoke tools/emit_pe64_dll_tls_smoke.cpp)
target_link_libraries(emit_pe64_dll_tls_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_pe64_dll_tls_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_pe64_dll_tls_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_pe64_n39_n43_smoke tools/emit_pe64_n39_n43_smoke.cpp)
target_link_libraries(emit_pe64_n39_n43_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_pe64_n39_n43_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_pe64_n39_n43_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(emit_pe64_n44_smoke tools/emit_pe64_n44_smoke.cpp)
target_link_libraries(emit_pe64_n44_smoke PRIVATE xfrontend)
if(MSVC)
    target_compile_options(emit_pe64_n44_smoke PRIVATE /W4 /WX /utf-8 /permissive-)
else()
    target_compile_options(emit_pe64_n44_smoke PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
