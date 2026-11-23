#include "xfrontend/native_program_pipeline.hpp"
#include "xfrontend/parser.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace xfrontend;

struct InputModule {
    std::filesystem::path path;
    std::shared_ptr<const CompilationUnit> unit;
};

struct Totals {
    std::uint64_t functions = 0U;
    std::uint64_t edges = 0U;
    std::uint64_t g4_live_values = 0U; // Records on the exported G6/G11 candidate edges.
    std::uint64_t g4_live_values_audited = 0U; // Full G4 CFG audit counter.
    std::uint64_t g6_candidates = 0U;
    std::uint64_t g7_attempts = 0U;
    std::uint64_t g7_successes = 0U;
    std::uint64_t g7_rejections = 0U;
    std::uint64_t g8_attempts = 0U;
    std::uint64_t g8_audit_completions = 0U;
    std::uint64_t g6_anchor_missing = 0U;
    std::uint64_t g6_anchor_inexact = 0U;
    std::uint64_t g11_passed_edges = 0U;
    std::uint64_t g11_blocked_edges = 0U;
};

std::string json_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char ch : value) {
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
                    << static_cast<unsigned int>(ch) << std::dec << std::setfill(' ');
            } else out << static_cast<char>(ch);
            break;
        }
    }
    out << '"';
    return out.str();
}

const char* register_class_name(NativeRegisterClass value) {
    return value == NativeRegisterClass::GeneralPurpose ? "GeneralPurpose" : "FloatingPoint";
}

const char* location_kind_name(NativeRegisterLocationKind value) {
    switch (value) {
    case NativeRegisterLocationKind::Register: return "Register";
    case NativeRegisterLocationKind::SpillSlot: return "SpillSlot";
    case NativeRegisterLocationKind::HomeSlot: return "HomeSlot";
    case NativeRegisterLocationKind::Temporary: return "Temporary";
    }
    return "Unknown";
}

const char* mismatch_name(NativeX64FlowOperandMismatchKind value) {
    switch (value) {
    case NativeX64FlowOperandMismatchKind::None: return "None";
    case NativeX64FlowOperandMismatchKind::RegisterIndexMismatch: return "RegisterIndexMismatch";
    case NativeX64FlowOperandMismatchKind::InvalidRegister: return "InvalidRegister";
    case NativeX64FlowOperandMismatchKind::FlowRegisterEmitterMemory: return "FlowRegisterEmitterMemory";
    case NativeX64FlowOperandMismatchKind::FlowMemoryEmitterRegister: return "FlowMemoryEmitterRegister";
    case NativeX64FlowOperandMismatchKind::FrameDisplacementMismatch: return "FrameDisplacementMismatch";
    case NativeX64FlowOperandMismatchKind::FrameMappingUnavailable: return "FrameMappingUnavailable";
    case NativeX64FlowOperandMismatchKind::EvidenceDisagreement: return "EvidenceDisagreement";
    case NativeX64FlowOperandMismatchKind::UnsupportedLocationKind: return "UnsupportedLocationKind";
    case NativeX64FlowOperandMismatchKind::RegisterClassMismatch: return "RegisterClassMismatch";
    }
    return "Unknown";
}

const char* operand_name(std::uint8_t value) {
    switch (static_cast<X64FlowEmitterAnchor::OperandKind>(value)) {
    case X64FlowEmitterAnchor::OperandKind::None: return "None";
    case X64FlowEmitterAnchor::OperandKind::ValueRead: return "ValueRead";
    case X64FlowEmitterAnchor::OperandKind::AddressRead: return "AddressRead";
    case X64FlowEmitterAnchor::OperandKind::ValueWrite: return "ValueWrite";
    case X64FlowEmitterAnchor::OperandKind::ValueDeclare: return "ValueDeclare";
    }
    return "Unknown";
}

std::string location_json(const NativeRegisterLocation& value) {
    std::ostringstream out;
    out << "{\"kind\":" << json_string(location_kind_name(value.kind))
        << ",\"register_index\":" << static_cast<unsigned int>(value.register_index)
        << ",\"slot_index\":" << value.spill_slot << '}';
    return out.str();
}

