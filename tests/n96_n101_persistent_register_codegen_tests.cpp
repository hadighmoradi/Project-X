#include "xfrontend/x64_codegen.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void run_case(const std::string& id, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(id)) return;
    const auto started = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(id, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(id, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    }
}

std::string gp_pressure_source() {
    return "module N96N101; "
           "private function Helper(int input) returns int { return input + 3; } "
           "private function Unused() returns int { return 99; } "
           "public function Main() returns int { "
           "int a = 1; int b = 2; int c = 3; int d = 4; "
           "int e = 5; int f = 6; int g = 7; int h = 11; "
           "int result = Helper(a + b + c + d); "
           "return result + e + f + g + h; }";
}

std::string xmm_pressure_source() {
    return "module N96Xmm; public function Main() returns double { "
           "double a = 1.5; double b = 2.5; double c = 3.5; double d = 4.5; "
           "return a + b + c + d; }";
}

std::string address_taken_source() {
    return "module N96Address; public function Main() returns int { "
           "int value = 5; int* pointer = &value; value = 42; return *pointer; }";
}

X64CodegenResult compile(const std::string& source, const std::string& id) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N96-N101 source did not parse");
    auto result = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{id, make_coff_x86_64_object_target()});
    if (!result.succeeded()) {
        const auto& diagnostic = result.diagnostics.front();
        throw std::runtime_error("N96-N101 codegen failed: " + diagnostic.code + " " + diagnostic.message);
    }
    return result;
}

void test_n96_gp_source_values_receive_nonvolatile_registers() {
    const auto result = compile(gp_pressure_source(), "n96-gp-register-residency");
    require(result.stats.native_persistent_gp_register_assignments > 0U,
            "N96 did not assign persistent GP source values to nonvolatile registers");
    require(result.stats.native_persistent_register_reads > 0U &&
            result.stats.native_persistent_register_writes > 0U,
            "N96 persistent GP registers were planned but not consumed by emitted loads/stores");
}

void test_n97_float_source_values_receive_nonvolatile_xmm_registers() {
    const auto result = compile(xmm_pressure_source(), "n97-xmm-register-residency");
    require(result.stats.native_persistent_xmm_register_assignments > 0U,
            "N97 did not assign persistent floating values to nonvolatile XMM registers");
    require(result.stats.native_persistent_register_reads > 0U &&
            result.stats.native_persistent_register_writes > 0U,
            "N97 XMM assignments were not consumed by emitted loads/stores");
}

void test_n98_persistent_registers_keep_canonical_home_mirrors() {
    const auto result = compile(gp_pressure_source(), "n98-register-home-mirrors");
    require(result.stats.native_persistent_homes_mapped >= 9U,
            "N98 did not preserve canonical stack homes for source variables");
    require(result.stats.native_persistent_register_writes >= 8U,
            "N98 source writes were not mirrored into allocated persistent registers");
    require(result.stats.native_persistent_register_reads >= 4U,
            "N98 source reads did not use the persistent register map");
}

void test_n99_call_crossing_values_use_win64_nonvolatile_registers_or_homes() {
    const auto result = compile(gp_pressure_source(), "n99-call-crossing-register-values");
    require(result.stats.native_call_crossing_values_homed > 0U,
            "N99 CFG liveness did not identify values crossing a call");
    require(result.stats.native_persistent_gp_register_assignments > 0U,
            "N99 call-crossing source values received no persistent register allocations");
    require(result.stats.native_persistent_register_stack_fallbacks > 0U,
            "N99 register pressure did not retain safe canonical-home fallback values");
}

void test_n100_address_taken_values_stay_memory_resident_and_unwind_saves_registers() {
    const auto addressed = compile(address_taken_source(), "n100-address-taken-memory-residency");
    require(addressed.stats.native_address_taken_variables_kept_in_memory >= 1U,
            "N100 address-taken variable was incorrectly promoted to a register-only location");
    const auto registers = compile(gp_pressure_source(), "n100-win64-save-unwind-gp");
    require(registers.stats.callee_saved_gp_registers > 0U,
            "N100 persistent GP allocation did not request ABI-preserved registers");
    require(registers.stats.win64_unwind_records_emitted == 3U,
            "N100 save/restore prologue did not emit unwind information for all functions");
    const auto xmm = compile(xmm_pressure_source(), "n100-win64-save-unwind-xmm");
    require(xmm.stats.native_persistent_xmm_register_assignments > 0U &&
            xmm.stats.callee_saved_xmm_registers > 0U,
            "N100 persistent XMM allocation did not preserve nonvolatile XMM registers");
    require(xmm.stats.win64_unwind_records_emitted == 1U,
            "N100 XMM save/restore prologue did not emit its UNWIND_INFO record");
}

void test_n101_native_object_contains_multiblock_unwind_records() {
    const auto result = compile(gp_pressure_source(), "n101-native-object-pressure-smoke");
    std::size_t text_sections = 0U;
    std::size_t pdata_sections = 0U;
    std::size_t xdata_sections = 0U;
    std::size_t addr32nb = 0U;
    for (const auto& section : result.native_object.object_file.sections) {
        if (section.kind == ObjectSectionKind::Text) ++text_sections;
        if (section.name.rfind(".pdata$", 0U) == 0U) ++pdata_sections;
        if (section.name.rfind(".xdata$", 0U) == 0U) ++xdata_sections;
    }
    for (const auto& relocation : result.native_object.object_file.relocations)
        if (relocation.kind == ObjectRelocationKind::CoffAddr32NB) ++addr32nb;
    require(text_sections == 3U && pdata_sections == 3U && xdata_sections == 3U,
            "N101 COFF output is missing text or associative unwind sections");
    require(addr32nb == 9U && !result.object_bytes.empty(),
            "N101 COFF unwind relocations or object bytes are incomplete");
    require(result.stats.native_persistent_gp_register_assignments > 0U &&
            result.stats.native_persistent_register_reads > 0U,
            "N101 machine object does not reflect persistent GP register residency");
}

} // namespace

void run_n96_n101_persistent_register_codegen_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 2000U; ++index) {
        run_case("N96A_gp_persistent_register_allocation_" + std::to_string(index),
            [] { test_n96_gp_source_values_receive_nonvolatile_registers(); }, passed, failed);
        run_case("N97A_xmm_persistent_register_allocation_" + std::to_string(index),
            [] { test_n97_float_source_values_receive_nonvolatile_xmm_registers(); }, passed, failed);
        run_case("N98A_persistent_register_home_mirroring_" + std::to_string(index),
            [] { test_n98_persistent_registers_keep_canonical_home_mirrors(); }, passed, failed);
        run_case("N99A_call_boundary_register_preservation_" + std::to_string(index),
            [] { test_n99_call_crossing_values_use_win64_nonvolatile_registers_or_homes(); }, passed, failed);
        run_case("N100A_address_taken_and_unwind_safety_" + std::to_string(index),
            [] { test_n100_address_taken_values_stay_memory_resident_and_unwind_saves_registers(); }, passed, failed);
        run_case("N101A_persistent_register_native_object_" + std::to_string(index),
            [] { test_n101_native_object_contains_multiblock_unwind_records(); }, passed, failed);
    }
}
