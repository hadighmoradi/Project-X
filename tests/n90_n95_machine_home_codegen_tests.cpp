#include "xfrontend/native_cfg_bridge.hpp"
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

std::string machine_home_source(std::size_t index) {
    const unsigned bump = 2U + static_cast<unsigned>(index % 11U);
    return "module N90N95; "
           "private function Helper(int input) returns int { return input + " +
           std::to_string(bump) + "; } "
           "private function Unused() returns int { return 99; } "
           "public function Main() returns int { "
           "int result = 0; int i = 0; int branch_value = 0; "
           "while (i < 4) { "
           "if (i == 1) { branch_value = Helper(i); result = result + branch_value; } "
           "else { branch_value = i + 2; result = result + branch_value; } "
           "i = i + 1; } "
           "if (result > 0) { return 42; } else { return 1; } }";
}

ParseResult parse_sample(std::size_t index) {
    auto parsed = parse_source(machine_home_source(index));
    require(parsed.succeeded(), "N90-N95 machine-home sample failed to parse");
    return parsed;
}

const NativeRegisterFlowFunction& find_main_flow(const NativeRegisterFlowLoweringResult& lowered) {
    const auto found = std::find_if(lowered.functions.begin(), lowered.functions.end(),
        [](const NativeRegisterFlowFunction& function) { return function.function_name == "Main"; });
    if (found == lowered.functions.end()) throw std::runtime_error("Main CFG function was not lowered");
    return *found;
}

X64CodegenResult compile_sample(std::size_t index) {
    auto parsed = parse_sample(index);
    auto compiled = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n90-n95-machine-homes", make_coff_x86_64_object_target()});
    if (!compiled.succeeded()) {
        const auto& diagnostic = compiled.diagnostics.front();
        throw std::runtime_error("N90-N95 codegen failed: " + diagnostic.code + " " + diagnostic.message);
    }
    return compiled;
}

void test_n90_every_flow_binding_has_stable_home(std::size_t index) {
    auto parsed = parse_sample(index);
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded && lowered.functions.size() == 3U,
            "N90 AST/CFG bridge did not provide all three functions");
    const auto& main = find_main_flow(lowered);
    const auto homes = build_native_register_variable_home_plan(main);
    require(homes.succeeded, "N90 variable-home planning failed");
    require(homes.values_homed == main.bindings.size() &&
            homes.home_slot_count == main.bindings.size(),
            "N90 did not assign one deterministic canonical home to every scalar binding");
    std::vector<std::uint32_t> value_ids;
    std::vector<std::uint32_t> slots;
    for (const auto& home : homes.homes) {
        require(home.value_id != 0U && home.flow_variable_id != InvalidFlowVariableId,
                "N90 emitted an invalid flow-to-native home binding");
        value_ids.push_back(home.value_id);
        slots.push_back(home.home_slot);
    }
    std::sort(value_ids.begin(), value_ids.end());
    std::sort(slots.begin(), slots.end());
    require(std::adjacent_find(value_ids.begin(), value_ids.end()) == value_ids.end(),
            "N90 assigned one native value to multiple home bindings");
    require(std::adjacent_find(slots.begin(), slots.end()) == slots.end(),
            "N90 canonical home slots are not unique");
}

void test_n91_live_in_phi_nodes_cover_every_predecessor(std::size_t index) {
    auto parsed = parse_sample(index);
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded, "N91 frontend CFG lowering failed");
    const auto& main = find_main_flow(lowered);
    const auto allocation = analyze_and_allocate_native_flow_function(main);
    require(allocation.succeeded, "N91 CFG liveness/allocation failed");
    const auto homes = build_native_register_variable_home_plan(main);
    require(homes.succeeded, "N91 home plan failed");
    const auto phis = build_native_register_home_phi_plan(main, allocation.liveness, homes);
    require(phis.succeeded, "N91 memory-SSA Phi construction failed");
    require(phis.phi_count > 0U && phis.incoming_edges >= phis.phi_count * 2U,
            "N91 no multi-predecessor live-in Phi nodes were generated");
    for (const auto& phi : phis.phis) {
        require(phi.incoming.size() >= 2U,
                "N91 Phi does not provide one incoming for each multi-predecessor join");
        std::vector<std::uint32_t> versions;
        for (const auto& incoming : phi.incoming) versions.push_back(incoming.source_value_id);
        std::sort(versions.begin(), versions.end());
        require(std::adjacent_find(versions.begin(), versions.end()) == versions.end(),
                "N91 predecessor edges do not carry distinct reaching-value versions");
    }
}

