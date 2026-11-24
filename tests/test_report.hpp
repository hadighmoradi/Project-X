// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef XFRONTEND_TEST_INVENTORY_TOTAL
#define XFRONTEND_TEST_INVENTORY_TOTAL 0
#endif

namespace xfrontend_test_report {

struct TestRecord {
    std::string name;
    std::string status;
    std::string message;
    double duration_ms = 0.0;
};

inline std::vector<TestRecord>& records() {
    static std::vector<TestRecord> value;
    return value;
}

inline std::string env_or(const char* name, const std::string& fallback);

inline int& skipped_records() {
    static int value = 0;
    return value;
}

inline bool should_run(const std::string& name) {
    const std::string filter = env_or("X_TEST_FILTER", "");
    if (filter.empty() || filter == "*" || filter == "all" || filter == "ALL") return true;

    std::size_t begin = 0U;
    while (begin <= filter.size()) {
        const std::size_t end = filter.find(';', begin);
        const std::string prefix = filter.substr(begin,
            end == std::string::npos ? std::string::npos : end - begin);
        if (prefix == "*" || prefix == "all" || prefix == "ALL" ||
            (!prefix.empty() && name.compare(0U, prefix.size(), prefix) == 0)) {
            return true;
        }
        if (end == std::string::npos) break;
        begin = end + 1U;
    }
    ++skipped_records();
    return false;
}

inline void record(const std::string& name, bool passed,
                   const std::string& message, double duration_ms) {
    records().push_back(TestRecord{name, passed ? "PASS" : "FAIL", message, duration_ms});
}

inline std::string env_or(const char* name, const std::string& fallback) {
#if defined(_MSC_VER)
    // MSVC deprecates getenv and /WX promotes that warning to an error.
    // _dupenv_s returns an allocated copy that must be released with free().
    char* value = nullptr;
    std::size_t value_size = 0;
    const errno_t result = ::_dupenv_s(&value, &value_size, name);
    if (result != 0 || value == nullptr) {
        std::free(value);
        return fallback;
    }
    const std::string copied(value);
    std::free(value);
    return copied.empty() ? fallback : copied;
#else
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
#endif
}

inline std::string json_escape(const std::string& input) {
    std::ostringstream out;
    for (unsigned char ch : input) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20U) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(ch) << std::dec;
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    return out.str();
}

