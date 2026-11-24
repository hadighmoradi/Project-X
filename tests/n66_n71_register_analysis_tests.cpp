// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/native_register_analysis.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
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

NativeRegisterInstruction instruction(std::vector<std::uint32_t> definitions = {},
                                      std::vector<std::uint32_t> uses = {},
                                      bool is_call = false) {
    NativeRegisterInstruction result;
    result.definitions = std::move(definitions);
    result.uses = std::move(uses);
    result.is_call = is_call;
    return result;
}

NativeRegisterFunctionIR diamond_function(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values.push_back({value_id, NativeRegisterClass::GeneralPurpose});
    function.entry_block = 0U;
    function.blocks = {
        {0U, {instruction({value_id})}, {1U, 2U}, {}},
        {1U, {}, {3U}, {}},
        {2U, {}, {3U}, {}},
        {3U, {instruction({}, {value_id})}, {}, {}}
    };
    return function;
}

NativeRegisterFunctionIR call_function(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values.push_back({value_id, NativeRegisterClass::GeneralPurpose});
    function.entry_block = 0U;
    function.blocks = {{0U, {
        instruction({value_id}), instruction({}, {}, true), instruction({}, {value_id})
    }, {}, {}}};
    return function;
}

void test_n66_cfg_predecessors_and_diamond(std::uint32_t value_id) {
    auto function = diamond_function(value_id);
    const auto result = validate_native_register_function_ir(function);
    require(result.succeeded, "N66 valid diamond CFG was rejected");
    require(result.normalized_function.blocks[3].predecessors == std::vector<NativeRegisterBlockId>({1U, 2U}),
            "N66 merge-block predecessor list was not normalized deterministically");
    require(result.normalized_function.blocks[0].predecessors.empty(),
            "N66 entry block unexpectedly has predecessors");
}

void test_n66_unknown_value_is_rejected(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values.push_back({value_id, NativeRegisterClass::GeneralPurpose});
    function.entry_block = 0U;
    function.blocks = {{0U, {instruction({}, {value_id + 1U})}, {}, {}}};
    const auto result = validate_native_register_function_ir(function);
    require(!result.succeeded && !result.diagnostics.empty() &&
            result.diagnostics.front().code == "N66E0010",
            "N66 unknown virtual-value use was not rejected with its diagnostic");
}

void test_n66_duplicate_edge_is_rejected(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values.push_back({value_id, NativeRegisterClass::GeneralPurpose});
    function.entry_block = 0U;
    function.blocks = {{0U, {}, {1U, 1U}, {}}, {1U, {}, {}, {}}};
    const auto result = validate_native_register_function_ir(function);
    require(!result.succeeded && result.diagnostics.front().code == "N66E0007",
            "N66 duplicate CFG edge was not rejected");
}

void test_n66_duplicate_definition_and_loop_edges(std::uint32_t value_id) {
    NativeRegisterFunctionIR valid;
    valid.values.push_back({value_id, NativeRegisterClass::FloatingPoint});
    valid.entry_block = 0U;
    valid.blocks = {{0U, {instruction({value_id})}, {0U}, {}}};
    const auto valid_result = validate_native_register_function_ir(valid);
    require(valid_result.succeeded && valid_result.normalized_function.blocks[0].predecessors ==
            std::vector<NativeRegisterBlockId>({0U}),
            "N66 valid self-loop was rejected or its predecessor was lost");

    valid.blocks[0].instructions.front() = instruction({value_id, value_id});
    const auto invalid_result = validate_native_register_function_ir(valid);
    require(!invalid_result.succeeded && invalid_result.diagnostics.front().code == "N66E0009",
            "N66 duplicate definition within one instruction was not rejected");
}

void test_n67_instruction_level_liveness(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values = {{value_id, NativeRegisterClass::GeneralPurpose},
                       {value_id + 1U, NativeRegisterClass::GeneralPurpose}};
    function.entry_block = 0U;
    function.blocks = {{0U, {
        instruction({value_id}), instruction({value_id + 1U}, {value_id}),
        instruction({}, {value_id + 1U})
    }, {}, {}}};
    const auto live = compute_native_register_liveness(function);
    require(live.succeeded && live.converged, "N67 straight-line liveness did not converge");
    require(live.blocks[0].live_in.empty() && live.blocks[0].live_out.empty(),
            "N67 local values incorrectly escaped the block");
    require(live.instructions[0][0].live_after == std::vector<std::uint32_t>({value_id}),
            "N67 first definition did not become live after its instruction");
    require(live.instructions[0][1].live_before == std::vector<std::uint32_t>({value_id}) &&
            live.instructions[0][1].live_after == std::vector<std::uint32_t>({value_id + 1U}),
            "N67 use/definition transfer equation is incorrect");
    require(live.instructions[0][2].live_before == std::vector<std::uint32_t>({value_id + 1U}) &&
            live.instructions[0][2].live_after.empty(),
            "N67 last use did not end the live range");
}

