#include "xfrontend/native_cfg_bridge.hpp"
#include "xfrontend/x64_codegen.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
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

std::string cfg_source(unsigned int variant) {
    const unsigned bump = 1U + (variant % 7U);
    return "module N108; "
           "private function Helper(int value) returns int { return value + " +
           std::to_string(bump) + "; } "
           "private function Unused() returns int { return 99; } "
           "public function Main() returns int { "
           "int result = 0; int i = 0; double fraction = 0.5; "
           "while (i < 5) { "
           "if (i == 2) { result = result + 10; } "
           "else { result = result + Helper(i); } "
           "fraction = fraction + 0.25; i = i + 1; } "
           "if (result >= 0) { return result; } else { return 1; } }";
}

ParseResult parse_valid(const std::string& source) {
    auto parsed = parse_source(source);
    require(parsed.succeeded(), parsed.diagnostics.empty()
        ? "N108 source parse failed without a diagnostic"
        : "N108 source parse failed: " + parsed.diagnostics.front().code + " " +
          parsed.diagnostics.front().message);
    return parsed;
}

std::vector<const NativeRegisterInstruction*> flow_instructions(
    const NativeRegisterFunctionIR& function) {
    std::vector<const NativeRegisterInstruction*> result;
    for (const auto& block : function.blocks) {
        for (const auto& instruction : block.instructions) {
            if (instruction.origin_kind != NativeRegisterInstructionOriginKind::Synthetic)
                result.push_back(&instruction);
        }
    }
    return result;
}

void test_flow_identity_is_canonical_and_repeatable(unsigned int variant) {
    const auto parsed = parse_valid(cfg_source(variant));
    const auto first = lower_control_flow_to_native_register_ir(parsed.unit);
    const auto second = lower_control_flow_to_native_register_ir(parsed.unit);
    require(first.succeeded && second.succeeded && first.functions.size() == second.functions.size(),
        "N108 CFG-to-register lowering failed");

    std::size_t anchored_count = 0U;
    std::set<NativeRegisterInstructionOriginKind> kinds;
    for (std::size_t function_index = 0U; function_index < first.functions.size(); ++function_index) {
        const auto& left = first.functions[function_index].register_ir;
        const auto& right = second.functions[function_index].register_ir;
        require(left.blocks.size() == right.blocks.size(), "N108 repeated lowering changed block count");
        const auto left_instructions = flow_instructions(left);
        const auto right_instructions = flow_instructions(right);
        require(left_instructions.size() == right_instructions.size(),
            "N108 repeated lowering changed Flow operation count");
        std::set<std::uint64_t> ids_in_function;
        for (std::size_t index = 0U; index < left_instructions.size(); ++index) {
            const auto& instruction = *left_instructions[index];
            const auto& repeated = *right_instructions[index];
            const std::uint64_t expected =
                ((static_cast<std::uint64_t>(instruction.flow_block_id) + 1U) << 32U) |
                (static_cast<std::uint64_t>(instruction.flow_operation_index) + 1U);
            require(instruction.flow_operation_id == expected,
                "N108 Flow operation ID is not the canonical (block, ordinal) encoding");
            require(instruction.flow_operation_id == repeated.flow_operation_id &&
                    instruction.flow_block_id == repeated.flow_block_id &&
                    instruction.flow_operation_index == repeated.flow_operation_index &&
                    instruction.origin_kind == repeated.origin_kind &&
                    instruction.source_span.source_id == repeated.source_span.source_id &&
                    instruction.source_span.start.byte_offset == repeated.source_span.start.byte_offset &&
                    instruction.source_span.end.byte_offset == repeated.source_span.end.byte_offset,
                "N108 Flow identity or source span is not deterministic");
            require(instruction.source_span.end.byte_offset > instruction.source_span.start.byte_offset,
                "N108 Flow operation has an empty or reversed source span");
            require(ids_in_function.insert(instruction.flow_operation_id).second,
                "N108 stable Flow ID is duplicated within a function");
            kinds.insert(instruction.origin_kind);
            ++anchored_count;
        }
    }
    require(anchored_count > 20U, "N108 CFG bridge exposed too few source Flow operations");
    require(kinds.count(NativeRegisterInstructionOriginKind::FlowRead) != 0U &&
            kinds.count(NativeRegisterInstructionOriginKind::FlowDeclare) != 0U &&
            kinds.count(NativeRegisterInstructionOriginKind::FlowWrite) != 0U &&
            kinds.count(NativeRegisterInstructionOriginKind::FlowCall) != 0U &&
            kinds.count(NativeRegisterInstructionOriginKind::FlowReturn) != 0U,
        "N108 CFG bridge did not retain all supported source-operation categories");
}

