#include "xfrontend/native_register_analysis.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace xfrontend {
namespace {

bool valid_register_class(NativeRegisterClass value) {
    return value == NativeRegisterClass::GeneralPurpose ||
           value == NativeRegisterClass::FloatingPoint;
}

bool contains(const std::vector<std::uint32_t>& values, std::uint32_t value) {
    return std::binary_search(values.begin(), values.end(), value);
}

std::vector<std::uint32_t> to_vector(const std::set<std::uint32_t>& values) {
    return std::vector<std::uint32_t>(values.begin(), values.end());
}

void add_diagnostic(std::vector<NativeRegisterIRDiagnostic>& diagnostics,
                    const char* code, const char* message) {
    diagnostics.push_back(NativeRegisterIRDiagnostic{code, message});
}

struct IntervalBounds {
    std::uint32_t start = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t end = 0U;
    bool seen = false;
    bool crosses_call = false;
};

void include_position(IntervalBounds& bounds, std::uint32_t position) {
    bounds.start = std::min(bounds.start, position);
    bounds.end = std::max(bounds.end, position);
    bounds.seen = true;
}

NativeRegisterLocation register_location(const NativeRegisterAssignment& assignment) {
    NativeRegisterLocation location;
    if (assignment.spilled) {
        location.kind = NativeRegisterLocationKind::SpillSlot;
        location.spill_slot = assignment.spill_slot;
    } else {
        location.kind = NativeRegisterLocationKind::Register;
        location.register_index = assignment.register_index;
    }
    return location;
}

} // namespace

NativeRegisterIRValidationResult validate_native_register_function_ir(
    const NativeRegisterFunctionIR& function) {
    NativeRegisterIRValidationResult result;
    if (function.blocks.empty()) {
        add_diagnostic(result.diagnostics, "N66E0001", "native register IR has no basic blocks");
        return result;
    }
    if (function.entry_block >= function.blocks.size()) {
        add_diagnostic(result.diagnostics, "N66E0002", "native register IR entry block is out of range");
        return result;
    }

    std::set<std::uint32_t> value_ids;
    for (const auto& value : function.values) {
        if (!valid_register_class(value.register_class)) {
            add_diagnostic(result.diagnostics, "N66E0003", "native register IR contains an invalid register class");
            return result;
        }
        if (!value_ids.insert(value.value_id).second) {
            add_diagnostic(result.diagnostics, "N66E0004", "native register IR contains a duplicate virtual value ID");
            return result;
        }
    }

    result.normalized_function = function;
    for (std::size_t block_index = 0U; block_index < result.normalized_function.blocks.size(); ++block_index) {
        auto& block = result.normalized_function.blocks[block_index];
        if (block.id != block_index) {
            add_diagnostic(result.diagnostics, "N66E0005", "native register block IDs must match their stable vector indices");
            return result;
        }
        block.predecessors.clear();
        std::set<NativeRegisterBlockId> successor_ids;
        for (const NativeRegisterBlockId successor : block.successors) {
            if (successor >= result.normalized_function.blocks.size()) {
                add_diagnostic(result.diagnostics, "N66E0006", "native register CFG edge targets an invalid block");
                return result;
            }
            if (!successor_ids.insert(successor).second) {
                add_diagnostic(result.diagnostics, "N66E0007", "native register CFG contains a duplicate successor edge");
                return result;
            }
        }
    }

    std::set<std::uint64_t> flow_operation_ids;
    for (std::size_t block_index = 0U; block_index < result.normalized_function.blocks.size(); ++block_index) {
        const auto& block = result.normalized_function.blocks[block_index];
        for (const NativeRegisterBlockId successor : block.successors) {
            result.normalized_function.blocks[successor].predecessors.push_back(
                static_cast<NativeRegisterBlockId>(block_index));
        }
        for (const auto& instruction : block.instructions) {
            const bool has_flow_origin = instruction.origin_kind !=
                NativeRegisterInstructionOriginKind::Synthetic;
            if (!has_flow_origin) {
                if (instruction.flow_operation_id != InvalidNativeFlowOperationId ||
                    instruction.flow_block_id != InvalidNativeRegisterBlockId ||
                    instruction.flow_operation_index != InvalidNativeFlowOperationIndex) {
                    add_diagnostic(result.diagnostics, "N108E0001",
                        "synthetic native instruction contains partial Flow origin metadata");
                    return result;
                }
            } else {
                const std::uint64_t expected_flow_operation_id =
                    ((static_cast<std::uint64_t>(block.id) + 1U) << 32U) |
                    (static_cast<std::uint64_t>(instruction.flow_operation_index) + 1U);
                if (instruction.flow_operation_id == InvalidNativeFlowOperationId ||
                    instruction.flow_block_id != block.id ||
                    instruction.flow_operation_index == InvalidNativeFlowOperationIndex ||
                    instruction.flow_operation_id != expected_flow_operation_id ||
                    instruction.source_span.end.byte_offset <= instruction.source_span.start.byte_offset) {
                    add_diagnostic(result.diagnostics, "N108E0002",
                        "Flow-origin native instruction has an invalid stable ID, block, ordinal, or source span");
                    return result;
                }
                if (!flow_operation_ids.insert(instruction.flow_operation_id).second) {
                    add_diagnostic(result.diagnostics, "N108E0003",
                        "native register IR contains a duplicate stable Flow operation ID");
                    return result;
                }
            }
            std::set<std::uint32_t> definitions;
            for (const auto value_id : instruction.definitions) {
                if (value_ids.find(value_id) == value_ids.end()) {
                    add_diagnostic(result.diagnostics, "N66E0008", "native register instruction defines an unknown virtual value");
                    return result;
                }
                if (!definitions.insert(value_id).second) {
                    add_diagnostic(result.diagnostics, "N66E0009", "native register instruction defines the same value more than once");
                    return result;
                }
            }
            for (const auto value_id : instruction.uses) {
                if (value_ids.find(value_id) == value_ids.end()) {
                    add_diagnostic(result.diagnostics, "N66E0010", "native register instruction uses an unknown virtual value");
                    return result;
                }
            }
        }
    }

    for (auto& block : result.normalized_function.blocks) {
        std::sort(block.predecessors.begin(), block.predecessors.end());
    }
    result.succeeded = true;
    return result;
}

