// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/native_register_analysis.hpp"
#include "xfrontend/native_cfg_bridge.hpp"
#include "xfrontend/x64_codegen.hpp"
#include "xfrontend/win64_unwind_codegen.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
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

NativeRegisterFunctionIR branch_cfg() {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    function.values = {
        {10U, NativeRegisterClass::GeneralPurpose},
        {11U, NativeRegisterClass::GeneralPurpose},
        {12U, NativeRegisterClass::GeneralPurpose},
        {13U, NativeRegisterClass::GeneralPurpose},
        {14U, NativeRegisterClass::GeneralPurpose},
        {15U, NativeRegisterClass::GeneralPurpose}
    };
    function.blocks = {
        {0U, {{{10U}, {}, false}}, {1U, 2U}, {}},
        {1U, {{{11U}, {}, false}, {{}, {10U, 11U}, false}}, {3U}, {}},
        {2U, {{{12U}, {}, false}, {{}, {10U, 12U}, false}}, {3U}, {}},
        {3U, {{{13U}, {}, false}, {{}, {10U, 13U}, false}}, {4U}, {}},
        {4U, {{{14U}, {}, false}, {{}, {10U, 13U, 14U}, false}}, {4U, 5U}, {}},
        {5U, {{{}, {13U}, false}}, {}, {}}
    };
    return function;
}

NativeRegisterAssignment assignment_for(const NativeRegisterAllocationResult& result,
                                        std::uint32_t value_id) {
    const auto found = std::find_if(result.assignments.begin(), result.assignments.end(),
        [value_id](const NativeRegisterAssignment& value) { return value.value_id == value_id; });
    if (found == result.assignments.end()) throw std::runtime_error("allocator omitted a requested value");
    return *found;
}

X64CodegenResult compile_source(const std::string& source, const std::string& module) {
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), module + " source did not parse");
    const auto compiled = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{module, make_coff_x86_64_object_target()});
    if (!compiled.succeeded()) {
        const auto& diagnostic = compiled.diagnostics.front();
        throw std::runtime_error(module + " codegen failed: " + diagnostic.code + " " + diagnostic.message);
    }
    return compiled;
}

void test_n102_cfg_interference_separates_disjoint_branches() {
    const auto function = branch_cfg();
    const auto live = compute_native_register_liveness(function);
    require(live.succeeded && live.converged, "N102 liveness failed to converge on branch CFG");
    const auto graph = build_native_register_interference_graph(function, live);
    require(graph.succeeded, "N102 interference graph construction failed");
    const auto has_edge = [&](std::uint32_t a, std::uint32_t b) {
        return std::any_of(graph.edges.begin(), graph.edges.end(), [&](const auto& edge) {
            return (edge.first_value_id == a && edge.second_value_id == b) ||
                   (edge.first_value_id == b && edge.second_value_id == a);
        });
    };
    require(has_edge(10U, 11U) && has_edge(10U, 12U) && has_edge(10U, 13U),
            "N102 graph omitted values simultaneously live with the branch condition or Phi result");
    require(!has_edge(11U, 12U),
            "N102 graph incorrectly made mutually exclusive branch locals interfere");
}