void test_n92_codegen_consumes_source_variable_homes(std::size_t index) {
    const auto compiled = compile_sample(index);
    require(compiled.stats.native_persistent_homes_mapped >= 4U,
            "N92 x64 Codegen did not bind all scalar locals and parameters to stack homes");
    require(compiled.stats.native_cfg_blocks_analyzed >= 10U,
            "N92 Codegen did not retain the real multi-block CFG analysis");
}

void test_n93_phi_edge_transfers_are_coalesced_to_physical_homes(std::size_t index) {
    const auto compiled = compile_sample(index);
    require(compiled.stats.native_home_phi_nodes > 0U,
            "N93 Codegen did not construct memory-SSA Phi nodes");
    require(compiled.stats.native_home_phi_incoming_edges >= compiled.stats.native_home_phi_nodes * 2U,
            "N93 Phi incoming edges were not fully represented");
    require(compiled.stats.native_home_phi_edges_coalesced ==
                compiled.stats.native_home_phi_incoming_edges,
            "N93 not all Phi incoming edges were coalesced to canonical homes");
    require(compiled.stats.native_home_phi_machine_copies == 0U,
            "N93 emitted an unnecessary edge move despite identical canonical homes");
    require(compiled.stats.native_call_crossing_values_homed > 0U,
            "N93 call-crossing persistent values were not kept in canonical stack homes");
}

void test_n94_frame_and_unwind_cover_homed_variables(std::size_t index) {
    const auto compiled = compile_sample(index);
    require(compiled.stats.win64_unwind_records_emitted == 3U,
            "N94 did not emit one unwind record per defined function");
    require(compiled.stats.stack_frame_bytes >= 32U &&
            (compiled.stats.stack_frame_bytes % 16U) == 0U,
            "N94 home slots were not included in an aligned Win64 frame");
    require(compiled.stats.native_persistent_homes_mapped >= 4U,
            "N94 persistent source homes were omitted from unwind-covered frame planning");
}

void test_n95_multiblock_coff_contains_unwind_and_branch_metadata(std::size_t index) {
    const auto compiled = compile_sample(index);
    std::size_t text_sections = 0U;
    std::size_t pdata_sections = 0U;
    std::size_t xdata_sections = 0U;
    std::size_t addr32nb = 0U;
    for (const auto& section : compiled.native_object.object_file.sections) {
        if (section.kind == ObjectSectionKind::Text) ++text_sections;
        if (section.name.rfind(".pdata$", 0U) == 0U) ++pdata_sections;
        if (section.name.rfind(".xdata$", 0U) == 0U) ++xdata_sections;
    }
    for (const auto& relocation : compiled.native_object.object_file.relocations) {
        if (relocation.kind == ObjectRelocationKind::CoffAddr32NB) ++addr32nb;
    }
    require(text_sections == 3U && pdata_sections == 3U && xdata_sections == 3U,
            "N95 COFF does not contain text and associative unwind sections for all functions");
    require(addr32nb == 9U, "N95 COFF runtime-function records do not have nine ADDR32NB relocations");
    require(!compiled.object_bytes.empty(), "N95 COFF object output is empty");
    require(compiled.stats.native_home_phi_nodes > 0U,
            "N95 machine code did not consume the memory-SSA Phi home plan");
}

} // namespace

void run_n90_n95_machine_home_codegen_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 2000U; ++index) {
        run_case("N90A_flow_value_home_mapping_" + std::to_string(index),
            [index] { test_n90_every_flow_binding_has_stable_home(index); }, passed, failed);
        run_case("N91A_multi_predecessor_memory_phi_" + std::to_string(index),
            [index] { test_n91_live_in_phi_nodes_cover_every_predecessor(index); }, passed, failed);
        run_case("N92A_x64_codegen_home_consumption_" + std::to_string(index),
            [index] { test_n92_codegen_consumes_source_variable_homes(index); }, passed, failed);
        run_case("N93A_phi_edge_home_coalescing_" + std::to_string(index),
            [index] { test_n93_phi_edge_transfers_are_coalesced_to_physical_homes(index); }, passed, failed);
        run_case("N94A_home_frame_unwind_integration_" + std::to_string(index),
            [index] { test_n94_frame_and_unwind_cover_homed_variables(index); }, passed, failed);
        run_case("N95A_multiblock_native_object_smoke_" + std::to_string(index),
            [index] { test_n95_multiblock_coff_contains_unwind_and_branch_metadata(index); }, passed, failed);
    }
}