NativeRegisterLivenessResult compute_native_register_liveness(
    const NativeRegisterFunctionIR& function) {
    NativeRegisterLivenessResult result;
    const auto validation = validate_native_register_function_ir(function);
    if (!validation.succeeded) {
        result.diagnostics = validation.diagnostics;
        return result;
    }
    const auto& normalized = validation.normalized_function;
    const std::size_t block_count = normalized.blocks.size();
    std::vector<std::set<std::uint32_t>> block_use(block_count);
    std::vector<std::set<std::uint32_t>> block_def(block_count);
    std::vector<std::set<std::uint32_t>> live_in(block_count);
    std::vector<std::set<std::uint32_t>> live_out(block_count);

    for (std::size_t block_index = 0U; block_index < block_count; ++block_index) {
        for (const auto& instruction : normalized.blocks[block_index].instructions) {
            for (const auto value_id : instruction.uses) {
                if (block_def[block_index].find(value_id) == block_def[block_index].end()) {
                    block_use[block_index].insert(value_id);
                }
            }
            for (const auto value_id : instruction.definitions) {
                block_def[block_index].insert(value_id);
            }
        }
    }

    const std::size_t value_factor = std::max<std::size_t>(1U, normalized.values.size());
    const std::size_t max_iterations = block_count * value_factor + block_count + 1U;
    bool changed = true;
    while (changed && result.iterations < max_iterations) {
        changed = false;
        ++result.iterations;
        for (std::size_t block_index = block_count; block_index-- > 0U;) {
            std::set<std::uint32_t> next_out;
            for (const NativeRegisterBlockId successor : normalized.blocks[block_index].successors) {
                next_out.insert(live_in[successor].begin(), live_in[successor].end());
            }
            std::set<std::uint32_t> next_in = block_use[block_index];
            for (const auto value_id : next_out) {
                if (block_def[block_index].find(value_id) == block_def[block_index].end()) {
                    next_in.insert(value_id);
                }
            }
            if (next_out != live_out[block_index] || next_in != live_in[block_index]) {
                live_out[block_index] = std::move(next_out);
                live_in[block_index] = std::move(next_in);
                changed = true;
            }
        }
    }
    if (changed) {
        add_diagnostic(result.diagnostics, "N67E0001", "native register liveness did not converge within its finite dataflow bound");
        return result;
    }

    result.blocks.resize(block_count);
    result.instructions.resize(block_count);
    for (std::size_t block_index = 0U; block_index < block_count; ++block_index) {
        result.blocks[block_index].live_in = to_vector(live_in[block_index]);
        result.blocks[block_index].live_out = to_vector(live_out[block_index]);
        const auto& instructions = normalized.blocks[block_index].instructions;
        auto& instruction_results = result.instructions[block_index];
        instruction_results.resize(instructions.size());
        std::set<std::uint32_t> current = live_out[block_index];
        for (std::size_t instruction_index = instructions.size(); instruction_index-- > 0U;) {
            const auto& instruction = instructions[instruction_index];
            instruction_results[instruction_index].live_after = to_vector(current);
            std::set<std::uint32_t> before;
            for (const auto value_id : instruction.uses) before.insert(value_id);
            std::set<std::uint32_t> definitions(instruction.definitions.begin(), instruction.definitions.end());
            for (const auto value_id : current) {
                if (definitions.find(value_id) == definitions.end()) before.insert(value_id);
            }
            instruction_results[instruction_index].live_before = to_vector(before);
            current = std::move(before);
        }
    }

    result.succeeded = true;
    result.converged = true;
    return result;
}

NativeRegisterInterferenceGraph build_native_register_interference_graph(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterLivenessResult& liveness) {
    NativeRegisterInterferenceGraph result;
    const auto validation = validate_native_register_function_ir(function);
    if (!validation.succeeded || !liveness.succeeded || !liveness.converged ||
        liveness.blocks.size() != function.blocks.size() ||
        liveness.instructions.size() != function.blocks.size()) {
        result.diagnostics = validation.diagnostics;
        if (result.diagnostics.empty()) {
            add_diagnostic(result.diagnostics, "N102E0001",
                "interference construction received incomplete CFG liveness");
        }
        return result;
    }

    std::map<std::uint32_t, NativeRegisterClass> register_classes;
    for (const auto& value : function.values) {
        register_classes.emplace(value.value_id, value.register_class);
        result.value_ids.push_back(value.value_id);
    }
    std::sort(result.value_ids.begin(), result.value_ids.end());

    std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
    const auto add_clique = [&](const std::vector<std::uint32_t>& live_values) {
        for (std::size_t left_index = 0U; left_index < live_values.size(); ++left_index) {
            const auto left_class = register_classes.find(live_values[left_index]);
            if (left_class == register_classes.end()) continue;
            for (std::size_t right_index = left_index + 1U; right_index < live_values.size(); ++right_index) {
                const auto right_class = register_classes.find(live_values[right_index]);
                if (right_class == register_classes.end() || left_class->second != right_class->second ||
                    live_values[left_index] == live_values[right_index]) continue;
                const auto first = std::min(live_values[left_index], live_values[right_index]);
                const auto second = std::max(live_values[left_index], live_values[right_index]);
                edges.emplace(first, second);
            }
        }
    };

    for (std::size_t block_index = 0U; block_index < function.blocks.size(); ++block_index) {
        const auto& block = function.blocks[block_index];
        if (liveness.instructions[block_index].size() != block.instructions.size()) {
            add_diagnostic(result.diagnostics, "N102E0002",
                "interference construction instruction-liveness shape does not match CFG");
            return result;
        }
        add_clique(liveness.blocks[block_index].live_in);
        add_clique(liveness.blocks[block_index].live_out);
        for (const auto& instruction_live : liveness.instructions[block_index]) {
            add_clique(instruction_live.live_before);
            add_clique(instruction_live.live_after);
        }

        std::set<std::uint32_t> live(liveness.blocks[block_index].live_out.begin(),
                                     liveness.blocks[block_index].live_out.end());
        for (std::size_t instruction_index = block.instructions.size(); instruction_index-- > 0U;) {
            const auto& instruction = block.instructions[instruction_index];
            std::vector<std::uint32_t> definitions = instruction.definitions;
            std::sort(definitions.begin(), definitions.end());
            for (const auto defined_value : definitions) {
                const auto defined_class = register_classes.find(defined_value);
                if (defined_class == register_classes.end()) continue;
                for (const auto live_value : live) {
                    const auto live_class = register_classes.find(live_value);
                    if (live_class == register_classes.end() || live_class->second != defined_class->second ||
                        live_value == defined_value) continue;
                    edges.emplace(std::min(defined_value, live_value),
                                  std::max(defined_value, live_value));
                }
            }
            for (const auto defined_value : instruction.definitions) live.erase(defined_value);
            for (const auto used_value : instruction.uses) live.insert(used_value);
        }
    }

    result.edges.reserve(edges.size());
    for (const auto& edge : edges) {
        result.edges.push_back(NativeRegisterInterferenceEdge{edge.first, edge.second});
    }
    result.succeeded = true;
    return result;
}