void test_n67_liveness_propagates_through_diamond(std::uint32_t value_id) {
    const auto function = diamond_function(value_id);
    const auto live = compute_native_register_liveness(function);
    require(live.succeeded, "N67 diamond liveness failed");
    for (const NativeRegisterBlockId block : {1U, 2U}) {
        require(live.blocks[block].live_out == std::vector<std::uint32_t>({value_id}),
                "N67 liveness failed to propagate across one side of a diamond");
    }
    require(live.blocks[3].live_in == std::vector<std::uint32_t>({value_id}),
            "N67 merge block did not receive the union of predecessor liveness");
    require(live.blocks[0].live_in.empty(), "N67 value defined in entry block was marked live-in");
}

void test_n67_loop_reaches_fixed_point(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values = {{value_id, NativeRegisterClass::GeneralPurpose},
                       {value_id + 1U, NativeRegisterClass::GeneralPurpose}};
    function.entry_block = 0U;
    function.blocks = {
        {0U, {instruction({value_id})}, {1U}, {}},
        {1U, {instruction({value_id + 1U}, {value_id}), instruction({}, {value_id + 1U})}, {1U, 2U}, {}},
        {2U, {instruction({}, {value_id + 1U})}, {}, {}}
    };
    const auto live = compute_native_register_liveness(function);
    require(live.succeeded && live.converged && live.iterations >= 2U,
            "N67 loop CFG did not require and complete a fixed-point iteration");
    require(std::find(live.blocks[1].live_in.begin(), live.blocks[1].live_in.end(), value_id) != live.blocks[1].live_in.end(),
            "N67 loop-carried source value is not live-in");
}

void test_n67_use_before_definition_is_live_in(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values = {{value_id, NativeRegisterClass::GeneralPurpose}};
    function.entry_block = 0U;
    function.blocks = {{0U, {instruction({}, {value_id}), instruction({value_id})}, {}, {}}};
    const auto live = compute_native_register_liveness(function);
    require(live.succeeded && live.blocks[0].live_in == std::vector<std::uint32_t>({value_id}),
            "N67 use-before-definition did not propagate to block live-in");
}

void test_n68_interval_positions_follow_def_use(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values = {{value_id, NativeRegisterClass::GeneralPurpose}};
    function.entry_block = 0U;
    function.blocks = {{0U, {instruction({value_id}), instruction({}, {value_id})}, {}, {}}};
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    require(intervals.succeeded && intervals.intervals.size() == 1U,
            "N68 did not produce one live interval");
    require(intervals.intervals.front().start == 1U && intervals.intervals.front().end == 2U,
            "N68 interval bounds do not match definition-after/use-before positions");
}

void test_n68_call_crossing_is_detected(std::uint32_t value_id) {
    const auto function = call_function(value_id);
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    require(intervals.succeeded && intervals.intervals.size() == 1U &&
            intervals.intervals.front().crosses_call,
            "N68 did not mark a value live across a call");
}

void test_n68_register_class_is_preserved(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values = {{value_id, NativeRegisterClass::FloatingPoint}};
    function.entry_block = 0U;
    function.blocks = {{0U, {instruction({value_id}), instruction({}, {value_id})}, {}, {}}};
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    require(intervals.succeeded && intervals.intervals.front().register_class == NativeRegisterClass::FloatingPoint,
            "N68 interval analysis changed the value's register class");
}

void test_n68_positions_are_deterministic_across_blocks(std::uint32_t value_id) {
    auto function = diamond_function(value_id);
    const auto live = compute_native_register_liveness(function);
    const auto first = analyze_native_register_live_intervals(function, live);
    const auto second = analyze_native_register_live_intervals(function, live);
    require(first.succeeded && second.succeeded &&
            first.block_start_positions == second.block_start_positions &&
            first.instruction_positions == second.instruction_positions,
            "N68 block/instruction positions are not deterministic");
    require(first.block_start_positions[1] > first.block_start_positions[0],
            "N68 independent blocks were assigned overlapping layout positions");
}