std::string g4_value_json(const X64FlowEdgeG4LiveValueDiagnosticReport& value) {
    std::ostringstream out;
    out << '{'
        << "\"value_id\":" << value.value_id << ','
        << "\"register_class\":" << json_string(register_class_name(value.register_class)) << ','
        << "\"predecessor_flow_location\":" << location_json(value.predecessor_flow_location) << ','
        << "\"successor_flow_location\":" << location_json(value.successor_flow_location) << ','
        << "\"emitter_active_location\":" << location_json(value.emitter_active_location) << ','
        << "\"predecessor_uses_frame\":" << (value.predecessor_uses_frame ? "true" : "false") << ','
        << "\"predecessor_frame_displacement\":" << value.predecessor_frame_displacement << ','
        << "\"successor_uses_frame\":" << (value.successor_uses_frame ? "true" : "false") << ','
        << "\"successor_frame_displacement\":" << value.successor_frame_displacement << ','
        << "\"flow_location_changes\":" << (value.flow_location_changes ? "true" : "false") << ','
        << "\"is_g6_candidate\":" << (value.is_g6_candidate ? "true" : "false") << '}';
    return out.str();
}

std::string g6_candidate_json(const X64FlowEdgeG6CandidateDiagnosticReport& value) {
    std::ostringstream out;
    out << '{'
        << "\"value_id\":" << value.value_id << ','
        << "\"register_class\":" << json_string(register_class_name(value.register_class)) << ','
        << "\"source_location\":" << location_json(value.source_location) << ','
        << "\"destination_location\":" << location_json(value.destination_location) << ','
        << "\"emitter_active_location\":" << location_json(value.emitter_active_location) << '}';
    return out.str();
}

std::string g7_trial_json(const X64FlowEdgeG7ScratchTrialDiagnosticReport& value) {
    std::ostringstream out;
    out << '{'
        << "\"gp_temporary_register\":" << static_cast<unsigned int>(value.gp_temporary_register) << ','
        << "\"xmm_temporary_register\":" << static_cast<unsigned int>(value.xmm_temporary_register) << ','
        << "\"g7_schedule_succeeded\":" << (value.g7_schedule_succeeded ? "true" : "false") << ','
        << "\"scheduled_operations\":" << value.scheduled_operations << ','
        << "\"g7_rejection_code\":" << json_string(value.g7_rejection_code) << ','
        << "\"g7_rejection_message\":" << json_string(value.g7_rejection_message) << ','
        << "\"g8_readiness_attempted\":" << (value.g8_readiness_attempted ? "true" : "false") << ','
        << "\"g8_readiness_audit_completed\":" << (value.g8_readiness_audit_completed ? "true" : "false") << ','
        << "\"g8_rejection_code\":" << json_string(value.g8_rejection_code) << ','
        << "\"g8_rejection_message\":" << json_string(value.g8_rejection_message) << ','
        << "\"scratch_liveness_proven\":" << (value.scratch_liveness_proven ? "true" : "false") << ','
        << "\"scratch_liveness_conflicts\":" << value.scratch_liveness_conflicts << '}';
    return out.str();
}

std::string g10_operation_json(const X64FlowEdgeBundleOperandEvidenceReport& value) {
    std::ostringstream out;
    out << '{'
        << "\"flow_operation_id\":" << value.flow_operation_id << ','
        << "\"flow_block_id\":" << value.flow_block_id << ','
        << "\"flow_operation_index\":" << value.flow_operation_index << ','
        << "\"operand_kind\":" << json_string(operand_name(value.operand_kind)) << ','
        << "\"flow_register_class\":" << json_string(register_class_name(value.flow_register_class)) << ','
        << "\"candidate_value_register_class\":" << json_string(register_class_name(value.candidate_value_register_class)) << ','
        << "\"flow_operation_location\":" << location_json(value.flow_operation_location) << ','
        << "\"emitter_operand_location\":" << location_json(value.emitter_operand_location) << ','
        << "\"flow_location_uses_frame\":" << (value.flow_location_uses_frame ? "true" : "false") << ','
        << "\"flow_frame_displacement\":" << value.flow_frame_displacement << ','
        << "\"emitter_location_uses_frame\":" << (value.emitter_location_uses_frame ? "true" : "false") << ','
        << "\"emitter_frame_displacement\":" << value.emitter_frame_displacement << ','
        << "\"location_matches\":" << (value.location_matches ? "true" : "false") << ','
        << "\"mismatch_kind\":" << json_string(mismatch_name(value.mismatch_kind)) << ','
        << "\"source_span_matches_emitter_anchor\":" << (value.source_span_matches_emitter_anchor ? "true" : "false") << '}';
    return out.str();
}

