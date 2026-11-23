#include "xfrontend/x64_location_transfer.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace xfrontend {
namespace {

void add_diagnostic(std::vector<NativeRegisterIRDiagnostic>& diagnostics,
                    const char* code, const char* message) {
    diagnostics.push_back(NativeRegisterIRDiagnostic{code, message});
}

bool same_location(const NativeRegisterLocation& left,
                   const NativeRegisterLocation& right) {
    return left.kind == right.kind && left.register_index == right.register_index &&
           left.spill_slot == right.spill_slot;
}

bool same_source_position(const SourcePosition& left, const SourcePosition& right) {
    return left.byte_offset == right.byte_offset && left.line == right.line &&
           left.column == right.column;
}

bool same_source_span(const SourceSpan& left, const SourceSpan& right) {
    return left.source_id == right.source_id &&
           same_source_position(left.start, right.start) &&
           same_source_position(left.end, right.end);
}

bool valid_gp_register(std::uint8_t index) {
    return index < 16U && index != 4U && index != 5U; // Keep RSP/RBP reserved.
}

bool valid_xmm_register(std::uint8_t index) {
    return index < 16U;
}

bool is_memory_location(const NativeRegisterLocation& location) {
    return location.kind == NativeRegisterLocationKind::SpillSlot ||
           location.kind == NativeRegisterLocationKind::HomeSlot;
}

NativeX64FlowOperandMismatchKind classify_operand_mismatch(
    const NativeRegisterLocation& flow_location,
    const NativeRegisterLocation& emitter_location,
    const NativeRegisterClass register_class,
    const bool flow_uses_frame,
    const bool emitter_uses_frame,
    const std::int32_t flow_frame_displacement,
    const std::int32_t emitter_frame_displacement,
    const bool register_class_matches,
    const bool location_matches) {
    if (!register_class_matches) return NativeX64FlowOperandMismatchKind::RegisterClassMismatch;
    if (location_matches) return NativeX64FlowOperandMismatchKind::None;

    if (flow_location.kind == NativeRegisterLocationKind::Register &&
        emitter_location.kind == NativeRegisterLocationKind::Register) {
        const bool valid_registers = register_class == NativeRegisterClass::GeneralPurpose
            ? valid_gp_register(flow_location.register_index) && valid_gp_register(emitter_location.register_index)
            : valid_xmm_register(flow_location.register_index) && valid_xmm_register(emitter_location.register_index);
        if (!valid_registers) return NativeX64FlowOperandMismatchKind::InvalidRegister;
        return flow_location.register_index == emitter_location.register_index
            ? NativeX64FlowOperandMismatchKind::EvidenceDisagreement
            : NativeX64FlowOperandMismatchKind::RegisterIndexMismatch;
    }
    if (flow_location.kind == NativeRegisterLocationKind::Register &&
        is_memory_location(emitter_location)) {
        return NativeX64FlowOperandMismatchKind::FlowRegisterEmitterMemory;
    }
    if (is_memory_location(flow_location) &&
        emitter_location.kind == NativeRegisterLocationKind::Register) {
        return NativeX64FlowOperandMismatchKind::FlowMemoryEmitterRegister;
    }
    if (is_memory_location(flow_location) &&
        emitter_location.kind == NativeRegisterLocationKind::HomeSlot) {
        if (flow_uses_frame && emitter_uses_frame &&
            flow_frame_displacement < 0 && emitter_frame_displacement < 0 &&
            (flow_frame_displacement % 8) == 0 && (emitter_frame_displacement % 8) == 0) {
            return flow_frame_displacement == emitter_frame_displacement
                ? NativeX64FlowOperandMismatchKind::EvidenceDisagreement
                : NativeX64FlowOperandMismatchKind::FrameDisplacementMismatch;
        }
        return NativeX64FlowOperandMismatchKind::FrameMappingUnavailable;
    }
    return NativeX64FlowOperandMismatchKind::UnsupportedLocationKind;
}

void accumulate_mismatch_count(NativeX64FlowOperandMismatchCounts& counts,
                               const NativeX64FlowOperandMismatchKind kind) {
    switch (kind) {
    case NativeX64FlowOperandMismatchKind::None:
        break;
    case NativeX64FlowOperandMismatchKind::RegisterIndexMismatch:
        ++counts.register_index;
        break;
    case NativeX64FlowOperandMismatchKind::InvalidRegister:
        ++counts.invalid_register;
        break;
    case NativeX64FlowOperandMismatchKind::FlowRegisterEmitterMemory:
        ++counts.flow_register_emitter_memory;
        break;
    case NativeX64FlowOperandMismatchKind::FlowMemoryEmitterRegister:
        ++counts.flow_memory_emitter_register;
        break;
    case NativeX64FlowOperandMismatchKind::FrameDisplacementMismatch:
        ++counts.frame_displacement;
        break;
    case NativeX64FlowOperandMismatchKind::FrameMappingUnavailable:
        ++counts.frame_mapping_unavailable;
        break;
    case NativeX64FlowOperandMismatchKind::EvidenceDisagreement:
        ++counts.evidence_disagreement;
        break;
    case NativeX64FlowOperandMismatchKind::UnsupportedLocationKind:
        ++counts.unsupported_location;
        break;
    case NativeX64FlowOperandMismatchKind::RegisterClassMismatch:
        ++counts.register_class;
        break;
    }
}

bool find_flow_value_location(const NativeRegisterFlowOperationLocation& operation,
                              const bool after,
                              const std::uint32_t value_id,
                              NativeRegisterLocation& location) {
    const auto& values = after ? operation.after_locations : operation.before_locations;
    bool found = false;
    for (const auto& value : values) {
        if (value.value_id != value_id) continue;
        if (found) return false;
        location = value.location;
        found = true;
    }
    return found;
}

bool requires_cycle_temporary(const NativeSpillReloadAction& action) {
    return action.source.kind == NativeRegisterLocationKind::Temporary ||
           action.destination.kind == NativeRegisterLocationKind::Temporary ||
           (is_memory_location(action.source) && is_memory_location(action.destination));
}

std::uint8_t select_cycle_temporary(
    const NativeRegisterClass register_class,
    const std::set<std::uint8_t>& expression_reserved,
    const std::vector<const NativeRegisterFlowOperationLocation*>& operations,
    std::uint8_t& selected) {
    // Stay within the allocator's caller-saved pools. RAX/RCX/RDX and XMM0
    // are fixed backend/ABI temporaries and are not safe insertion scratch.
    static constexpr std::array<std::uint8_t, 4U> gp_candidates{{8U, 9U, 10U, 11U}};
    static constexpr std::array<std::uint8_t, 5U> xmm_candidates{{1U, 2U, 3U, 4U, 5U}};
    const auto try_candidate = [&](const std::uint8_t candidate) {
        if (expression_reserved.find(candidate) != expression_reserved.end()) return false;
        for (const auto* operation : operations) {
            if (operation == nullptr) return false;
            const auto has_conflict = [candidate, register_class](
                const std::vector<NativeRegisterFlowValueLocation>& values) {
                return std::any_of(values.begin(), values.end(), [candidate, register_class](const auto& value) {
                    return value.register_class == register_class &&
                           value.location.kind == NativeRegisterLocationKind::Register &&
                           value.location.register_index == candidate;
                });
            };
            if (has_conflict(operation->before_locations) || has_conflict(operation->after_locations))
                return false;
        }
        selected = candidate;
        return true;
    };

    if (register_class == NativeRegisterClass::GeneralPurpose) {
        for (const auto candidate : gp_candidates) if (try_candidate(candidate)) return 1U;
    } else {
        for (const auto candidate : xmm_candidates) if (try_candidate(candidate)) return 1U;
    }
    return 0U;
}

void emit_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

struct ResolvedLocation {
    bool is_register = false;
    std::uint8_t register_index = 0U;
    std::int32_t rbp_displacement = 0;
};

bool resolve_location(const NativeRegisterLocation& location,
                      const NativeRegisterClass register_class,
                      const NativeRegisterLocation& cycle_temporary,
                      const NativeX64FrameLocationMap& frame,
                      ResolvedLocation& output,
                      std::vector<NativeRegisterIRDiagnostic>& diagnostics) {
    if (location.kind == NativeRegisterLocationKind::Register) {
        const bool valid = register_class == NativeRegisterClass::GeneralPurpose
            ? valid_gp_register(location.register_index)
            : valid_xmm_register(location.register_index);
        if (!valid) {
            add_diagnostic(diagnostics, "N109BE0001", "parallel-copy schedule references an invalid physical register");
            return false;
        }
        output.is_register = true;
        output.register_index = location.register_index;
        return true;
    }
    if (location.kind == NativeRegisterLocationKind::Temporary) {
        if (cycle_temporary.kind != NativeRegisterLocationKind::Temporary ||
            location.register_index != cycle_temporary.register_index) {
            add_diagnostic(diagnostics, "N109BE0002", "parallel-copy temporary does not match the caller-reserved cycle register");
            return false;
        }
        const bool valid = register_class == NativeRegisterClass::GeneralPurpose
            ? valid_gp_register(location.register_index)
            : valid_xmm_register(location.register_index);
        if (!valid) {
            add_diagnostic(diagnostics, "N109BE0003", "caller-reserved parallel-copy temporary is not a valid register");
            return false;
        }
        output.is_register = true;
        output.register_index = location.register_index;
        return true;
    }

    const auto& slots = location.kind == NativeRegisterLocationKind::SpillSlot
        ? frame.spill_slot_offsets : frame.home_slot_offsets;
    const auto found = slots.find(location.spill_slot);
    if (found == slots.end()) {
        add_diagnostic(diagnostics, "N109BE0004", "parallel-copy stack location has no frame displacement");
        return false;
    }
    const std::int32_t displacement = found->second;
    if (displacement >= 0 || (displacement % 8) != 0) {
        add_diagnostic(diagnostics, "N109BE0005", "Win64 RBP-relative transfer slot must be negative and 8-byte aligned");
        return false;
    }
    output.is_register = false;
    output.rbp_displacement = displacement;
    return true;
}

void emit_gp_move(std::vector<std::uint8_t>& bytes,
                  const ResolvedLocation& source,
                  const ResolvedLocation& destination) {
    if (source.is_register && destination.is_register) {
        const std::uint8_t rex = static_cast<std::uint8_t>(0x48U |
            (destination.register_index >= 8U ? 0x04U : 0U) |
            (source.register_index >= 8U ? 0x01U : 0U));
        bytes.push_back(rex);
        bytes.push_back(0x8BU); // mov r64, r/m64
        bytes.push_back(static_cast<std::uint8_t>(0xC0U |
            ((destination.register_index & 7U) << 3U) | (source.register_index & 7U)));
        return;
    }
    if (!source.is_register && destination.is_register) {
        const std::uint8_t rex = static_cast<std::uint8_t>(0x48U |
            (destination.register_index >= 8U ? 0x04U : 0U));
        bytes.push_back(rex);
        bytes.push_back(0x8BU);
        bytes.push_back(static_cast<std::uint8_t>(0x85U | ((destination.register_index & 7U) << 3U)));
        emit_u32(bytes, static_cast<std::uint32_t>(source.rbp_displacement));
        return;
    }
    if (source.is_register && !destination.is_register) {
        const std::uint8_t rex = static_cast<std::uint8_t>(0x48U |
            (source.register_index >= 8U ? 0x04U : 0U));
        bytes.push_back(rex);
        bytes.push_back(0x89U); // mov r/m64, r64
        bytes.push_back(static_cast<std::uint8_t>(0x85U | ((source.register_index & 7U) << 3U)));
        emit_u32(bytes, static_cast<std::uint32_t>(destination.rbp_displacement));
    }
}

void emit_xmm_move(std::vector<std::uint8_t>& bytes,
                   const ResolvedLocation& source,
                   const ResolvedLocation& destination) {
    if (source.is_register && destination.is_register) {
        bytes.push_back(0xF3U);
        const std::uint8_t rex = static_cast<std::uint8_t>(0x40U |
            (destination.register_index >= 8U ? 0x04U : 0U) |
            (source.register_index >= 8U ? 0x01U : 0U));
        if (rex != 0x40U) bytes.push_back(rex);
        bytes.push_back(0x0FU);
        bytes.push_back(0x7EU); // movq xmm, xmm/m64
        bytes.push_back(static_cast<std::uint8_t>(0xC0U |
            ((destination.register_index & 7U) << 3U) | (source.register_index & 7U)));
        return;
    }
    if (!source.is_register && destination.is_register) {
        bytes.push_back(0xF3U);
        if (destination.register_index >= 8U) bytes.push_back(0x44U); // REX.R
        bytes.push_back(0x0FU);
        bytes.push_back(0x7EU);
        bytes.push_back(static_cast<std::uint8_t>(0x85U | ((destination.register_index & 7U) << 3U)));
        emit_u32(bytes, static_cast<std::uint32_t>(source.rbp_displacement));
        return;
    }
    if (source.is_register && !destination.is_register) {
        bytes.push_back(0x66U);
        if (source.register_index >= 8U) bytes.push_back(0x44U); // REX.R follows legacy prefix.
        bytes.push_back(0x0FU);
        bytes.push_back(0xD6U); // movq m64, xmm
        bytes.push_back(static_cast<std::uint8_t>(0x85U | ((source.register_index & 7U) << 3U)));
        emit_u32(bytes, static_cast<std::uint32_t>(destination.rbp_displacement));
    }
}

using ActionKey = std::tuple<std::uint64_t, unsigned int, std::uint32_t>;

struct InsertedBytes {
    std::uint32_t offset = 0U;
    std::vector<std::uint8_t> bytes;
};

std::uint32_t map_offset_before_insertions(
    std::uint32_t offset,
    const std::vector<NativeX64CodeInsertionAdjustment>& adjustments) {
    std::uint64_t mapped = offset;
    for (const auto& item : adjustments) {
        if (item.original_offset < offset) mapped += item.inserted_bytes;
    }
    return mapped > std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max() : static_cast<std::uint32_t>(mapped);
}

std::uint32_t map_offset_after_insertions(
    std::uint32_t offset,
    const std::vector<NativeX64CodeInsertionAdjustment>& adjustments) {
    std::uint64_t mapped = offset;
    for (const auto& item : adjustments) {
        if (item.original_offset <= offset) mapped += item.inserted_bytes;
    }
    return mapped > std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max() : static_cast<std::uint32_t>(mapped);
}

bool patch_relative32(std::vector<std::uint8_t>& code,
                      std::uint32_t displacement_offset,
                      std::uint32_t target_offset,
                      std::vector<NativeRegisterIRDiagnostic>& diagnostics,
                      const char* error_code) {
    if (static_cast<std::uint64_t>(displacement_offset) + 4U > code.size() ||
        target_offset > code.size()) {
        add_diagnostic(diagnostics, error_code, "relative branch fixup lies outside transformed function code");
        return false;
    }
    const std::int64_t delta = static_cast<std::int64_t>(target_offset) -
        static_cast<std::int64_t>(displacement_offset + 4U);
    if (delta < std::numeric_limits<std::int32_t>::min() ||
        delta > std::numeric_limits<std::int32_t>::max()) {
        add_diagnostic(diagnostics, error_code, "transformed relative branch displacement exceeds rel32 range");
        return false;
    }
    const auto encoded = static_cast<std::uint32_t>(static_cast<std::int32_t>(delta));
    for (unsigned byte = 0U; byte < 4U; ++byte)
        code[displacement_offset + byte] = static_cast<std::uint8_t>((encoded >> (byte * 8U)) & 0xffU);
    return true;
}

bool encode_single_transfer(const NativeBoundSpillReloadAction& binding,
                            const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
                            NativeRegisterLocation gp_temporary,
                            NativeRegisterLocation xmm_temporary,
                            const NativeX64FrameLocationMap& frame,
                            NativeX64ParallelCopyEncodingResult& encoded) {
    NativeRegisterParallelCopyOperation operation;
    operation.value_id = binding.action.value_id;
    operation.source = binding.action.source;
    operation.destination = binding.action.destination;

    // A GP-typed memory-to-memory action may use a proven-dead volatile XMM1-XMM5
    // register solely as a 64-bit MOVQ expansion register. This is not a cycle
    // storage protocol: the action has one source and one destination, and the
    // XMM value is consumed immediately by the second MOVQ.
    if (binding.register_class == NativeRegisterClass::GeneralPurpose &&
        binding.cycle_temporary_assigned &&
        binding.cycle_temporary_register_class == NativeRegisterClass::FloatingPoint) {
        if (!is_memory_location(binding.action.source) ||
            !is_memory_location(binding.action.destination) ||
            binding.cycle_temporary.kind != NativeRegisterLocationKind::Temporary ||
            binding.cycle_temporary.register_index < 1U ||
            binding.cycle_temporary.register_index > 5U) {
            add_diagnostic(encoded.diagnostics, "N113G24E0101",
                "GP XMM expansion requires two frame-memory endpoints and a volatile XMM1-XMM5 Temporary");
            return false;
        }
        const auto value_class = value_classes.find(binding.action.value_id);
        if (value_class == value_classes.end() || value_class->second != NativeRegisterClass::GeneralPurpose) {
            add_diagnostic(encoded.diagnostics, "N113G24E0102",
                "GP XMM expansion action has no matching GP ValueId class");
            return false;
        }
        ResolvedLocation source;
        ResolvedLocation destination;
        if (!resolve_location(binding.action.source, NativeRegisterClass::GeneralPurpose,
                              gp_temporary, frame, source, encoded.diagnostics) ||
            !resolve_location(binding.action.destination, NativeRegisterClass::GeneralPurpose,
                              gp_temporary, frame, destination, encoded.diagnostics)) return false;
        ResolvedLocation expansion;
        expansion.is_register = true;
        expansion.register_index = binding.cycle_temporary.register_index;
        emit_xmm_move(encoded.bytes, source, expansion);
        emit_xmm_move(encoded.bytes, expansion, destination);
        encoded.machine_moves_emitted = 2U;
        encoded.memory_to_memory_expansions = 1U;
        encoded.temporary_register_moves = 2U;
        encoded.succeeded = true;
        return true;
    }

    if (binding.cycle_temporary_assigned &&
        binding.cycle_temporary_register_class != binding.register_class) {
        add_diagnostic(encoded.diagnostics, "N113G24E0103",
            "cross-bank expansion is supported only for GP memory-to-memory MOVQ transfers");
        return false;
    }
    const NativeRegisterLocation temporary = binding.cycle_temporary_assigned
        ? binding.cycle_temporary
        : (binding.register_class == NativeRegisterClass::GeneralPurpose ? gp_temporary : xmm_temporary);
    encoded = encode_native_x64_parallel_copy_schedule(
        {operation}, value_classes, temporary, frame);
    return encoded.succeeded;
}

} // namespace

NativeX64FlowFrameBindingPlan plan_native_x64_flow_frame_binding(
    const NativeRegisterAllocationPlan& emitter_allocation,
    const NativeRegisterAllocationPlan& flow_allocation,
    const std::uint32_t local_area_bytes) {
    NativeX64FlowFrameBindingPlan result;
    if (!emitter_allocation.succeeded || !flow_allocation.succeeded) {
        add_diagnostic(result.diagnostics, "N113F0001",
            "Flow frame binding requires successful emitter and Flow allocation plans");
        return result;
    }
    if (flow_allocation.spill_slot_count >
            std::numeric_limits<std::uint32_t>::max() - emitter_allocation.spill_slot_count ||
        flow_allocation.home_slot_count >
            std::numeric_limits<std::uint32_t>::max() - emitter_allocation.home_slot_count) {
        add_diagnostic(result.diagnostics, "N113F0002",
            "combined emitter and Flow frame slot counts exceed the 32-bit backend limit");
        return result;
    }

    NativeRegisterAllocationPlan combined = emitter_allocation;
    combined.spill_slot_count += flow_allocation.spill_slot_count;
    combined.home_slot_count += flow_allocation.home_slot_count;
    result.frame_plan = build_native_win64_frame_plan(combined, local_area_bytes);
    if (!result.frame_plan.succeeded) {
        result.diagnostics = result.frame_plan.diagnostics;
        return result;
    }

    result.flow_spill_slots_reserved = flow_allocation.spill_slot_count;
    result.flow_home_slots_reserved = flow_allocation.home_slot_count;
    for (std::uint32_t slot = 0U; slot < flow_allocation.spill_slot_count; ++slot) {
        const std::uint64_t frame_index =
            static_cast<std::uint64_t>(emitter_allocation.spill_slot_count) + slot;
        if (frame_index >= result.frame_plan.spill_slot_offsets.size()) {
            add_diagnostic(result.diagnostics, "N113F0003",
                "Flow spill-slot index is outside the combined Win64 frame plan");
            return result;
        }
        result.flow_frame_locations.spill_slot_offsets.emplace(
            slot, result.frame_plan.spill_slot_offsets[static_cast<std::size_t>(frame_index)]);
    }
    for (std::uint32_t slot = 0U; slot < flow_allocation.home_slot_count; ++slot) {
        const std::uint64_t frame_index =
            static_cast<std::uint64_t>(emitter_allocation.home_slot_count) + slot;
        if (frame_index >= result.frame_plan.home_slot_offsets.size()) {
            add_diagnostic(result.diagnostics, "N113F0004",
                "Flow home-slot index is outside the combined Win64 frame plan");
            return result;
        }
        result.flow_frame_locations.home_slot_offsets.emplace(
            slot, result.frame_plan.home_slot_offsets[static_cast<std::size_t>(frame_index)]);
    }
    result.succeeded = true;
    return result;
}