void test_n69_call_boundary_splits_live_range(std::uint32_t value_id) {
    const auto function = call_function(value_id);
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    const auto split = split_native_register_live_ranges(function, live, intervals);
    require(split.succeeded && split.segments.size() == 2U,
            "N69 call-crossing value was not split into two register segments");
    require(split.segments[0].split_after == NativeRegisterSplitReason::CallBoundary &&
            split.segments[1].split_before == NativeRegisterSplitReason::CallBoundary,
            "N69 call-boundary split reasons were not preserved");
    require(split.segments[0].end + 1U == split.segments[1].start,
            "N69 call-split segments do not meet at the before/after boundary");
}

void test_n69_argument_only_value_is_not_split(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values = {{value_id, NativeRegisterClass::GeneralPurpose}};
    function.entry_block = 0U;
    function.blocks = {{0U, {instruction({value_id}), instruction({}, {value_id}, true)}, {}, {}}};
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    const auto split = split_native_register_live_ranges(function, live, intervals);
    require(split.succeeded && split.segments.size() == 1U &&
            split.segments.front().split_after == NativeRegisterSplitReason::None,
            "N69 argument dying at a call was incorrectly treated as call-crossing");
}

void test_n69_redefinition_creates_live_range_hole(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values = {{value_id, NativeRegisterClass::GeneralPurpose},
                       {value_id + 1U, NativeRegisterClass::GeneralPurpose}};
    function.entry_block = 0U;
    function.blocks = {{0U, {
        instruction({value_id}), instruction({}, {value_id}),
        instruction({value_id + 1U}), instruction({value_id}), instruction({}, {value_id})
    }, {}, {}}};
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    const auto split = split_native_register_live_ranges(function, live, intervals);
    std::vector<NativeRegisterLiveSegment> value_segments;
    for (const auto& segment : split.segments) if (segment.value_id == value_id) value_segments.push_back(segment);
    require(split.succeeded && value_segments.size() == 2U &&
            value_segments[0].split_after == NativeRegisterSplitReason::LiveRangeHole &&
            value_segments[1].split_before == NativeRegisterSplitReason::LiveRangeHole,
            "N69 dead interval between separate definitions was not split");
}

void test_n69_live_values_get_block_local_segments(std::uint32_t value_id) {
    const auto function = diamond_function(value_id);
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    const auto split = split_native_register_live_ranges(function, live, intervals);
    require(split.succeeded, "N69 branch CFG splitting failed");
    std::set<NativeRegisterBlockId> blocks;
    for (const auto& segment : split.segments) if (segment.value_id == value_id) blocks.insert(segment.block_id);
    require(blocks.size() >= 2U,
            "N69 CFG live range was not represented in more than one basic block");
}

void test_n70_call_preservation_actions_use_home_slot(std::uint32_t value_id) {
    const auto function = call_function(value_id);
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    const auto split = split_native_register_live_ranges(function, live, intervals);
    const auto plan = build_native_register_allocation_plan(function, live, intervals, split,
        NativeRegisterAllocationOptions{true, true, true});
    require(plan.succeeded && plan.home_slot_count == 1U,
            "N70 call-crossing value did not receive one canonical home slot");
    std::size_t stores = 0U;
    std::size_t reloads = 0U;
    for (const auto& action : plan.actions) {
        if (action.value_id != value_id) continue;
        if (action.kind == NativeSpillReloadActionKind::StoreBeforeCall) {
            ++stores;
            require(action.destination.kind == NativeRegisterLocationKind::HomeSlot,
                    "N70 call store does not target the canonical home slot");
        }
        if (action.kind == NativeSpillReloadActionKind::ReloadAfterCall) {
            ++reloads;
            require(action.source.kind == NativeRegisterLocationKind::HomeSlot,
                    "N70 call reload does not read the canonical home slot");
        }
    }
    require(stores == 1U && reloads == 1U,
            "N70 did not emit exactly one store/reload pair around a call");
}