std::string mismatch_counts_json(const NativeX64FlowOperandMismatchCounts& value) {
    std::ostringstream out;
    out << '{'
        << "\"register_class\":" << value.register_class << ','
        << "\"register_index\":" << value.register_index << ','
        << "\"invalid_register\":" << value.invalid_register << ','
        << "\"flow_register_emitter_memory\":" << value.flow_register_emitter_memory << ','
        << "\"flow_memory_emitter_register\":" << value.flow_memory_emitter_register << ','
        << "\"frame_displacement\":" << value.frame_displacement << ','
        << "\"frame_mapping_unavailable\":" << value.frame_mapping_unavailable << ','
        << "\"evidence_disagreement\":" << value.evidence_disagreement << ','
        << "\"unsupported_location\":" << value.unsupported_location << '}';
    return out.str();
}

Totals collect_totals(const NativeProgramBuildResult& build) {
    Totals total;
    for (const auto& module : build.modules) {
        for (const auto& function : module.codegen.stats.native_n113_flow_edge_diagnostic_function_reports) {
            ++total.functions;
            total.g4_live_values_audited += function.g4_live_values_audited;
            total.g4_live_values += function.g4_candidate_edge_live_values;
            total.g6_candidates += function.g6_candidate_values;
            total.g7_attempts += function.g7_attempts;
            total.g7_successes += function.g7_schedule_successes;
            total.g7_rejections += function.g7_schedule_rejections;
            total.g8_attempts += function.g8_readiness_attempts;
            total.g8_audit_completions += function.g8_readiness_audit_completions;
            total.g6_anchor_missing += function.edges_with_g6_branch_anchor_missing;
            total.g6_anchor_inexact += function.edges_with_g6_branch_anchor_inexact;
            for (const auto& edge : function.edges) {
                ++total.edges;
                if (edge.g11_proof_gate_passed) ++total.g11_passed_edges;
                else ++total.g11_blocked_edges;
            }
        }
    }
    return total;
}