NativeX64FlowEdgeLocationAuditPlan audit_native_x64_flow_edge_locations(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterIntervalAnalysis& intervals,
    const NativeRegisterFlowSegmentPlan& flow_segments,
    const NativeRegisterHomePhiPlan& home_phi_plan,
    const NativeX64MachineLocationPlan& emitter_locations,
    const NativeX64FrameLocationMap& flow_frame_locations) {
    NativeX64FlowEdgeLocationAuditPlan result;
    const auto fail = [&](const char* code, const std::string& message) {
        result.diagnostics.push_back(NativeRegisterIRDiagnostic{code, message});
        return result;
    };

    const auto validated = validate_native_register_function_ir(function.register_ir);
    if (!validated.succeeded)
        return fail("N113H0001", "G4 received an invalid NativeRegisterFlowFunction IR");
    const auto& ir = validated.normalized_function;
    if (!liveness.succeeded || !liveness.converged ||
        liveness.blocks.size() != ir.blocks.size() ||
        liveness.instructions.size() != ir.blocks.size() ||
        !intervals.succeeded ||
        intervals.block_start_positions.size() != ir.blocks.size() ||
        intervals.instruction_positions.size() != ir.blocks.size() ||
        !flow_segments.succeeded || !emitter_locations.succeeded ||
        !home_phi_plan.succeeded) {
        return fail("N113H0002", "G4 requires converged liveness, complete N112 segments, N111 locations and a successful home-Phi plan");
    }

    std::map<std::uint32_t, NativeRegisterClass> value_classes;
    for (const auto& value : ir.values) {
        if (value.value_id == 0U || !value_classes.emplace(value.value_id, value.register_class).second)
            return fail("N113H0003", "G4 Flow value table contains an invalid or duplicate value ID");
    }
    if (emitter_locations.values.size() != value_classes.size())
        return fail("N113H0004", "G4 N111 location table does not cover every Flow value exactly once");
    for (const auto& item : emitter_locations.values) {
        const auto type = value_classes.find(item.first);
        if (type == value_classes.end() || item.second.value_id != item.first ||
            item.second.register_class != type->second ||
            (item.second.active_location.kind != NativeRegisterLocationKind::Register &&
             item.second.active_location.kind != NativeRegisterLocationKind::HomeSlot)) {
            return fail("N113H0005", "G4 N111 active location is missing, abstract, or register-class inconsistent");
        }
        if (item.second.active_location.kind == NativeRegisterLocationKind::Register &&
            item.second.active_location.register_index >= 16U)
            return fail("N113H0006", "G4 N111 active register is outside the AMD64 register file");
        if (item.second.active_location.kind == NativeRegisterLocationKind::HomeSlot &&
            item.second.active_location.spill_slot == InvalidNativeX64HomeSlot)
            return fail("N113H0007", "G4 N111 active HomeSlot is still an unresolved sentinel");
    }

    std::map<std::uint32_t, const NativeRegisterFlowSegmentLocation*> segment_ids;
    for (const auto& segment : flow_segments.segments) {
        const auto type = value_classes.find(segment.segment.value_id);
        if (segment.segment.segment_id == 0U || type == value_classes.end() ||
            segment.segment.block_id >= ir.blocks.size() ||
            segment.segment.start > segment.segment.end ||
            segment.segment.register_class != type->second ||
            !segment_ids.emplace(segment.segment.segment_id, &segment).second)
            return fail("N113H0008", "G4 N112 segment table contains an invalid, duplicate, or inconsistent segment");
        if (segment.location.kind == NativeRegisterLocationKind::Register) {
            if (segment.location.register_index >= 16U)
                return fail("N113H0009", "G4 N112 segment references a register outside the AMD64 register file");
        } else if (segment.location.kind == NativeRegisterLocationKind::SpillSlot) {
            const auto offset = flow_frame_locations.spill_slot_offsets.find(segment.location.spill_slot);
            if (offset == flow_frame_locations.spill_slot_offsets.end() || offset->second >= 0 ||
                (offset->second % 8) != 0)
                return fail("N113H000A", "G4 N112 spill segment has no unique negative aligned Flow-frame displacement");
        } else if (segment.location.kind == NativeRegisterLocationKind::HomeSlot) {
            const auto offset = flow_frame_locations.home_slot_offsets.find(segment.location.spill_slot);
            if (offset == flow_frame_locations.home_slot_offsets.end() || offset->second >= 0 ||
                (offset->second % 8) != 0)
                return fail("N113H000B", "G4 N112 home segment has no unique negative aligned Flow-frame displacement");
        } else {
            return fail("N113H000C", "G4 N112 segment contains an abstract Temporary location");
        }
    }

    const auto same_flow_location = [](const NativeRegisterLocation& left,
                                       const NativeRegisterLocation& right) {
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
    const auto find_segment_location = [&](NativeRegisterBlockId block_id,
                                           std::uint32_t value_id,
                                           std::uint32_t position,
                                           NativeRegisterLocation& location) {
        const NativeRegisterFlowSegmentLocation* match = nullptr;
        for (const auto& candidate : flow_segments.segments) {
            if (candidate.segment.block_id != block_id || candidate.segment.value_id != value_id ||
                candidate.segment.start > position || candidate.segment.end < position)
                continue;
            if (match != nullptr) return -1;
            match = &candidate;
        }
        if (match == nullptr) return 0;
        location = match->location;
        return 1;
    };

    // Validate memory-SSA Phi aliases before looking at individual edges. The
    // current Emitter Phi model must map every incoming synthetic version and
    // the destination version back to exactly one original source value.
    std::map<std::uint32_t, std::uint32_t> home_aliases;
    for (const auto& alias : home_phi_plan.home_aliases) {
        if (alias.synthetic_value_id == 0U || alias.source_home_value_id == 0U ||
            alias.synthetic_value_id == alias.source_home_value_id ||
            value_classes.find(alias.source_home_value_id) == value_classes.end() ||
            !home_aliases.emplace(alias.synthetic_value_id, alias.source_home_value_id).second)
            return fail("N113H000D", "G4 home-Phi alias table contains an invalid, unknown, or duplicate alias");
    }
    std::set<std::pair<NativeRegisterBlockId, std::uint32_t>> unique_phis;
    for (const auto& phi : home_phi_plan.phis) {
        if (phi.block_id >= ir.blocks.size() || phi.destination_value_id == 0U ||
            !unique_phis.emplace(phi.block_id, phi.destination_value_id).second)
            return fail("N113H000E", "G4 home-Phi table contains an invalid or duplicate destination");
        const auto destination_alias = home_aliases.find(phi.destination_value_id);
        if (destination_alias == home_aliases.end())
            return fail("N113H000F", "G4 home-Phi destination has no canonical source-home alias");
        const auto& block = ir.blocks[phi.block_id];
        if (phi.incoming.size() != block.predecessors.size())
            return fail("N113H0010", "G4 home-Phi does not contain exactly one incoming version per CFG predecessor");
        std::set<NativeRegisterBlockId> predecessors;
        for (const auto& incoming : phi.incoming) {
            const auto source_alias = home_aliases.find(incoming.source_value_id);
            if (source_alias == home_aliases.end() || source_alias->second != destination_alias->second ||
                incoming.predecessor_block_id >= ir.blocks.size() ||
                std::find(block.predecessors.begin(), block.predecessors.end(), incoming.predecessor_block_id) == block.predecessors.end() ||
                !predecessors.insert(incoming.predecessor_block_id).second)
                return fail("N113H0011", "G4 home-Phi incoming version does not map to the same source home and exact predecessor");
            const auto edge = std::find_if(function.edges.begin(), function.edges.end(),
                [&](const FlowEdge& candidate) {
                    return candidate.from_block_id == incoming.predecessor_block_id &&
                           candidate.to_block_id == phi.block_id;
                });
            if (edge == function.edges.end())
                return fail("N113H0012", "G4 home-Phi incoming predecessor has no corresponding FlowEdge identity");
            ++result.home_phi_incoming_edges_audited;
        }
        ++result.home_phi_nodes_audited;
    }

    std::set<FlowEdgeId> edge_ids;
    std::set<std::pair<NativeRegisterBlockId, NativeRegisterBlockId>> edge_endpoints;
    for (const auto& edge : function.edges) {
        if (edge.id == InvalidFlowEdgeId || !edge_ids.insert(edge.id).second ||
            edge.from_block_id >= ir.blocks.size() || edge.to_block_id >= ir.blocks.size())
            return fail("N113H0013", "G4 FlowEdge table contains an invalid ID, duplicate, or out-of-range endpoint");
        const auto predecessor = static_cast<NativeRegisterBlockId>(edge.from_block_id);
        const auto successor = static_cast<NativeRegisterBlockId>(edge.to_block_id);
        if (ir.blocks[predecessor].id != predecessor || ir.blocks[successor].id != successor ||
            std::find(ir.blocks[predecessor].successors.begin(), ir.blocks[predecessor].successors.end(), successor) ==
                ir.blocks[predecessor].successors.end() ||
            std::find(ir.blocks[successor].predecessors.begin(), ir.blocks[successor].predecessors.end(), predecessor) ==
                ir.blocks[successor].predecessors.end() ||
            !edge_endpoints.emplace(predecessor, successor).second)
            return fail("N113H0014", "G4 FlowEdge identity disagrees with or duplicates NativeRegisterIR predecessor/successor topology");
        if (liveness.instructions[predecessor].size() != ir.blocks[predecessor].instructions.size() ||
            liveness.instructions[successor].size() != ir.blocks[successor].instructions.size() ||
            intervals.instruction_positions[predecessor].size() != ir.blocks[predecessor].instructions.size() ||
            intervals.instruction_positions[successor].size() != ir.blocks[successor].instructions.size())
            return fail("N113H0015", "G4 N112 block position/liveness shape is inconsistent with the CFG");

        ++result.edges_audited;
        const auto& pred_instructions = ir.blocks[predecessor].instructions;
        const auto& succ_instructions = ir.blocks[successor].instructions;
        const auto& pred_live_out = liveness.blocks[predecessor].live_out;
        const auto& succ_live_in = liveness.blocks[successor].live_in;
        std::set<std::uint32_t> live_out_ids(pred_live_out.begin(), pred_live_out.end());
        if (live_out_ids.size() != pred_live_out.size())
            return fail("N113H0016", "G4 predecessor live-out set contains duplicate value IDs");
        std::set<std::uint32_t> live_in_ids(succ_live_in.begin(), succ_live_in.end());
        if (live_in_ids.size() != succ_live_in.size())
            return fail("N113H0017", "G4 successor live-in set contains duplicate value IDs");

        for (const auto value_id : pred_live_out) {
            if (live_in_ids.find(value_id) == live_in_ids.end()) continue;
            if (pred_instructions.empty() || succ_instructions.empty())
                return fail("N113H0018", "G4 cannot bind a live-through value at an empty predecessor/successor block boundary");
            const auto type = value_classes.find(value_id);
            const auto emitter = emitter_locations.values.find(value_id);
            if (type == value_classes.end() || emitter == emitter_locations.values.end() ||
                emitter->second.register_class != type->second)
                return fail("N113H0019", "G4 live-through value has no authoritative N111 emitter location");
            const auto pred_last_position = intervals.instruction_positions[predecessor].back();
            const auto succ_first_position = intervals.instruction_positions[successor].front();
            if (pred_last_position == std::numeric_limits<std::uint32_t>::max())
                return fail("N113H001A", "G4 predecessor exit position overflows the N112 coordinate space");
            NativeRegisterLocation source_location;
            NativeRegisterLocation destination_location;
            const int source_state = find_segment_location(predecessor, value_id,
                pred_last_position + 1U, source_location);
            const int destination_state = find_segment_location(successor, value_id,
                succ_first_position, destination_location);
            if (source_state != 1 || destination_state != 1)
                return fail("N113H001B", "G4 live-through value has a missing or ambiguous N112 segment at an edge boundary");

            NativeX64FlowEdgeValueAudit audit;
            audit.edge_id = edge.id;
            audit.predecessor_block_id = predecessor;
            audit.successor_block_id = successor;
            audit.value_id = value_id;
            audit.register_class = type->second;
            audit.predecessor_flow_location = source_location;
            audit.successor_flow_location = destination_location;
            audit.emitter_active_location = emitter->second.active_location;
            audit.flow_location_changes = !same_flow_location(source_location, destination_location);
            const auto bind_frame = [&](const NativeRegisterLocation& location,
                                        bool& uses_frame, std::int32_t& displacement) {
                if (location.kind == NativeRegisterLocationKind::Register) return true;
                const auto& offsets = location.kind == NativeRegisterLocationKind::SpillSlot
                    ? flow_frame_locations.spill_slot_offsets : flow_frame_locations.home_slot_offsets;
                const auto found = offsets.find(location.spill_slot);
                if (found == offsets.end() || found->second >= 0 || (found->second % 8) != 0) return false;
                uses_frame = true;
                displacement = found->second;
                return true;
            };
            if (!bind_frame(source_location, audit.predecessor_uses_frame, audit.predecessor_frame_displacement) ||
                !bind_frame(destination_location, audit.successor_uses_frame, audit.successor_frame_displacement))
                return fail("N113H001C", "G4 edge boundary references an N112 frame slot without a valid physical displacement");
            if (audit.flow_location_changes) ++result.flow_location_changes;
            else ++result.flow_locations_coalesced;
            result.values.push_back(std::move(audit));
            ++result.live_values_audited;
        }
    }

    // The FlowEdge identity table must cover the complete CFG edge topology.
    // Partial edge metadata would otherwise make a successful audit look complete.
    for (const auto& block : ir.blocks) {
        for (const auto successor : block.successors) {
            if (edge_endpoints.find({block.id, successor}) == edge_endpoints.end())
                return fail("N113H001D", "G4 FlowEdge table omits a NativeRegisterIR CFG successor edge");
        }
    }
    if (edge_endpoints.size() != function.edges.size())
        return fail("N113H001E", "G4 FlowEdge table does not exactly match the CFG edge topology");

    // A Phi must be represented by one exact CFG edge per incoming predecessor.
    // The source-level Emitter still uses its N111 canonical home; differing
    // N112 segment locations are reported, not silently emitted as edge moves.
    result.succeeded = true;
    return result;
}

NativeX64FlowEdgeTransferCandidatePlan plan_native_x64_flow_edge_transfer_candidates(
    const NativeX64FlowEdgeLocationAuditPlan& audit,
    const std::vector<X64FlowBranchAnchor>& branch_anchors,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes) {
    NativeX64FlowEdgeTransferCandidatePlan result;
    const auto fail = [&](const char* code, const char* message) {
        add_diagnostic(result.diagnostics, code, message);
        return result;
    };
    if (!audit.succeeded) {
        return fail("N113H0020", "G6 requires a successful G4/G5 CFG edge-location audit");
    }

    std::map<FlowEdgeId, const X64FlowBranchAnchor*> anchors_by_edge;
    for (const auto& anchor : branch_anchors) {
        if (anchor.edge_id == InvalidFlowEdgeId ||
            !anchors_by_edge.emplace(anchor.edge_id, &anchor).second) {
            return fail("N113H0021", "G6 branch-anchor table contains an invalid or duplicate FlowEdgeId");
        }
        if (anchor.encoding != X64FlowBranchEncoding::Fallthrough &&
            anchor.encoding != X64FlowBranchEncoding::ConditionalRel32 &&
            anchor.encoding != X64FlowBranchEncoding::UnconditionalRel32) {
            return fail("N113H0029", "G6 branch anchor uses an unsupported encoding kind");
        }
        if (anchor.function_code_size == 0U ||
            anchor.instruction_offset > anchor.function_code_size ||
            anchor.target_offset > anchor.function_code_size) {
            return fail("N113H0029", "G6 branch anchor offsets exceed or omit the final function code extent");
        }
        if (anchor.encoding == X64FlowBranchEncoding::Fallthrough) {
            if (anchor.displacement_offset != InvalidNativeFlowOperationIndex) {
                return fail("N113H0029", "G6 fallthrough anchor unexpectedly carries a rel32 displacement");
            }
        } else if (anchor.displacement_offset == InvalidNativeFlowOperationIndex ||
                   anchor.displacement_offset > anchor.function_code_size ||
                   4U > anchor.function_code_size - anchor.displacement_offset) {
            return fail("N113H0029", "G6 rel32 branch anchor has an invalid displacement field");
        }
    }

    std::set<std::pair<FlowEdgeId, std::uint32_t>> unique_edge_values;
    std::set<FlowEdgeId> candidate_edges;
    const auto valid_location = [](const NativeRegisterLocation& location,
                                   const NativeRegisterClass register_class,
                                   const bool uses_frame,
                                   const std::int32_t displacement) {
        if (location.kind == NativeRegisterLocationKind::Register) {
            return !uses_frame && (register_class == NativeRegisterClass::GeneralPurpose
                ? valid_gp_register(location.register_index)
                : valid_xmm_register(location.register_index));
        }
        if (location.kind == NativeRegisterLocationKind::SpillSlot ||
            location.kind == NativeRegisterLocationKind::HomeSlot) {
            return uses_frame && displacement < 0 && (displacement % 8) == 0;
        }
        return false;
    };

    for (const auto& value : audit.values) {
        if (value.edge_id == InvalidFlowEdgeId ||
            value.predecessor_block_id == InvalidNativeRegisterBlockId ||
            value.successor_block_id == InvalidNativeRegisterBlockId ||
            value.value_id == 0U ||
            !unique_edge_values.emplace(value.edge_id, value.value_id).second) {
            return fail("N113H0022", "G6 audit contains an invalid or duplicate edge/value identity");
        }
        const auto value_class = value_classes.find(value.value_id);
        if (value_class == value_classes.end() || value_class->second != value.register_class) {
            return fail("N113H0023", "G6 edge candidate has no matching authoritative value register class");
        }
        if (same_location(value.predecessor_flow_location, value.successor_flow_location) ==
            value.flow_location_changes) {
            return fail("N113H0024", "G6 Flow location-change flag disagrees with its predecessor/successor endpoints");
        }
        if (!valid_location(value.predecessor_flow_location, value.register_class,
                             value.predecessor_uses_frame, value.predecessor_frame_displacement)) {
            return fail("N113H0025", "G6 predecessor Flow endpoint has an invalid register class or frame mapping");
        }
        if (!valid_location(value.successor_flow_location, value.register_class,
                             value.successor_uses_frame, value.successor_frame_displacement)) {
            return fail("N113H0026", "G6 successor Flow endpoint has an invalid register class or frame mapping");
        }
        if (value.emitter_active_location.kind == NativeRegisterLocationKind::Temporary ||
            (value.emitter_active_location.kind == NativeRegisterLocationKind::Register &&
             !(value.register_class == NativeRegisterClass::GeneralPurpose
                 ? valid_gp_register(value.emitter_active_location.register_index)
                 : valid_xmm_register(value.emitter_active_location.register_index))) ||
            (value.emitter_active_location.kind != NativeRegisterLocationKind::Register &&
             value.emitter_active_location.kind != NativeRegisterLocationKind::HomeSlot &&
             value.emitter_active_location.kind != NativeRegisterLocationKind::SpillSlot)) {
            return fail("N113H0027", "G6 value has no valid N111 FunctionEmitter active location");
        }

        if (!value.flow_location_changes) continue;
        ++result.flow_location_changes_considered;

        const auto anchor_it = anchors_by_edge.find(value.edge_id);
        if (anchor_it == anchors_by_edge.end()) {
            return fail("N113H0028", "G6 changed Flow location has no exact FunctionEmitter branch/fallthrough anchor");
        }
        const auto& anchor = *anchor_it->second;
        if (anchor.from_block_id != value.predecessor_block_id ||
            anchor.to_block_id != value.successor_block_id) {
            return fail("N113H002A", "G6 branch anchor endpoints do not match the audited CFG edge identity");
        }

        NativeX64FlowEdgeTransferCandidate candidate;
        candidate.edge_id = value.edge_id;
        candidate.predecessor_block_id = value.predecessor_block_id;
        candidate.successor_block_id = value.successor_block_id;
        candidate.branch_anchor = anchor;
        candidate.copy = NativeRegisterParallelCopy{
            value.value_id, value.predecessor_flow_location, value.successor_flow_location};
        candidate.emitter_active_location = value.emitter_active_location;
        result.candidates.push_back(std::move(candidate));
        candidate_edges.insert(value.edge_id);
    }

    result.candidate_copies = static_cast<std::uint32_t>(result.candidates.size());
    result.candidate_edges = static_cast<std::uint32_t>(candidate_edges.size());
    // Deliberately false: G5 proves the FunctionEmitter's N111 location, but
    // it does not prove that N112 block-local register assignments are used as
    // machine operands. G6 only assembles a validated candidate table.
    result.ready_for_machine_insertion = false;
    result.succeeded = true;
    return result;
}

namespace {

bool same_branch_anchor_identity(const X64FlowBranchAnchor& left,
                                 const X64FlowBranchAnchor& right) {
    return left.function_index == right.function_index &&
        left.edge_id == right.edge_id &&
        left.control_statement_id == right.control_statement_id &&
        left.control_construct_id == right.control_construct_id &&
        left.from_block_id == right.from_block_id &&
        left.to_block_id == right.to_block_id &&
        left.edge_kind == right.edge_kind &&
        left.encoding == right.encoding &&
        left.conditional_taken == right.conditional_taken &&
        left.instruction_offset == right.instruction_offset &&
        left.displacement_offset == right.displacement_offset &&
        left.target_offset == right.target_offset &&
        left.function_code_size == right.function_code_size &&
        left.transfer_instruction_offset == right.transfer_instruction_offset &&
        left.transfer_displacement_offset == right.transfer_displacement_offset &&
        left.transfer_target_offset == right.transfer_target_offset &&
        left.transfer_byte_count == right.transfer_byte_count;
}

using G7LocationKey = std::tuple<unsigned int, unsigned int, std::uint32_t>;
using G7DestinationKey = std::tuple<unsigned int, unsigned int, unsigned int, std::uint32_t>;

G7LocationKey g7_location_key(const NativeRegisterLocation& location) {
    const auto first = location.kind == NativeRegisterLocationKind::Register
        ? static_cast<unsigned int>(location.register_index) : 0U;
    const auto second = location.kind == NativeRegisterLocationKind::Register
        ? 0U : location.spill_slot;
    return G7LocationKey{static_cast<unsigned int>(location.kind), first, second};
}

G7DestinationKey g7_destination_key(const NativeRegisterLocation& location,
                                    const NativeRegisterClass register_class) {
    if (location.kind == NativeRegisterLocationKind::Register) {
        return G7DestinationKey{static_cast<unsigned int>(location.kind),
            static_cast<unsigned int>(register_class),
            static_cast<unsigned int>(location.register_index), 0U};
    }
    // Stack slots are shared frame memory across GP/XMM classes; class is not
    // part of the alias key for memory endpoints.
    return G7DestinationKey{static_cast<unsigned int>(location.kind), 0U, 0U,
        location.spill_slot};
}

bool g7_valid_persistent_location(const NativeRegisterLocation& location,
                                  const NativeRegisterClass register_class) {
    if (location.kind == NativeRegisterLocationKind::Register) {
        return register_class == NativeRegisterClass::GeneralPurpose
            ? valid_gp_register(location.register_index)
            : valid_xmm_register(location.register_index);
    }
    return location.kind == NativeRegisterLocationKind::SpillSlot ||
           location.kind == NativeRegisterLocationKind::HomeSlot;
}

bool g7_valid_edge_anchor(const X64FlowBranchAnchor& anchor) {
    if (anchor.edge_id == InvalidFlowEdgeId ||
        anchor.from_block_id == InvalidFlowBlockId ||
        anchor.to_block_id == InvalidFlowBlockId ||
        anchor.function_code_size == 0U ||
        anchor.instruction_offset > anchor.function_code_size ||
        anchor.target_offset > anchor.function_code_size) return false;
    if (anchor.encoding == X64FlowBranchEncoding::Fallthrough) {
        return anchor.displacement_offset == InvalidNativeFlowOperationIndex;
    }
    if (anchor.encoding == X64FlowBranchEncoding::ConditionalRel32 ||
        anchor.encoding == X64FlowBranchEncoding::UnconditionalRel32) {
        return anchor.displacement_offset != InvalidNativeFlowOperationIndex &&
            anchor.displacement_offset <= anchor.function_code_size &&
            4U <= anchor.function_code_size - anchor.displacement_offset;
    }
    return false;
}

} // namespace

NativeX64FlowEdgeTransferSchedulePlan schedule_native_x64_flow_edge_transfer_candidates(
    const NativeX64FlowEdgeTransferCandidatePlan& candidates,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    const NativeRegisterLocation gp_cycle_temporary,
    const NativeRegisterLocation xmm_cycle_temporary) {
    NativeX64FlowEdgeTransferSchedulePlan result;
    const auto fail = [&](const char* code, const char* message) {
        add_diagnostic(result.diagnostics, code, message);
        return result;
    };

    if (!candidates.succeeded || candidates.ready_for_machine_insertion) {
        return fail("N113H0030", "G7 requires a successful G6 candidate plan that is not insertion-ready");
    }
    if (candidates.candidate_copies != candidates.candidates.size()) {
        return fail("N113H0031", "G7 candidate-copy count does not match the supplied candidate records");
    }
    if (gp_cycle_temporary.kind != NativeRegisterLocationKind::Temporary ||
        gp_cycle_temporary.register_index < 8U || gp_cycle_temporary.register_index > 11U) {
        return fail("N113H0032", "G7 GP cycle temporary must be a dedicated R8-R11 temporary");
    }
    if (xmm_cycle_temporary.kind != NativeRegisterLocationKind::Temporary ||
        xmm_cycle_temporary.register_index < 1U || xmm_cycle_temporary.register_index > 5U) {
        return fail("N113H0033", "G7 XMM cycle temporary must be a dedicated XMM1-XMM5 temporary");
    }

    struct EdgeWork {
        X64FlowBranchAnchor anchor;
        NativeRegisterBlockId predecessor = InvalidNativeRegisterBlockId;
        NativeRegisterBlockId successor = InvalidNativeRegisterBlockId;
        std::vector<NativeRegisterParallelCopy> gp_copies;
        std::vector<NativeRegisterParallelCopy> xmm_copies;
        std::set<G7DestinationKey> destinations;
        bool initialized = false;
    };
    std::map<FlowEdgeId, EdgeWork> work_by_edge;
    std::set<std::pair<FlowEdgeId, std::uint32_t>> edge_values;

    for (const auto& candidate : candidates.candidates) {
        if (candidate.edge_id == InvalidFlowEdgeId || candidate.copy.value_id == 0U ||
            candidate.copy.source.kind == NativeRegisterLocationKind::Temporary ||
            candidate.copy.destination.kind == NativeRegisterLocationKind::Temporary ||
            !g7_valid_persistent_location(candidate.copy.source,
                value_classes.count(candidate.copy.value_id) ? value_classes.at(candidate.copy.value_id) : NativeRegisterClass::GeneralPurpose) ||
            !g7_valid_persistent_location(candidate.copy.destination,
                value_classes.count(candidate.copy.value_id) ? value_classes.at(candidate.copy.value_id) : NativeRegisterClass::GeneralPurpose)) {
            return fail("N113H0034", "G7 candidate contains an invalid identity or non-persistent endpoint");
        }
        const auto value_class = value_classes.find(candidate.copy.value_id);
        if (value_class == value_classes.end()) {
            return fail("N113H0035", "G7 candidate references a value without an authoritative register class");
        }
        const NativeRegisterLocation class_temporary =
            value_class->second == NativeRegisterClass::GeneralPurpose
                ? gp_cycle_temporary : xmm_cycle_temporary;
        const auto aliases_class_temporary = [&](const NativeRegisterLocation& location) {
            return location.kind == NativeRegisterLocationKind::Register &&
                   location.register_index == class_temporary.register_index;
        };
        if (aliases_class_temporary(candidate.copy.source) ||
            aliases_class_temporary(candidate.copy.destination)) {
            return fail("N113H003F", "G7 cycle temporary aliases a physical register endpoint in the same register class");
        }
        if (same_location(candidate.copy.source, candidate.copy.destination)) {
            return fail("N113H0036", "G7 candidate contains an identity copy that should have been removed by G6");
        }
        if (!g7_valid_edge_anchor(candidate.branch_anchor) ||
            candidate.branch_anchor.edge_id != candidate.edge_id ||
            candidate.branch_anchor.from_block_id != candidate.predecessor_block_id ||
            candidate.branch_anchor.to_block_id != candidate.successor_block_id) {
            return fail("N113H0037", "G7 candidate has no exact, bounded branch/fallthrough anchor for its CFG edge");
        }
        if (!edge_values.emplace(candidate.edge_id, candidate.copy.value_id).second) {
            return fail("N113H0038", "G7 candidate set repeats one Value on the same FlowEdgeId");
        }

        auto& edge = work_by_edge[candidate.edge_id];
        if (!edge.initialized) {
            edge.anchor = candidate.branch_anchor;
            edge.predecessor = candidate.predecessor_block_id;
            edge.successor = candidate.successor_block_id;
            edge.initialized = true;
        } else if (edge.predecessor != candidate.predecessor_block_id ||
                   edge.successor != candidate.successor_block_id ||
                   !same_branch_anchor_identity(edge.anchor, candidate.branch_anchor)) {
            return fail("N113H0039", "G7 candidates for one EdgeId disagree on the exact branch-anchor identity");
        }
        if (!edge.destinations.insert(g7_destination_key(candidate.copy.destination, value_class->second)).second) {
            return fail("N113H003A", "G7 edge bundle writes the same physical location more than once");
        }

        if (value_class->second == NativeRegisterClass::GeneralPurpose)
            edge.gp_copies.push_back(candidate.copy);
        else
            edge.xmm_copies.push_back(candidate.copy);
    }

    if (work_by_edge.size() != candidates.candidate_edges) {
        return fail("N113H003B", "G7 candidate-edge count does not match the supplied edge identities");
    }

    result.candidate_edges = static_cast<std::uint32_t>(work_by_edge.size());
    result.candidate_copies = static_cast<std::uint32_t>(candidates.candidates.size());

    const auto copy_less = [](const NativeRegisterParallelCopy& left,
                              const NativeRegisterParallelCopy& right) {
        if (left.value_id != right.value_id) return left.value_id < right.value_id;
        if (g7_location_key(left.source) != g7_location_key(right.source))
            return g7_location_key(left.source) < g7_location_key(right.source);
        return g7_location_key(left.destination) < g7_location_key(right.destination);
    };

    for (auto& item : work_by_edge) {
        auto& edge = item.second;
        std::sort(edge.gp_copies.begin(), edge.gp_copies.end(), copy_less);
        std::sort(edge.xmm_copies.begin(), edge.xmm_copies.end(), copy_less);

        NativeX64FlowEdgeTransferSchedule scheduled;
        scheduled.edge_id = item.first;
        scheduled.predecessor_block_id = edge.predecessor;
        scheduled.successor_block_id = edge.successor;
        scheduled.branch_anchor = edge.anchor;
        scheduled.bundle.edge_id = item.first;

        if (!edge.gp_copies.empty()) {
            const auto gp = resolve_native_register_parallel_copies(edge.gp_copies, gp_cycle_temporary);
            if (!gp.succeeded) {
                result.diagnostics.insert(result.diagnostics.end(), gp.diagnostics.begin(), gp.diagnostics.end());
                return fail("N113H003C", "G7 GP simultaneous-copy set could not be scheduled safely");
            }
            scheduled.gp_cycle_temporaries_used = gp.cycle_temporaries_used;
            scheduled.bundle.operations.insert(scheduled.bundle.operations.end(),
                gp.operations.begin(), gp.operations.end());
        }
        if (!edge.xmm_copies.empty()) {
            const auto xmm = resolve_native_register_parallel_copies(edge.xmm_copies, xmm_cycle_temporary);
            if (!xmm.succeeded) {
                result.diagnostics.insert(result.diagnostics.end(), xmm.diagnostics.begin(), xmm.diagnostics.end());
                return fail("N113H003D", "G7 XMM simultaneous-copy set could not be scheduled safely");
            }
            scheduled.xmm_cycle_temporaries_used = xmm.cycle_temporaries_used;
            scheduled.bundle.operations.insert(scheduled.bundle.operations.end(),
                xmm.operations.begin(), xmm.operations.end());
        }

        if (scheduled.bundle.operations.empty()) {
            return fail("N113H003E", "G7 produced an empty schedule for a non-empty G6 candidate edge");
        }
        result.scheduled_copies += static_cast<std::uint32_t>(edge.gp_copies.size() + edge.xmm_copies.size());
        result.scheduled_operations += static_cast<std::uint32_t>(scheduled.bundle.operations.size());
        result.gp_cycle_temporaries_used += scheduled.gp_cycle_temporaries_used;
        result.xmm_cycle_temporaries_used += scheduled.xmm_cycle_temporaries_used;
        result.edges.push_back(std::move(scheduled));
    }

    result.scheduled_edges = static_cast<std::uint32_t>(result.edges.size());
    // G7 schedules abstract candidates only. It does not establish N112
    // machine-operand ownership or prove temporary deadness at every CFG edge.
    result.scratch_liveness_proven = false;
    result.machine_insertion_ready = false;
    result.succeeded = true;
    return result;
}

NativeX64FlowEdgeScheduleReadinessPlan audit_native_x64_flow_edge_schedule_readiness(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeX64FlowEdgeLocationAuditPlan& edge_location_audit,
    const NativeX64FlowEdgeTransferSchedulePlan& schedule,
    const NativeX64MachineLocationPlan& emitter_locations,
    const std::map<std::uint32_t, std::int32_t>& emitter_local_home_offsets,
    const NativeX64FrameLocationMap& flow_frame_locations,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors,
    const std::vector<X64FlowBranchAnchor>& branch_anchors,
    const NativeRegisterLocation gp_cycle_temporary,
    const NativeRegisterLocation xmm_cycle_temporary,
    const std::vector<std::uint8_t>& expression_reserved_gp,
    const std::vector<std::uint8_t>& expression_reserved_xmm) {
    NativeX64FlowEdgeScheduleReadinessPlan result;
    const auto fail = [&](const char* code, const std::string& message) {
        add_diagnostic(result.diagnostics, code, message.c_str());
        return result;
    };

    const auto validated = validate_native_register_function_ir(function.register_ir);
    if (!validated.succeeded || !liveness.succeeded || !liveness.converged ||
        liveness.blocks.size() != function.register_ir.blocks.size() ||
        !edge_location_audit.succeeded || !schedule.succeeded ||
        schedule.machine_insertion_ready || !emitter_locations.succeeded) {
        return fail("N113H0040",
            "G8 requires valid CFG IR, converged liveness, successful G4/G7 plans and authoritative N111 locations");
    }
    if (gp_cycle_temporary.kind != NativeRegisterLocationKind::Temporary ||
        gp_cycle_temporary.register_index < 8U || gp_cycle_temporary.register_index > 11U ||
        xmm_cycle_temporary.kind != NativeRegisterLocationKind::Temporary ||
        xmm_cycle_temporary.register_index < 1U || xmm_cycle_temporary.register_index > 5U) {
        return fail("N113H0041", "G8 cycle temporaries are outside the established GP R8-R11/XMM1-XMM5 pools");
    }

    std::map<std::uint32_t, NativeRegisterClass> value_classes;
    for (const auto& value : validated.normalized_function.values) {
        if (value.value_id == 0U || !value_classes.emplace(value.value_id, value.register_class).second)
            return fail("N113H0042", "G8 Flow value table contains an invalid or duplicate ValueId");
    }
    if (emitter_locations.values.size() != value_classes.size())
        return fail("N113H0043", "G8 N111 location table does not cover the complete Flow value table");

    std::map<std::uint64_t, const NativeRegisterInstruction*> instructions_by_id;
    for (const auto& block : validated.normalized_function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic) continue;
            if (instruction.flow_operation_id == InvalidNativeFlowOperationId ||
                !instructions_by_id.emplace(instruction.flow_operation_id, &instruction).second)
                return fail("N113H0044", "G8 Flow IR has an invalid or duplicate source-operation identity");
        }
    }

    std::map<std::uint64_t, const X64FlowEmitterAnchor*> anchors_by_operation;
    std::map<std::uint32_t, std::uint32_t> operand_anchor_counts;
    for (const auto& anchor : operation_anchors) {
        if (anchor.flow_operation_id == InvalidNativeFlowOperationId ||
            !anchors_by_operation.emplace(anchor.flow_operation_id, &anchor).second)
            return fail("N113H0045", "G8 emitted operation anchors contain an invalid or duplicate FlowOperationId");
        const auto instruction = instructions_by_id.find(anchor.flow_operation_id);
        if (instruction == instructions_by_id.end() || instruction->second == nullptr ||
            anchor.flow_block_id != instruction->second->flow_block_id ||
            anchor.flow_operation_index != instruction->second->flow_operation_index ||
            anchor.origin_kind != instruction->second->origin_kind ||
            anchor.function_code_size == 0U || anchor.machine_code_offset > anchor.function_code_size ||
            (anchor.machine_code_before_offset != InvalidNativeFlowOperationIndex &&
             (anchor.machine_code_before_offset > anchor.machine_code_offset ||
              anchor.machine_code_before_offset > anchor.function_code_size))) {
            return fail("N113H0046", "G8 operation anchor mismatch: op=" +
                std::to_string(anchor.flow_operation_id) + " block=" +
                std::to_string(anchor.flow_block_id) + "/" +
                std::to_string(instruction->second->flow_block_id) + " index=" +
                std::to_string(anchor.flow_operation_index) + "/" +
                std::to_string(instruction->second->flow_operation_index) + " origin=" +
                std::to_string(static_cast<unsigned int>(anchor.origin_kind)) + "/" +
                std::to_string(static_cast<unsigned int>(instruction->second->origin_kind)) +
                " code=" + std::to_string(anchor.function_code_size) +
                " offset=" + std::to_string(anchor.machine_code_offset) +
                " before=" + std::to_string(anchor.machine_code_before_offset));
        }
        if (anchor.emitted_operand_kind == X64FlowEmitterAnchor::OperandKind::None) continue;

        const std::uint32_t value_id = anchor.emitted_operand_value_id;
        const auto value_class = value_classes.find(value_id);
        const auto machine_value = emitter_locations.values.find(value_id);
        if (value_class == value_classes.end() || machine_value == emitter_locations.values.end() ||
            machine_value->second.register_class != value_class->second ||
            !same_location(anchor.emitted_operand_location, machine_value->second.active_location)) {
            return fail("N113H0047", "G8 emitted operand anchor does not identify the authoritative N111 active Value location");
        }
        bool operand_role_valid = false;
        switch (anchor.emitted_operand_kind) {
        case X64FlowEmitterAnchor::OperandKind::ValueRead:
        case X64FlowEmitterAnchor::OperandKind::AddressRead:
            operand_role_valid = instruction->second->origin_kind == NativeRegisterInstructionOriginKind::FlowRead &&
                std::find(instruction->second->uses.begin(), instruction->second->uses.end(), value_id) !=
                    instruction->second->uses.end();
            break;
        case X64FlowEmitterAnchor::OperandKind::ValueWrite:
        case X64FlowEmitterAnchor::OperandKind::ValueDeclare:
            operand_role_valid = (instruction->second->origin_kind == NativeRegisterInstructionOriginKind::FlowWrite ||
                instruction->second->origin_kind == NativeRegisterInstructionOriginKind::FlowDeclare) &&
                std::find(instruction->second->definitions.begin(), instruction->second->definitions.end(), value_id) !=
                    instruction->second->definitions.end();
            break;
        case X64FlowEmitterAnchor::OperandKind::None:
            break;
        }
        if (!operand_role_valid)
            return fail("N113H0048", "G8 emitted operand anchor has a ValueId/FlowRead-Write role mismatch");
        ++operand_anchor_counts[value_id];
    }

    std::map<FlowEdgeId, const FlowEdge*> flow_edges_by_id;
    for (const auto& edge : function.edges) {
        if (edge.id == InvalidFlowEdgeId || !flow_edges_by_id.emplace(edge.id, &edge).second)
            return fail("N113H0049", "G8 Function FlowEdge table contains an invalid or duplicate EdgeId");
    }
    std::map<FlowEdgeId, const X64FlowBranchAnchor*> branch_anchors_by_edge;
    for (const auto& anchor : branch_anchors) {
        if (anchor.edge_id == InvalidFlowEdgeId ||
            !branch_anchors_by_edge.emplace(anchor.edge_id, &anchor).second)
            return fail("N113H004A", "G8 emitted CFG branch anchors contain an invalid or duplicate EdgeId");
    }

    std::set<FlowEdgeId> scheduled_edge_ids;
    std::map<FlowEdgeId, std::vector<const NativeX64FlowEdgeValueAudit*>> audit_values_by_edge;
    for (const auto& value : edge_location_audit.values)
        audit_values_by_edge[value.edge_id].push_back(&value);

    const auto flow_location_matches_emitter = [&](const NativeRegisterLocation& flow_location,
                                                   const NativeRegisterClass register_class,
                                                   const NativeRegisterLocation& emitter_location) {
        if (flow_location.kind == NativeRegisterLocationKind::Register)
            return emitter_location.kind == NativeRegisterLocationKind::Register &&
                flow_location.register_index == emitter_location.register_index;
        if (flow_location.kind != NativeRegisterLocationKind::SpillSlot &&
            flow_location.kind != NativeRegisterLocationKind::HomeSlot) return false;
        if (emitter_location.kind != NativeRegisterLocationKind::HomeSlot) return false;
        const auto& offsets = flow_location.kind == NativeRegisterLocationKind::SpillSlot
            ? flow_frame_locations.spill_slot_offsets : flow_frame_locations.home_slot_offsets;
        const auto flow_offset = offsets.find(flow_location.spill_slot);
        const auto emitter_offset = emitter_local_home_offsets.find(emitter_location.spill_slot);
        if (flow_offset == offsets.end() || emitter_offset == emitter_local_home_offsets.end()) return false;
        if (flow_offset->second >= 0 || emitter_offset->second >= 0 ||
            (flow_offset->second % 8) != 0 || (emitter_offset->second % 8) != 0) return false;
        (void)register_class;
        return flow_offset->second == emitter_offset->second;
    };

    const std::set<std::uint8_t> reserved_gp(expression_reserved_gp.begin(), expression_reserved_gp.end());
    const std::set<std::uint8_t> reserved_xmm(expression_reserved_xmm.begin(), expression_reserved_xmm.end());
    bool all_ownership_proven = true;
    bool all_scratch_proven = true;

    if (schedule.edges.size() != schedule.scheduled_edges)
        return fail("N113H004B", "G8 schedule edge count does not match the scheduled-edge inventory");

    for (const auto& scheduled : schedule.edges) {
        if (scheduled.edge_id == InvalidFlowEdgeId ||
            !scheduled_edge_ids.insert(scheduled.edge_id).second ||
            scheduled.bundle.edge_id != scheduled.edge_id ||
            !g7_valid_edge_anchor(scheduled.branch_anchor) ||
            scheduled.branch_anchor.edge_id != scheduled.edge_id ||
            scheduled.branch_anchor.from_block_id != scheduled.predecessor_block_id ||
            scheduled.branch_anchor.to_block_id != scheduled.successor_block_id) {
            return fail("N113H004C", "G8 schedule contains an invalid or duplicate exact edge identity");
        }
        const auto flow_edge = flow_edges_by_id.find(scheduled.edge_id);
        const auto branch_anchor = branch_anchors_by_edge.find(scheduled.edge_id);
        if (flow_edge == flow_edges_by_id.end() || flow_edge->second == nullptr ||
            flow_edge->second->from_block_id != scheduled.predecessor_block_id ||
            flow_edge->second->to_block_id != scheduled.successor_block_id ||
            branch_anchor == branch_anchors_by_edge.end() || branch_anchor->second == nullptr ||
            !same_branch_anchor_identity(scheduled.branch_anchor, *branch_anchor->second)) {
            return fail("N113H004D", "G8 schedule does not match the exact Function CFG edge and final emitted branch anchor");
        }
        if (scheduled.bundle.operations.empty())
            return fail("N113H004E", "G8 refuses an empty transfer schedule for a scheduled FlowEdgeId");

        NativeX64FlowEdgeReadinessEntry proof;
        proof.edge_id = scheduled.edge_id;
        proof.predecessor_block_id = scheduled.predecessor_block_id;
        proof.successor_block_id = scheduled.successor_block_id;
        proof.gp_scratch_register = gp_cycle_temporary.register_index;
        proof.xmm_scratch_register = xmm_cycle_temporary.register_index;

        const auto values_it = audit_values_by_edge.find(scheduled.edge_id);
        if (values_it == audit_values_by_edge.end() || values_it->second.empty())
            return fail("N113H004F", "G8 scheduled edge has no G4 live-through Value ownership evidence");
        std::set<std::uint32_t> edge_value_ids;
        const auto& pred_live_out = liveness.blocks[scheduled.predecessor_block_id].live_out;
        const auto& succ_live_in = liveness.blocks[scheduled.successor_block_id].live_in;
        for (const auto* value_audit : values_it->second) {
            if (value_audit == nullptr || value_audit->value_id == 0U ||
                !edge_value_ids.insert(value_audit->value_id).second ||
                value_audit->predecessor_block_id != scheduled.predecessor_block_id ||
                value_audit->successor_block_id != scheduled.successor_block_id ||
                std::find(pred_live_out.begin(), pred_live_out.end(), value_audit->value_id) == pred_live_out.end() ||
                std::find(succ_live_in.begin(), succ_live_in.end(), value_audit->value_id) == succ_live_in.end()) {
                return fail("N113H0050", "G8 edge Value evidence is not live through the exact predecessor/successor pair");
            }
            const auto value_class = value_classes.find(value_audit->value_id);
            const auto machine_value = emitter_locations.values.find(value_audit->value_id);
            if (value_class == value_classes.end() || machine_value == emitter_locations.values.end() ||
                value_class->second != value_audit->register_class ||
                machine_value->second.register_class != value_audit->register_class ||
                !same_location(value_audit->emitter_active_location, machine_value->second.active_location)) {
                return fail("N113H0051", "G8 edge Value does not retain the authoritative N111 active location and register class");
            }
            ++proof.live_values_audited;
            result.live_edge_values_audited++;
            const auto anchor_count = operand_anchor_counts.find(value_audit->value_id);
            if (anchor_count == operand_anchor_counts.end() || anchor_count->second == 0U) {
                // No concrete emitted source operand exists for this Value; the
                // edge stays non-ready, but the structurally valid audit is kept.
                all_ownership_proven = false;
            } else {
                proof.operand_anchors_validated += anchor_count->second;
                result.operand_anchors_validated += anchor_count->second;
            }
            const bool predecessor_match = flow_location_matches_emitter(
                value_audit->predecessor_flow_location, value_audit->register_class,
                value_audit->emitter_active_location);
            const bool successor_match = flow_location_matches_emitter(
                value_audit->successor_flow_location, value_audit->register_class,
                value_audit->emitter_active_location);

            // G19 records the exact operands and frame evidence already used by
            // the G8 proof. This is deliberately downstream of both comparisons
            // and has no effect on the existing counts or readiness booleans.
            NativeX64FlowEdgeEndpointValueEvidence endpoint_evidence;
            endpoint_evidence.value_id = value_audit->value_id;
            endpoint_evidence.register_class = value_audit->register_class;
            endpoint_evidence.predecessor_flow_location = value_audit->predecessor_flow_location;
            endpoint_evidence.successor_flow_location = value_audit->successor_flow_location;
            endpoint_evidence.emitter_active_location = value_audit->emitter_active_location;
            endpoint_evidence.predecessor_uses_frame = value_audit->predecessor_uses_frame;
            endpoint_evidence.predecessor_frame_displacement = value_audit->predecessor_frame_displacement;
            endpoint_evidence.successor_uses_frame = value_audit->successor_uses_frame;
            endpoint_evidence.successor_frame_displacement = value_audit->successor_frame_displacement;
            endpoint_evidence.emitter_location_is_home_slot =
                value_audit->emitter_active_location.kind == NativeRegisterLocationKind::HomeSlot;
            if (endpoint_evidence.emitter_location_is_home_slot) {
                const auto emitter_offset = emitter_local_home_offsets.find(
                    value_audit->emitter_active_location.spill_slot);
                if (emitter_offset != emitter_local_home_offsets.end() &&
                    emitter_offset->second < 0 && (emitter_offset->second % 8) == 0) {
                    endpoint_evidence.emitter_frame_mapping_available = true;
                    endpoint_evidence.emitter_frame_displacement = emitter_offset->second;
                }
            }
            endpoint_evidence.predecessor_matches_emitter = predecessor_match;
            endpoint_evidence.successor_matches_emitter = successor_match;
            endpoint_evidence.function_operand_anchor_count = anchor_count == operand_anchor_counts.end()
                ? 0U : anchor_count->second;
            proof.endpoint_value_evidence.push_back(std::move(endpoint_evidence));

            if (predecessor_match) {
                ++proof.endpoint_ownership_matches;
                ++result.endpoint_ownership_matches;
            } else {
                ++proof.endpoint_ownership_mismatches;
                ++result.endpoint_ownership_mismatches;
            }
            if (successor_match) {
                ++proof.endpoint_ownership_matches;
                ++result.endpoint_ownership_matches;
            } else {
                ++proof.endpoint_ownership_mismatches;
                ++result.endpoint_ownership_mismatches;
            }
        }

        for (const auto& operation : scheduled.bundle.operations) {
            const auto value_class = value_classes.find(operation.value_id);
            if (value_class == value_classes.end())
                return fail("N113H0052", "G8 scheduled operation references a Value with no authoritative register class");
            const bool memory_to_memory =
                (operation.source.kind == NativeRegisterLocationKind::SpillSlot ||
                 operation.source.kind == NativeRegisterLocationKind::HomeSlot) &&
                (operation.destination.kind == NativeRegisterLocationKind::SpillSlot ||
                 operation.destination.kind == NativeRegisterLocationKind::HomeSlot);
            const bool uses_temporary = operation.source.kind == NativeRegisterLocationKind::Temporary ||
                operation.destination.kind == NativeRegisterLocationKind::Temporary;
            const auto register_endpoint_aliases = [&](const NativeRegisterLocation& location,
                                                       const NativeRegisterClass cls,
                                                       const NativeRegisterLocation& scratch) {
                return location.kind == NativeRegisterLocationKind::Register &&
                    cls == value_class->second && location.register_index == scratch.register_index;
            };
            if (value_class->second == NativeRegisterClass::GeneralPurpose) {
                proof.gp_scratch_required = proof.gp_scratch_required || memory_to_memory || uses_temporary;
                if (uses_temporary && operation.source.kind == NativeRegisterLocationKind::Temporary &&
                    operation.source.register_index != gp_cycle_temporary.register_index)
                    return fail("N113H0053", "G8 GP cycle operation does not use the selected G7 GP temporary");
                if (uses_temporary && operation.destination.kind == NativeRegisterLocationKind::Temporary &&
                    operation.destination.register_index != gp_cycle_temporary.register_index)
                    return fail("N113H0054", "G8 GP cycle destination does not use the selected G7 GP temporary");
                if (register_endpoint_aliases(operation.source, value_class->second, gp_cycle_temporary) ||
                    register_endpoint_aliases(operation.destination, value_class->second, gp_cycle_temporary))
                    proof.gp_scratch_dead = false;
            } else {
                proof.xmm_scratch_required = proof.xmm_scratch_required || memory_to_memory || uses_temporary;
                if (uses_temporary && operation.source.kind == NativeRegisterLocationKind::Temporary &&
                    operation.source.register_index != xmm_cycle_temporary.register_index)
                    return fail("N113H0055", "G8 XMM cycle operation does not use the selected G7 XMM temporary");
                if (uses_temporary && operation.destination.kind == NativeRegisterLocationKind::Temporary &&
                    operation.destination.register_index != xmm_cycle_temporary.register_index)
                    return fail("N113H0056", "G8 XMM cycle destination does not use the selected G7 XMM temporary");
                if (register_endpoint_aliases(operation.source, value_class->second, xmm_cycle_temporary) ||
                    register_endpoint_aliases(operation.destination, value_class->second, xmm_cycle_temporary))
                    proof.xmm_scratch_dead = false;
            }
        }

        // Scratch is checked against every live-through endpoint, including the
        // actual N111 active location, not merely the copies in the bundle.
        for (const auto* value_audit : values_it->second) {
            if (value_audit->register_class == NativeRegisterClass::GeneralPurpose &&
                proof.gp_scratch_required) {
                const auto conflicts = [&](const NativeRegisterLocation& location) {
                    return location.kind == NativeRegisterLocationKind::Register &&
                        location.register_index == gp_cycle_temporary.register_index;
                };
                if (conflicts(value_audit->predecessor_flow_location) ||
                    conflicts(value_audit->successor_flow_location) ||
                    conflicts(value_audit->emitter_active_location)) proof.gp_scratch_dead = false;
            }
            if (value_audit->register_class == NativeRegisterClass::FloatingPoint &&
                proof.xmm_scratch_required) {
                const auto conflicts = [&](const NativeRegisterLocation& location) {
                    return location.kind == NativeRegisterLocationKind::Register &&
                        location.register_index == xmm_cycle_temporary.register_index;
                };
                if (conflicts(value_audit->predecessor_flow_location) ||
                    conflicts(value_audit->successor_flow_location) ||
                    conflicts(value_audit->emitter_active_location)) proof.xmm_scratch_dead = false;
            }
        }
        if (proof.gp_scratch_required && reserved_gp.find(gp_cycle_temporary.register_index) != reserved_gp.end())
            proof.gp_scratch_dead = false;
        if (proof.xmm_scratch_required && reserved_xmm.find(xmm_cycle_temporary.register_index) != reserved_xmm.end())
            proof.xmm_scratch_dead = false;

        std::sort(proof.endpoint_value_evidence.begin(), proof.endpoint_value_evidence.end(),
            [](const auto& left, const auto& right) { return left.value_id < right.value_id; });
        proof.machine_operand_ownership_proven =
            proof.endpoint_ownership_mismatches == 0U &&
            proof.endpoint_ownership_matches == proof.live_values_audited * 2U &&
            proof.operand_anchors_validated > 0U;
        proof.scratch_liveness_proven =
            (!proof.gp_scratch_required || proof.gp_scratch_dead) &&
            (!proof.xmm_scratch_required || proof.xmm_scratch_dead);
        if (proof.gp_scratch_required) {
            if (proof.gp_scratch_dead) ++result.gp_scratch_registers_proven_dead;
            else ++result.scratch_liveness_conflicts;
        }
        if (proof.xmm_scratch_required) {
            if (proof.xmm_scratch_dead) ++result.xmm_scratch_registers_proven_dead;
            else ++result.scratch_liveness_conflicts;
        }
        if (!proof.machine_operand_ownership_proven) all_ownership_proven = false;
        if (!proof.scratch_liveness_proven) all_scratch_proven = false;
        result.edges.push_back(std::move(proof));
    }

    result.scheduled_edges_audited = static_cast<std::uint32_t>(result.edges.size());
    result.machine_operand_ownership_proven = all_ownership_proven &&
        result.endpoint_ownership_mismatches == 0U;
    result.scratch_liveness_proven = all_scratch_proven;
    result.machine_insertion_ready = !result.edges.empty() &&
        result.machine_operand_ownership_proven && result.scratch_liveness_proven;
    result.succeeded = true;
    if (!result.machine_operand_ownership_proven)
        add_diagnostic(result.diagnostics, "N113H0057",
            "G8 found one or more N112 edge endpoints that are not proven to be the concrete FunctionEmitter machine operands");
    if (!result.scratch_liveness_proven)
        add_diagnostic(result.diagnostics, "N113H0058",
            "G8 found one or more cycle/memory-expansion scratch registers that are live or expression-reserved on an exact CFG edge");
    if (result.machine_insertion_ready)
        add_diagnostic(result.diagnostics, "N113H0059",
            "G8 readiness evidence is complete; physical insertion remains a separate opt-in stage");
    return result;
}