void test_n70_register_pressure_generates_spill_work(std::uint32_t first_value) {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        function.values.push_back({first_value + index, NativeRegisterClass::GeneralPurpose});
    }
    NativeRegisterBasicBlock block;
    block.id = 0U;
    for (std::uint32_t index = 0U; index < 8U; ++index) block.instructions.push_back(instruction({first_value + index}));
    for (std::uint32_t index = 0U; index < 8U; ++index) block.instructions.push_back(instruction({}, {first_value + index}));
    function.blocks.push_back(std::move(block));
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    const auto split = split_native_register_live_ranges(function, live, intervals);
    const auto plan = build_native_register_allocation_plan(function, live, intervals, split,
        NativeRegisterAllocationOptions{true, true, true});
    require(plan.succeeded && plan.spill_slot_count >= 4U,
            "N70 pressure scenario did not spill values beyond the four Win64 GP scratch registers");
    std::size_t store_count = 0U;
    std::size_t reload_count = 0U;
    for (const auto& action : plan.actions) {
        if (action.kind == NativeSpillReloadActionKind::StoreAfterDefinition) ++store_count;
        if (action.kind == NativeSpillReloadActionKind::ReloadBeforeUse) ++reload_count;
    }
    require(store_count >= 4U && reload_count >= 4U,
            "N70 spilled values did not receive definition stores and use reloads");
}

void test_n70_no_call_means_no_home_slots(std::uint32_t value_id) {
    NativeRegisterFunctionIR function;
    function.values = {{value_id, NativeRegisterClass::GeneralPurpose}};
    function.entry_block = 0U;
    function.blocks = {{0U, {instruction({value_id}), instruction({}, {value_id})}, {}, {}}};
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    const auto split = split_native_register_live_ranges(function, live, intervals);
    const auto plan = build_native_register_allocation_plan(function, live, intervals, split);
    require(plan.succeeded && plan.home_slot_count == 0U,
            "N70 allocated a canonical call-home slot when no call is crossed");
}

void test_n70_plan_is_input_metadata_order_independent(std::uint32_t first_value) {
    NativeRegisterFunctionIR first;
    first.entry_block = 0U;
    for (std::uint32_t index = 0U; index < 6U; ++index)
        first.values.push_back({first_value + index, NativeRegisterClass::GeneralPurpose});
    NativeRegisterBasicBlock block;
    block.id = 0U;
    for (std::uint32_t index = 0U; index < 6U; ++index) block.instructions.push_back(instruction({first_value + index}));
    for (std::uint32_t index = 0U; index < 6U; ++index) block.instructions.push_back(instruction({}, {first_value + index}));
    first.blocks.push_back(block);
    NativeRegisterFunctionIR second = first;
    std::reverse(second.values.begin(), second.values.end());
    const auto make_plan = [](const NativeRegisterFunctionIR& function) {
        const auto live = compute_native_register_liveness(function);
        const auto intervals = analyze_native_register_live_intervals(function, live);
        const auto split = split_native_register_live_ranges(function, live, intervals);
        return build_native_register_allocation_plan(function, live, intervals, split);
    };
    const auto left = make_plan(first);
    const auto right = make_plan(second);
    require(left.succeeded && right.succeeded && left.segments.size() == right.segments.size() &&
            left.spill_slot_count == right.spill_slot_count && left.home_slot_count == right.home_slot_count,
            "N70 allocation plan changed when value metadata order was reversed");
    for (std::size_t index = 0U; index < left.segments.size(); ++index) {
        require(left.segments[index].segment.value_id == right.segments[index].segment.value_id &&
                left.segments[index].assignment.spilled == right.segments[index].assignment.spilled &&
                left.segments[index].assignment.register_index == right.segments[index].assignment.register_index &&
                left.segments[index].assignment.spill_slot == right.segments[index].assignment.spill_slot,
                "N70 segment assignments are not deterministic");
    }
}

NativeRegisterAllocationPlan nonvolatile_test_plan(std::uint32_t first_value,
                                                    NativeRegisterClass register_class) {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    function.values = {{first_value, register_class}, {first_value + 1U, register_class}};
    function.blocks = {{0U, {
        instruction({first_value}), instruction({first_value + 1U}),
        instruction({}, {first_value}), instruction({}, {first_value + 1U})
    }, {}, {}}};
    const auto live = compute_native_register_liveness(function);
    const auto intervals = analyze_native_register_live_intervals(function, live);
    const auto split = split_native_register_live_ranges(function, live, intervals);
    return build_native_register_allocation_plan(function, live, intervals, split,
        NativeRegisterAllocationOptions{false, true, false});
}