NativeRegisterIntervalAnalysis analyze_native_register_live_intervals(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterLivenessResult& liveness) {
    NativeRegisterIntervalAnalysis result;
    const auto validation = validate_native_register_function_ir(function);
    if (!validation.succeeded || !liveness.succeeded || !liveness.converged ||
        liveness.blocks.size() != function.blocks.size() ||
        liveness.instructions.size() != function.blocks.size()) {
        result.diagnostics = validation.diagnostics;
        if (result.diagnostics.empty()) {
            add_diagnostic(result.diagnostics, "N68E0001", "native register interval analysis received an incomplete liveness result");
        }
        return result;
    }

    std::unordered_map<std::uint32_t, NativeRegisterClass> classes;
    for (const auto& value : function.values) classes.emplace(value.value_id, value.register_class);
    std::map<std::uint32_t, IntervalBounds> bounds;
    result.block_start_positions.resize(function.blocks.size());
    result.instruction_positions.resize(function.blocks.size());
    std::uint64_t cursor = 0U;

    for (std::size_t block_index = 0U; block_index < function.blocks.size(); ++block_index) {
        const auto& block = function.blocks[block_index];
        if (cursor > std::numeric_limits<std::uint32_t>::max()) {
            add_diagnostic(result.diagnostics, "N68E0002", "native register instruction-position space overflowed");
            return result;
        }
        result.block_start_positions[block_index] = static_cast<std::uint32_t>(cursor);
        auto& positions = result.instruction_positions[block_index];
        positions.resize(block.instructions.size());
        for (std::size_t instruction_index = 0U; instruction_index < block.instructions.size(); ++instruction_index) {
            const std::uint64_t wide_position = cursor + 2U * instruction_index;
            if (wide_position + 1U > std::numeric_limits<std::uint32_t>::max()) {
                add_diagnostic(result.diagnostics, "N68E0002", "native register instruction-position space overflowed");
                return result;
            }
            const auto position = static_cast<std::uint32_t>(wide_position);
            positions[instruction_index] = position;
            const auto& instruction = block.instructions[instruction_index];
            const auto& live = liveness.instructions[block_index][instruction_index];
            for (const auto value_id : live.live_before) include_position(bounds[value_id], position);
            for (const auto value_id : instruction.uses) include_position(bounds[value_id], position);
            for (const auto value_id : live.live_after) include_position(bounds[value_id], position + 1U);
            for (const auto value_id : instruction.definitions) include_position(bounds[value_id], position + 1U);
            if (instruction.is_call) {
                for (const auto value_id : live.live_before) {
                    if (contains(live.live_after, value_id)) bounds[value_id].crosses_call = true;
                }
            }
        }
        cursor += 2U * block.instructions.size() + 2U;
    }

    for (const auto& entry : bounds) {
        if (!entry.second.seen) continue;
        const auto class_it = classes.find(entry.first);
        if (class_it == classes.end()) {
            add_diagnostic(result.diagnostics, "N68E0003", "liveness refers to a value absent from native register metadata");
            return result;
        }
        result.intervals.push_back(NativeLiveInterval{entry.first, entry.second.start,
            entry.second.end, class_it->second, entry.second.crosses_call});
    }
    std::sort(result.intervals.begin(), result.intervals.end(), [](const auto& left, const auto& right) {
        return std::tie(left.start, left.end, left.value_id) <
               std::tie(right.start, right.end, right.value_id);
    });
    result.succeeded = true;
    return result;
}

NativeRegisterSegmentAnalysis split_native_register_live_ranges(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterIntervalAnalysis& intervals) {
    NativeRegisterSegmentAnalysis result;
    const auto validation = validate_native_register_function_ir(function);
    if (!validation.succeeded || !liveness.succeeded || !intervals.succeeded ||
        liveness.blocks.size() != function.blocks.size() ||
        liveness.instructions.size() != function.blocks.size() ||
        intervals.instruction_positions.size() != function.blocks.size()) {
        result.diagnostics = validation.diagnostics;
        if (result.diagnostics.empty()) {
            add_diagnostic(result.diagnostics, "N69E0001", "native live-range splitting received incomplete analysis data");
        }
        return result;
    }

    std::unordered_map<std::uint32_t, NativeRegisterClass> classes;
    for (const auto& value : function.values) classes.emplace(value.value_id, value.register_class);
    std::uint32_t next_segment_id = 1U;

    for (std::size_t block_index = 0U; block_index < function.blocks.size(); ++block_index) {
        const auto& block = function.blocks[block_index];
        std::map<std::uint32_t, std::set<std::uint32_t>> points_by_value;
        std::map<std::uint32_t, std::set<std::uint32_t>> call_breaks_by_value;
        for (std::size_t instruction_index = 0U; instruction_index < block.instructions.size(); ++instruction_index) {
            const auto& instruction = block.instructions[instruction_index];
            const auto& live = liveness.instructions[block_index][instruction_index];
            const std::uint32_t before_position = intervals.instruction_positions[block_index][instruction_index];
            const std::uint32_t after_position = before_position + 1U;
            std::set<std::uint32_t> before_values(live.live_before.begin(), live.live_before.end());
            std::set<std::uint32_t> after_values(live.live_after.begin(), live.live_after.end());
            before_values.insert(instruction.uses.begin(), instruction.uses.end());
            after_values.insert(instruction.definitions.begin(), instruction.definitions.end());
            for (const auto value_id : before_values) points_by_value[value_id].insert(before_position);
            for (const auto value_id : after_values) points_by_value[value_id].insert(after_position);
            if (instruction.is_call) {
                for (const auto value_id : live.live_before) {
                    if (contains(live.live_after, value_id)) call_breaks_by_value[value_id].insert(before_position);
                }
            }
        }

        for (const auto& value_points : points_by_value) {
            const auto class_it = classes.find(value_points.first);
            if (class_it == classes.end()) {
                add_diagnostic(result.diagnostics, "N69E0002", "live-range splitting found an unknown virtual value");
                return result;
            }
            if (value_points.second.empty()) continue;
            auto point = value_points.second.begin();
            std::uint32_t segment_start = *point;
            std::uint32_t previous = *point;
            NativeRegisterSplitReason split_before = NativeRegisterSplitReason::None;
            ++point;
            for (; point != value_points.second.end(); ++point) {
                NativeRegisterSplitReason reason = NativeRegisterSplitReason::None;
                if (*point == previous + 1U &&
                    call_breaks_by_value[value_points.first].find(previous) !=
                        call_breaks_by_value[value_points.first].end()) {
                    reason = NativeRegisterSplitReason::CallBoundary;
                } else if (*point > previous + 1U) {
                    reason = NativeRegisterSplitReason::LiveRangeHole;
                }
                if (reason != NativeRegisterSplitReason::None) {
                    result.segments.push_back(NativeRegisterLiveSegment{next_segment_id++,
                        value_points.first, static_cast<NativeRegisterBlockId>(block_index),
                        segment_start, previous, class_it->second, split_before, reason});
                    segment_start = *point;
                    split_before = reason;
                }
                previous = *point;
            }
            result.segments.push_back(NativeRegisterLiveSegment{next_segment_id++,
                value_points.first, static_cast<NativeRegisterBlockId>(block_index),
                segment_start, previous, class_it->second, split_before,
                NativeRegisterSplitReason::None});
        }
    }
    result.succeeded = true;
    return result;
}