NativeX64FlowEdgePerEdgeScratchPlan plan_native_x64_flow_edge_schedules_per_edge(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeX64FlowEdgeLocationAuditPlan& edge_location_audit,
    const NativeX64FlowEdgeTransferCandidatePlan& candidates,
    const NativeX64MachineLocationPlan& emitter_locations,
    const std::map<std::uint32_t, std::int32_t>& emitter_local_home_offsets,
    const NativeX64FrameLocationMap& flow_frame_locations,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors,
    const std::vector<X64FlowBranchAnchor>& branch_anchors,
    const std::vector<std::uint8_t>& expression_reserved_gp,
    const std::vector<std::uint8_t>& expression_reserved_xmm) {
    NativeX64FlowEdgePerEdgeScratchPlan result;
    const auto fail = [&](const char* code, const char* message) {
        add_diagnostic(result.diagnostics, code, message);
        return result;
    };

    if (!candidates.succeeded || candidates.ready_for_machine_insertion ||
        candidates.candidate_copies != candidates.candidates.size() ||
        !edge_location_audit.succeeded || !emitter_locations.succeeded) {
        return fail("N113H0060",
            "G9 requires a valid non-insertion-ready G6 candidate plan, successful G4 edge audit and authoritative N111 locations");
    }

    std::map<FlowEdgeId, std::vector<NativeX64FlowEdgeTransferCandidate>> candidates_by_edge;
    for (const auto& candidate : candidates.candidates) {
        if (candidate.edge_id == InvalidFlowEdgeId || candidate.copy.value_id == 0U) {
            return fail("N113H0061", "G9 candidate contains an invalid exact FlowEdgeId or ValueId");
        }
        candidates_by_edge[candidate.edge_id].push_back(candidate);
    }
    if (candidates_by_edge.size() != candidates.candidate_edges) {
        return fail("N113H0062", "G9 candidate-edge inventory does not match the unique exact FlowEdgeIds");
    }

    result.candidate_edges = static_cast<std::uint32_t>(candidates_by_edge.size());
    bool all_ownership_proven = !candidates_by_edge.empty();
    bool all_scratch_proven = !candidates_by_edge.empty();

    // The established safe pools are searched independently for every edge.
    // Stable ascending order makes the selected pair reproducible across hosts.
    for (const auto& edge_item : candidates_by_edge) {
        NativeX64FlowEdgePerEdgeScratchEntry entry;
        entry.edge_id = edge_item.first;
        for (const auto& value_audit : edge_location_audit.values) {
            if (value_audit.edge_id == edge_item.first)
                entry.g4_live_through_values.push_back(value_audit);
        }
        entry.g6_candidates = edge_item.second;
        if (!entry.g6_candidates.empty()) {
            entry.g6_branch_anchor = entry.g6_candidates.front().branch_anchor;
            entry.g6_branch_anchor_present =
                entry.g6_branch_anchor.edge_id != InvalidFlowEdgeId;
            entry.g6_branch_anchor_identity_exact =
                entry.g6_branch_anchor_present &&
                entry.g6_branch_anchor.from_block_id == entry.g6_candidates.front().predecessor_block_id &&
                entry.g6_branch_anchor.to_block_id == entry.g6_candidates.front().successor_block_id &&
                entry.g6_branch_anchor.edge_id == edge_item.first &&
                entry.g6_branch_anchor.function_index != InvalidNativeFunctionIndex;
            for (const auto& candidate : entry.g6_candidates) {
                if (candidate.branch_anchor.edge_id != entry.g6_branch_anchor.edge_id ||
                    candidate.branch_anchor.function_index != entry.g6_branch_anchor.function_index ||
                    candidate.branch_anchor.from_block_id != entry.g6_branch_anchor.from_block_id ||
                    candidate.branch_anchor.to_block_id != entry.g6_branch_anchor.to_block_id ||
                    candidate.branch_anchor.instruction_offset != entry.g6_branch_anchor.instruction_offset ||
                    candidate.branch_anchor.displacement_offset != entry.g6_branch_anchor.displacement_offset ||
                    candidate.branch_anchor.target_offset != entry.g6_branch_anchor.target_offset ||
                    candidate.branch_anchor.encoding != entry.g6_branch_anchor.encoding ||
                    candidate.branch_anchor.edge_kind != entry.g6_branch_anchor.edge_kind ||
                    candidate.branch_anchor.conditional_taken != entry.g6_branch_anchor.conditional_taken) {
                    entry.g6_branch_anchor_identity_exact = false;
                    break;
                }
            }
        }

        NativeX64FlowEdgeTransferCandidatePlan edge_candidates;
        edge_candidates.succeeded = true;
        edge_candidates.ready_for_machine_insertion = false;
        edge_candidates.candidates = edge_item.second;
        edge_candidates.candidate_copies = static_cast<std::uint32_t>(edge_candidates.candidates.size());
        edge_candidates.candidate_edges = 1U;
        edge_candidates.flow_location_changes_considered = edge_candidates.candidate_copies;

        bool have_best_schedule = false;
        bool have_safe_selection = false;
        NativeX64FlowEdgeTransferSchedulePlan best_schedule;
        NativeX64FlowEdgeScheduleReadinessPlan best_proof;

        std::map<std::uint32_t, NativeRegisterClass> edge_value_classes;
        for (const auto& candidate : edge_candidates.candidates) {
            const auto value = std::find_if(function.register_ir.values.begin(),
                function.register_ir.values.end(), [&](const NativeRegisterValueInfo& item) {
                    return item.value_id == candidate.copy.value_id;
                });
            if (value == function.register_ir.values.end()) {
                edge_value_classes.clear();
                break;
            }
            edge_value_classes.emplace(value->value_id, value->register_class);
        }
        if (edge_value_classes.size() != edge_candidates.candidates.size()) {
            return fail("N113H0063", "G9 candidate ValueId has no unique authoritative class in the Function CFG");
        }

        for (std::uint8_t gp = 8U; gp <= 11U && !have_safe_selection; ++gp) {
            for (std::uint8_t xmm = 1U; xmm <= 5U; ++xmm) {
                NativeRegisterLocation gp_temporary;
                gp_temporary.kind = NativeRegisterLocationKind::Temporary;
                gp_temporary.register_index = gp;
                NativeRegisterLocation xmm_temporary;
                xmm_temporary.kind = NativeRegisterLocationKind::Temporary;
                xmm_temporary.register_index = xmm;

                NativeX64FlowEdgeG7ScratchTrialEvidence trial;
                trial.gp_temporary_register = gp;
                trial.xmm_temporary_register = xmm;
                auto schedule = schedule_native_x64_flow_edge_transfer_candidates(
                    edge_candidates, edge_value_classes, gp_temporary, xmm_temporary);
                if (!schedule.succeeded || schedule.edges.size() != 1U ||
                    schedule.edges.front().edge_id != edge_item.first) {
                    ++result.g7_schedule_rejections;
                    if (!schedule.diagnostics.empty()) {
                        trial.g7_rejection_code = schedule.diagnostics.front().code;
                        trial.g7_rejection_message = schedule.diagnostics.front().message;
                    } else {
                        trial.g7_rejection_code = "N113G20_G7_NO_SCHEDULE_DETAIL";
                        trial.g7_rejection_message = "G7 did not return one exact-edge schedule and supplied no diagnostic.";
                    }
                    if (result.first_g7_rejection.empty() && !schedule.diagnostics.empty())
                        result.first_g7_rejection = schedule.diagnostics.front().code + " " +
                            schedule.diagnostics.front().message;
                    if (result.diagnostics.empty() && !schedule.diagnostics.empty())
                        result.diagnostics.push_back(schedule.diagnostics.front());
                    entry.g7_trials.push_back(std::move(trial));
                    continue;
                }
                trial.g7_schedule_succeeded = true;
                trial.scheduled_operations = static_cast<std::uint32_t>(
                    schedule.edges.front().bundle.operations.size());

                entry.g8_readiness_attempted = true;
                trial.g8_readiness_attempted = true;
                auto proof = audit_native_x64_flow_edge_schedule_readiness(
                    function, liveness, edge_location_audit, schedule, emitter_locations,
                    emitter_local_home_offsets, flow_frame_locations, operation_anchors,
                    branch_anchors, gp_temporary, xmm_temporary,
                    expression_reserved_gp, expression_reserved_xmm);
                if (!proof.succeeded || proof.edges.size() != 1U ||
                    proof.edges.front().edge_id != edge_item.first) {
                    ++result.readiness_proof_rejections;
                    if (!proof.diagnostics.empty()) {
                        trial.g8_rejection_code = proof.diagnostics.front().code;
                        trial.g8_rejection_message = proof.diagnostics.front().message;
                        if (entry.g8_rejection_code.empty()) {
                            entry.g8_rejection_code = trial.g8_rejection_code;
                            entry.g8_rejection_message = trial.g8_rejection_message;
                        }
                    } else {
                        trial.g8_rejection_code = "N113G20_G8_NO_READINESS_DETAIL";
                        trial.g8_rejection_message = "G8 did not return one exact-edge readiness proof and supplied no diagnostic.";
                    }
                    if (result.first_readiness_proof_rejection.empty() && !proof.diagnostics.empty())
                        result.first_readiness_proof_rejection = proof.diagnostics.front().code + " " +
                            proof.diagnostics.front().message;
                    if (result.diagnostics.empty() && !proof.diagnostics.empty())
                        result.diagnostics.push_back(proof.diagnostics.front());
                    entry.g7_trials.push_back(std::move(trial));
                    continue;
                }
                trial.g8_readiness_audit_completed = true;
                trial.scratch_liveness_proven = proof.scratch_liveness_proven;
                trial.scratch_liveness_conflicts = proof.scratch_liveness_conflicts;
                entry.g8_readiness_audit_completed = true;
                entry.g7_trials.push_back(std::move(trial));

                const bool proof_is_safe = proof.scratch_liveness_proven;
                if (!have_best_schedule ||
                    proof.scratch_liveness_conflicts < best_proof.scratch_liveness_conflicts) {
                    best_schedule = schedule;
                    best_proof = proof;
                    entry.gp_cycle_temporary = gp_temporary;
                    entry.xmm_cycle_temporary = xmm_temporary;
                    have_best_schedule = true;
                }

                if (proof_is_safe) {
                    // Keep the first deterministic pair proven dead on this
                    // exact edge. Ownership remains an independent proof.
                    best_schedule = std::move(schedule);
                    best_proof = std::move(proof);
                    entry.gp_cycle_temporary = gp_temporary;
                    entry.xmm_cycle_temporary = xmm_temporary;
                    have_best_schedule = true;
                    have_safe_selection = true;
                    break;
                }
            }
        }

        if (have_best_schedule) {
            entry.schedule_built = true;
            entry.schedule = best_schedule.edges.front();
            entry.readiness = best_proof.edges.front();
            ++result.scheduled_edges;
            result.endpoint_ownership_mismatches += entry.readiness.endpoint_ownership_mismatches;
            if (entry.readiness.machine_operand_ownership_proven) {
                // Ownership is an independent proof; scratch selection cannot
                // upgrade a mismatched endpoint into a concrete Emitter operand.
            } else {
                all_ownership_proven = false;
            }
        } else {
            all_ownership_proven = false;
            all_scratch_proven = false;
            ++result.scratch_unavailable_edges;
            add_diagnostic(result.diagnostics, "N113H0064",
                "G9 could not construct a structurally valid one-edge G7 schedule and G8 readiness proof for an exact FlowEdgeId");
        }

        if (have_safe_selection) {
            entry.scratch_selection_proven = true;
            ++result.scratch_proven_edges;
        } else {
            entry.scratch_selection_proven = false;
            all_scratch_proven = false;
            if (have_best_schedule) {
                ++result.scratch_unavailable_edges;
                result.scratch_liveness_conflicts += entry.readiness.scratch_liveness_proven
                    ? 0U : 1U;
                add_diagnostic(result.diagnostics, "N113H0065",
                    "G9 found no GP/XMM scratch pair proven dead for this exact CFG edge; the edge remains non-insertion-ready");
            }
        }

        result.edges.push_back(std::move(entry));
    }

    result.machine_operand_ownership_proven = all_ownership_proven &&
        result.endpoint_ownership_mismatches == 0U &&
        result.scheduled_edges == result.candidate_edges;
    result.scratch_liveness_proven = all_scratch_proven &&
        result.scratch_proven_edges == result.candidate_edges;
    result.machine_insertion_ready = result.candidate_edges != 0U &&
        result.machine_operand_ownership_proven && result.scratch_liveness_proven;
    result.succeeded = true;
    return result;
}