std::string build_json(const std::vector<InputModule>& inputs,
                       const NativeProgramBuildResult& build,
                       const Totals& total) {
    std::ostringstream out;
    out << "{\n  \"schema\":\"project-x.n113-g20-edge-diagnostics.v1\",\n"
        << "  \"scope\":\"read-only exact-edge reconciliation of G4/G6/G7/G8/G9/G10/G11 evidence\",\n"
        << "  \"target\":\"AMD64 COFF\",\n"
        << "  \"audit_enabled\":true,\n"
        << "  \"edge_bundle_insertion_enabled\":false,\n"
        << "  \"spill_reload_insertion_enabled\":false,\n"
        << "  \"insertion_ready\":false,\n"
        << "  \"source_files\":[";
    for (std::size_t i = 0U; i < inputs.size(); ++i) {
        if (i != 0U) out << ',';
        out << json_string(inputs[i].path.string());
    }
    out << "],\n  \"summary\":{"
        << "\"modules\":" << build.modules.size() << ','
        << "\"functions\":" << total.functions << ','
        << "\"edges\":" << total.edges << ','
        << "\"g4_live_value_records\":" << total.g4_live_values << ','
        << "\"g4_live_values_audited\":" << total.g4_live_values_audited << ','
        << "\"g6_candidate_values\":" << total.g6_candidates << ','
        << "\"g7_scratch_pair_attempts\":" << total.g7_attempts << ','
        << "\"g7_schedule_successes\":" << total.g7_successes << ','
        << "\"g7_schedule_rejections\":" << total.g7_rejections << ','
        << "\"g8_readiness_attempted_edges\":" << total.g8_attempts << ','
        << "\"g8_readiness_audit_completed_edges\":" << total.g8_audit_completions << ','
        << "\"g6_branch_anchor_missing_edges\":" << total.g6_anchor_missing << ','
        << "\"g6_branch_anchor_inexact_edges\":" << total.g6_anchor_inexact << ','
        << "\"g11_proof_gate_passed_edges\":" << total.g11_passed_edges << ','
        << "\"g11_blocked_edges\":" << total.g11_blocked_edges
        << "},\n  \"modules\":[";

    bool first_module = true;
    for (const auto& module : build.modules) {
        const auto& functions = module.codegen.stats.native_n113_flow_edge_diagnostic_function_reports;
        if (functions.empty()) continue;
        if (!first_module) out << ',';
        first_module = false;
        out << "\n    {\"module_name\":" << json_string(module.module_name) << ",\"functions\":[";
        for (std::size_t f = 0U; f < functions.size(); ++f) {
            if (f != 0U) out << ',';
            const auto& function = functions[f];
            out << "\n      {\"function_index\":" << function.function_index
                << ",\"function_name\":" << json_string(function.function_name)
                << ",\"plan_status\":{\"g4\":" << (function.g4_plan_succeeded ? "true" : "false")
                << ",\"g6\":" << (function.g6_plan_succeeded ? "true" : "false")
                << ",\"g9\":" << (function.g9_plan_succeeded ? "true" : "false")
                << ",\"g10\":" << (function.g10_plan_succeeded ? "true" : "false")
                << ",\"g11\":" << (function.g11_plan_succeeded ? "true" : "false") << '}'
                << ",\"summary\":{\"g4_edges_audited\":" << function.g4_edges_audited
                << ",\"g4_live_values_audited\":" << function.g4_live_values_audited
                << ",\"g4_candidate_edge_live_values\":" << function.g4_candidate_edge_live_values
                << ",\"g6_candidate_edges\":" << function.g6_candidate_edges
                << ",\"g6_candidate_values\":" << function.g6_candidate_values
                << ",\"g7_attempts\":" << function.g7_attempts
                << ",\"g7_successes\":" << function.g7_schedule_successes
                << ",\"g7_rejections\":" << function.g7_schedule_rejections
                << ",\"g8_attempts\":" << function.g8_readiness_attempts
                << ",\"g8_audit_completions\":" << function.g8_readiness_audit_completions
                << ",\"g6_anchor_missing\":" << function.edges_with_g6_branch_anchor_missing
                << ",\"g6_anchor_inexact\":" << function.edges_with_g6_branch_anchor_inexact << "}"
                << ",\"edges\":[";
            for (std::size_t e = 0U; e < function.edges.size(); ++e) {
                if (e != 0U) out << ',';
                const auto& edge = function.edges[e];
                out << "\n        {\"edge_id\":" << edge.edge_id
                    << ",\"predecessor_block_id\":" << edge.predecessor_block_id
                    << ",\"successor_block_id\":" << edge.successor_block_id
                    << ",\"g4_edge_audited\":" << (edge.g4_edge_audited ? "true" : "false")
                    << ",\"g4_live_values\":" << edge.g4_live_values
                    << ",\"g4_values\":[";
                for (std::size_t v = 0U; v < edge.g4_values.size(); ++v) {
                    if (v != 0U) out << ',';
                    out << g4_value_json(edge.g4_values[v]);
                }
                out << "],\"g6_candidate_values\":" << edge.g6_candidate_values
                    << ",\"g6_candidates\":[";
                for (std::size_t c = 0U; c < edge.g6_candidates.size(); ++c) {
                    if (c != 0U) out << ',';
                    out << g6_candidate_json(edge.g6_candidates[c]);
                }
                out << "],\"branch_anchor\":{\"present\":" << (edge.g6_branch_anchor_present ? "true" : "false")
                    << ",\"identity_exact\":" << (edge.g6_branch_anchor_identity_exact ? "true" : "false")
                    << ",\"encoding\":" << static_cast<unsigned int>(edge.branch_anchor_encoding)
                    << ",\"edge_kind\":" << static_cast<unsigned int>(edge.branch_edge_kind)
                    << ",\"conditional_taken\":" << (edge.branch_conditional_taken ? "true" : "false")
                    << ",\"instruction_offset\":" << edge.branch_instruction_offset
                    << ",\"displacement_offset\":" << edge.branch_displacement_offset
                    << ",\"target_offset\":" << edge.branch_target_offset
                    << ",\"function_code_size\":" << edge.branch_function_code_size << "}"
                    << ",\"g7_attempts\":" << edge.g7_attempts
                    << ",\"g7_schedule_successes\":" << edge.g7_schedule_successes
                    << ",\"g7_schedule_rejections\":" << edge.g7_schedule_rejections
                    << ",\"g7_trials\":[";
                for (std::size_t t = 0U; t < edge.g7_trials.size(); ++t) {
                    if (t != 0U) out << ',';
                    out << g7_trial_json(edge.g7_trials[t]);
                }
                out << "],\"g8\":{\"readiness_attempted\":" << (edge.g8_readiness_attempted ? "true" : "false")
                    << ",\"readiness_audit_completed\":" << (edge.g8_readiness_audit_completed ? "true" : "false")
                    << ",\"rejection_code\":" << json_string(edge.g8_rejection_code)
                    << ",\"rejection_message\":" << json_string(edge.g8_rejection_message)
                    << ",\"live_values\":" << edge.g8_live_values
                    << ",\"endpoint_matches\":" << edge.g8_endpoint_matches
                    << ",\"endpoint_mismatches\":" << edge.g8_endpoint_mismatches << "}"
                    << ",\"g9\":{\"schedule_built\":" << (edge.g9_schedule_built ? "true" : "false")
                    << ",\"scratch_selection_proven\":" << (edge.g9_scratch_selection_proven ? "true" : "false")
                    << ",\"selected_gp_temporary\":" << static_cast<unsigned int>(edge.g9_selected_gp_temporary)
                    << ",\"selected_xmm_temporary\":" << static_cast<unsigned int>(edge.g9_selected_xmm_temporary) << "}"
                    << ",\"g10\":{\"values_with_evidence\":" << edge.g10_values_with_evidence
                    << ",\"values_proven\":" << edge.g10_values_proven
                    << ",\"values_mismatched\":" << edge.g10_values_mismatched
                    << ",\"values_missing\":" << edge.g10_values_missing
                    << ",\"mismatch_counts\":" << mismatch_counts_json(edge.g10_mismatch_counts)
                    << ",\"values\":[";
                for (std::size_t v = 0U; v < edge.g10_values.size(); ++v) {
                    if (v != 0U) out << ',';
                    const auto& value = edge.g10_values[v];
                    out << "{\"value_id\":" << value.value_id
                        << ",\"register_class\":" << json_string(register_class_name(value.register_class))
                        << ",\"concrete_evidence_found\":" << (value.concrete_evidence_found ? "true" : "false")
                        << ",\"operand_operations\":" << value.operand_operations
                        << ",\"matching_operations\":" << value.matching_operations
                        << ",\"mismatching_operations\":" << value.mismatching_operations
                        << ",\"mismatch_counts\":" << mismatch_counts_json(value.mismatch_counts)
                        << ",\"operations\":[";
                    for (std::size_t o = 0U; o < value.operations.size(); ++o) {
                        if (o != 0U) out << ',';
                        out << g10_operation_json(value.operations[o]);
                    }
                    out << "]}";
                }
                out << "]}"
                    << ",\"g11\":{\"exact_schedule\":" << (edge.g11_exact_schedule ? "true" : "false")
                    << ",\"endpoint_ownership_proven\":" << (edge.g11_endpoint_ownership_proven ? "true" : "false")
                    << ",\"scratch_liveness_proven\":" << (edge.g11_scratch_liveness_proven ? "true" : "false")
                    << ",\"operation_operand_ownership_proven\":" << (edge.g11_operation_operand_ownership_proven ? "true" : "false")
                    << ",\"proof_gate_passed\":" << (edge.g11_proof_gate_passed ? "true" : "false")
                    << ",\"blockers\":[";
                for (std::size_t b = 0U; b < edge.g11_blockers.size(); ++b) {
                    if (b != 0U) out << ',';
                    out << json_string(edge.g11_blockers[b]);
                }
                out << "]}}";
            }
            out << "]}";
        }
        out << "]}";
    }
    out << "\n  ],\n  \"diagnostics\":[";
    for (std::size_t i = 0U; i < build.diagnostics.size(); ++i) {
        if (i != 0U) out << ',';
        out << "{\"code\":" << json_string(build.diagnostics[i].code)
            << ",\"message\":" << json_string(build.diagnostics[i].message) << '}';
    }
    out << "]\n}\n";
    return out.str();
}