void test_n71_nonvolatile_gp_registers_get_save_slots(std::uint32_t value_id) {
    const auto allocation = nonvolatile_test_plan(value_id, NativeRegisterClass::GeneralPurpose);
    const auto frame = build_native_win64_frame_plan(allocation, 24U);
    require(allocation.succeeded && frame.succeeded && frame.prologue_saves.size() == 2U,
            "N71 nonvolatile GP registers did not receive two save slots");
    for (const auto& save : frame.prologue_saves) {
        require(save.register_class == NativeRegisterClass::GeneralPurpose && save.size_bytes == 8U &&
                save.frame_offset < 0,
                "N71 GP save slot has an invalid class, width or RBP-relative offset");
    }
}

void test_n71_nonvolatile_xmm_saves_are_aligned(std::uint32_t value_id) {
    const auto allocation = nonvolatile_test_plan(value_id, NativeRegisterClass::FloatingPoint);
    const auto frame = build_native_win64_frame_plan(allocation, 8U);
    require(allocation.succeeded && frame.succeeded && frame.prologue_saves.size() == 2U,
            "N71 nonvolatile XMM values did not receive two save slots");
    for (const auto& save : frame.prologue_saves) {
        require(save.register_class == NativeRegisterClass::FloatingPoint && save.size_bytes == 16U &&
                save.frame_offset < 0 && (save.frame_offset % 16) == 0,
                "N71 XMM save slot is not 16-byte aligned");
    }
}

void test_n71_frame_areas_do_not_overlap_and_are_aligned(std::uint32_t value_id) {
    NativeRegisterAllocationPlan allocation = nonvolatile_test_plan(value_id, NativeRegisterClass::GeneralPurpose);
    allocation.spill_slot_count = 3U;
    allocation.home_slot_count = 2U;
    const auto frame = build_native_win64_frame_plan(allocation, 13U);
    require(frame.succeeded && (frame.frame_size_bytes % 16U) == 0U,
            "N71 Win64 frame size is not 16-byte aligned");
    require(frame.spill_slot_offsets.size() == 3U && frame.home_slot_offsets.size() == 2U,
            "N71 frame did not lay out all spill and canonical home slots");
    std::set<std::int32_t> offsets;
    for (const auto offset : frame.spill_slot_offsets) require(offset < 0 && offsets.insert(offset).second,
        "N71 spill slot offsets overlap");
    for (const auto offset : frame.home_slot_offsets) require(offset < 0 && offsets.insert(offset).second,
        "N71 home slot offsets overlap another frame slot");
    for (const auto& save : frame.prologue_saves) require(offsets.insert(save.frame_offset).second,
        "N71 callee-save slot overlaps another frame area");
    require(frame.epilogue_restores.size() == frame.prologue_saves.size(),
            "N71 epilogue restore list does not mirror prologue saves");
}

void test_n71_frame_overflow_is_rejected(std::uint32_t value_id) {
    (void)value_id;
    NativeRegisterAllocationPlan allocation;
    allocation.succeeded = true;

    allocation.spill_slot_count = std::numeric_limits<std::uint32_t>::max();
    const auto huge_spills = build_native_win64_frame_plan(allocation, 0U);
    require(!huge_spills.succeeded && !huge_spills.diagnostics.empty() &&
                huge_spills.diagnostics.front().code == "N71E0009",
            "N71 oversized spill area was not rejected before layout iteration");

    allocation.spill_slot_count = 0U;
    allocation.home_slot_count = std::numeric_limits<std::uint32_t>::max();
    const auto huge_homes = build_native_win64_frame_plan(allocation, 0U);
    require(!huge_homes.succeeded && !huge_homes.diagnostics.empty() &&
                huge_homes.diagnostics.front().code == "N71E0009",
            "N71 oversized home area was not rejected before layout iteration");

    allocation.home_slot_count = 0U;
    const auto frame = build_native_win64_frame_plan(allocation, std::numeric_limits<std::uint32_t>::max());
    require(!frame.succeeded && !frame.diagnostics.empty(),
            "N71 oversized Win64 frame was not rejected");
}

} // namespace