void test_validator_rejects_noncanonical_flow_metadata() {
    const auto parsed = parse_valid(cfg_source(0U));
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded && !lowered.functions.empty(), "N108 source lowering failed");
    const auto& original = lowered.functions.front().register_ir;
    require(validate_native_register_function_ir(original).succeeded,
        "N108 validator rejected valid Flow origin metadata");

    auto malformed = original;
    bool changed = false;
    for (auto& block : malformed.blocks) {
        for (auto& instruction : block.instructions) {
            if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic) continue;
            instruction.flow_operation_id ^= (1ULL << 32U);
            changed = true;
            break;
        }
        if (changed) break;
    }
    require(changed, "N108 test fixture has no Flow-origin instruction");
    const auto rejected = validate_native_register_function_ir(malformed);
    require(!rejected.succeeded && !rejected.diagnostics.empty() &&
            rejected.diagnostics.front().code == "N108E0002",
        "N108 validator accepted a noncanonical Flow operation ID");

    malformed = original;
    changed = false;
    for (auto& block : malformed.blocks) {
        for (auto& instruction : block.instructions) {
            if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic) continue;
            instruction.source_span.end.byte_offset = instruction.source_span.start.byte_offset;
            changed = true;
            break;
        }
        if (changed) break;
    }
    require(changed, "N108 test fixture has no Flow-origin span");
    const auto span_rejected = validate_native_register_function_ir(malformed);
    require(!span_rejected.succeeded && !span_rejected.diagnostics.empty() &&
            span_rejected.diagnostics.front().code == "N108E0002",
        "N108 validator accepted an empty Flow source span");
}

void test_codegen_emits_one_machine_anchor_per_flow_operation(unsigned int variant) {
    const auto parsed = parse_valid(cfg_source(variant));
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded, "N108 CFG lowering failed before code generation");
    const auto compiled = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n108-flow-anchor", make_coff_x86_64_object_target()});
    require(compiled.succeeded(), compiled.diagnostics.empty()
        ? "N108 x64 code generation failed without a diagnostic"
        : "N108 x64 code generation failed: " + compiled.diagnostics.front().code + " " +
          compiled.diagnostics.front().message);

    std::size_t expected_anchor_count = 0U;
    for (const auto& function : lowered.functions)
        expected_anchor_count += flow_instructions(function.register_ir).size();
    require(compiled.flow_anchors.size() == expected_anchor_count,
        "N108 emitter did not produce exactly one anchor per Flow-origin register instruction");
    require(compiled.stats.native_flow_emitter_anchors_emitted == expected_anchor_count,
        "N108 anchor statistics do not match the exported anchor table");

    std::set<std::pair<std::size_t, std::uint64_t>> anchor_ids;
    std::set<NativeRegisterInstructionOriginKind> emitted_kinds;
    for (const auto& anchor : compiled.flow_anchors) {
        require(anchor.function_index < parsed.unit.functions.size(),
            "N108 anchor refers to an invalid source function index");
        require(anchor.flow_operation_id != InvalidNativeFlowOperationId &&
                anchor.flow_block_id != InvalidNativeRegisterBlockId &&
                anchor.flow_operation_index != InvalidNativeFlowOperationIndex,
            "N108 anchor lost its Flow identity");
        require(anchor.source_span.end.byte_offset > anchor.source_span.start.byte_offset,
            "N108 anchor lost its source span");
        require(anchor.function_code_size > 0U &&
                anchor.machine_code_offset <= anchor.function_code_size,
            "N108 machine-code boundary lies outside its final function code");
        if (anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowRead) {
            require(anchor.machine_code_before_offset != InvalidNativeFlowOperationIndex &&
                    anchor.machine_code_before_offset <= anchor.machine_code_offset,
                "N110B pre-use reload boundary is missing or follows the emitted read");
        }
        if (anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowCall) {
            require(anchor.machine_code_before_offset != InvalidNativeFlowOperationIndex &&
                    anchor.machine_code_after_call_offset != InvalidNativeFlowOperationIndex &&
                    anchor.machine_code_before_offset <= anchor.machine_code_after_call_offset &&
                    anchor.machine_code_after_call_offset <= anchor.machine_code_offset,
                "N110B call save/reload boundaries are missing or out of order");
        }
        require(anchor_ids.insert({anchor.function_index, anchor.flow_operation_id}).second,
            "N108 emitter exported a duplicate anchor ID");
        emitted_kinds.insert(anchor.origin_kind);
    }
    require(emitted_kinds.count(NativeRegisterInstructionOriginKind::FlowRead) != 0U &&
            emitted_kinds.count(NativeRegisterInstructionOriginKind::FlowDeclare) != 0U &&
            emitted_kinds.count(NativeRegisterInstructionOriginKind::FlowWrite) != 0U &&
            emitted_kinds.count(NativeRegisterInstructionOriginKind::FlowCall) != 0U &&
            emitted_kinds.count(NativeRegisterInstructionOriginKind::FlowReturn) != 0U,
        "N108 x64 emitter omitted one or more supported source-operation categories");

    std::vector<std::size_t> body_indices;
    for (std::size_t index = 0U; index < parsed.unit.functions.size(); ++index) {
        if (parsed.unit.functions[index].has_body && parsed.unit.functions[index].body)
            body_indices.push_back(index);
    }
    require(body_indices.size() == lowered.functions.size(),
        "N108 source functions and lowered CFG functions are not aligned");
    for (std::size_t function_index = 0U; function_index < lowered.functions.size(); ++function_index) {
        const std::size_t source_function_index = body_indices[function_index];
        for (const auto* instruction : flow_instructions(lowered.functions[function_index].register_ir)) {
            const auto found = std::find_if(compiled.flow_anchors.begin(), compiled.flow_anchors.end(),
                [&](const X64FlowEmitterAnchor& anchor) {
                    return anchor.function_index == source_function_index &&
                           anchor.flow_operation_id == instruction->flow_operation_id;
                });
            require(found != compiled.flow_anchors.end(),
                "N108 emitter omitted a Flow operation from the final anchor table");
            require(found->flow_block_id == instruction->flow_block_id &&
                    found->flow_operation_index == instruction->flow_operation_index &&
                    found->origin_kind == instruction->origin_kind &&
                    found->source_span.start.byte_offset == instruction->source_span.start.byte_offset &&
                    found->source_span.end.byte_offset == instruction->source_span.end.byte_offset,
                "N108 emitter anchor metadata does not match the originating Flow operation");
        }
    }
}