NativeX64FlowOperationOperandOwnershipPlan audit_native_x64_flow_operation_operand_ownership(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterFlowSegmentPlan& flow_segment_plan,
    const NativeX64MachineLocationPlan& emitter_locations,
    const std::map<std::uint32_t, std::int32_t>& emitter_local_home_offsets,
    const NativeX64FrameLocationMap& flow_frame_locations,
    const std::size_t function_index,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors) {
    NativeX64FlowOperationOperandOwnershipPlan result;
    const auto fail = [&](const char* code, const std::string& message) {
        add_diagnostic(result.diagnostics, code, message.c_str());
        return result;
    };

    const auto validated = validate_native_register_function_ir(function.register_ir);
    if (!validated.succeeded || !flow_segment_plan.succeeded || !emitter_locations.succeeded ||
        function_index == InvalidNativeFunctionIndex) {
        return fail("N113J0001",
            "G10 requires valid Flow IR, successful N112/N111 plans and a concrete FunctionEmitter index");
    }

    result.function_index = function_index;

    struct InstructionRecord {
        const NativeRegisterInstruction* instruction = nullptr;
    };
    std::map<std::uint64_t, InstructionRecord> instructions;
    for (const auto& block : validated.normalized_function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic) continue;
            if (instruction.flow_operation_id == InvalidNativeFlowOperationId ||
                !instructions.emplace(instruction.flow_operation_id, InstructionRecord{&instruction}).second) {
                return fail("N113J0002", "G10 Flow IR contains an invalid or duplicate source-operation identity");
            }
        }
    }

    std::map<std::uint64_t, const NativeRegisterFlowOperationLocation*> operations;
    for (const auto& operation : flow_segment_plan.operations) {
        if (operation.flow_operation_id == InvalidNativeFlowOperationId ||
            !operations.emplace(operation.flow_operation_id, &operation).second) {
            return fail("N113J0003", "G10 N112 operation table contains an invalid or duplicate FlowOperationId");
        }
        const auto instruction = instructions.find(operation.flow_operation_id);
        if (instruction == instructions.end() || instruction->second.instruction == nullptr ||
            operation.flow_block_id != instruction->second.instruction->flow_block_id ||
            operation.flow_operation_index != instruction->second.instruction->flow_operation_index) {
            return fail("N113J0004", "G10 N112 operation identity disagrees with the source Flow instruction");
        }
    }
    if (operations.size() != instructions.size())
        return fail("N113J0005", "G10 N112 plan does not cover every non-synthetic Flow operation exactly once");

    std::map<std::uint64_t, const X64FlowEmitterAnchor*> anchors;
    for (const auto& anchor : operation_anchors) {
        if (anchor.function_index != function_index ||
            anchor.flow_operation_id == InvalidNativeFlowOperationId ||
            !anchors.emplace(anchor.flow_operation_id, &anchor).second) {
            return fail("N113J0006", "G10 operation anchors contain a wrong function index, invalid ID or duplicate operation");
        }
    }
    if (anchors.size() != instructions.size())
        return fail("N113J0007", "G10 emitted anchors do not cover every non-synthetic Flow operation exactly once");

    const auto find_flow_operand = [](const NativeRegisterFlowOperationLocation& operation,
                                      const bool after,
                                      const std::uint32_t value_id,
                                      NativeRegisterLocation& location,
                                      NativeRegisterClass& register_class) {
        const auto& values = after ? operation.after_locations : operation.before_locations;
        const NativeRegisterFlowValueLocation* match = nullptr;
        for (const auto& value : values) {
            if (value.value_id != value_id) continue;
            if (match != nullptr) return false;
            match = &value;
        }
        if (match == nullptr) return false;
        location = match->location;
        register_class = match->register_class;
        return true;
    };

    const auto compare_physical_location = [&](const NativeRegisterLocation& flow_location,
                                               const NativeRegisterLocation& emitter_location,
                                               const NativeRegisterClass register_class,
                                               NativeX64FlowOperationOperandOwnershipEntry& entry) {
        if (flow_location.kind == NativeRegisterLocationKind::Register) {
            if (emitter_location.kind != NativeRegisterLocationKind::Register) return false;
            const bool valid_register = register_class == NativeRegisterClass::GeneralPurpose
                ? valid_gp_register(flow_location.register_index) && valid_gp_register(emitter_location.register_index)
                : valid_xmm_register(flow_location.register_index) && valid_xmm_register(emitter_location.register_index);
            return valid_register && flow_location.register_index == emitter_location.register_index;
        }
        if (!is_memory_location(flow_location) || emitter_location.kind != NativeRegisterLocationKind::HomeSlot)
            return false;
        const auto& offsets = flow_location.kind == NativeRegisterLocationKind::SpillSlot
            ? flow_frame_locations.spill_slot_offsets : flow_frame_locations.home_slot_offsets;
        const auto flow_offset = offsets.find(flow_location.spill_slot);
        const auto emitter_offset = emitter_local_home_offsets.find(emitter_location.spill_slot);
        if (flow_offset == offsets.end() || emitter_offset == emitter_local_home_offsets.end() ||
            flow_offset->second >= 0 || emitter_offset->second >= 0 ||
            (flow_offset->second % 8) != 0 || (emitter_offset->second % 8) != 0) return false;
        entry.flow_location_uses_frame = true;
        entry.emitter_location_uses_frame = true;
        entry.flow_frame_displacement = flow_offset->second;
        entry.emitter_frame_displacement = emitter_offset->second;
        return flow_offset->second == emitter_offset->second;
    };

    result.source_operations_audited = static_cast<std::uint32_t>(instructions.size());
    for (const auto& instruction_item : instructions) {
        const auto* instruction = instruction_item.second.instruction;
        const auto operation_it = operations.find(instruction_item.first);
        const auto anchor_it = anchors.find(instruction_item.first);
        if (instruction == nullptr || operation_it == operations.end() || anchor_it == anchors.end() ||
            operation_it->second == nullptr || anchor_it->second == nullptr) {
            return fail("N113J0008", "G10 operation lost its exact N112 plan or FunctionEmitter anchor");
        }
        const auto& operation = *operation_it->second;
        const auto& anchor = *anchor_it->second;
        if (anchor.flow_block_id != instruction->flow_block_id ||
            anchor.flow_operation_index != instruction->flow_operation_index ||
            anchor.origin_kind != instruction->origin_kind ||
            anchor.function_code_size == 0U ||
            anchor.machine_code_offset > anchor.function_code_size) {
            return fail("N113J0009", "G10 emitted operation anchor has invalid function/block/index/origin or code bounds");
        }

        std::uint32_t value_id = 0U;
        bool uses_before_position = false;
        bool expects_operand = false;
        bool address_read = false;
        X64FlowEmitterAnchor::OperandKind expected_kind = X64FlowEmitterAnchor::OperandKind::None;
        switch (instruction->origin_kind) {
        case NativeRegisterInstructionOriginKind::FlowRead:
            if (instruction->is_call || instruction->uses.size() != 1U || !instruction->definitions.empty())
                return fail("N113J000A", "G10 FlowRead must contain exactly one use and no definitions");
            value_id = instruction->uses.front();
            uses_before_position = true;
            expects_operand = true;
            if (anchor.emitted_operand_kind == X64FlowEmitterAnchor::OperandKind::AddressRead) {
                expected_kind = X64FlowEmitterAnchor::OperandKind::AddressRead;
                address_read = true;
            } else {
                expected_kind = X64FlowEmitterAnchor::OperandKind::ValueRead;
            }
            break;
        case NativeRegisterInstructionOriginKind::FlowWrite:
            if (instruction->is_call || !instruction->uses.empty() || instruction->definitions.size() != 1U)
                return fail("N113J000B", "G10 FlowWrite must contain exactly one definition and no uses");
            value_id = instruction->definitions.front();
            expects_operand = true;
            expected_kind = X64FlowEmitterAnchor::OperandKind::ValueWrite;
            break;
        case NativeRegisterInstructionOriginKind::FlowDeclare:
            if (instruction->is_call || !instruction->uses.empty() || instruction->definitions.size() > 1U)
                return fail("N113J000C", "G10 FlowDeclare has an unsupported operand shape");
            if (!instruction->definitions.empty()) {
                value_id = instruction->definitions.front();
                expects_operand = true;
                expected_kind = X64FlowEmitterAnchor::OperandKind::ValueDeclare;
            }
            break;
        case NativeRegisterInstructionOriginKind::FlowCall:
        case NativeRegisterInstructionOriginKind::FlowReturn:
            if (!instruction->uses.empty() || !instruction->definitions.empty() ||
                anchor.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::None)
                return fail("N113J000D", "G10 call/return anchor unexpectedly claims a source-value operand");
            break;
        case NativeRegisterInstructionOriginKind::Synthetic:
            return fail("N113J000E", "G10 synthetic instruction must not appear in the source-operation map");
        }

        if (!expects_operand) {
            if (anchor.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::None)
                return fail("N113J000F", "G10 operation without a source operand unexpectedly contains an emitter operand");
            continue;
        }
        if (anchor.emitted_operand_kind != expected_kind || anchor.emitted_operand_value_id != value_id ||
            anchor.machine_code_before_offset == InvalidNativeFlowOperationIndex ||
            anchor.machine_code_before_offset >= anchor.machine_code_offset) {
            return fail("N113J0010", "G10 source operand does not have one exact, non-empty emitted operand range and matching ValueId/role");
        }
        const auto machine_value = emitter_locations.values.find(value_id);
        if (machine_value == emitter_locations.values.end() ||
            machine_value->second.value_id != value_id ||
            machine_value->second.register_class !=
                (std::find_if(validated.normalized_function.values.begin(), validated.normalized_function.values.end(),
                    [value_id](const NativeRegisterValueInfo& value) { return value.value_id == value_id; }) ==
                 validated.normalized_function.values.end()
                    ? NativeRegisterClass::GeneralPurpose
                    : std::find_if(validated.normalized_function.values.begin(), validated.normalized_function.values.end(),
                        [value_id](const NativeRegisterValueInfo& value) { return value.value_id == value_id; })->register_class) ||
            !same_location(anchor.emitted_operand_location, machine_value->second.active_location)) {
            return fail("N113J0011", "G10 emitted operand does not match the authoritative N111 value/class/location");
        }
        if (address_read && (!machine_value->second.address_taken ||
            anchor.emitted_operand_location.kind != NativeRegisterLocationKind::HomeSlot ||
            !same_location(anchor.emitted_operand_location, machine_value->second.canonical_home))) {
            return fail("N113J0012", "G10 AddressRead is not anchored to the address-taken canonical N111 home");
        }

        NativeRegisterLocation flow_location;
        NativeRegisterClass flow_class = NativeRegisterClass::GeneralPurpose;
        if (!find_flow_operand(operation, !uses_before_position, value_id, flow_location, flow_class)) {
            return fail("N113J0013", "G10 N112 operation position has no unique location for its concrete source operand");
        }
        // G15: retain Flow-vs-N111 register-class disagreement as concrete
        // operand evidence. It is a classification blocker, not a malformed
        // FlowOperationId/anchor crosswalk; G11/G14/G15 must be able to report it.
        const bool register_class_matches = flow_class == machine_value->second.register_class;

        NativeX64FlowOperationOperandOwnershipEntry entry;
        entry.flow_operation_id = instruction->flow_operation_id;
        entry.flow_block_id = instruction->flow_block_id;
        entry.flow_operation_index = instruction->flow_operation_index;
        entry.value_id = value_id;
        entry.register_class = flow_class;
        entry.candidate_value_register_class = machine_value->second.register_class;
        entry.flow_origin_kind = instruction->origin_kind;
        entry.flow_position_kind = uses_before_position
            ? NativeX64FlowOperandPositionKind::BeforeOperation
            : NativeX64FlowOperandPositionKind::AfterOperation;
        entry.source_span = instruction->source_span;
        entry.source_span_matches_emitter_anchor = same_source_span(
            instruction->source_span, anchor.source_span);
        entry.operand_kind = expected_kind;
        entry.flow_operation_location = flow_location;
        entry.emitter_operand_location = anchor.emitted_operand_location;
        entry.location_matches = register_class_matches && compare_physical_location(
            flow_location, anchor.emitted_operand_location, flow_class, entry);
        ++result.operand_operations_audited;
        if (address_read) ++result.address_operands_audited;
        if (entry.location_matches) {
            ++result.operand_location_matches;
            if (entry.flow_location_uses_frame) ++result.frame_location_matches;
            else ++result.register_location_matches;
        } else {
            ++result.operand_location_mismatches;
            if (!register_class_matches) {
                add_diagnostic(result.diagnostics, "N113J0016",
                    ("N112 operation register class disagrees with the authoritative N111 Value class at FlowOperationId " +
                     std::to_string(entry.flow_operation_id) + " ValueId " + std::to_string(value_id)).c_str());
            } else {
                add_diagnostic(result.diagnostics, "N113J0015",
                    ("N112 operation operand differs from the concrete N111 emitter operand at FlowOperationId " +
                     std::to_string(entry.flow_operation_id) + " ValueId " + std::to_string(value_id)).c_str());
            }
        }
        result.operands.push_back(std::move(entry));
    }

    result.all_operands_owned = result.operand_location_mismatches == 0U &&
        result.operand_location_matches == result.operand_operations_audited;
    result.succeeded = true;
    return result;
}

NativeX64FlowEdgeBundleEligibilityPlan audit_native_x64_flow_edge_bundle_eligibility(
    const NativeX64FlowEdgeTransferCandidatePlan& candidates,
    const NativeX64FlowEdgePerEdgeScratchPlan& per_edge_scratch,
    const NativeX64FlowOperationOperandOwnershipPlan& operation_ownership,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    const std::size_t function_index) {
    NativeX64FlowEdgeBundleEligibilityPlan result;
    result.function_index = function_index;
    const auto fail = [&](const char* code, const char* message) {
        add_diagnostic(result.diagnostics, code, message);
        return result;
    };

    if (function_index == InvalidNativeFunctionIndex ||
        !candidates.succeeded || candidates.ready_for_machine_insertion ||
        candidates.candidate_copies != candidates.candidates.size() ||
        !per_edge_scratch.succeeded ||
        !operation_ownership.succeeded ||
        operation_ownership.function_index != function_index) {
        return fail("N113K0001",
            "G11 requires a valid non-ready G6 inventory, successful G9/G10 audits and a matching FunctionEmitter index");
    }
    if (operation_ownership.operand_operations_audited != operation_ownership.operands.size() ||
        operation_ownership.operand_location_matches + operation_ownership.operand_location_mismatches !=
            operation_ownership.operand_operations_audited) {
        return fail("N113K0002", "G11 G10 operation-operand counters do not match the concrete crosswalk entries");
    }

    std::map<FlowEdgeId, std::vector<const NativeX64FlowEdgeTransferCandidate*>> candidates_by_edge;
    std::map<FlowEdgeId, std::pair<NativeRegisterBlockId, NativeRegisterBlockId>> edge_endpoints;
    std::set<std::pair<FlowEdgeId, std::uint32_t>> unique_edge_values;
    for (const auto& candidate : candidates.candidates) {
        const auto value_class = value_classes.find(candidate.copy.value_id);
        if (candidate.edge_id == InvalidFlowEdgeId || candidate.copy.value_id == 0U ||
            candidate.predecessor_block_id == InvalidNativeRegisterBlockId ||
            candidate.successor_block_id == InvalidNativeRegisterBlockId ||
            candidate.branch_anchor.edge_id != candidate.edge_id ||
            candidate.branch_anchor.function_index != function_index ||
            candidate.branch_anchor.from_block_id != candidate.predecessor_block_id ||
            candidate.branch_anchor.to_block_id != candidate.successor_block_id ||
            value_class == value_classes.end() ||
            !unique_edge_values.emplace(candidate.edge_id, candidate.copy.value_id).second) {
            return fail("N113K0003", "G11 G6 candidate contains an invalid/duplicate edge value, class or function-scoped branch anchor");
        }
        const auto endpoints = std::make_pair(candidate.predecessor_block_id, candidate.successor_block_id);
        const auto inserted = edge_endpoints.emplace(candidate.edge_id, endpoints);
        if (!inserted.second && inserted.first->second != endpoints) {
            return fail("N113K0004", "G11 one FlowEdgeId is associated with conflicting predecessor/successor blocks");
        }
        candidates_by_edge[candidate.edge_id].push_back(&candidate);
    }
    if (candidates_by_edge.size() != candidates.candidate_edges ||
        per_edge_scratch.edges.size() != per_edge_scratch.candidate_edges ||
        per_edge_scratch.candidate_edges != candidates.candidate_edges) {
        return fail("N113K0005", "G11 exact-edge candidate and G9 per-edge inventories disagree");
    }

    std::map<FlowEdgeId, const NativeX64FlowEdgePerEdgeScratchEntry*> scratch_by_edge;
    for (const auto& entry : per_edge_scratch.edges) {
        if (entry.edge_id == InvalidFlowEdgeId ||
            !scratch_by_edge.emplace(entry.edge_id, &entry).second) {
            return fail("N113K0006", "G11 G9 table contains an invalid or duplicate FlowEdgeId");
        }
    }
    for (const auto& edge_item : candidates_by_edge) {
        if (scratch_by_edge.find(edge_item.first) == scratch_by_edge.end())
            return fail("N113K0007", "G11 G9 per-edge table is missing a candidate FlowEdgeId");
    }
    for (const auto& edge_item : scratch_by_edge) {
        if (candidates_by_edge.find(edge_item.first) == candidates_by_edge.end())
            return fail("N113K0008", "G11 G9 per-edge table contains an unrequested FlowEdgeId");
    }

    std::map<std::uint32_t, std::vector<const NativeX64FlowOperationOperandOwnershipEntry*>>
        operation_operands_by_value;
    std::set<std::pair<std::uint64_t, std::uint32_t>> unique_operation_values;
    for (const auto& operand : operation_ownership.operands) {
        const auto value_class = value_classes.find(operand.value_id);
        if (operand.flow_operation_id == InvalidNativeFlowOperationId ||
            operand.flow_block_id == InvalidNativeRegisterBlockId ||
            operand.flow_operation_index == InvalidNativeFlowOperationIndex ||
            operand.value_id == 0U ||
            operand.operand_kind == X64FlowEmitterAnchor::OperandKind::None ||
            value_class == value_classes.end() ||
            !unique_operation_values.emplace(operand.flow_operation_id, operand.value_id).second) {
            return fail("N113K0009", "G11 G10 crosswalk contains an invalid/duplicate operation operand or an unknown ValueId/class");
        }
        const bool physical_match =
            operand.flow_location_uses_frame || operand.emitter_location_uses_frame
                ? (operand.flow_location_uses_frame && operand.emitter_location_uses_frame &&
                   operand.flow_frame_displacement < 0 &&
                   operand.emitter_frame_displacement < 0 &&
                   (operand.flow_frame_displacement % 8) == 0 &&
                   (operand.emitter_frame_displacement % 8) == 0 &&
                   operand.flow_frame_displacement == operand.emitter_frame_displacement)
                : (operand.flow_operation_location.kind == NativeRegisterLocationKind::Register &&
                   operand.emitter_operand_location.kind == NativeRegisterLocationKind::Register &&
                   operand.flow_operation_location.register_index ==
                       operand.emitter_operand_location.register_index);
        if (operand.location_matches && !physical_match) {
            return fail("N113K000A", "G11 G10 entry claims a physical location match but its register/frame evidence disagrees");
        }
        operation_operands_by_value[operand.value_id].push_back(&operand);
    }

    result.candidate_edges = static_cast<std::uint32_t>(candidates_by_edge.size());
    for (const auto& edge_item : candidates_by_edge) {
        NativeX64FlowEdgeBundleEligibilityEntry entry;
        entry.edge_id = edge_item.first;
        entry.predecessor_block_id = edge_endpoints.at(edge_item.first).first;
        entry.successor_block_id = edge_endpoints.at(edge_item.first).second;
        entry.candidate_values = static_cast<std::uint32_t>(edge_item.second.size());
        bool all_value_operands_proven = true;
        bool all_values_in_schedule = true;
        std::set<std::uint32_t> candidate_value_ids;
        for (const auto* candidate : edge_item.second) {
            candidate_value_ids.insert(candidate->copy.value_id);
            entry.value_ids.push_back(candidate->copy.value_id);
        }

        const auto* scratch = scratch_by_edge.at(edge_item.first);
        const auto endpoint_count = scratch->readiness.live_values_audited * 2U;
        entry.live_values_audited = scratch->readiness.live_values_audited;
        entry.operand_anchors_validated = scratch->readiness.operand_anchors_validated;
        entry.endpoint_ownership_matches = scratch->readiness.endpoint_ownership_matches;
        entry.endpoint_ownership_mismatches = scratch->readiness.endpoint_ownership_mismatches;
        entry.endpoint_value_evidence = scratch->readiness.endpoint_value_evidence;
        const auto& candidate_anchor = edge_item.second.front()->branch_anchor;
        entry.exact_g9_schedule = scratch->schedule_built &&
            scratch->schedule.edge_id == entry.edge_id &&
            scratch->schedule.bundle.edge_id == entry.edge_id &&
            scratch->schedule.branch_anchor.edge_id == entry.edge_id &&
            scratch->schedule.branch_anchor.function_index == function_index &&
            same_branch_anchor_identity(scratch->schedule.branch_anchor, candidate_anchor) &&
            scratch->schedule.predecessor_block_id == entry.predecessor_block_id &&
            scratch->schedule.successor_block_id == entry.successor_block_id &&
            scratch->schedule.branch_anchor.from_block_id == entry.predecessor_block_id &&
            scratch->schedule.branch_anchor.to_block_id == entry.successor_block_id &&
            !scratch->schedule.bundle.operations.empty();
        if (!entry.exact_g9_schedule) {
            entry.blockers.push_back("exact G9 schedule/branch-anchor identity is missing or inconsistent");
        } else {
            for (const auto value_id : candidate_value_ids) {
                const bool scheduled_value = std::any_of(
                    scratch->schedule.bundle.operations.begin(),
                    scratch->schedule.bundle.operations.end(),
                    [value_id](const NativeRegisterParallelCopyOperation& operation) {
                        return operation.value_id == value_id;
                    });
                if (!scheduled_value) all_values_in_schedule = false;
            }
            if (!all_values_in_schedule)
                entry.blockers.push_back("G9 scheduled bundle does not cover every exact G6 candidate ValueId");
        }

        entry.endpoint_ownership_proven = scratch->readiness.edge_id == entry.edge_id &&
            scratch->readiness.machine_operand_ownership_proven &&
            scratch->readiness.endpoint_ownership_mismatches == 0U &&
            scratch->readiness.endpoint_ownership_matches == endpoint_count &&
            scratch->readiness.operand_anchors_validated > 0U;
        if (!entry.endpoint_ownership_proven)
            entry.blockers.push_back("G8 endpoint ownership is incomplete for this exact edge");

        entry.scratch_liveness_proven = scratch->scratch_selection_proven &&
            scratch->readiness.scratch_liveness_proven;
        if (!entry.scratch_liveness_proven)
            entry.blockers.push_back("G9 has no GP/XMM scratch pair proven dead on this exact edge");

        for (const auto value_id : candidate_value_ids) {
            NativeX64FlowEdgeValueOperandEvidenceEntry value_evidence;
            value_evidence.value_id = value_id;
            value_evidence.register_class = value_classes.at(value_id);
            const auto operands = operation_operands_by_value.find(value_id);
            if (operands == operation_operands_by_value.end() || operands->second.empty()) {
                all_value_operands_proven = false;
                ++entry.operation_values_missing;
                entry.value_operand_evidence.push_back(std::move(value_evidence));
                continue;
            }

            value_evidence.concrete_evidence_found = true;
            value_evidence.operand_operations = static_cast<std::uint32_t>(operands->second.size());
            ++entry.operation_values_with_evidence;
            value_evidence.operations.reserve(operands->second.size());
            for (const auto* operand : operands->second) {
                NativeX64FlowEdgeBundleOperandEvidenceEntry operation_evidence;
                operation_evidence.flow_operation_id = operand->flow_operation_id;
                operation_evidence.flow_block_id = operand->flow_block_id;
                operation_evidence.flow_operation_index = operand->flow_operation_index;
                operation_evidence.operand_kind = static_cast<std::uint8_t>(operand->operand_kind);
                operation_evidence.flow_register_class = operand->register_class;
                operation_evidence.candidate_value_register_class = operand->candidate_value_register_class;
                operation_evidence.flow_origin_kind = operand->flow_origin_kind;
                operation_evidence.flow_position_kind = operand->flow_position_kind;
                operation_evidence.source_span = operand->source_span;
                operation_evidence.source_span_matches_emitter_anchor =
                    operand->source_span_matches_emitter_anchor;
                operation_evidence.flow_operation_location = operand->flow_operation_location;
                operation_evidence.emitter_operand_location = operand->emitter_operand_location;
                operation_evidence.flow_location_uses_frame = operand->flow_location_uses_frame;
                operation_evidence.flow_frame_displacement = operand->flow_frame_displacement;
                operation_evidence.emitter_location_uses_frame = operand->emitter_location_uses_frame;
                operation_evidence.emitter_frame_displacement = operand->emitter_frame_displacement;
                const bool register_class_matches = operand->register_class == value_evidence.register_class;
                operation_evidence.location_matches = register_class_matches && operand->location_matches;
                operation_evidence.mismatch_kind = classify_operand_mismatch(
                    operand->flow_operation_location, operand->emitter_operand_location,
                    value_evidence.register_class, operand->flow_location_uses_frame,
                    operand->emitter_location_uses_frame, operand->flow_frame_displacement,
                    operand->emitter_frame_displacement, register_class_matches,
                    operation_evidence.location_matches);
                accumulate_mismatch_count(value_evidence.mismatch_counts, operation_evidence.mismatch_kind);
                accumulate_mismatch_count(entry.mismatch_counts, operation_evidence.mismatch_kind);
                accumulate_mismatch_count(result.mismatch_counts, operation_evidence.mismatch_kind);
                value_evidence.operations.push_back(std::move(operation_evidence));

                if (register_class_matches && operand->location_matches) {
                    ++value_evidence.matching_operations;
                } else {
                    ++value_evidence.mismatching_operations;
                }
            }
            std::sort(value_evidence.operations.begin(), value_evidence.operations.end(),
                [](const auto& left, const auto& right) {
                    return left.flow_operation_id < right.flow_operation_id;
                });
            value_evidence.all_observed_operands_match =
                value_evidence.mismatching_operations == 0U &&
                value_evidence.matching_operations == value_evidence.operand_operations;
            if (value_evidence.all_observed_operands_match) {
                ++entry.operation_values_proven;
            } else {
                ++entry.operation_values_mismatched;
                all_value_operands_proven = false;
            }
            entry.value_operand_evidence.push_back(std::move(value_evidence));
        }
        entry.operation_operand_ownership_proven = all_value_operands_proven &&
            entry.operation_values_proven == entry.candidate_values;
        if (!entry.operation_operand_ownership_proven)
            entry.blockers.push_back("G10 has missing or mismatched concrete operation-operand evidence for one or more edge Values");

        entry.proof_gate_passed = entry.exact_g9_schedule && all_values_in_schedule &&
            entry.endpoint_ownership_proven && entry.scratch_liveness_proven &&
            entry.operation_operand_ownership_proven;
        if (entry.proof_gate_passed) {
            ++result.proof_gate_passed_edges;
        } else {
            ++result.blocked_edges;
            if (!entry.exact_g9_schedule || !all_values_in_schedule) ++result.schedule_blocked_edges;
            if (!entry.endpoint_ownership_proven) ++result.endpoint_ownership_blocked_edges;
            if (!entry.scratch_liveness_proven) ++result.scratch_blocked_edges;
            if (!entry.operation_operand_ownership_proven) ++result.operation_operand_blocked_edges;
        }
        result.edges.push_back(std::move(entry));
    }

    result.succeeded = true;
    if (result.blocked_edges != 0U)
        add_diagnostic(result.diagnostics, "N113K0010",
            "G11 retained one or more exact CFG edges as blocked; only edges passing every independent proof gate are marked eligible for later review");
    if (result.proof_gate_passed_edges != 0U)
        add_diagnostic(result.diagnostics, "N113K0011",
            "G11 identified per-edge proof-gate passes; physical Edge Bundle insertion remains a separate, unperformed stage");
    return result;
}

namespace {

NativeX64ParallelCopyEncodingResult encode_native_x64_parallel_copy_schedule_impl(
    const std::vector<NativeRegisterParallelCopyOperation>& operations,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    const NativeRegisterLocation cycle_storage,
    const NativeRegisterLocation expansion_temporary,
    const NativeX64FrameLocationMap& frame_locations,
    const bool memory_cycle_storage,
    const bool gp_xmm_expansion) {
    NativeX64ParallelCopyEncodingResult result;
    if (memory_cycle_storage) {
        if (cycle_storage.kind != NativeRegisterLocationKind::SpillSlot ||
            expansion_temporary.kind != NativeRegisterLocationKind::Temporary) {
            add_diagnostic(result.diagnostics, "N113G22E0001",
                "memory cycle encoding requires a SpillSlot cycle storage and a separate Temporary expansion register");
            return result;
        }
        const auto scratch = frame_locations.spill_slot_offsets.find(cycle_storage.spill_slot);
        if (scratch == frame_locations.spill_slot_offsets.end()) {
            add_diagnostic(result.diagnostics, "N113G22E0002",
                "memory cycle SpillSlot has no concrete active-frame displacement");
            return result;
        }
        if (scratch->second >= 0 || (scratch->second % 8) != 0) {
            add_diagnostic(result.diagnostics, "N113G22E0003",
                "memory cycle SpillSlot must map to a negative 8-byte-aligned RBP displacement");
            return result;
        }
        for (const auto& item : frame_locations.spill_slot_offsets) {
            if (item.first != cycle_storage.spill_slot && item.second == scratch->second) {
                add_diagnostic(result.diagnostics, "N113G22E0004",
                    "memory cycle SpillSlot aliases another physical SpillSlot frame displacement");
                return result;
            }
        }
        for (const auto& item : frame_locations.home_slot_offsets) {
            if (item.second == scratch->second) {
                add_diagnostic(result.diagnostics, "N113G22E0005",
                    "memory cycle SpillSlot aliases a canonical HomeSlot frame displacement");
                return result;
            }
        }

        if (gp_xmm_expansion &&
            (expansion_temporary.register_index < 1U || expansion_temporary.register_index > 5U)) {
            add_diagnostic(result.diagnostics, "N113G24E0001",
                "GP MOVQ expansion requires a Win64 caller-saved XMM1-XMM5 temporary; XMM0 and nonvolatile XMM6-XMM15 are excluded");
            return result;
        }
        bool cycle_slot_is_written = false;
        bool cycle_slot_is_read = false;
        bool schedule_class_found = false;
        NativeRegisterClass schedule_class = NativeRegisterClass::GeneralPurpose;
        for (const auto& operation : operations) {
            const auto value_class = value_classes.find(operation.value_id);
            if (value_class == value_classes.end()) {
                add_diagnostic(result.diagnostics, "N109BE0007",
                    "parallel-copy operation references a value with no register class");
                return result;
            }
            if (!schedule_class_found) {
                schedule_class = value_class->second;
                schedule_class_found = true;
            } else if (schedule_class != value_class->second) {
                add_diagnostic(result.diagnostics, "N113G22E0010",
                    "memory-cycle encoder requires one register class per schedule; split GP and XMM bundles first");
                return result;
            }
            const auto conflicts_with_expansion = [&](const NativeRegisterLocation& location) {
                if (location.kind == NativeRegisterLocationKind::Temporary) return true;
                // GP and XMM are disjoint physical register banks. For this
                // explicitly GP-typed path, RAX..R15 numeric indices cannot
                // alias XMM1..XMM5; do not reject equal numbers across banks.
                if (gp_xmm_expansion) return false;
                return location.kind == NativeRegisterLocationKind::Register &&
                    location.register_index == expansion_temporary.register_index;
            };
            if (conflicts_with_expansion(operation.source) ||
                conflicts_with_expansion(operation.destination)) {
                add_diagnostic(result.diagnostics, "N113G22E0006",
                    "memory expansion register aliases an explicit transfer operand or Temporary location");
                return result;
            }
            const bool source_is_cycle_slot =
                operation.source.kind == NativeRegisterLocationKind::SpillSlot &&
                operation.source.spill_slot == cycle_storage.spill_slot;
            const bool destination_is_cycle_slot =
                operation.destination.kind == NativeRegisterLocationKind::SpillSlot &&
                operation.destination.spill_slot == cycle_storage.spill_slot;
            const bool touches_cycle_slot = source_is_cycle_slot || destination_is_cycle_slot;
            if (operation.uses_cycle_temporary != touches_cycle_slot) {
                add_diagnostic(result.diagnostics, "N113G22E0011",
                    "cycle-temporary operation marker must exactly match reads/writes of the dedicated memory cycle SpillSlot");
                return result;
            }
            cycle_slot_is_written = cycle_slot_is_written || destination_is_cycle_slot;
            cycle_slot_is_read = cycle_slot_is_read || source_is_cycle_slot;
            if (gp_xmm_expansion) {
                if (value_class->second != NativeRegisterClass::GeneralPurpose ||
                    !valid_xmm_register(expansion_temporary.register_index)) {
                    add_diagnostic(result.diagnostics, "N113G24E0002",
                        "XMM MOVQ expansion is valid only for a GP-only schedule and a valid XMM register");
                    return result;
                }
            } else {
                if (value_class->second == NativeRegisterClass::GeneralPurpose &&
                    !valid_gp_register(expansion_temporary.register_index)) {
                    add_diagnostic(result.diagnostics, "N113G22E0007",
                        "memory expansion register is not a valid allocatable GP register");
                    return result;
                }
                if (value_class->second == NativeRegisterClass::FloatingPoint &&
                    !valid_xmm_register(expansion_temporary.register_index)) {
                    add_diagnostic(result.diagnostics, "N113G22E0008",
                        "memory expansion register is not a valid XMM register");
                    return result;
                }
            }
        }
        if (!cycle_slot_is_written || !cycle_slot_is_read) {
            add_diagnostic(result.diagnostics, "N113G22E0009",
                "memory cycle SpillSlot must be both written and subsequently read by the resolved schedule");
            return result;
        }
    } else if (cycle_storage.kind != NativeRegisterLocationKind::Temporary ||
               expansion_temporary.kind != NativeRegisterLocationKind::Temporary ||
               cycle_storage.register_index != expansion_temporary.register_index) {
        add_diagnostic(result.diagnostics, "N109BE0006",
            "x64 parallel-copy encoder requires a caller-reserved temporary register");
        return result;
    }

    for (const auto& operation : operations) {
        const auto value_class = value_classes.find(operation.value_id);
        if (value_class == value_classes.end()) {
            add_diagnostic(result.diagnostics, "N109BE0007",
                "parallel-copy operation references a value with no register class");
            return result;
        }
        const auto register_class = value_class->second;
        ResolvedLocation source;
        ResolvedLocation destination;
        if (!resolve_location(operation.source, register_class, expansion_temporary,
                              frame_locations, source, result.diagnostics) ||
            !resolve_location(operation.destination, register_class, expansion_temporary,
                              frame_locations, destination, result.diagnostics)) return result;
        if (same_location(operation.source, operation.destination)) continue;

        if (!source.is_register && !destination.is_register) {
            ResolvedLocation temporary;
            temporary.is_register = true;
            temporary.register_index = expansion_temporary.register_index;
            if (register_class == NativeRegisterClass::GeneralPurpose && !gp_xmm_expansion &&
                !valid_gp_register(temporary.register_index)) {
                add_diagnostic(result.diagnostics, "N109BE0008",
                    "GP memory-to-memory copy requires a valid GP expansion register");
                return result;
            }
            if (register_class == NativeRegisterClass::FloatingPoint &&
                !valid_xmm_register(temporary.register_index)) {
                add_diagnostic(result.diagnostics, "N109BE0009",
                    "XMM memory-to-memory copy requires a valid XMM expansion register");
                return result;
            }
            if (gp_xmm_expansion || register_class == NativeRegisterClass::FloatingPoint) {
                // MOVQ xmm, m64 followed by MOVQ m64, xmm preserves all 64 bits
                // while allowing a GP spill/home copy to use the XMM register bank.
                emit_xmm_move(result.bytes, source, temporary);
                emit_xmm_move(result.bytes, temporary, destination);
            } else {
                emit_gp_move(result.bytes, source, temporary);
                emit_gp_move(result.bytes, temporary, destination);
            }
            ++result.memory_to_memory_expansions;
            result.machine_moves_emitted += 2U;
            result.temporary_register_moves += 2U;
            continue;
        }

        const bool uses_temporary = operation.source.kind == NativeRegisterLocationKind::Temporary ||
                                    operation.destination.kind == NativeRegisterLocationKind::Temporary;
        if (register_class == NativeRegisterClass::GeneralPurpose)
            emit_gp_move(result.bytes, source, destination);
        else
            emit_xmm_move(result.bytes, source, destination);
        ++result.machine_moves_emitted;
        if (uses_temporary) ++result.temporary_register_moves;
    }
    result.succeeded = true;
    return result;
}

} // namespace