void run_n66_n71_register_analysis_tests(int& passed, int& failed) {
    for (std::uint32_t index = 0U; index < 500U; ++index) {
        const std::uint32_t value_id = 100000U + index * 8U;
        run_case("N66A_cfg_predecessors_diamond_" + std::to_string(index),
            [value_id] { test_n66_cfg_predecessors_and_diamond(value_id); }, passed, failed);
        run_case("N66B_unknown_value_rejected_" + std::to_string(index),
            [value_id] { test_n66_unknown_value_is_rejected(value_id); }, passed, failed);
        run_case("N66C_duplicate_edge_rejected_" + std::to_string(index),
            [value_id] { test_n66_duplicate_edge_is_rejected(value_id); }, passed, failed);
        run_case("N66D_duplicate_definition_loop_" + std::to_string(index),
            [value_id] { test_n66_duplicate_definition_and_loop_edges(value_id); }, passed, failed);

        run_case("N67A_instruction_liveness_" + std::to_string(index),
            [value_id] { test_n67_instruction_level_liveness(value_id); }, passed, failed);
        run_case("N67B_diamond_liveness_" + std::to_string(index),
            [value_id] { test_n67_liveness_propagates_through_diamond(value_id); }, passed, failed);
        run_case("N67C_loop_fixed_point_" + std::to_string(index),
            [value_id] { test_n67_loop_reaches_fixed_point(value_id); }, passed, failed);
        run_case("N67D_use_before_def_" + std::to_string(index),
            [value_id] { test_n67_use_before_definition_is_live_in(value_id); }, passed, failed);

        run_case("N68A_interval_def_use_positions_" + std::to_string(index),
            [value_id] { test_n68_interval_positions_follow_def_use(value_id); }, passed, failed);
        run_case("N68B_call_crossing_metadata_" + std::to_string(index),
            [value_id] { test_n68_call_crossing_is_detected(value_id); }, passed, failed);
        run_case("N68C_register_class_preserved_" + std::to_string(index),
            [value_id] { test_n68_register_class_is_preserved(value_id); }, passed, failed);
        run_case("N68D_deterministic_positions_" + std::to_string(index),
            [value_id] { test_n68_positions_are_deterministic_across_blocks(value_id); }, passed, failed);

        run_case("N69A_call_boundary_split_" + std::to_string(index),
            [value_id] { test_n69_call_boundary_splits_live_range(value_id); }, passed, failed);
        run_case("N69B_call_argument_not_split_" + std::to_string(index),
            [value_id] { test_n69_argument_only_value_is_not_split(value_id); }, passed, failed);
        run_case("N69C_live_range_hole_split_" + std::to_string(index),
            [value_id] { test_n69_redefinition_creates_live_range_hole(value_id); }, passed, failed);
        run_case("N69D_block_local_segments_" + std::to_string(index),
            [value_id] { test_n69_live_values_get_block_local_segments(value_id); }, passed, failed);

        run_case("N70A_call_home_store_reload_" + std::to_string(index),
            [value_id] { test_n70_call_preservation_actions_use_home_slot(value_id); }, passed, failed);
        run_case("N70B_pressure_spill_work_" + std::to_string(index),
            [value_id] { test_n70_register_pressure_generates_spill_work(value_id); }, passed, failed);
        run_case("N70C_no_call_no_home_" + std::to_string(index),
            [value_id] { test_n70_no_call_means_no_home_slots(value_id); }, passed, failed);
        run_case("N70D_plan_determinism_" + std::to_string(index),
            [value_id] { test_n70_plan_is_input_metadata_order_independent(value_id); }, passed, failed);

        run_case("N71A_gp_nonvolatile_save_slots_" + std::to_string(index),
            [value_id] { test_n71_nonvolatile_gp_registers_get_save_slots(value_id); }, passed, failed);
        run_case("N71B_xmm_nonvolatile_alignment_" + std::to_string(index),
            [value_id] { test_n71_nonvolatile_xmm_saves_are_aligned(value_id); }, passed, failed);
        run_case("N71C_frame_area_layout_" + std::to_string(index),
            [value_id] { test_n71_frame_areas_do_not_overlap_and_are_aligned(value_id); }, passed, failed);
        run_case("N71D_frame_overflow_rejected_" + std::to_string(index),
            [value_id] { test_n71_frame_overflow_is_rejected(value_id); }, passed, failed);
    }
}
