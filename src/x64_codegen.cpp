#include "xfrontend/x64_codegen.hpp"

#include "xfrontend/semantic.hpp"
#include "xfrontend/linear_scan_allocator.hpp"
#include "xfrontend/native_register_analysis.hpp"
#include "xfrontend/native_cfg_bridge.hpp"
#include "xfrontend/win64_unwind_codegen.hpp"
#include "xfrontend/x64_location_transfer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>

namespace xfrontend {
namespace {

void add_diag(std::vector<Diagnostic>& diagnostics, const char* code,
              const std::string& message, const SourceSpan& span = {}) {
    diagnostics.push_back(Diagnostic{code, message, span});
}

std::string fold_name(std::string value) {
    for (char& character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            character = static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
    }
    return value;
}

std::string fold_type(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), value.end());
    return fold_name(std::move(value));
}

bool is_integer_type(const std::string& value) {
    const std::string type = fold_type(value);
    return type == "int" || type == "i64" || type == "int64" || type == "long";
}

bool is_boolean_type(const std::string& value) {
    const std::string type = fold_type(value);
    return type == "bool" || type == "boolean";
}

bool is_single_float_type(const std::string& value) {
    return fold_type(value) == "float" || fold_type(value) == "f32";
}

bool is_double_float_type(const std::string& value) {
    return fold_type(value) == "double" || fold_type(value) == "f64";
}

bool is_float_type(const std::string& value) {
    return is_single_float_type(value) || is_double_float_type(value);
}

bool is_pointer_type(const std::string& value) {
    const std::string type = fold_type(value);
    return (!type.empty() && (type.front() == '*' || type.back() == '*'));
}

bool parse_floating(const std::string& text, double& value) {
    std::string clean;
    clean.reserve(text.size());
    for (char ch : text) if (ch != '_') clean.push_back(ch);
    if (clean.empty()) return false;
    char* end = nullptr;
    const double parsed = std::strtod(clean.c_str(), &end);
    if (end == clean.c_str() || end == nullptr || *end != '\0') return false;
    value = parsed;
    return true;
}

std::string pointee_type(std::string value) {
    value = fold_type(std::move(value));
    if (!value.empty() && value.back() == '*') value.pop_back();
    else if (!value.empty() && value.front() == '*') value.erase(value.begin());
    return value;
}

DropSymbolVisibility native_visibility(Visibility visibility) {
    switch (visibility) {
    case Visibility::Public: return DropSymbolVisibility::Public;
    case Visibility::Internal: return DropSymbolVisibility::Internal;
    case Visibility::Private: return DropSymbolVisibility::Private;
    }
    return DropSymbolVisibility::Private;
}