NativeRegisterAllocationPlan build_native_register_allocation_plan(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterIntervalAnalysis& intervals,
    const NativeRegisterSegmentAnalysis& segments,
    NativeRegisterAllocationOptions options) {
    NativeRegisterAllocationPlan result;
    if (!validate_native_register_function_ir(function).succeeded ||
        !liveness.succeeded || !intervals.succeeded || !segments.succeeded) {
        add_diagnostic(result.diagnostics, "N70E0001", "native register allocation plan received invalid or incomplete analysis data");
        return result;
    }

    std::vector<NativeLiveInterval> allocation_intervals;
    allocation_intervals.reserve(segments.segments.size());
    for (const auto& segment : segments.segments) {
        allocation_intervals.push_back(NativeLiveInterval{segment.segment_id, segment.start,
            segment.end, segment.register_class, false});
    }
    NativeRegisterAllocationResult allocated;
    if (!allocation_intervals.empty()) {
        allocated = allocate_native_registers(allocation_intervals, options);
        if (!allocated.succeeded || allocated.assignments.size() != segments.segments.size()) {
            add_diagnostic(result.diagnostics, "N70E0002", "split-segment register allocation failed");
            return result;
        }
    } else {
        allocated.succeeded = true;
    }

    std::map<std::uint32_t, NativeRegisterAssignment> assignment_by_segment;
    for (const auto& assignment : allocated.assignments) {
        assignment_by_segment.emplace(assignment.value_id, assignment);
    }
    for (const auto& segment : segments.segments) {
        const auto found = assignment_by_segment.find(segment.segment_id);
        if (found == assignment_by_segment.end()) {
            add_diagnostic(result.diagnostics, "N70E0003", "allocator omitted a split live segment");
            return result;
        }
        result.segments.push_back(NativeAllocatedSegment{segment, found->second});
    }
    result.spill_slot_count = allocated.spill_slot_count;
    result.used_callee_saved_gp = allocated.used_callee_saved_gp;
    result.used_callee_saved_xmm = allocated.used_callee_saved_xmm;
    result.stats = allocated.stats;

    // Canonical home slots are needed only for values that cross a call while
    // allocating from caller-saved pools. Color homes over the original full
    // intervals, not the call-split fragments, so a home remains stable.
    std::set<std::uint32_t> home_values;
    if (options.prefer_caller_saved) {
        for (std::size_t block_index = 0U; block_index < function.blocks.size(); ++block_index) {
            for (std::size_t instruction_index = 0U;
                 instruction_index < function.blocks[block_index].instructions.size(); ++instruction_index) {
                if (!function.blocks[block_index].instructions[instruction_index].is_call) continue;
                const auto& live = liveness.instructions[block_index][instruction_index];
                for (const auto value_id : live.live_before) {
                    if (contains(live.live_after, value_id)) home_values.insert(value_id);
                }
            }
        }
    }
    std::vector<NativeLiveInterval> home_intervals;
    for (const auto& interval : intervals.intervals) {
        if (home_values.find(interval.value_id) != home_values.end()) home_intervals.push_back(interval);
    }
    std::sort(home_intervals.begin(), home_intervals.end(), [](const auto& left, const auto& right) {
        return std::tie(left.start, left.end, left.value_id) <
               std::tie(right.start, right.end, right.value_id);
    });
    std::vector<std::uint32_t> home_slot_ends;
    std::map<std::uint32_t, std::uint32_t> home_slot_by_value;
    for (const auto& interval : home_intervals) {
        std::size_t slot = 0U;
        while (slot < home_slot_ends.size() && home_slot_ends[slot] >= interval.start) ++slot;
        if (slot == home_slot_ends.size()) home_slot_ends.push_back(interval.end);
        else home_slot_ends[slot] = interval.end;
        home_slot_by_value.emplace(interval.value_id, static_cast<std::uint32_t>(slot));
    }
    result.home_slot_count = static_cast<std::uint32_t>(home_slot_ends.size());

    for (std::size_t block_index = 0U; block_index < function.blocks.size(); ++block_index) {
        const auto& block = function.blocks[block_index];
        for (std::size_t instruction_index = 0U; instruction_index < block.instructions.size(); ++instruction_index) {
            const auto& instruction = block.instructions[instruction_index];
            const auto& live = liveness.instructions[block_index][instruction_index];
            if (instruction.is_call && options.prefer_caller_saved) {
                for (const auto value_id : live.live_before) {
                    if (!contains(live.live_after, value_id)) continue;
                    const auto home = home_slot_by_value.find(value_id);
                    if (home == home_slot_by_value.end()) {
                        add_diagnostic(result.diagnostics, "N70E0004", "call-crossing value has no canonical home slot");
                        return result;
                    }
                    const std::uint32_t position = intervals.instruction_positions[block_index][instruction_index];
                    const NativeAllocatedSegment* before_segment = nullptr;
                    const NativeAllocatedSegment* after_segment = nullptr;
                    for (const auto& candidate : result.segments) {
                        if (candidate.segment.value_id != value_id || candidate.segment.block_id != block_index) continue;
                        if (candidate.segment.start <= position && candidate.segment.end >= position) before_segment = &candidate;
                        if (candidate.segment.start <= position + 1U && candidate.segment.end >= position + 1U) after_segment = &candidate;
                    }
                    NativeRegisterLocation home_location;
                    home_location.kind = NativeRegisterLocationKind::HomeSlot;
                    home_location.spill_slot = home->second;
                    if (before_segment != nullptr) {
                        result.actions.push_back(NativeSpillReloadAction{
                            NativeSpillReloadActionKind::StoreBeforeCall, value_id,
                            static_cast<NativeRegisterBlockId>(block_index),
                            static_cast<std::uint32_t>(instruction_index), home->second,
                            register_location(before_segment->assignment), home_location});
                    }
                    if (after_segment != nullptr) {
                        result.actions.push_back(NativeSpillReloadAction{
                            NativeSpillReloadActionKind::ReloadAfterCall, value_id,
                            static_cast<NativeRegisterBlockId>(block_index),
                            static_cast<std::uint32_t>(instruction_index), home->second,
                            home_location, register_location(after_segment->assignment)});
                    }
                }
            }
        }
    }

    // A segment assigned to a spill slot gets explicit abstract memory actions
    // at each definition and use in that segment. The eventual emitter selects
    // an ephemeral scratch register according to the operand class.
    for (const auto& allocated_segment : result.segments) {
        if (!allocated_segment.assignment.spilled) continue;
        const auto block_index = allocated_segment.segment.block_id;
        const auto& block = function.blocks[block_index];
        for (std::size_t instruction_index = 0U; instruction_index < block.instructions.size(); ++instruction_index) {
            const auto& instruction = block.instructions[instruction_index];
            const std::uint32_t position = intervals.instruction_positions[block_index][instruction_index];
            const bool has_use = std::find(instruction.uses.begin(), instruction.uses.end(),
                allocated_segment.segment.value_id) != instruction.uses.end();
            const bool has_definition = std::find(instruction.definitions.begin(), instruction.definitions.end(),
                allocated_segment.segment.value_id) != instruction.definitions.end();
            if (has_use && position >= allocated_segment.segment.start && position <= allocated_segment.segment.end) {
                NativeRegisterLocation source;
                source.kind = NativeRegisterLocationKind::SpillSlot;
                source.spill_slot = allocated_segment.assignment.spill_slot;
                NativeRegisterLocation destination;
                destination.kind = NativeRegisterLocationKind::Temporary;
                result.actions.push_back(NativeSpillReloadAction{
                    NativeSpillReloadActionKind::ReloadBeforeUse, allocated_segment.segment.value_id,
                    block_index, static_cast<std::uint32_t>(instruction_index),
                    allocated_segment.assignment.spill_slot, source, destination});
            }
            if (has_definition && position + 1U >= allocated_segment.segment.start &&
                position + 1U <= allocated_segment.segment.end) {
                NativeRegisterLocation source;
                source.kind = NativeRegisterLocationKind::Temporary;
                NativeRegisterLocation destination;
                destination.kind = NativeRegisterLocationKind::SpillSlot;
                destination.spill_slot = allocated_segment.assignment.spill_slot;
                result.actions.push_back(NativeSpillReloadAction{
                    NativeSpillReloadActionKind::StoreAfterDefinition, allocated_segment.segment.value_id,
                    block_index, static_cast<std::uint32_t>(instruction_index),
                    allocated_segment.assignment.spill_slot, source, destination});
            }
        }
    }

    std::sort(result.actions.begin(), result.actions.end(), [](const auto& left, const auto& right) {
        return std::tie(left.block_id, left.instruction_index, left.kind, left.value_id) <
               std::tie(right.block_id, right.instruction_index, right.kind, right.value_id);
    });
    result.succeeded = true;
    return result;
}