std::string large_frame_source() {
    std::string source = "module N108Large; public function Main(int seed) returns int { int v0 = seed; ";
    for (unsigned index = 1U; index < 520U; ++index)
        source += "int v" + std::to_string(index) + " = " + std::to_string(index) + "; ";
    source += "return v519; }";
    return source;
}

void test_anchor_offsets_follow_stack_probe_insertion() {
    const auto parsed = parse_valid(large_frame_source());
    const auto compiled = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n108-flow-anchor-large-frame", make_coff_x86_64_object_target()});
    require(compiled.succeeded(), compiled.diagnostics.empty()
        ? "N108 large-frame code generation failed without a diagnostic"
        : "N108 large-frame code generation failed: " + compiled.diagnostics.front().code + " " +
          compiled.diagnostics.front().message);
    require(compiled.stats.win64_stack_probe_functions == 1U,
        "N108 large-frame fixture did not activate the N106 Stack Probe path");
    require(!compiled.flow_anchors.empty(), "N108 large-frame compile emitted no Flow anchors");
    const auto first_declaration = std::find_if(compiled.flow_anchors.begin(), compiled.flow_anchors.end(),
        [](const X64FlowEmitterAnchor& anchor) {
            return anchor.origin_kind == NativeRegisterInstructionOriginKind::FlowDeclare;
        });
    require(first_declaration != compiled.flow_anchors.end() && first_declaration->machine_code_offset >= 14U,
        "N108 declaration anchor was not shifted beyond the inserted __chkstk prologue sequence");
    for (const auto& anchor : compiled.flow_anchors) {
        require(anchor.function_code_size > anchor.machine_code_offset,
            "N108 large-frame anchor is not within the final post-probe code body");
    }
}

void test_final_anchor_offsets_are_deterministic() {
    const auto parsed = parse_valid(cfg_source(3U));
    const auto first = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n108-flow-anchor-repeat", make_coff_x86_64_object_target()});
    const auto second = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n108-flow-anchor-repeat", make_coff_x86_64_object_target()});
    require(first.succeeded() && second.succeeded(), "N108 repeated codegen failed");
    require(first.flow_anchors.size() == second.flow_anchors.size(),
        "N108 repeated codegen changed the anchor count");
    for (std::size_t index = 0U; index < first.flow_anchors.size(); ++index) {
        const auto& left = first.flow_anchors[index];
        const auto& right = second.flow_anchors[index];
        require(left.function_index == right.function_index &&
                left.flow_operation_id == right.flow_operation_id &&
                left.flow_block_id == right.flow_block_id &&
                left.flow_operation_index == right.flow_operation_index &&
                left.origin_kind == right.origin_kind &&
                left.machine_code_offset == right.machine_code_offset &&
                left.function_code_size == right.function_code_size,
            "N108 repeated codegen changed a stable Flow-to-machine anchor");
    }
}

} // namespace

void run_n108_flow_emitter_anchor_tests(int& passed, int& failed) {
    for (unsigned int index = 0U; index < 10U; ++index) {
        run_case("N108A_flow_identity_and_source_span_" + std::to_string(index),
            [index]() { test_flow_identity_is_canonical_and_repeatable(index); }, passed, failed);
        run_case("N108C_codegen_one_anchor_per_flow_operation_" + std::to_string(index),
            [index]() { test_codegen_emits_one_machine_anchor_per_flow_operation(index); }, passed, failed);
    }
    run_case("N108B_reject_noncanonical_flow_metadata",
        test_validator_rejects_noncanonical_flow_metadata, passed, failed);
    run_case("N108D_anchor_offsets_after_stack_probe_insertion",
        test_anchor_offsets_follow_stack_probe_insertion, passed, failed);
    run_case("N108E_deterministic_final_anchor_offsets",
        test_final_anchor_offsets_are_deterministic, passed, failed);
}