inline std::string category_of(const std::string& name) {
    if (name.find("_absolute_relocation_layout") != std::string::npos) return "Absolute relocation application matrix";
    if (name.find("_pc_relative_formula") != std::string::npos) return "PC-relative relocation formula matrix";
    if (name.find("_archive_dependency_chain") != std::string::npos) return "Static archive dependency closure matrix";
    if (name.find("_linker_rejection_matrix") != std::string::npos) return "Linker validation and overflow matrix";
    if (name.find("_elf64_header_program_table") != std::string::npos) return "ELF64 executable header and program table matrix";
    if (name.find("_segment_permissions_bss") != std::string::npos) return "Executable segment permission and BSS matrix";
    if (name.find("_direct_startup_contract") != std::string::npos) return "Direct process startup contract matrix";
    if (name.find("_executable_rejection_matrix") != std::string::npos) return "Executable image rejection matrix";
    if (name.find("_et_dyn_header_program_loads") != std::string::npos) return "ELF64 ET_DYN header and load segment matrix";
    if (name.find("_dynamic_symbols_hash_table") != std::string::npos) return "ELF64 dynamic symbols and SysV hash matrix";
    if (name.find("_relative_rela_serialization") != std::string::npos) return "ELF64 relative RELA relocation matrix";
    if (name.find("_shared_object_rejection_matrix") != std::string::npos) return "ELF64 shared object rejection matrix";
    if (name.find("_needed_import_dynsym_hash") != std::string::npos) return "ELF64 DT_NEEDED and imported dynsym matrix";
    if (name.find("_got_glob_dat_relocations") != std::string::npos) return "ELF64 GOT and GLOB_DAT relocation matrix";
    if (name.find("_rip_relative_got_patch") != std::string::npos) return "ELF64 RIP-relative GOT patch matrix";
    if (name.find("_dynamic_import_rejection_matrix") != std::string::npos) return "ELF64 dynamic import rejection matrix";
    if (name.find("N31A") == 0U) return "GNU ar archive container matrix";
    if (name.find("N31B") == 0U) return "COFF LIB archive index matrix";
    if (name.find("N31C") == 0U) return "Object archive binary round-trip matrix";
    if (name.find("N31D") == 0U) return "System linker response plan matrix";
    if (name.find("N39A") == 0U) return "AMD64 exception/termination handler metadata";
    if (name.find("N39B") == 0U) return "AMD64 chained runtime-function metadata";
    if (name.find("N39C") == 0U) return "AMD64 extended unwind opcode encoding";
    if (name.find("N39D") == 0U) return "AMD64 unwind metadata rejection";
    if (name.find("N40A") == 0U) return "PE resource tree layout";
    if (name.find("N40B") == 0U) return "PE resource deterministic stress";
    if (name.find("N40C") == 0U) return "PE resource validation";
    if (name.find("N40D") == 0U) return "PE resource directory integration";
    if (name.find("N41A") == 0U) return "PE64 load configuration and GuardCF";
    if (name.find("N41B") == 0U) return "PE64 GuardCF base relocation integration";
    if (name.find("N41C") == 0U) return "PE64 load configuration validation";
    if (name.find("N41D") == 0U) return "PE64 minimal load configuration";
    if (name.find("N42A") == 0U) return "PE64 delay and bound import layout";
    if (name.find("N42B") == 0U) return "PE64 delay import validation";
    if (name.find("N42C") == 0U) return "PE64 bound import validation";
    if (name.find("N42D") == 0U) return "PE64 delay import integration";
    if (name.find("N43A") == 0U) return "COFF object-to-PE DLL pipeline";
    if (name.find("N43B") == 0U) return "COFF archive extraction-to-PE pipeline";
    if (name.find("N43C") == 0U) return "COFF binary object round-trip link pipeline";
    if (name.find("N43D") == 0U) return "COFF link failure validation";
    if (name.find("N47A") == 0U) return "X source declarations to independently collectible function sections";
    if (name.find("N47B") == 0U) return "X Math archive extraction and per-function COMDAT elimination";
    if (name.find("N47C") == 0U) return "X source global data sections and BSS reachability";
    if (name.find("N47D") == 0U) return "X native object emitter invalid artifact rejection";
    if (name.find("N46A") == 0U) return "Fine-grained reachability and transitive function retention";
    if (name.find("N46B") == 0U) return "Static archive extraction and unused section elimination";
    if (name.find("N46C") == 0U) return "Explicit roots, metadata and side-effect section preservation";
    if (name.find("N46D") == 0U) return "Conservative fallback and optimization accounting";
    if (name.find("N44A") == 0U) return "PE64 CodeView RSDS Debug Directory round-trip";
    if (name.find("N44B") == 0U) return "PE64 CodeView input validation and rejection";
    if (name.find("N44C") == 0U) return "PE64 CodeView corruption and pointer validation";
    if (name.find("N44D") == 0U) return "PE64 Authenticode Certificate Table structure";
    if (name.find("N33A") == 0U) return "PE32+ Export Directory round-trip matrix";
    if (name.find("N33B") == 0U) return "PE32+ export name, alias and ordinal mapping matrix";
    if (name.find("N33C") == 0U) return "PE32+ export-to-COFF import-library matrix";
    if (name.find("N33D") == 0U) return "PE32+ Export Directory malformed-image rejection matrix";
    if (name.find("N29A") == 0U) return "PE32+ headers and section layout matrix";
    if (name.find("N29B") == 0U) return "PE32+ Import Directory and IAT matrix";
    if (name.find("N29C") == 0U) return "PE32+ DIR64 base relocation matrix";
    if (name.find("N29D") == 0U) return "PE32+ executable rejection matrix";
    if (name.find("_binary_writer_") != std::string::npos) return "Binary object writer format matrix";
    if (name.find("_binary_reader_roundtrip_") != std::string::npos) return "Binary object reader round-trip matrix";
    if (name.find("_binary_relocation_addend_") != std::string::npos) return "Binary object relocation/addend matrix";
    if (name.find("_binary_reader_corruption_") != std::string::npos) return "Binary object malformed-input matrix";
    if (name.find("_object_descriptor_sections_") != std::string::npos) return "Object file descriptor and sections matrix";
    if (name.find("_object_symbol_relocation_") != std::string::npos) return "Object symbol and relocation validation matrix";
    if (name.find("_archive_extraction_resolution_") != std::string::npos) return "Static archive extraction and resolution matrix";
    if (name.find("_object_link_relocation_") != std::string::npos) return "Object link and relocation planning matrix";
    if (name.find("_symbol_mangling_") != std::string::npos) return "Native symbol mangling matrix";
    if (name.find("_symbol_table_matrix_") != std::string::npos) return "Native symbol table and collision matrix";
    if (name.find("_link_resolution_matrix_") != std::string::npos) return "Native linker resolution matrix";
    if (name.find("_link_abi_integration_") != std::string::npos) return "Native linker ABI integration matrix";
    if (name.find("_target_profile_") != std::string::npos) return "Target ABI descriptor matrix";
    if (name.find("_argument_classification_") != std::string::npos) return "Target ABI argument classification matrix";
    if (name.find("_register_stack_layout_") != std::string::npos) return "Target ABI register and stack layout matrix";
    if (name.find("_drop_target_lowering_") != std::string::npos) return "Drop target ABI integration matrix";
    if (name.find("_symbol_identity_") != std::string::npos) return "Drop ABI symbol identity matrix";
    if (name.find("_destructor_validation_") != std::string::npos) return "Drop ABI destructor validation matrix";
    if (name.find("_deleter_signature_") != std::string::npos) return "Drop ABI deleter signature matrix";
    if (name.find("_abi_layout_") != std::string::npos) return "Drop ABI binding layout matrix";
    if (name.find("_contract_identity_") != std::string::npos) return "Drop contract identity matrix";
    if (name.find("_contract_validation_") != std::string::npos) return "Drop contract validation matrix";
    if (name.find("_failure_policy_") != std::string::npos) return "Drop failure policy matrix";
    if (name.find("_invocation_order_") != std::string::npos) return "Drop invocation ordering matrix";
    if (name.find("_init_plan_") != std::string::npos) return "Ownership lowering initialization matrix";
    if (name.find("_reset_plan_") != std::string::npos) return "Ownership lowering reset matrix";
    if (name.find("_take_plan_") != std::string::npos) return "Ownership lowering transfer matrix";
    if (name.find("_cleanup_plan_") != std::string::npos) return "Ownership lowering cleanup matrix";
    if (name.find("_move_copy_matrix_") != std::string::npos) return "Unique copy-initialization matrix";
    if (name.find("_move_assignment_matrix_") != std::string::npos) return "Unique assignment/transfer matrix";
    if (name.find("_copy_parameter_matrix_") != std::string::npos) return "Unique parameter copyability matrix";
    if (name.find("_take_transfer_matrix_") != std::string::npos) return "Take transfer lifetime matrix";
    if (name.find("_cfg_ownership_matrix_") != std::string::npos) return "CFG ownership event matrix";
    if (name.find("_unique_stale_") != std::string::npos ||
        name.find("_casefold_owner_") != std::string::npos) return "Ownership stale matrix";
    if (name.find("_ownership_recovery_") != std::string::npos) return "Ownership recovery matrix";
    if (name.find("_borrow_matrix_") != std::string::npos) return "Borrow conflict matrix";
    if (name.find("_flow_scope_matrix_") != std::string::npos) return "Flow and scope matrix";
    if (name.find("_generation_matrix_") != std::string::npos) return "Resource generation matrix";
    if (name.find("_move_join_") != std::string::npos) return "CFG ownership join matrix";
    if (name.find("_transfer_state_") != std::string::npos) return "Move transfer state matrix";
    if (name.find("_scope_drop_") != std::string::npos) return "Lexical scope drop matrix";
    if (name.find("_loop_cleanup_") != std::string::npos) return "Loop cleanup matrix";
    if (name.find("_return_drop_") != std::string::npos) return "Return cleanup matrix";
    if (name.find("_transfer_generation_matrix_") != std::string::npos) return "Transfer generation deep matrix";
    if (name.find("_flow_join_matrix_") != std::string::npos) return "Ownership join deep matrix";
    if (name.find("_scope_order_matrix_") != std::string::npos) return "Nested scope order deep matrix";
    if (name.find("_loop_owner_matrix_") != std::string::npos) return "Loop cleanup deep matrix";
    if (name.find("_cfg_integrity_matrix_") != std::string::npos) return "CFG lifecycle integrity deep matrix";
    if (name.find("_lifecycle_matrix_") != std::string::npos) return "Ownership lifecycle acceptance matrix";
    if (name.find("_take_chain_matrix_") != std::string::npos) return "Multi-hop Take transfer acceptance matrix";
    if (name.find("_branch_join_matrix_") != std::string::npos) return "Branch ownership lattice acceptance matrix";
    if (name.find("_return_cleanup_matrix_") != std::string::npos) return "Return cleanup acceptance matrix";
    if (name.find("_loop_owner_matrix_") != std::string::npos) return "Loop owner acceptance matrix";
    std::size_t index = 0;
    while (index < name.size() && ((name[index] >= 'A' && name[index] <= 'Z') ||
           (name[index] >= 'a' && name[index] <= 'z'))) ++index;
    if (index == 0U) return "Other";
    const std::string prefix = name.substr(0U, index);
    return prefix == "T" ? "Lifetime core" : prefix;
}