bool parse_integer(const std::string& text, std::int64_t& value) {
    std::string clean;
    clean.reserve(text.size());
    for (char ch : text) if (ch != '_') clean.push_back(ch);
    if (clean.empty()) return false;
    try {
        std::size_t consumed = 0U;
        const long long parsed = std::stoll(clean, &consumed, 0);
        if (consumed != clean.size()) return false;
        value = static_cast<std::int64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

class FunctionEmitter {
public:
    FunctionEmitter(const CompilationUnit& unit,
                    const std::vector<std::string>& mangled_names,
                    const X64CodegenOptions& options,
                    std::size_t function_index,
                    const NativeRegisterFlowAllocationResult* flow_allocation,
                    const NativeRegisterFlowFunction* flow_function,
                    X64CodegenStats& stats)
        : unit_(unit), mangled_names_(mangled_names), options_(options), function_index_(function_index),
          function_(unit.functions[function_index]), flow_allocation_(flow_allocation),
          flow_function_(flow_function), stats_(stats) {}

    bool run(NativeFunctionCodeArtifact& artifact, std::vector<Diagnostic>& diagnostics,
             std::vector<X64FlowEmitterAnchor>& flow_anchors,
             std::vector<X64FlowBranchAnchor>& flow_branch_anchors) {
        diagnostics_ = &diagnostics;
        if (!supported_function_signature()) return false;
        if (!function_.body) {
            add_diag(diagnostics, "XN48C0001", "function body is missing", function_.span);
            return false;
        }
        prepare_expression_register_allocation();
        if (!diagnostics.empty()) return false;
        if (flow_allocation_ == nullptr || !flow_allocation_->succeeded ||
            flow_function_ == nullptr) {
            add_diag(diagnostics, "XN86C0001", "CFG-derived persistent-value allocation plan is missing or invalid", function_.span);
            return false;
        }
        if (!prepare_flow_binding_lookup()) return false;
        if (!prepare_flow_emitter_anchor_lookup()) return false;
        if (!prepare_flow_branch_anchor_lookup()) return false;
        if ((options_.validate_n113_flow_action_anchors ||
             options_.validate_n113_flow_action_locations ||
             options_.validate_n113_flow_operation_locations ||
             options_.validate_n113_flow_emitter_operand_locations ||
             options_.plan_n113_emitter_action_rebinding ||
             options_.insert_n113_emitter_spill_reload_actions ||
             options_.audit_n113_flow_edge_locations ||
             options_.audit_n113_flow_edge_schedule_readiness ||
             options_.plan_n113_flow_edge_scratch_per_edge ||
             options_.audit_n113_flow_operation_operand_ownership ||
             options_.audit_n113_flow_edge_bundle_eligibility ||
             options_.audit_n113_frame_spill_scratch) &&
            !prepare_n113_flow_action_plan()) return false;

        code_.push_back(0x55U);                         // push rbp
        code_.insert(code_.end(), {0x48U, 0x89U, 0xE5U}); // mov rbp,rsp
        stack_allocation_opcode_offset_ = code_.size();
        code_.insert(code_.end(), {0x48U, 0x81U, 0xECU}); // sub rsp, frame-size
        frame_size_patch_ = code_.size();
        emit_u32(0U);
        emit_callee_save_prologue();
        prologue_end_offset_ = code_.size();

        stats_.native_cfg_blocks_analyzed += static_cast<std::uint32_t>(flow_allocation_->liveness.blocks.size());
        stats_.native_flow_values_analyzed += static_cast<std::uint32_t>(flow_allocation_->intervals.intervals.size());
        stats_.native_flow_spill_actions_planned += static_cast<std::uint32_t>(flow_allocation_->allocation.actions.size());
        stats_.native_flow_spill_slots_planned += flow_allocation_->allocation.spill_slot_count;

        scopes_.emplace_back();
        scope_slots_.emplace_back();
        for (std::size_t index = 0U; index < function_.parameters.size(); ++index) {
            const auto& parameter = function_.parameters[index];
            const std::string name = fold_name(parameter.name);
            if (scopes_.back().find(name) != scopes_.back().end()) {
                add_diag(diagnostics, "XN48C0002", "duplicate parameter name", parameter.span);
                continue;
            }
            const auto binding = flow_value_by_binding_key_.find(flow_binding_key(parameter.name, parameter.span));
            if (binding == flow_value_by_binding_key_.end()) {
                add_diag(diagnostics, "XN90C0001", "parameter has no scalar CFG home binding", parameter.span);
                continue;
            }
            const std::size_t slot = next_slot_++;
            if (slot >= slot_types_.size()) slot_types_.resize(slot + 1U);
            slot_types_[slot] = parameter.type_name;
            scopes_.back().emplace(name, slot);
            scope_slots_.back().push_back(slot);
            if (!record_flow_home(parameter.name, parameter.span, slot)) continue;
            if (is_float_type(parameter.type_name)) emit_store_float_argument_register(index, slot, is_single_float_type(parameter.type_name));
            else emit_store_argument_register(index, slot);
            emit_local_register_initialize_from_home(slot, parameter.type_name);
        }

        if (diagnostics.empty()) compile_statement(*function_.body);
        if (!diagnostics.empty()) return false;
        if (!finalize_home_phi_coalescing()) return false;

        emit_xor_eax_eax(); // deterministic fallback for permitted fall-through paths
        const std::size_t epilogue = code_.size();
        emit_callee_save_epilogue();
        code_.insert(code_.end(), {0x48U, 0x89U, 0xECU, 0x5DU, 0xC3U}); // mov rsp,rbp; pop rbp; ret
        for (const auto displacement_offset : epilogue_fixups_) {
            if (!patch_rel32(displacement_offset, epilogue)) {
                add_diag(diagnostics, "XN48C0003", "function branch displacement is outside rel32 range", function_.span);
                return false;
            }
        }
        const std::uint64_t local_bytes_64 = static_cast<std::uint64_t>(next_slot_) * 8U;
        if (local_bytes_64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            add_diag(diagnostics, "XN48C0004", "function local stack area exceeds Win64 backend limit", function_.span);
            return false;
        }
        if (options_.reserve_n113_flow_frame_slots || options_.insert_n113_emitter_spill_reload_actions) {
            n113_flow_frame_binding_ = plan_native_x64_flow_frame_binding(
                allocation_plan_, flow_allocation_->allocation,
                static_cast<std::uint32_t>(local_bytes_64));
            if (!n113_flow_frame_binding_.succeeded) {
                const std::string detail = n113_flow_frame_binding_.diagnostics.empty()
                    ? "unknown Flow frame-binding failure"
                    : n113_flow_frame_binding_.diagnostics.front().code + " " +
                      n113_flow_frame_binding_.diagnostics.front().message;
                add_diag(diagnostics, "XN113F0001",
                    "N113 Flow frame-slot reservation failed: " + detail, function_.span);
                return false;
            }
            frame_plan_ = n113_flow_frame_binding_.frame_plan;
            stats_.native_n113_flow_frame_spill_slots_reserved +=
                n113_flow_frame_binding_.flow_spill_slots_reserved;
            stats_.native_n113_flow_frame_home_slots_reserved +=
                n113_flow_frame_binding_.flow_home_slots_reserved;
        } else {
            frame_plan_ = build_native_win64_frame_plan(
                allocation_plan_, static_cast<std::uint32_t>(local_bytes_64));
        }
        if (!frame_plan_.succeeded) {
            const std::string detail = frame_plan_.diagnostics.empty() ? "unknown frame-planning failure" :
                frame_plan_.diagnostics.front().code + " " + frame_plan_.diagnostics.front().message;
            add_diag(diagnostics, "XN72C0001", "Win64 frame planning failed: " + detail, function_.span);
            return false;
        }
        if ((options_.audit_n113_flow_edge_locations || options_.audit_n113_flow_edge_schedule_readiness ||
             options_.plan_n113_flow_edge_scratch_per_edge ||
             options_.audit_n113_flow_operation_operand_ownership ||
             options_.audit_n113_flow_edge_bundle_eligibility ||
             options_.audit_n113_frame_spill_scratch) &&
            !audit_n113_flow_edge_location_crosswalk(static_cast<std::uint32_t>(local_bytes_64))) return false;
        if (frame_plan_.frame_size_bytes > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
            add_diag(diagnostics, "XN48C0004", "function stack frame exceeds Win64 backend limit", function_.span);
            return false;
        }
        if (frame_plan_.frame_size_bytes >= 4096U) {
            const auto probe_plan = build_win64_stack_probe_plan(frame_plan_.frame_size_bytes);
            if (!probe_plan.succeeded || probe_plan.probe_offsets.empty()) {
                const std::string detail = probe_plan.diagnostics.empty()
                    ? "large-frame stack probe plan is empty"
                    : probe_plan.diagnostics.front().code + " " + probe_plan.diagnostics.front().message;
                add_diag(*diagnostics_, "XN106C0001", "Win64 stack probe planning failed: " + detail, function_.span);
                return false;
            }
            if (!insert_win64_stack_probe_call(frame_plan_.frame_size_bytes)) return false;
            ++stats_.win64_stack_probe_functions;
        }
        patch_u32(frame_size_patch_, frame_plan_.frame_size_bytes);
        if (!patch_frame_save_fixups()) return false;
        if (!patch_spill_slot_fixups()) return false;
        if (!finalize_flow_emitter_anchors(flow_anchors)) return false;
        if (!finalize_flow_branch_anchors(flow_branch_anchors)) return false;
        if (options_.validate_n113_flow_action_anchors && !validate_n113_flow_action_anchors()) return false;
        if (options_.validate_n113_flow_action_locations && !validate_n113_flow_action_locations()) return false;
        if (options_.validate_n113_flow_operation_locations &&
            !validate_n113_flow_operation_locations()) return false;
        if (options_.validate_n113_flow_emitter_operand_locations &&
            !validate_n113_flow_emitter_operand_locations()) return false;
        if ((options_.plan_n113_emitter_action_rebinding ||
             options_.insert_n113_emitter_spill_reload_actions) &&
            !build_n113_emitter_action_rebinding_plan()) return false;
        if (options_.insert_n113_emitter_spill_reload_actions &&
            !insert_n113_emitter_action_transfers(flow_anchors, flow_branch_anchors)) return false;
        if (options_.audit_n113_flow_edge_schedule_readiness &&
            !audit_n113_flow_edge_schedule_readiness_plan(flow_anchors, flow_branch_anchors)) return false;
        if ((options_.plan_n113_flow_edge_scratch_per_edge ||
             options_.audit_n113_flow_edge_bundle_eligibility) &&
            !plan_n113_flow_edge_scratch_per_edge(flow_anchors, flow_branch_anchors)) return false;
        if ((options_.audit_n113_flow_operation_operand_ownership ||
             options_.audit_n113_flow_edge_bundle_eligibility) &&
            !audit_n113_flow_operation_operand_ownership_plan(flow_anchors)) return false;
        if (options_.audit_n113_flow_edge_bundle_eligibility &&
            !audit_n113_flow_edge_bundle_eligibility()) return false;

        if (prologue_end_offset_ == 0U || prologue_end_offset_ > 255U) {
            add_diag(*diagnostics_, "XN88C0001", "Win64 prologue exceeds the UNWIND_INFO v1 byte-sized prologue limit", function_.span);
            return false;
        }
        std::vector<Win64PrologueAction> unwind_actions;
        unwind_actions.push_back(Win64PrologueAction{
            Win64PrologueAction::Kind::PushNonvolatileGpr, 1U, 5U, 0U, 0U});
        unwind_actions.push_back(Win64PrologueAction{
            Win64PrologueAction::Kind::SetFramePointer, 4U, 5U, 0U, 0U});
        if (frame_plan_.frame_size_bytes != 0U) {
            const std::size_t allocation_end = stack_allocation_opcode_offset_ + 7U;
            if (allocation_end > std::numeric_limits<std::uint8_t>::max()) {
                add_diag(*diagnostics_, "XN106C0002", "Win64 stack-allocation prologue offset exceeds UNWIND_INFO v1 range", function_.span);
                return false;
            }
            unwind_actions.push_back(Win64PrologueAction{
                Win64PrologueAction::Kind::AllocateStack, static_cast<std::uint8_t>(allocation_end), 0U, 0U,
                frame_plan_.frame_size_bytes});
        }
        for (const auto& fixup : prologue_save_fixups_) {
            const auto found = std::find_if(frame_plan_.prologue_saves.begin(), frame_plan_.prologue_saves.end(),
                [&fixup](const NativeWin64RegisterSave& save) {
                    return save.register_class == fixup.register_class && save.register_index == fixup.register_index;
                });
            if (found == frame_plan_.prologue_saves.end()) {
                add_diag(*diagnostics_, "XN88C0002", "Win64 unwind save action has no matching frame save slot", function_.span);
                return false;
            }
            const std::int64_t rsp_offset = static_cast<std::int64_t>(frame_plan_.frame_size_bytes) + found->frame_offset;
            if (rsp_offset < 0 || rsp_offset > std::numeric_limits<std::uint32_t>::max()) {
                add_diag(*diagnostics_, "XN88C0003", "Win64 unwind save offset is outside the supported stack range", function_.span);
                return false;
            }
            const auto kind = fixup.register_class == NativeRegisterClass::GeneralPurpose
                ? Win64PrologueAction::Kind::SaveNonvolatileGpr
                : Win64PrologueAction::Kind::SaveNonvolatileXmm;
            unwind_actions.push_back(Win64PrologueAction{
                kind, static_cast<std::uint8_t>(fixup.instruction_end_offset),
                fixup.register_index, static_cast<std::uint32_t>(rsp_offset), 0U});
        }
        const auto unwind = build_win64_prologue_unwind_plan(
            0U, code_.size(), static_cast<std::uint8_t>(prologue_end_offset_), 5U, 0U, unwind_actions);
        if (!unwind.succeeded()) {
            const std::string detail = unwind.diagnostics.empty() ? "unknown UNWIND_INFO failure" :
                unwind.diagnostics.front().code + " " + unwind.diagnostics.front().message;
            add_diag(*diagnostics_, "XN88C0004", "Win64 function unwind metadata generation failed: " + detail, function_.span);
            return false;
        }
        artifact.win64_unwind_info = unwind.encoded_unwind_info;
        ++stats_.win64_unwind_records_emitted;

        stats_.stack_frame_bytes = frame_plan_.frame_size_bytes;
        stats_.local_stack_slots_peak = static_cast<std::uint32_t>(next_slot_);
        stats_.native_spill_slots = allocation_plan_.spill_slot_count;
        stats_.callee_saved_gp_registers = static_cast<std::uint32_t>(allocation_plan_.used_callee_saved_gp.size());
        stats_.callee_saved_xmm_registers = static_cast<std::uint32_t>(allocation_plan_.used_callee_saved_xmm.size());

        artifact.function_index = function_index_;
        artifact.code = std::move(code_);
        artifact.alignment = 16U;
        artifact.linker_root = function_.visibility == Visibility::Public && fold_name(function_.name) == "main";
        artifact.relocations = std::move(relocations_);
        return true;
    }

private:
    struct PendingBreakBranch {
        FlowControlStatementId control_statement_id = InvalidFlowControlStatementId;
        std::size_t displacement_offset = 0U;
    };

    struct LoopContext {
        std::size_t continue_target = 0U;
        FlowControlConstructId control_construct_id = InvalidFlowControlConstructId;
        std::vector<PendingBreakBranch> break_displacements;
    };

    using FlowAnchorKey = std::tuple<unsigned int, std::size_t, std::size_t, unsigned int>;

    struct FlowAnchorCandidate {
        const NativeRegisterInstruction* instruction = nullptr;
        bool recorded = false;
    };

    struct FrameDisplacementFixup {
        NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
        std::uint8_t register_index = 0U;
        std::size_t displacement_offset = 0U;
        std::size_t instruction_end_offset = 0U;
    };

    const CompilationUnit& unit_;
    const std::vector<std::string>& mangled_names_;
    const X64CodegenOptions& options_;
    std::size_t function_index_;
    const FunctionDeclaration& function_;
    const NativeRegisterFlowAllocationResult* flow_allocation_ = nullptr;
    const NativeRegisterFlowFunction* flow_function_ = nullptr;
    X64CodegenStats& stats_;
    std::vector<Diagnostic>* diagnostics_ = nullptr;
    std::vector<std::uint8_t> code_;
    std::vector<NativeObjectRelocationInput> relocations_;
    std::vector<NativeX64RelativeBranchFixup> relative_branch_fixups_;
    std::vector<std::map<std::string, std::size_t>> scopes_;
    std::vector<std::vector<std::size_t>> scope_slots_;
    std::vector<std::size_t> free_local_slots_;
    std::vector<std::string> slot_types_;
    std::vector<LoopContext> loops_;
    std::vector<std::size_t> epilogue_fixups_;
    std::unordered_map<const Expression*, std::uint32_t> expression_register_ids_;
    std::unordered_map<const Expression*, NativeRegisterAssignment> expression_registers_;
    std::vector<std::uint8_t> active_expression_gp_registers_;
    std::vector<std::uint8_t> active_expression_xmm_registers_;
    std::map<std::uint64_t, NativeX64ExpressionRegisterLiveness> expression_liveness_by_operation_;
    std::map<std::string, std::uint32_t> flow_value_by_binding_key_;
    std::map<std::uint32_t, NativeRegisterLocation> flow_value_home_locations_;
    NativeX64MachineLocationPlan flow_machine_location_plan_;
    NativeRegisterFlowSegmentPlan n113_flow_segment_plan_;
    NativeFlowSegmentSpillReloadResolutionResult n113_flow_action_resolution_;
    NativeX64EmitterActionRebindingResult n113_emitter_action_rebinding_;
    std::map<FlowAnchorKey, std::vector<FlowAnchorCandidate>> flow_emitter_anchor_candidates_;
    std::set<std::uint64_t> recorded_flow_operation_ids_;
    std::vector<X64FlowEmitterAnchor> emitted_flow_anchors_;
    std::unordered_map<const Statement*, FlowControlStatementId> control_statement_ids_;
    std::map<std::pair<FlowControlStatementId, FlowEdgeKind>, std::vector<const FlowEdge*>> flow_branch_edge_candidates_;
    std::set<FlowEdgeId> recorded_flow_edge_ids_;
    std::vector<X64FlowBranchAnchor> emitted_flow_branch_anchors_;
    std::map<std::size_t, NativeRegisterLocation> local_register_locations_;
    NativeRegisterVariableHomePlan flow_home_plan_;
    NativeRegisterAllocationResult persistent_register_allocation_;
    NativeRegisterAllocationPlan allocation_plan_;
    NativeWin64FramePlan frame_plan_;
    NativeX64FlowFrameBindingPlan n113_flow_frame_binding_;
    NativeX64FrameLocationMap n113_flow_edge_frame_locations_;
    NativeX64FlowEdgeLocationAuditPlan n113_flow_edge_location_audit_;
    NativeX64FlowEdgeScheduleReadinessPlan n113_flow_edge_readiness_;
    NativeX64FlowEdgeTransferCandidatePlan n113_flow_edge_transfer_candidates_;
    NativeX64FlowEdgePerEdgeScratchPlan n113_flow_edge_per_edge_scratch_plan_;
    NativeX64FlowOperationOperandOwnershipPlan n113_flow_operation_operand_ownership_plan_;
    NativeX64FlowEdgeBundleEligibilityPlan n113_flow_edge_bundle_eligibility_plan_;
    std::vector<FrameDisplacementFixup> prologue_save_fixups_;
    std::vector<FrameDisplacementFixup> epilogue_restore_fixups_;
    std::vector<std::pair<std::size_t, std::uint32_t>> spill_displacement_fixups_;
    std::size_t next_slot_ = 0U;
    std::size_t temporary_depth_ = 0U;
    std::size_t frame_size_patch_ = 0U;
    std::size_t stack_allocation_opcode_offset_ = 0U;
    std::size_t prologue_end_offset_ = 0U;

    static std::size_t align_up(std::size_t value, std::size_t alignment) {
        return (value + alignment - 1U) & ~(alignment - 1U);
    }

    bool expression_may_be_floating(const Expression& expression) const {
        if (expression.kind == ExprKind::Literal && expression.text.find('.') != std::string::npos) return true;
        if (expression.kind == ExprKind::Identifier) {
            const std::string wanted = fold_name(expression.text);
            for (const auto& parameter : function_.parameters)
                if (fold_name(parameter.name) == wanted && is_float_type(parameter.type_name)) return true;
            const auto scan_statement = [&](const auto& self, const Statement& statement) -> bool {
                if (statement.kind == StatementKind::VariableDeclaration &&
                    fold_name(statement.name) == wanted && is_float_type(statement.type_name)) return true;
                for (const auto& child : statement.statements)
                    if (child && self(self, *child)) return true;
                return statement.else_branch && self(self, *statement.else_branch);
            };
            if (function_.body && scan_statement(scan_statement, *function_.body)) return true;
        }
        if (expression.kind == ExprKind::Call && !expression.children.empty()) {
            const auto& callee = *expression.children.front();
            if (callee.kind == ExprKind::Identifier) {
                for (const auto& candidate : unit_.functions)
                    if (fold_name(candidate.name) == fold_name(callee.text) && is_float_type(candidate.return_type)) return true;
            } else if (callee.kind == ExprKind::Member && !callee.children.empty() &&
                       callee.children.front()->kind == ExprKind::Identifier) {
                for (const auto& imported : options_.imported_functions)
                    if (fold_name(imported.module_name) == fold_name(callee.children.front()->text) &&
                        fold_name(imported.name) == fold_name(callee.text) && is_float_type(imported.return_type)) return true;
            }
        }
        for (const auto& child : expression.children)
            if (child && expression_may_be_floating(*child)) return true;
        return false;
    }

    static bool expression_contains_call(const Expression& expression) {
        if (expression.kind == ExprKind::Call) return true;
        for (const auto& child : expression.children)
            if (child && expression_contains_call(*child)) return true;
        return false;
    }

    void collect_expression_register_ir(const Expression& expression,
                                        NativeRegisterFunctionIR& function_ir,
                                        std::uint32_t& next_id) {
        auto& block = function_ir.blocks.front();
        if (expression.kind == ExprKind::Binary && expression.children.size() == 2U &&
            expression.text != "&&" && expression.text != "||") {
            collect_expression_register_ir(*expression.children[0], function_ir, next_id);
            const std::uint32_t id = next_id++;
            const NativeRegisterClass register_class = expression_may_be_floating(expression)
                ? NativeRegisterClass::FloatingPoint : NativeRegisterClass::GeneralPurpose;
            function_ir.values.push_back(NativeRegisterValueInfo{id, register_class});
            NativeRegisterInstruction definition;
            definition.definitions.push_back(id);
            block.instructions.push_back(std::move(definition));
            expression_register_ids_[&expression] = id;
            collect_expression_register_ir(*expression.children[1], function_ir, next_id);
            NativeRegisterInstruction use;
            use.uses.push_back(id);
            block.instructions.push_back(std::move(use));
            return;
        }
        for (const auto& child : expression.children)
            if (child) collect_expression_register_ir(*child, function_ir, next_id);
        if (expression.kind == ExprKind::Call) {
            NativeRegisterInstruction call;
            call.is_call = true;
            block.instructions.push_back(std::move(call));
        }
    }

    void collect_statement_register_ir(const Statement& statement,
                                       NativeRegisterFunctionIR& function_ir,
                                       std::uint32_t& next_id) {
        for (const auto& expression : statement.expressions)
            if (expression) collect_expression_register_ir(*expression, function_ir, next_id);
        for (const auto& child : statement.statements)
            if (child) collect_statement_register_ir(*child, function_ir, next_id);
        if (statement.else_branch)
            collect_statement_register_ir(*statement.else_branch, function_ir, next_id);
    }

    void prepare_expression_register_allocation() {
        NativeRegisterFunctionIR function_ir;
        function_ir.entry_block = 0U;
        NativeRegisterBasicBlock entry;
        entry.id = 0U;
        function_ir.blocks.push_back(std::move(entry));
        std::uint32_t next_id = 1U;
        if (function_.body) collect_statement_register_ir(*function_.body, function_ir, next_id);

        if (function_ir.values.empty()) {
            allocation_plan_.succeeded = true;
            return;
        }
        const auto validated = validate_native_register_function_ir(function_ir);
        if (!validated.succeeded) {
            const std::string detail = validated.diagnostics.empty() ? "invalid native register IR" :
                validated.diagnostics.front().code + " " + validated.diagnostics.front().message;
            add_diag(*diagnostics_, "XN72C0002", "AST-to-register-IR lowering failed: " + detail, function_.span);
            return;
        }
        const auto liveness = compute_native_register_liveness(validated.normalized_function);
        if (!liveness.succeeded || !liveness.converged) {
            add_diag(*diagnostics_, "XN72C0003", "native register liveness did not converge", function_.span);
            return;
        }
        const auto intervals = analyze_native_register_live_intervals(validated.normalized_function, liveness);
        if (!intervals.succeeded) {
            add_diag(*diagnostics_, "XN72C0004", "native register interval analysis failed", function_.span);
            return;
        }
        const auto allocated = allocate_native_registers(intervals.intervals,
            NativeRegisterAllocationOptions{true, true, true, true});
        if (!allocated.succeeded || allocated.assignments.size() != intervals.intervals.size()) {
            add_diag(*diagnostics_, "XN72C0005", "native register allocation failed", function_.span);
            return;
        }

        std::map<std::uint32_t, NativeRegisterAssignment> assignments;
        for (const auto& assignment : allocated.assignments) assignments.emplace(assignment.value_id, assignment);
        allocation_plan_.succeeded = true;
        allocation_plan_.spill_slot_count = allocated.spill_slot_count;
        allocation_plan_.used_callee_saved_gp = allocated.used_callee_saved_gp;
        allocation_plan_.used_callee_saved_xmm = allocated.used_callee_saved_xmm;
        allocation_plan_.stats = allocated.stats;
        std::uint32_t segment_id = 1U;
        for (const auto& interval : intervals.intervals) {
            const auto found = assignments.find(interval.value_id);
            if (found == assignments.end()) {
                add_diag(*diagnostics_, "XN72C0006", "register allocator omitted an AST temporary", function_.span);
                allocation_plan_.succeeded = false;
                return;
            }
            NativeRegisterLiveSegment segment;
            segment.segment_id = segment_id++;
            segment.value_id = interval.value_id;
            segment.block_id = 0U;
            segment.start = interval.start;
            segment.end = interval.end;
            segment.register_class = interval.register_class;
            allocation_plan_.segments.push_back(NativeAllocatedSegment{segment, found->second});
        }
        for (const auto& expression_entry : expression_register_ids_) {
            const auto found = assignments.find(expression_entry.second);
            if (found != assignments.end()) expression_registers_.emplace(expression_entry.first, found->second);
        }
        stats_.native_register_values_analyzed = static_cast<std::uint32_t>(function_ir.values.size());
    }

    static std::string flow_binding_key(const std::string& name, const SourceSpan& span) {
        return fold_name(name) + "#" + std::to_string(span.source_id) + "#" +
            std::to_string(span.start.byte_offset);
    }

    void collect_address_taken_expression(const Expression& expression,
                                          std::set<std::string>& names) const {
        if (expression.kind == ExprKind::Unary && expression.text == "&" &&
            expression.children.size() == 1U &&
            expression.children.front()->kind == ExprKind::Identifier) {
            names.insert(fold_name(expression.children.front()->text));
        }
        for (const auto& child : expression.children)
            if (child) collect_address_taken_expression(*child, names);
    }

    void collect_address_taken_statement(const Statement& statement,
                                         std::set<std::string>& names) const {
        for (const auto& expression : statement.expressions)
            if (expression) collect_address_taken_expression(*expression, names);
        for (const auto& child : statement.statements)
            if (child) collect_address_taken_statement(*child, names);
        if (statement.else_branch) collect_address_taken_statement(*statement.else_branch, names);
    }

    void merge_persistent_callee_saves() {
        auto merge = [](std::vector<std::uint8_t>& target, const std::vector<std::uint8_t>& source) {
            target.insert(target.end(), source.begin(), source.end());
            std::sort(target.begin(), target.end());
            target.erase(std::unique(target.begin(), target.end()), target.end());
        };
        merge(allocation_plan_.used_callee_saved_gp, persistent_register_allocation_.used_callee_saved_gp);
        merge(allocation_plan_.used_callee_saved_xmm, persistent_register_allocation_.used_callee_saved_xmm);
    }

    static FlowAnchorKey flow_anchor_key(NativeRegisterInstructionOriginKind kind,
                                          const SourceSpan& span) {
        return FlowAnchorKey{span.source_id, span.start.byte_offset, span.end.byte_offset,
            static_cast<unsigned int>(kind)};
    }

    bool prepare_flow_emitter_anchor_lookup() {
        if (flow_function_ == nullptr) return false;
        for (const auto& block : flow_function_->register_ir.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic) continue;
                flow_emitter_anchor_candidates_[flow_anchor_key(
                    instruction.origin_kind, instruction.source_span)].push_back(
                        FlowAnchorCandidate{&instruction, false});
            }
        }
        return true;
    }

    FlowControlStatementId flow_control_statement_id(const Statement& statement) const {
        const auto found = control_statement_ids_.find(&statement);
        return found == control_statement_ids_.end()
            ? InvalidFlowControlStatementId : found->second;
    }

    bool prepare_flow_branch_anchor_lookup() {
        if (flow_function_ == nullptr || function_.body == nullptr) return false;
        const auto identities = enumerate_flow_control_statements(*function_.body);
        if (identities.size() != flow_function_->control_statements.size()) {
            add_diag(*diagnostics_, "XN109AC0001",
                "control-statement identities differ between the CFG and native emitter", function_.span);
            return false;
        }
        control_statement_ids_.clear();
        for (std::size_t index = 0U; index < identities.size(); ++index) {
            const auto& identity = identities[index];
            const auto& lowered = flow_function_->control_statements[index];
            if (identity.statement == nullptr || identity.id != lowered.id ||
                identity.statement->kind != lowered.kind ||
                identity.parent_construct_id != lowered.parent_construct_id ||
                identity.statement->span.start.byte_offset != lowered.span.start.byte_offset ||
                identity.statement->span.end.byte_offset != lowered.span.end.byte_offset) {
                add_diag(*diagnostics_, "XN109AC0002",
                    "control-statement identity or source metadata is not canonical", function_.span);
                return false;
            }
            control_statement_ids_.emplace(identity.statement, identity.id);
        }
        flow_branch_edge_candidates_.clear();
        for (const auto& edge : flow_function_->edges) {
            switch (edge.kind) {
            case FlowEdgeKind::IfTrue:
            case FlowEdgeKind::IfFalse:
            case FlowEdgeKind::IfThenMerge:
            case FlowEdgeKind::IfElseMerge:
            case FlowEdgeKind::LoopEntry:
            case FlowEdgeKind::LoopBody:
            case FlowEdgeKind::LoopExit:
            case FlowEdgeKind::LoopBack:
            case FlowEdgeKind::LoopBreak:
            case FlowEdgeKind::LoopContinue:
                if (edge.control_statement_id == InvalidFlowControlStatementId ||
                    edge.control_construct_id == InvalidFlowControlConstructId) {
                    add_diag(*diagnostics_, "XN109AC0003",
                        "control-flow edge is missing its stable statement/construct identity", edge.span);
                    return false;
                }
                flow_branch_edge_candidates_[{edge.control_statement_id, edge.kind}].push_back(&edge);
                break;
            case FlowEdgeKind::Sequential:
            case FlowEdgeKind::Return:
            case FlowEdgeKind::ForEachEntry:
            case FlowEdgeKind::ForEachBody:
            case FlowEdgeKind::ForEachExit:
            case FlowEdgeKind::ForEachBack:
                break;
            }
        }
        for (const auto& candidate : flow_branch_edge_candidates_) {
            if (candidate.second.size() != 1U || candidate.second.front() == nullptr) {
                add_diag(*diagnostics_, "XN109AC0004",
                    "a control statement has an ambiguous CFG edge identity", function_.span);
                return false;
            }
        }
        return true;
    }

    bool record_flow_branch_anchor(FlowControlStatementId control_statement_id,
                                   FlowEdgeKind edge_kind,
                                   X64FlowBranchEncoding encoding,
                                   bool conditional_taken,
                                   std::size_t instruction_offset,
                                   std::size_t displacement_offset,
                                   std::size_t target_offset,
                                   bool required = false) {
        if (!diagnostics_->empty()) return false;
        const auto found = flow_branch_edge_candidates_.find({control_statement_id, edge_kind});
        if (found == flow_branch_edge_candidates_.end()) {
            if (required) {
                add_diag(*diagnostics_, "XN109AC0005",
                    "required CFG control edge has no machine branch anchor", function_.span);
                return false;
            }
            return true; // Constant-condition CFG pruning may omit one path.
        }
        if (found->second.size() != 1U || found->second.front() == nullptr) {
            add_diag(*diagnostics_, "XN109AC0006",
                "CFG control edge lookup is ambiguous", function_.span);
            return false;
        }
        const FlowEdge& edge = *found->second.front();
        if (!recorded_flow_edge_ids_.insert(edge.id).second) {
            add_diag(*diagnostics_, "XN109AC0007",
                "CFG control edge was anchored more than once", edge.span);
            return false;
        }
        if (instruction_offset > std::numeric_limits<std::uint32_t>::max() ||
            target_offset > std::numeric_limits<std::uint32_t>::max() ||
            (encoding != X64FlowBranchEncoding::Fallthrough &&
             displacement_offset > std::numeric_limits<std::uint32_t>::max())) {
            add_diag(*diagnostics_, "XN109AC0008",
                "CFG branch anchor exceeds the N109A 32-bit code-offset format", edge.span);
            return false;
        }
        emitted_flow_branch_anchors_.push_back(X64FlowBranchAnchor{
            function_index_, edge.id, edge.control_statement_id, edge.control_construct_id,
            edge.from_block_id, edge.to_block_id, edge.kind, encoding, conditional_taken,
            static_cast<std::uint32_t>(instruction_offset),
            encoding == X64FlowBranchEncoding::Fallthrough
                ? InvalidNativeFlowOperationIndex : static_cast<std::uint32_t>(displacement_offset),
            static_cast<std::uint32_t>(target_offset), 0U});
        return true;
    }

    bool finalize_flow_branch_anchors(std::vector<X64FlowBranchAnchor>& output) {
        if (code_.size() > std::numeric_limits<std::uint32_t>::max()) {
            add_diag(*diagnostics_, "XN109AC0009",
                "native function code size exceeds the N109A 32-bit anchor format", function_.span);
            return false;
        }
        for (const auto& candidate : flow_branch_edge_candidates_) {
            const FlowEdge* edge = candidate.second.empty() ? nullptr : candidate.second.front();
            if (edge != nullptr && recorded_flow_edge_ids_.find(edge->id) == recorded_flow_edge_ids_.end()) {
                add_diag(*diagnostics_, "XN109AC000A",
                    "CFG control edge was not mapped to a final machine-code boundary", edge->span);
                return false;
            }
        }
        const auto code_size = static_cast<std::uint32_t>(code_.size());
        for (auto& anchor : emitted_flow_branch_anchors_) {
            if (anchor.instruction_offset > code_size || anchor.target_offset > code_size ||
                (anchor.displacement_offset != InvalidNativeFlowOperationIndex &&
                 static_cast<std::uint64_t>(anchor.displacement_offset) + 4U > code_size)) {
                add_diag(*diagnostics_, "XN109AC000B",
                    "final CFG branch anchor lies outside the emitted function body", function_.span);
                return false;
            }
            anchor.function_code_size = code_size;
            output.push_back(anchor);
        }
        stats_.native_cfg_branch_anchors_emitted +=
            static_cast<std::uint32_t>(emitted_flow_branch_anchors_.size());
        return true;
    }

    void record_flow_emitter_anchor(NativeRegisterInstructionOriginKind kind,
                                    const SourceSpan& span,
                                    std::size_t machine_code_before_offset = std::numeric_limits<std::size_t>::max(),
                                    std::size_t machine_code_after_call_offset = std::numeric_limits<std::size_t>::max(),
                                    std::size_t machine_code_call_instruction_offset = std::numeric_limits<std::size_t>::max(),
                                    std::size_t local_operand_slot = std::numeric_limits<std::size_t>::max(),
                                    bool address_operand = false) {
        if (!diagnostics_->empty()) return;
        const auto found = flow_emitter_anchor_candidates_.find(flow_anchor_key(kind, span));
        if (found == flow_emitter_anchor_candidates_.end()) return;
        if (found->second.size() != 1U || found->second.front().instruction == nullptr) {
            add_diag(*diagnostics_, "XN108C0001",
                "Flow operation source anchor is ambiguous in the native emitter", span);
            return;
        }
        auto& candidate = found->second.front();
        const auto& instruction = *candidate.instruction;
        if (candidate.recorded || !recorded_flow_operation_ids_.insert(
                instruction.flow_operation_id).second) {
            add_diag(*diagnostics_, "XN108C0002",
                "Flow operation source anchor was emitted more than once", span);
            return;
        }
        if (code_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            add_diag(*diagnostics_, "XN108C0003",
                "native function code offset exceeds the N108 32-bit anchor format", span);
            return;
        }
        candidate.recorded = true;
        if ((machine_code_before_offset != std::numeric_limits<std::size_t>::max() &&
             machine_code_before_offset > code_.size()) ||
            (machine_code_after_call_offset != std::numeric_limits<std::size_t>::max() &&
             machine_code_after_call_offset > code_.size()) ||
            (machine_code_call_instruction_offset != std::numeric_limits<std::size_t>::max() &&
             machine_code_call_instruction_offset > code_.size())) {
            add_diag(*diagnostics_, "XN110BI0010", "Flow operation transfer boundary is outside emitted code", span);
            return;
        }
        X64FlowEmitterAnchor anchor{
            function_index_, instruction.flow_operation_id, instruction.flow_block_id,
            instruction.flow_operation_index, instruction.origin_kind, instruction.source_span,
            static_cast<std::uint32_t>(code_.size()),
            machine_code_before_offset == std::numeric_limits<std::size_t>::max()
                ? InvalidNativeFlowOperationIndex : static_cast<std::uint32_t>(machine_code_before_offset),
            machine_code_after_call_offset == std::numeric_limits<std::size_t>::max()
                ? InvalidNativeFlowOperationIndex : static_cast<std::uint32_t>(machine_code_after_call_offset),
            0U};
        if (machine_code_call_instruction_offset != std::numeric_limits<std::size_t>::max()) {
            if (machine_code_after_call_offset == std::numeric_limits<std::size_t>::max() ||
                machine_code_call_instruction_offset + 5U != machine_code_after_call_offset ||
                machine_code_call_instruction_offset + 5U > code_.size() ||
                code_[machine_code_call_instruction_offset] != 0xE8U) {
                add_diag(*diagnostics_, "XN113E0001",
                    "FlowCall anchor does not identify the emitted direct CALL rel32 instruction", span);
                return;
            }
            const std::size_t relocation_offset = machine_code_call_instruction_offset + 1U;
            const auto relocation = std::find_if(relocations_.begin(), relocations_.end(),
                [relocation_offset](const NativeObjectRelocationInput& item) {
                    return item.offset == relocation_offset && item.width == 4U &&
                           item.kind == ObjectRelocationKind::CoffRelative32;
                });
            if (relocation == relocations_.end()) {
                add_diag(*diagnostics_, "XN113E0002",
                    "FlowCall instruction has no matching COFF rel32 relocation", span);
                return;
            }
            anchor.machine_code_call_instruction_offset =
                static_cast<std::uint32_t>(machine_code_call_instruction_offset);
        }
        if (local_operand_slot != std::numeric_limits<std::size_t>::max()) {
            const std::vector<std::uint32_t>* value_ids = nullptr;
            X64FlowEmitterAnchor::OperandKind operand_kind = X64FlowEmitterAnchor::OperandKind::None;
            switch (kind) {
            case NativeRegisterInstructionOriginKind::FlowRead:
                value_ids = &instruction.uses;
                operand_kind = address_operand
                    ? X64FlowEmitterAnchor::OperandKind::AddressRead
                    : X64FlowEmitterAnchor::OperandKind::ValueRead;
                break;
            case NativeRegisterInstructionOriginKind::FlowWrite:
                value_ids = &instruction.definitions;
                operand_kind = X64FlowEmitterAnchor::OperandKind::ValueWrite;
                break;
            case NativeRegisterInstructionOriginKind::FlowDeclare:
                value_ids = &instruction.definitions;
                operand_kind = X64FlowEmitterAnchor::OperandKind::ValueDeclare;
                break;
            default:
                add_diag(*diagnostics_, "XN113E0003",
                    "emitter supplied a local operand for an unsupported Flow operation kind", span);
                return;
            }
            if (value_ids->size() == 1U) {
                const std::uint32_t value_id = value_ids->front();
                const auto home = flow_value_home_locations_.find(value_id);
                const auto machine_value = flow_machine_location_plan_.values.find(value_id);
                if (home == flow_value_home_locations_.end() ||
                    machine_value == flow_machine_location_plan_.values.end() ||
                    home->second.kind != NativeRegisterLocationKind::HomeSlot ||
                    home->second.spill_slot != local_operand_slot) {
                    add_diag(*diagnostics_, "XN113E0004",
                        "emitted Flow operand does not resolve to its bound source local home", span);
                    return;
                }
                NativeRegisterLocation emitted_location = home->second;
                if (!address_operand) {
                    const auto active_register = local_register_locations_.find(local_operand_slot);
                    if (active_register != local_register_locations_.end())
                        emitted_location = active_register->second;
                }
                const auto& active = machine_value->second.active_location;
                const bool same = emitted_location.kind == active.kind &&
                    ((active.kind == NativeRegisterLocationKind::Register &&
                      emitted_location.register_index == active.register_index) ||
                     (active.kind == NativeRegisterLocationKind::HomeSlot &&
                      emitted_location.spill_slot == active.spill_slot));
                if (!same || (address_operand && !machine_value->second.address_taken)) {
                    add_diag(*diagnostics_, "XN113E0005",
                        "recorded Flow operand is inconsistent with the active N111 Machine Location", span);
                    return;
                }
                anchor.emitted_operand_kind = operand_kind;
                anchor.emitted_operand_value_id = value_id;
                anchor.emitted_operand_location = emitted_location;
            } else if (!value_ids->empty()) {
                add_diag(*diagnostics_, "XN113E0006",
                    "source Flow operation has an unexpected number of emitter operands", span);
                return;
            }
        }
        NativeX64ExpressionRegisterLiveness expression_snapshot;
        expression_snapshot.gp_registers = active_expression_gp_registers_;
        expression_snapshot.xmm_registers = active_expression_xmm_registers_;
        if (!expression_liveness_by_operation_.emplace(
                instruction.flow_operation_id, std::move(expression_snapshot)).second) {
            add_diag(*diagnostics_, "XN113G0025",
                "expression-register liveness snapshot was recorded more than once for a Flow operation", span);
            return;
        }
        emitted_flow_anchors_.push_back(std::move(anchor));
    }

    bool finalize_flow_emitter_anchors(std::vector<X64FlowEmitterAnchor>& output) {
        if (code_.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            add_diag(*diagnostics_, "XN108C0004",
                "native function code size exceeds the N108 32-bit anchor format", function_.span);
            return false;
        }
        for (const auto& item : flow_emitter_anchor_candidates_) {
            for (const auto& candidate : item.second) {
                if (!candidate.recorded) {
                    const auto* instruction = candidate.instruction;
                    const std::string detail = instruction == nullptr ? "unknown operation" :
                        "Flow operation ID " + std::to_string(instruction->flow_operation_id) +
                        " has no native-emitter boundary";
                    add_diag(*diagnostics_, "XN108C0005", detail, instruction == nullptr
                        ? function_.span : instruction->source_span);
                    return false;
                }
            }
        }
        const auto code_size = static_cast<std::uint32_t>(code_.size());
        for (auto& anchor : emitted_flow_anchors_) {
            if (anchor.machine_code_offset > code_size ||
                (anchor.machine_code_before_offset != InvalidNativeFlowOperationIndex &&
                 anchor.machine_code_before_offset > code_size) ||
                (anchor.machine_code_after_call_offset != InvalidNativeFlowOperationIndex &&
                 anchor.machine_code_after_call_offset > code_size)) {
                add_diag(*diagnostics_, "XN108C0006",
                    "Flow emitter anchor lies beyond the final function code", anchor.source_span);
                return false;
            }
            anchor.function_code_size = code_size;
            output.push_back(anchor);
        }
        stats_.native_flow_emitter_anchors_emitted +=
            static_cast<std::uint32_t>(emitted_flow_anchors_.size());
        return true;
    }

    bool prepare_flow_binding_lookup() {
        if (flow_function_ == nullptr || flow_allocation_ == nullptr) return false;
        flow_home_plan_ = build_native_register_variable_home_plan(*flow_function_);
        if (!flow_home_plan_.succeeded) {
            const std::string detail = flow_home_plan_.diagnostics.empty() ? "invalid flow home plan" :
                flow_home_plan_.diagnostics.front().code + " " + flow_home_plan_.diagnostics.front().message;
            add_diag(*diagnostics_, "XN90C0002", "persistent variable home planning failed: " + detail, function_.span);
            return false;
        }
        for (const auto& binding : flow_function_->bindings) {
            const auto inserted = flow_value_by_binding_key_.emplace(
                flow_binding_key(binding.name, binding.declaration_span), binding.native_value_id);
            if (!inserted.second) {
                add_diag(*diagnostics_, "XN90C0003", "duplicate source binding key in CFG home plan", binding.declaration_span);
                return false;
            }
        }

        // A variable whose address escapes is kept memory-resident: calls or
        // indirect stores may mutate its canonical stack home behind our back.
        std::set<std::string> address_taken_names;
        if (function_.body) collect_address_taken_statement(*function_.body, address_taken_names);
        std::set<std::uint32_t> address_taken_value_ids;
        for (const auto& binding : flow_function_->bindings) {
            if (address_taken_names.find(fold_name(binding.name)) != address_taken_names.end())
                address_taken_value_ids.insert(binding.native_value_id);
        }
        std::vector<NativeLiveInterval> eligible_intervals;
        for (const auto& interval : flow_allocation_->intervals.intervals) {
            const auto binding = std::find_if(flow_function_->bindings.begin(), flow_function_->bindings.end(),
                [&interval](const NativeRegisterFlowValueBinding& item) {
                    return item.native_value_id == interval.value_id;
                });
            if (binding == flow_function_->bindings.end()) continue;
            if (address_taken_names.find(fold_name(binding->name)) != address_taken_names.end()) {
                ++stats_.native_address_taken_variables_kept_in_memory;
                continue;
            }
            eligible_intervals.push_back(interval);
        }

        // Keep the expression allocator's register reservations separate
        // from the final prologue save set. The latter is extended below with
        // the persistent source-binding assignments and must not be fed back
        // as an expression-only exclusion set to the reconciliation pass.
        const auto expression_reserved_gp = allocation_plan_.used_callee_saved_gp;
        const auto expression_reserved_xmm = allocation_plan_.used_callee_saved_xmm;
        NativeRegisterAllocationOptions persistent_options;
        persistent_options.prefer_caller_saved = false;
        persistent_options.spill_call_crossing = false;
        persistent_options.reserve_xmm0 = true;
        persistent_options.use_callee_saved_fallback = false;
        // Expression temporaries have already been allocated. Never let a
        // persistent source value alias one of their nonvolatile registers.
        persistent_options.reserved_gp_registers = expression_reserved_gp;
        persistent_options.reserved_xmm_registers = expression_reserved_xmm;
        if (!eligible_intervals.empty()) {
            const auto interference = build_native_register_interference_graph(
                flow_function_->register_ir, flow_allocation_->liveness);
            if (!interference.succeeded) {
                const std::string detail = interference.diagnostics.empty() ? "interference graph construction failed" :
                    interference.diagnostics.front().code + " " + interference.diagnostics.front().message;
                add_diag(*diagnostics_, "XN102C0001", "CFG interference analysis failed: " + detail, function_.span);
                return false;
            }
            const auto allocated = allocate_native_registers_with_interference(
                eligible_intervals, interference.edges, persistent_options);
            if (!allocated.succeeded || allocated.assignments.size() != eligible_intervals.size()) {
                add_diag(*diagnostics_, "XN96C0001", "persistent source-variable register allocation failed", function_.span);
                return false;
            }
            persistent_register_allocation_ = allocated;
            std::uint32_t next_segment_id = 1U;
            for (const auto& existing : allocation_plan_.segments)
                next_segment_id = std::max(next_segment_id, existing.segment.segment_id + 1U);
            for (const auto& assignment : allocated.assignments) {
                if (assignment.spilled) {
                    ++stats_.native_persistent_register_stack_fallbacks;
                } else {
                    const auto interval = std::find_if(eligible_intervals.begin(), eligible_intervals.end(),
                        [&assignment](const NativeLiveInterval& candidate) {
                            return candidate.value_id == assignment.value_id;
                        });
                    if (interval == eligible_intervals.end()) {
                        add_diag(*diagnostics_, "XN96C0002", "persistent register assignment has no source interval", function_.span);
                        return false;
                    }
                    NativeRegisterLiveSegment segment;
                    segment.segment_id = next_segment_id++;
                    segment.value_id = assignment.value_id;
                    segment.block_id = InvalidNativeRegisterBlockId;
                    segment.start = interval->start;
                    segment.end = interval->end;
                    segment.register_class = assignment.register_class;
                    allocation_plan_.segments.push_back(NativeAllocatedSegment{segment, assignment});
                    if (assignment.register_class == NativeRegisterClass::GeneralPurpose)
                        ++stats_.native_persistent_gp_register_assignments;
                    else
                        ++stats_.native_persistent_xmm_register_assignments;
                }
            }
            merge_persistent_callee_saves();
        } else {
            persistent_register_allocation_.succeeded = true;
        }
        flow_machine_location_plan_ = build_native_x64_machine_location_plan(
            *flow_function_, flow_allocation_->liveness, persistent_register_allocation_,
            address_taken_value_ids, expression_reserved_gp, expression_reserved_xmm);
        if (!flow_machine_location_plan_.succeeded) {
            const std::string detail = flow_machine_location_plan_.diagnostics.empty()
                ? "unknown machine-location reconciliation failure"
                : flow_machine_location_plan_.diagnostics.front().code + " " +
                  flow_machine_location_plan_.diagnostics.front().message;
            add_diag(*diagnostics_, "XN111C0001",
                "authoritative Flow machine-location plan failed: " + detail, function_.span);
            return false;
        }
        stats_.native_machine_location_plans_built += 1U;
        stats_.native_machine_location_register_values += flow_machine_location_plan_.register_resident_values;
        stats_.native_machine_location_home_values += flow_machine_location_plan_.home_resident_values;
        stats_.native_machine_location_address_taken_values += flow_machine_location_plan_.address_taken_values;
        return true;
    }

    bool prepare_n113_flow_action_plan() {
        n113_flow_segment_plan_ = build_native_register_flow_segment_plan(
            flow_function_->register_ir, flow_allocation_->liveness, flow_allocation_->intervals,
            flow_allocation_->segments, flow_allocation_->allocation);
        if (!n113_flow_segment_plan_.succeeded) {
            const std::string detail = n113_flow_segment_plan_.diagnostics.empty()
                ? "unknown N112 Flow segment-plan failure"
                : n113_flow_segment_plan_.diagnostics.front().code + " " +
                  n113_flow_segment_plan_.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113C0001",
                "N113 could not build the Flow Segment/Operation plan: " + detail, function_.span);
            return false;
        }

        std::vector<std::uint8_t> expression_gp;
        std::vector<std::uint8_t> expression_xmm;
        for (const auto& item : expression_registers_) {
            const auto& assignment = item.second;
            if (assignment.spilled) continue;
            (assignment.register_class == NativeRegisterClass::GeneralPurpose
                ? expression_gp : expression_xmm).push_back(assignment.register_index);
        }
        const auto canonicalize = [](std::vector<std::uint8_t>& registers) {
            std::sort(registers.begin(), registers.end());
            registers.erase(std::unique(registers.begin(), registers.end()), registers.end());
        };
        canonicalize(expression_gp);
        canonicalize(expression_xmm);
        n113_flow_action_resolution_ = resolve_native_flow_segment_spill_reload_actions(
            flow_function_->register_ir, n113_flow_segment_plan_, expression_gp, expression_xmm);
        if (!n113_flow_action_resolution_.succeeded) {
            const std::string detail = n113_flow_action_resolution_.diagnostics.empty()
                ? "unknown N113 action-resolution failure"
                : n113_flow_action_resolution_.diagnostics.front().code + " " +
                  n113_flow_action_resolution_.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113C0002",
                "N113 could not resolve the Flow spill/reload plan: " + detail, function_.span);
            return false;
        }
        if (n113_flow_action_resolution_.bindings.size() != n113_flow_segment_plan_.actions.size()) {
            add_diag(*diagnostics_, "XN113C0003",
                "N113 action resolution did not retain every planned action exactly once", function_.span);
            return false;
        }
        ++stats_.native_n113_flow_segment_plans_validated;
        stats_.native_n113_flow_actions_resolved +=
            static_cast<std::uint32_t>(n113_flow_action_resolution_.bindings.size());
        return true;
    }

    bool build_n113_emitter_action_rebinding_plan() {
        const std::uint64_t local_bytes_64 = static_cast<std::uint64_t>(next_slot_) * 8U;
        if (local_bytes_64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) ||
            next_slot_ > std::numeric_limits<std::uint32_t>::max()) {
            add_diag(*diagnostics_, "XN113G0001",
                "G1 lexical local-home area exceeds the 32-bit frame-location format", function_.span);
            return false;
        }
        const auto emitter_frame = build_native_win64_frame_plan(
            allocation_plan_, static_cast<std::uint32_t>(local_bytes_64));
        if (!emitter_frame.succeeded) {
            const std::string detail = emitter_frame.diagnostics.empty()
                ? "unknown emitter frame planning failure"
                : emitter_frame.diagnostics.front().code + " " + emitter_frame.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113G0002",
                "G1 could not construct the unchanged emitter frame map: " + detail, function_.span);
            return false;
        }
        const auto flow_frame = plan_native_x64_flow_frame_binding(
            allocation_plan_, flow_allocation_->allocation,
            static_cast<std::uint32_t>(local_bytes_64));
        if (!flow_frame.succeeded) {
            const std::string detail = flow_frame.diagnostics.empty()
                ? "unknown Flow frame map planning failure"
                : flow_frame.diagnostics.front().code + " " + flow_frame.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113G0003",
                "G1 could not map N112 Flow frame slots without changing the active frame: " + detail,
                function_.span);
            return false;
        }

        NativeX64FrameLocationMap emitter_locations;
        for (std::uint32_t slot = 0U; slot < emitter_frame.spill_slot_offsets.size(); ++slot)
            emitter_locations.spill_slot_offsets.emplace(slot, emitter_frame.spill_slot_offsets[slot]);
        for (std::uint32_t slot = 0U; slot < emitter_frame.home_slot_offsets.size(); ++slot)
            emitter_locations.home_slot_offsets.emplace(slot, emitter_frame.home_slot_offsets[slot]);

        std::map<std::uint32_t, std::int32_t> local_home_offsets;
        for (std::uint32_t slot = 0U; slot < static_cast<std::uint32_t>(next_slot_); ++slot)
            local_home_offsets.emplace(slot, local_displacement(slot));

        std::vector<std::uint8_t> expression_gp;
        std::vector<std::uint8_t> expression_xmm;
        for (const auto& item : expression_registers_) {
            const auto& assignment = item.second;
            if (assignment.spilled) continue;
            (assignment.register_class == NativeRegisterClass::GeneralPurpose
                ? expression_gp : expression_xmm).push_back(assignment.register_index);
        }
        const auto canonicalize = [](std::vector<std::uint8_t>& registers) {
            std::sort(registers.begin(), registers.end());
            registers.erase(std::unique(registers.begin(), registers.end()), registers.end());
        };
        canonicalize(expression_gp);
        canonicalize(expression_xmm);

        n113_emitter_action_rebinding_ = rebind_native_x64_flow_actions_to_emitter_locations(
            flow_function_->register_ir, n113_flow_segment_plan_,
            n113_flow_action_resolution_.bindings, flow_machine_location_plan_,
            emitted_flow_anchors_, emitter_locations, local_home_offsets,
            flow_frame.flow_frame_locations, allocation_plan_.spill_slot_count,
            allocation_plan_.home_slot_count, static_cast<std::uint32_t>(next_slot_),
            expression_gp, expression_xmm, expression_liveness_by_operation_);
        if (!n113_emitter_action_rebinding_.succeeded) {
            const std::string detail = n113_emitter_action_rebinding_.diagnostics.empty()
                ? "unknown emitter action-rebinding failure"
                : n113_emitter_action_rebinding_.diagnostics.front().code + " " +
                  n113_emitter_action_rebinding_.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113G0004",
                "G1 rejected the N112-to-N111 emitter action rebinding plan: " + detail, function_.span);
            return false;
        }
        if (n113_emitter_action_rebinding_.rebound_actions !=
                n113_flow_action_resolution_.bindings.size() ||
            n113_emitter_action_rebinding_.bindings.size() != n113_flow_segment_plan_.actions.size()) {
            add_diag(*diagnostics_, "XN113G0005",
                "G1 did not preserve every N112 action exactly once after emitter rebinding", function_.span);
            return false;
        }
        stats_.native_n113_emitter_actions_rebound += n113_emitter_action_rebinding_.rebound_actions;
        stats_.native_n113_emitter_memory_to_memory_actions +=
            n113_emitter_action_rebinding_.memory_to_memory_actions;
        stats_.native_n113_emitter_flow_spill_slots_rebased +=
            n113_emitter_action_rebinding_.flow_spill_slots_rebased;
        stats_.native_n113_emitter_flow_home_slots_rebased +=
            n113_emitter_action_rebinding_.flow_home_slots_rebased;
        stats_.native_n113_emitter_gp_scratch_required +=
            n113_emitter_action_rebinding_.gp_cycle_temporary_required ? 1U : 0U;
        stats_.native_n113_emitter_xmm_scratch_required +=
            n113_emitter_action_rebinding_.xmm_cycle_temporary_required ? 1U : 0U;
        return true;
    }

    bool audit_n113_flow_edge_location_crosswalk(std::uint32_t local_area_bytes) {
        if (flow_function_ == nullptr || flow_allocation_ == nullptr ||
            !flow_allocation_->succeeded || !n113_flow_segment_plan_.succeeded ||
            !flow_machine_location_plan_.succeeded) {
            add_diag(*diagnostics_, "XN113G5001",
                "G5 requires successful N112 segment and authoritative N111 location plans", function_.span);
            return false;
        }

        // Build a separate Flow-frame map for the audit if the caller did not
        // opt into actually reserving those slots. This hypothetical map is
        // never assigned to frame_plan_ and therefore cannot alter codegen.
        NativeX64FlowFrameBindingPlan audit_frame = n113_flow_frame_binding_;
        if (!audit_frame.succeeded) {
            audit_frame = plan_native_x64_flow_frame_binding(
                allocation_plan_, flow_allocation_->allocation, local_area_bytes);
        }
        if (!audit_frame.succeeded) {
            const std::string detail = audit_frame.diagnostics.empty()
                ? "unknown hypothetical Flow-frame map failure"
                : audit_frame.diagnostics.front().code + " " + audit_frame.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113G5002",
                "G5 could not create a read-only Flow-frame map: " + detail, function_.span);
            return false;
        }

        const auto home_phi = build_native_register_home_phi_plan(
            *flow_function_, flow_allocation_->liveness, flow_home_plan_);
        if (!home_phi.succeeded) {
            const std::string detail = home_phi.diagnostics.empty()
                ? "unknown Home-Phi plan failure"
                : home_phi.diagnostics.front().code + " " + home_phi.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113G5003",
                "G5 could not validate the FunctionEmitter Home-Phi aliases: " + detail, function_.span);
            return false;
        }

        const auto audit = audit_native_x64_flow_edge_locations(
            *flow_function_, flow_allocation_->liveness, flow_allocation_->intervals,
            n113_flow_segment_plan_, home_phi, flow_machine_location_plan_,
            audit_frame.flow_frame_locations);
        if (!audit.succeeded) {
            const std::string detail = audit.diagnostics.empty()
                ? "unknown CFG Edge Location Audit failure"
                : audit.diagnostics.front().code + " " + audit.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113G5004",
                "G5 rejected the real FunctionEmitter CFG edge crosswalk: " + detail, function_.span);
            return false;
        }

        n113_flow_edge_frame_locations_ = audit_frame.flow_frame_locations;
        n113_flow_edge_location_audit_ = audit;
        ++stats_.native_n113_flow_edge_audit_plans_built;
        stats_.native_n113_flow_edges_audited += audit.edges_audited;
        stats_.native_n113_flow_edge_live_values_audited += audit.live_values_audited;
        stats_.native_n113_flow_edge_location_changes += audit.flow_location_changes;
        stats_.native_n113_flow_edge_locations_coalesced += audit.flow_locations_coalesced;
        stats_.native_n113_flow_home_phi_nodes_audited += audit.home_phi_nodes_audited;
        stats_.native_n113_flow_home_phi_incoming_edges_audited += audit.home_phi_incoming_edges_audited;
        return true;
    }

    bool audit_n113_flow_edge_schedule_readiness_plan(
        const std::vector<X64FlowEmitterAnchor>& flow_anchors,
        const std::vector<X64FlowBranchAnchor>& flow_branch_anchors) {
        if (flow_function_ == nullptr || flow_allocation_ == nullptr ||
            !flow_allocation_->succeeded || !n113_flow_edge_location_audit_.succeeded ||
            !flow_machine_location_plan_.succeeded) {
            add_diag(*diagnostics_, "XN113G8001",
                "G8 requires the real FunctionEmitter G4 edge audit and authoritative N111 locations", function_.span);
            return false;
        }

        std::map<std::uint32_t, NativeRegisterClass> value_classes;
        for (const auto& value : flow_function_->register_ir.values) {
            if (value.value_id == 0U || !value_classes.emplace(value.value_id, value.register_class).second) {
                add_diag(*diagnostics_, "XN113G8002",
                    "G8 FunctionEmitter value-class table contains an invalid or duplicate ValueId", function_.span);
                return false;
            }
        }

        std::vector<X64FlowEmitterAnchor> function_operation_anchors;
        for (const auto& anchor : flow_anchors) {
            if (anchor.function_index == function_index_) function_operation_anchors.push_back(anchor);
        }
        std::vector<X64FlowBranchAnchor> function_branch_anchors;
        for (const auto& anchor : flow_branch_anchors) {
            if (anchor.function_index == function_index_) function_branch_anchors.push_back(anchor);
        }

        const auto candidates = plan_native_x64_flow_edge_transfer_candidates(
            n113_flow_edge_location_audit_, function_branch_anchors, value_classes);
        if (!candidates.succeeded) {
            const std::string detail = candidates.diagnostics.empty()
                ? "unknown G6 edge-candidate planning failure"
                : candidates.diagnostics.front().code + " " + candidates.diagnostics.front().message;
            n113_flow_edge_readiness_.succeeded = false;
            n113_flow_edge_readiness_.machine_insertion_ready = false;
            n113_flow_edge_readiness_.diagnostics.push_back(
                NativeRegisterIRDiagnostic{"XN113G8003",
                    "G8 audit could not build exact Flow-edge transfer candidates: " + detail});
            ++stats_.native_n113_flow_edge_schedule_plans_built;
            return true; // Read-only proof failure must never invalidate normal code emission.
        }

        std::vector<std::uint8_t> expression_gp;
        std::vector<std::uint8_t> expression_xmm;
        for (const auto& item : expression_registers_) {
            const auto& assignment = item.second;
            if (assignment.spilled) continue;
            (assignment.register_class == NativeRegisterClass::GeneralPurpose
                ? expression_gp : expression_xmm).push_back(assignment.register_index);
        }
        const auto canonicalize = [](std::vector<std::uint8_t>& registers) {
            std::sort(registers.begin(), registers.end());
            registers.erase(std::unique(registers.begin(), registers.end()), registers.end());
        };
        canonicalize(expression_gp);
        canonicalize(expression_xmm);

        std::map<std::uint32_t, std::int32_t> emitter_local_home_offsets;
        for (std::uint32_t slot = 0U; slot < static_cast<std::uint32_t>(next_slot_); ++slot)
            emitter_local_home_offsets.emplace(slot, local_displacement(slot));

        NativeX64FlowEdgeTransferSchedulePlan selected_schedule;
        NativeX64FlowEdgeScheduleReadinessPlan selected_proof;
        bool have_schedule = false;
        bool have_scratch_proof = false;
        // G7 used one class-specific temporary for the schedule. G8 tests every
        // permitted pair and prefers the first pair proven dead on every edge.
        for (std::uint8_t gp = 8U; gp <= 11U && !have_scratch_proof; ++gp) {
            for (std::uint8_t xmm = 1U; xmm <= 5U; ++xmm) {
                NativeRegisterLocation gp_temporary;
                gp_temporary.kind = NativeRegisterLocationKind::Temporary;
                gp_temporary.register_index = gp;
                NativeRegisterLocation xmm_temporary;
                xmm_temporary.kind = NativeRegisterLocationKind::Temporary;
                xmm_temporary.register_index = xmm;

                auto schedule = schedule_native_x64_flow_edge_transfer_candidates(
                    candidates, value_classes, gp_temporary, xmm_temporary);
                if (!schedule.succeeded) continue;
                auto proof = audit_native_x64_flow_edge_schedule_readiness(
                    *flow_function_, flow_allocation_->liveness, n113_flow_edge_location_audit_,
                    schedule, flow_machine_location_plan_, emitter_local_home_offsets,
                    n113_flow_edge_frame_locations_, function_operation_anchors, function_branch_anchors,
                    gp_temporary, xmm_temporary, expression_gp, expression_xmm);
                if (!proof.succeeded) continue;
                if (!have_schedule || (proof.scratch_liveness_proven && !have_scratch_proof)) {
                    selected_schedule = std::move(schedule);
                    selected_proof = std::move(proof);
                    have_schedule = true;
                    have_scratch_proof = selected_proof.scratch_liveness_proven;
                }
                if (have_scratch_proof) break;
            }
        }
        if (!have_schedule) {
            n113_flow_edge_readiness_.succeeded = false;
            n113_flow_edge_readiness_.machine_insertion_ready = false;
            n113_flow_edge_readiness_.scratch_liveness_proven = false;
            n113_flow_edge_readiness_.diagnostics.push_back(
                NativeRegisterIRDiagnostic{"XN113G8004",
                    "G8 read-only audit could not find one globally safe G7 scratch pair; per-edge scratch selection remains an open requirement"});
            ++stats_.native_n113_flow_edge_schedule_plans_built;
            ++stats_.native_n113_flow_edge_scratch_schedule_unavailable;
            return true; // Never turn an audit-only blocker into a codegen failure.
        }

        n113_flow_edge_readiness_ = selected_proof;
        ++stats_.native_n113_flow_edge_schedule_plans_built;
        stats_.native_n113_flow_edge_schedules_audited += selected_proof.scheduled_edges_audited;
        stats_.native_n113_flow_edge_operand_anchors_validated += selected_proof.operand_anchors_validated;
        stats_.native_n113_flow_edge_endpoint_ownership_matches += selected_proof.endpoint_ownership_matches;
        stats_.native_n113_flow_edge_endpoint_ownership_mismatches += selected_proof.endpoint_ownership_mismatches;
        stats_.native_n113_flow_edge_gp_scratch_proofs += selected_proof.gp_scratch_registers_proven_dead;
        stats_.native_n113_flow_edge_xmm_scratch_proofs += selected_proof.xmm_scratch_registers_proven_dead;
        stats_.native_n113_flow_edge_scratch_conflicts += selected_proof.scratch_liveness_conflicts;
        stats_.native_n113_flow_edge_machine_ready_plans +=
            selected_proof.machine_insertion_ready ? 1U : 0U;
        return true;
    }

    bool plan_n113_flow_edge_scratch_per_edge(
        const std::vector<X64FlowEmitterAnchor>& flow_anchors,
        const std::vector<X64FlowBranchAnchor>& flow_branch_anchors) {
        if (flow_function_ == nullptr || flow_allocation_ == nullptr ||
            !flow_allocation_->succeeded || !n113_flow_edge_location_audit_.succeeded ||
            !flow_machine_location_plan_.succeeded) {
            n113_flow_edge_per_edge_scratch_plan_.diagnostics.push_back(
                NativeRegisterIRDiagnostic{"XN113G9001",
                    "G9 requires the real FunctionEmitter G4 edge audit and authoritative N111 locations"});
            ++stats_.native_n113_flow_edge_per_edge_plans_built;
            return true; // Read-only audit prerequisites must not disrupt normal emission.
        }

        std::map<std::uint32_t, NativeRegisterClass> value_classes;
        for (const auto& value : flow_function_->register_ir.values) {
            if (value.value_id == 0U || !value_classes.emplace(value.value_id, value.register_class).second) {
                n113_flow_edge_per_edge_scratch_plan_.diagnostics.push_back(
                    NativeRegisterIRDiagnostic{"XN113G9002",
                        "G9 FunctionEmitter value-class table contains an invalid or duplicate ValueId"});
                ++stats_.native_n113_flow_edge_per_edge_plans_built;
                return true;
            }
        }

        std::vector<X64FlowEmitterAnchor> function_operation_anchors;
        for (const auto& anchor : flow_anchors) {
            if (anchor.function_index == function_index_) function_operation_anchors.push_back(anchor);
        }
        std::vector<X64FlowBranchAnchor> function_branch_anchors;
        for (const auto& anchor : flow_branch_anchors) {
            if (anchor.function_index == function_index_) function_branch_anchors.push_back(anchor);
        }

        n113_flow_edge_transfer_candidates_ = plan_native_x64_flow_edge_transfer_candidates(
            n113_flow_edge_location_audit_, function_branch_anchors, value_classes);
        if (!n113_flow_edge_transfer_candidates_.succeeded) {
            const std::string detail = n113_flow_edge_transfer_candidates_.diagnostics.empty()
                ? "unknown G6 edge-candidate planning failure"
                : n113_flow_edge_transfer_candidates_.diagnostics.front().code + " " +
                    n113_flow_edge_transfer_candidates_.diagnostics.front().message;
            n113_flow_edge_per_edge_scratch_plan_.diagnostics.push_back(
                NativeRegisterIRDiagnostic{"XN113G9003",
                    "G9 could not construct exact per-edge candidates: " + detail});
            ++stats_.native_n113_flow_edge_per_edge_plans_built;
            return true;
        }

        std::vector<std::uint8_t> expression_gp;
        std::vector<std::uint8_t> expression_xmm;
        for (const auto& item : expression_registers_) {
            const auto& assignment = item.second;
            if (assignment.spilled) continue;
            (assignment.register_class == NativeRegisterClass::GeneralPurpose
                ? expression_gp : expression_xmm).push_back(assignment.register_index);
        }
        const auto canonicalize = [](std::vector<std::uint8_t>& registers) {
            std::sort(registers.begin(), registers.end());
            registers.erase(std::unique(registers.begin(), registers.end()), registers.end());
        };
        canonicalize(expression_gp);
        canonicalize(expression_xmm);

        std::map<std::uint32_t, std::int32_t> emitter_local_home_offsets;
        for (std::uint32_t slot = 0U; slot < static_cast<std::uint32_t>(next_slot_); ++slot)
            emitter_local_home_offsets.emplace(slot, local_displacement(slot));

        n113_flow_edge_per_edge_scratch_plan_ = plan_native_x64_flow_edge_schedules_per_edge(
            *flow_function_, flow_allocation_->liveness, n113_flow_edge_location_audit_,
            n113_flow_edge_transfer_candidates_, flow_machine_location_plan_, emitter_local_home_offsets,
            n113_flow_edge_frame_locations_, function_operation_anchors, function_branch_anchors,
            expression_gp, expression_xmm);
        ++stats_.native_n113_flow_edge_per_edge_plans_built;
        stats_.native_n113_flow_edge_per_edge_schedules_built +=
            n113_flow_edge_per_edge_scratch_plan_.scheduled_edges;
        stats_.native_n113_flow_edge_per_edge_scratch_proven +=
            n113_flow_edge_per_edge_scratch_plan_.scratch_proven_edges;
        stats_.native_n113_flow_edge_per_edge_scratch_blocked +=
            n113_flow_edge_per_edge_scratch_plan_.scratch_unavailable_edges;
        stats_.native_n113_flow_edge_per_edge_g7_rejections +=
            n113_flow_edge_per_edge_scratch_plan_.g7_schedule_rejections;
        stats_.native_n113_flow_edge_per_edge_proof_rejections +=
            n113_flow_edge_per_edge_scratch_plan_.readiness_proof_rejections;
        if (stats_.native_n113_flow_edge_per_edge_first_g7_rejection.empty())
            stats_.native_n113_flow_edge_per_edge_first_g7_rejection =
                n113_flow_edge_per_edge_scratch_plan_.first_g7_rejection;
        if (stats_.native_n113_flow_edge_per_edge_first_proof_rejection.empty())
            stats_.native_n113_flow_edge_per_edge_first_proof_rejection =
                n113_flow_edge_per_edge_scratch_plan_.first_readiness_proof_rejection;
        stats_.native_n113_flow_edge_per_edge_machine_ready_plans +=
            n113_flow_edge_per_edge_scratch_plan_.machine_insertion_ready ? 1U : 0U;
        return true; // G9 is an audit only; proof blockers are non-fatal.
    }

    bool audit_n113_flow_operation_operand_ownership_plan(
        const std::vector<X64FlowEmitterAnchor>& flow_anchors) {
        if (flow_function_ == nullptr || flow_allocation_ == nullptr ||
            !flow_allocation_->succeeded || !n113_flow_segment_plan_.succeeded ||
            !flow_machine_location_plan_.succeeded) {
            n113_flow_operation_operand_ownership_plan_.diagnostics.push_back(
                NativeRegisterIRDiagnostic{"XN113J9001",
                    "G10 requires the real FunctionEmitter's N112 operation plan, N111 locations and Flow frame map"});
            ++stats_.native_n113_flow_operation_operand_ownership_plans;
            return true; // Audit-only prerequisites must not affect normal emission.
        }

        std::vector<X64FlowEmitterAnchor> function_operation_anchors;
        for (const auto& anchor : flow_anchors) {
            if (anchor.function_index == function_index_) function_operation_anchors.push_back(anchor);
        }

        std::map<std::uint32_t, std::int32_t> emitter_local_home_offsets;
        for (std::uint32_t slot = 0U; slot < static_cast<std::uint32_t>(next_slot_); ++slot)
            emitter_local_home_offsets.emplace(slot, local_displacement(slot));

        n113_flow_operation_operand_ownership_plan_ = audit_native_x64_flow_operation_operand_ownership(
            *flow_function_, n113_flow_segment_plan_, flow_machine_location_plan_,
            emitter_local_home_offsets, n113_flow_edge_frame_locations_, function_index_,
            function_operation_anchors);
        ++stats_.native_n113_flow_operation_operand_ownership_plans;
        stats_.native_n113_flow_operation_operands_audited +=
            n113_flow_operation_operand_ownership_plan_.operand_operations_audited;
        stats_.native_n113_flow_operation_operand_matches +=
            n113_flow_operation_operand_ownership_plan_.operand_location_matches;
        stats_.native_n113_flow_operation_operand_mismatches +=
            n113_flow_operation_operand_ownership_plan_.operand_location_mismatches;
        stats_.native_n113_flow_operation_address_operands_audited +=
            n113_flow_operation_operand_ownership_plan_.address_operands_audited;
        // A structurally valid audit may report mismatches. They remain a
        // non-fatal blocker and never enable physical transfer insertion.
        return true;
    }

    void record_n113_flow_edge_bundle_function_report() {
        X64FlowEdgeBundleEligibilityFunctionReport report;
        report.function_index = function_index_;
        report.function_name = function_.name;
        report.plan_succeeded = n113_flow_edge_bundle_eligibility_plan_.succeeded;
        report.candidate_edges = n113_flow_edge_bundle_eligibility_plan_.candidate_edges;
        report.proof_gate_passed_edges = n113_flow_edge_bundle_eligibility_plan_.proof_gate_passed_edges;
        report.blocked_edges = n113_flow_edge_bundle_eligibility_plan_.blocked_edges;
        report.schedule_blocked_edges = n113_flow_edge_bundle_eligibility_plan_.schedule_blocked_edges;
        report.endpoint_ownership_blocked_edges =
            n113_flow_edge_bundle_eligibility_plan_.endpoint_ownership_blocked_edges;
        report.scratch_blocked_edges = n113_flow_edge_bundle_eligibility_plan_.scratch_blocked_edges;
        report.operation_operand_blocked_edges =
            n113_flow_edge_bundle_eligibility_plan_.operation_operand_blocked_edges;
        report.mismatch_counts = n113_flow_edge_bundle_eligibility_plan_.mismatch_counts;
        report.diagnostics = n113_flow_edge_bundle_eligibility_plan_.diagnostics;
        report.edges.reserve(n113_flow_edge_bundle_eligibility_plan_.edges.size());
        for (const auto& edge : n113_flow_edge_bundle_eligibility_plan_.edges) {
            X64FlowEdgeBundleEligibilityEdgeReport edge_report;
            edge_report.edge_id = edge.edge_id;
            edge_report.predecessor_block_id = edge.predecessor_block_id;
            edge_report.successor_block_id = edge.successor_block_id;
            edge_report.candidate_values = edge.candidate_values;
            edge_report.operation_values_proven = edge.operation_values_proven;
            edge_report.operation_values_with_evidence = edge.operation_values_with_evidence;
            edge_report.operation_values_mismatched = edge.operation_values_mismatched;
            edge_report.operation_values_missing = edge.operation_values_missing;
            edge_report.mismatch_counts = edge.mismatch_counts;
            edge_report.live_values_audited = edge.live_values_audited;
            edge_report.operand_anchors_validated = edge.operand_anchors_validated;
            edge_report.endpoint_ownership_matches = edge.endpoint_ownership_matches;
            edge_report.endpoint_ownership_mismatches = edge.endpoint_ownership_mismatches;
            edge_report.endpoint_value_evidence.reserve(edge.endpoint_value_evidence.size());
            for (const auto& endpoint : edge.endpoint_value_evidence) {
                X64FlowEdgeEndpointValueEvidenceReport endpoint_report;
                endpoint_report.value_id = endpoint.value_id;
                endpoint_report.register_class = endpoint.register_class;
                endpoint_report.predecessor_flow_location = endpoint.predecessor_flow_location;
                endpoint_report.successor_flow_location = endpoint.successor_flow_location;
                endpoint_report.emitter_active_location = endpoint.emitter_active_location;
                endpoint_report.predecessor_uses_frame = endpoint.predecessor_uses_frame;
                endpoint_report.predecessor_frame_displacement = endpoint.predecessor_frame_displacement;
                endpoint_report.successor_uses_frame = endpoint.successor_uses_frame;
                endpoint_report.successor_frame_displacement = endpoint.successor_frame_displacement;
                endpoint_report.emitter_location_is_home_slot = endpoint.emitter_location_is_home_slot;
                endpoint_report.emitter_frame_mapping_available = endpoint.emitter_frame_mapping_available;
                endpoint_report.emitter_frame_displacement = endpoint.emitter_frame_displacement;
                endpoint_report.predecessor_matches_emitter = endpoint.predecessor_matches_emitter;
                endpoint_report.successor_matches_emitter = endpoint.successor_matches_emitter;
                endpoint_report.function_operand_anchor_count = endpoint.function_operand_anchor_count;
                edge_report.endpoint_value_evidence.push_back(std::move(endpoint_report));
            }
            edge_report.value_operand_evidence.reserve(edge.value_operand_evidence.size());
            for (const auto& value : edge.value_operand_evidence) {
                X64FlowEdgeValueOperandEvidenceReport value_report;
                value_report.value_id = value.value_id;
                value_report.register_class = value.register_class;
                value_report.concrete_evidence_found = value.concrete_evidence_found;
                value_report.operand_operations = value.operand_operations;
                value_report.matching_operations = value.matching_operations;
                value_report.mismatching_operations = value.mismatching_operations;
                value_report.all_observed_operands_match = value.all_observed_operands_match;
                value_report.mismatch_counts = value.mismatch_counts;
                value_report.operations.reserve(value.operations.size());
                for (const auto& operation : value.operations) {
                    X64FlowEdgeBundleOperandEvidenceReport operation_report;
                    operation_report.flow_operation_id = operation.flow_operation_id;
                    operation_report.flow_block_id = operation.flow_block_id;
                    operation_report.flow_operation_index = operation.flow_operation_index;
                    operation_report.operand_kind = operation.operand_kind;
                    // N113-G17: preserve the actual N112 Flow class in the public
                    // FunctionEmitter census. G16 retained it in the internal G11
                    // plan, but the report projection previously copied only the
                    // authoritative candidate Value class.
                    operation_report.flow_register_class = operation.flow_register_class;
                    operation_report.candidate_value_register_class = operation.candidate_value_register_class;
                    operation_report.flow_origin_kind = operation.flow_origin_kind;
                    operation_report.flow_position_kind = operation.flow_position_kind;
                    operation_report.source_span = operation.source_span;
                    operation_report.source_span_matches_emitter_anchor =
                        operation.source_span_matches_emitter_anchor;
                    operation_report.flow_operation_location = operation.flow_operation_location;
                    operation_report.emitter_operand_location = operation.emitter_operand_location;
                    operation_report.flow_location_uses_frame = operation.flow_location_uses_frame;
                    operation_report.flow_frame_displacement = operation.flow_frame_displacement;
                    operation_report.emitter_location_uses_frame = operation.emitter_location_uses_frame;
                    operation_report.emitter_frame_displacement = operation.emitter_frame_displacement;
                    operation_report.location_matches = operation.location_matches;
                    operation_report.mismatch_kind = operation.mismatch_kind;
                    value_report.operations.push_back(std::move(operation_report));
                }
                edge_report.value_operand_evidence.push_back(std::move(value_report));
            }
            edge_report.exact_g9_schedule = edge.exact_g9_schedule;
            edge_report.endpoint_ownership_proven = edge.endpoint_ownership_proven;
            edge_report.scratch_liveness_proven = edge.scratch_liveness_proven;
            edge_report.operation_operand_ownership_proven = edge.operation_operand_ownership_proven;
            edge_report.proof_gate_passed = edge.proof_gate_passed;
            edge_report.value_ids = edge.value_ids;
            edge_report.blockers = edge.blockers;
            report.edges.push_back(std::move(edge_report));
        }
        stats_.native_n113_flow_edge_bundle_function_reports.push_back(std::move(report));

        // N113-G20: publish a parallel diagnostic view. It joins the already
        // completed G4/G6/G9/G10/G11 evidence by exact FlowEdgeId. This block
        // is observational only and does not feed back into any planner.
        X64FlowEdgeDiagnosticFunctionReport diagnostic_report;
        diagnostic_report.function_index = function_index_;
        diagnostic_report.function_name = function_.name;
        diagnostic_report.g4_plan_succeeded = n113_flow_edge_location_audit_.succeeded;
        diagnostic_report.g6_plan_succeeded = n113_flow_edge_transfer_candidates_.succeeded;
        diagnostic_report.g9_plan_succeeded = n113_flow_edge_per_edge_scratch_plan_.succeeded;
        diagnostic_report.g10_plan_succeeded = n113_flow_operation_operand_ownership_plan_.succeeded;
        diagnostic_report.g11_plan_succeeded = n113_flow_edge_bundle_eligibility_plan_.succeeded;
        diagnostic_report.g4_edges_audited = n113_flow_edge_location_audit_.edges_audited;
        diagnostic_report.g4_live_values_audited = n113_flow_edge_location_audit_.live_values_audited;
        diagnostic_report.g6_candidate_edges = n113_flow_edge_transfer_candidates_.candidate_edges;
        diagnostic_report.g6_candidate_values = n113_flow_edge_transfer_candidates_.candidate_copies;

        const auto& projected_g11 = stats_.native_n113_flow_edge_bundle_function_reports.back();
        diagnostic_report.edges.reserve(projected_g11.edges.size());
        for (const auto& g11_edge : projected_g11.edges) {
            X64FlowEdgeDiagnosticEdgeReport edge_report;
            edge_report.edge_id = g11_edge.edge_id;
            edge_report.predecessor_block_id = g11_edge.predecessor_block_id;
            edge_report.successor_block_id = g11_edge.successor_block_id;
            edge_report.g4_edge_audited = n113_flow_edge_location_audit_.succeeded;

            for (const auto& value : n113_flow_edge_location_audit_.values) {
                if (value.edge_id != g11_edge.edge_id) continue;
                X64FlowEdgeG4LiveValueDiagnosticReport value_report;
                value_report.value_id = value.value_id;
                value_report.register_class = value.register_class;
                value_report.predecessor_flow_location = value.predecessor_flow_location;
                value_report.successor_flow_location = value.successor_flow_location;
                value_report.emitter_active_location = value.emitter_active_location;
                value_report.predecessor_uses_frame = value.predecessor_uses_frame;
                value_report.predecessor_frame_displacement = value.predecessor_frame_displacement;
                value_report.successor_uses_frame = value.successor_uses_frame;
                value_report.successor_frame_displacement = value.successor_frame_displacement;
                value_report.flow_location_changes = value.flow_location_changes;
                value_report.is_g6_candidate = std::any_of(
                    n113_flow_edge_transfer_candidates_.candidates.begin(),
                    n113_flow_edge_transfer_candidates_.candidates.end(),
                    [&](const NativeX64FlowEdgeTransferCandidate& candidate) {
                        return candidate.edge_id == value.edge_id &&
                            candidate.copy.value_id == value.value_id;
                    });
                edge_report.g4_values.push_back(std::move(value_report));
            }
            edge_report.g4_live_values = static_cast<std::uint32_t>(edge_report.g4_values.size());
            diagnostic_report.g4_candidate_edge_live_values += edge_report.g4_live_values;

            for (const auto& candidate : n113_flow_edge_transfer_candidates_.candidates) {
                if (candidate.edge_id != g11_edge.edge_id) continue;
                X64FlowEdgeG6CandidateDiagnosticReport candidate_report;
                candidate_report.value_id = candidate.copy.value_id;
                const auto class_value = std::find_if(flow_function_->register_ir.values.begin(),
                    flow_function_->register_ir.values.end(), [&](const NativeRegisterValueInfo& value) {
                        return value.value_id == candidate.copy.value_id;
                    });
                if (class_value != flow_function_->register_ir.values.end())
                    candidate_report.register_class = class_value->register_class;
                candidate_report.source_location = candidate.copy.source;
                candidate_report.destination_location = candidate.copy.destination;
                candidate_report.emitter_active_location = candidate.emitter_active_location;
                edge_report.g6_candidates.push_back(std::move(candidate_report));
            }
            edge_report.g6_candidate_values = static_cast<std::uint32_t>(edge_report.g6_candidates.size());

            const auto scratch = std::find_if(
                n113_flow_edge_per_edge_scratch_plan_.edges.begin(),
                n113_flow_edge_per_edge_scratch_plan_.edges.end(),
                [&](const NativeX64FlowEdgePerEdgeScratchEntry& item) {
                    return item.edge_id == g11_edge.edge_id;
                });
            if (scratch != n113_flow_edge_per_edge_scratch_plan_.edges.end()) {
                edge_report.g6_branch_anchor_present = scratch->g6_branch_anchor_present;
                edge_report.g6_branch_anchor_identity_exact = scratch->g6_branch_anchor_identity_exact;
                edge_report.branch_anchor_encoding = static_cast<std::uint8_t>(scratch->g6_branch_anchor.encoding);
                edge_report.branch_edge_kind = static_cast<std::uint8_t>(scratch->g6_branch_anchor.edge_kind);
                edge_report.branch_conditional_taken = scratch->g6_branch_anchor.conditional_taken;
                edge_report.branch_instruction_offset = scratch->g6_branch_anchor.instruction_offset;
                edge_report.branch_displacement_offset = scratch->g6_branch_anchor.displacement_offset;
                edge_report.branch_target_offset = scratch->g6_branch_anchor.target_offset;
                edge_report.branch_function_code_size = scratch->g6_branch_anchor.function_code_size;
                edge_report.g8_readiness_attempted = scratch->g8_readiness_attempted;
                edge_report.g8_readiness_audit_completed = scratch->g8_readiness_audit_completed;
                edge_report.g8_rejection_code = scratch->g8_rejection_code;
                edge_report.g8_rejection_message = scratch->g8_rejection_message;
                edge_report.g9_schedule_built = scratch->schedule_built;
                edge_report.g9_scratch_selection_proven = scratch->scratch_selection_proven;
                edge_report.g9_selected_gp_temporary = scratch->gp_cycle_temporary.register_index;
                edge_report.g9_selected_xmm_temporary = scratch->xmm_cycle_temporary.register_index;
                edge_report.g7_trials.reserve(scratch->g7_trials.size());
                for (const auto& trial : scratch->g7_trials) {
                    X64FlowEdgeG7ScratchTrialDiagnosticReport trial_report;
                    trial_report.gp_temporary_register = trial.gp_temporary_register;
                    trial_report.xmm_temporary_register = trial.xmm_temporary_register;
                    trial_report.g7_schedule_succeeded = trial.g7_schedule_succeeded;
                    trial_report.scheduled_operations = trial.scheduled_operations;
                    trial_report.g7_rejection_code = trial.g7_rejection_code;
                    trial_report.g7_rejection_message = trial.g7_rejection_message;
                    trial_report.g8_readiness_attempted = trial.g8_readiness_attempted;
                    trial_report.g8_readiness_audit_completed = trial.g8_readiness_audit_completed;
                    trial_report.g8_rejection_code = trial.g8_rejection_code;
                    trial_report.g8_rejection_message = trial.g8_rejection_message;
                    trial_report.scratch_liveness_proven = trial.scratch_liveness_proven;
                    trial_report.scratch_liveness_conflicts = trial.scratch_liveness_conflicts;
                    edge_report.g7_trials.push_back(std::move(trial_report));
                }
            }

            edge_report.g7_attempts = static_cast<std::uint32_t>(edge_report.g7_trials.size());
            for (const auto& trial : edge_report.g7_trials) {
                if (trial.g7_schedule_succeeded) ++edge_report.g7_schedule_successes;
                else ++edge_report.g7_schedule_rejections;
            }
            edge_report.g8_live_values = g11_edge.live_values_audited;
            edge_report.g8_endpoint_matches = g11_edge.endpoint_ownership_matches;
            edge_report.g8_endpoint_mismatches = g11_edge.endpoint_ownership_mismatches;
            edge_report.g10_values_with_evidence = g11_edge.operation_values_with_evidence;
            edge_report.g10_values_proven = g11_edge.operation_values_proven;
            edge_report.g10_values_mismatched = g11_edge.operation_values_mismatched;
            edge_report.g10_values_missing = g11_edge.operation_values_missing;
            edge_report.g10_mismatch_counts = g11_edge.mismatch_counts;
            edge_report.g10_values = g11_edge.value_operand_evidence;
            edge_report.g11_exact_schedule = g11_edge.exact_g9_schedule;
            edge_report.g11_endpoint_ownership_proven = g11_edge.endpoint_ownership_proven;
            edge_report.g11_scratch_liveness_proven = g11_edge.scratch_liveness_proven;
            edge_report.g11_operation_operand_ownership_proven = g11_edge.operation_operand_ownership_proven;
            edge_report.g11_proof_gate_passed = g11_edge.proof_gate_passed;
            edge_report.g11_blockers = g11_edge.blockers;

            diagnostic_report.g7_attempts += edge_report.g7_attempts;
            diagnostic_report.g7_schedule_successes += edge_report.g7_schedule_successes;
            diagnostic_report.g7_schedule_rejections += edge_report.g7_schedule_rejections;
            if (edge_report.g8_readiness_attempted) ++diagnostic_report.g8_readiness_attempts;
            if (edge_report.g8_readiness_audit_completed) ++diagnostic_report.g8_readiness_audit_completions;
            if (!edge_report.g6_branch_anchor_present)
                ++diagnostic_report.edges_with_g6_branch_anchor_missing;
            else if (!edge_report.g6_branch_anchor_identity_exact)
                ++diagnostic_report.edges_with_g6_branch_anchor_inexact;
            diagnostic_report.edges.push_back(std::move(edge_report));
        }
        stats_.native_n113_flow_edge_diagnostic_function_reports.push_back(std::move(diagnostic_report));
        if (options_.audit_n113_frame_spill_scratch)
            record_n113_frame_spill_scratch_function_report();
    }

    void record_n113_frame_spill_scratch_function_report() {
        X64FlowFrameScratchDiagnosticFunctionReport report;
        report.function_index = function_index_;
        report.function_name = function_.name;
        report.active_frame_plan_succeeded = frame_plan_.succeeded;
        report.active_frame_size_bytes = frame_plan_.frame_size_bytes;
        report.active_emitter_spill_slot_count = static_cast<std::uint32_t>(frame_plan_.spill_slot_offsets.size());
        report.active_emitter_home_slot_count = static_cast<std::uint32_t>(frame_plan_.home_slot_offsets.size());
        report.flow_frame_slots_reserved_in_active_frame =
            (options_.reserve_n113_flow_frame_slots || options_.insert_n113_emitter_spill_reload_actions) &&
            n113_flow_frame_binding_.succeeded;
        if (flow_allocation_ != nullptr && flow_allocation_->succeeded) {
            report.flow_spill_slots_planned = flow_allocation_->allocation.spill_slot_count;
            report.flow_home_slots_planned = flow_allocation_->allocation.home_slot_count;
        }
        report.hypothetical_flow_spill_slots_mapped =
            static_cast<std::uint32_t>(n113_flow_edge_frame_locations_.spill_slot_offsets.size());

        // Existing Emitter spill slots are part of the active frame, but their
        // owner intervals come from the separate expression-linear allocator.
        // Do not infer exact CFG-edge deadness from that unrelated position space.
        for (std::uint32_t slot = 0U; slot < report.active_emitter_spill_slot_count; ++slot) {
            X64FlowEmitterSpillSlotDiagnosticReport slot_report;
            slot_report.slot_index = slot;
            slot_report.active_frame_displacement = frame_plan_.spill_slot_offsets[slot];
            for (const auto& segment : allocation_plan_.segments) {
                if (!segment.assignment.spilled || segment.assignment.spill_slot != slot) continue;
                slot_report.owners.push_back(X64FlowFrameSpillSlotOwnerDiagnosticReport{
                    segment.segment.segment_id, segment.segment.value_id, segment.segment.block_id,
                    segment.segment.start, segment.segment.end, segment.segment.register_class});
            }
            slot_report.edge_liveness_crosswalk_available = false;
            slot_report.proven_free_for_edge_scratch = slot_report.owners.empty();
            report.emitter_spill_slots.push_back(std::move(slot_report));
        }

        if (flow_allocation_ != nullptr && flow_allocation_->succeeded) {
            const auto& g11_report = stats_.native_n113_flow_edge_bundle_function_reports.back();
            report.edges.reserve(g11_report.edges.size());
            for (const auto& g11_edge : g11_report.edges) {
                X64FlowEdgeFrameScratchDiagnosticEdgeReport edge_report;
                edge_report.edge_id = g11_edge.edge_id;
                edge_report.predecessor_block_id = g11_edge.predecessor_block_id;
                edge_report.successor_block_id = g11_edge.successor_block_id;
                std::vector<const NativeX64FlowEdgeValueAudit*> live_values;
                for (const auto& value : n113_flow_edge_location_audit_.values) {
                    if (value.edge_id == g11_edge.edge_id) live_values.push_back(&value);
                }
                edge_report.g4_live_values = static_cast<std::uint32_t>(live_values.size());

                for (std::uint32_t slot = 0U; slot < flow_allocation_->allocation.spill_slot_count; ++slot) {
                    X64FlowEdgeFlowSpillSlotDiagnosticReport slot_report;
                    slot_report.slot_index = slot;
                    const auto offset = n113_flow_edge_frame_locations_.spill_slot_offsets.find(slot);
                    if (offset != n113_flow_edge_frame_locations_.spill_slot_offsets.end()) {
                        slot_report.frame_mapping_available = offset->second < 0 && (offset->second % 8) == 0;
                        slot_report.hypothetical_frame_displacement = offset->second;
                    }
                    slot_report.active_frame_reserved = report.flow_frame_slots_reserved_in_active_frame;
                    for (const auto& segment : flow_allocation_->allocation.segments) {
                        if (!segment.assignment.spilled || segment.assignment.spill_slot != slot) continue;
                        slot_report.owners.push_back(X64FlowFrameSpillSlotOwnerDiagnosticReport{
                            segment.segment.segment_id, segment.segment.value_id, segment.segment.block_id,
                            segment.segment.start, segment.segment.end, segment.segment.register_class});
                    }
                    for (const auto* value : live_values) {
                        if (value == nullptr) continue;
                        const bool predecessor_uses =
                            value->predecessor_flow_location.kind == NativeRegisterLocationKind::SpillSlot &&
                            value->predecessor_flow_location.spill_slot == slot;
                        const bool successor_uses =
                            value->successor_flow_location.kind == NativeRegisterLocationKind::SpillSlot &&
                            value->successor_flow_location.spill_slot == slot;
                        if (predecessor_uses) ++slot_report.live_endpoint_uses;
                        if (successor_uses) ++slot_report.live_endpoint_uses;
                        if ((predecessor_uses || successor_uses) &&
                            std::find(slot_report.live_endpoint_value_ids.begin(),
                                      slot_report.live_endpoint_value_ids.end(), value->value_id) ==
                                slot_report.live_endpoint_value_ids.end()) {
                            slot_report.live_endpoint_value_ids.push_back(value->value_id);
                        }
                    }
                    slot_report.no_g4_live_endpoint_alias = slot_report.live_endpoint_uses == 0U;
                    slot_report.candidate_after_flow_frame_reservation =
                        slot_report.frame_mapping_available && slot_report.no_g4_live_endpoint_alias;
                    slot_report.usable_in_current_active_frame = slot_report.active_frame_reserved &&
                        slot_report.candidate_after_flow_frame_reservation;
                    slot_report.current_g7_accepts_spill_slot_temporary = false;
                    edge_report.flow_spill_slots.push_back(std::move(slot_report));
                }
                report.edges.push_back(std::move(edge_report));
            }
        }
        stats_.native_n113_frame_scratch_diagnostic_function_reports.push_back(std::move(report));
    }

    bool audit_n113_flow_edge_bundle_eligibility() {
        n113_flow_edge_bundle_eligibility_plan_ = NativeX64FlowEdgeBundleEligibilityPlan{};
        n113_flow_edge_bundle_eligibility_plan_.function_index = function_index_;
        if (flow_function_ == nullptr || !n113_flow_edge_transfer_candidates_.succeeded ||
            !n113_flow_edge_per_edge_scratch_plan_.succeeded ||
            !n113_flow_operation_operand_ownership_plan_.succeeded) {
            n113_flow_edge_bundle_eligibility_plan_.diagnostics.push_back(
                NativeRegisterIRDiagnostic{"XN113K9001",
                    "G11 requires successful G6 candidate, G9 per-edge scratch and G10 operation-operand audits"});
            ++stats_.native_n113_flow_edge_bundle_eligibility_plans;
            record_n113_flow_edge_bundle_function_report();
            return true; // A read-only proof blocker must never disrupt ordinary emission.
        }

        std::map<std::uint32_t, NativeRegisterClass> value_classes;
        for (const auto& value : flow_function_->register_ir.values) {
            if (value.value_id == 0U || !value_classes.emplace(value.value_id, value.register_class).second) {
                n113_flow_edge_bundle_eligibility_plan_.diagnostics.push_back(
                    NativeRegisterIRDiagnostic{"XN113K9002",
                        "G11 FunctionEmitter value-class table contains an invalid or duplicate ValueId"});
                ++stats_.native_n113_flow_edge_bundle_eligibility_plans;
                record_n113_flow_edge_bundle_function_report();
                return true;
            }
        }

        n113_flow_edge_bundle_eligibility_plan_ = audit_native_x64_flow_edge_bundle_eligibility(
            n113_flow_edge_transfer_candidates_, n113_flow_edge_per_edge_scratch_plan_,
            n113_flow_operation_operand_ownership_plan_, value_classes, function_index_);
        ++stats_.native_n113_flow_edge_bundle_eligibility_plans;
        stats_.native_n113_flow_edge_bundle_candidate_edges +=
            n113_flow_edge_bundle_eligibility_plan_.candidate_edges;
        stats_.native_n113_flow_edge_bundle_proof_gate_passed +=
            n113_flow_edge_bundle_eligibility_plan_.proof_gate_passed_edges;
        stats_.native_n113_flow_edge_bundle_blocked +=
            n113_flow_edge_bundle_eligibility_plan_.blocked_edges;
        stats_.native_n113_flow_edge_bundle_schedule_blocked +=
            n113_flow_edge_bundle_eligibility_plan_.schedule_blocked_edges;
        stats_.native_n113_flow_edge_bundle_endpoint_blocked +=
            n113_flow_edge_bundle_eligibility_plan_.endpoint_ownership_blocked_edges;
        stats_.native_n113_flow_edge_bundle_scratch_blocked +=
            n113_flow_edge_bundle_eligibility_plan_.scratch_blocked_edges;
        stats_.native_n113_flow_edge_bundle_operand_blocked +=
            n113_flow_edge_bundle_eligibility_plan_.operation_operand_blocked_edges;
        record_n113_flow_edge_bundle_function_report();
        return true; // G11 is an evidence gate only; no Edge Bundle is inserted.
    }

    bool insert_n113_emitter_action_transfers(
        std::vector<X64FlowEmitterAnchor>& flow_anchors,
        std::vector<X64FlowBranchAnchor>& flow_branch_anchors) {
        if (!n113_emitter_action_rebinding_.succeeded || !n113_flow_frame_binding_.succeeded) {
            add_diag(*diagnostics_, "XN113G2001",
                "G2 requires successful G1 rebinding and an active combined N112/Emitter frame plan",
                function_.span);
            return false;
        }
        if (n113_flow_frame_binding_.frame_plan.frame_size_bytes != frame_plan_.frame_size_bytes) {
            add_diag(*diagnostics_, "XN113G2002",
                "G2 combined frame size does not match the frame used by the emitted prologue",
                function_.span);
            return false;
        }

        std::map<std::uint32_t, NativeRegisterClass> value_classes;
        for (const auto& value : flow_function_->register_ir.values) {
            if (!value_classes.emplace(value.value_id, value.register_class).second) {
                add_diag(*diagnostics_, "XN113G2003",
                    "G2 Flow value table contains duplicate value IDs", function_.span);
                return false;
            }
        }

        const std::size_t original_code_size = code_.size();
        const std::size_t operation_anchor_count = emitted_flow_anchors_.size();
        const std::size_t branch_anchor_count = emitted_flow_branch_anchors_.size();
        const auto insertion = insert_native_x64_spill_reload_actions(
            code_, relative_branch_fixups_, emitted_flow_anchors_, emitted_flow_branch_anchors_,
            n113_emitter_action_rebinding_.bindings, value_classes,
            n113_emitter_action_rebinding_.gp_cycle_temporary,
            n113_emitter_action_rebinding_.xmm_cycle_temporary,
            n113_emitter_action_rebinding_.frame_locations, relocations_);
        if (!insertion.succeeded) {
            const std::string detail = insertion.diagnostics.empty()
                ? "unknown spill/reload insertion failure"
                : insertion.diagnostics.front().code + " " + insertion.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113G2004",
                "G2 rejected the rebound spill/reload insertion: " + detail, function_.span);
            return false;
        }
        for (const auto& adjustment : insertion.offset_adjustments) {
            if (adjustment.original_offset < prologue_end_offset_) {
                add_diag(*diagnostics_, "XN113G2005",
                    "G2 transfer insertion would modify the Win64 prologue covered by UNWIND_INFO",
                    function_.span);
                return false;
            }
        }
        if (insertion.machine_actions_inserted != n113_emitter_action_rebinding_.bindings.size()) {
            add_diag(*diagnostics_, "XN113G2006",
                "G2 did not materialize every rebound N112 action exactly once", function_.span);
            return false;
        }
        if (!n113_emitter_action_rebinding_.bindings.empty() &&
            (insertion.code.size() <= original_code_size || insertion.machine_moves_inserted == 0U)) {
            add_diag(*diagnostics_, "XN113G2007",
                "G2 reported actions but produced no machine transfer bytes", function_.span);
            return false;
        }
        if (flow_anchors.size() < operation_anchor_count ||
            flow_branch_anchors.size() < branch_anchor_count) {
            add_diag(*diagnostics_, "XN113G2008",
                "G2 output anchor vectors lost entries before insertion rebasing", function_.span);
            return false;
        }

        code_ = insertion.code;
        relative_branch_fixups_ = insertion.relative_branch_fixups;
        relocations_ = insertion.external_relocations;
        emitted_flow_anchors_ = insertion.operation_anchors;
        emitted_flow_branch_anchors_ = insertion.branch_anchors;

        // finalize_* appended this function's anchors last; replace only that
        // suffix so anchors from previously emitted functions remain intact.
        flow_anchors.resize(flow_anchors.size() - operation_anchor_count);
        flow_anchors.insert(flow_anchors.end(), emitted_flow_anchors_.begin(), emitted_flow_anchors_.end());
        flow_branch_anchors.resize(flow_branch_anchors.size() - branch_anchor_count);
        flow_branch_anchors.insert(flow_branch_anchors.end(),
            emitted_flow_branch_anchors_.begin(), emitted_flow_branch_anchors_.end());

        stats_.native_n113_emitter_actions_inserted += insertion.machine_actions_inserted;
        stats_.native_n113_emitter_machine_moves_inserted += insertion.machine_moves_inserted;
        stats_.native_n113_emitter_transfer_bytes_inserted +=
            static_cast<std::uint32_t>(insertion.code.size() - original_code_size);
        return true;
    }

    bool validate_n113_flow_action_anchors() {
        std::map<std::uint64_t, const X64FlowEmitterAnchor*> anchors;
        for (const auto& anchor : emitted_flow_anchors_) {
            if (anchor.flow_operation_id == InvalidNativeFlowOperationId ||
                !anchors.emplace(anchor.flow_operation_id, &anchor).second) {
                add_diag(*diagnostics_, "XN113C0004",
                    "N113 found an invalid or duplicate emitted Flow operation anchor", anchor.source_span);
                return false;
            }
        }
        std::map<std::uint64_t, const NativeRegisterFlowOperationLocation*> operations;
        for (const auto& operation : n113_flow_segment_plan_.operations) {
            if (operation.flow_operation_id == InvalidNativeFlowOperationId ||
                !operations.emplace(operation.flow_operation_id, &operation).second) {
                add_diag(*diagnostics_, "XN113C0005",
                    "N113 found an invalid or duplicate planned Flow operation", function_.span);
                return false;
            }
        }
        std::set<std::tuple<std::uint64_t, std::uint32_t, NativeSpillReloadActionKind>> seen;
        std::uint32_t validated_actions = 0U;
        for (const auto& binding : n113_flow_action_resolution_.bindings) {
            const auto anchor = anchors.find(binding.flow_operation_id);
            const auto operation = operations.find(binding.flow_operation_id);
            if (anchor == anchors.end() || operation == operations.end() || anchor->second == nullptr ||
                operation->second == nullptr ||
                anchor->second->function_index != function_index_ ||
                anchor->second->flow_block_id != operation->second->flow_block_id ||
                anchor->second->flow_operation_index != operation->second->flow_operation_index ||
                anchor->second->origin_kind != binding.origin_kind ||
                operation->second->native_instruction_index != binding.action.instruction_index) {
                add_diag(*diagnostics_, "XN113C0006",
                    "N113 action does not match its emitted function/block/operation anchor", function_.span);
                return false;
            }

            const bool before_action = binding.action.kind == NativeSpillReloadActionKind::StoreBeforeCall ||
                binding.action.kind == NativeSpillReloadActionKind::ReloadBeforeUse;
            const bool after_call_action = binding.action.kind == NativeSpillReloadActionKind::ReloadAfterCall;
            const auto expected_placement = before_action
                ? NativeSpillReloadPlacement::BeforeOperation
                : NativeSpillReloadPlacement::AfterOperation;
            if (binding.placement != expected_placement) {
                add_diag(*diagnostics_, "XN113C0007",
                    "N113 action placement disagrees with the spill/reload action kind", anchor->second->source_span);
                return false;
            }
            std::uint32_t boundary = InvalidNativeFlowOperationIndex;
            if (before_action) boundary = anchor->second->machine_code_before_offset;
            else if (after_call_action) boundary = anchor->second->machine_code_after_call_offset;
            else boundary = anchor->second->machine_code_offset;
            if (boundary == InvalidNativeFlowOperationIndex ||
                boundary > anchor->second->function_code_size ||
                anchor->second->function_code_size != code_.size()) {
                add_diag(*diagnostics_, "XN113C0008",
                    "N113 action has no valid operation/call boundary in final emitted code", anchor->second->source_span);
                return false;
            }
            if (!seen.emplace(binding.flow_operation_id, binding.action.value_id, binding.action.kind).second) {
                add_diag(*diagnostics_, "XN113C0009",
                    "N113 validated the same action more than once", anchor->second->source_span);
                return false;
            }
            ++stats_.native_n113_actions_anchor_validated;
            ++validated_actions;
        }
        return validated_actions == n113_flow_action_resolution_.bindings.size();
    }

    bool validate_n113_flow_action_locations() {
        const auto audit = audit_native_x64_flow_action_locations(
            n113_flow_action_resolution_.bindings, flow_machine_location_plan_);
        if (!audit.succeeded) {
            const std::string detail = audit.diagnostics.empty()
                ? "unknown N111/N112 location-audit failure"
                : audit.diagnostics.front().code + " " + audit.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113C0010",
                "N113 could not audit N112 action endpoints against N111 locations: " + detail, function_.span);
            return false;
        }
        if (audit.audited_actions != n113_flow_action_resolution_.bindings.size()) {
            add_diag(*diagnostics_, "XN113C0011",
                "N113 location audit did not account for every resolved action", function_.span);
            return false;
        }
        stats_.native_n113_actions_location_audited += audit.audited_actions;
        stats_.native_n113_action_values_without_persistent_location +=
            audit.actions_without_persistent_binding;
        stats_.native_n113_action_location_register_matches += audit.register_endpoint_matches;
        stats_.native_n113_action_location_register_mismatches += audit.register_endpoint_mismatches;
        stats_.native_n113_action_location_unmapped_endpoints += audit.unmapped_slot_or_temporary_endpoints;
        stats_.native_n113_action_location_unbound_endpoints += audit.endpoints_without_persistent_binding;
        return true;
    }

    bool validate_n113_flow_operation_locations() {
        const auto audit = audit_native_x64_flow_operation_locations(
            n113_flow_segment_plan_, flow_machine_location_plan_);
        if (!audit.succeeded) {
            const std::string detail = audit.diagnostics.empty()
                ? "unknown N111/N112 operation-location audit failure"
                : audit.diagnostics.front().code + " " + audit.diagnostics.front().message;
            add_diag(*diagnostics_, "XN113D0001",
                "N113 could not audit N112 operation positions against N111 locations: " + detail,
                function_.span);
            return false;
        }
        if (audit.audited_operations != n113_flow_segment_plan_.operations.size() ||
            audit.audited_value_positions != audit.entries.size()) {
            add_diag(*diagnostics_, "XN113D0002",
                "N113 operation-location audit did not account for every operation/value position",
                function_.span);
            return false;
        }
        stats_.native_n113_flow_operations_location_audited += audit.audited_operations;
        stats_.native_n113_flow_value_positions_location_audited += audit.audited_value_positions;
        stats_.native_n113_flow_positions_register_matches += audit.register_position_matches;
        stats_.native_n113_flow_positions_register_mismatches += audit.register_position_mismatches;
        stats_.native_n113_flow_positions_unmapped += audit.unmapped_slot_or_temporary_positions;
        stats_.native_n113_flow_positions_unbound += audit.unbound_value_positions;
        return true;
    }

    bool validate_n113_flow_emitter_operand_locations() {
        const auto location_equal = [](const NativeRegisterLocation& left,
                                       const NativeRegisterLocation& right) {
            if (left.kind != right.kind) return false;
            if (left.kind == NativeRegisterLocationKind::Register)
                return left.register_index == right.register_index;
            if (left.kind == NativeRegisterLocationKind::HomeSlot ||
                left.kind == NativeRegisterLocationKind::SpillSlot)
                return left.spill_slot == right.spill_slot;
            return false;
        };
        const auto find_flow_location = [](const std::vector<NativeRegisterFlowValueLocation>& locations,
                                           std::uint32_t value_id)
            -> const NativeRegisterFlowValueLocation* {
            const auto found = std::find_if(locations.begin(), locations.end(),
                [value_id](const NativeRegisterFlowValueLocation& item) {
                    return item.value_id == value_id;
                });
            return found == locations.end() ? nullptr : &*found;
        };

        std::map<std::uint64_t, const NativeRegisterInstruction*> instructions;
        for (const auto& block : flow_function_->register_ir.blocks) {
            for (const auto& instruction : block.instructions) {
                if (instruction.origin_kind == NativeRegisterInstructionOriginKind::Synthetic) continue;
                if (instruction.flow_operation_id == InvalidNativeFlowOperationId ||
                    !instructions.emplace(instruction.flow_operation_id, &instruction).second) {
                    add_diag(*diagnostics_, "XN113E0007",
                        "Flow emitter crosswalk has an invalid or duplicate native operation identity",
                        instruction.source_span);
                    return false;
                }
            }
        }
        std::map<std::uint64_t, const NativeRegisterFlowOperationLocation*> locations_by_operation;
        for (const auto& operation : n113_flow_segment_plan_.operations) {
            if (operation.flow_operation_id == InvalidNativeFlowOperationId ||
                !locations_by_operation.emplace(operation.flow_operation_id, &operation).second) {
                add_diag(*diagnostics_, "XN113E0008",
                    "Flow emitter crosswalk has a duplicate N112 operation location", function_.span);
                return false;
            }
        }
        std::map<std::uint64_t, const X64FlowEmitterAnchor*> anchors;
        for (const auto& anchor : emitted_flow_anchors_) {
            if (!anchors.emplace(anchor.flow_operation_id, &anchor).second) {
                add_diag(*diagnostics_, "XN113E0009",
                    "Flow emitter crosswalk has a duplicate emitted operation anchor", anchor.source_span);
                return false;
            }
        }
        if (instructions.size() != locations_by_operation.size() ||
            instructions.size() != anchors.size()) {
            add_diag(*diagnostics_, "XN113E000A",
                "Flow IR, N112 locations and emitted operation anchors have different cardinalities",
                function_.span);
            return false;
        }

        std::uint32_t mapped_operations = 0U;
        std::uint32_t mapped_reads = 0U;
        std::uint32_t mapped_writes = 0U;
        std::uint32_t mapped_calls = 0U;
        std::uint32_t operand_matches = 0U;
        std::uint32_t register_mismatches = 0U;
        std::uint32_t unmapped_locations = 0U;
        for (const auto& item : instructions) {
            const auto anchor_it = anchors.find(item.first);
            const auto locations_it = locations_by_operation.find(item.first);
            if (anchor_it == anchors.end() || locations_it == locations_by_operation.end() ||
                item.second == nullptr || anchor_it->second == nullptr || locations_it->second == nullptr) {
                add_diag(*diagnostics_, "XN113E000B",
                    "Flow operation has no exact emitter-anchor/N112-location crosswalk", function_.span);
                return false;
            }
            const auto& instruction = *item.second;
            const auto& anchor = *anchor_it->second;
            const auto& operation = *locations_it->second;
            if (anchor.function_index != function_index_ ||
                anchor.flow_block_id != instruction.flow_block_id ||
                anchor.flow_operation_index != instruction.flow_operation_index ||
                anchor.origin_kind != instruction.origin_kind ||
                operation.flow_block_id != instruction.flow_block_id ||
                operation.flow_operation_index != instruction.flow_operation_index ||
                operation.native_instruction_index == InvalidNativeFlowOperationIndex) {
                add_diag(*diagnostics_, "XN113E000C",
                    "Flow operation, N112 plan and emitted anchor disagree on stable identity", anchor.source_span);
                return false;
            }
            if (anchor.machine_code_offset > code_.size() ||
                anchor.function_code_size != code_.size()) {
                add_diag(*diagnostics_, "XN113E000D",
                    "Flow emitter operand crosswalk has an invalid final code boundary", anchor.source_span);
                return false;
            }

            const auto validate_operand = [&](std::uint32_t value_id, bool before,
                                              X64FlowEmitterAnchor::OperandKind expected_kind) -> bool {
                if (anchor.emitted_operand_kind != expected_kind ||
                    anchor.emitted_operand_value_id != value_id ||
                    anchor.machine_code_before_offset == InvalidNativeFlowOperationIndex ||
                    anchor.machine_code_before_offset >= anchor.machine_code_offset) {
                    add_diag(*diagnostics_, "XN113E000E",
                        "Flow source operand has no exact non-empty emitter instruction range", anchor.source_span);
                    return false;
                }
                const auto machine_value = flow_machine_location_plan_.values.find(value_id);
                if (machine_value == flow_machine_location_plan_.values.end() ||
                    !location_equal(anchor.emitted_operand_location, machine_value->second.active_location)) {
                    add_diag(*diagnostics_, "XN113E000F",
                        "emitted source operand location differs from the active N111 location", anchor.source_span);
                    return false;
                }
                const auto& candidates = before ? operation.before_locations : operation.after_locations;
                const auto* planned = find_flow_location(candidates, value_id);
                if (planned == nullptr) {
                    add_diag(*diagnostics_, "XN113E0010",
                        "N112 operation position does not contain its emitted source operand", anchor.source_span);
                    return false;
                }
                if (planned->location.kind == NativeRegisterLocationKind::Register &&
                    anchor.emitted_operand_location.kind == NativeRegisterLocationKind::Register) {
                    if (planned->location.register_index == anchor.emitted_operand_location.register_index)
                        ++operand_matches;
                    else ++register_mismatches;
                } else {
                    // N111 HomeSlot and N112 SpillSlot/HomeSlot IDs are distinct
                    // namespaces until N113-F constructs a physical frame map.
                    ++unmapped_locations;
                }
                return true;
            };

            switch (instruction.origin_kind) {
            case NativeRegisterInstructionOriginKind::FlowRead:
                if (instruction.is_call || instruction.uses.size() != 1U ||
                    !instruction.definitions.empty() ||
                    (anchor.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::ValueRead &&
                     anchor.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::AddressRead)) {
                    add_diag(*diagnostics_, "XN113E0011",
                        "FlowRead is not represented by exactly one concrete emitter operand", anchor.source_span);
                    return false;
                }
                if (anchor.emitted_operand_kind == X64FlowEmitterAnchor::OperandKind::AddressRead) {
                    if (anchor.emitted_operand_value_id != instruction.uses.front() ||
                        anchor.emitted_operand_location.kind != NativeRegisterLocationKind::HomeSlot ||
                        !flow_machine_location_plan_.values.at(instruction.uses.front()).address_taken ||
                        anchor.machine_code_before_offset + 7U > code_.size() ||
                        code_[anchor.machine_code_before_offset] != 0x48U ||
                        code_[anchor.machine_code_before_offset + 1U] != 0x8DU) {
                        add_diag(*diagnostics_, "XN113E0012",
                            "address-of FlowRead is not anchored to an address-taken canonical home", anchor.source_span);
                        return false;
                    }
                } else if (!validate_operand(instruction.uses.front(), true,
                                              X64FlowEmitterAnchor::OperandKind::ValueRead)) return false;
                ++mapped_reads;
                break;
            case NativeRegisterInstructionOriginKind::FlowWrite:
                if (instruction.is_call || instruction.definitions.size() != 1U ||
                    !instruction.uses.empty() ||
                    !validate_operand(instruction.definitions.front(), false,
                                      X64FlowEmitterAnchor::OperandKind::ValueWrite)) return false;
                ++mapped_writes;
                break;
            case NativeRegisterInstructionOriginKind::FlowDeclare:
                if (instruction.is_call || !instruction.uses.empty() || instruction.definitions.size() > 1U) {
                    add_diag(*diagnostics_, "XN113E0013",
                        "FlowDeclare has unsupported native operands", anchor.source_span);
                    return false;
                }
                if (!instruction.definitions.empty()) {
                    if (!validate_operand(instruction.definitions.front(), false,
                                          X64FlowEmitterAnchor::OperandKind::ValueDeclare)) return false;
                    ++mapped_writes;
                } else if (anchor.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::None) {
                    add_diag(*diagnostics_, "XN113E0014",
                        "uninitialized FlowDeclare unexpectedly claims a value definition", anchor.source_span);
                    return false;
                }
                break;
            case NativeRegisterInstructionOriginKind::FlowCall: {
                const std::uint32_t call_offset = anchor.machine_code_call_instruction_offset;
                if (!instruction.is_call || !instruction.uses.empty() || !instruction.definitions.empty() ||
                    call_offset == InvalidNativeFlowOperationIndex ||
                    anchor.machine_code_after_call_offset != call_offset + 5U ||
                    call_offset + 5U > code_.size() || code_[call_offset] != 0xE8U ||
                    anchor.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::None) {
                    add_diag(*diagnostics_, "XN113E0015",
                        "FlowCall is not mapped to its concrete CALL rel32 instruction", anchor.source_span);
                    return false;
                }
                const std::size_t relocation_offset = static_cast<std::size_t>(call_offset) + 1U;
                const auto relocation = std::find_if(relocations_.begin(), relocations_.end(),
                    [relocation_offset](const NativeObjectRelocationInput& value) {
                        return value.offset == relocation_offset && value.width == 4U &&
                               value.kind == ObjectRelocationKind::CoffRelative32;
                    });
                if (relocation == relocations_.end()) {
                    add_diag(*diagnostics_, "XN113E0016",
                        "FlowCall rel32 field has no matching COFF relocation", anchor.source_span);
                    return false;
                }
                ++mapped_calls;
                break;
            }
            case NativeRegisterInstructionOriginKind::FlowReturn:
                if (instruction.is_call || !instruction.uses.empty() || !instruction.definitions.empty() ||
                    anchor.emitted_operand_kind != X64FlowEmitterAnchor::OperandKind::None) {
                    add_diag(*diagnostics_, "XN113E0017",
                        "FlowReturn has unexpected emitter value operands", anchor.source_span);
                    return false;
                }
                break;
            case NativeRegisterInstructionOriginKind::Synthetic:
                add_diag(*diagnostics_, "XN113E0018",
                    "synthetic operation unexpectedly entered the source emitter crosswalk", anchor.source_span);
                return false;
            }
            ++mapped_operations;
        }
        if (mapped_operations != emitted_flow_anchors_.size()) {
            add_diag(*diagnostics_, "XN113E0019",
                "Flow emitter crosswalk did not account for each stable operation exactly once", function_.span);
            return false;
        }
        stats_.native_n113_flow_emitter_operations_mapped += mapped_operations;
        stats_.native_n113_flow_emitter_reads_mapped += mapped_reads;
        stats_.native_n113_flow_emitter_writes_mapped += mapped_writes;
        stats_.native_n113_flow_emitter_calls_mapped += mapped_calls;
        stats_.native_n113_flow_emitter_operand_matches += operand_matches;
        stats_.native_n113_flow_emitter_operand_register_mismatches += register_mismatches;
        stats_.native_n113_flow_emitter_operand_unmapped += unmapped_locations;
        return true;
    }

    bool record_flow_home(const std::string& name, const SourceSpan& span, std::size_t slot) {
        const auto found = flow_value_by_binding_key_.find(flow_binding_key(name, span));
        if (found == flow_value_by_binding_key_.end()) {
            add_diag(*diagnostics_, "XN90C0004", "local or parameter is missing from the frontend CFG binding map", span);
            return false;
        }
        if (slot > std::numeric_limits<std::uint32_t>::max()) {
            add_diag(*diagnostics_, "XN90C0005", "persistent home slot exceeds 32-bit native location range", span);
            return false;
        }
        NativeRegisterLocation location;
        location.kind = NativeRegisterLocationKind::HomeSlot;
        location.spill_slot = static_cast<std::uint32_t>(slot);
        const auto inserted = flow_value_home_locations_.emplace(found->second, location);
        if (!inserted.second && (inserted.first->second.kind != location.kind ||
                                 inserted.first->second.spill_slot != location.spill_slot)) {
            add_diag(*diagnostics_, "XN90C0006", "one CFG value was assigned multiple physical home slots", span);
            return false;
        }
        // Bind the canonical home through the authoritative plan. For a
        // Home-resident value this also resolves its active location; for a
        // register-resident value the active register remains unchanged.
        if (!bind_native_x64_machine_location_home(
                flow_machine_location_plan_, found->second, location.spill_slot)) {
            const std::string detail = flow_machine_location_plan_.diagnostics.empty()
                ? "unknown canonical-home binding failure"
                : flow_machine_location_plan_.diagnostics.back().code + " " +
                  flow_machine_location_plan_.diagnostics.back().message;
            add_diag(*diagnostics_, "XN111C0002",
                "could not bind the authoritative Machine Location home: " + detail, span);
            return false;
        }
        const auto planned = flow_machine_location_plan_.values.find(found->second);
        if (planned == flow_machine_location_plan_.values.end()) {
            add_diag(*diagnostics_, "XN111C0003",
                "source binding is missing from the authoritative Machine Location plan", span);
            return false;
        }
        local_register_locations_.erase(slot);
        if (planned->second.active_location.kind == NativeRegisterLocationKind::Register) {
            local_register_locations_[slot] = planned->second.active_location;
        }
        return true;
    }

    bool finalize_home_phi_coalescing() {
        if (flow_function_ == nullptr || flow_allocation_ == nullptr) return false;
        if (flow_value_home_locations_.size() != flow_function_->bindings.size()) {
            add_diag(*diagnostics_, "XN90C0007",
                "not every scalar/pointer source variable received a physical stack home", function_.span);
            return false;
        }
        if (!flow_machine_location_plan_.succeeded ||
            flow_machine_location_plan_.values.size() != flow_function_->bindings.size()) {
            add_diag(*diagnostics_, "XN111C0004",
                "authoritative Machine Location plan is missing one or more source bindings", function_.span);
            return false;
        }
        for (const auto& home : flow_value_home_locations_) {
            const auto planned = flow_machine_location_plan_.values.find(home.first);
            if (planned == flow_machine_location_plan_.values.end() ||
                planned->second.canonical_home.kind != NativeRegisterLocationKind::HomeSlot ||
                planned->second.canonical_home.spill_slot != home.second.spill_slot ||
                (planned->second.active_location.kind == NativeRegisterLocationKind::HomeSlot &&
                 planned->second.active_location.spill_slot != home.second.spill_slot) ||
                (planned->second.address_taken &&
                 planned->second.active_location.kind != NativeRegisterLocationKind::HomeSlot)) {
                add_diag(*diagnostics_, "XN111C0005",
                    "canonical home and active Machine Location plan are inconsistent", function_.span);
                return false;
            }
        }
        std::map<std::uint32_t, std::uint32_t> physical_home_slots;
        for (const auto& location : flow_value_home_locations_)
            physical_home_slots.emplace(location.first, location.second.spill_slot);
        flow_home_plan_ = build_native_register_variable_home_plan(*flow_function_, physical_home_slots);
        if (!flow_home_plan_.succeeded) {
            add_diag(*diagnostics_, "XN90C0008", "final physical home map failed CFG binding validation", function_.span);
            return false;
        }
        stats_.native_persistent_homes_mapped += flow_home_plan_.values_homed;
        for (std::size_t block_index = 0U; block_index < flow_function_->register_ir.blocks.size(); ++block_index) {
            const auto& block = flow_function_->register_ir.blocks[block_index];
            for (std::size_t instruction_index = 0U; instruction_index < block.instructions.size(); ++instruction_index) {
                if (!block.instructions[instruction_index].is_call) continue;
                const auto& live = flow_allocation_->liveness.instructions[block_index][instruction_index];
                for (const auto value : live.live_before) {
                    if (std::find(live.live_after.begin(), live.live_after.end(), value) != live.live_after.end() &&
                        flow_value_home_locations_.find(value) != flow_value_home_locations_.end()) {
                        ++stats_.native_call_crossing_values_homed;
                    }
                }
            }
        }
        const auto phi_plan = build_native_register_home_phi_plan(
            *flow_function_, flow_allocation_->liveness, flow_home_plan_);
        if (!phi_plan.succeeded) {
            const std::string detail = phi_plan.diagnostics.empty() ? "unknown home Phi planning failure" :
                phi_plan.diagnostics.front().code + " " + phi_plan.diagnostics.front().message;
            add_diag(*diagnostics_, "XN91C0001", "memory-SSA Phi planning failed: " + detail, function_.span);
            return false;
        }
        stats_.native_home_phi_nodes += phi_plan.phi_count;
        stats_.native_home_phi_incoming_edges += phi_plan.incoming_edges;
        if (phi_plan.phis.empty()) return true;

        NativeRegisterFunctionIR phi_function = flow_function_->register_ir;
        std::map<std::uint32_t, NativeRegisterLocation> locations = flow_value_home_locations_;
        for (const auto& alias : phi_plan.home_aliases) {
            const auto source_location = locations.find(alias.source_home_value_id);
            const auto source_info = std::find_if(phi_function.values.begin(), phi_function.values.end(),
                [&alias](const NativeRegisterValueInfo& value) {
                    return value.value_id == alias.source_home_value_id;
                });
            if (source_location == locations.end() || source_info == phi_function.values.end()) {
                add_diag(*diagnostics_, "XN91C0003", "synthetic Phi edge version has no source home metadata", function_.span);
                return false;
            }
            phi_function.values.push_back(NativeRegisterValueInfo{alias.synthetic_value_id, source_info->register_class});
            locations.emplace(alias.synthetic_value_id, source_location->second);
        }
        const auto edge_lowering = lower_native_register_phi_edges(
            phi_function, phi_plan.phis, locations,
            NativeRegisterLocation{NativeRegisterLocationKind::Temporary, 0U,
                std::numeric_limits<std::uint32_t>::max()});
        if (!edge_lowering.succeeded) {
            const std::string detail = edge_lowering.diagnostics.empty() ? "unknown Phi edge lowering failure" :
                edge_lowering.diagnostics.front().code + " " + edge_lowering.diagnostics.front().message;
            add_diag(*diagnostics_, "XN91C0005", "memory-SSA Phi edge lowering failed: " + detail, function_.span);
            return false;
        }
        for (const auto& edge : edge_lowering.edge_transfers) {
            if (!edge.operations.empty()) {
                add_diag(*diagnostics_, "XN91C0006",
                    "canonical stack-home Phi coalescing unexpectedly requires a physical edge copy", function_.span);
                return false;
            }
        }
        stats_.native_home_phi_edges_coalesced += phi_plan.incoming_edges;
        stats_.native_home_phi_critical_edges += edge_lowering.critical_edges_split;
        stats_.native_home_phi_machine_copies += 0U;
        return true;
    }

    void emit_gp_frame_save(std::uint8_t reg, bool restore,
                            std::vector<FrameDisplacementFixup>& fixups) {
        code_.push_back(static_cast<std::uint8_t>(0x48U | (reg >= 8U ? (restore ? 0x04U : 0x04U) : 0U)));
        code_.push_back(restore ? 0x8BU : 0x89U);
        code_.push_back(static_cast<std::uint8_t>(0x85U | ((reg & 7U) << 3U)));
        const std::size_t displacement = code_.size();
        emit_u32(0U);
        fixups.push_back(FrameDisplacementFixup{NativeRegisterClass::GeneralPurpose, reg, displacement, code_.size()});
    }

    void emit_xmm_frame_save(std::uint8_t reg, bool restore,
                             std::vector<FrameDisplacementFixup>& fixups) {
        code_.push_back(0xF3U); // MOVDQU is safe even for unusual local-area alignment.
        if (reg >= 8U) code_.push_back(0x44U); // REX.R extends the XMM register field.
        code_.insert(code_.end(), {0x0FU, static_cast<std::uint8_t>(restore ? 0x6FU : 0x7FU),
            static_cast<std::uint8_t>(0x85U | ((reg & 7U) << 3U))});
        const std::size_t displacement = code_.size();
        emit_u32(0U);
        fixups.push_back(FrameDisplacementFixup{NativeRegisterClass::FloatingPoint, reg, displacement, code_.size()});
    }

    void emit_callee_save_prologue() {
        for (const auto reg : allocation_plan_.used_callee_saved_gp)
            emit_gp_frame_save(reg, false, prologue_save_fixups_);
        for (const auto reg : allocation_plan_.used_callee_saved_xmm)
            emit_xmm_frame_save(reg, false, prologue_save_fixups_);
    }

    void emit_callee_save_epilogue() {
        for (auto it = allocation_plan_.used_callee_saved_xmm.rbegin();
             it != allocation_plan_.used_callee_saved_xmm.rend(); ++it)
            emit_xmm_frame_save(*it, true, epilogue_restore_fixups_);
        for (auto it = allocation_plan_.used_callee_saved_gp.rbegin();
             it != allocation_plan_.used_callee_saved_gp.rend(); ++it)
            emit_gp_frame_save(*it, true, epilogue_restore_fixups_);
    }

    bool insert_win64_stack_probe_call(std::uint32_t frame_bytes) {
        const std::size_t insertion = stack_allocation_opcode_offset_;
        if (insertion > code_.size() || frame_bytes < 4096U) {
            add_diag(*diagnostics_, "XN106C0003", "Win64 stack-probe insertion point or frame size is invalid", function_.span);
            return false;
        }

        // The Microsoft x64 prologue contract passes the fixed allocation size
        // in RAX, calls __chkstk before changing RSP, then executes SUB RSP,RAX.
        std::vector<std::uint8_t> probe_code;
        probe_code.reserve(10U);
        probe_code.push_back(0xB8U); // mov eax, imm32 (zero-extends into RAX)
        probe_code.push_back(static_cast<std::uint8_t>(frame_bytes & 0xFFU));
        probe_code.push_back(static_cast<std::uint8_t>((frame_bytes >> 8U) & 0xFFU));
        probe_code.push_back(static_cast<std::uint8_t>((frame_bytes >> 16U) & 0xFFU));
        probe_code.push_back(static_cast<std::uint8_t>((frame_bytes >> 24U) & 0xFFU));
        probe_code.push_back(0xE8U); // call rel32 __chkstk
        const std::size_t relocation_offset = insertion + probe_code.size();
        probe_code.insert(probe_code.end(), 4U, 0U);

        const std::size_t shift = probe_code.size();
        if (code_.size() + shift > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
            add_diag(*diagnostics_, "XN106C0004", "stack-probe insertion exceeds the 32-bit function-offset format", function_.span);
            return false;
        }
        const auto shift_offset = [insertion, shift](std::size_t& offset) {
            if (offset >= insertion) offset += shift;
        };
        shift_offset(frame_size_patch_);
        shift_offset(stack_allocation_opcode_offset_);
        shift_offset(prologue_end_offset_);
        for (auto& fixup : prologue_save_fixups_) {
            shift_offset(fixup.displacement_offset);
            shift_offset(fixup.instruction_end_offset);
        }
        for (auto& fixup : epilogue_restore_fixups_) {
            shift_offset(fixup.displacement_offset);
            shift_offset(fixup.instruction_end_offset);
        }
        for (auto& fixup : spill_displacement_fixups_) shift_offset(fixup.first);
        for (auto& fixup : relocations_) shift_offset(fixup.offset);
        for (auto& fixup : epilogue_fixups_) shift_offset(fixup);
        for (auto& fixup : relative_branch_fixups_) {
            if (fixup.displacement_offset >= insertion)
                fixup.displacement_offset += static_cast<std::uint32_t>(shift);
            if (fixup.target_offset >= insertion)
                fixup.target_offset += static_cast<std::uint32_t>(shift);
        }
        for (auto& anchor : emitted_flow_anchors_) {
            if (anchor.machine_code_offset >= insertion)
                anchor.machine_code_offset += static_cast<std::uint32_t>(shift);
            if (anchor.machine_code_before_offset != InvalidNativeFlowOperationIndex &&
                anchor.machine_code_before_offset >= insertion)
                anchor.machine_code_before_offset += static_cast<std::uint32_t>(shift);
            if (anchor.machine_code_after_call_offset != InvalidNativeFlowOperationIndex &&
                anchor.machine_code_after_call_offset >= insertion)
                anchor.machine_code_after_call_offset += static_cast<std::uint32_t>(shift);
            if (anchor.machine_code_call_instruction_offset != InvalidNativeFlowOperationIndex &&
                anchor.machine_code_call_instruction_offset >= insertion)
                anchor.machine_code_call_instruction_offset += static_cast<std::uint32_t>(shift);
        }
        for (auto& anchor : emitted_flow_branch_anchors_) {
            if (anchor.instruction_offset >= insertion)
                anchor.instruction_offset += static_cast<std::uint32_t>(shift);
            if (anchor.displacement_offset != InvalidNativeFlowOperationIndex &&
                anchor.displacement_offset >= insertion)
                anchor.displacement_offset += static_cast<std::uint32_t>(shift);
            if (anchor.target_offset >= insertion)
                anchor.target_offset += static_cast<std::uint32_t>(shift);
        }

        code_.insert(code_.begin() + static_cast<std::ptrdiff_t>(insertion),
                     probe_code.begin(), probe_code.end());

        NativeObjectRelocationInput relocation;
        relocation.offset = relocation_offset;
        relocation.kind = ObjectRelocationKind::CoffRelative32;
        relocation.width = 4U;
        relocation.target_link_name = "__chkstk";
        relocation.target_signature_key = "win64-internal::__chkstk:rax-fixed-frame-v1";
        relocation.target_kind = ObjectSymbolKind::Function;
        relocations_.push_back(std::move(relocation));
        return true;
    }

    bool patch_frame_save_fixups() {
        const auto patch_group = [&](const std::vector<FrameDisplacementFixup>& fixups) -> bool {
            for (const auto& fixup : fixups) {
                const auto found = std::find_if(frame_plan_.prologue_saves.begin(), frame_plan_.prologue_saves.end(),
                    [&](const NativeWin64RegisterSave& save) {
                        return save.register_class == fixup.register_class && save.register_index == fixup.register_index;
                    });
                if (found == frame_plan_.prologue_saves.end()) {
                    add_diag(*diagnostics_, "XN75C0001", "frame save fixup has no matching Win64 save slot", function_.span);
                    return false;
                }
                patch_u32(fixup.displacement_offset, static_cast<std::uint32_t>(found->frame_offset));
            }
            return true;
        };
        if (!patch_group(prologue_save_fixups_)) return false;
        for (const auto& fixup : epilogue_restore_fixups_) {
            const auto found = std::find_if(frame_plan_.epilogue_restores.begin(), frame_plan_.epilogue_restores.end(),
                [&](const NativeWin64RegisterSave& save) {
                    return save.register_class == fixup.register_class && save.register_index == fixup.register_index;
                });
            if (found == frame_plan_.epilogue_restores.end()) {
                add_diag(*diagnostics_, "XN75C0002", "epilogue restore fixup has no matching Win64 restore slot", function_.span);
                return false;
            }
            patch_u32(fixup.displacement_offset, static_cast<std::uint32_t>(found->frame_offset));
        }
        return true;
    }

    bool patch_spill_slot_fixups() {
        for (const auto& fixup : spill_displacement_fixups_) {
            if (fixup.second >= frame_plan_.spill_slot_offsets.size()) {
                add_diag(*diagnostics_, "XN74C0001", "spill memory reference exceeds the Win64 frame spill area", function_.span);
                return false;
            }
            patch_u32(fixup.first, static_cast<std::uint32_t>(frame_plan_.spill_slot_offsets[fixup.second]));
        }
        return true;
    }

    void emit_store_rax_spill(std::uint32_t slot) {
        code_.insert(code_.end(), {0x48U, 0x89U, 0x85U});
        const std::size_t displacement = code_.size();
        emit_u32(0U);
        spill_displacement_fixups_.emplace_back(displacement, slot);
    }

    void emit_load_rax_spill(std::uint32_t slot) {
        code_.insert(code_.end(), {0x48U, 0x8BU, 0x85U});
        const std::size_t displacement = code_.size();
        emit_u32(0U);
        spill_displacement_fixups_.emplace_back(displacement, slot);
    }

    void emit_store_xmm0_spill(std::uint32_t slot, bool single) {
        code_.push_back(single ? 0xF3U : 0xF2U);
        code_.insert(code_.end(), {0x0FU, 0x11U, 0x85U});
        const std::size_t displacement = code_.size();
        emit_u32(0U);
        spill_displacement_fixups_.emplace_back(displacement, slot);
    }

    void emit_load_xmm1_spill(std::uint32_t slot, bool single) {
        code_.push_back(single ? 0xF3U : 0xF2U);
        code_.insert(code_.end(), {0x0FU, 0x10U, 0x8DU});
        const std::size_t displacement = code_.size();
        emit_u32(0U);
        spill_displacement_fixups_.emplace_back(displacement, slot);
    }

    void emit_move_rax_to_volatile_register(std::uint8_t register_index) {
        if (!(register_index == 3U || register_index == 6U || register_index == 7U ||
              (register_index >= 8U && register_index <= 15U))) {
            add_diag(*diagnostics_, "XN54C0002", "allocator selected an unsupported Win64 GP register", function_.span);
            return;
        }
        code_.push_back(static_cast<std::uint8_t>(0x48U | (register_index >= 8U ? 0x01U : 0U)));
        code_.insert(code_.end(), {0x89U, static_cast<std::uint8_t>(0xC0U | (register_index & 7U))});
    }

    void emit_move_volatile_register_to_rax(std::uint8_t register_index) {
        if (!(register_index == 3U || register_index == 6U || register_index == 7U ||
              (register_index >= 8U && register_index <= 15U))) {
            add_diag(*diagnostics_, "XN54C0002", "allocator selected an unsupported Win64 GP register", function_.span);
            return;
        }
        code_.push_back(static_cast<std::uint8_t>(0x48U | (register_index >= 8U ? 0x04U : 0U)));
        code_.insert(code_.end(), {0x89U, static_cast<std::uint8_t>(0xC0U | ((register_index & 7U) << 3U))});
    }

    void emit_move_xmm(std::uint8_t destination, std::uint8_t source) {
        if (destination > 15U || source > 15U) {
            add_diag(*diagnostics_, "XN55C0001", "XMM temporary allocation exceeds the architectural XMM0-XMM15 range", function_.span);
            return;
        }
        // MOVAPS xmm(destination), xmm(source), with REX extension for XMM8-XMM15.
        const std::uint8_t rex = static_cast<std::uint8_t>(0x40U |
            (destination >= 8U ? 0x04U : 0U) | (source >= 8U ? 0x01U : 0U));
        if (rex != 0x40U) code_.push_back(rex);
        code_.insert(code_.end(), {0x0FU, 0x28U,
            static_cast<std::uint8_t>(0xC0U | ((destination & 7U) << 3U) | (source & 7U))});
    }

    bool get_floating_temporary(const Expression& expression, std::uint8_t& register_index) {
        const auto found = expression_registers_.find(&expression);
        if (found == expression_registers_.end() || found->second.register_class != NativeRegisterClass::FloatingPoint ||
            found->second.spilled) return false;
        if (found->second.register_index < 1U || found->second.register_index > 15U) {
            add_diag(*diagnostics_, "XN55C0002", "allocator assigned the reserved XMM0 or an unsupported XMM register", expression.span);
            return false;
        }
        register_index = found->second.register_index;
        return true;
    }

    static bool guaranteed_return(const Statement& statement) {
        if (statement.kind == StatementKind::Return) return true;
        if (statement.kind == StatementKind::Block) {
            for (const auto& child : statement.statements) {
                if (child && guaranteed_return(*child)) return true;
            }
            return false;
        }
        if (statement.kind == StatementKind::If) {
            if (!statement.else_branch || statement.statements.empty() || !statement.statements.front()) return false;
            return guaranteed_return(*statement.statements.front()) && guaranteed_return(*statement.else_branch);
        }
        return false;
    }

    bool supported_function_signature() {
        if (!function_.generic_parameters.empty()) {
            add_diag(*diagnostics_, "XN48C0005", "generic functions must be specialized before N48 x64 code generation", function_.span);
            return false;
        }
        if (fold_type(function_.return_type).empty()) {
            add_diag(*diagnostics_, "XN48C0006", "N48 initial native backend requires an explicit return type", function_.span);
            return false;
        }
        if (fold_type(function_.return_type) != "void" && function_.body && !guaranteed_return(*function_.body)) {
            add_diag(*diagnostics_, "XN48C0006A", "non-void N48 function must return a value on every control-flow path", function_.span);
            return false;
        }
        if (!(is_integer_type(function_.return_type) || is_boolean_type(function_.return_type) ||
              is_float_type(function_.return_type) || is_pointer_type(function_.return_type) ||
              fold_type(function_.return_type) == "void")) {
            add_diag(*diagnostics_, "XN49C0006", "native backend supports int64, bool, float, double, pointers and void return types", function_.span);
            return false;
        }
        if (function_.parameters.size() > 4U) {
            add_diag(*diagnostics_, "XN48C0007", "N48 Win64 initial ABI subset supports at most four register parameters", function_.span);
            return false;
        }
        for (const auto& parameter : function_.parameters) {
            if (!(is_integer_type(parameter.type_name) || is_boolean_type(parameter.type_name) ||
                  is_float_type(parameter.type_name) || is_pointer_type(parameter.type_name))) {
                add_diag(*diagnostics_, "XN49C0008", "native backend parameter type is outside the supported scalar ABI subset", parameter.span);
                return false;
            }
            if (parameter.default_value) {
                add_diag(*diagnostics_, "XN48C0009", "default arguments are not yet lowered by N48", parameter.span);
                return false;
            }
        }
        return true;
    }

    void emit_u32(std::uint32_t value) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U)
            code_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }

    void emit_u64(std::uint64_t value) {
        for (unsigned shift = 0U; shift < 64U; shift += 8U)
            code_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }

    void patch_u32(std::size_t offset, std::uint32_t value) {
        for (unsigned shift = 0U; shift < 32U; shift += 8U)
            code_[offset + shift / 8U] = static_cast<std::uint8_t>((value >> shift) & 0xffU);
    }

    bool patch_rel32(std::size_t displacement_offset, std::size_t target) {
        const std::int64_t delta = static_cast<std::int64_t>(target) -
            static_cast<std::int64_t>(displacement_offset + 4U);
        if (delta < std::numeric_limits<std::int32_t>::min() ||
            delta > std::numeric_limits<std::int32_t>::max()) return false;
        patch_u32(displacement_offset, static_cast<std::uint32_t>(static_cast<std::int32_t>(delta)));
        if (displacement_offset > std::numeric_limits<std::uint32_t>::max() ||
            target > std::numeric_limits<std::uint32_t>::max()) return false;
        const auto existing = std::find_if(relative_branch_fixups_.begin(), relative_branch_fixups_.end(),
            [displacement_offset](const NativeX64RelativeBranchFixup& item) {
                return item.displacement_offset == displacement_offset;
            });
        const NativeX64RelativeBranchFixup fixup{static_cast<std::uint32_t>(displacement_offset),
                                                 static_cast<std::uint32_t>(target)};
        if (existing == relative_branch_fixups_.end()) relative_branch_fixups_.push_back(fixup);
        else *existing = fixup;
        return true;
    }

    std::size_t emit_jump(std::uint8_t condition_opcode = 0U) {
        if (condition_opcode == 0U) {
            code_.push_back(0xE9U);
        } else {
            code_.push_back(0x0FU);
            code_.push_back(condition_opcode);
        }
        const std::size_t displacement = code_.size();
        emit_u32(0U);
        return displacement;
    }

    void emit_xor_eax_eax() { code_.insert(code_.end(), {0x31U, 0xC0U}); }
    void emit_mov_rax_immediate(std::uint64_t value) {
        code_.insert(code_.end(), {0x48U, 0xB8U});
        emit_u64(value);
    }

    std::int32_t local_displacement(std::size_t slot) const {
        return -static_cast<std::int32_t>((slot + 1U) * 8U);
    }

    void emit_store_argument_register(std::size_t argument_index, std::size_t slot) {
        static const std::uint8_t opcodes[][3] = {
            {0x48U, 0x89U, 0x8DU}, // rcx -> [rbp+disp32]
            {0x48U, 0x89U, 0x95U}, // rdx
            {0x4CU, 0x89U, 0x85U}, // r8
            {0x4CU, 0x89U, 0x8DU}  // r9
        };
        if (argument_index >= 4U) {
            add_diag(*diagnostics_, "XN49C000A", "Win64 stack-passed arguments are not yet supported", function_.span);
            return;
        }
        code_.insert(code_.end(), std::begin(opcodes[argument_index]), std::end(opcodes[argument_index]));
        emit_u32(static_cast<std::uint32_t>(local_displacement(slot)));
    }

    void emit_store_float_argument_register(std::size_t argument_index, std::size_t slot, bool single) {
        if (argument_index >= 4U) {
            add_diag(*diagnostics_, "XN49C000A", "Win64 stack-passed floating arguments are not yet supported", function_.span);
            return;
        }
        code_.push_back(single ? 0xF3U : 0xF2U);
        code_.insert(code_.end(), {0x0FU, 0x11U, static_cast<std::uint8_t>(0x85U | (argument_index << 3U))});
        emit_u32(static_cast<std::uint32_t>(local_displacement(slot)));
    }

    std::size_t allocate_local(const std::string& name, const std::string& type, const SourceSpan& span) {
        if (scopes_.empty()) scopes_.emplace_back();
        const std::string key = fold_name(name);
        if (scopes_.back().find(key) != scopes_.back().end()) {
            add_diag(*diagnostics_, "XN48C0010", "duplicate local variable in the same lexical scope", span);
            return std::numeric_limits<std::size_t>::max();
        }
        if (next_slot_ >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) / 8U) {
            add_diag(*diagnostics_, "XN48C0010A", "local stack frame exceeds the supported signed displacement range", span);
            return std::numeric_limits<std::size_t>::max();
        }
        std::size_t slot = 0U;
        if (!free_local_slots_.empty()) {
            slot = free_local_slots_.back();
            free_local_slots_.pop_back();
            ++stats_.local_stack_slots_reused;
        } else {
            slot = next_slot_++;
        }
        if (slot >= slot_types_.size()) slot_types_.resize(slot + 1U);
        slot_types_[slot] = type;
        next_slot_ = std::max(next_slot_, slot + 1U);
        if (!record_flow_home(name, span, slot)) return std::numeric_limits<std::size_t>::max();
        scopes_.back().emplace(key, slot);
        if (scope_slots_.empty()) scope_slots_.emplace_back();
        scope_slots_.back().push_back(slot);
        return slot;
    }

    bool find_local(const std::string& name, std::size_t& slot) const {
        const std::string key = fold_name(name);
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            const auto found = scope->find(key);
            if (found != scope->end()) {
                slot = found->second;
                return true;
            }
        }
        return false;
    }

    std::string local_type(std::size_t slot) const {
        return slot < slot_types_.size() ? slot_types_[slot] : "int";
    }

    void emit_local_register_load(std::size_t slot, const std::string& type) {
        const auto found = local_register_locations_.find(slot);
        if (found == local_register_locations_.end()) {
            if (is_float_type(type)) emit_load_xmm0_local(slot, is_single_float_type(type));
            else emit_load_local(slot);
            return;
        }
        if (is_float_type(type)) emit_move_xmm(0U, found->second.register_index);
        else emit_move_volatile_register_to_rax(found->second.register_index);
        ++stats_.native_persistent_register_reads;
    }

    void emit_local_register_store_from_result(std::size_t slot, const std::string& type) {
        const auto found = local_register_locations_.find(slot);
        if (found == local_register_locations_.end()) return;
        if (is_float_type(type)) emit_move_xmm(found->second.register_index, 0U);
        else emit_move_rax_to_volatile_register(found->second.register_index);
        ++stats_.native_persistent_register_writes;
        // Register-resident, non-address-taken locals use the register as the
        // authoritative home. Avoid emitting a duplicate RBP-relative mirror
        // store; address-taken locals are excluded from register promotion.
        ++stats_.native_persistent_home_mirror_stores_elided;
    }

    void emit_local_register_initialize_from_home(std::size_t slot, const std::string& type) {
        const auto found = local_register_locations_.find(slot);
        if (found == local_register_locations_.end()) return;
        if (is_float_type(type)) {
            emit_load_xmm0_local(slot, is_single_float_type(type));
            emit_move_xmm(found->second.register_index, 0U);
        } else {
            emit_load_local(slot);
            emit_move_rax_to_volatile_register(found->second.register_index);
        }
        ++stats_.native_persistent_register_writes;
    }

    void emit_load_local(std::size_t slot) {
        code_.insert(code_.end(), {0x48U, 0x8BU, 0x85U});
        emit_u32(static_cast<std::uint32_t>(local_displacement(slot)));
    }

    void emit_store_rax_local(std::size_t slot) {
        code_.insert(code_.end(), {0x48U, 0x89U, 0x85U});
        emit_u32(static_cast<std::uint32_t>(local_displacement(slot)));
    }

    void emit_load_xmm0_local(std::size_t slot, bool single) {
        code_.push_back(single ? 0xF3U : 0xF2U);
        code_.insert(code_.end(), {0x0FU, 0x10U, 0x85U});
        emit_u32(static_cast<std::uint32_t>(local_displacement(slot)));
    }

    void emit_store_xmm0_local(std::size_t slot, bool single) {
        code_.push_back(single ? 0xF3U : 0xF2U);
        code_.insert(code_.end(), {0x0FU, 0x11U, 0x85U});
        emit_u32(static_cast<std::uint32_t>(local_displacement(slot)));
    }

    void emit_convert_xmm0(bool from_single, bool to_single) {
        if (from_single == to_single) return;
        if (to_single) code_.insert(code_.end(), {0xF2U, 0x0FU, 0x5AU, 0xC0U}); // cvtsd2ss xmm0,xmm0
        else code_.insert(code_.end(), {0xF3U, 0x0FU, 0x5AU, 0xC0U}); // cvtss2sd xmm0,xmm0
    }

    void emit_convert_expression_to_float(const std::string& source_type, bool target_single) {
        if (is_float_type(source_type)) {
            emit_convert_xmm0(is_single_float_type(source_type), target_single);
        } else {
            // Convert the current signed 64-bit integer in RAX into XMM0.
            code_.push_back(target_single ? 0xF3U : 0xF2U);
            code_.insert(code_.end(), {0x48U, 0x0FU, 0x2AU, 0xC0U});
        }
    }

    std::string expression_type(const Expression& expression) const {
        if (expression.kind == ExprKind::Literal) {
            if (expression.text == "true" || expression.text == "false" || expression.text == "TRUE" || expression.text == "FALSE") return "bool";
            return expression.text.find('.') != std::string::npos ? "double" : "int";
        }
        if (expression.kind == ExprKind::Identifier) {
            std::size_t slot = 0U;
            return find_local(expression.text, slot) ? local_type(slot) : "<unknown>";
        }
        if (expression.kind == ExprKind::Unary && !expression.children.empty()) {
            if (expression.text == "&") return expression_type(*expression.children[0]) + "*";
            if (expression.text == "*") return pointee_type(expression_type(*expression.children[0]));
            return expression_type(*expression.children[0]);
        }
        if (expression.kind == ExprKind::Binary && expression.children.size() == 2U) {
            const std::string operation = expression.text;
            if (operation == "==" || operation == "!=" || operation == "<" || operation == "<=" || operation == ">" || operation == ">=" || operation == "&&" || operation == "||") return "bool";
            const std::string left = expression_type(*expression.children[0]);
            const std::string right = expression_type(*expression.children[1]);
            if (is_double_float_type(left) || is_double_float_type(right)) return "double";
            if (is_single_float_type(left) || is_single_float_type(right)) return "float";
            return left;
        }
        if (expression.kind == ExprKind::Call && !expression.children.empty()) {
            const Expression& callee = *expression.children[0];
            if (callee.kind == ExprKind::Identifier) {
                for (const auto& function : unit_.functions)
                    if (fold_name(function.name) == fold_name(callee.text) && function.parameters.size() == expression.children.size() - 1U)
                        return function.return_type;
            } else if (callee.kind == ExprKind::Member && !callee.children.empty() && callee.children[0]->kind == ExprKind::Identifier) {
                for (const auto& imported : options_.imported_functions)
                    if (fold_name(imported.module_name) == fold_name(callee.children[0]->text) &&
                        fold_name(imported.name) == fold_name(callee.text)) return imported.return_type;
            }
        }
        return "<unknown>";
    }

    void emit_double_immediate(double value) {
        std::uint64_t bits = 0U;
        static_assert(sizeof(bits) == sizeof(value), "double and uint64 sizes must match");
        std::memcpy(&bits, &value, sizeof(bits));
        emit_mov_rax_immediate(bits);
        code_.insert(code_.end(), {0x66U, 0x48U, 0x0FU, 0x6EU, 0xC0U}); // movq xmm0,rax
    }

    struct ResolvedCallee {
        std::size_t function_index = InvalidNativeFunctionIndex;
        std::string link_name;
        std::string signature_key;
        std::string return_type;
        std::vector<std::string> parameter_types;
        bool external = false;
    };

    bool resolve_call(const Expression& expression, ResolvedCallee& target) {
        if (expression.children.empty() || !expression.children[0]) {
            add_diag(*diagnostics_, "XN48C0011", "malformed function call", expression.span);
            return false;
        }
        const Expression& callee = *expression.children[0];
        const std::size_t argument_count = expression.children.size() - 1U;
        if (callee.kind == ExprKind::Identifier) {
            const std::string name = fold_name(callee.text);
            std::vector<std::size_t> matches;
            for (std::size_t index = 0U; index < unit_.functions.size(); ++index) {
                const auto& candidate = unit_.functions[index];
                if (fold_name(candidate.name) == name && candidate.parameters.size() == argument_count) matches.push_back(index);
            }
            if (matches.size() != 1U) {
                add_diag(*diagnostics_, "XN48C0012", matches.empty()
                    ? "called function was not declared in this compilation unit"
                    : "native backend cannot select this overload unambiguously", expression.span);
                return false;
            }
            target.function_index = matches.front();
            target.link_name = mangled_names_[target.function_index];
            const auto& function = unit_.functions[target.function_index];
            target.return_type = function.return_type;
            for (const auto& parameter : function.parameters) target.parameter_types.push_back(parameter.type_name);
            if (target.link_name.empty()) {
                add_diag(*diagnostics_, "XN48C0013", "called function has no resolved native symbol", expression.span);
                return false;
            }
            return true;
        }
        if (callee.kind == ExprKind::Member && !callee.children.empty() && callee.children[0] &&
            callee.children[0]->kind == ExprKind::Identifier) {
            const std::string module_name = fold_name(callee.children[0]->text);
            const std::string function_name = fold_name(callee.text);
            const bool imported = std::any_of(unit_.imports.begin(), unit_.imports.end(), [&](const std::string& name) {
                return fold_name(name) == module_name;
            });
            if (!imported) {
                add_diag(*diagnostics_, "XN48C0011A", "qualified call must target an explicitly imported module", expression.span);
                return false;
            }
            for (const auto& item : options_.imported_functions) {
                if (fold_name(item.module_name) == module_name && fold_name(item.name) == function_name && item.parameter_types.size() == argument_count) {
                    target.external = true;
                    target.link_name = item.mangled_name;
                    target.signature_key = item.signature_key;
                    target.return_type = item.return_type;
                    target.parameter_types = item.parameter_types;
                    return true;
                }
            }
            add_diag(*diagnostics_, "XN48C0012A", "imported function is not available in the typed module interface", expression.span);
            return false;
        }
        add_diag(*diagnostics_, "XN48C0011", "call target must be a local function name or qualified imported module function", expression.span);
        return false;
    }

    void emit_call(const Expression& expression,
                   std::size_t* machine_code_before_offset = nullptr,
                   std::size_t* machine_code_after_call_offset = nullptr,
                   std::size_t* machine_code_call_instruction_offset = nullptr) {
        ResolvedCallee target;
        if (!resolve_call(expression, target)) return;
        const std::size_t count = expression.children.size() - 1U;
        if (count > 4U) {
            add_diag(*diagnostics_, "XN48C0014", "Win64 scalar ABI currently supports at most four register arguments", expression.span);
            return;
        }
        if (machine_code_before_offset != nullptr) *machine_code_before_offset = code_.size();
        for (std::size_t index = 0U; index < count; ++index) {
            compile_expression(*expression.children[index + 1U]);
            if (!diagnostics_->empty()) return;
            const bool parameter_float = is_float_type(target.parameter_types[index]);
            if (parameter_float) {
                const bool target_single = is_single_float_type(target.parameter_types[index]);
                emit_convert_expression_to_float(expression_type(*expression.children[index + 1U]), target_single);
                code_.insert(code_.end(), {0x48U, 0x83U, 0xECU, 0x08U});
                code_.push_back(target_single ? 0xF3U : 0xF2U);
                code_.insert(code_.end(), {0x0FU, 0x11U, 0x04U, 0x24U});
            } else {
                code_.push_back(0x50U);
            }
            ++temporary_depth_;
        }
        const std::size_t padding = (temporary_depth_ % 2U) == 0U ? 0U : 8U;
        const std::size_t call_area = 32U + padding;
        code_.insert(code_.end(), {0x48U, 0x83U, 0xECU, static_cast<std::uint8_t>(call_area)});
        static const std::uint8_t gp_load[][4] = {
            {0x48U, 0x8BU, 0x8CU, 0x24U}, {0x48U, 0x8BU, 0x94U, 0x24U},
            {0x4CU, 0x8BU, 0x84U, 0x24U}, {0x4CU, 0x8BU, 0x8CU, 0x24U}};
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t stack_offset = call_area + (count - 1U - index) * 8U;
            if (is_float_type(target.parameter_types[index])) {
                code_.push_back(is_single_float_type(target.parameter_types[index]) ? 0xF3U : 0xF2U);
                code_.insert(code_.end(), {0x0FU, 0x10U, static_cast<std::uint8_t>(0x84U | (index << 3U)), 0x24U});
                emit_u32(static_cast<std::uint32_t>(stack_offset));
            } else {
                code_.insert(code_.end(), std::begin(gp_load[index]), std::end(gp_load[index]));
                emit_u32(static_cast<std::uint32_t>(stack_offset));
            }
        }
        if (machine_code_call_instruction_offset != nullptr)
            *machine_code_call_instruction_offset = code_.size();
        code_.push_back(0xE8U);
        NativeObjectRelocationInput relocation;
        relocation.offset = code_.size();
        relocation.kind = ObjectRelocationKind::CoffRelative32;
        relocation.width = 4U;
        relocation.target_kind = ObjectSymbolKind::Function;
        if (target.external) {
            relocation.target_link_name = target.link_name;
            relocation.target_signature_key = target.signature_key;
        } else {
            relocation.target_function_index = target.function_index;
        }
        relocations_.push_back(std::move(relocation));
        emit_u32(0U);
        if (machine_code_after_call_offset != nullptr) *machine_code_after_call_offset = code_.size();
        code_.insert(code_.end(), {0x48U, 0x83U, 0xC4U, static_cast<std::uint8_t>(call_area)});
        if (count != 0U) {
            const std::size_t cleanup = count * 8U;
            code_.insert(code_.end(), {0x48U, 0x83U, 0xC4U, static_cast<std::uint8_t>(cleanup)});
            temporary_depth_ -= count;
        }
    }

    struct ConstantValue {
        bool floating = false;
        std::int64_t integer = 0;
        double real = 0.0;
    };

    static bool checked_add(std::int64_t left, std::int64_t right, std::int64_t& output) {
        if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
            (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) return false;
        output = left + right;
        return true;
    }

    static bool checked_subtract(std::int64_t left, std::int64_t right, std::int64_t& output) {
        if ((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right) ||
            (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) return false;
        output = left - right;
        return true;
    }

    static bool checked_multiply(std::int64_t left, std::int64_t right, std::int64_t& output) {
        if (left == 0 || right == 0) { output = 0; return true; }
        if ((left == -1 && right == std::numeric_limits<std::int64_t>::min()) ||
            (right == -1 && left == std::numeric_limits<std::int64_t>::min())) return false;
        if (left > 0) {
            if (right > 0 && left > std::numeric_limits<std::int64_t>::max() / right) return false;
            if (right < 0 && right < std::numeric_limits<std::int64_t>::min() / left) return false;
        } else {
            if (right > 0 && left < std::numeric_limits<std::int64_t>::min() / right) return false;
            if (right < 0 && left < std::numeric_limits<std::int64_t>::max() / right) return false;
        }
        output = left * right;
        return true;
    }

    bool evaluate_constant(const Expression& expression, ConstantValue& value) const {
        if (expression.kind == ExprKind::Literal) {
            if (expression.text == "true" || expression.text == "TRUE") { value.integer = 1; return true; }
            if (expression.text == "false" || expression.text == "FALSE") { value.integer = 0; return true; }
            if (expression.text.find('.') != std::string::npos) {
                value.floating = true;
                return parse_floating(expression.text, value.real) && std::isfinite(value.real);
            }
            return parse_integer(expression.text, value.integer);
        }
        if (expression.kind == ExprKind::Unary && expression.children.size() == 1U) {
            ConstantValue child;
            if (!evaluate_constant(*expression.children.front(), child)) return false;
            if (expression.text == "!") {
                value.integer = child.floating ? (child.real == 0.0 ? 1 : 0) : (child.integer == 0 ? 1 : 0);
                return true;
            }
            if (expression.text == "+") { value = child; return true; }
            if (expression.text == "-") {
                if (child.floating) { value.floating = true; value.real = -child.real; return std::isfinite(value.real); }
                if (child.integer == std::numeric_limits<std::int64_t>::min()) return false;
                value.integer = -child.integer;
                return true;
            }
            return false;
        }
        if (expression.kind != ExprKind::Binary || expression.children.size() != 2U) return false;
        ConstantValue left, right;
        if (!evaluate_constant(*expression.children[0], left) || !evaluate_constant(*expression.children[1], right)) return false;
        const std::string& op = expression.text;
        const bool comparison = op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
        const bool logical = op == "&&" || op == "||";
        if (left.floating || right.floating) {
            if (!(op == "+" || op == "-" || op == "*" || op == "/" || comparison || logical)) return false;
            const double a = left.floating ? left.real : static_cast<double>(left.integer);
            const double b = right.floating ? right.real : static_cast<double>(right.integer);
            if (op == "/" && b == 0.0) return false;
            if (logical) value.integer = (op == "&&") ? (a != 0.0 && b != 0.0) : (a != 0.0 || b != 0.0);
            else if (comparison) {
                if (op == "==") value.integer = a == b;
                else if (op == "!=") value.integer = a != b;
                else if (op == "<") value.integer = a < b;
                else if (op == "<=") value.integer = a <= b;
                else if (op == ">") value.integer = a > b;
                else value.integer = a >= b;
            } else {
                value.floating = true;
                if (op == "+") value.real = a + b;
                else if (op == "-") value.real = a - b;
                else if (op == "*") value.real = a * b;
                else value.real = a / b;
                if (!std::isfinite(value.real)) return false;
            }
            return true;
        }
        const std::int64_t a = left.integer, b = right.integer;
        if (logical) value.integer = (op == "&&") ? (a != 0 && b != 0) : (a != 0 || b != 0);
        else if (comparison) {
            if (op == "==") value.integer = a == b;
            else if (op == "!=") value.integer = a != b;
            else if (op == "<") value.integer = a < b;
            else if (op == "<=") value.integer = a <= b;
            else if (op == ">") value.integer = a > b;
            else value.integer = a >= b;
        } else if (op == "+") { if (!checked_add(a, b, value.integer)) return false; }
        else if (op == "-") { if (!checked_subtract(a, b, value.integer)) return false; }
        else if (op == "*") { if (!checked_multiply(a, b, value.integer)) return false; }
        else if (op == "/" || op == "%") {
            if (b == 0 || (a == std::numeric_limits<std::int64_t>::min() && b == -1)) return false;
            value.integer = op == "/" ? a / b : a % b;
        } else if (op == "&") value.integer = a & b;
        else if (op == "|") value.integer = a | b;
        else if (op == "^") value.integer = a ^ b;
        else return false;
        return true;
    }

    void compile_expression(const Expression& expression) {
        if (!diagnostics_->empty()) return;
        if (expression.kind == ExprKind::Binary) {
            ConstantValue folded;
            if (evaluate_constant(expression, folded)) {
                if (folded.floating) emit_double_immediate(folded.real);
                else emit_mov_rax_immediate(static_cast<std::uint64_t>(folded.integer));
                ++stats_.constant_folded_expressions;
                return;
            }
        }
        switch (expression.kind) {
        case ExprKind::Literal: {
            if (expression.text == "true" || expression.text == "TRUE") {
                emit_mov_rax_immediate(1U);
            } else if (expression.text == "false" || expression.text == "FALSE") {
                emit_xor_eax_eax();
            } else if (expression.text.find('.') != std::string::npos) {
                double value = 0.0;
                if (!parse_floating(expression.text, value)) {
                    add_diag(*diagnostics_, "XN51C0016", "invalid floating-point literal", expression.span);
                    return;
                }
                emit_double_immediate(value);
            } else {
                std::int64_t value = 0;
                if (!parse_integer(expression.text, value)) {
                    add_diag(*diagnostics_, "XN48C0016", "integer backend does not support this literal form", expression.span);
                    return;
                }
                emit_mov_rax_immediate(static_cast<std::uint64_t>(value));
            }
            return;
        }
        case ExprKind::Identifier: {
            std::size_t slot = 0U;
            if (!find_local(expression.text, slot)) {
                add_diag(*diagnostics_, "XN48C0017", "identifier is not a local variable or parameter in the native backend subset", expression.span);
                return;
            }
            const std::string type = local_type(slot);
            const std::size_t read_machine_code_before = code_.size();
            emit_local_register_load(slot, type);
            record_flow_emitter_anchor(NativeRegisterInstructionOriginKind::FlowRead, expression.span,
                read_machine_code_before, std::numeric_limits<std::size_t>::max(),
                std::numeric_limits<std::size_t>::max(), slot);
            return;
        }
        case ExprKind::Unary:
            if (expression.children.size() != 1U) {
                add_diag(*diagnostics_, "XN48C0018", "malformed unary expression", expression.span);
                return;
            }
            if (expression.text == "&") {
                if (expression.children[0]->kind != ExprKind::Identifier) {
                    add_diag(*diagnostics_, "XN52C0018", "address-of currently requires a local variable identifier", expression.span);
                    return;
                }
                std::size_t slot = 0U;
                if (!find_local(expression.children[0]->text, slot)) {
                    add_diag(*diagnostics_, "XN52C0018A", "address-of target is not a local variable", expression.span);
                    return;
                }
                const std::size_t read_machine_code_before = code_.size();
                code_.insert(code_.end(), {0x48U, 0x8DU, 0x85U});
                emit_u32(static_cast<std::uint32_t>(local_displacement(slot)));
                record_flow_emitter_anchor(NativeRegisterInstructionOriginKind::FlowRead,
                    expression.children[0]->span, read_machine_code_before,
                    std::numeric_limits<std::size_t>::max(),
                    std::numeric_limits<std::size_t>::max(), slot, true);
                return;
            }
            if (expression.text == "*") {
                compile_expression(*expression.children[0]);
                if (diagnostics_->empty()) code_.insert(code_.end(), {0x48U, 0x8BU, 0x00U});
                return;
            }
            compile_expression(*expression.children[0]);
            if (!diagnostics_->empty()) return;
            if (expression.text == "-") {
                if (is_float_type(expression_type(expression))) {
                    if (is_single_float_type(expression_type(expression))) {
                        code_.insert(code_.end(), {0xB8U}); emit_u32(0x80000000U);
                        code_.insert(code_.end(), {0x66U, 0x0FU, 0x6EU, 0xC8U, 0x0FU, 0x57U, 0xC1U});
                    } else {
                        code_.insert(code_.end(), {0x48U, 0xB8U});
                        emit_u64(0x8000000000000000ULL);
                        code_.insert(code_.end(), {0x66U, 0x48U, 0x0FU, 0x6EU, 0xC8U, 0x66U, 0x0FU, 0x57U, 0xC1U});
                    }
                } else code_.insert(code_.end(), {0x48U, 0xF7U, 0xD8U});
            } else if (expression.text == "+") { /* identity */ }
            else if (expression.text == "!") {
                code_.insert(code_.end(), {0x48U, 0x85U, 0xC0U, 0x0FU, 0x94U, 0xC0U, 0x0FU, 0xB6U, 0xC0U});
            } else add_diag(*diagnostics_, "XN48C0019", "unary operator is not supported by the native backend", expression.span);
            return;
        case ExprKind::Binary:
            compile_binary(expression);
            return;
        case ExprKind::Call: {
            std::size_t call_expression_before = std::numeric_limits<std::size_t>::max();
            std::size_t call_instruction_after = std::numeric_limits<std::size_t>::max();
            std::size_t call_instruction_start = std::numeric_limits<std::size_t>::max();
            emit_call(expression, &call_expression_before, &call_instruction_after, &call_instruction_start);
            record_flow_emitter_anchor(NativeRegisterInstructionOriginKind::FlowCall, expression.span,
                call_expression_before, call_instruction_after, call_instruction_start);
            return;
        }
        default:
            add_diag(*diagnostics_, "XN48C001A", "expression kind is not supported by the N48 integer backend", expression.span);
            return;
        }
    }

    void compile_binary(const Expression& expression) {
        if (expression.children.size() != 2U) {
            add_diag(*diagnostics_, "XN48C001B", "malformed binary expression", expression.span);
            return;
        }
        const std::string& operation = expression.text;
        if (operation == "&&" || operation == "||") {
            compile_expression(*expression.children[0]);
            code_.insert(code_.end(), {0x48U, 0x85U, 0xC0U});
            const std::size_t short_jump = emit_jump(operation == "&&" ? 0x84U : 0x85U);
            compile_expression(*expression.children[1]);
            code_.insert(code_.end(), {0x48U, 0x85U, 0xC0U, 0x0FU, 0x95U, 0xC0U, 0x0FU, 0xB6U, 0xC0U});
            const std::size_t end_jump = emit_jump();
            const std::size_t short_label = code_.size();
            if (operation == "&&") emit_xor_eax_eax();
            else emit_mov_rax_immediate(1U);
            const std::size_t end_label = code_.size();
            if (!patch_rel32(short_jump, short_label) || !patch_rel32(end_jump, end_label))
                add_diag(*diagnostics_, "XN48C001C", "logical expression branch exceeds rel32 range", expression.span);
            return;
        }
        const std::string left_type = expression_type(*expression.children[0]);
        const std::string right_type = expression_type(*expression.children[1]);
        if (!is_float_type(left_type) && !is_float_type(right_type)) {
            std::int64_t literal = 0;
            const bool right_literal = expression.children[1]->kind == ExprKind::Literal &&
                parse_integer(expression.children[1]->text, literal);
            const bool left_literal = expression.children[0]->kind == ExprKind::Literal &&
                parse_integer(expression.children[0]->text, literal);
            if ((operation == "+" && right_literal && literal == 0) ||
                (operation == "-" && right_literal && literal == 0) ||
                (operation == "+" && left_literal && literal == 0) ||
                (operation == "*" && right_literal && literal == 1) ||
                (operation == "*" && left_literal && literal == 1)) {
                const std::size_t side = left_literal && (operation == "+" || operation == "*") ? 1U : 0U;
                compile_expression(*expression.children[side]);
                if (diagnostics_->empty()) ++stats_.peephole_optimizations;
                return;
            }
        }
        if (is_float_type(left_type) || is_float_type(right_type)) {
            const bool single = is_single_float_type(left_type) && is_single_float_type(right_type);
            const bool comparison = operation == "==" || operation == "!=" || operation == "<" ||
                operation == "<=" || operation == ">" || operation == ">=";
            if (!(comparison || operation == "+" || operation == "-" || operation == "*" || operation == "/")) {
                add_diag(*diagnostics_, "XN51C001D", "floating-point operator is not supported", expression.span);
                return;
            }
            compile_expression(*expression.children[0]);
            if (!diagnostics_->empty()) return;
            emit_convert_expression_to_float(left_type, single);
            std::uint8_t xmm_temporary = 0U;
            const bool use_xmm_register = get_floating_temporary(expression, xmm_temporary);
            if (!diagnostics_->empty()) return;
            const auto float_allocation = expression_registers_.find(&expression);
            const bool use_frame_spill = !use_xmm_register && float_allocation != expression_registers_.end() &&
                float_allocation->second.spilled && float_allocation->second.register_class == NativeRegisterClass::FloatingPoint;
            if (use_xmm_register) {
                emit_move_xmm(xmm_temporary, 0U);
                active_expression_xmm_registers_.push_back(xmm_temporary);
                ++stats_.xmm_register_temporaries;
            } else if (use_frame_spill) {
                emit_store_xmm0_spill(float_allocation->second.spill_slot, single);
                ++stats_.xmm_stack_spilled_temporaries;
            } else {
                code_.insert(code_.end(), {0x48U, 0x83U, 0xECU, 0x08U});
                code_.push_back(single ? 0xF3U : 0xF2U);
                code_.insert(code_.end(), {0x0FU, 0x11U, 0x04U, 0x24U});
                ++temporary_depth_;
            }
            compile_expression(*expression.children[1]);
            if (use_xmm_register) active_expression_xmm_registers_.pop_back();
            if (!diagnostics_->empty()) return;
            emit_convert_expression_to_float(right_type, single);
            if (use_xmm_register) emit_move_xmm(1U, xmm_temporary);
            else if (use_frame_spill) emit_load_xmm1_spill(float_allocation->second.spill_slot, single);
            else {
                code_.push_back(single ? 0xF3U : 0xF2U);
                code_.insert(code_.end(), {0x0FU, 0x10U, 0x0CU, 0x24U}); // movss/sd xmm1,[rsp]
            }
            if (comparison) {
                if (single) code_.insert(code_.end(), {0x0FU, 0x2EU, 0xC8U});
                else code_.insert(code_.end(), {0x66U, 0x0FU, 0x2EU, 0xC8U});
                std::uint8_t set_opcode = 0x94U;
                if (operation == "!=") set_opcode = 0x95U;
                else if (operation == "<") set_opcode = 0x92U;
                else if (operation == "<=") set_opcode = 0x96U;
                else if (operation == ">") set_opcode = 0x97U;
                else if (operation == ">=") set_opcode = 0x93U;
                code_.insert(code_.end(), {0x0FU, set_opcode, 0xC0U, 0x0FU, 0xB6U, 0xC0U});
                if (operation == "!=" ) {
                    code_.insert(code_.end(), {0x0FU, 0x9AU, 0xC1U, 0x08U, 0xC8U}); // SETP CL; OR AL,CL
                } else if (operation == "==" || operation == "<" || operation == "<=" || operation == ">=" ) {
                    code_.insert(code_.end(), {0x0FU, 0x9BU, 0xC1U, 0x20U, 0xC8U}); // SETNP CL; AND AL,CL
                }
            } else {
                if (operation == "+") {
                    code_.push_back(single ? 0xF3U : 0xF2U); code_.insert(code_.end(), {0x0FU, 0x58U, 0xC1U});
                } else if (operation == "-") {
                    code_.push_back(single ? 0xF3U : 0xF2U); code_.insert(code_.end(), {0x0FU, 0x5CU, 0xC8U});
                    code_.insert(code_.end(), {0x66U, 0x0FU, 0x28U, 0xC1U}); // movapd xmm0,xmm1
                } else if (operation == "*") {
                    code_.push_back(single ? 0xF3U : 0xF2U); code_.insert(code_.end(), {0x0FU, 0x59U, 0xC1U});
                } else {
                    code_.push_back(single ? 0xF3U : 0xF2U); code_.insert(code_.end(), {0x0FU, 0x5EU, 0xC1U});
                    code_.insert(code_.end(), {0x66U, 0x0FU, 0x28U, 0xC1U});
                }
            }
            if (!use_xmm_register && !use_frame_spill) {
                code_.insert(code_.end(), {0x48U, 0x83U, 0xC4U, 0x08U});
                --temporary_depth_;
            }
            return;
        }
        compile_expression(*expression.children[0]);
        if (!diagnostics_->empty()) return;
        const auto allocation = expression_registers_.find(&expression);
        const bool use_register = allocation != expression_registers_.end() && !allocation->second.spilled;
        const bool use_frame_spill = allocation != expression_registers_.end() && allocation->second.spilled;
        std::uint8_t scratch_register = 0U;
        if (use_register) {
            scratch_register = allocation->second.register_index;
            emit_move_rax_to_volatile_register(scratch_register);
            active_expression_gp_registers_.push_back(scratch_register);
            ++stats_.volatile_register_temporaries;
            stats_.maximum_volatile_register_index = std::max(stats_.maximum_volatile_register_index, scratch_register);
            stats_.stack_temporary_bytes_avoided += 8U;
        } else if (use_frame_spill) {
            emit_store_rax_spill(allocation->second.spill_slot);
            ++stats_.stack_spilled_temporaries;
        } else {
            code_.push_back(0x50U); // conservative fallback when no interval metadata exists
            ++temporary_depth_;
        }
        compile_expression(*expression.children[1]);
        if (use_register) active_expression_gp_registers_.pop_back();
        if (!diagnostics_->empty()) return;
        code_.insert(code_.end(), {0x48U, 0x89U, 0xC1U}); // mov rcx,rax
        if (use_register) emit_move_volatile_register_to_rax(scratch_register);
        else if (use_frame_spill) emit_load_rax_spill(allocation->second.spill_slot);
        else {
            code_.push_back(0x58U); // pop rax
            --temporary_depth_;
        }
        if (operation == "+") code_.insert(code_.end(), {0x48U, 0x01U, 0xC8U});
        else if (operation == "-") code_.insert(code_.end(), {0x48U, 0x29U, 0xC8U});
        else if (operation == "*") code_.insert(code_.end(), {0x48U, 0x0FU, 0xAFU, 0xC1U});
        else if (operation == "/" || operation == "%") {
            code_.insert(code_.end(), {0x48U, 0x99U, 0x48U, 0xF7U, 0xF9U}); // cqo; idiv rcx
            if (operation == "%") code_.insert(code_.end(), {0x48U, 0x89U, 0xD0U}); // mov rax,rdx
        } else if (operation == "&") code_.insert(code_.end(), {0x48U, 0x21U, 0xC8U});
        else if (operation == "|") code_.insert(code_.end(), {0x48U, 0x09U, 0xC8U});
        else if (operation == "^") code_.insert(code_.end(), {0x48U, 0x31U, 0xC8U});
        else if (operation == "<<") code_.insert(code_.end(), {0x48U, 0xD3U, 0xE0U});
        else if (operation == ">>") code_.insert(code_.end(), {0x48U, 0xD3U, 0xF8U});
        else if (operation == "==" || operation == "!=" || operation == "<" || operation == "<=" || operation == ">" || operation == ">=") {
            std::uint8_t set_opcode = 0x94U;
            if (operation == "!=") set_opcode = 0x95U;
            else if (operation == "<") set_opcode = 0x9CU;
            else if (operation == "<=") set_opcode = 0x9EU;
            else if (operation == ">") set_opcode = 0x9FU;
            else if (operation == ">=") set_opcode = 0x9DU;
            code_.insert(code_.end(), {0x48U, 0x39U, 0xC8U, 0x0FU, set_opcode, 0xC0U, 0x0FU, 0xB6U, 0xC0U});
        } else {
            add_diag(*diagnostics_, "XN48C001D", "binary operator is not supported by N48", expression.span);
        }
    }

    void compile_statement(const Statement& statement) {
        if (!diagnostics_->empty()) return;
        switch (statement.kind) {
        case StatementKind::Block:
            scopes_.emplace_back();
            scope_slots_.emplace_back();
            for (const auto& child : statement.statements) if (child) compile_statement(*child);
            for (const std::size_t slot : scope_slots_.back()) free_local_slots_.push_back(slot);
            scope_slots_.pop_back();
            scopes_.pop_back();
            return;
        case StatementKind::VariableDeclaration: {
            if (!(is_integer_type(statement.type_name) || is_boolean_type(statement.type_name) ||
                  is_float_type(statement.type_name) || is_pointer_type(statement.type_name))) {
                add_diag(*diagnostics_, "XN49C001E", "local declaration uses a type outside the native scalar subset", statement.span);
                return;
            }
            const std::size_t slot = allocate_local(statement.name, statement.type_name, statement.span);
            if (slot == std::numeric_limits<std::size_t>::max()) return;
            if (statement.expressions.empty()) {
                if (is_float_type(statement.type_name)) {
                    code_.insert(code_.end(), {0x0FU, 0x57U, 0xC0U}); // xorps xmm0,xmm0
                } else emit_xor_eax_eax();
            } else {
                compile_expression(*statement.expressions.front());
                if (is_float_type(statement.type_name)) emit_convert_expression_to_float(expression_type(*statement.expressions.front()), is_single_float_type(statement.type_name));
            }
            if (diagnostics_->empty()) {
                const std::size_t write_machine_code_before = code_.size();
                if (is_float_type(statement.type_name)) emit_store_xmm0_local(slot, is_single_float_type(statement.type_name));
                else emit_store_rax_local(slot);
                emit_local_register_store_from_result(slot, statement.type_name);
                record_flow_emitter_anchor(NativeRegisterInstructionOriginKind::FlowDeclare, statement.span,
                    write_machine_code_before, std::numeric_limits<std::size_t>::max(),
                    std::numeric_limits<std::size_t>::max(), slot);
            }
            return;
        }
        case StatementKind::Assignment: {
            if (statement.expressions.size() != 2U || !statement.expressions[0] || !statement.expressions[1]) {
                add_diag(*diagnostics_, "XN48C001F", "malformed assignment", statement.span);
                return;
            }
            const Expression& target = *statement.expressions[0];
            if (target.kind == ExprKind::Identifier) {
                std::size_t slot = 0U;
                if (!find_local(target.text, slot)) {
                    add_diag(*diagnostics_, "XN48C0020", "assignment target is not a local variable", statement.span);
                    return;
                }
                compile_expression(*statement.expressions[1]);
                if (diagnostics_->empty()) {
                    const std::size_t write_machine_code_before = code_.size();
                    if (is_float_type(local_type(slot))) {
                        emit_convert_expression_to_float(expression_type(*statement.expressions[1]), is_single_float_type(local_type(slot)));
                        emit_store_xmm0_local(slot, is_single_float_type(local_type(slot)));
                    } else emit_store_rax_local(slot);
                    emit_local_register_store_from_result(slot, local_type(slot));
                    record_flow_emitter_anchor(NativeRegisterInstructionOriginKind::FlowWrite, target.span,
                        write_machine_code_before, std::numeric_limits<std::size_t>::max(),
                        std::numeric_limits<std::size_t>::max(), slot);
                }
                return;
            }
            if (target.kind == ExprKind::Unary && target.text == "*" && !target.children.empty()) {
                compile_expression(*target.children[0]);
                if (!diagnostics_->empty()) return;
                code_.push_back(0x50U);
                ++temporary_depth_;
                compile_expression(*statement.expressions[1]);
                if (!diagnostics_->empty()) return;
                code_.insert(code_.end(), {0x48U, 0x89U, 0xC1U, 0x58U, 0x48U, 0x89U, 0x08U});
                --temporary_depth_;
                return;
            }
            add_diag(*diagnostics_, "XN52C001F", "assignment target must be a local or a supported pointer dereference", statement.span);
            return;
        }
        case StatementKind::Expression:
            if (!statement.expressions.empty()) compile_expression(*statement.expressions.front());
            return;
        case StatementKind::Return:
            if (statement.expressions.empty()) {
                if (!(fold_type(function_.return_type).empty() || fold_type(function_.return_type) == "void"))
                    add_diag(*diagnostics_, "XN48C0021", "non-void function requires a return value", statement.span);
            } else {
                if (fold_type(function_.return_type).empty() || fold_type(function_.return_type) == "void") {
                    add_diag(*diagnostics_, "XN48C0022", "void function cannot return a value", statement.span);
                    return;
                }
                compile_expression(*statement.expressions.front());
                if (diagnostics_->empty() && is_float_type(function_.return_type)) {
                    emit_convert_expression_to_float(expression_type(*statement.expressions.front()),
                                                     is_single_float_type(function_.return_type));
                }
            }
            if (diagnostics_->empty()) {
                epilogue_fixups_.push_back(emit_jump());
                record_flow_emitter_anchor(NativeRegisterInstructionOriginKind::FlowReturn, statement.span);
            }
            return;
        case StatementKind::If: {
            if (statement.expressions.size() != 1U || statement.statements.empty() || !statement.statements[0]) {
                add_diag(*diagnostics_, "XN48C0023", "malformed if statement", statement.span);
                return;
            }
            const FlowControlStatementId control_id = flow_control_statement_id(statement);
            if (control_id == InvalidFlowControlStatementId) {
                add_diag(*diagnostics_, "XN109AC000C", "if statement has no stable control identity", statement.span);
                return;
            }
            compile_expression(*statement.expressions.front());
            if (!diagnostics_->empty()) return;
            code_.insert(code_.end(), {0x48U, 0x85U, 0xC0U});
            const std::size_t else_fixup = emit_jump(0x84U);
            const std::size_t conditional_instruction = else_fixup - 2U;
            const std::size_t then_entry = code_.size();
            compile_statement(*statement.statements[0]);
            if (diagnostics_->empty() && statement.else_branch) {
                const std::size_t end_fixup = emit_jump();
                const std::size_t then_merge_instruction = end_fixup - 1U;
                const std::size_t else_label = code_.size();
                if (!patch_rel32(else_fixup, else_label)) add_diag(*diagnostics_, "XN48C0024", "if branch exceeds rel32 range", statement.span);
                if (diagnostics_->empty()) {
                    record_flow_branch_anchor(control_id, FlowEdgeKind::IfTrue,
                        X64FlowBranchEncoding::ConditionalRel32, false, conditional_instruction,
                        else_fixup, then_entry);
                    record_flow_branch_anchor(control_id, FlowEdgeKind::IfFalse,
                        X64FlowBranchEncoding::ConditionalRel32, true, conditional_instruction,
                        else_fixup, else_label);
                    compile_statement(*statement.else_branch);
                }
                const std::size_t end_label = code_.size();
                if (diagnostics_->empty() && !patch_rel32(end_fixup, end_label))
                    add_diag(*diagnostics_, "XN48C0025", "else branch exceeds rel32 range", statement.span);
                if (diagnostics_->empty()) {
                    record_flow_branch_anchor(control_id, FlowEdgeKind::IfThenMerge,
                        X64FlowBranchEncoding::UnconditionalRel32, true, then_merge_instruction,
                        end_fixup, end_label);
                    record_flow_branch_anchor(control_id, FlowEdgeKind::IfElseMerge,
                        X64FlowBranchEncoding::Fallthrough, false, end_label,
                        std::numeric_limits<std::size_t>::max(), end_label);
                }
            } else if (diagnostics_->empty()) {
                const std::size_t join_label = code_.size();
                if (!patch_rel32(else_fixup, join_label)) add_diag(*diagnostics_, "XN48C0026", "if branch exceeds rel32 range", statement.span);
                if (diagnostics_->empty()) {
                    record_flow_branch_anchor(control_id, FlowEdgeKind::IfTrue,
                        X64FlowBranchEncoding::ConditionalRel32, false, conditional_instruction,
                        else_fixup, then_entry);
                    record_flow_branch_anchor(control_id, FlowEdgeKind::IfFalse,
                        X64FlowBranchEncoding::ConditionalRel32, true, conditional_instruction,
                        else_fixup, join_label);
                    record_flow_branch_anchor(control_id, FlowEdgeKind::IfThenMerge,
                        X64FlowBranchEncoding::Fallthrough, false, join_label,
                        std::numeric_limits<std::size_t>::max(), join_label);
                }
            }
            return;
        }
        case StatementKind::While: {
            if (statement.expressions.size() != 1U || statement.statements.empty() || !statement.statements[0]) {
                add_diag(*diagnostics_, "XN48C0027", "malformed while statement", statement.span);
                return;
            }
            const FlowControlStatementId control_id = flow_control_statement_id(statement);
            if (control_id == InvalidFlowControlStatementId) {
                add_diag(*diagnostics_, "XN109AC000D", "while statement has no stable control identity", statement.span);
                return;
            }
            const std::size_t loop_start = code_.size();
            record_flow_branch_anchor(control_id, FlowEdgeKind::LoopEntry,
                X64FlowBranchEncoding::Fallthrough, false, loop_start,
                std::numeric_limits<std::size_t>::max(), loop_start);
            compile_expression(*statement.expressions.front());
            if (!diagnostics_->empty()) return;
            code_.insert(code_.end(), {0x48U, 0x85U, 0xC0U});
            const std::size_t exit_fixup = emit_jump(0x84U);
            const std::size_t conditional_instruction = exit_fixup - 2U;
            const std::size_t body_entry = code_.size();
            loops_.push_back(LoopContext{loop_start, control_id, {}});
            record_flow_branch_anchor(control_id, FlowEdgeKind::LoopBody,
                X64FlowBranchEncoding::ConditionalRel32, false, conditional_instruction,
                exit_fixup, body_entry);
            compile_statement(*statement.statements[0]);
            const std::size_t back_fixup = emit_jump();
            const std::size_t back_instruction = back_fixup - 1U;
            if (!patch_rel32(back_fixup, loop_start)) add_diag(*diagnostics_, "XN48C0028", "while loop branch exceeds rel32 range", statement.span);
            const std::size_t loop_end = code_.size();
            if (!patch_rel32(exit_fixup, loop_end)) add_diag(*diagnostics_, "XN48C0029", "while exit branch exceeds rel32 range", statement.span);
            if (diagnostics_->empty()) {
                record_flow_branch_anchor(control_id, FlowEdgeKind::LoopExit,
                    X64FlowBranchEncoding::ConditionalRel32, true, conditional_instruction,
                    exit_fixup, loop_end);
                record_flow_branch_anchor(control_id, FlowEdgeKind::LoopBack,
                    X64FlowBranchEncoding::UnconditionalRel32, true, back_instruction,
                    back_fixup, loop_start);
            }
            for (const auto& pending : loops_.back().break_displacements) {
                if (!patch_rel32(pending.displacement_offset, loop_end)) {
                    add_diag(*diagnostics_, "XN48C002A", "break branch exceeds rel32 range", statement.span);
                } else if (diagnostics_->empty()) {
                    record_flow_branch_anchor(pending.control_statement_id, FlowEdgeKind::LoopBreak,
                        X64FlowBranchEncoding::UnconditionalRel32, true,
                        pending.displacement_offset - 1U, pending.displacement_offset, loop_end);
                }
            }
            loops_.pop_back();
            return;
        }
        case StatementKind::Break:
            if (loops_.empty()) add_diag(*diagnostics_, "XN48C002B", "break is only valid inside a supported loop", statement.span);
            else {
                const FlowControlStatementId control_id = flow_control_statement_id(statement);
                const std::size_t displacement = emit_jump();
                loops_.back().break_displacements.push_back(PendingBreakBranch{control_id, displacement});
            }
            return;
        case StatementKind::Continue:
            if (loops_.empty()) add_diag(*diagnostics_, "XN48C002C", "continue is only valid inside a supported loop", statement.span);
            else {
                const FlowControlStatementId control_id = flow_control_statement_id(statement);
                const std::size_t jump = emit_jump();
                if (!patch_rel32(jump, loops_.back().continue_target))
                    add_diag(*diagnostics_, "XN48C002D", "continue branch exceeds rel32 range", statement.span);
                else record_flow_branch_anchor(control_id, FlowEdgeKind::LoopContinue,
                    X64FlowBranchEncoding::UnconditionalRel32, true, jump - 1U, jump,
                    loops_.back().continue_target);
            }
            return;
        case StatementKind::ForEach:
            add_diag(*diagnostics_, "XN48C002E", "for-in iteration is not yet lowered by the N48 initial backend", statement.span);
            return;
        }
    }
};