std::string build_text(const std::vector<InputModule>& inputs,
                       const NativeProgramBuildResult& build,
                       const Totals& total) {
    std::ostringstream out;
    out << "Project X N113-G20 exact-edge diagnostic reconciliation\n"
        << "Scope: read-only G4/G6/G7/G8/G9/G10/G11 evidence; no insertion\n"
        << "Source files: " << inputs.size() << "\n"
        << "Modules: " << build.modules.size() << "\n"
        << "Functions: " << total.functions << "\n"
        << "Candidate edges: " << total.edges << "\n"
        << "G4 live-through Value records on exported candidate edges: " << total.g4_live_values << "\n"
        << "G4 full-plan live-through Value count: " << total.g4_live_values_audited << "\n"
        << "G6 location-change candidates: " << total.g6_candidates << "\n"
        << "G7 GP/XMM scratch-pair attempts: " << total.g7_attempts << "\n"
        << "G7 schedule successes: " << total.g7_successes << "\n"
        << "G7 schedule rejections: " << total.g7_rejections << "\n"
        << "G8 readiness attempted edges: " << total.g8_attempts << "\n"
        << "G8 readiness audit-completed edges: " << total.g8_audit_completions << "\n"
        << "G6 branch anchors missing: " << total.g6_anchor_missing << "\n"
        << "G6 branch anchors inexact: " << total.g6_anchor_inexact << "\n"
        << "G11 passed/blocked edges: " << total.g11_passed_edges << '/' << total.g11_blocked_edges << "\n"
        << "INSERTION_READY=FALSE\nMACHINE_CODE_INSERTION=NOT_PERFORMED\n\n";
    for (const auto& module : build.modules) {
        out << "MODULE " << module.module_name << "\n";
        for (const auto& function : module.codegen.stats.native_n113_flow_edge_diagnostic_function_reports) {
            out << " FUNCTION " << function.function_index << ' ' << function.function_name
                << " G4(edges/live)=" << function.g4_edges_audited << '/' << function.g4_live_values_audited
                << " G6(edges/candidates)=" << function.g6_candidate_edges << '/' << function.g6_candidate_values
                << " G7(attempt/success/reject)=" << function.g7_attempts << '/'
                << function.g7_schedule_successes << '/' << function.g7_schedule_rejections
                << " G8(attempt/audit-completed)=" << function.g8_readiness_attempts << '/'
                << function.g8_readiness_audit_completions << "\n";
            for (const auto& edge : function.edges) {
                out << "  EDGE " << edge.edge_id << ' ' << edge.predecessor_block_id << "->"
                    << edge.successor_block_id << " G4_live=" << edge.g4_live_values
                    << " G6_candidates=" << edge.g6_candidate_values
                    << " anchor(present/exact)=" << edge.g6_branch_anchor_present << '/'
                    << edge.g6_branch_anchor_identity_exact
                    << " G7(attempt/success/reject)=" << edge.g7_attempts << '/'
                    << edge.g7_schedule_successes << '/' << edge.g7_schedule_rejections
                    << " G8(attempt/audit-completed/live/match/mismatch)=" << edge.g8_readiness_attempted << '/'
                    << edge.g8_readiness_audit_completed << '/' << edge.g8_live_values << '/'
                    << edge.g8_endpoint_matches << '/' << edge.g8_endpoint_mismatches
                    << " G9(schedule/scratch)=" << edge.g9_schedule_built << '/'
                    << edge.g9_scratch_selection_proven
                    << " G10(values proven/evidence/mismatch/missing)=" << edge.g10_values_proven << '/'
                    << edge.g10_values_with_evidence << '/' << edge.g10_values_mismatched << '/'
                    << edge.g10_values_missing
                    << " G11(gate)=" << edge.g11_proof_gate_passed << "\n";
                for (const auto& value : edge.g4_values) {
                    out << "   G4_VALUE " << value.value_id << " class=" << register_class_name(value.register_class)
                        << " pred=" << location_kind_name(value.predecessor_flow_location.kind) << ':'
                        << static_cast<unsigned int>(value.predecessor_flow_location.register_index) << ':'
                        << value.predecessor_flow_location.spill_slot
                        << " succ=" << location_kind_name(value.successor_flow_location.kind) << ':'
                        << static_cast<unsigned int>(value.successor_flow_location.register_index) << ':'
                        << value.successor_flow_location.spill_slot
                        << " emitter=" << location_kind_name(value.emitter_active_location.kind) << ':'
                        << static_cast<unsigned int>(value.emitter_active_location.register_index) << ':'
                        << value.emitter_active_location.spill_slot
                        << " changes=" << value.flow_location_changes
                        << " g6_candidate=" << value.is_g6_candidate << "\n";
                }
                for (const auto& candidate : edge.g6_candidates) {
                    out << "   G6_CANDIDATE " << candidate.value_id << " class="
                        << register_class_name(candidate.register_class)
                        << " source=" << location_kind_name(candidate.source_location.kind) << ':'
                        << static_cast<unsigned int>(candidate.source_location.register_index) << ':'
                        << candidate.source_location.spill_slot
                        << " destination=" << location_kind_name(candidate.destination_location.kind) << ':'
                        << static_cast<unsigned int>(candidate.destination_location.register_index) << ':'
                        << candidate.destination_location.spill_slot << "\n";
                }
                for (const auto& trial : edge.g7_trials) {
                    out << "   G7_TRIAL GP=R" << static_cast<unsigned int>(trial.gp_temporary_register)
                        << " XMM=" << static_cast<unsigned int>(trial.xmm_temporary_register)
                        << " schedule=" << trial.g7_schedule_succeeded
                        << " operations=" << trial.scheduled_operations
                        << " reject=" << trial.g7_rejection_code << " " << trial.g7_rejection_message
                        << " G8_attempted=" << trial.g8_readiness_attempted
                        << " G8_audit_completed=" << trial.g8_readiness_audit_completed
                        << " G8_reject=" << trial.g8_rejection_code << " " << trial.g8_rejection_message
                        << " scratch_dead=" << trial.scratch_liveness_proven
                        << " conflicts=" << trial.scratch_liveness_conflicts << "\n";
                }
                for (const auto& value : edge.g10_values) {
                    out << "   G10_VALUE " << value.value_id << " evidence=" << value.concrete_evidence_found
                        << " operations=" << value.operand_operations << " matches=" << value.matching_operations
                        << " mismatches=" << value.mismatching_operations << "\n";
                    for (const auto& operation : value.operations) {
                        if (operation.mismatch_kind == NativeX64FlowOperandMismatchKind::None) continue;
                        out << "    G10_OPERATION " << operation.flow_operation_id << " block="
                            << operation.flow_block_id << " index=" << operation.flow_operation_index
                            << " kind=" << operand_name(operation.operand_kind)
                            << " flow=" << location_kind_name(operation.flow_operation_location.kind) << ':'
                            << static_cast<unsigned int>(operation.flow_operation_location.register_index) << ':'
                            << operation.flow_operation_location.spill_slot
                            << " emitter=" << location_kind_name(operation.emitter_operand_location.kind) << ':'
                            << static_cast<unsigned int>(operation.emitter_operand_location.register_index) << ':'
                            << operation.emitter_operand_location.spill_slot
                            << " mismatch=" << mismatch_name(operation.mismatch_kind) << "\n";
                    }
                }
                for (const auto& blocker : edge.g11_blockers) out << "   G11_BLOCKER " << blocker << "\n";
            }
        }
    }
    return out.str();
}