inline void write_reports(const std::string& directory, int passed, int failed) {
    std::filesystem::create_directories(directory);
    const std::filesystem::path json_path = std::filesystem::path(directory) / "test-report.json";
    const std::filesystem::path text_path = std::filesystem::path(directory) / "test-report.txt";

    std::map<std::string, int> category_passed;
    std::map<std::string, int> category_failed;
    for (const auto& item : records()) {
        const std::string category = category_of(item.name);
        if (item.status == "PASS") ++category_passed[category];
        else ++category_failed[category];
    }

    const std::string compiler = env_or("X_TEST_COMPILER", "unknown");
    const std::string compiler_family = env_or("X_TEST_COMPILER_FAMILY", "unknown");
    const std::string compiler_version = env_or("X_TEST_COMPILER_VERSION", "not-reported");
    const std::string platform = env_or("X_TEST_PLATFORM", "unknown");
    const std::string configuration = env_or("X_TEST_CONFIGURATION", "C++17 strict-warnings");
    const std::string selection_mode = env_or("X_TEST_MODE", "Full");
    const std::string selection_suite = env_or("X_TEST_SUITE", "All");
    const std::string selection_filter = env_or("X_TEST_FILTER", "*");
    const int executed = passed + failed;
    const int skipped = skipped_records();
    const int discovered = executed + skipped;
    const int project_inventory_total = XFRONTEND_TEST_INVENTORY_TOTAL > 0
        ? XFRONTEND_TEST_INVENTORY_TOTAL : discovered;
    const int project_not_run = project_inventory_total > executed
        ? project_inventory_total - executed : 0;
    std::set<std::string> unique_names;
    for (const auto& item : records()) unique_names.insert(item.name);
    const int unique_ids = static_cast<int>(unique_names.size());
    const int duplicate_ids = executed - unique_ids;

    std::ofstream json(json_path, std::ios::binary | std::ios::trunc);
    if (!json) throw std::runtime_error("Cannot write JSON test report: " + json_path.string());
    {
        json << "{\n"
             << "  \"schema_version\": \"1.1\",\n"
             << "  \"tool\": \"X Native Frontend Test Runner\",\n"
             << "  \"compiler\": \"" << json_escape(compiler) << "\",\n"
             << "  \"compiler_family\": \"" << json_escape(compiler_family) << "\",\n"
             << "  \"compiler_version\": \"" << json_escape(compiler_version) << "\",\n"
             << "  \"platform\": \"" << json_escape(platform) << "\",\n"
             << "  \"configuration\": \"" << json_escape(configuration) << "\",\n"
             << "  \"selection\": {\n"
             << "    \"mode\": \"" << json_escape(selection_mode) << "\",\n"
             << "    \"suite\": \"" << json_escape(selection_suite) << "\",\n"
             << "    \"filter\": \"" << json_escape(selection_filter) << "\",\n"
             << "    \"executed\": " << executed << ",\n"
             << "    \"skipped\": " << skipped << ",\n"
             << "    \"discovered\": " << discovered << ",\n"
             << "    \"project_inventory_total\": " << project_inventory_total << ",\n"
             << "    \"project_not_run\": " << project_not_run << ",\n"
             << "    \"unique_ids\": " << unique_ids << ",\n"
             << "    \"duplicate_ids\": " << duplicate_ids << "\n"
             << "  },\n"
             << "  \"summary\": {\n"
             << "    \"passed\": " << passed << ",\n"
             << "    \"failed\": " << failed << ",\n"
             << "    \"total\": " << (passed + failed) << ",\n"
             << "    \"status\": \"" << (executed > 0 && failed == 0 ? "passed" : "failed") << "\"\n"
             << "  },\n"
             << "  \"categories\": [\n";
        bool first = true;
        for (const auto& item : category_passed) {
            if (!first) json << ",\n";
            first = false;
            const int count_failed = category_failed[item.first];
            json << "    {\"name\":\"" << json_escape(item.first)
                 << "\",\"passed\":" << item.second
                 << ",\"failed\":" << count_failed
                 << ",\"total\":" << (item.second + count_failed) << "}";
        }
        for (const auto& item : category_failed) {
            if (category_passed.find(item.first) != category_passed.end()) continue;
            if (!first) json << ",\n";
            first = false;
            json << "    {\"name\":\"" << json_escape(item.first)
                 << "\",\"passed\":0,\"failed\":" << item.second
                 << ",\"total\":" << item.second << "}";
        }
        json << "\n  ],\n  \"tests\": [\n";
        for (std::size_t i = 0; i < records().size(); ++i) {
            const auto& item = records()[i];
            json << "    {\"id\":\"" << json_escape(item.name)
                 << "\",\"category\":\"" << json_escape(category_of(item.name))
                 << "\",\"status\":\"" << item.status
                 << "\",\"duration_ms\":" << std::fixed << std::setprecision(3) << item.duration_ms
                 << ",\"message\":\"" << json_escape(item.message) << "\"}"
                 << (i + 1U == records().size() ? "\n" : ",\n");
        }
        json << "  ]\n}\n";
    }

    std::ofstream text(text_path, std::ios::binary | std::ios::trunc);
    if (!text) throw std::runtime_error("Cannot write text test report: " + text_path.string());
    {
        text << "X Native Frontend Test Report\n"
             << "Compiler: " << compiler << " (" << compiler_family << ")\n"
             << "Compiler version: " << compiler_version << "\n"
             << "Platform: " << platform << "\n"
             << "Configuration: " << configuration << "\n"
             << "Selection mode: " << selection_mode << "\n"
             << "Selection suite: " << selection_suite << "\n"
             << "Selection filter: " << selection_filter << "\n"
             << "Result: " << (executed > 0 && failed == 0 ? "PASSED" : "FAILED") << "\n"
             << "Passed: " << passed << "\n"
             << "Failed: " << failed << "\n"
             << "Total: " << executed << "\n"
             << "Executed: " << executed << "\n"
             << "Skipped: " << skipped << "\n"
             << "Discovered in selected executable: " << discovered << "\n"
             << "Project regression inventory: " << project_inventory_total << "\n"
             << "Project tests not run in this pass: " << project_not_run << "\n"
             << "Unique IDs: " << unique_ids << "\n"
             << "Duplicate IDs: " << duplicate_ids << "\n\n"
             << "Category summary (executed tests only):\n";
        for (const auto& item : category_passed) {
            const int count_failed = category_failed[item.first];
            text << "  " << item.first << ": " << item.second << " passed, "
                 << count_failed << " failed, " << (item.second + count_failed) << " total\n";
        }
        for (const auto& item : category_failed) {
            if (category_passed.find(item.first) == category_passed.end()) {
                text << "  " << item.first << ": 0 passed, " << item.second
                     << " failed, " << item.second << " total\n";
            }
        }
        text << "\nFailed tests:\n";
        bool any_failed = false;
        for (const auto& item : records()) {
            if (item.status != "FAIL") continue;
            any_failed = true;
            text << "  FAIL " << item.name << ": " << item.message << '\n';
        }
        if (!any_failed) text << "  None\n";
        text << "\nAll test cases:\n";
        for (const auto& item : records()) {
            text << "  " << item.status << " " << item.name;
            if (!item.message.empty()) text << " -- " << item.message;
            text << '\n';
        }
    }
}

} // namespace xfrontend_test_report