void test_n103_graph_coloring_reuses_locations_for_phi_inputs() {
    const auto function = branch_cfg();
    const auto live = compute_native_register_liveness(function);
    const auto graph = build_native_register_interference_graph(function, live);
    require(graph.succeeded, "N103 graph construction failed");
    const std::vector<NativeLiveInterval> intervals = {
        {10U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false},
        {11U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false},
        {12U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false},
        {15U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false}
    };
    auto phi_edges = graph.edges;
    phi_edges.push_back({10U, 15U});
    const auto allocated = allocate_native_registers_with_interference(intervals, phi_edges, {});
    require(allocated.succeeded, "N103 graph coloring failed");
    const auto left = assignment_for(allocated, 11U);
    const auto right = assignment_for(allocated, 12U);
    require(!left.spilled && !right.spilled && left.register_index == right.register_index,
            "N103 mutually exclusive Phi inputs did not coalesce to one machine register");

    NativeRegisterFunctionIR phi_function = function;
    phi_function.blocks[3U].predecessors = {1U, 2U};
    NativeRegisterPhiNode phi;
    phi.block_id = 3U;
    phi.destination_value_id = 15U;
    phi.incoming = {{1U, 11U}, {2U, 12U}};
    std::map<std::uint32_t, NativeRegisterLocation> locations;
    for (const auto value_id : {11U, 12U, 15U}) {
        const auto allocated_value = assignment_for(allocated, value_id);
        require(!allocated_value.spilled, "N103 Phi input/result unexpectedly spilled");
        NativeRegisterLocation location;
        location.kind = NativeRegisterLocationKind::Register;
        location.register_index = allocated_value.register_index;
        locations.emplace(value_id, location);
    }
    require(locations.at(11U).register_index == locations.at(15U).register_index,
            "N103 Phi destination was not coalesced with non-interfering branch inputs");
    NativeRegisterLocation temporary;
    temporary.kind = NativeRegisterLocationKind::Temporary;
    temporary.spill_slot = 99U;
    const auto lowered = lower_native_register_phi_edges(phi_function, {phi}, locations, temporary);
    require(lowered.succeeded && lowered.edge_transfers.size() == 2U,
            "N103 Phi-edge lowering failed for coalesced locations");
    require(std::all_of(lowered.edge_transfers.begin(), lowered.edge_transfers.end(),
        [](const NativeRegisterPhiEdgeTransfer& edge) { return edge.operations.empty(); }),
        "N103 coalesced Phi edges emitted redundant machine copies");
}

void test_n104_spill_slots_are_colored_by_cfg_interference() {
    const std::vector<NativeLiveInterval> intervals = {
        {1U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false},
        {2U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false},
        {3U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false}
    };
    NativeRegisterAllocationOptions options;
    options.reserved_gp_registers = {3U, 6U, 7U, 12U, 13U, 14U};
    const std::vector<NativeRegisterInterferenceEdge> edges = {{1U, 2U}, {1U, 3U}};
    const auto allocated = allocate_native_registers_with_interference(intervals, edges, options);
    require(allocated.succeeded, "N104 graph allocator rejected valid branch-exclusive spill graph");
    const auto root = assignment_for(allocated, 1U);
    const auto left = assignment_for(allocated, 2U);
    const auto right = assignment_for(allocated, 3U);
    require(!root.spilled && left.spilled && right.spilled,
            "N104 register pressure did not create the expected two spills");
    require(left.spill_slot == right.spill_slot && allocated.spill_slot_count == 1U,
            "N104 branch-exclusive spilled values did not share a safe spill slot");
}

void test_n105_register_residency_elides_redundant_home_mirror_stores() {
    const std::string source =
        "module N105DirtyHome; public function Main() returns int { "
        "int a = 1; int b = 2; int c = 3; int d = 4; "
        "return a + b + c + d; }";
    const auto result = compile_source(source, "n105-dirty-home");
    require(result.stats.native_persistent_gp_register_assignments > 0U,
            "N105 test source did not receive persistent GP register locations");
    require(result.stats.native_persistent_home_mirror_stores_elided > 0U,
            "N105 register-resident writes did not suppress redundant stack mirror stores");
    const std::string addressed =
        "module N105Address; public function Main() returns int { "
        "int value = 5; int* pointer = &value; value = 42; return *pointer; }";
    const auto addressed_result = compile_source(addressed, "n105-address-taken");
    require(addressed_result.stats.native_address_taken_variables_kept_in_memory >= 1U,
            "N105 address-taken local was incorrectly made register-authoritative");
}

void test_n106_large_frame_probe_and_far_unwind_encoding() {
    const auto probe = build_win64_stack_probe_plan(1024U * 1024U + 128U);
    require(probe.succeeded && probe.probe_offsets.size() == 256U &&
            probe.final_remainder_bytes == 128U,
            "N106 large-frame stack-probe plan has incorrect page touches");

    std::vector<Win64PrologueAction> actions = {
        {Win64PrologueAction::Kind::PushNonvolatileGpr, 1U, 5U, 0U, 0U},
        {Win64PrologueAction::Kind::SetFramePointer, 4U, 5U, 0U, 0U},
        {Win64PrologueAction::Kind::AllocateStack, 11U, 0U, 0U, 1024U * 1024U},
        {Win64PrologueAction::Kind::SaveNonvolatileGpr, 12U, 12U, 1024U * 1024U, 0U},
        {Win64PrologueAction::Kind::SaveNonvolatileXmm, 13U, 6U, 1024U * 1024U, 0U}
    };
    const auto unwind = build_win64_prologue_unwind_plan(0U, 512U, 13U, 5U, 0U, actions);
    require(unwind.succeeded(), "N106 large-frame FAR unwind encoding failed");
    const bool has_gp_far = std::any_of(unwind.runtime_function.unwind_codes.begin(),
        unwind.runtime_function.unwind_codes.end(), [](const Pe64DllUnwindCode& code) {
            return code.operation == Pe64DllUnwindOperation::SaveNonVolFar;
        });
    const bool has_xmm_far = std::any_of(unwind.runtime_function.unwind_codes.begin(),
        unwind.runtime_function.unwind_codes.end(), [](const Pe64DllUnwindCode& code) {
            return code.operation == Pe64DllUnwindOperation::SaveXmm128Far;
        });
    require(has_gp_far && has_xmm_far, "N106 did not select both FAR save opcodes for large offsets");
}