NativeRegisterFlowSegmentPlan build_native_register_flow_segment_plan(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterIntervalAnalysis& intervals,
    const NativeRegisterSegmentAnalysis& segments,
    const NativeRegisterAllocationPlan& allocation) {
    NativeRegisterFlowSegmentPlan result;
    const auto validated = validate_native_register_function_ir(function);
    if (!validated.succeeded || !liveness.succeeded || !liveness.converged ||
        !intervals.succeeded || !segments.succeeded || !allocation.succeeded ||
        liveness.blocks.size() != validated.normalized_function.blocks.size() ||
        liveness.instructions.size() != validated.normalized_function.blocks.size() ||
        intervals.block_start_positions.size() != validated.normalized_function.blocks.size() ||
        intervals.instruction_positions.size() != validated.normalized_function.blocks.size() ||
        segments.segments.size() != allocation.segments.size()) {
        result.diagnostics = validated.diagnostics;
        if (result.diagnostics.empty()) {
            add_diagnostic(result.diagnostics, "N112E0001",
                "Flow segment reconciliation received failed, incomplete, or shape-inconsistent analysis data");
        }
        return result;
    }

    const auto& normalized = validated.normalized_function;
    std::map<std::uint32_t, NativeRegisterClass> value_classes;
    for (const auto& value : normalized.values) value_classes.emplace(value.value_id, value.register_class);

    auto same_location = [](const NativeRegisterLocation& left, const NativeRegisterLocation& right) {
        if (left.kind != right.kind) return false;
        switch (left.kind) {
        case NativeRegisterLocationKind::Register:
            return left.register_index == right.register_index;
        case NativeRegisterLocationKind::SpillSlot:
        case NativeRegisterLocationKind::HomeSlot:
            return left.spill_slot == right.spill_slot;
        case NativeRegisterLocationKind::Temporary:
            return true;
        }
        return false;
    };
    auto location_for_assignment = [](const NativeRegisterAssignment& assignment) {
        NativeRegisterLocation location;
        if (assignment.spilled) {
            location.kind = NativeRegisterLocationKind::SpillSlot;
            location.spill_slot = assignment.spill_slot;
        } else {
            location.kind = NativeRegisterLocationKind::Register;
            location.register_index = assignment.register_index;
        }
        return location;
    };
    auto same_segment = [](const NativeRegisterLiveSegment& left, const NativeRegisterLiveSegment& right) {
        return left.segment_id == right.segment_id && left.value_id == right.value_id &&
            left.block_id == right.block_id && left.start == right.start && left.end == right.end &&
            left.register_class == right.register_class && left.split_before == right.split_before &&
            left.split_after == right.split_after;
    };

    // Validate the two independently produced segment tables before creating
    // any lookup. The allocator key is segment_id, not the virtual value_id.
    std::map<std::uint32_t, const NativeRegisterAssignment*> assignments_by_segment;
    std::map<std::uint32_t, const NativeRegisterLiveSegment*> segments_by_id;
    for (const auto& segment : segments.segments) {
        const auto value = value_classes.find(segment.value_id);
        if (segment.segment_id == 0U || value == value_classes.end() ||
            segment.block_id >= normalized.blocks.size() || segment.start > segment.end ||
            value->second != segment.register_class ||
            !segments_by_id.emplace(segment.segment_id, &segment).second) {
            add_diagnostic(result.diagnostics, "N112E0002",
                "split-segment table contains an invalid ID, value, block, range, class, or duplicate segment");
            return result;
        }
    }
    for (const auto& allocated : allocation.segments) {
        const auto segment = segments_by_id.find(allocated.segment.segment_id);
        if (segment == segments_by_id.end() || !same_segment(*segment->second, allocated.segment) ||
            allocated.assignment.value_id != allocated.segment.segment_id ||
            allocated.assignment.register_class != allocated.segment.register_class ||
            !assignments_by_segment.emplace(allocated.segment.segment_id, &allocated.assignment).second) {
            add_diagnostic(result.diagnostics, "N112E0003",
                "allocated segments do not exactly match split segments or allocator segment keys");
            return result;
        }
        if (allocated.assignment.spilled) {
            if (allocated.assignment.spill_slot >= allocation.spill_slot_count) {
                add_diagnostic(result.diagnostics, "N112E0004",
                    "allocated segment references a spill slot outside the allocation plan");
                return result;
            }
        } else if (allocated.assignment.register_index >= 16U) {
            add_diagnostic(result.diagnostics, "N112E0005",
                "allocated segment references a physical register outside the AMD64 register file");
            return result;
        }
    }
    if (assignments_by_segment.size() != segments_by_id.size()) {
        add_diagnostic(result.diagnostics, "N112E0006",
            "allocation plan is missing one or more split-segment assignments");
        return result;
    }

    result.segments.reserve(segments.segments.size());
    for (const auto& segment : segments.segments) {
        const auto assignment = assignments_by_segment.find(segment.segment_id);
        if (assignment == assignments_by_segment.end()) {
            add_diagnostic(result.diagnostics, "N112E0006",
                "allocation plan is missing one or more split-segment assignments");
            return result;
        }
        result.segments.push_back(NativeRegisterFlowSegmentLocation{
            segment, location_for_assignment(*assignment->second)});
    }
    auto find_location = [&](NativeRegisterBlockId block_id, std::uint32_t value_id,
                              std::uint32_t position, NativeRegisterFlowValueLocation& out) {
        const NativeRegisterFlowSegmentLocation* match = nullptr;
        for (const auto& candidate : result.segments) {
            if (candidate.segment.block_id != block_id || candidate.segment.value_id != value_id ||
                candidate.segment.start > position || candidate.segment.end < position) continue;
            if (match != nullptr) return -1; // Ambiguous overlapping fragments.
            match = &candidate;
        }
        if (match == nullptr) return 0;
        out.value_id = value_id;
        out.register_class = match->segment.register_class;
        out.location = match->location;
        return 1;
    };

    std::map<std::pair<NativeRegisterBlockId, std::uint32_t>, std::size_t> operation_by_native_index;
    std::set<std::uint64_t> operation_ids;
    for (std::size_t block_index = 0U; block_index < normalized.blocks.size(); ++block_index) {
        const auto& block = normalized.blocks[block_index];
        if (liveness.instructions[block_index].size() != block.instructions.size() ||
            intervals.instruction_positions[block_index].size() != block.instructions.size()) {
            add_diagnostic(result.diagnostics, "N112E0007",
                "liveness or instruction-position shape differs from the native register block");
            return result;
        }
        for (std::size_t instruction_index = 0U; instruction_index < block.instructions.size(); ++instruction_index) {
            const auto& instruction = block.instructions[instruction_index];
            if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic) continue;
            if (instruction.flow_operation_id == InvalidNativeFlowOperationId ||
                !operation_ids.insert(instruction.flow_operation_id).second) {
                add_diagnostic(result.diagnostics, "N112E0008",
                    "Flow-origin instruction has a missing or duplicate stable operation ID");
                return result;
            }
            const std::uint32_t before_position = intervals.instruction_positions[block_index][instruction_index];
            if (before_position == std::numeric_limits<std::uint32_t>::max()) {
                add_diagnostic(result.diagnostics, "N112E0009", "Flow operation after-position overflows 32-bit coordinates");
                return result;
            }
            NativeRegisterFlowOperationLocation operation;
            operation.flow_operation_id = instruction.flow_operation_id;
            operation.flow_block_id = instruction.flow_block_id;
            operation.flow_operation_index = instruction.flow_operation_index;
            operation.native_instruction_index = static_cast<std::uint32_t>(instruction_index);
            operation.before_position = before_position;
            operation.after_position = before_position + 1U;

            std::set<std::uint32_t> before_values(
                liveness.instructions[block_index][instruction_index].live_before.begin(),
                liveness.instructions[block_index][instruction_index].live_before.end());
            before_values.insert(instruction.uses.begin(), instruction.uses.end());
            std::set<std::uint32_t> after_values(
                liveness.instructions[block_index][instruction_index].live_after.begin(),
                liveness.instructions[block_index][instruction_index].live_after.end());
            after_values.insert(instruction.definitions.begin(), instruction.definitions.end());

            for (const auto value_id : before_values) {
                NativeRegisterFlowValueLocation location;
                const int state = find_location(static_cast<NativeRegisterBlockId>(block_index), value_id,
                                                before_position, location);
                if (state != 1) {
                    add_diagnostic(result.diagnostics, state < 0 ? "N112E0010" : "N112E0011",
                        state < 0 ? "multiple split segments cover one value before a Flow operation" :
                                    "no allocated split segment covers a value before a Flow operation");
                    return result;
                }
                operation.before_locations.push_back(location);
            }
            for (const auto value_id : after_values) {
                NativeRegisterFlowValueLocation location;
                const int state = find_location(static_cast<NativeRegisterBlockId>(block_index), value_id,
                                                before_position + 1U, location);
                if (state != 1) {
                    add_diagnostic(result.diagnostics, state < 0 ? "N112E0012" : "N112E0013",
                        state < 0 ? "multiple split segments cover one value after a Flow operation" :
                                    "no allocated split segment covers a value after a Flow operation");
                    return result;
                }
                operation.after_locations.push_back(location);
            }
            operation_by_native_index.emplace(
                std::make_pair(static_cast<NativeRegisterBlockId>(block_index),
                               static_cast<std::uint32_t>(instruction_index)), result.operations.size());
            result.operations.push_back(std::move(operation));
        }
    }

    auto find_operation_value = [&](const NativeRegisterFlowOperationLocation& operation,
                                    std::uint32_t value_id, bool before,
                                    NativeRegisterFlowValueLocation& location) {
        const auto& values = before ? operation.before_locations : operation.after_locations;
        const auto found = std::find_if(values.begin(), values.end(), [value_id](const auto& item) {
            return item.value_id == value_id;
        });
        if (found == values.end()) return false;
        location = *found;
        return true;
    };
    struct ActionIdentity {
        std::uint64_t operation_id;
        unsigned int kind;
        std::uint32_t value_id;
        bool operator<(const ActionIdentity& other) const {
            return std::tie(operation_id, kind, value_id) <
                   std::tie(other.operation_id, other.kind, other.value_id);
        }
    };
    std::set<ActionIdentity> unique_actions;
    for (const auto& action : allocation.actions) {
        if (action.block_id >= normalized.blocks.size() ||
            action.instruction_index >= normalized.blocks[action.block_id].instructions.size()) {
            add_diagnostic(result.diagnostics, "N112E0014", "spill/reload action references an invalid native instruction");
            return result;
        }
        const auto operation_index = operation_by_native_index.find(
            std::make_pair(action.block_id, action.instruction_index));
        if (operation_index == operation_by_native_index.end()) {
            add_diagnostic(result.diagnostics, "N112E0015",
                "spill/reload action targets a synthetic instruction without a stable Flow operation anchor");
            return result;
        }
        const auto& instruction = normalized.blocks[action.block_id].instructions[action.instruction_index];
        const auto& operation = result.operations[operation_index->second];
        const auto value = value_classes.find(action.value_id);
        if (value == value_classes.end()) {
            add_diagnostic(result.diagnostics, "N112E0016", "spill/reload action references an unknown virtual value");
            return result;
        }

        NativeRegisterFlowActionPlacement placement = NativeRegisterFlowActionPlacement::BeforeOperation;
        bool compatible = false;
        bool compare_source_to_before = false;
        bool compare_destination_to_after = false;
        switch (action.kind) {
        case NativeSpillReloadActionKind::StoreBeforeCall:
            compatible = instruction.is_call && instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowCall &&
                (action.source.kind == NativeRegisterLocationKind::Register ||
                 action.source.kind == NativeRegisterLocationKind::SpillSlot) &&
                action.destination.kind == NativeRegisterLocationKind::HomeSlot &&
                action.destination.spill_slot == action.slot_index && action.slot_index < allocation.home_slot_count;
            compare_source_to_before = true;
            placement = NativeRegisterFlowActionPlacement::BeforeOperation;
            break;
        case NativeSpillReloadActionKind::ReloadAfterCall:
            compatible = instruction.is_call && instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowCall &&
                action.source.kind == NativeRegisterLocationKind::HomeSlot &&
                action.source.spill_slot == action.slot_index && action.slot_index < allocation.home_slot_count &&
                (action.destination.kind == NativeRegisterLocationKind::Register ||
                 action.destination.kind == NativeRegisterLocationKind::SpillSlot);
            compare_destination_to_after = true;
            placement = NativeRegisterFlowActionPlacement::AfterOperation;
            break;
        case NativeSpillReloadActionKind::StoreAfterDefinition:
            compatible = !instruction.is_call &&
                (instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowWrite ||
                 instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowDeclare) &&
                std::find(instruction.definitions.begin(), instruction.definitions.end(), action.value_id) != instruction.definitions.end() &&
                action.source.kind == NativeRegisterLocationKind::Temporary &&
                action.destination.kind == NativeRegisterLocationKind::SpillSlot &&
                action.destination.spill_slot == action.slot_index && action.slot_index < allocation.spill_slot_count;
            compare_destination_to_after = true;
            placement = NativeRegisterFlowActionPlacement::AfterOperation;
            break;
        case NativeSpillReloadActionKind::ReloadBeforeUse:
            compatible = !instruction.is_call && instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowRead &&
                std::find(instruction.uses.begin(), instruction.uses.end(), action.value_id) != instruction.uses.end() &&
                action.source.kind == NativeRegisterLocationKind::SpillSlot &&
                action.source.spill_slot == action.slot_index && action.slot_index < allocation.spill_slot_count &&
                action.destination.kind == NativeRegisterLocationKind::Temporary;
            compare_source_to_before = true;
            placement = NativeRegisterFlowActionPlacement::BeforeOperation;
            break;
        default:
            compatible = false;
            break;
        }
        if (!compatible) {
            add_diagnostic(result.diagnostics, "N112E0017",
                "spill/reload action kind, Flow operation, slot bounds, or location roles are inconsistent");
            return result;
        }
        NativeRegisterFlowValueLocation mapped_value;
        if (compare_source_to_before &&
            (!find_operation_value(operation, action.value_id, true, mapped_value) ||
             !same_location(mapped_value.location, action.source))) {
            add_diagnostic(result.diagnostics, "N112E0018",
                "spill/reload source does not match the segment location at the pre-operation anchor");
            return result;
        }
        if (compare_destination_to_after &&
            (!find_operation_value(operation, action.value_id, false, mapped_value) ||
             !same_location(mapped_value.location, action.destination))) {
            add_diagnostic(result.diagnostics, "N112E0019",
                "spill/reload destination does not match the segment location at the post-operation anchor");
            return result;
        }

        const ActionIdentity identity{operation.flow_operation_id,
            static_cast<unsigned int>(action.kind), action.value_id};
        if (!unique_actions.insert(identity).second) {
            add_diagnostic(result.diagnostics, "N112E0020",
                "duplicate spill/reload action is bound to the same stable Flow operation and value");
            return result;
        }
        result.actions.push_back(NativeRegisterFlowSegmentActionBinding{
            action, operation.flow_operation_id, operation.flow_block_id,
            operation.flow_operation_index, instruction.origin_kind, value->second, placement});
    }

    std::sort(result.segments.begin(), result.segments.end(), [](const auto& left, const auto& right) {
        return std::tie(left.segment.block_id, left.segment.start, left.segment.end, left.segment.value_id,
                        left.segment.segment_id) <
               std::tie(right.segment.block_id, right.segment.start, right.segment.end, right.segment.value_id,
                        right.segment.segment_id);
    });
    std::sort(result.operations.begin(), result.operations.end(), [](const auto& left, const auto& right) {
        return left.flow_operation_id < right.flow_operation_id;
    });
    std::sort(result.actions.begin(), result.actions.end(), [](const auto& left, const auto& right) {
        return std::tie(left.flow_operation_id, left.placement, left.action.kind, left.action.value_id) <
               std::tie(right.flow_operation_id, right.placement, right.action.kind, right.action.value_id);
    });
    result.succeeded = true;
    return result;
}

