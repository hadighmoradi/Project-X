// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

﻿#include "xfrontend/native_program_pipeline.hpp"
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
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    out << '"';
    return out.str();
}

const char* register_class_name(NativeRegisterClass value) {
    return value == NativeRegisterClass::GeneralPurpose ? "GeneralPurpose" : "FloatingPoint";
}

const char* origin_name(NativeRegisterInstructionOriginKind value) {
    switch (value) {
    case NativeRegisterInstructionOriginKind::Synthetic: return "Synthetic";
    case NativeRegisterInstructionOriginKind::FlowRead: return "FlowRead";
    case NativeRegisterInstructionOriginKind::FlowDeclare: return "FlowDeclare";
    case NativeRegisterInstructionOriginKind::FlowWrite: return "FlowWrite";
    case NativeRegisterInstructionOriginKind::FlowCall: return "FlowCall";
    case NativeRegisterInstructionOriginKind::FlowReturn: return "FlowReturn";
    }
    return "Unknown";
}

const char* position_name(NativeX64FlowOperandPositionKind value) {
    return value == NativeX64FlowOperandPositionKind::BeforeOperation
        ? "BeforeOperation" : "AfterOperation";
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

std::string location_json(const NativeRegisterLocation& value) {
    std::ostringstream out;
    out << "{\"kind\":" << json_string(location_kind_name(value.kind))
        << ",\"register_index\":" << static_cast<unsigned int>(value.register_index)
        << ",\"slot_index\":" << value.spill_slot << '}';
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
        << "\"unsupported_location\":" << value.unsupported_location
        << '}';
    return out.str();
}

std::string endpoint_value_json(const X64FlowEdgeEndpointValueEvidenceReport& value) {
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
        << "\"emitter_location_is_home_slot\":" << (value.emitter_location_is_home_slot ? "true" : "false") << ','
        << "\"emitter_frame_mapping_available\":" << (value.emitter_frame_mapping_available ? "true" : "false") << ','
        << "\"emitter_frame_displacement\":" << value.emitter_frame_displacement << ','
        << "\"predecessor_matches_emitter\":" << (value.predecessor_matches_emitter ? "true" : "false") << ','
        << "\"successor_matches_emitter\":" << (value.successor_matches_emitter ? "true" : "false") << ','
        << "\"function_operand_anchor_count\":" << value.function_operand_anchor_count
        << '}';
    return out.str();
}

std::string operation_json(const X64FlowEdgeBundleOperandEvidenceReport& value) {
    std::ostringstream out;
    out << '{'
        << "\"flow_operation_id\":" << value.flow_operation_id << ','
        << "\"flow_block_id\":" << value.flow_block_id << ','
        << "\"flow_operation_index\":" << value.flow_operation_index << ','
        << "\"operand_kind\":" << json_string(operand_name(value.operand_kind)) << ','
        << "\"flow_register_class\":" << json_string(register_class_name(value.flow_register_class)) << ','
        << "\"candidate_value_register_class\":" << json_string(register_class_name(value.candidate_value_register_class)) << ','
        << "\"flow_origin_kind\":" << json_string(origin_name(value.flow_origin_kind)) << ','
        << "\"flow_position_kind\":" << json_string(position_name(value.flow_position_kind)) << ','
        << "\"source_span\":{\"source_id\":" << value.source_span.source_id
        << ",\"start_byte_offset\":" << value.source_span.start.byte_offset
        << ",\"start_line\":" << value.source_span.start.line
        << ",\"start_column\":" << value.source_span.start.column
        << ",\"end_byte_offset\":" << value.source_span.end.byte_offset
        << ",\"end_line\":" << value.source_span.end.line
        << ",\"end_column\":" << value.source_span.end.column << "},"
        << "\"source_span_matches_emitter_anchor\":"
        << (value.source_span_matches_emitter_anchor ? "true" : "false") << ','
        << "\"flow_operation_location\":" << location_json(value.flow_operation_location) << ','
        << "\"emitter_operand_location\":" << location_json(value.emitter_operand_location) << ','
        << "\"flow_location_uses_frame\":" << (value.flow_location_uses_frame ? "true" : "false") << ','
        << "\"flow_frame_displacement\":" << value.flow_frame_displacement << ','
        << "\"emitter_location_uses_frame\":" << (value.emitter_location_uses_frame ? "true" : "false") << ','
        << "\"emitter_frame_displacement\":" << value.emitter_frame_displacement << ','
        << "\"location_matches\":" << (value.location_matches ? "true" : "false") << ','
        << "\"mismatch_kind\":" << json_string(mismatch_name(value.mismatch_kind))
        << '}';
    return out.str();
}

struct Totals {
    std::uint64_t functions = 0U;
    std::uint64_t candidate_edges = 0U;
    std::uint64_t passed_edges = 0U;
    std::uint64_t blocked_edges = 0U;
    std::uint64_t schedule_blocked_edges = 0U;
    std::uint64_t endpoint_blocked_edges = 0U;
    std::uint64_t scratch_blocked_edges = 0U;
    std::uint64_t operand_blocked_edges = 0U;
    std::uint64_t values = 0U;
    std::uint64_t operations = 0U;
    std::uint64_t endpoint_value_records = 0U;
    std::uint64_t endpoint_ownership_matches = 0U;
    std::uint64_t endpoint_ownership_mismatches = 0U;
    NativeX64FlowOperandMismatchCounts mismatches;
};

void add_counts(NativeX64FlowOperandMismatchCounts& total,
                const NativeX64FlowOperandMismatchCounts& value) {
    total.register_class += value.register_class;
    total.register_index += value.register_index;
    total.invalid_register += value.invalid_register;
    total.flow_register_emitter_memory += value.flow_register_emitter_memory;
    total.flow_memory_emitter_register += value.flow_memory_emitter_register;
    total.frame_displacement += value.frame_displacement;
    total.frame_mapping_unavailable += value.frame_mapping_unavailable;
    total.evidence_disagreement += value.evidence_disagreement;
    total.unsupported_location += value.unsupported_location;
}

Totals collect_totals(const NativeProgramBuildResult& build) {
    Totals totals;
    for (const auto& module : build.modules) {
        for (const auto& function : module.codegen.stats.native_n113_flow_edge_bundle_function_reports) {
            ++totals.functions;
            totals.candidate_edges += function.candidate_edges;
            totals.passed_edges += function.proof_gate_passed_edges;
            totals.blocked_edges += function.blocked_edges;
            totals.schedule_blocked_edges += function.schedule_blocked_edges;
            totals.endpoint_blocked_edges += function.endpoint_ownership_blocked_edges;
            totals.scratch_blocked_edges += function.scratch_blocked_edges;
            totals.operand_blocked_edges += function.operation_operand_blocked_edges;
            add_counts(totals.mismatches, function.mismatch_counts);
            for (const auto& edge : function.edges) {
                totals.endpoint_value_records += edge.endpoint_value_evidence.size();
                totals.endpoint_ownership_matches += edge.endpoint_ownership_matches;
                totals.endpoint_ownership_mismatches += edge.endpoint_ownership_mismatches;
                for (const auto& value : edge.value_operand_evidence) {
                    ++totals.values;
                    totals.operations += value.operations.size();
                }
            }
        }
    }
    return totals;
}

std::string build_json(const std::vector<InputModule>& inputs,
                       const NativeProgramBuildResult& build,
                       const Totals& totals) {
    std::ostringstream out;
    out << "{\n  \"schema\":\"project-x.n113-g19-endpoint-census.v1\",\n"
        << "  \"scope\":\"read-only FunctionEmitter Flow-edge endpoint evidence census\",\n"
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
        << "\"functions\":" << totals.functions << ','
        << "\"candidate_edges\":" << totals.candidate_edges << ','
        << "\"passed_edges\":" << totals.passed_edges << ','
        << "\"blocked_edges\":" << totals.blocked_edges << ','
        << "\"schedule_blocked_edges\":" << totals.schedule_blocked_edges << ','
        << "\"endpoint_blocked_edges\":" << totals.endpoint_blocked_edges << ','
        << "\"scratch_blocked_edges\":" << totals.scratch_blocked_edges << ','
        << "\"operand_blocked_edges\":" << totals.operand_blocked_edges << ','
        << "\"value_evidence_records\":" << totals.values << ','
        << "\"operation_evidence_records\":" << totals.operations << ','
        << "\"endpoint_value_evidence_records\":" << totals.endpoint_value_records << ','
        << "\"endpoint_ownership_matches\":" << totals.endpoint_ownership_matches << ','
        << "\"endpoint_ownership_mismatches\":" << totals.endpoint_ownership_mismatches << ','
        << "\"endpoint_comparisons\":" << (totals.endpoint_ownership_matches + totals.endpoint_ownership_mismatches) << ','
        << "\"mismatch_counts\":" << mismatch_counts_json(totals.mismatches)
        << "},\n  \"modules\":[";

    for (std::size_t m = 0U; m < build.modules.size(); ++m) {
        if (m != 0U) out << ',';
        const auto& module = build.modules[m];
        out << "\n    {\"module_name\":" << json_string(module.module_name)
            << ",\"functions\":[";
        const auto& functions = module.codegen.stats.native_n113_flow_edge_bundle_function_reports;
        for (std::size_t f = 0U; f < functions.size(); ++f) {
            if (f != 0U) out << ',';
            const auto& function = functions[f];
            out << "\n      {\"function_index\":" << function.function_index
                << ",\"function_name\":" << json_string(function.function_name)
                << ",\"plan_succeeded\":" << (function.plan_succeeded ? "true" : "false")
                << ",\"candidate_edges\":" << function.candidate_edges
                << ",\"proof_gate_passed_edges\":" << function.proof_gate_passed_edges
                << ",\"blocked_edges\":" << function.blocked_edges
                << ",\"schedule_blocked_edges\":" << function.schedule_blocked_edges
                << ",\"endpoint_ownership_blocked_edges\":" << function.endpoint_ownership_blocked_edges
                << ",\"scratch_blocked_edges\":" << function.scratch_blocked_edges
                << ",\"operation_operand_blocked_edges\":" << function.operation_operand_blocked_edges
                << ",\"mismatch_counts\":" << mismatch_counts_json(function.mismatch_counts)
                << ",\"edges\":[";
            for (std::size_t e = 0U; e < function.edges.size(); ++e) {
                if (e != 0U) out << ',';
                const auto& edge = function.edges[e];
                out << "\n        {\"edge_id\":" << edge.edge_id
                    << ",\"predecessor_block_id\":" << edge.predecessor_block_id
                    << ",\"successor_block_id\":" << edge.successor_block_id
                    << ",\"candidate_values\":" << edge.candidate_values
                    << ",\"operation_values_proven\":" << edge.operation_values_proven
                    << ",\"operation_values_with_evidence\":" << edge.operation_values_with_evidence
                    << ",\"operation_values_mismatched\":" << edge.operation_values_mismatched
                    << ",\"operation_values_missing\":" << edge.operation_values_missing
                    << ",\"live_values_audited\":" << edge.live_values_audited
                    << ",\"operand_anchors_validated\":" << edge.operand_anchors_validated
                    << ",\"endpoint_ownership_matches\":" << edge.endpoint_ownership_matches
                    << ",\"endpoint_ownership_mismatches\":" << edge.endpoint_ownership_mismatches
                    << ",\"exact_g9_schedule\":" << (edge.exact_g9_schedule ? "true" : "false")
                    << ",\"endpoint_ownership_proven\":" << (edge.endpoint_ownership_proven ? "true" : "false")
                    << ",\"scratch_liveness_proven\":" << (edge.scratch_liveness_proven ? "true" : "false")
                    << ",\"operation_operand_ownership_proven\":" << (edge.operation_operand_ownership_proven ? "true" : "false")
                    << ",\"proof_gate_passed\":" << (edge.proof_gate_passed ? "true" : "false")
                    << ",\"blockers\":[";
                for (std::size_t b = 0U; b < edge.blockers.size(); ++b) {
                    if (b != 0U) out << ',';
                    out << json_string(edge.blockers[b]);
                }
                out << "],\"mismatch_counts\":" << mismatch_counts_json(edge.mismatch_counts)
                    << ",\"endpoint_values\":[";
                for (std::size_t v = 0U; v < edge.endpoint_value_evidence.size(); ++v) {
                    if (v != 0U) out << ',';
                    out << endpoint_value_json(edge.endpoint_value_evidence[v]);
                }
                out << "],\"values\":[";
                for (std::size_t v = 0U; v < edge.value_operand_evidence.size(); ++v) {
                    if (v != 0U) out << ',';
                    const auto& value = edge.value_operand_evidence[v];
                    out << "{\"value_id\":" << value.value_id
                        << ",\"register_class\":" << json_string(register_class_name(value.register_class))
                        << ",\"concrete_evidence_found\":" << (value.concrete_evidence_found ? "true" : "false")
                        << ",\"operand_operations\":" << value.operand_operations
                        << ",\"matching_operations\":" << value.matching_operations
                        << ",\"mismatching_operations\":" << value.mismatching_operations
                        << ",\"all_observed_operands_match\":" << (value.all_observed_operands_match ? "true" : "false")
                        << ",\"mismatch_counts\":" << mismatch_counts_json(value.mismatch_counts)
                        << ",\"operations\":[";
                    for (std::size_t o = 0U; o < value.operations.size(); ++o) {
                        if (o != 0U) out << ',';
                        out << operation_json(value.operations[o]);
                    }
                    out << "]}";
                }
                out << "]}";
            }
            out << "]} ";
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
                       const Totals& totals) {
    std::ostringstream out;
    out << "Project X N113-G19 FunctionEmitter Flow-Edge Census\n"
        << "Scope: opt-in, read-only AMD64 COFF eligibility audit\n"
        << "Source files: " << inputs.size() << "\n"
        << "Modules: " << build.modules.size() << "\n"
        << "Functions: " << totals.functions << "\n"
        << "Candidate edges: " << totals.candidate_edges << "\n"
        << "Proof-gate passed: " << totals.passed_edges << "\n"
        << "Blocked: " << totals.blocked_edges << "\n"
        << "Blocked by schedule: " << totals.schedule_blocked_edges << "\n"
        << "Blocked by endpoint ownership: " << totals.endpoint_blocked_edges << "\n"
        << "Blocked by scratch liveness: " << totals.scratch_blocked_edges << "\n"
        << "Blocked by operation operands: " << totals.operand_blocked_edges << "\n"
        << "Value evidence records: " << totals.values << "\n"
        << "Operation evidence records (edge-occurrence based): " << totals.operations << "\n"
        << "Endpoint Value evidence records (edge/value based): " << totals.endpoint_value_records << "\n"
        << "Endpoint comparisons: " << (totals.endpoint_ownership_matches + totals.endpoint_ownership_mismatches) << "\n"
        << "Endpoint ownership matches: " << totals.endpoint_ownership_matches << "\n"
        << "Endpoint ownership mismatches: " << totals.endpoint_ownership_mismatches << "\n"
        << "Mismatch occurrences (not unique OperationIds):\n"
        << "  RegisterClassMismatch: " << totals.mismatches.register_class << "\n"
        << "  RegisterIndexMismatch: " << totals.mismatches.register_index << "\n"
        << "  InvalidRegister: " << totals.mismatches.invalid_register << "\n"
        << "  FlowRegisterEmitterMemory: " << totals.mismatches.flow_register_emitter_memory << "\n"
        << "  FlowMemoryEmitterRegister: " << totals.mismatches.flow_memory_emitter_register << "\n"
        << "  FrameDisplacementMismatch: " << totals.mismatches.frame_displacement << "\n"
        << "  FrameMappingUnavailable: " << totals.mismatches.frame_mapping_unavailable << "\n"
        << "  EvidenceDisagreement: " << totals.mismatches.evidence_disagreement << "\n"
        << "  UnsupportedLocationKind: " << totals.mismatches.unsupported_location << "\n"
        << "Insertion enabled: FALSE\n"
        << "INSERTION_READY=FALSE\n\n";
    for (const auto& module : build.modules) {
        out << "MODULE " << module.module_name << "\n";
        for (const auto& function : module.codegen.stats.native_n113_flow_edge_bundle_function_reports) {
            out << " FUNCTION " << function.function_index << ' ' << function.function_name
                << " candidates=" << function.candidate_edges
                << " pass=" << function.proof_gate_passed_edges
                << " blocked=" << function.blocked_edges << "\n";
            for (const auto& edge : function.edges) {
                out << "  EDGE " << edge.edge_id << ' ' << edge.predecessor_block_id << "->"
                    << edge.successor_block_id << " pass=" << (edge.proof_gate_passed ? "true" : "false")
                    << " gates(schedule/endpoint/scratch/operand)=" << edge.exact_g9_schedule << '/'
                    << edge.endpoint_ownership_proven << '/' << edge.scratch_liveness_proven << '/'
                    << edge.operation_operand_ownership_proven
                    << " live_values=" << edge.live_values_audited
                    << " endpoint_matches=" << edge.endpoint_ownership_matches
                    << " endpoint_mismatches=" << edge.endpoint_ownership_mismatches << "\n";
                for (const auto& blocker : edge.blockers) out << "   BLOCKER " << blocker << "\n";
                for (const auto& endpoint : edge.endpoint_value_evidence) {
                    out << "   ENDPOINT_VALUE " << endpoint.value_id
                        << " class=" << register_class_name(endpoint.register_class)
                        << " predecessor=" << location_kind_name(endpoint.predecessor_flow_location.kind)
                        << ':' << static_cast<unsigned int>(endpoint.predecessor_flow_location.register_index)
                        << ':' << endpoint.predecessor_flow_location.spill_slot
                        << " successor=" << location_kind_name(endpoint.successor_flow_location.kind)
                        << ':' << static_cast<unsigned int>(endpoint.successor_flow_location.register_index)
                        << ':' << endpoint.successor_flow_location.spill_slot
                        << " emitter=" << location_kind_name(endpoint.emitter_active_location.kind)
                        << ':' << static_cast<unsigned int>(endpoint.emitter_active_location.register_index)
                        << ':' << endpoint.emitter_active_location.spill_slot
                        << " frame(pred/succ/emitter)=" << endpoint.predecessor_uses_frame << ':'
                        << endpoint.predecessor_frame_displacement << '/' << endpoint.successor_uses_frame << ':'
                        << endpoint.successor_frame_displacement << '/' << endpoint.emitter_location_is_home_slot << ':'
                        << endpoint.emitter_frame_mapping_available << ':' << endpoint.emitter_frame_displacement
                        << " match(pred/succ)=" << endpoint.predecessor_matches_emitter << '/'
                        << endpoint.successor_matches_emitter
                        << " anchors=" << endpoint.function_operand_anchor_count << "\n";
                }
                for (const auto& value : edge.value_operand_evidence) {
                    out << "   VALUE " << value.value_id << " class=" << register_class_name(value.register_class)
                        << " operations=" << value.operand_operations << " matching=" << value.matching_operations
                        << " mismatching=" << value.mismatching_operations << "\n";
                    for (const auto& operation : value.operations) {
                        if (operation.mismatch_kind == NativeX64FlowOperandMismatchKind::None) continue;
                        out << "    OP " << operation.flow_operation_id << " block=" << operation.flow_block_id
                            << " index=" << operation.flow_operation_index
                            << " origin=" << origin_name(operation.flow_origin_kind)
                            << " position=" << position_name(operation.flow_position_kind)
                            << " class(flow/candidate)=" << register_class_name(operation.flow_register_class)
                            << '/' << register_class_name(operation.candidate_value_register_class)
                            << " flow_location=" << location_kind_name(operation.flow_operation_location.kind)
                            << ':' << static_cast<unsigned int>(operation.flow_operation_location.register_index)
                            << ':' << operation.flow_operation_location.spill_slot
                            << " emitter_location=" << location_kind_name(operation.emitter_operand_location.kind)
                            << ':' << static_cast<unsigned int>(operation.emitter_operand_location.register_index)
                            << ':' << operation.emitter_operand_location.spill_slot
                            << " frame(flow/emitter)=" << operation.flow_location_uses_frame << ':'
                            << operation.flow_frame_displacement << '/' << operation.emitter_location_uses_frame << ':'
                            << operation.emitter_frame_displacement
                            << " mismatch=" << mismatch_name(operation.mismatch_kind)
                            << " span_anchor=" << operation.source_span_matches_emitter_anchor << "\n";
                    }
                }
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
                  << " | source_id=" << diagnostic.span.source_id
                  << " | line=" << diagnostic.span.start.line
                  << " | column=" << diagnostic.span.start.column << '\n';
    }
}

void write_failure_report(const std::filesystem::path& output_directory,
                          const std::string& stage,
                          const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream out;
    out << "Project X N113-G19 Census FAILED\nStage: " << stage
        << "\nInsertion performed: FALSE\nINSERTION_READY=FALSE\n";
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.code << " | " << diagnostic.message
            << " | source_id=" << diagnostic.span.source_id
            << " | line=" << diagnostic.span.start.line
            << " | column=" << diagnostic.span.start.column << "\n";
    }
    (void)write_text_file(output_directory / "N113_G19_Endpoint_Census_Failure.txt", out.str());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4 || std::string(argv[1]) != "--out") {
        std::cerr << "usage: xfrontend_n113_g19_endpoint_census --out <output-directory> <source-file> [source-file ...]\n";
        return 2;
    }

    const std::filesystem::path output_directory(argv[2]);
    std::error_code filesystem_error;
    std::filesystem::create_directories(output_directory, filesystem_error);
    if (filesystem_error) {
        std::cerr << "N113_G19_OUTPUT | FAILED | " << filesystem_error.message() << '\n';
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
            std::cerr << "N113_G19_INPUT | FAILED | Cannot open source file: " << path.string() << '\n';
            return 4;
        }
        std::ostringstream source_stream;
        source_stream << file.rdbuf();
        if (!file.good() && !file.eof()) {
            std::cerr << "N113_G19_INPUT | FAILED | Cannot read source file: " << path.string() << '\n';
            return 5;
        }
        std::string source_text = source_stream.str();
        if (source_text.size() >= 3U &&
            static_cast<unsigned char>(source_text[0]) == 0xEFU &&
            static_cast<unsigned char>(source_text[1]) == 0xBBU &&
            static_cast<unsigned char>(source_text[2]) == 0xBFU) {
            source_text.erase(0U, 3U);
        }
        auto parsed = parse_source(source_text);
        if (!parsed.succeeded()) {
            std::cerr << "N113_G19_PARSE | FAILED | source=" << path.string() << '\n';
            print_diagnostics("PARSE", parsed.diagnostics);
            write_failure_report(output_directory, "Parse", parsed.diagnostics);
            return 6;
        }
        auto unit = std::make_shared<CompilationUnit>(std::move(parsed.unit));
        if (unit->module_name.empty()) {
            std::cerr << "N113_G19_PARSE | FAILED | source has no module declaration: " << path.string() << '\n';
            return 7;
        }
        inputs.push_back(InputModule{path, unit});
        modules.push_back(NativeProgramModuleInput{path.string(), std::move(unit)});
    }

    X64CodegenOptions options{"n113-g19-endpoint-evidence-census", make_coff_x86_64_object_target()};
    options.audit_n113_flow_edge_bundle_eligibility = true;
    options.insert_n113_emitter_spill_reload_actions = false;
    const auto compiled = compile_x64_coff_program(modules, std::move(options));
    if (!compiled.succeeded()) {
        std::cerr << "N113_G19_CODEGEN | FAILED | whole-program parse/semantic/codegen/link-plan pipeline did not complete\n";
        print_diagnostics("CODEGEN", compiled.diagnostics);
        write_failure_report(output_directory, "WholeProgramSemanticCodegen", compiled.diagnostics);
        return 8;
    }

    for (const auto& module : compiled.modules) {
        if (module.codegen.stats.native_n113_emitter_actions_inserted != 0U ||
            module.codegen.stats.native_n113_emitter_machine_moves_inserted != 0U ||
            module.codegen.stats.native_n113_emitter_transfer_bytes_inserted != 0U) {
            std::cerr << "N113_G19_GUARD | FAILED | an unexpected N113 spill/reload insertion counter is nonzero\n";
            return 9;
        }
    }

    const Totals totals = collect_totals(compiled);
    const std::string json = build_json(inputs, compiled, totals);
    const std::string text_report = build_text(inputs, compiled, totals);
    const auto json_path = output_directory / "N113_G19_Endpoint_Census.json";
    const auto text_path = output_directory / "N113_G19_Endpoint_Census.txt";
    if (!write_text_file(json_path, json) || !write_text_file(text_path, text_report)) {
        std::cerr << "N113_G19_OUTPUT | FAILED | could not write TXT/JSON report files\n";
        return 10;
    }

    std::cout << "N113_G19_CENSUS | RESULT=PASSED"
              << " | MODULES=" << compiled.modules.size()
              << " | FUNCTIONS=" << totals.functions
              << " | CANDIDATE_EDGES=" << totals.candidate_edges
              << " | PASSED_EDGES=" << totals.passed_edges
              << " | BLOCKED_EDGES=" << totals.blocked_edges
              << " | VALUE_EVIDENCE=" << totals.values
              << " | OPERATION_EVIDENCE=" << totals.operations
              << " | ENDPOINT_VALUE_RECORDS=" << totals.endpoint_value_records
              << " | ENDPOINT_MATCHES=" << totals.endpoint_ownership_matches
              << " | ENDPOINT_MISMATCHES=" << totals.endpoint_ownership_mismatches
              << " | INSERTION_READY=FALSE"
              << " | INSERTION_PERFORMED=FALSE\n"
              << "JSON=" << json_path.string() << '\n'
              << "TXT=" << text_path.string() << '\n';
    return 0;
}