void test_n106_codegen_splices_chkstk_before_stack_allocation() {
    std::string source = "module N106ProbeEmission; public function Main() returns int { ";
    for (std::size_t index = 0U; index < 1024U; ++index) {
        source += "int stack_slot_" + std::to_string(index) + "=" + std::to_string(index) + "; ";
    }
    source += "return 42; }";

    const auto result = compile_source(source, "n106-probe-emission");
    require(result.stats.stack_frame_bytes >= 4096U,
            "N106 generated source did not produce a stack frame at least one page large");
    require(result.stats.win64_stack_probe_functions == 1U,
            "N106 codegen did not record exactly one stack-probed function");

    const auto probe_relocation = std::find_if(result.native_object.object_file.relocations.begin(),
        result.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) {
            return relocation.target_symbol == "__chkstk" &&
                   relocation.kind == ObjectRelocationKind::CoffRelative32 && relocation.width == 4U;
        });
    require(probe_relocation != result.native_object.object_file.relocations.end(),
            "N106 large-frame function has no COFF rel32 relocation to __chkstk");

    const auto text_section = std::find_if(result.native_object.object_file.sections.begin(),
        result.native_object.object_file.sections.end(), [&](const ObjectSection& section) {
            return section.id == probe_relocation->section_id;
        });
    require(text_section != result.native_object.object_file.sections.end() &&
            probe_relocation->offset >= 6U && probe_relocation->offset < text_section->contents.size(),
            "N106 __chkstk relocation does not refer to a valid text-section range");
    require(text_section->contents[probe_relocation->offset - 6U] == 0xB8U &&
            text_section->contents[probe_relocation->offset - 1U] == 0xE8U,
            "N106 prologue does not load EAX then call __chkstk before stack allocation");
    const std::uint32_t immediate = static_cast<std::uint32_t>(text_section->contents[probe_relocation->offset - 5U]) |
        (static_cast<std::uint32_t>(text_section->contents[probe_relocation->offset - 4U]) << 8U) |
        (static_cast<std::uint32_t>(text_section->contents[probe_relocation->offset - 3U]) << 16U) |
        (static_cast<std::uint32_t>(text_section->contents[probe_relocation->offset - 2U]) << 24U);
    require(immediate == result.stats.stack_frame_bytes,
            "N106 __chkstk RAX immediate does not match the final aligned stack frame size");

    const auto undefined_helper = std::find_if(result.native_object.object_file.symbols.begin(),
        result.native_object.object_file.symbols.end(), [](const ObjectSymbol& symbol) {
            return symbol.name == "__chkstk" && !symbol.defined && symbol.kind == ObjectSymbolKind::Function;
        });
    require(undefined_helper != result.native_object.object_file.symbols.end(),
            "N106 object symbol table does not declare __chkstk as an external function");

    const auto unwind_section = std::find_if(result.native_object.object_file.sections.begin(),
        result.native_object.object_file.sections.end(), [](const ObjectSection& section) {
            return section.name.rfind(".xdata$", 0U) == 0U;
        });
    require(unwind_section != result.native_object.object_file.sections.end() &&
            unwind_section->contents.size() >= 6U && unwind_section->contents[0U] == 1U,
            "N106 large-frame function has no valid UNWIND_INFO v1 payload");
    const std::size_t stack_allocation_end = 21U;
    bool unwind_alloc_follows_probe = false;
    const std::size_t unwind_code_count = unwind_section->contents[2U];
    for (std::size_t index = 0U; index < unwind_code_count; ++index) {
        const std::size_t code_offset = unwind_section->contents[4U + index * 2U];
        const std::uint8_t operation = static_cast<std::uint8_t>(unwind_section->contents[5U + index * 2U] & 0x0FU);
        if (code_offset == stack_allocation_end && operation == 1U) { // UWOP_ALLOC_LARGE
            unwind_alloc_follows_probe = true;
            break;
        }
    }
    require(unwind_section->contents[1U] >= stack_allocation_end && unwind_alloc_follows_probe,
            "N106 UNWIND_INFO does not place ALLOC_STACK after the __chkstk call");

    const auto small_frame = compile_source(
        "module N106SmallFrame; public function Main() returns int { return 42; }",
        "n106-small-frame-no-probe");
    const bool small_frame_has_probe = std::any_of(
        small_frame.native_object.object_file.relocations.begin(),
        small_frame.native_object.object_file.relocations.end(), [](const ObjectRelocation& relocation) {
            return relocation.target_symbol == "__chkstk";
        });
    require(small_frame.stats.stack_frame_bytes < 4096U &&
            small_frame.stats.win64_stack_probe_functions == 0U && !small_frame_has_probe,
            "N106 inserted a stack-probe helper reference for a small fixed frame");
}