NativeWin64FramePlan build_native_win64_frame_plan(
    const NativeRegisterAllocationPlan& allocation,
    std::uint32_t local_area_bytes) {
    NativeWin64FramePlan result;
    if (!allocation.succeeded) {
        add_diagnostic(result.diagnostics, "N71E0001", "Win64 frame planner received a failed register-allocation plan");
        return result;
    }

    std::set<std::uint8_t> gp_registers;
    std::set<std::uint8_t> xmm_registers;
    const std::set<std::uint8_t> valid_gp{3U, 6U, 7U, 12U, 13U, 14U, 15U};
    for (const auto& segment : allocation.segments) {
        if (segment.assignment.spilled || !segment.assignment.requires_callee_save) continue;
        if (segment.assignment.register_class == NativeRegisterClass::GeneralPurpose) {
            if (valid_gp.find(segment.assignment.register_index) == valid_gp.end()) {
                add_diagnostic(result.diagnostics, "N71E0002", "allocation plan contains a non-Win64 nonvolatile GP register");
                return result;
            }
            gp_registers.insert(segment.assignment.register_index);
        } else {
            if (segment.assignment.register_index < 6U || segment.assignment.register_index > 15U) {
                add_diagnostic(result.diagnostics, "N71E0003", "allocation plan contains a volatile XMM register marked callee-saved");
                return result;
            }
            xmm_registers.insert(segment.assignment.register_index);
        }
    }

    result.local_area_bytes = local_area_bytes;
    std::uint64_t cursor = local_area_bytes;
    cursor = (cursor + 7U) & ~std::uint64_t(7U);
    const std::uint64_t spill_bytes = static_cast<std::uint64_t>(allocation.spill_slot_count) * 8U;
    const std::uint64_t home_bytes = static_cast<std::uint64_t>(allocation.home_slot_count) * 8U;
    if (spill_bytes > std::numeric_limits<std::uint32_t>::max() ||
        home_bytes > std::numeric_limits<std::uint32_t>::max()) {
        add_diagnostic(result.diagnostics, "N71E0009", "Win64 spill or home area exceeds the 32-bit backend limit");
        return result;
    }
    result.spill_area_bytes = static_cast<std::uint32_t>(spill_bytes);
    for (std::uint32_t slot = 0U; slot < allocation.spill_slot_count; ++slot) {
        cursor += 8U;
        if (cursor > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            add_diagnostic(result.diagnostics, "N71E0004", "Win64 spill area exceeds signed RBP-relative offset range");
            return result;
        }
        result.spill_slot_offsets.push_back(-static_cast<std::int32_t>(cursor));
    }
    result.home_area_bytes = static_cast<std::uint32_t>(home_bytes);
    for (std::uint32_t slot = 0U; slot < allocation.home_slot_count; ++slot) {
        cursor += 8U;
        if (cursor > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            add_diagnostic(result.diagnostics, "N71E0005", "Win64 canonical home area exceeds signed RBP-relative offset range");
            return result;
        }
        result.home_slot_offsets.push_back(-static_cast<std::int32_t>(cursor));
    }

    for (const std::uint8_t reg : gp_registers) {
        cursor = (cursor + 7U) & ~std::uint64_t(7U);
        cursor += 8U;
        if (cursor > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            add_diagnostic(result.diagnostics, "N71E0006", "Win64 GP save area exceeds signed RBP-relative offset range");
            return result;
        }
        result.prologue_saves.push_back(NativeWin64RegisterSave{
            NativeRegisterClass::GeneralPurpose, reg, -static_cast<std::int32_t>(cursor), 8U});
    }
    for (const std::uint8_t reg : xmm_registers) {
        cursor = (cursor + 15U) & ~std::uint64_t(15U);
        cursor += 16U;
        if (cursor > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            add_diagnostic(result.diagnostics, "N71E0007", "Win64 XMM save area exceeds signed RBP-relative offset range");
            return result;
        }
        result.prologue_saves.push_back(NativeWin64RegisterSave{
            NativeRegisterClass::FloatingPoint, reg, -static_cast<std::int32_t>(cursor), 16U});
    }

    const std::uint64_t aligned_frame = (cursor + 15U) & ~std::uint64_t(15U);
    if (aligned_frame > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        add_diagnostic(result.diagnostics, "N71E0008", "Win64 frame size exceeds signed 32-bit backend displacement limit");
        return result;
    }
    result.frame_size_bytes = static_cast<std::uint32_t>(aligned_frame);
    result.callee_save_area_bytes = static_cast<std::uint32_t>(gp_registers.size() * 8U + xmm_registers.size() * 16U);
    result.epilogue_restores = result.prologue_saves;
    std::reverse(result.epilogue_restores.begin(), result.epilogue_restores.end());
    result.succeeded = true;
    return result;
}

} // namespace xfrontend