NativeX64ParallelCopyEncodingResult encode_native_x64_parallel_copy_schedule(
    const std::vector<NativeRegisterParallelCopyOperation>& operations,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    const NativeRegisterLocation cycle_temporary,
    const NativeX64FrameLocationMap& frame_locations) {
    return encode_native_x64_parallel_copy_schedule_impl(
        operations, value_classes, cycle_temporary, cycle_temporary,
        frame_locations, false, false);
}

NativeX64ParallelCopyEncodingResult encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
    const std::vector<NativeRegisterParallelCopyOperation>& operations,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    const NativeRegisterLocation memory_cycle_temporary,
    const NativeRegisterLocation expansion_temporary,
    const NativeX64FrameLocationMap& frame_locations) {
    return encode_native_x64_parallel_copy_schedule_impl(
        operations, value_classes, memory_cycle_temporary, expansion_temporary,
        frame_locations, true, false);
}

NativeX64ParallelCopyEncodingResult encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
    const std::vector<NativeRegisterParallelCopyOperation>& operations,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    const NativeRegisterLocation memory_cycle_temporary,
    const NativeRegisterLocation xmm_expansion_temporary,
    const NativeX64FrameLocationMap& frame_locations) {
    if (xmm_expansion_temporary.kind != NativeRegisterLocationKind::Temporary) {
        NativeX64ParallelCopyEncodingResult result;
        add_diagnostic(result.diagnostics, "N113G24E0003",
            "GP MOVQ expansion register must be represented by a dedicated Temporary location");
        return result;
    }
    return encode_native_x64_parallel_copy_schedule_impl(
        operations, value_classes, memory_cycle_temporary, xmm_expansion_temporary,
        frame_locations, true, true);
}

NativeX64GpXmmExpansionLivenessPlan audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeX64FlowEdgeLocationAuditPlan& edge_location_audit,
    const NativeX64FlowEdgeTransferCandidatePlan& candidates,
    const std::vector<std::uint8_t>& expression_reserved_xmm) {
    NativeX64GpXmmExpansionLivenessPlan result;
    const auto fail = [&](const char* code, const char* message) {
        add_diagnostic(result.diagnostics, code, message);
        return result;
    };

    const auto validated = validate_native_register_function_ir(function.register_ir);
    if (!validated.succeeded || !liveness.succeeded || !liveness.converged ||
        liveness.blocks.size() != function.register_ir.blocks.size() ||
        !edge_location_audit.succeeded || !candidates.succeeded ||
        candidates.ready_for_machine_insertion ||
        candidates.candidate_copies != candidates.candidates.size()) {
        return fail("N113G25E0001",
            "G25 requires valid CFG IR, converged liveness, successful G4/G6 plans and a non-insertion-ready candidate inventory");
    }
    if (edge_location_audit.edges_audited != function.edges.size()) {
        return fail("N113G25E0002",
            "G25 G4 edge count does not cover the exact Function CFG edge table");
    }
    if (edge_location_audit.live_values_audited != edge_location_audit.values.size()) {
        return fail("N113G25E0016",
            "G25 G4 live-value counter does not equal the number of supplied exact-edge records");
    }

    std::set<std::uint8_t> reserved_xmm;
    for (const auto reg : expression_reserved_xmm) {
        if (reg >= 16U)
            return fail("N113G25E0003", "G25 expression-reserved XMM register index is outside the AMD64 register file");
        if (!reserved_xmm.insert(reg).second)
            return fail("N113G25E0004", "G25 expression-reserved XMM register list contains a duplicate index");
    }

    std::map<std::uint32_t, NativeRegisterClass> value_classes;
    for (const auto& value : validated.normalized_function.values) {
        if (value.value_id == 0U || !value_classes.emplace(value.value_id, value.register_class).second)
            return fail("N113G25E0005", "G25 Function CFG contains an invalid or duplicate ValueId");
    }

    std::map<FlowEdgeId, const FlowEdge*> edges_by_id;
    for (const auto& edge : function.edges) {
        if (edge.id == InvalidFlowEdgeId || edge.from_block_id >= validated.normalized_function.blocks.size() ||
            edge.to_block_id >= validated.normalized_function.blocks.size() ||
            !edges_by_id.emplace(edge.id, &edge).second) {
            return fail("N113G25E0006", "G25 Function CFG edge table contains an invalid or duplicate exact EdgeId");
        }
    }

    // G4's record set must equal the independently available block-boundary
    // liveness intersection for every CFG edge, including edges with no G6 move.
    std::map<FlowEdgeId, std::map<std::uint32_t, const NativeX64FlowEdgeValueAudit*>> g4_by_edge;
    for (const auto& audit : edge_location_audit.values) {
        const auto edge = edges_by_id.find(audit.edge_id);
        if (edge == edges_by_id.end() || edge->second == nullptr ||
            audit.predecessor_block_id != edge->second->from_block_id ||
            audit.successor_block_id != edge->second->to_block_id ||
            value_classes.find(audit.value_id) == value_classes.end() ||
            value_classes.at(audit.value_id) != audit.register_class || audit.value_id == 0U ||
            !g4_by_edge[audit.edge_id].emplace(audit.value_id, &audit).second) {
            return fail("N113G25E0007", "G25 G4 live-through table contains an unknown, duplicate, or edge-mismatched ValueId");
        }
    }

    for (const auto& edge_item : edges_by_id) {
        const auto edge_id = edge_item.first;
        const auto& edge = *edge_item.second;
        const auto predecessor = static_cast<std::size_t>(edge.from_block_id);
        const auto successor = static_cast<std::size_t>(edge.to_block_id);
        if (predecessor >= liveness.blocks.size() || successor >= liveness.blocks.size())
            return fail("N113G25E0008", "G25 liveness endpoint block is outside the converged liveness table");

        const auto& live_out = liveness.blocks[predecessor].live_out;
        const auto& live_in = liveness.blocks[successor].live_in;
        const std::set<std::uint32_t> live_out_set(live_out.begin(), live_out.end());
        const std::set<std::uint32_t> live_in_set(live_in.begin(), live_in.end());
        if (live_out_set.size() != live_out.size() || live_in_set.size() != live_in.size())
            return fail("N113G25E0009", "G25 predecessor live-out or successor live-in contains duplicate ValueIds");

        std::set<std::uint32_t> expected_live;
        for (const auto value_id : live_out_set) {
            if (live_in_set.find(value_id) != live_in_set.end()) {
                if (value_classes.find(value_id) == value_classes.end())
                    return fail("N113G25E000A", "G25 liveness intersection contains an undeclared ValueId");
                expected_live.insert(value_id);
            }
        }
        const auto actual = g4_by_edge.find(edge_id);
        const std::size_t actual_count = actual == g4_by_edge.end() ? 0U : actual->second.size();
        if (actual_count != expected_live.size())
            return fail("N113G25E000B", "G25 G4 live-through record count differs from live_out intersect live_in on an exact edge");
        if (actual != g4_by_edge.end()) {
            for (const auto value_id : expected_live) {
                if (actual->second.find(value_id) == actual->second.end())
                    return fail("N113G25E000C", "G25 G4 live-through ValueId set differs from the exact liveness intersection");
            }
        }
        result.g4_live_values_reconciled += static_cast<std::uint32_t>(expected_live.size());
        ++result.cfg_edges_reconciled;
    }
    const auto counted_g4_changes = static_cast<std::uint32_t>(std::count_if(
        edge_location_audit.values.begin(), edge_location_audit.values.end(),
        [](const auto& value) { return value.flow_location_changes; }));
    if (counted_g4_changes != edge_location_audit.flow_location_changes) {
        return fail("N113G25E0017",
            "G25 G4 flow-location-change counter does not equal the supplied endpoint records");
    }
    result.g4_live_sets_reconciled = true;

    std::map<FlowEdgeId, std::vector<const NativeX64FlowEdgeTransferCandidate*>> candidates_by_edge;
    std::set<std::pair<FlowEdgeId, std::uint32_t>> unique_candidates;
    for (const auto& candidate : candidates.candidates) {
        const auto edge = edges_by_id.find(candidate.edge_id);
        if (edge == edges_by_id.end() || edge->second == nullptr || candidate.copy.value_id == 0U ||
            !unique_candidates.emplace(candidate.edge_id, candidate.copy.value_id).second ||
            candidate.predecessor_block_id != edge->second->from_block_id ||
            candidate.successor_block_id != edge->second->to_block_id ||
            candidate.branch_anchor.edge_id != candidate.edge_id ||
            candidate.branch_anchor.from_block_id != candidate.predecessor_block_id ||
            candidate.branch_anchor.to_block_id != candidate.successor_block_id ||
            !g7_valid_edge_anchor(candidate.branch_anchor)) {
            return fail("N113G25E000D", "G25 G6 candidate has an invalid exact EdgeId, duplicate ValueId, or malformed branch anchor");
        }
        const auto edge_audit = g4_by_edge.find(candidate.edge_id);
        if (edge_audit == g4_by_edge.end())
            return fail("N113G25E000E", "G25 G6 candidate has no matching G4 exact-edge live-value table");
        const auto value_audit = edge_audit->second.find(candidate.copy.value_id);
        if (value_audit == edge_audit->second.end() || value_audit->second == nullptr ||
            !value_audit->second->flow_location_changes ||
            !same_location(candidate.copy.source, value_audit->second->predecessor_flow_location) ||
            !same_location(candidate.copy.destination, value_audit->second->successor_flow_location) ||
            !same_location(candidate.emitter_active_location, value_audit->second->emitter_active_location)) {
            return fail("N113G25E000F", "G25 G6 candidate source/destination/N111 location does not exactly match its G4 evidence");
        }
        candidates_by_edge[candidate.edge_id].push_back(&candidate);
    }
    if (candidates_by_edge.size() != candidates.candidate_edges ||
        candidates.candidates.size() != candidates.candidate_copies ||
        candidates.flow_location_changes_considered != edge_location_audit.flow_location_changes) {
        return fail("N113G25E0010", "G25 G6 candidate edge/copy/change inventory does not match its exact unique records and G4 counter");
    }

    // G6 must enumerate every and only location-changing G4 Value globally,
    // including edges with zero or missing candidates. Per-present-edge checks
    // alone would let an omitted entire edge escape validation.
    std::set<std::pair<FlowEdgeId, std::uint32_t>> expected_all_candidates;
    for (const auto& edge_item : g4_by_edge) {
        for (const auto& value_item : edge_item.second) {
            if (value_item.second->flow_location_changes)
                expected_all_candidates.emplace(edge_item.first, value_item.first);
        }
    }
    if (unique_candidates != expected_all_candidates) {
        return fail("N113G25E0018", "G25 G6 global EdgeId/ValueId set does not exactly cover all G4 location changes");
    }

    // G6 must enumerate every and only location-changing G4 Value on each
    // candidate edge. Otherwise a partial move set could mask an operand alias.
    for (const auto& edge_item : candidates_by_edge) {
        const auto audit_edge = g4_by_edge.find(edge_item.first);
        if (audit_edge == g4_by_edge.end())
            return fail("N113G25E0011", "G25 G6 edge is missing its G4 live-through table");
        std::set<std::uint32_t> expected_candidates;
        for (const auto& value_item : audit_edge->second) {
            if (value_item.second->flow_location_changes)
                expected_candidates.insert(value_item.first);
        }
        std::set<std::uint32_t> actual_candidates;
        const X64FlowBranchAnchor* anchor = nullptr;
        for (const auto* candidate : edge_item.second) {
            actual_candidates.insert(candidate->copy.value_id);
            if (anchor == nullptr) anchor = &candidate->branch_anchor;
            else if (!same_branch_anchor_identity(*anchor, candidate->branch_anchor))
                return fail("N113G25E0012", "G25 G6 candidates on one edge disagree on exact branch-anchor identity");
        }
        if (actual_candidates != expected_candidates)
            return fail("N113G25E0013", "G25 G6 ValueId set does not exactly cover all G4 location changes for the edge");
    }
    result.candidate_edge_identities_exact = true;

    // Analyze candidates in stable EdgeId order and XMM1..XMM5 order.
    for (const auto& edge_item : candidates_by_edge) {
        NativeX64FlowEdgeGpXmmExpansionEvidence edge_evidence;
        edge_evidence.edge_id = edge_item.first;
        edge_evidence.predecessor_block_id = edge_item.second.front()->predecessor_block_id;
        edge_evidence.successor_block_id = edge_item.second.front()->successor_block_id;
        const auto live_values = g4_by_edge.find(edge_item.first);
        edge_evidence.actual_g4_live_values = live_values == g4_by_edge.end()
            ? 0U : static_cast<std::uint32_t>(live_values->second.size());
        edge_evidence.expected_g4_live_values = edge_evidence.actual_g4_live_values;
        edge_evidence.g4_live_set_complete = true;
        edge_evidence.g6_candidate_values = static_cast<std::uint32_t>(edge_item.second.size());
        edge_evidence.branch_anchor_identity_exact = true;
        edge_evidence.gp_only_candidates = true;

        for (const auto* candidate : edge_item.second) {
            const auto value_class = value_classes.find(candidate->copy.value_id);
            if (value_class == value_classes.end() || value_class->second != NativeRegisterClass::GeneralPurpose) {
                edge_evidence.gp_only_candidates = false;
                continue;
            }
            if (is_memory_location(candidate->copy.source) &&
                is_memory_location(candidate->copy.destination)) {
                edge_evidence.gp_memory_to_memory_required = true;
            }
        }

        for (std::uint8_t xmm = 1U; xmm <= 5U; ++xmm) {
            NativeX64FlowEdgeXmmExpansionRegisterEvidence candidate_evidence;
            candidate_evidence.xmm_register = xmm;
            candidate_evidence.win64_volatile = true;
            candidate_evidence.g4_live_set_complete = edge_evidence.g4_live_set_complete;
            candidate_evidence.expression_reserved = reserved_xmm.find(xmm) != reserved_xmm.end();

            if (live_values != g4_by_edge.end()) {
                for (const auto& value_item : live_values->second) {
                    const auto& value = *value_item.second;
                    if (value.register_class != NativeRegisterClass::FloatingPoint) continue;
                    const auto aliases_xmm = [xmm](const NativeRegisterLocation& location) {
                        return location.kind == NativeRegisterLocationKind::Register &&
                            location.register_index == xmm;
                    };
                    if (aliases_xmm(value.predecessor_flow_location) ||
                        aliases_xmm(value.successor_flow_location) ||
                        aliases_xmm(value.emitter_active_location)) {
                        candidate_evidence.live_value_conflict = true;
                        candidate_evidence.conflicting_value_ids.push_back(value.value_id);
                    }
                }
            }
            std::sort(candidate_evidence.conflicting_value_ids.begin(),
                      candidate_evidence.conflicting_value_ids.end());
            candidate_evidence.conflicting_value_ids.erase(
                std::unique(candidate_evidence.conflicting_value_ids.begin(),
                            candidate_evidence.conflicting_value_ids.end()),
                candidate_evidence.conflicting_value_ids.end());
            candidate_evidence.conflicting_values = static_cast<std::uint32_t>(
                candidate_evidence.conflicting_value_ids.size());
            candidate_evidence.available = edge_evidence.gp_only_candidates &&
                candidate_evidence.g4_live_set_complete &&
                !candidate_evidence.expression_reserved &&
                !candidate_evidence.live_value_conflict;

            ++result.xmm_register_candidates_audited;
            if (candidate_evidence.available) ++result.xmm_register_candidates_dead;
            else ++result.xmm_register_candidates_blocked;
            edge_evidence.xmm_candidates.push_back(std::move(candidate_evidence));
        }

        if (edge_evidence.gp_memory_to_memory_required && edge_evidence.gp_only_candidates) {
            ++result.gp_memory_expansion_edges;
            const auto available = std::find_if(
                edge_evidence.xmm_candidates.begin(), edge_evidence.xmm_candidates.end(),
                [](const auto& candidate) { return candidate.available; });
            if (available != edge_evidence.xmm_candidates.end()) {
                edge_evidence.selected_xmm_register = available->xmm_register;
                edge_evidence.xmm_expansion_selection_proven = true;
                ++result.edges_with_dead_expansion_register;
            }
        }
        result.edges.push_back(std::move(edge_evidence));
    }

    result.candidate_edges_audited = static_cast<std::uint32_t>(result.edges.size());
    result.all_required_edges_have_dead_xmm =
        result.edges_with_dead_expansion_register == result.gp_memory_expansion_edges;
    result.machine_insertion_ready = false;
    result.succeeded = true;
    if (result.gp_memory_expansion_edges != result.edges_with_dead_expansion_register) {
        add_diagnostic(result.diagnostics, "N113G25E0014",
            "G25 found a GP memory-expansion edge without an XMM1-XMM5 register proven dead by exact G4/N111 evidence");
    }
    add_diagnostic(result.diagnostics, "N113G25E0015",
        "G25 proves only the exact-edge XMM expansion-register liveness contract; dedicated cycle SpillSlot reservation, frame/unwind updates, Product Source proof and edge insertion remain separate prerequisites");
    return result;
}

NativeX64MultiEdgeMemoryCycleAuditPlan audit_native_x64_multi_edge_gp_memory_cycle_protocol(
    const NativeX64GpXmmExpansionLivenessPlan& g25_liveness,
    const std::vector<NativeX64GpMemoryCycleEdgeScheduleInput>& edge_schedules,
    const NativeRegisterLocation cycle_spill_slot,
    const NativeX64FrameLocationMap& frame_locations) {
    NativeX64MultiEdgeMemoryCycleAuditPlan result;
    const auto fail = [&](const char* code, const char* message) {
        add_diagnostic(result.diagnostics, code, message);
        return result;
    };

    if (!g25_liveness.succeeded || !g25_liveness.g4_live_sets_reconciled ||
        !g25_liveness.candidate_edge_identities_exact || g25_liveness.machine_insertion_ready ||
        !g25_liveness.all_required_edges_have_dead_xmm ||
        g25_liveness.edges_with_dead_expansion_register != g25_liveness.gp_memory_expansion_edges ||
        g25_liveness.candidate_edges_audited != g25_liveness.edges.size()) {
        return fail("N113G26E0001",
            "G26 requires a successful, exact-edge G25 XMM liveness plan with every GP memory-expansion edge proven and insertion still disabled");
    }
    if (cycle_spill_slot.kind != NativeRegisterLocationKind::SpillSlot) {
        return fail("N113G26E0002", "G26 requires one dedicated SpillSlot as the shared memory cycle storage");
    }
    const auto mapped_slot = frame_locations.spill_slot_offsets.find(cycle_spill_slot.spill_slot);
    if (mapped_slot == frame_locations.spill_slot_offsets.end() || mapped_slot->second >= 0 ||
        (mapped_slot->second % 8) != 0) {
        return fail("N113G26E0003", "G26 cycle SpillSlot must have a concrete negative 8-byte-aligned frame displacement");
    }
    for (const auto& item : frame_locations.spill_slot_offsets) {
        if (item.first != cycle_spill_slot.spill_slot && item.second == mapped_slot->second) {
            return fail("N113G26E0004", "G26 shared cycle SpillSlot aliases another physical SpillSlot displacement");
        }
    }
    for (const auto& item : frame_locations.home_slot_offsets) {
        if (item.second == mapped_slot->second) {
            return fail("N113G26E0005", "G26 shared cycle SpillSlot aliases a canonical HomeSlot displacement");
        }
    }
    result.cycle_slot_frame_mapping_valid = true;

    std::map<FlowEdgeId, const NativeX64FlowEdgeGpXmmExpansionEvidence*> g25_edges;
    std::set<std::uint8_t> selected_xmms;
    std::uint32_t required_edges = 0U;
    for (const auto& edge : g25_liveness.edges) {
        if (edge.edge_id == InvalidFlowEdgeId || !g25_edges.emplace(edge.edge_id, &edge).second) {
            return fail("N113G26E0006", "G25 plan contains an invalid or duplicate exact EdgeId");
        }
        if (!edge.gp_memory_to_memory_required) continue;
        ++required_edges;
        if (!edge.gp_only_candidates || !edge.g4_live_set_complete ||
            !edge.xmm_expansion_selection_proven || edge.selected_xmm_register < 1U ||
            edge.selected_xmm_register > 5U) {
            return fail("N113G26E0007", "A required G25 edge lacks a complete GP-only exact-edge XMM1-XMM5 proof");
        }
        selected_xmms.insert(edge.selected_xmm_register);
    }
    if (required_edges != g25_liveness.gp_memory_expansion_edges ||
        required_edges != edge_schedules.size() || required_edges == 0U) {
        return fail("N113G26E0008", "G26 schedule EdgeId set does not cover every and only G25 GP memory-expansion edge");
    }

    std::set<FlowEdgeId> scheduled_edges;
    std::uint32_t edges_with_multiple_cycles = 0U;
    for (const auto& schedule : edge_schedules) {
        if (schedule.edge_id == InvalidFlowEdgeId || !scheduled_edges.insert(schedule.edge_id).second) {
            return fail("N113G26E0009", "G26 schedule contains an invalid or duplicate exact EdgeId");
        }
        const auto proof_it = g25_edges.find(schedule.edge_id);
        if (proof_it == g25_edges.end() || proof_it->second == nullptr ||
            !proof_it->second->gp_memory_to_memory_required ||
            !proof_it->second->g4_live_set_complete ||
            !proof_it->second->xmm_expansion_selection_proven ||
            proof_it->second->selected_xmm_register < 1U || proof_it->second->selected_xmm_register > 5U) {
            return fail("N113G26E000A", "G26 schedule EdgeId has no matching exact G25 XMM expansion proof");
        }
        if (schedule.operations.empty()) {
            return fail("N113G26E000B", "G26 refuses an empty schedule for a GP memory-expansion edge");
        }

        NativeX64MultiEdgeMemoryCycleEdgeEvidence edge_evidence;
        edge_evidence.edge_id = schedule.edge_id;
        edge_evidence.selected_xmm_register = proof_it->second->selected_xmm_register;
        edge_evidence.g25_exact_edge_xmm_proof_matched = true;
        edge_evidence.operations_audited = static_cast<std::uint32_t>(schedule.operations.size());

        bool cycle_slot_occupied = false;
        std::uint32_t saved_value_id = 0U;
        std::uint32_t save_operation_index = 0U;
        for (std::size_t index = 0U; index < schedule.operations.size(); ++index) {
            const auto& operation = schedule.operations[index];
            const bool source_is_cycle_slot =
                operation.source.kind == NativeRegisterLocationKind::SpillSlot &&
                operation.source.spill_slot == cycle_spill_slot.spill_slot;
            const bool destination_is_cycle_slot =
                operation.destination.kind == NativeRegisterLocationKind::SpillSlot &&
                operation.destination.spill_slot == cycle_spill_slot.spill_slot;
            const bool touches_cycle_slot = source_is_cycle_slot || destination_is_cycle_slot;
            if (operation.uses_cycle_temporary != touches_cycle_slot) {
                return fail("N113G26E000C", "G26 cycle-temporary marker does not exactly match access to the dedicated SpillSlot");
            }
            if (source_is_cycle_slot && destination_is_cycle_slot) {
                return fail("N113G26E000D", "G26 rejects a single operation that reads and writes the same cycle SpillSlot");
            }
            if (operation.value_id == 0U) {
                return fail("N113G26E000E", "G26 cycle protocol contains an invalid ValueId");
            }
            if (destination_is_cycle_slot) {
                if (cycle_slot_occupied) {
                    return fail("N113G26E000F", "G26 detected a cycle SpillSlot overwrite before the previous transaction was restored");
                }
                cycle_slot_occupied = true;
                saved_value_id = operation.value_id;
                save_operation_index = static_cast<std::uint32_t>(index);
            } else if (source_is_cycle_slot) {
                if (!cycle_slot_occupied) {
                    return fail("N113G26E0010", "G26 detected a cycle SpillSlot restore before a matching save");
                }
                if (operation.value_id != saved_value_id) {
                    return fail("N113G26E0011", "G26 cycle SpillSlot restore ValueId differs from the saved ValueId");
                }
                NativeX64MemoryCycleTransactionEvidence transaction;
                transaction.cycle_index = edge_evidence.cycle_transactions_audited;
                transaction.value_id = saved_value_id;
                transaction.save_operation_index = save_operation_index;
                transaction.restore_operation_index = static_cast<std::uint32_t>(index);
                edge_evidence.transactions.push_back(transaction);
                ++edge_evidence.cycle_transactions_audited;
                cycle_slot_occupied = false;
                saved_value_id = 0U;
            }
        }
        if (cycle_slot_occupied) {
            return fail("N113G26E0012", "G26 edge schedule ends while its cycle SpillSlot transaction is still open");
        }
        if (edge_evidence.cycle_transactions_audited == 0U) {
            return fail("N113G26E0013", "G26 GP memory-expansion edge has no complete cycle SpillSlot save/restore transaction");
        }
        edge_evidence.ordered_cycle_protocol_valid = true;
        if (edge_evidence.cycle_transactions_audited > 1U) ++edges_with_multiple_cycles;
        result.total_operations_audited += edge_evidence.operations_audited;
        result.total_cycle_transactions_audited += edge_evidence.cycle_transactions_audited;
        result.edges.push_back(std::move(edge_evidence));
    }

    // The map and schedule counts were checked above; this explicit equality
    // makes omitted whole-edge schedules fail closed even if counters drift.
    for (const auto& item : g25_edges) {
        if (item.second != nullptr && item.second->gp_memory_to_memory_required &&
            scheduled_edges.find(item.first) == scheduled_edges.end()) {
            return fail("N113G26E0014", "G26 omitted an entire G25 GP memory-expansion EdgeId schedule");
        }
    }

    result.edges_audited = static_cast<std::uint32_t>(result.edges.size());
    result.distinct_xmm_registers_selected = static_cast<std::uint32_t>(selected_xmms.size());
    result.cycle_slot_reused_across_edges = result.edges_audited > 1U;
    result.cycle_slot_reused_across_transactions = edges_with_multiple_cycles > 0U;
    result.g25_edge_coverage_exact = true;
    result.ordered_cycle_protocol_valid = true;
    result.machine_insertion_ready = false;
    result.succeeded = true;
    add_diagnostic(result.diagnostics, "N113G26E0015",
        "G26 verifies edge-local ordered cycle SpillSlot save/restore transactions and shared-slot reuse only; active-frame reservation, Product Source evidence, and machine insertion remain separate prerequisites");
    return result;
}