void test_n107_multiblock_gp_xmm_native_object_stress() {
    const std::string source =
        "module N107Stress; "
        "private function Helper(int p, int q) returns int { return p + q; } "
        "private function FloatHelper(double x, double y) returns double { return x + y; } "
        "private function Unused() returns int { return 77; } "
        "public function Main() returns int { "
        "int a=1; int b=2; int c=3; int d=4; int e=5; int f=6; "
        "double x=1.0; double y=2.0; double z=3.0; double w=4.0; "
        "double total = FloatHelper(x+y, z+w); "
        "int i=0; int result=0; "
        "while (i < 4) { if (i < 2) { result = result + Helper(a,b); } "
        "else { result = result + Helper(c,d); } i = i + 1; } "
        "return result + e + f; }";
    const auto result = compile_source(source, "n107-native-stress");
    std::size_t pdata = 0U;
    std::size_t xdata = 0U;
    for (const auto& section : result.native_object.object_file.sections) {
        if (section.name.rfind(".pdata$", 0U) == 0U) ++pdata;
        if (section.name.rfind(".xdata$", 0U) == 0U) ++xdata;
    }
    require(pdata == 4U && xdata == 4U && !result.object_bytes.empty(),
            "N107 multi-function native stress object is missing unwind sections");
    require(result.stats.native_persistent_gp_register_assignments > 0U &&
            result.stats.native_persistent_xmm_register_assignments > 0U,
            "N107 stress program did not exercise both GP and XMM persistent allocation");
    require(result.stats.native_persistent_register_reads > 0U &&
            result.stats.native_persistent_register_writes > 0U,
            "N107 stress program did not consume persistent machine locations");
}

} // namespace

void run_n102_n107_allocator_hardening_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 100U; ++index) {
        run_case("N102A_cfg_interference_branch_loop_" + std::to_string(index),
            test_n102_cfg_interference_separates_disjoint_branches, passed, failed);
        run_case("N103A_phi_edge_machine_location_coalescing_" + std::to_string(index),
            test_n103_graph_coloring_reuses_locations_for_phi_inputs, passed, failed);
        run_case("N104A_cfg_spill_slot_coloring_" + std::to_string(index),
            test_n104_spill_slots_are_colored_by_cfg_interference, passed, failed);
        run_case("N105A_dirty_home_mirror_elision_" + std::to_string(index),
            test_n105_register_residency_elides_redundant_home_mirror_stores, passed, failed);
        run_case("N106A_large_frame_probe_far_unwind_" + std::to_string(index),
            test_n106_large_frame_probe_and_far_unwind_encoding, passed, failed);
        run_case("N107A_native_gp_xmm_multiblock_stress_" + std::to_string(index),
            test_n107_multiblock_gp_xmm_native_object_stress, passed, failed);
    }
    run_case("N106B_real_codegen_chkstk_unwind_integration",
        test_n106_codegen_splices_chkstk_before_stack_allocation, passed, failed);
}