bool write_text_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(output);
}

void print_diagnostics(const std::string& label, const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << label << ' ' << diagnostic.code << " | " << diagnostic.message
                  << " | line=" << diagnostic.span.start.line
                  << " | column=" << diagnostic.span.start.column << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4 || std::string(argv[1]) != "--out") {
        std::cerr << "usage: xfrontend_n113_g20_edge_diagnostics --out <output-directory> <source-file> [source-file ...]\n";
        return 2;
    }

    const std::filesystem::path output_directory(argv[2]);
    std::error_code filesystem_error;
    std::filesystem::create_directories(output_directory, filesystem_error);
    if (filesystem_error) {
        std::cerr << "N113_G20_OUTPUT | FAILED | " << filesystem_error.message() << '\n';
        return 3;
    }

    std::vector<InputModule> inputs;
    std::vector<NativeProgramModuleInput> modules;
    inputs.reserve(static_cast<std::size_t>(argc - 3));
    modules.reserve(static_cast<std::size_t>(argc - 3));
    for (int index = 3; index < argc; ++index) {
        const std::filesystem::path path(argv[index]);
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            std::cerr << "N113_G20_INPUT | FAILED | Cannot open source file: " << path.string() << '\n';
            return 4;
        }
        std::ostringstream source_stream;
        source_stream << file.rdbuf();
        if (!file.good() && !file.eof()) {
            std::cerr << "N113_G20_INPUT | FAILED | Cannot read source file: " << path.string() << '\n';
            return 5;
        }
        std::string source_text = source_stream.str();
        if (source_text.size() >= 3U &&
            static_cast<unsigned char>(source_text[0]) == 0xEFU &&
            static_cast<unsigned char>(source_text[1]) == 0xBBU &&
            static_cast<unsigned char>(source_text[2]) == 0xBFU) source_text.erase(0U, 3U);
        auto parsed = parse_source(source_text);
        if (!parsed.succeeded()) {
            std::cerr << "N113_G20_PARSE | FAILED | source=" << path.string() << '\n';
            print_diagnostics("PARSE", parsed.diagnostics);
            return 6;
        }
        auto unit = std::make_shared<CompilationUnit>(std::move(parsed.unit));
        if (unit->module_name.empty()) {
            std::cerr << "N113_G20_PARSE | FAILED | source has no module declaration: " << path.string() << '\n';
            return 7;
        }
        inputs.push_back(InputModule{path, unit});
        modules.push_back(NativeProgramModuleInput{path.string(), std::move(unit)});
    }

    X64CodegenOptions options{"n113-g20-exact-edge-diagnostics", make_coff_x86_64_object_target()};
    options.audit_n113_flow_edge_bundle_eligibility = true;
    options.insert_n113_emitter_spill_reload_actions = false;
    const auto compiled = compile_x64_coff_program(modules, std::move(options));
    if (!compiled.succeeded()) {
        std::cerr << "N113_G20_CODEGEN | FAILED | whole-program compile pipeline did not complete\n";
        print_diagnostics("CODEGEN", compiled.diagnostics);
        return 8;
    }

    for (const auto& module : compiled.modules) {
        if (module.codegen.stats.native_n113_emitter_actions_inserted != 0U ||
            module.codegen.stats.native_n113_emitter_machine_moves_inserted != 0U ||
            module.codegen.stats.native_n113_emitter_transfer_bytes_inserted != 0U) {
            std::cerr << "N113_G20_GUARD | FAILED | unexpected spill/reload insertion counter\n";
            return 9;
        }
    }

    const Totals totals = collect_totals(compiled);
    if (totals.functions == 0U || totals.edges == 0U || totals.g4_live_values < totals.g6_candidates ||
        totals.g7_attempts != totals.g7_successes + totals.g7_rejections) {
        std::cerr << "N113_G20_RECONCILIATION | FAILED | diagnostic aggregate invariants did not reconcile\n";
        return 10;
    }

    const std::string json = build_json(inputs, compiled, totals);
    const std::string text_report = build_text(inputs, compiled, totals);
    const auto json_path = output_directory / "N113_G20_Edge_Diagnostics.json";
    const auto text_path = output_directory / "N113_G20_Edge_Diagnostics.txt";
    if (!write_text_file(json_path, json) || !write_text_file(text_path, text_report)) {
        std::cerr << "N113_G20_OUTPUT | FAILED | could not write TXT/JSON diagnostics\n";
        return 11;
    }

    std::cout << "N113_G20_DIAGNOSTICS | RESULT=PASSED"
              << " | MODULES=" << compiled.modules.size()
              << " | FUNCTIONS=" << totals.functions
              << " | EDGES=" << totals.edges
              << " | G4_LIVE_VALUES=" << totals.g4_live_values
              << " | G6_CANDIDATES=" << totals.g6_candidates
              << " | G7_ATTEMPTS=" << totals.g7_attempts
              << " | G7_SUCCESSES=" << totals.g7_successes
              << " | G7_REJECTIONS=" << totals.g7_rejections
              << " | G8_ATTEMPTS=" << totals.g8_attempts
              << " | G8_AUDIT_COMPLETIONS=" << totals.g8_audit_completions
              << " | G6_ANCHOR_MISSING=" << totals.g6_anchor_missing
              << " | G6_ANCHOR_INEXACT=" << totals.g6_anchor_inexact
              << " | G11_PASSED=" << totals.g11_passed_edges
              << " | G11_BLOCKED=" << totals.g11_blocked_edges
              << " | INSERTION_READY=FALSE"
              << " | INSERTION_PERFORMED=FALSE\n"
              << "JSON=" << json_path.string() << '\n'
              << "TXT=" << text_path.string() << '\n';
    return 0;
}