NativeX64EdgeTransferInsertionResult insert_native_x64_edge_transfer_bundles(
    const std::vector<std::uint8_t>& code,
    const std::vector<NativeX64RelativeBranchFixup>& relative_branch_fixups,
    const std::vector<X64FlowBranchAnchor>& branch_anchors,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors,
    const std::vector<NativeX64EdgeTransferBundle>& bundles,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    NativeRegisterLocation gp_cycle_temporary,
    NativeRegisterLocation xmm_cycle_temporary,
    const NativeX64FrameLocationMap& frame_locations,
    const std::vector<NativeObjectRelocationInput>& external_relocations) {
    NativeX64EdgeTransferInsertionResult result;
    if (code.empty()) {
        add_diagnostic(result.diagnostics, "N109BI0001", "edge-transfer insertion received an empty function body");
        return result;
    }
    if (gp_cycle_temporary.kind != NativeRegisterLocationKind::Temporary ||
        xmm_cycle_temporary.kind != NativeRegisterLocationKind::Temporary) {
        add_diagnostic(result.diagnostics, "N109BI0002", "edge-transfer insertion requires distinct class-specific cycle temporaries");
        return result;
    }

    std::map<FlowEdgeId, const X64FlowBranchAnchor*> anchors_by_edge;
    for (const auto& anchor : branch_anchors) {
        if (anchor.edge_id == InvalidFlowEdgeId ||
            !anchors_by_edge.emplace(anchor.edge_id, &anchor).second) {
            add_diagnostic(result.diagnostics, "N109BI0003", "branch-anchor table contains an invalid or duplicate FlowEdgeId");
            return result;
        }
        if (anchor.function_code_size != 0U && anchor.function_code_size != code.size()) {
            add_diagnostic(result.diagnostics, "N109BI0004", "branch anchor was produced for a different function-code size");
            return result;
        }
        if (anchor.instruction_offset > code.size() || anchor.target_offset > code.size() ||
            (anchor.displacement_offset != InvalidNativeFlowOperationIndex &&
             static_cast<std::uint64_t>(anchor.displacement_offset) + 4U > code.size())) {
            add_diagnostic(result.diagnostics, "N109BI0005", "branch anchor instruction, displacement or logical target lies outside function code");
            return result;
        }
    }
    std::set<std::uint64_t> operation_ids;
    for (const auto& anchor : operation_anchors) {
        if (anchor.flow_operation_id == InvalidNativeFlowOperationId ||
            !operation_ids.insert(anchor.flow_operation_id).second ||
            anchor.machine_code_offset > code.size() ||
            (anchor.machine_code_before_offset != InvalidNativeFlowOperationIndex && anchor.machine_code_before_offset > code.size()) ||
            (anchor.machine_code_after_call_offset != InvalidNativeFlowOperationIndex && anchor.machine_code_after_call_offset > code.size()) ||
            (anchor.machine_code_call_instruction_offset != InvalidNativeFlowOperationIndex &&
             static_cast<std::uint64_t>(anchor.machine_code_call_instruction_offset) + 5U > code.size()) ||
            (anchor.function_code_size != 0U && anchor.function_code_size != code.size())) {
            add_diagnostic(result.diagnostics, "N109BI0018", "operation-anchor table is invalid, duplicated, or belongs to a different code body");
            return result;
        }
    }

    struct PendingBundle {
        FlowEdgeId edge_id = InvalidFlowEdgeId;
        const X64FlowBranchAnchor* anchor = nullptr;
        std::vector<std::uint8_t> bytes;
        std::uint32_t machine_moves = 0U;
        std::uint32_t redirect_offset = InvalidNativeFlowOperationIndex;
        std::uint32_t original_target = 0U;
        std::uint32_t island_offset = 0U;
        std::uint32_t encoded_displacement_offset = InvalidNativeFlowOperationIndex;
    };
    std::vector<PendingBundle> pending;
    std::set<FlowEdgeId> unique_bundles;
    std::set<std::uint32_t> inline_redirect_offsets;
    for (const auto& bundle : bundles) {
        if (bundle.edge_id == InvalidFlowEdgeId || !unique_bundles.insert(bundle.edge_id).second) {
            add_diagnostic(result.diagnostics, "N109BI0006", "edge-transfer bundle has an invalid or duplicate FlowEdgeId");
            return result;
        }
        if (bundle.operations.empty()) continue;
        const auto found = anchors_by_edge.find(bundle.edge_id);
        if (found == anchors_by_edge.end() || found->second == nullptr) {
            add_diagnostic(result.diagnostics, "N109BI0007", "non-empty edge-transfer bundle has no exact machine branch anchor");
            return result;
        }
        const auto& anchor = *found->second;
        std::vector<NativeRegisterParallelCopyOperation> gp_operations;
        std::vector<NativeRegisterParallelCopyOperation> xmm_operations;
        for (const auto& operation : bundle.operations) {
            const auto value_class = value_classes.find(operation.value_id);
            if (value_class == value_classes.end()) {
                add_diagnostic(result.diagnostics, "N109BI0008", "edge-transfer operation references a value with no register class");
                return result;
            }
            (value_class->second == NativeRegisterClass::GeneralPurpose ? gp_operations : xmm_operations)
                .push_back(operation);
        }
        PendingBundle item;
        item.edge_id = bundle.edge_id;
        item.anchor = &anchor;
        item.original_target = anchor.target_offset;
        if (!gp_operations.empty()) {
            const auto encoded = encode_native_x64_parallel_copy_schedule(
                gp_operations, value_classes, gp_cycle_temporary, frame_locations);
            if (!encoded.succeeded) {
                result.diagnostics.insert(result.diagnostics.end(), encoded.diagnostics.begin(), encoded.diagnostics.end());
                add_diagnostic(result.diagnostics, "N109BI0009", "GP edge-transfer bundle could not be encoded");
                return result;
            }
            item.machine_moves += encoded.machine_moves_emitted;
            item.bytes.insert(item.bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
        }
        if (!xmm_operations.empty()) {
            const auto encoded = encode_native_x64_parallel_copy_schedule(
                xmm_operations, value_classes, xmm_cycle_temporary, frame_locations);
            if (!encoded.succeeded) {
                result.diagnostics.insert(result.diagnostics.end(), encoded.diagnostics.begin(), encoded.diagnostics.end());
                add_diagnostic(result.diagnostics, "N109BI000A", "XMM edge-transfer bundle could not be encoded");
                return result;
            }
            item.machine_moves += encoded.machine_moves_emitted;
            item.bytes.insert(item.bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
        }
        if (item.bytes.empty()) continue;
        switch (anchor.encoding) {
        case X64FlowBranchEncoding::ConditionalRel32:
            if (anchor.displacement_offset == InvalidNativeFlowOperationIndex ||
                static_cast<std::uint64_t>(anchor.instruction_offset) + 6U > code.size() ||
                code[anchor.instruction_offset] != 0x0FU ||
                (code[anchor.instruction_offset + 1U] & 0xF0U) != 0x80U ||
                anchor.displacement_offset != anchor.instruction_offset + 2U) {
                add_diagnostic(result.diagnostics, "N109BI000B", "conditional edge anchor does not identify a canonical Jcc rel32 instruction");
                return result;
            }
            if (!anchor.conditional_taken) {
                item.redirect_offset = anchor.instruction_offset + 6U;
            }
            break;
        case X64FlowBranchEncoding::UnconditionalRel32:
            if (anchor.displacement_offset == InvalidNativeFlowOperationIndex ||
                static_cast<std::uint64_t>(anchor.instruction_offset) + 5U > code.size() ||
                code[anchor.instruction_offset] != 0xE9U ||
                anchor.displacement_offset != anchor.instruction_offset + 1U) {
                add_diagnostic(result.diagnostics, "N109BI000C", "unconditional edge anchor does not identify a canonical JMP rel32 instruction");
                return result;
            }
            break;
        case X64FlowBranchEncoding::Fallthrough:
            if (anchor.instruction_offset > code.size()) {
                add_diagnostic(result.diagnostics, "N109BI000D", "fallthrough edge redirect lies outside function code");
                return result;
            }
            item.redirect_offset = anchor.instruction_offset;
            break;
        }
        if (item.redirect_offset != InvalidNativeFlowOperationIndex &&
            !inline_redirect_offsets.insert(item.redirect_offset).second) {
            add_diagnostic(result.diagnostics, "N109BI000E", "multiple edge bundles require the same fallthrough redirect boundary");
            return result;
        }
        pending.push_back(std::move(item));
    }
    if (pending.empty()) {
        result.succeeded = true;
        result.code = code;
        result.relative_branch_fixups = relative_branch_fixups;
        result.branch_anchors = branch_anchors;
        result.operation_anchors = operation_anchors;
        return result;
    }
    std::sort(pending.begin(), pending.end(), [](const PendingBundle& left, const PendingBundle& right) {
        return left.edge_id < right.edge_id;
    });

    // Every rel32 branch in FunctionEmitter must be present, including
    // short-circuit-expression and epilogue branches that do not have a CFG
    // FlowEdge anchor.
    std::set<std::uint32_t> fixup_offsets;
    for (const auto& fixup : relative_branch_fixups) {
        if (static_cast<std::uint64_t>(fixup.displacement_offset) + 4U > code.size() ||
            fixup.target_offset > code.size() || !fixup_offsets.insert(fixup.displacement_offset).second) {
            add_diagnostic(result.diagnostics, "N109BI000F", "relative-branch fixup table is invalid or incomplete");
            return result;
        }
    }
    for (const auto& item : pending) {
        if (item.anchor->encoding == X64FlowBranchEncoding::Fallthrough) continue;
        const auto fixup = std::find_if(relative_branch_fixups.begin(), relative_branch_fixups.end(),
            [&](const NativeX64RelativeBranchFixup& candidate) {
                return candidate.displacement_offset == item.anchor->displacement_offset;
            });
        if (fixup == relative_branch_fixups.end() || fixup->target_offset != item.anchor->target_offset) {
            add_diagnostic(result.diagnostics, "N109BI0010", "anchored taken edge is missing or disagrees with the complete rel32 fixup table");
            return result;
        }
    }

    std::uint64_t total_inserted_bytes = static_cast<std::uint64_t>(inline_redirect_offsets.size()) * 5U;
    for (const auto& item : pending) total_inserted_bytes += item.bytes.size() + 5U;
    if (static_cast<std::uint64_t>(code.size()) + total_inserted_bytes >
        std::numeric_limits<std::uint32_t>::max()) {
        add_diagnostic(result.diagnostics, "N109BI0019", "edge-transfer expansion exceeds the 32-bit function-offset format");
        return result;
    }

    for (const auto offset : inline_redirect_offsets) {
        result.offset_adjustments.push_back(NativeX64CodeInsertionAdjustment{offset, 5U});
    }
    std::sort(result.offset_adjustments.begin(), result.offset_adjustments.end(),
        [](const auto& left, const auto& right) { return left.original_offset < right.original_offset; });

    result.code.reserve(code.size() + inline_redirect_offsets.size() * 5U + pending.size() * 16U);
    std::size_t cursor = 0U;
    std::map<std::uint32_t, std::uint32_t> redirect_disp_by_original_offset;
    for (const auto& adjustment : result.offset_adjustments) {
        if (adjustment.original_offset < cursor || adjustment.original_offset > code.size()) {
            add_diagnostic(result.diagnostics, "N109BI0011", "fallthrough redirect insertions are not in a valid original-code order");
            return result;
        }
        result.code.insert(result.code.end(), code.begin() + static_cast<std::ptrdiff_t>(cursor),
                           code.begin() + static_cast<std::ptrdiff_t>(adjustment.original_offset));
        const auto redirect_start = static_cast<std::uint32_t>(result.code.size());
        result.code.push_back(0xE9U);
        const auto displacement = static_cast<std::uint32_t>(result.code.size());
        result.code.insert(result.code.end(), 4U, 0U);
        redirect_disp_by_original_offset.emplace(adjustment.original_offset, displacement);
        (void)redirect_start;
        cursor = adjustment.original_offset;
    }
    result.code.insert(result.code.end(), code.begin() + static_cast<std::ptrdiff_t>(cursor), code.end());

    result.relative_branch_fixups.reserve(relative_branch_fixups.size() + inline_redirect_offsets.size() + pending.size());
    for (const auto& fixup : relative_branch_fixups) {
        result.relative_branch_fixups.push_back(NativeX64RelativeBranchFixup{
            map_offset_after_insertions(fixup.displacement_offset, result.offset_adjustments),
            map_offset_after_insertions(fixup.target_offset, result.offset_adjustments)});
    }

    // Append one isolated transfer island per logical CFG edge. Each island
    // executes the resolved copy sequence, then jumps to the original edge
    // destination in the rebased function body.
    for (auto& item : pending) {
        if (result.code.size() > std::numeric_limits<std::uint32_t>::max() ||
            item.bytes.size() > std::numeric_limits<std::uint32_t>::max() - result.code.size()) {
            add_diagnostic(result.diagnostics, "N109BI0012", "edge-transfer islands exceed the 32-bit machine-offset format");
            return result;
        }
        item.island_offset = static_cast<std::uint32_t>(result.code.size());
        result.code.insert(result.code.end(), item.bytes.begin(), item.bytes.end());
        result.code.push_back(0xE9U);
        const auto island_displacement = static_cast<std::uint32_t>(result.code.size());
        result.code.insert(result.code.end(), 4U, 0U);
        const auto logical_target = map_offset_after_insertions(item.original_target, result.offset_adjustments);
        result.relative_branch_fixups.push_back(NativeX64RelativeBranchFixup{island_displacement, logical_target});
        item.encoded_displacement_offset = island_displacement;
    }

    // Each inline JMP is a new internal branch and must be included in the
    // same fixup table as the original branches and island return jumps.
    for (const auto& item : pending) {
        if (item.redirect_offset == InvalidNativeFlowOperationIndex) continue;
        const auto redirect = redirect_disp_by_original_offset.find(item.redirect_offset);
        if (redirect == redirect_disp_by_original_offset.end()) {
            add_diagnostic(result.diagnostics, "N109BI0017", "inline edge redirect is missing its displacement field");
            return result;
        }
        result.relative_branch_fixups.push_back(
            NativeX64RelativeBranchFixup{redirect->second, item.island_offset});
    }

    // Rebase all existing branch and operation anchors. Existing explicit
    // branches that target an inline redirect boundary intentionally land
    // after the inserted JMP; only the selected fallthrough path executes it.
    for (const auto& original : branch_anchors) {
        auto anchor = original;
        anchor.instruction_offset = map_offset_after_insertions(anchor.instruction_offset, result.offset_adjustments);
        if (anchor.displacement_offset != InvalidNativeFlowOperationIndex)
            anchor.displacement_offset = map_offset_after_insertions(anchor.displacement_offset, result.offset_adjustments);
        anchor.target_offset = map_offset_after_insertions(anchor.target_offset, result.offset_adjustments);
        anchor.function_code_size = static_cast<std::uint32_t>(result.code.size());
        result.branch_anchors.push_back(std::move(anchor));
    }
    for (const auto& original : operation_anchors) {
        auto anchor = original;
        anchor.machine_code_offset = map_offset_after_insertions(anchor.machine_code_offset, result.offset_adjustments);
        if (anchor.machine_code_before_offset != InvalidNativeFlowOperationIndex)
            anchor.machine_code_before_offset = map_offset_after_insertions(anchor.machine_code_before_offset, result.offset_adjustments);
        if (anchor.machine_code_after_call_offset != InvalidNativeFlowOperationIndex)
            anchor.machine_code_after_call_offset = map_offset_after_insertions(anchor.machine_code_after_call_offset, result.offset_adjustments);
        if (anchor.machine_code_call_instruction_offset != InvalidNativeFlowOperationIndex)
            anchor.machine_code_call_instruction_offset = map_offset_after_insertions(
                anchor.machine_code_call_instruction_offset, result.offset_adjustments);
        anchor.function_code_size = static_cast<std::uint32_t>(result.code.size());
        result.operation_anchors.push_back(std::move(anchor));
    }

    // Retarget exact taken branches to their islands. Fallthrough edges use
    // the inline redirect JMP; their original logical instruction/edge anchor
    // remains intact and the physical redirect is recorded in transfer_*.
    for (auto& item : pending) {
        auto anchor_it = std::find_if(result.branch_anchors.begin(), result.branch_anchors.end(),
            [&](const X64FlowBranchAnchor& candidate) { return candidate.edge_id == item.edge_id; });
        if (anchor_it == result.branch_anchors.end()) {
            add_diagnostic(result.diagnostics, "N109BI0013", "selected edge anchor disappeared during code insertion");
            return result;
        }
        if (item.redirect_offset != InvalidNativeFlowOperationIndex) {
            const auto redirect = redirect_disp_by_original_offset.find(item.redirect_offset);
            if (redirect == redirect_disp_by_original_offset.end()) {
                add_diagnostic(result.diagnostics, "N109BI0014", "fallthrough edge has no emitted inline redirect");
                return result;
            }
            anchor_it->transfer_instruction_offset = map_offset_before_insertions(
                item.redirect_offset, result.offset_adjustments);
            anchor_it->transfer_displacement_offset = redirect->second;
        } else {
            const auto mapped_displacement = map_offset_after_insertions(
                item.anchor->displacement_offset, result.offset_adjustments);
            const auto found = std::find_if(result.relative_branch_fixups.begin(), result.relative_branch_fixups.end(),
                [mapped_displacement](const NativeX64RelativeBranchFixup& fixup) {
                    return fixup.displacement_offset == mapped_displacement;
                });
            if (found == result.relative_branch_fixups.end()) {
                add_diagnostic(result.diagnostics, "N109BI0015", "taken edge branch fixup disappeared during code insertion");
                return result;
            }
            found->target_offset = item.island_offset;
            anchor_it->transfer_instruction_offset = anchor_it->instruction_offset;
            anchor_it->transfer_displacement_offset = anchor_it->displacement_offset;
        }
        anchor_it->transfer_target_offset = item.island_offset;
        anchor_it->transfer_byte_count = static_cast<std::uint32_t>(item.bytes.size());
        ++result.edge_bundles_inserted;
        result.machine_moves_inserted += item.machine_moves;
        ++result.transfer_islands_emitted;
    }

    std::set<std::size_t> unique_relocation_offsets;
    for (const auto& relocation : external_relocations) {
        if (relocation.width == 0U || relocation.offset > code.size() ||
            relocation.width > code.size() - relocation.offset ||
            !unique_relocation_offsets.insert(relocation.offset).second) {
            add_diagnostic(result.diagnostics, "N109BI001A", "edge insertion received an invalid, overlapping-by-offset, or duplicate external COFF relocation");
            return result;
        }
        if (relocation.offset > std::numeric_limits<std::uint32_t>::max()) {
            add_diagnostic(result.diagnostics, "N109BI001A", "external COFF relocation offset exceeds the 32-bit function-offset format");
            return result;
        }
        auto rebased = relocation;
        rebased.offset = map_offset_after_insertions(
            static_cast<std::uint32_t>(relocation.offset), result.offset_adjustments);
        if (rebased.offset > result.code.size() || rebased.width > result.code.size() - rebased.offset) {
            add_diagnostic(result.diagnostics, "N109BI001A", "rebased external COFF relocation lies outside transformed function code");
            return result;
        }
        result.external_relocations.push_back(std::move(rebased));
    }

    for (const auto& fixup : result.relative_branch_fixups) {
        if (!patch_relative32(result.code, fixup.displacement_offset, fixup.target_offset,
                              result.diagnostics, "N109BI0016")) return result;
    }
    result.succeeded = true;
    return result;
}

NativeX64SpillReloadInsertionResult insert_native_x64_spill_reload_actions(
    const std::vector<std::uint8_t>& code,
    const std::vector<NativeX64RelativeBranchFixup>& relative_branch_fixups,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors,
    const std::vector<X64FlowBranchAnchor>& branch_anchors,
    const std::vector<NativeBoundSpillReloadAction>& bindings,
    const std::map<std::uint32_t, NativeRegisterClass>& value_classes,
    NativeRegisterLocation gp_cycle_temporary,
    NativeRegisterLocation xmm_cycle_temporary,
    const NativeX64FrameLocationMap& frame_locations,
    const std::vector<NativeObjectRelocationInput>& external_relocations) {
    NativeX64SpillReloadInsertionResult result;
    if (code.empty() && !bindings.empty()) {
        add_diagnostic(result.diagnostics, "N110BI0001", "spill/reload insertion received an empty function body");
        return result;
    }
    if (gp_cycle_temporary.kind != NativeRegisterLocationKind::Temporary ||
        xmm_cycle_temporary.kind != NativeRegisterLocationKind::Temporary) {
        add_diagnostic(result.diagnostics, "N110BI0002", "spill/reload insertion requires caller-reserved class-specific scratch registers");
        return result;
    }

    std::map<std::uint64_t, const X64FlowEmitterAnchor*> anchors_by_operation;
    for (const auto& anchor : operation_anchors) {
        if (anchor.flow_operation_id == InvalidNativeFlowOperationId ||
            !anchors_by_operation.emplace(anchor.flow_operation_id, &anchor).second) {
            add_diagnostic(result.diagnostics, "N110BI0003", "operation-anchor table contains an invalid or duplicate Flow operation ID");
            return result;
        }
        if ((anchor.function_code_size != 0U && anchor.function_code_size != code.size()) ||
            anchor.machine_code_offset > code.size() ||
            (anchor.machine_code_before_offset != InvalidNativeFlowOperationIndex && anchor.machine_code_before_offset > code.size()) ||
            (anchor.machine_code_after_call_offset != InvalidNativeFlowOperationIndex && anchor.machine_code_after_call_offset > code.size()) ||
            (anchor.machine_code_call_instruction_offset != InvalidNativeFlowOperationIndex &&
             static_cast<std::uint64_t>(anchor.machine_code_call_instruction_offset) + 5U > code.size())) {
            add_diagnostic(result.diagnostics, "N110BI0004", "operation anchor has an invalid offset or was produced for a different function-code size");
            return result;
        }
    }

    std::set<FlowEdgeId> unique_branch_edges;
    for (const auto& anchor : branch_anchors) {
        if (anchor.edge_id == InvalidFlowEdgeId || !unique_branch_edges.insert(anchor.edge_id).second) {
            add_diagnostic(result.diagnostics, "N110BI0011", "branch-anchor table contains an invalid or duplicate FlowEdgeId");
            return result;
        }
        if ((anchor.function_code_size != 0U && anchor.function_code_size != code.size()) ||
            anchor.instruction_offset > code.size() || anchor.target_offset > code.size() ||
            (anchor.displacement_offset != InvalidNativeFlowOperationIndex &&
             static_cast<std::uint64_t>(anchor.displacement_offset) + 4U > code.size())) {
            add_diagnostic(result.diagnostics, "N110BI0012", "branch anchor has invalid offsets or was produced for a different function-code size");
            return result;
        }
    }

    struct PendingAction {
        std::uint64_t operation_id = InvalidNativeFlowOperationId;
        NativeSpillReloadActionKind kind = NativeSpillReloadActionKind::StoreBeforeCall;
        std::uint32_t value_id = 0U;
        std::uint32_t offset = 0U;
        std::vector<std::uint8_t> bytes;
        std::uint32_t move_count = 0U;
    };
    std::vector<PendingAction> pending;
    std::set<std::tuple<std::uint64_t, unsigned int, std::uint32_t>> unique_actions;
    for (const auto& binding : bindings) {
        const auto anchor_it = anchors_by_operation.find(binding.flow_operation_id);
        if (anchor_it == anchors_by_operation.end() || anchor_it->second == nullptr) {
            add_diagnostic(result.diagnostics, "N110BI0005", "bound spill/reload action has no exact N108 operation anchor");
            return result;
        }
        const auto& anchor = *anchor_it->second;
        const auto value_class = value_classes.find(binding.action.value_id);
        if (value_class == value_classes.end() || value_class->second != binding.register_class) {
            add_diagnostic(result.diagnostics, "N110BI0006", "bound spill/reload action register class is missing or inconsistent");
            return result;
        }
        const auto key = std::make_tuple(binding.flow_operation_id,
            static_cast<unsigned int>(binding.action.kind), binding.action.value_id);
        if (!unique_actions.insert(key).second) {
            add_diagnostic(result.diagnostics, "N110BI0007", "duplicate bound spill/reload insertion action");
            return result;
        }
        std::uint32_t insertion_offset = InvalidNativeFlowOperationIndex;
        switch (binding.action.kind) {
        case NativeSpillReloadActionKind::StoreBeforeCall:
            insertion_offset = anchor.machine_code_before_offset;
            break;
        case NativeSpillReloadActionKind::ReloadAfterCall:
            insertion_offset = anchor.machine_code_after_call_offset;
            break;
        case NativeSpillReloadActionKind::StoreAfterDefinition:
            insertion_offset = anchor.machine_code_offset;
            break;
        case NativeSpillReloadActionKind::ReloadBeforeUse:
            insertion_offset = anchor.machine_code_before_offset;
            break;
        }
        if (insertion_offset == InvalidNativeFlowOperationIndex || insertion_offset > code.size()) {
            add_diagnostic(result.diagnostics, "N110BI0008", "bound action has no valid before/call-after/operation insertion boundary");
            return result;
        }
        NativeX64ParallelCopyEncodingResult encoded;
        NativeRegisterLocation action_gp_cycle_temporary = gp_cycle_temporary;
        NativeRegisterLocation action_xmm_cycle_temporary = xmm_cycle_temporary;
        if (binding.cycle_temporary_assigned) {
            if (binding.cycle_temporary_register_class == NativeRegisterClass::GeneralPurpose)
                action_gp_cycle_temporary = binding.cycle_temporary;
            else
                action_xmm_cycle_temporary = binding.cycle_temporary;
        }
        if (!encode_single_transfer(binding, value_classes, action_gp_cycle_temporary,
                                     action_xmm_cycle_temporary, frame_locations, encoded)) {
            result.diagnostics.insert(result.diagnostics.end(), encoded.diagnostics.begin(), encoded.diagnostics.end());
            add_diagnostic(result.diagnostics, "N110BI0009", "bound spill/reload action could not be encoded as a machine transfer");
            return result;
        }
        if (!encoded.bytes.empty()) {
            pending.push_back(PendingAction{binding.flow_operation_id, binding.action.kind,
                binding.action.value_id, insertion_offset, std::move(encoded.bytes), encoded.machine_moves_emitted});
        }
    }

    // An operation may have multiple call-crossing values saved/reloaded at
    // the same boundary; preserve stable binding order when concatenating them.
    std::stable_sort(pending.begin(), pending.end(), [](const PendingAction& left, const PendingAction& right) {
        return std::tie(left.offset, left.kind, left.value_id, left.operation_id) <
               std::tie(right.offset, right.kind, right.value_id, right.operation_id);
    });
    std::map<std::uint32_t, std::vector<std::uint8_t>> bytes_by_offset;
    for (const auto& item : pending) {
        auto& bytes = bytes_by_offset[item.offset];
        bytes.insert(bytes.end(), item.bytes.begin(), item.bytes.end());
        result.machine_actions_inserted++;
        result.machine_moves_inserted += item.move_count;
    }
    for (const auto& item : bytes_by_offset) {
        if (item.first > code.size() || item.second.size() > std::numeric_limits<std::uint32_t>::max()) {
            add_diagnostic(result.diagnostics, "N110BI000A", "spill/reload code insertion exceeds the function offset format");
            return result;
        }
        result.offset_adjustments.push_back(NativeX64CodeInsertionAdjustment{
            item.first, static_cast<std::uint32_t>(item.second.size())});
    }

    std::uint64_t total_inserted_bytes = 0U;
    for (const auto& item : result.offset_adjustments) total_inserted_bytes += item.inserted_bytes;
    if (static_cast<std::uint64_t>(code.size()) + total_inserted_bytes >
        std::numeric_limits<std::uint32_t>::max()) {
        add_diagnostic(result.diagnostics, "N110BI0014", "spill/reload expansion exceeds the 32-bit function-offset format");
        return result;
    }
    result.code.reserve(code.size() + static_cast<std::size_t>(total_inserted_bytes));
    std::size_t cursor = 0U;
    for (const auto& item : bytes_by_offset) {
        result.code.insert(result.code.end(), code.begin() + static_cast<std::ptrdiff_t>(cursor),
                           code.begin() + static_cast<std::ptrdiff_t>(item.first));
        result.code.insert(result.code.end(), item.second.begin(), item.second.end());
        cursor = item.first;
    }
    result.code.insert(result.code.end(), code.begin() + static_cast<std::ptrdiff_t>(cursor), code.end());

    std::set<std::size_t> unique_relocation_offsets;
    for (const auto& relocation : external_relocations) {
        if (relocation.width == 0U || relocation.offset > code.size() ||
            relocation.width > code.size() - relocation.offset ||
            !unique_relocation_offsets.insert(relocation.offset).second) {
            add_diagnostic(result.diagnostics, "N110BI000D", "spill/reload insertion received an invalid, overlapping-by-offset, or duplicate external COFF relocation");
            return result;
        }
        if (relocation.offset > std::numeric_limits<std::uint32_t>::max()) {
            add_diagnostic(result.diagnostics, "N110BI000D", "external COFF relocation offset exceeds the 32-bit function-offset format");
            return result;
        }
        auto rebased = relocation;
        rebased.offset = map_offset_after_insertions(
            static_cast<std::uint32_t>(relocation.offset), result.offset_adjustments);
        if (rebased.offset > result.code.size() || rebased.width > result.code.size() - rebased.offset) {
            add_diagnostic(result.diagnostics, "N110BI000D", "rebased external COFF relocation lies outside transformed function code");
            return result;
        }
        result.external_relocations.push_back(std::move(rebased));
    }

    std::set<std::uint32_t> unique_fixup_offsets;
    for (const auto& fixup : relative_branch_fixups) {
        if (static_cast<std::uint64_t>(fixup.displacement_offset) + 4U > code.size() ||
            fixup.target_offset > code.size() || !unique_fixup_offsets.insert(fixup.displacement_offset).second) {
            add_diagnostic(result.diagnostics, "N110BI000B", "spill/reload insertion received an invalid or duplicate rel32 fixup");
            return result;
        }
        result.relative_branch_fixups.push_back(NativeX64RelativeBranchFixup{
            map_offset_after_insertions(fixup.displacement_offset, result.offset_adjustments),
            map_offset_before_insertions(fixup.target_offset, result.offset_adjustments)});
    }
    for (const auto& anchor : branch_anchors) {
        // A conditional CFG pair shares one Jcc rel32 field: only the taken
        // edge targets that displacement. The not-taken edge is the machine
        // fallthrough path even when the anchor records the Jcc encoding.
        if (anchor.encoding == X64FlowBranchEncoding::Fallthrough ||
            (anchor.encoding == X64FlowBranchEncoding::ConditionalRel32 && !anchor.conditional_taken))
            continue;
        const auto fixup = std::find_if(relative_branch_fixups.begin(), relative_branch_fixups.end(),
            [&](const NativeX64RelativeBranchFixup& candidate) {
                return candidate.displacement_offset == anchor.displacement_offset;
            });
        if (fixup == relative_branch_fixups.end() || fixup->target_offset != anchor.target_offset) {
            const std::string message = "CFG branch anchor disagrees with rel32 fixup table (disp=" +
                std::to_string(anchor.displacement_offset) + ", anchor_target=" +
                std::to_string(anchor.target_offset) + ", fixup_target=" +
                (fixup == relative_branch_fixups.end() ? std::string("missing") :
                    std::to_string(fixup->target_offset)) + ")";
            add_diagnostic(result.diagnostics, "N110BI0013", message.c_str());
            return result;
        }
    }
    for (const auto& fixup : result.relative_branch_fixups) {
        if (!patch_relative32(result.code, fixup.displacement_offset, fixup.target_offset,
                              result.diagnostics, "N110BI000C")) return result;
    }

    for (const auto& original : operation_anchors) {
        auto anchor = original;
        anchor.machine_code_before_offset = original.machine_code_before_offset == InvalidNativeFlowOperationIndex
            ? InvalidNativeFlowOperationIndex
            : map_offset_before_insertions(original.machine_code_before_offset, result.offset_adjustments);
        anchor.machine_code_offset = map_offset_after_insertions(original.machine_code_offset, result.offset_adjustments);
        anchor.machine_code_after_call_offset = original.machine_code_after_call_offset == InvalidNativeFlowOperationIndex
            ? InvalidNativeFlowOperationIndex
            : map_offset_after_insertions(original.machine_code_after_call_offset, result.offset_adjustments);
        anchor.machine_code_call_instruction_offset = original.machine_code_call_instruction_offset == InvalidNativeFlowOperationIndex
            ? InvalidNativeFlowOperationIndex
            : map_offset_after_insertions(original.machine_code_call_instruction_offset, result.offset_adjustments);
        anchor.function_code_size = static_cast<std::uint32_t>(result.code.size());
        result.operation_anchors.push_back(std::move(anchor));
    }
    for (const auto& original : branch_anchors) {
        auto anchor = original;
        anchor.instruction_offset = map_offset_after_insertions(original.instruction_offset, result.offset_adjustments);
        if (anchor.displacement_offset != InvalidNativeFlowOperationIndex)
            anchor.displacement_offset = map_offset_after_insertions(original.displacement_offset, result.offset_adjustments);
        // A branch entering an operation boundary must execute any inserted
        // reload/store action at that boundary, so logical targets map before.
        anchor.target_offset = map_offset_before_insertions(original.target_offset, result.offset_adjustments);
        if (anchor.transfer_instruction_offset != InvalidNativeFlowOperationIndex)
            anchor.transfer_instruction_offset = map_offset_after_insertions(original.transfer_instruction_offset, result.offset_adjustments);
        if (anchor.transfer_displacement_offset != InvalidNativeFlowOperationIndex)
            anchor.transfer_displacement_offset = map_offset_after_insertions(original.transfer_displacement_offset, result.offset_adjustments);
        if (anchor.transfer_target_offset != InvalidNativeFlowOperationIndex)
            anchor.transfer_target_offset = map_offset_before_insertions(original.transfer_target_offset, result.offset_adjustments);
        anchor.function_code_size = static_cast<std::uint32_t>(result.code.size());
        result.branch_anchors.push_back(std::move(anchor));
    }
    result.succeeded = true;
    return result;
}

NativeX64MachineLocationPlan build_native_x64_machine_location_plan(
    const NativeRegisterFlowFunction& function,
    const NativeRegisterLivenessResult& liveness,
    const NativeRegisterAllocationResult& persistent_allocation,
    const std::set<std::uint32_t>& address_taken_value_ids,
    const std::vector<std::uint8_t>& reserved_expression_gp,
    const std::vector<std::uint8_t>& reserved_expression_xmm) {
    NativeX64MachineLocationPlan result;
    const auto validated = validate_native_register_function_ir(function.register_ir);
    if (!validated.succeeded || !liveness.succeeded || !liveness.converged ||
        liveness.blocks.size() != validated.normalized_function.blocks.size() ||
        !persistent_allocation.succeeded) {
        add_diagnostic(result.diagnostics, "N111ME0001",
            "machine-location reconciliation received incomplete Flow, liveness, or allocation data");
        return result;
    }

    std::map<std::uint32_t, NativeRegisterClass> value_classes;
    for (const auto& value : validated.normalized_function.values) {
        if (!value_classes.emplace(value.value_id, value.register_class).second) {
            add_diagnostic(result.diagnostics, "N111ME0002", "Flow IR contains duplicate virtual value IDs");
            return result;
        }
    }

    std::set<std::uint32_t> binding_ids;
    for (const auto& binding : function.bindings) {
        if (binding.native_value_id == 0U ||
            value_classes.find(binding.native_value_id) == value_classes.end() ||
            !binding_ids.insert(binding.native_value_id).second) {
            add_diagnostic(result.diagnostics, "N111ME0003",
                "Flow binding has an unknown, invalid, or duplicate native value ID");
            return result;
        }
    }
    for (const auto value_id : address_taken_value_ids) {
        if (binding_ids.find(value_id) == binding_ids.end()) {
            add_diagnostic(result.diagnostics, "N111ME0004",
                "address-taken set references a value that is not a source binding");
            return result;
        }
    }

    const std::set<std::uint8_t> nonvolatile_gp{3U, 6U, 7U, 12U, 13U, 14U, 15U};
    const auto reserved_gp = std::set<std::uint8_t>(reserved_expression_gp.begin(), reserved_expression_gp.end());
    const auto reserved_xmm = std::set<std::uint8_t>(reserved_expression_xmm.begin(), reserved_expression_xmm.end());
    std::map<std::uint32_t, NativeRegisterAssignment> assignments;
    for (const auto& assignment : persistent_allocation.assignments) {
        if (binding_ids.find(assignment.value_id) == binding_ids.end() ||
            !assignments.emplace(assignment.value_id, assignment).second) {
            add_diagnostic(result.diagnostics, "N111ME0005",
                "persistent allocation contains an unknown or duplicate binding assignment");
            return result;
        }
        const auto value_class = value_classes.find(assignment.value_id);
        if (value_class == value_classes.end() || value_class->second != assignment.register_class) {
            add_diagnostic(result.diagnostics, "N111ME0006",
                "persistent allocation register class does not match its Flow value");
            return result;
        }
        if (assignment.spilled) continue; // The emitter's canonical home is the safe fallback.

        if (!assignment.requires_callee_save) {
            add_diagnostic(result.diagnostics, "N111ME0007",
                "persistent source binding was assigned a volatile register without a call-safe materialization plan");
            return result;
        }
        if (assignment.register_class == NativeRegisterClass::GeneralPurpose) {
            if (nonvolatile_gp.find(assignment.register_index) == nonvolatile_gp.end()) {
                add_diagnostic(result.diagnostics, "N111ME0008",
                    "persistent source binding uses a non-Win64 or frame-reserved GP register");
                return result;
            }
            if (reserved_gp.find(assignment.register_index) != reserved_gp.end()) {
                add_diagnostic(result.diagnostics, "N111ME0009",
                    "persistent GP register aliases an expression-temporary callee-save register");
                return result;
            }
        } else {
            if (assignment.register_index < 6U || assignment.register_index > 15U) {
                add_diagnostic(result.diagnostics, "N111ME000A",
                    "persistent XMM binding must use a Win64 nonvolatile XMM register");
                return result;
            }
            if (reserved_xmm.find(assignment.register_index) != reserved_xmm.end()) {
                add_diagnostic(result.diagnostics, "N111ME000B",
                    "persistent XMM register aliases an expression-temporary callee-save register");
                return result;
            }
        }
        if (address_taken_value_ids.find(assignment.value_id) != address_taken_value_ids.end()) {
            add_diagnostic(result.diagnostics, "N111ME000C",
                "address-taken source binding cannot be register-resident");
            return result;
        }
    }

    const auto interference = build_native_register_interference_graph(
        validated.normalized_function, liveness);
    if (!interference.succeeded) {
        add_diagnostic(result.diagnostics, "N111ME000D",
            "machine-location reconciliation could not validate CFG interference");
        return result;
    }

    for (const auto value_id : binding_ids) {
        const auto value_class = value_classes.find(value_id);
        NativeX64MachineValueLocation value;
        value.value_id = value_id;
        value.register_class = value_class->second;
        value.address_taken = address_taken_value_ids.find(value_id) != address_taken_value_ids.end();
        value.canonical_home.kind = NativeRegisterLocationKind::HomeSlot;
        value.canonical_home.spill_slot = InvalidNativeX64HomeSlot;
        value.active_location = value.canonical_home;

        const auto assignment = assignments.find(value_id);
        if (!value.address_taken && assignment != assignments.end() && !assignment->second.spilled) {
            value.active_location.kind = NativeRegisterLocationKind::Register;
            value.active_location.register_index = assignment->second.register_index;
            value.active_location.spill_slot = 0U;
            if (value.register_class == NativeRegisterClass::GeneralPurpose) {
                result.used_callee_saved_gp.push_back(assignment->second.register_index);
            } else {
                result.used_callee_saved_xmm.push_back(assignment->second.register_index);
            }
            ++result.register_resident_values;
        } else {
            ++result.home_resident_values;
        }
        if (value.address_taken) ++result.address_taken_values;
        result.values.emplace(value_id, value);
    }

    // Two simultaneously live source values may not share the same physical
    // register. Reuse for non-interfering values remains legal and deterministic.
    for (const auto& edge : interference.edges) {
        const auto first = result.values.find(edge.first_value_id);
        const auto second = result.values.find(edge.second_value_id);
        if (first == result.values.end() || second == result.values.end()) continue;
        const auto& left = first->second;
        const auto& right = second->second;
        if (left.active_location.kind == NativeRegisterLocationKind::Register &&
            right.active_location.kind == NativeRegisterLocationKind::Register &&
            left.register_class == right.register_class &&
            left.active_location.register_index == right.active_location.register_index) {
            add_diagnostic(result.diagnostics, "N111ME000E",
                "interfering Flow bindings share one active physical register");
            return result;
        }
    }

    auto normalize_registers = [](std::vector<std::uint8_t>& registers) {
        std::sort(registers.begin(), registers.end());
        registers.erase(std::unique(registers.begin(), registers.end()), registers.end());
    };
    normalize_registers(result.used_callee_saved_gp);
    normalize_registers(result.used_callee_saved_xmm);
    result.succeeded = true;
    return result;
}

bool bind_native_x64_machine_location_home(
    NativeX64MachineLocationPlan& plan,
    std::uint32_t value_id,
    std::uint32_t home_slot) {
    const auto fail = [&plan](const char* code, const char* message) {
        add_diagnostic(plan.diagnostics, code, message);
        plan.succeeded = false;
        return false;
    };
    if (!plan.succeeded) {
        return fail("N111ME0010", "cannot bind a canonical home on an unsuccessful Machine Location plan");
    }
    if (home_slot == InvalidNativeX64HomeSlot) {
        return fail("N111ME0011", "canonical home binding uses the reserved invalid HomeSlot sentinel");
    }
    const auto found = plan.values.find(value_id);
    if (found == plan.values.end()) {
        return fail("N111ME0012", "canonical home binding references an unknown Flow value");
    }
    auto& value = found->second;
    if (value.canonical_home.kind != NativeRegisterLocationKind::HomeSlot) {
        return fail("N111ME0013", "canonical home binding encountered a non-HomeSlot location kind");
    }
    if (value.canonical_home.spill_slot != InvalidNativeX64HomeSlot) {
        if (value.canonical_home.spill_slot == home_slot) return true;
        return fail("N111ME0014", "Flow value was rebound to a different canonical home slot");
    }
    value.canonical_home.spill_slot = home_slot;
    if (value.active_location.kind == NativeRegisterLocationKind::HomeSlot) {
        value.active_location = value.canonical_home;
    } else if (value.active_location.kind != NativeRegisterLocationKind::Register) {
        return fail("N111ME0015", "active Machine Location must be a register or canonical HomeSlot");
    }
    return true;
}

NativeSpillReloadBindingResult bind_native_spill_reload_actions_to_flow_operations(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterAllocationPlan& allocation) {
    NativeSpillReloadBindingResult result;
    const auto validated = validate_native_register_function_ir(function);
    if (!validated.succeeded) {
        result.diagnostics = validated.diagnostics;
        return result;
    }

    std::map<std::uint32_t, NativeRegisterClass> value_classes;
    for (const auto& value : validated.normalized_function.values) {
        if (!value_classes.emplace(value.value_id, value.register_class).second) {
            add_diagnostic(result.diagnostics, "N110AE0001", "spill/reload binding received duplicate virtual value IDs");
            return result;
        }
    }
    std::set<ActionKey> unique_actions;
    for (const auto& action : allocation.actions) {
        if (action.block_id >= validated.normalized_function.blocks.size()) {
            add_diagnostic(result.diagnostics, "N110AE0002", "spill/reload action references an unknown CFG block");
            return result;
        }
        const auto& block = validated.normalized_function.blocks[action.block_id];
        if (action.instruction_index >= block.instructions.size()) {
            add_diagnostic(result.diagnostics, "N110AE0003", "spill/reload action references an unknown instruction index");
            return result;
        }
        const auto& instruction = block.instructions[action.instruction_index];
        const auto value = value_classes.find(action.value_id);
        if (value == value_classes.end()) {
            add_diagnostic(result.diagnostics, "N110AE0004", "spill/reload action references an undeclared virtual value");
            return result;
        }
        if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic ||
            instruction.flow_operation_id == InvalidNativeFlowOperationId ||
            instruction.flow_block_id != action.block_id ||
            instruction.flow_operation_index == InvalidNativeFlowOperationIndex) {
            add_diagnostic(result.diagnostics, "N110AE0005", "spill/reload action has no stable N108 Flow-operation anchor");
            return result;
        }

        NativeSpillReloadPlacement placement = NativeSpillReloadPlacement::BeforeOperation;
        bool compatible = false;
        switch (action.kind) {
        case NativeSpillReloadActionKind::StoreBeforeCall:
            compatible = instruction.is_call && instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowCall &&
                action.source.kind == NativeRegisterLocationKind::Register &&
                action.destination.kind == NativeRegisterLocationKind::HomeSlot &&
                action.destination.spill_slot == action.slot_index;
            placement = NativeSpillReloadPlacement::BeforeOperation;
            break;
        case NativeSpillReloadActionKind::ReloadAfterCall:
            compatible = instruction.is_call && instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowCall &&
                action.source.kind == NativeRegisterLocationKind::HomeSlot &&
                action.source.spill_slot == action.slot_index &&
                action.destination.kind == NativeRegisterLocationKind::Register;
            placement = NativeSpillReloadPlacement::AfterOperation;
            break;
        case NativeSpillReloadActionKind::StoreAfterDefinition:
            compatible = !instruction.is_call &&
                (instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowWrite ||
                 instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowDeclare) &&
                std::find(instruction.definitions.begin(), instruction.definitions.end(), action.value_id) != instruction.definitions.end() &&
                action.source.kind == NativeRegisterLocationKind::Temporary &&
                action.destination.kind == NativeRegisterLocationKind::SpillSlot &&
                action.destination.spill_slot == action.slot_index;
            placement = NativeSpillReloadPlacement::AfterOperation;
            break;
        case NativeSpillReloadActionKind::ReloadBeforeUse:
            compatible = !instruction.is_call && instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowRead &&
                std::find(instruction.uses.begin(), instruction.uses.end(), action.value_id) != instruction.uses.end() &&
                action.source.kind == NativeRegisterLocationKind::SpillSlot &&
                action.source.spill_slot == action.slot_index &&
                action.destination.kind == NativeRegisterLocationKind::Temporary;
            placement = NativeSpillReloadPlacement::BeforeOperation;
            break;
        }
        if (!compatible) {
            add_diagnostic(result.diagnostics, "N110AE0006", "spill/reload action kind, Flow operation, or source/destination roles are inconsistent");
            return result;
        }
        const ActionKey key{instruction.flow_operation_id,
            static_cast<unsigned int>(action.kind), action.value_id};
        if (!unique_actions.insert(key).second) {
            add_diagnostic(result.diagnostics, "N110AE0007", "duplicate spill/reload action is bound to one Flow operation");
            return result;
        }
        result.bindings.push_back(NativeBoundSpillReloadAction{
            action, instruction.flow_operation_id, instruction.origin_kind,
            value->second, placement});
    }
    std::sort(result.bindings.begin(), result.bindings.end(), [](const auto& left, const auto& right) {
        return std::tie(left.flow_operation_id, left.placement, left.action.kind, left.action.value_id) <
               std::tie(right.flow_operation_id, right.placement, right.action.kind, right.action.value_id);
    });
    result.succeeded = true;
    return result;
}

NativeFlowSegmentSpillReloadResolutionResult resolve_native_flow_segment_spill_reload_actions(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterFlowSegmentPlan& flow_plan,
    const std::vector<std::uint8_t>& reserved_expression_gp,
    const std::vector<std::uint8_t>& reserved_expression_xmm) {
    NativeFlowSegmentSpillReloadResolutionResult result;
    result.gp_cycle_temporary.kind = NativeRegisterLocationKind::Temporary;
    result.xmm_cycle_temporary.kind = NativeRegisterLocationKind::Temporary;
    const auto fail = [&result](const char* code, const char* message) {
        add_diagnostic(result.diagnostics, code, message);
        result.bindings.clear();
        return result;
    };

    if (!flow_plan.succeeded)
        return fail("N113R0001", "N113 resolver requires a successful N112 Flow segment plan");
    const auto validated = validate_native_register_function_ir(function);
    if (!validated.succeeded) {
        result.diagnostics = validated.diagnostics;
        if (result.diagnostics.empty())
            add_diagnostic(result.diagnostics, "N113R0002", "N113 resolver received invalid Flow register IR");
        return result;
    }

    struct FlowInstructionAnchor {
        const NativeRegisterInstruction* instruction = nullptr;
        std::uint32_t block_index = 0U;
        std::uint32_t instruction_index = 0U;
    };
    std::map<std::uint32_t, NativeRegisterClass> value_classes;
    for (const auto& value : validated.normalized_function.values) {
        if (!value_classes.emplace(value.value_id, value.register_class).second)
            return fail("N113R0003", "Flow IR contains duplicate value IDs while resolving N112 actions");
    }

    std::map<std::uint64_t, FlowInstructionAnchor> anchors_by_operation;
    for (std::size_t block_index = 0U;
         block_index < validated.normalized_function.blocks.size(); ++block_index) {
        const auto& block = validated.normalized_function.blocks[block_index];
        for (std::size_t instruction_index = 0U;
             instruction_index < block.instructions.size(); ++instruction_index) {
            const auto& instruction = block.instructions[instruction_index];
            if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic) continue;
            if (instruction.flow_operation_id == InvalidNativeFlowOperationId ||
                instruction.flow_block_id == InvalidNativeRegisterBlockId ||
                instruction.flow_operation_index == InvalidNativeFlowOperationIndex ||
                block_index > std::numeric_limits<std::uint32_t>::max() ||
                instruction_index > std::numeric_limits<std::uint32_t>::max()) {
                return fail("N113R0004", "Flow-origin instruction has an invalid stable operation anchor");
            }
            const auto inserted = anchors_by_operation.emplace(instruction.flow_operation_id,
                FlowInstructionAnchor{&instruction, static_cast<std::uint32_t>(block_index),
                                      static_cast<std::uint32_t>(instruction_index)});
            if (!inserted.second)
                return fail("N113R0005", "Flow IR contains a duplicate stable operation ID");
        }
    }

    const auto validate_locations = [&value_classes](
        const std::vector<NativeRegisterFlowValueLocation>& locations) {
        std::set<std::uint32_t> seen_values;
        for (const auto& location : locations) {
            const auto value = value_classes.find(location.value_id);
            if (value == value_classes.end() || value->second != location.register_class ||
                !seen_values.insert(location.value_id).second ||
                location.location.kind == NativeRegisterLocationKind::Temporary) return false;
            if (location.location.kind == NativeRegisterLocationKind::Register) {
                const bool valid = location.register_class == NativeRegisterClass::GeneralPurpose
                    ? valid_gp_register(location.location.register_index)
                    : valid_xmm_register(location.location.register_index);
                if (!valid) return false;
            }
        }
        return true;
    };
    std::map<std::uint64_t, const NativeRegisterFlowOperationLocation*> operations_by_id;
    for (const auto& operation : flow_plan.operations) {
        const auto anchor = anchors_by_operation.find(operation.flow_operation_id);
        if (operation.flow_operation_id == InvalidNativeFlowOperationId ||
            anchor == anchors_by_operation.end() || anchor->second.instruction == nullptr ||
            !operations_by_id.emplace(operation.flow_operation_id, &operation).second) {
            return fail("N113R0006", "N112 operation table has an invalid, duplicate, or unanchored Flow ID");
        }
        const auto& instruction = *anchor->second.instruction;
        if (operation.flow_block_id != instruction.flow_block_id ||
            operation.flow_operation_index != instruction.flow_operation_index ||
            operation.native_instruction_index != anchor->second.instruction_index ||
            operation.before_position == InvalidNativeFlowOperationIndex ||
            operation.after_position == InvalidNativeFlowOperationIndex ||
            !validate_locations(operation.before_locations) ||
            !validate_locations(operation.after_locations)) {
            return fail("N113R0007", "N112 operation locations or stable indices do not match Flow IR");
        }
    }

    struct PendingBinding {
        NativeBoundSpillReloadAction binding;
        const NativeRegisterFlowOperationLocation* operation = nullptr;
        bool needs_cycle_temporary = false;
    };
    std::vector<PendingBinding> pending;
    std::set<std::tuple<std::uint64_t, unsigned int, std::uint32_t>> unique_actions;
    std::vector<const NativeRegisterFlowOperationLocation*> gp_temporary_operations;
    std::vector<const NativeRegisterFlowOperationLocation*> xmm_temporary_operations;
    for (const auto& action_binding : flow_plan.actions) {
        const auto anchor = anchors_by_operation.find(action_binding.flow_operation_id);
        const auto operation = operations_by_id.find(action_binding.flow_operation_id);
        if (anchor == anchors_by_operation.end() || operation == operations_by_id.end() ||
            anchor->second.instruction == nullptr || operation->second == nullptr) {
            return fail("N113R0008", "N112 action has no matching Flow IR instruction and operation location");
        }
        const auto& instruction = *anchor->second.instruction;
        const auto& operation_location = *operation->second;
        const auto value = value_classes.find(action_binding.action.value_id);
        if (action_binding.action.block_id != anchor->second.block_index ||
            action_binding.action.instruction_index != anchor->second.instruction_index ||
            action_binding.flow_block_id != operation_location.flow_block_id ||
            action_binding.flow_operation_index != operation_location.flow_operation_index ||
            action_binding.origin_kind != instruction.origin_kind ||
            value == value_classes.end() || value->second != action_binding.register_class) {
            return fail("N113R0009", "N112 action identity, origin, or register class is inconsistent");
        }

        NativeRegisterFlowActionPlacement expected_placement =
            NativeRegisterFlowActionPlacement::BeforeOperation;
        bool compatible = false;
        bool compare_source_before = false;
        bool compare_destination_after = false;
        const auto& action = action_binding.action;
        switch (action.kind) {
        case NativeSpillReloadActionKind::StoreBeforeCall:
            compatible = instruction.is_call &&
                instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowCall &&
                (action.source.kind == NativeRegisterLocationKind::Register ||
                 action.source.kind == NativeRegisterLocationKind::SpillSlot) &&
                action.destination.kind == NativeRegisterLocationKind::HomeSlot &&
                action.destination.spill_slot == action.slot_index;
            compare_source_before = true;
            break;
        case NativeSpillReloadActionKind::ReloadAfterCall:
            compatible = instruction.is_call &&
                instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowCall &&
                action.source.kind == NativeRegisterLocationKind::HomeSlot &&
                action.source.spill_slot == action.slot_index &&
                (action.destination.kind == NativeRegisterLocationKind::Register ||
                 action.destination.kind == NativeRegisterLocationKind::SpillSlot);
            compare_destination_after = true;
            expected_placement = NativeRegisterFlowActionPlacement::AfterOperation;
            break;
        case NativeSpillReloadActionKind::StoreAfterDefinition:
            compatible = !instruction.is_call &&
                (instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowWrite ||
                 instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowDeclare) &&
                std::find(instruction.definitions.begin(), instruction.definitions.end(), action.value_id) !=
                    instruction.definitions.end() &&
                action.source.kind == NativeRegisterLocationKind::Temporary &&
                action.destination.kind == NativeRegisterLocationKind::SpillSlot &&
                action.destination.spill_slot == action.slot_index;
            compare_destination_after = true;
            expected_placement = NativeRegisterFlowActionPlacement::AfterOperation;
            break;
        case NativeSpillReloadActionKind::ReloadBeforeUse:
            compatible = !instruction.is_call &&
                instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowRead &&
                std::find(instruction.uses.begin(), instruction.uses.end(), action.value_id) !=
                    instruction.uses.end() &&
                action.source.kind == NativeRegisterLocationKind::SpillSlot &&
                action.source.spill_slot == action.slot_index &&
                action.destination.kind == NativeRegisterLocationKind::Temporary;
            compare_source_before = true;
            break;
        default:
            compatible = false;
            break;
        }
        if (!compatible || action_binding.placement != expected_placement)
            return fail("N113R000A", "N112 action kind, placement, Flow operands, or location roles are inconsistent");

        NativeRegisterLocation mapped_location;
        if ((compare_source_before &&
             (!find_flow_value_location(operation_location, false, action.value_id, mapped_location) ||
              !same_location(mapped_location, action.source))) ||
            (compare_destination_after &&
             (!find_flow_value_location(operation_location, true, action.value_id, mapped_location) ||
              !same_location(mapped_location, action.destination)))) {
            return fail("N113R000B", "N112 action endpoint does not match its before/after Machine Location");
        }

        const auto identity = std::make_tuple(action_binding.flow_operation_id,
            static_cast<unsigned int>(action.kind), action.value_id);
        if (!unique_actions.insert(identity).second)
            return fail("N113R000C", "N112 action is bound more than once to the same Flow operation and value");

        NativeBoundSpillReloadAction binding;
        binding.action = action;
        binding.flow_operation_id = action_binding.flow_operation_id;
        binding.origin_kind = action_binding.origin_kind;
        binding.register_class = action_binding.register_class;
        binding.placement = expected_placement == NativeRegisterFlowActionPlacement::BeforeOperation
            ? NativeSpillReloadPlacement::BeforeOperation
            : NativeSpillReloadPlacement::AfterOperation;
        const bool needs_temporary = requires_cycle_temporary(action);
        pending.push_back(PendingBinding{std::move(binding), &operation_location, needs_temporary});
        if (needs_temporary) {
            if (action_binding.register_class == NativeRegisterClass::GeneralPurpose) {
                result.gp_cycle_temporary_required = true;
                gp_temporary_operations.push_back(&operation_location);
            } else {
                result.xmm_cycle_temporary_required = true;
                xmm_temporary_operations.push_back(&operation_location);
            }
        }
    }

    const std::set<std::uint8_t> reserved_gp(reserved_expression_gp.begin(), reserved_expression_gp.end());
    const std::set<std::uint8_t> reserved_xmm(reserved_expression_xmm.begin(), reserved_expression_xmm.end());
    if (result.gp_cycle_temporary_required) {
        std::uint8_t selected = 0U;
        if (select_cycle_temporary(NativeRegisterClass::GeneralPurpose, reserved_gp,
                                   gp_temporary_operations, selected) == 0U)
            return fail("N113R000D", "no caller-saved GP temporary is free at every spill/reload action boundary");
        result.gp_cycle_temporary.register_index = selected;
    }
    if (result.xmm_cycle_temporary_required) {
        std::uint8_t selected = 0U;
        if (select_cycle_temporary(NativeRegisterClass::FloatingPoint, reserved_xmm,
                                   xmm_temporary_operations, selected) == 0U)
            return fail("N113R000E", "no caller-saved XMM temporary is free at every spill/reload action boundary");
        result.xmm_cycle_temporary.register_index = selected;
    }

    result.bindings.reserve(pending.size());
    for (auto& item : pending) {
        auto& action = item.binding.action;
        const auto temporary = item.binding.register_class == NativeRegisterClass::GeneralPurpose
            ? result.gp_cycle_temporary : result.xmm_cycle_temporary;
        if (action.source.kind == NativeRegisterLocationKind::Temporary)
            action.source.register_index = temporary.register_index;
        if (action.destination.kind == NativeRegisterLocationKind::Temporary)
            action.destination.register_index = temporary.register_index;
        result.bindings.push_back(std::move(item.binding));
    }
    result.succeeded = true;
    return result;
}