std::vector<NativeSymbolDeclaration> make_declarations(const CompilationUnit& unit) {
    std::vector<NativeSymbolDeclaration> declarations;
    declarations.reserve(unit.functions.size());
    for (std::size_t index = 0U; index < unit.functions.size(); ++index) {
        const auto& function = unit.functions[index];
        if (!function.generic_parameters.empty()) continue;
        NativeSymbolDeclaration declaration;
        declaration.symbol_id = fold_name(unit.module_name) + "#function#" + std::to_string(index);
        declaration.module_name = unit.module_name;
        declaration.source_name = function.name;
        declaration.return_type = function.return_type.empty() ? "void" : function.return_type;
        declaration.visibility = native_visibility(function.visibility);
        declaration.linkage = function.visibility == Visibility::Public
            ? DropSymbolLinkage::External : DropSymbolLinkage::Internal;
        declaration.entity_kind = NativeSymbolEntityKind::Function;
        declaration.defined = function.has_body;
        for (const auto& parameter : function.parameters)
            declaration.parameters.push_back(NativeSymbolParameter{parameter.type_name, DropParameterPassing::Value});
        declarations.push_back(std::move(declaration));
    }
    return declarations;
}

} // namespace

X64CodegenResult compile_x64_coff_object(const CompilationUnit& unit, X64CodegenOptions options) {
    X64CodegenResult result;
    if (options.target.format != ObjectFileFormat::COFF ||
        options.target.architecture != ObjectArchitecture::X86_64 ||
        options.target.operating_system != ObjectOperatingSystem::Windows) {
        add_diag(result.diagnostics, "XN48C1000", "N48 initial backend requires the AMD64 Windows COFF target");
        return result;
    }
    if (!options.skip_semantic_validation) {
        const auto semantic = analyze_semantics(unit);
        if (!semantic.succeeded()) {
            result.diagnostics = semantic.diagnostics;
            return result;
        }
    }
    const auto declarations = make_declarations(unit);
    const auto symbols = build_native_symbol_table(declarations);
    if (!symbols.succeeded()) {
        result.diagnostics = symbols.diagnostics;
        return result;
    }
    std::vector<std::string> mangled_names(unit.functions.size());
    for (std::size_t index = 0U; index < unit.functions.size(); ++index) {
        if (!unit.functions[index].generic_parameters.empty()) continue;
        const std::string symbol_id = fold_name(unit.module_name) + "#function#" + std::to_string(index);
        const auto found = std::find_if(symbols.symbols.begin(), symbols.symbols.end(), [&symbol_id](const NativeSymbolRecord& record) {
            return record.symbol_id == symbol_id;
        });
        if (found != symbols.symbols.end()) mangled_names[index] = found->mangled_name;
    }
    const auto native_flow = lower_control_flow_to_native_register_ir(unit);
    if (!native_flow.succeeded) {
        for (const auto& diagnostic : native_flow.diagnostics)
            add_diag(result.diagnostics, diagnostic.code.c_str(), diagnostic.message, {});
        return result;
    }
    if (native_flow.functions.size() != static_cast<std::size_t>(std::count_if(
            unit.functions.begin(), unit.functions.end(), [](const FunctionDeclaration& function) {
                return function.has_body && static_cast<bool>(function.body);
            }))) {
        add_diag(result.diagnostics, "XN86C0002", "frontend CFG function count does not match source function bodies");
        return result;
    }
    std::vector<NativeRegisterFlowAllocationResult> flow_allocations;
    flow_allocations.reserve(native_flow.functions.size());
    for (const auto& function : native_flow.functions) {
        auto allocation = analyze_and_allocate_native_flow_function(function);
        if (!allocation.succeeded) {
            const std::string detail = allocation.diagnostics.empty() ? "unknown flow allocation failure" :
                allocation.diagnostics.front().code + " " + allocation.diagnostics.front().message;
            add_diag(result.diagnostics, "XN86C0003", "CFG-derived persistent-value allocation failed: " + detail);
            return result;
        }
        flow_allocations.push_back(std::move(allocation));
    }

    std::vector<NativeFunctionCodeArtifact> artifacts;
    std::size_t flow_function_index = 0U;
    for (std::size_t index = 0U; index < unit.functions.size(); ++index) {
        if (!unit.functions[index].has_body || !unit.functions[index].body) continue;
        NativeFunctionCodeArtifact artifact;
        FunctionEmitter emitter(unit, mangled_names, options, index,
            &flow_allocations[flow_function_index], &native_flow.functions[flow_function_index], result.stats);
        ++flow_function_index;
        if (emitter.run(artifact, result.diagnostics, result.flow_anchors,
                        result.flow_branch_anchors))
            artifacts.push_back(std::move(artifact));
    }
    if (!result.diagnostics.empty()) return result;
    std::sort(result.flow_anchors.begin(), result.flow_anchors.end(),
        [](const X64FlowEmitterAnchor& left, const X64FlowEmitterAnchor& right) {
            if (left.function_index != right.function_index) return left.function_index < right.function_index;
            if (left.flow_block_id != right.flow_block_id) return left.flow_block_id < right.flow_block_id;
            return left.flow_operation_index < right.flow_operation_index;
        });
    std::sort(result.flow_branch_anchors.begin(), result.flow_branch_anchors.end(),
        [](const X64FlowBranchAnchor& left, const X64FlowBranchAnchor& right) {
            if (left.function_index != right.function_index) return left.function_index < right.function_index;
            return left.edge_id < right.edge_id;
        });
    result.native_object = emit_native_object_sections(unit, artifacts, {},
        NativeObjectEmissionOptions{std::move(options.object_id), std::move(options.target)});
    if (!result.native_object.succeeded()) {
        result.diagnostics = result.native_object.diagnostics;
        return result;
    }
    // AMD64 COFF COMDAT sections require a linker-visible external key symbol.
    // X visibility remains enforced by the frontend/module resolver; opaque X
    // mangled names are made external only at the object-file layer so REL32
    // references between independently collectible COMDAT functions can link.
    if (result.native_object.object_file.descriptor.format == ObjectFileFormat::COFF) {
        for (auto& symbol : result.native_object.object_file.symbols) {
            if (symbol.kind == ObjectSymbolKind::Function && symbol.defined &&
                symbol.binding == ObjectSymbolBinding::Local) {
                symbol.binding = ObjectSymbolBinding::Global;
            }
        }
    }
    const auto binary = write_binary_object(result.native_object.object_file);
    if (!binary.succeeded()) {
        result.diagnostics = binary.diagnostics;
        return result;
    }
    result.object_bytes = binary.bytes;
    return result;
}

} // namespace xfrontend