NativeX64EmitterActionRebindingResult rebind_native_x64_flow_actions_to_emitter_locations(
    const NativeRegisterFunctionIR& function,
    const NativeRegisterFlowSegmentPlan& flow_plan,
    const std::vector<NativeBoundSpillReloadAction>& resolved_bindings,
    const NativeX64MachineLocationPlan& machine_locations,
    const std::vector<X64FlowEmitterAnchor>& operation_anchors,
    const NativeX64FrameLocationMap& emitter_frame_locations,
    const std::map<std::uint32_t, std::int32_t>& emitter_local_home_offsets,
    const NativeX64FrameLocationMap& flow_frame_locations,
    const std::uint32_t emitter_spill_slot_count,
    const std::uint32_t emitter_home_slot_count,
    const std::uint32_t local_home_slot_count,
    const std::vector<std::uint8_t>& reserved_expression_gp,
    const std::vector<std::uint8_t>& reserved_expression_xmm,
    const std::map<std::uint64_t, NativeX64ExpressionRegisterLiveness>& expression_liveness) {
    NativeX64EmitterActionRebindingResult result;
    result.gp_cycle_temporary.kind = NativeRegisterLocationKind::Temporary;
    result.xmm_cycle_temporary.kind = NativeRegisterLocationKind::Temporary;
    const auto fail = [&result](const char* code, const char* message) {
        add_diagnostic(result.diagnostics, code, message);
        result.bindings.clear();
        result.frame_locations = NativeX64FrameLocationMap{};
        return result;
    };
    const auto same = [](const NativeRegisterLocation& left, const NativeRegisterLocation& right) {
        return left.kind == right.kind && left.register_index == right.register_index &&
               left.spill_slot == right.spill_slot;
    };
    const auto valid_location = [](const NativeRegisterLocation& location,
                                   const NativeRegisterClass register_class) {
        if (location.kind == NativeRegisterLocationKind::Register)
            return register_class == NativeRegisterClass::GeneralPurpose
                ? valid_gp_register(location.register_index) : valid_xmm_register(location.register_index);
        return location.kind == NativeRegisterLocationKind::SpillSlot ||
               location.kind == NativeRegisterLocationKind::HomeSlot;
    };
    const auto find_position = [](const NativeRegisterFlowOperationLocation& operation,
                                  const bool after, const std::uint32_t value_id)
        -> const NativeRegisterFlowValueLocation* {
        const auto& values = after ? operation.after_locations : operation.before_locations;
        const NativeRegisterFlowValueLocation* found = nullptr;
        for (const auto& value : values) {
            if (value.value_id != value_id) continue;
            if (found != nullptr) return nullptr;
            found = &value;
        }
        return found;
    };

    if (!flow_plan.succeeded || !machine_locations.succeeded)
        return fail("N113G0001", "G1 rebinding requires successful N112 and N111 plans");
    if (resolved_bindings.size() != flow_plan.actions.size())
        return fail("N113G0002", "G1 rebinding requires every N112 action exactly once");
    if (emitter_local_home_offsets.size() != local_home_slot_count)
        return fail("N113G0003", "G1 local-home offset map does not cover the declared lexical local-slot range");
    if (emitter_frame_locations.spill_slot_offsets.size() != emitter_spill_slot_count ||
        emitter_frame_locations.home_slot_offsets.size() != emitter_home_slot_count)
        return fail("N113G0004", "G1 emitter frame map does not cover its complete spill/home allocation namespace");
    if (flow_frame_locations.spill_slot_offsets.size() == 0U &&
        flow_frame_locations.home_slot_offsets.size() == 0U && !flow_plan.actions.empty())
        return fail("N113G0005", "G1 Flow frame map is empty despite planned spill/reload actions");
    if (static_cast<std::uint64_t>(emitter_spill_slot_count) +
            flow_frame_locations.spill_slot_offsets.size() > std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(emitter_home_slot_count) + local_home_slot_count +
            flow_frame_locations.home_slot_offsets.size() > std::numeric_limits<std::uint32_t>::max())
        return fail("N113G0006", "G1 rebased frame-slot namespace exceeds the 32-bit location format");

    const std::uint32_t flow_spill_base = emitter_spill_slot_count;
    const std::uint32_t flow_home_base = emitter_home_slot_count + local_home_slot_count;
    result.frame_locations = emitter_frame_locations;

    std::set<std::int32_t> occupied_offsets;
    const auto add_frame_offset = [&occupied_offsets](const std::int32_t offset) {
        return offset < 0 && (offset % 8) == 0 && occupied_offsets.insert(offset).second;
    };
    for (const auto& item : result.frame_locations.spill_slot_offsets) {
        if (item.first >= emitter_spill_slot_count || !add_frame_offset(item.second))
            return fail("N113G0007", "G1 emitter spill frame map contains an invalid, duplicate, or overlapping offset");
    }
    for (const auto& item : result.frame_locations.home_slot_offsets) {
        if (item.first >= emitter_home_slot_count || !add_frame_offset(item.second))
            return fail("N113G0008", "G1 emitter home frame map contains an invalid, duplicate, or overlapping offset");
    }
    for (std::uint32_t slot = 0U; slot < local_home_slot_count; ++slot) {
        const auto found = emitter_local_home_offsets.find(slot);
        if (found == emitter_local_home_offsets.end() || !add_frame_offset(found->second))
            return fail("N113G0009", "G1 lexical local home has no unique negative aligned RBP displacement");
        const std::uint64_t key = static_cast<std::uint64_t>(emitter_home_slot_count) + slot;
        if (key > std::numeric_limits<std::uint32_t>::max() ||
            !result.frame_locations.home_slot_offsets.emplace(
                static_cast<std::uint32_t>(key), found->second).second)
            return fail("N113G000A", "G1 lexical local-home namespace collides with emitter home slots");
    }
    for (const auto& item : flow_frame_locations.spill_slot_offsets) {
        if (item.first >= flow_frame_locations.spill_slot_offsets.size() || !add_frame_offset(item.second))
            return fail("N113G000B", "G1 Flow spill frame map contains a missing, invalid, duplicate, or overlapping slot");
        const std::uint64_t key = static_cast<std::uint64_t>(flow_spill_base) + item.first;
        if (key > std::numeric_limits<std::uint32_t>::max() ||
            !result.frame_locations.spill_slot_offsets.emplace(
                static_cast<std::uint32_t>(key), item.second).second)
            return fail("N113G000C", "G1 Flow spill-slot namespace collides with emitter spill slots");
        ++result.flow_spill_slots_rebased;
    }
    for (const auto& item : flow_frame_locations.home_slot_offsets) {
        if (item.first >= flow_frame_locations.home_slot_offsets.size() || !add_frame_offset(item.second))
            return fail("N113G000D", "G1 Flow home frame map contains a missing, invalid, duplicate, or overlapping slot");
        const std::uint64_t key = static_cast<std::uint64_t>(flow_home_base) + item.first;
        if (key > std::numeric_limits<std::uint32_t>::max() ||
            !result.frame_locations.home_slot_offsets.emplace(
                static_cast<std::uint32_t>(key), item.second).second)
            return fail("N113G000E", "G1 Flow home-slot namespace collides with emitter/local home slots");
        ++result.flow_home_slots_rebased;
    }

    struct InstructionInfo {
        const NativeRegisterInstruction* instruction = nullptr;
        std::uint32_t block_index = 0U;
        std::uint32_t instruction_index = 0U;
    };
    std::map<std::uint32_t, NativeRegisterClass> classes;
    for (const auto& value : function.values) {
        if (!classes.emplace(value.value_id, value.register_class).second)
            return fail("N113G000F", "G1 Flow function contains duplicate value IDs");
    }
    std::map<std::uint64_t, InstructionInfo> instructions;
    for (std::size_t block_index = 0U; block_index < function.blocks.size(); ++block_index) {
        const auto& block = function.blocks[block_index];
        if (block_index > std::numeric_limits<std::uint32_t>::max())
            return fail("N113G0010", "G1 Flow block index exceeds the 32-bit action format");
        for (std::size_t instruction_index = 0U; instruction_index < block.instructions.size(); ++instruction_index) {
            const auto& instruction = block.instructions[instruction_index];
            if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic) continue;
            if (instruction.flow_operation_id == InvalidNativeFlowOperationId ||
                instruction_index > std::numeric_limits<std::uint32_t>::max() ||
                !instructions.emplace(instruction.flow_operation_id, InstructionInfo{
                    &instruction, static_cast<std::uint32_t>(block_index),
                    static_cast<std::uint32_t>(instruction_index)}).second)
                return fail("N113G0011", "G1 Flow instruction table contains an invalid or duplicate operation ID");
        }
    }
    std::map<std::uint64_t, const NativeRegisterFlowOperationLocation*> operations;
    std::set<std::tuple<std::uint64_t, unsigned int, std::uint32_t>> planned_actions;
    for (const auto& flow_action : flow_plan.actions) {
        const auto key = std::make_tuple(flow_action.flow_operation_id,
            static_cast<unsigned int>(flow_action.action.kind), flow_action.action.value_id);
        if (!planned_actions.insert(key).second)
            return fail("N113G0012A", "G1 N112 action table contains a duplicate operation/value/action identity");
    }
    for (const auto& operation : flow_plan.operations) {
        if (operation.flow_operation_id == InvalidNativeFlowOperationId ||
            !operations.emplace(operation.flow_operation_id, &operation).second)
            return fail("N113G0012", "G1 N112 operation table contains an invalid or duplicate Flow operation ID");
        const auto instruction = instructions.find(operation.flow_operation_id);
        if (instruction == instructions.end() || instruction->second.instruction == nullptr ||
            operation.flow_block_id != instruction->second.instruction->flow_block_id ||
            operation.flow_operation_index != instruction->second.instruction->flow_operation_index ||
            operation.native_instruction_index != instruction->second.instruction_index)
            return fail("N113G0013", "G1 N112 operation identity does not match the original Flow instruction");
    }
    std::map<std::uint64_t, const X64FlowEmitterAnchor*> anchors;
    for (const auto& anchor : operation_anchors) {
        if (anchor.flow_operation_id == InvalidNativeFlowOperationId ||
            !anchors.emplace(anchor.flow_operation_id, &anchor).second)
            return fail("N113G0014", "G1 emitter-anchor table contains an invalid or duplicate Flow operation ID");
    }

    const auto rebase_emitter_location = [&](const NativeRegisterLocation& location,
                                              const NativeRegisterClass register_class,
                                              NativeRegisterLocation& mapped) -> bool {
        mapped = location;
        if (!valid_location(location, register_class)) return false;
        if (location.kind == NativeRegisterLocationKind::HomeSlot) {
            if (location.spill_slot >= local_home_slot_count) return false;
            const std::uint64_t key = static_cast<std::uint64_t>(emitter_home_slot_count) + location.spill_slot;
            if (key > std::numeric_limits<std::uint32_t>::max() ||
                result.frame_locations.home_slot_offsets.find(static_cast<std::uint32_t>(key)) ==
                    result.frame_locations.home_slot_offsets.end()) return false;
            mapped.spill_slot = static_cast<std::uint32_t>(key);
        } else if (location.kind == NativeRegisterLocationKind::SpillSlot) {
            if (location.spill_slot >= emitter_spill_slot_count ||
                result.frame_locations.spill_slot_offsets.find(location.spill_slot) ==
                    result.frame_locations.spill_slot_offsets.end()) return false;
        }
        return true;
    };
    const auto rebase_flow_location = [&](const NativeRegisterLocation& location,
                                          const NativeRegisterClass register_class,
                                          NativeRegisterLocation& mapped) -> bool {
        mapped = location;
        if (location.kind == NativeRegisterLocationKind::SpillSlot) {
            if (location.spill_slot >= flow_frame_locations.spill_slot_offsets.size()) return false;
            const std::uint64_t key = static_cast<std::uint64_t>(flow_spill_base) + location.spill_slot;
            if (key > std::numeric_limits<std::uint32_t>::max() ||
                result.frame_locations.spill_slot_offsets.find(static_cast<std::uint32_t>(key)) ==
                    result.frame_locations.spill_slot_offsets.end()) return false;
            mapped.spill_slot = static_cast<std::uint32_t>(key);
            return true;
        }
        if (location.kind == NativeRegisterLocationKind::HomeSlot) {
            if (location.spill_slot >= flow_frame_locations.home_slot_offsets.size()) return false;
            const std::uint64_t key = static_cast<std::uint64_t>(flow_home_base) + location.spill_slot;
            if (key > std::numeric_limits<std::uint32_t>::max() ||
                result.frame_locations.home_slot_offsets.find(static_cast<std::uint32_t>(key)) ==
                    result.frame_locations.home_slot_offsets.end()) return false;
            mapped.spill_slot = static_cast<std::uint32_t>(key);
            return true;
        }
        if (location.kind == NativeRegisterLocationKind::Register)
            return valid_location(location, register_class);
        return false;
    };

    std::map<std::pair<std::uint64_t, std::uint32_t>, std::uint32_t> call_store_home_slots;
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::uint32_t> call_reload_home_slots;
    std::set<std::tuple<std::uint64_t, unsigned int, std::uint32_t>> unique_actions;
    result.bindings.reserve(resolved_bindings.size());
    for (const auto& binding : resolved_bindings) {
        const auto instruction = instructions.find(binding.flow_operation_id);
        const auto operation = operations.find(binding.flow_operation_id);
        const auto anchor = anchors.find(binding.flow_operation_id);
        if (instruction == instructions.end() || operation == operations.end() || anchor == anchors.end() ||
            instruction->second.instruction == nullptr || operation->second == nullptr || anchor->second == nullptr)
            return fail("N113G0015", "G1 action has no exact Flow instruction, N112 operation, or emitter anchor");
        const auto& flow_instruction = *instruction->second.instruction;
        const auto& flow_operation = *operation->second;
        const auto& emitter_anchor = *anchor->second;
        const auto value_class = classes.find(binding.action.value_id);
        const auto machine_value = machine_locations.values.find(binding.action.value_id);
        if (value_class == classes.end() || value_class->second != binding.register_class ||
            machine_value == machine_locations.values.end() ||
            machine_value->second.register_class != binding.register_class ||
            emitter_anchor.flow_block_id != flow_instruction.flow_block_id ||
            emitter_anchor.flow_operation_index != flow_instruction.flow_operation_index ||
            emitter_anchor.origin_kind != flow_instruction.origin_kind ||
            emitter_anchor.flow_block_id != flow_operation.flow_block_id ||
            emitter_anchor.flow_operation_index != flow_operation.flow_operation_index ||
            emitter_anchor.function_index == InvalidNativeFunctionIndex ||
            binding.origin_kind != flow_instruction.origin_kind ||
            binding.action.block_id != instruction->second.block_index ||
            binding.action.instruction_index != instruction->second.instruction_index ||
            !unique_actions.emplace(binding.flow_operation_id,
                static_cast<unsigned int>(binding.action.kind), binding.action.value_id).second ||
            planned_actions.find(std::make_tuple(binding.flow_operation_id,
                static_cast<unsigned int>(binding.action.kind), binding.action.value_id)) == planned_actions.end())
            return fail("N113G0016", "G1 action value/class/function/block/operation identity is inconsistent or duplicated");
        const auto planned_action = std::find_if(flow_plan.actions.begin(), flow_plan.actions.end(),
            [&binding](const NativeRegisterFlowSegmentActionBinding& item) {
                return item.flow_operation_id == binding.flow_operation_id &&
                    item.action.kind == binding.action.kind && item.action.value_id == binding.action.value_id;
            });
        if (planned_action == flow_plan.actions.end() ||
            planned_action->placement != (binding.placement == NativeSpillReloadPlacement::BeforeOperation
                ? NativeRegisterFlowActionPlacement::BeforeOperation
                : NativeRegisterFlowActionPlacement::AfterOperation) ||
            planned_action->action.slot_index != binding.action.slot_index ||
            planned_action->action.block_id != binding.action.block_id ||
            planned_action->action.instruction_index != binding.action.instruction_index ||
            !same(planned_action->action.source, binding.action.source) ||
            !same(planned_action->action.destination, binding.action.destination) ||
            planned_action->flow_block_id != operation->second->flow_block_id ||
            planned_action->flow_operation_index != operation->second->flow_operation_index ||
            planned_action->origin_kind != binding.origin_kind ||
            planned_action->register_class != binding.register_class)
            return fail("N113G0016A", "G1 resolved N110B binding differs from the original N112 action contract");

        const auto before = find_position(flow_operation, false, binding.action.value_id);
        const auto after = find_position(flow_operation, true, binding.action.value_id);
        if (before == nullptr && after == nullptr)
            return fail("N113G0017", "G1 action value is absent from both N112 operation positions");
        const auto& active = machine_value->second.active_location;
        NativeRegisterLocation mapped_active;
        if (!rebase_emitter_location(active, binding.register_class, mapped_active))
            return fail("N113G0018", "G1 N111 active location has no valid concrete emitter frame/register mapping");

        NativeBoundSpillReloadAction rebound = binding;
        NativeRegisterLocation flow_endpoint;
        switch (binding.action.kind) {
        case NativeSpillReloadActionKind::StoreBeforeCall: {
            if (!flow_instruction.is_call || flow_instruction.origin_kind != NativeRegisterInstructionOriginKind::FlowCall ||
                binding.placement != NativeSpillReloadPlacement::BeforeOperation || before == nullptr || after == nullptr ||
                !same(before->location, binding.action.source) ||
                binding.action.destination.kind != NativeRegisterLocationKind::HomeSlot ||
                binding.action.destination.spill_slot != binding.action.slot_index ||
                emitter_anchor.machine_code_before_offset == InvalidNativeFlowOperationIndex ||
                emitter_anchor.machine_code_call_instruction_offset == InvalidNativeFlowOperationIndex ||
                emitter_anchor.machine_code_after_call_offset !=
                    emitter_anchor.machine_code_call_instruction_offset + 5U ||
                emitter_anchor.function_code_size == 0U ||
                emitter_anchor.machine_code_before_offset > emitter_anchor.machine_code_call_instruction_offset ||
                emitter_anchor.machine_code_after_call_offset > emitter_anchor.machine_code_offset ||
                emitter_anchor.machine_code_after_call_offset > emitter_anchor.function_code_size)
                return fail("N113G0019", "G1 StoreBeforeCall is not bound to its exact call boundary and Flow HomeSlot");
            if (!rebase_flow_location(binding.action.destination, binding.register_class, flow_endpoint))
                return fail("N113G001A", "G1 StoreBeforeCall destination Flow HomeSlot has no reserved frame offset");
            rebound.action.source = mapped_active;
            rebound.action.destination = flow_endpoint;
            call_store_home_slots.emplace(std::make_pair(binding.flow_operation_id, binding.action.value_id),
                binding.action.destination.spill_slot);
            break;
        }
        case NativeSpillReloadActionKind::ReloadAfterCall: {
            if (!flow_instruction.is_call || flow_instruction.origin_kind != NativeRegisterInstructionOriginKind::FlowCall ||
                binding.placement != NativeSpillReloadPlacement::AfterOperation || before == nullptr || after == nullptr ||
                !same(after->location, binding.action.destination) ||
                binding.action.source.kind != NativeRegisterLocationKind::HomeSlot ||
                binding.action.source.spill_slot != binding.action.slot_index ||
                emitter_anchor.machine_code_after_call_offset == InvalidNativeFlowOperationIndex ||
                emitter_anchor.machine_code_call_instruction_offset == InvalidNativeFlowOperationIndex ||
                emitter_anchor.machine_code_after_call_offset !=
                    emitter_anchor.machine_code_call_instruction_offset + 5U ||
                emitter_anchor.function_code_size == 0U ||
                emitter_anchor.machine_code_after_call_offset > emitter_anchor.machine_code_offset ||
                emitter_anchor.machine_code_after_call_offset > emitter_anchor.function_code_size)
                return fail("N113G001B", "G1 ReloadAfterCall is not bound to its exact post-call boundary and Flow HomeSlot");
            if (!rebase_flow_location(binding.action.source, binding.register_class, flow_endpoint))
                return fail("N113G001C", "G1 ReloadAfterCall source Flow HomeSlot has no reserved frame offset");
            rebound.action.source = flow_endpoint;
            rebound.action.destination = mapped_active;
            call_reload_home_slots.emplace(std::make_pair(binding.flow_operation_id, binding.action.value_id),
                binding.action.source.spill_slot);
            break;
        }
        case NativeSpillReloadActionKind::StoreAfterDefinition: {
            const auto expected_kind = flow_instruction.origin_kind == NativeRegisterInstructionOriginKind::FlowWrite
                ? X64FlowEmitterAnchor::OperandKind::ValueWrite
                : X64FlowEmitterAnchor::OperandKind::ValueDeclare;
            if (flow_instruction.is_call ||
                (flow_instruction.origin_kind != NativeRegisterInstructionOriginKind::FlowWrite &&
                 flow_instruction.origin_kind != NativeRegisterInstructionOriginKind::FlowDeclare) ||
                binding.placement != NativeSpillReloadPlacement::AfterOperation || after == nullptr ||
                !same(after->location, binding.action.destination) ||
                binding.action.source.kind != NativeRegisterLocationKind::Temporary ||
                binding.action.destination.kind != NativeRegisterLocationKind::SpillSlot ||
                binding.action.destination.spill_slot != binding.action.slot_index ||
                emitter_anchor.emitted_operand_kind != expected_kind ||
                emitter_anchor.emitted_operand_value_id != binding.action.value_id ||
                !same(emitter_anchor.emitted_operand_location, active) ||
                emitter_anchor.machine_code_before_offset == InvalidNativeFlowOperationIndex ||
                emitter_anchor.machine_code_before_offset >= emitter_anchor.machine_code_offset ||
                emitter_anchor.machine_code_offset > emitter_anchor.function_code_size)
                return fail("N113G001D", "G1 StoreAfterDefinition does not match the exact emitted definition operand and Flow SpillSlot");
            if (!rebase_flow_location(binding.action.destination, binding.register_class, flow_endpoint))
                return fail("N113G001E", "G1 StoreAfterDefinition destination Flow SpillSlot has no reserved frame offset");
            rebound.action.source = mapped_active;
            rebound.action.destination = flow_endpoint;
            break;
        }
        case NativeSpillReloadActionKind::ReloadBeforeUse: {
            if (flow_instruction.is_call || flow_instruction.origin_kind != NativeRegisterInstructionOriginKind::FlowRead ||
                binding.placement != NativeSpillReloadPlacement::BeforeOperation || before == nullptr ||
                !same(before->location, binding.action.source) ||
                binding.action.source.kind != NativeRegisterLocationKind::SpillSlot ||
                binding.action.source.spill_slot != binding.action.slot_index ||
                emitter_anchor.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::ValueRead ||
                emitter_anchor.emitted_operand_value_id != binding.action.value_id ||
                !same(emitter_anchor.emitted_operand_location, active) ||
                emitter_anchor.machine_code_before_offset == InvalidNativeFlowOperationIndex ||
                emitter_anchor.machine_code_before_offset >= emitter_anchor.machine_code_offset ||
                emitter_anchor.machine_code_offset > emitter_anchor.function_code_size)
                return fail("N113G001F", "G1 ReloadBeforeUse does not match the exact emitted value-read operand and Flow SpillSlot");
            if (!rebase_flow_location(binding.action.source, binding.register_class, flow_endpoint))
                return fail("N113G0020", "G1 ReloadBeforeUse source Flow SpillSlot has no reserved frame offset");
            rebound.action.source = flow_endpoint;
            rebound.action.destination = mapped_active;
            break;
        }
        default:
            return fail("N113G0021", "G1 received an unsupported spill/reload action kind");
        }

        if (rebound.action.source.kind == NativeRegisterLocationKind::Temporary ||
            rebound.action.destination.kind == NativeRegisterLocationKind::Temporary ||
            !valid_location(rebound.action.source, binding.register_class) ||
            !valid_location(rebound.action.destination, binding.register_class))
            return fail("N113G0022", "G1 rebinding left an abstract or register-class-invalid endpoint");
        const auto memory = [](const NativeRegisterLocation& location) {
            return location.kind == NativeRegisterLocationKind::SpillSlot ||
                   location.kind == NativeRegisterLocationKind::HomeSlot;
        };
        const bool memory_to_memory = memory(rebound.action.source) && memory(rebound.action.destination);
        if (memory_to_memory) {
            ++result.memory_to_memory_actions;
            const bool gp_action = binding.register_class == NativeRegisterClass::GeneralPurpose;
            // Prove a scratch register at this action's exact insertion boundary.
            // The previous algorithm intersected before+after and every action
            // in the function, although these transfers execute sequentially.
            NativeRegisterFlowOperationLocation boundary_operation;
            const auto& boundary_flow_locations = binding.placement == NativeSpillReloadPlacement::AfterOperation
                ? flow_operation.after_locations : flow_operation.before_locations;
            boundary_operation.before_locations = boundary_flow_locations;
            for (const auto& flow_location : boundary_flow_locations) {
                const auto active_value = machine_locations.values.find(flow_location.value_id);
                if (active_value == machine_locations.values.end()) continue;
                boundary_operation.before_locations.push_back(NativeRegisterFlowValueLocation{
                    flow_location.value_id, active_value->second.register_class,
                    active_value->second.active_location});
            }

            const auto live_expression = expression_liveness.find(binding.flow_operation_id);
            const bool exact_expression_snapshot = live_expression != expression_liveness.end();
            const std::set<std::uint8_t> reserved_gp(
                exact_expression_snapshot ? live_expression->second.gp_registers.begin() : reserved_expression_gp.begin(),
                exact_expression_snapshot ? live_expression->second.gp_registers.end() : reserved_expression_gp.end());
            const std::set<std::uint8_t> reserved_xmm(
                exact_expression_snapshot ? live_expression->second.xmm_registers.begin() : reserved_expression_xmm.begin(),
                exact_expression_snapshot ? live_expression->second.xmm_registers.end() : reserved_expression_xmm.end());
            const std::vector<const NativeRegisterFlowOperationLocation*> boundary_pointers{&boundary_operation};
            std::uint8_t selected = 0U;
            NativeRegisterClass selected_scratch_class = binding.register_class;
            bool scratch_found = select_cycle_temporary(
                binding.register_class, gp_action ? reserved_gp : reserved_xmm,
                boundary_pointers, selected) != 0U;
            if (!scratch_found && gp_action) {
                // GP memory-to-memory movement is bitwise: a dead volatile
                // XMM1-XMM5 register can carry the 64-bit payload via MOVQ.
                // No cycle SpillSlot is needed because this is one sequential
                // transfer, not a parallel-copy dependency cycle.
                selected_scratch_class = NativeRegisterClass::FloatingPoint;
                scratch_found = select_cycle_temporary(
                    NativeRegisterClass::FloatingPoint, reserved_xmm,
                    boundary_pointers, selected) != 0U;
            }
            if (!scratch_found) {
                std::string detail = gp_action
                    ? "no safe GP or XMM expansion scratch register is free at this exact N112/N111 action boundary"
                    : "no caller-saved XMM scratch register is free at this exact N112/N111 action boundary";
                detail += " | FlowOperationId=" + std::to_string(binding.flow_operation_id);
                detail += binding.placement == NativeSpillReloadPlacement::AfterOperation
                    ? " | Placement=AfterOperation" : " | Placement=BeforeOperation";
                detail += exact_expression_snapshot ? " | ExpressionSnapshot=Exact" : " | ExpressionSnapshot=FunctionWideFallback";
                const auto append_register_bank = [&](const char* prefix, const std::vector<std::uint8_t>& candidates,
                                                      const std::set<std::uint8_t>& reserved_registers,
                                                      const NativeRegisterClass candidate_class) {
                    for (const auto candidate : candidates) {
                        detail += std::string(" | ") + prefix + std::to_string(candidate) + "=";
                        if (reserved_registers.find(candidate) != reserved_registers.end()) {
                            detail += "ExpressionReserved";
                            continue;
                        }
                        bool found = false;
                        for (std::size_t i = 0U; i < boundary_flow_locations.size(); ++i) {
                            const auto& value = boundary_flow_locations[i];
                            if (value.register_class == candidate_class &&
                                value.location.kind == NativeRegisterLocationKind::Register &&
                                value.location.register_index == candidate) {
                                detail += (found ? "," : "N112ValueIds=") + std::to_string(value.value_id);
                                found = true;
                            }
                        }
                        for (std::size_t i = boundary_flow_locations.size();
                             i < boundary_operation.before_locations.size(); ++i) {
                            const auto& value = boundary_operation.before_locations[i];
                            if (value.register_class == candidate_class &&
                                value.location.kind == NativeRegisterLocationKind::Register &&
                                value.location.register_index == candidate) {
                                detail += (found ? "," : "N111ValueIds=") + std::to_string(value.value_id);
                                found = true;
                            }
                        }
                        if (!found) detail += "NotConflictingByKnownLocations";
                    }
                };
                append_register_bank("R", {8U, 9U, 10U, 11U}, reserved_gp,
                                     NativeRegisterClass::GeneralPurpose);
                append_register_bank("XMM", {1U, 2U, 3U, 4U, 5U}, reserved_xmm,
                                     NativeRegisterClass::FloatingPoint);
                return fail(gp_action ? "N113G0023" : "N113G0024", detail.c_str());
            }
            rebound.cycle_temporary.kind = NativeRegisterLocationKind::Temporary;
            rebound.cycle_temporary.register_index = selected;
            rebound.cycle_temporary_assigned = true;
            rebound.cycle_temporary_register_class = selected_scratch_class;
            if (selected_scratch_class == NativeRegisterClass::GeneralPurpose) {
                result.gp_cycle_temporary_required = true;
                if (result.gp_cycle_temporary.register_index == 0U)
                    result.gp_cycle_temporary.register_index = selected;
            } else {
                result.xmm_cycle_temporary_required = true;
                if (result.xmm_cycle_temporary.register_index == 0U)
                    result.xmm_cycle_temporary.register_index = selected;
            }
        }
        result.bindings.push_back(std::move(rebound));
    }

    if (call_store_home_slots.size() != call_reload_home_slots.size())
        return fail("N113G0022A", "G1 call-crossing Flow value is missing its matching pre-call store or post-call reload");
    for (const auto& item : call_store_home_slots) {
        const auto reload = call_reload_home_slots.find(item.first);
        if (reload == call_reload_home_slots.end() || reload->second != item.second)
            return fail("N113G0022B", "G1 call-crossing store and reload do not use the same N112 Flow HomeSlot");
    }

    // Per-binding scratch proofs above replace the old function-wide intersection.
    // Keep the first selected scratch in the legacy scalar fields for old callers.

    result.rebound_actions = static_cast<std::uint32_t>(result.bindings.size());
    result.succeeded = true;
    return result;
}

NativeX64ActionLocationAuditResult audit_native_x64_flow_action_locations(
    const std::vector<NativeBoundSpillReloadAction>& bindings,
    const NativeX64MachineLocationPlan& machine_locations) {
    NativeX64ActionLocationAuditResult result;
    const auto fail = [&result](const std::string& code, const std::string& message) {
        add_diagnostic(result.diagnostics, code.c_str(), message.c_str());
        return result;
    };
    if (!machine_locations.succeeded)
        return fail("N113L0001", "N111 machine-location plan is not successful");

    using AuditedActionIdentity = std::tuple<std::uint64_t, std::uint32_t, NativeSpillReloadActionKind>;
    std::set<AuditedActionIdentity> unique_actions;
    const auto classify_endpoint = [&result](
        const NativeRegisterLocation& endpoint,
        const NativeX64MachineValueLocation* active_location) {
        if (active_location == nullptr) {
            ++result.endpoints_without_persistent_binding;
            return NativeX64ActionEndpointLocationStatus::NoPersistentBinding;
        }
        if (endpoint.kind != NativeRegisterLocationKind::Register ||
            active_location->active_location.kind != NativeRegisterLocationKind::Register) {
            ++result.unmapped_slot_or_temporary_endpoints;
            return NativeX64ActionEndpointLocationStatus::UnmappedSlotOrTemporary;
        }
        if (endpoint.register_index == active_location->active_location.register_index) {
            ++result.register_endpoint_matches;
            return NativeX64ActionEndpointLocationStatus::RegisterMatch;
        }
        ++result.register_endpoint_mismatches;
        return NativeX64ActionEndpointLocationStatus::RegisterMismatch;
    };

    result.entries.reserve(bindings.size());
    for (const auto& binding : bindings) {
        if (binding.flow_operation_id == InvalidNativeFlowOperationId)
            return fail("N113L0002", "N112 action has no stable Flow operation identity");
        const AuditedActionIdentity key{
            binding.flow_operation_id, binding.action.value_id, binding.action.kind};
        if (!unique_actions.insert(key).second)
            return fail("N113L0003", "duplicate N112 action identity reached the N111 location audit");

        const auto found = machine_locations.values.find(binding.action.value_id);
        const NativeX64MachineValueLocation* active_location =
            found == machine_locations.values.end() ? nullptr : &found->second;
        if (active_location != nullptr && active_location->register_class != binding.register_class)
            return fail("N113L0004", "N112 action value class disagrees with the N111 active location");
        if (active_location == nullptr) ++result.actions_without_persistent_binding;

        NativeX64ActionLocationAuditEntry entry;
        entry.flow_operation_id = binding.flow_operation_id;
        entry.value_id = binding.action.value_id;
        entry.action_kind = binding.action.kind;
        entry.source_status = classify_endpoint(binding.action.source, active_location);
        entry.destination_status = classify_endpoint(binding.action.destination, active_location);
        result.entries.push_back(entry);
        ++result.audited_actions;
    }
    result.succeeded = true;
    return result;
}

NativeX64FlowOperationLocationAuditResult audit_native_x64_flow_operation_locations(
    const NativeRegisterFlowSegmentPlan& flow_plan,
    const NativeX64MachineLocationPlan& machine_locations) {
    NativeX64FlowOperationLocationAuditResult result;
    const auto fail = [&result](const std::string& code, const std::string& message) {
        add_diagnostic(result.diagnostics, code.c_str(), message.c_str());
        return result;
    };
    if (!flow_plan.succeeded)
        return fail("N113D0001", "N112 Flow operation-location plan is not successful");
    if (!machine_locations.succeeded)
        return fail("N113D0002", "N111 machine-location plan is not successful");

    std::set<std::uint64_t> unique_operations;
    const auto audit_locations = [&result, &machine_locations](
        std::uint64_t operation_id,
        const std::vector<NativeRegisterFlowValueLocation>& locations,
        bool before_operation) -> bool {
        std::set<std::uint32_t> unique_values;
        for (const auto& location : locations) {
            if (!unique_values.insert(location.value_id).second) return false;
            const auto found = machine_locations.values.find(location.value_id);
            const NativeX64MachineValueLocation* active_location =
                found == machine_locations.values.end() ? nullptr : &found->second;
            NativeX64ActionEndpointLocationStatus status =
                NativeX64ActionEndpointLocationStatus::NoPersistentBinding;
            if (active_location == nullptr) {
                ++result.value_positions_without_persistent_binding;
                ++result.unbound_value_positions;
            } else {
                ++result.persistent_value_positions;
                if (active_location->register_class != location.register_class) return false;
                if (location.location.kind != NativeRegisterLocationKind::Register ||
                    active_location->active_location.kind != NativeRegisterLocationKind::Register) {
                    status = NativeX64ActionEndpointLocationStatus::UnmappedSlotOrTemporary;
                    ++result.unmapped_slot_or_temporary_positions;
                } else if (location.location.register_index ==
                           active_location->active_location.register_index) {
                    status = NativeX64ActionEndpointLocationStatus::RegisterMatch;
                    ++result.register_position_matches;
                } else {
                    status = NativeX64ActionEndpointLocationStatus::RegisterMismatch;
                    ++result.register_position_mismatches;
                }
            }
            result.entries.push_back(NativeX64FlowOperationLocationAuditEntry{
                operation_id, location.value_id, before_operation, status});
            ++result.audited_value_positions;
        }
        return true;
    };

    for (const auto& operation : flow_plan.operations) {
        if (operation.flow_operation_id == InvalidNativeFlowOperationId ||
            operation.before_position == InvalidNativeFlowOperationIndex ||
            operation.after_position == InvalidNativeFlowOperationIndex ||
            !unique_operations.insert(operation.flow_operation_id).second) {
            return fail("N113D0003", "N112 operation has an invalid or duplicate stable Flow identity/position");
        }
        if (!audit_locations(operation.flow_operation_id, operation.before_locations, true) ||
            !audit_locations(operation.flow_operation_id, operation.after_locations, false)) {
            return fail("N113D0004", "N112 operation contains duplicate values or a value-class mismatch");
        }
        ++result.audited_operations;
    }
    result.succeeded = true;
    return result;
}

} // namespace xfrontend
